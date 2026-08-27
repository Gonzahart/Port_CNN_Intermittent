// infer.c -- drives the engine, plus the checkpoint glue.
//
// Runs on nn_engine.c, not TFLM. TFLM's Invoke() was atomic, which is why the
// network used to be two sub-models: splitting it was the only way to get a
// point where inference could stop. Now it stops every INFER_TILE units and
// the network is one model again.
#include "infer.h"
#include "energy_source.h"
#include "power_policy.h"
#include "workload.h"
#include "preprocess.h"
#include "power.h"
#include "nn_engine.h"
#include "ckpt.h"
#include "am_mcu_apollo.h"
#include "am_util.h"
#include <math.h>

#define INFER_TILE 8

// timing (Ambiq STIMER @ 6 MHz -- not gated by the debug power domain)
#define STIMER_HZ 6000000u
#define TICK_TO_US(t) ((uint32_t)((uint64_t)(t) * 1000000u / STIMER_HZ))

static uint32_t g_infer_tk  = 0;   // last inference, STIMER ticks
static uint64_t g_infer_sum = 0;
static uint32_t g_infer_n   = 0;
static uint32_t g_infer_min = 0xFFFFFFFFu;
static uint32_t g_infer_max = 0;

// Ordinary RAM. ckpt_save() copies the live part of it into the slots.
static nn_ctx_t g_net;
static int8_t   g_scores[10];

static int      g_boot_layer = -1;
static int      g_boot_unit  = -1;
static uint32_t g_boot_seq   = 0;
static uint32_t g_boot_crc   = 0;

// Checkpoint test harness. Set CKPT_TEST_RESUME to 1 to save partway through
// an inference and pause, so you can cut power and watch it resume
// mid-network. 0 for normal operation.
// BISen's Stop (Sp) state, as the paper describes it: below Th_safe the MCU
// "preemptively leaves the active state", retains SRAM and registers, and the
// RTC wakes it periodically to "check its status and power level". NVM is NOT
// touched -- that only happens later, below Th_str. Most dips end here without
// a single write, which is where the paper's 86.4% reduction comes from.
//
// How long to stay asleep between checks. Long enough that the wait is cheap,
// short enough to notice a recovery promptly.
#ifndef SP_WAIT_US
#define SP_WAIT_US 20000u
#endif
// A bound, so a supply that never recovers cannot wedge the frame forever. On
// hitting it we fall through and let the checkpoint condition decide.
#ifndef SP_MAX_WAITS
#define SP_MAX_WAITS 250u
#endif
// Re-read the supply every N tiles rather than every tile: with a real ADC a
// read costs ~5 ms, which would dwarf the work between checks.
#ifndef INFER_POLL_TILES
#define INFER_POLL_TILES 8u
#endif

#define CKPT_TEST_RESUME   0
#define CKPT_TEST_AT_UNIT  20
#define CKPT_TEST_PAUSE_MS 15000   // 3 s proved too tight to catch by hand

// Knowing power is about to fail is the energy state machine's job. That state
// machine is now present: bisen_policy.cc, copied verbatim from the BISen port,
// reached through power_policy.h. This asks the cached decision rather than
// sampling, because the loop below runs about a thousand times per inference
// and a real VCAP reading still has nonzero ADC/mode-switch overhead.
static inline int power_critical(void) { return pp_should_checkpoint(); }

// Newest committed header across the slots, so we can print the seq and crc
// that were really stored instead of claiming a number came from storage.
#if CKPT_TEST_RESUME
static int newest_slot_hdr(ckpt_hdr_t *out) {
    int found = 0;
    for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
        ckpt_hdr_t h;
        if (ckpt_peek(s, &h) && (!found || h.seq > out->seq)) {
            *out = h;
            found = 1;
        }
    }
    return found;
}
#endif

// Run from wherever the context currently is, then publish the scores. Same
// code for a fresh inference and a restored one; the engine cannot tell.
static int run_to_completion(void) {
#if CKPT_TEST_RESUME
    int paused = 0;
    // Vary the trigger per inference. A fixed position makes the resumed value
    // a constant, which proves nothing.
    const uint32_t test_at = 20u + (g_infer_n * 37u) % 200u;
#endif
    int stopped_early = 0;
    uint32_t t0 = am_hal_stimer_counter_get();
    pwr_phase_t pwr_prev = pwr_begin(PWR_INFER);

    // One reading as the phase begins. The band it selects also sizes the tile:
    // more energy, more work per stop. Floored at INFER_TILE so the wait band
    // cannot stall a frame -- gating compute entirely is the scheduler's call,
    // and this build has no scheduler to hand the frame back to.
    pp_sample();
    pp_mark_compute();
    uint32_t tile = pp_chunk_units();
    if (tile == 0u) tile = INFER_TILE;

    uint32_t poll_n = 0;
    while (!nn_step(&g_net, (int)tile)) {
        // Re-measure periodically. es_read_cost_us() is 0 for a free source, so
        // a cheap one could poll every tile; the divide keeps an expensive one
        // from costing more than the work it protects.
        // Self-driving only -- see WL_EXTERNAL_DRIVER in workload.h.
        if (!WL_EXTERNAL_DRIVER &&
            ++poll_n >= (es_read_cost_us() == 0u ? 1u : INFER_POLL_TILES)) {
            poll_n = 0;
            pp_sample();

            // Th_safe: not enough energy to keep computing, but still enough to
            // finish a store if it comes to that. Stop, stay in RAM, and wait.
            // Deliberately BEFORE the checkpoint test -- waiting is the cheap
            // outcome and most dips end here without touching NVM.
            uint32_t waits = 0;
            if (!pp_compute_allowed() && !power_critical()) {
                am_util_stdio_printf(
                    "WAIT (Th_safe) at layer %u unit %u  VCAP=%u mV band=%s"
                    " -- holding in RAM, nothing written\n",
                    (unsigned)g_net.layer, (unsigned)g_net.unit,
                    (unsigned)pp_vcap_mv(), pp_band_name());
            }

            // Try to hand the watching to the ADC's window comparator. Armed,
            // it signals when the rail leaves the [checkpoint, resume] band in
            // either direction, so a poll costs one status bit instead of a
            // full conversion. Unarmed -- a simulated supply, or hardware that
            // refused -- we fall straight back to sampling every poll, which is
            // what this loop always did.
            const int armed = pp_wait_arm(
                bisen_checkpoint_trigger_mv(), bisen_resume_mv());
            if (armed) {
                am_util_stdio_printf(
                    "WAIT: window comparator armed, self-repeating=%d\n",
                    pp_wait_self_repeats());
            }

            while (!pp_compute_allowed() && !power_critical() &&
                   waits < SP_MAX_WAITS) {
                pp_enter_wait();
                pwr_wait_us(SP_WAIT_US);
                pp_leave_wait();

                if (armed) {
                    // Nothing to read while the rail stays inside the band.
                    // This is the whole point: the expensive part of the old
                    // loop happened on every poll to learn nothing.
                    if (!pp_wait_left_band()) { waits++; continue; }
                    // It left. ONE real reading tells the policy which way.
                    pp_wait_disarm();
                    pp_sample();
                    if (!pp_compute_allowed() && !power_critical() &&
                        waits + 1u < SP_MAX_WAITS) {
                        // Still in the wait band after all -- a transient or a
                        // sample that remains below the direct resume threshold.
                        // Re-arm instead of spinning on full ADC reads.
                        (void)pp_wait_arm(bisen_checkpoint_trigger_mv(),
                                          bisen_resume_mv());
                    }
                } else {
                    pp_sample();
                }
                waits++;
            }
            pp_wait_disarm();
            if (waits > 0u) {
                am_util_stdio_printf(
                    "WAIT ended after %u x %u us  VCAP=%u mV band=%s%s\n",
                    (unsigned)waits, (unsigned)SP_WAIT_US,
                    (unsigned)pp_vcap_mv(), pp_band_name(),
                    waits >= SP_MAX_WAITS ? "  (gave up waiting)" : "");
                // The band may have moved while we waited.
                tile = pp_chunk_units();
                if (tile == 0u) tile = INFER_TILE;
            }
        }

        // Th_str: below the backup threshold. Only now is NVM touched.
        if (power_critical()) {
            pp_mark_nvm_write();
            const int rc = ckpt_save(&g_net);
            if (rc == 0) pp_mark_committed(); else pp_note_checkpoint_failed();
            stopped_early = 1;
            am_util_stdio_printf(
                "CKPT %s (policy) at layer %u unit %u  VCAP=%u mV band=%s"
                "  %u B ~%u us\n",
                rc == 0 ? "saved" : "FAILED", (unsigned)g_net.layer,
                (unsigned)g_net.unit, (unsigned)pp_vcap_mv(), pp_band_name(),
                ckpt_bytes(&g_net), ckpt_write_us(&g_net));
            break;
        }
#if CKPT_TEST_RESUME
        if (!paused && nn_units_done(&g_net) >= test_at) {
            paused = 1;
            // ckpt_save returns an error code. Ignoring it made every failed
            // save look identical to a successful one on the serial log.
            int rc = ckpt_save(&g_net);
            am_util_stdio_printf(
                "CKPT %s at layer %u unit %u (%u/%u units, %u bytes, ~%u us)"
                " -- reset now to test resume (%ds)...\n",
                rc == 0 ? "saved" : "SAVE FAILED",
                (unsigned)g_net.layer, (unsigned)g_net.unit,
                (unsigned)nn_units_done(&g_net), (unsigned)nn_total_units(),
                ckpt_bytes(&g_net), ckpt_write_us(&g_net),
                CKPT_TEST_PAUSE_MS / 1000);
            // Read the header back and show seq + crc. The crc is computed over
            // THIS frame's pixels, so it differs every save -- which is what
            // lets you check afterwards that the value reported on resume came
            // from storage rather than from a constant.
            ckpt_hdr_t vh;
            if (newest_slot_hdr(&vh)) {
                am_util_stdio_printf(
                    "  STORED seq=%u crc=0x%08X at layer %u unit %u"
                    "  <-- remember this crc\n",
                    (unsigned)vh.seq, (unsigned)vh.crc,
                    (unsigned)vh.layer, (unsigned)vh.unit);
            } else {
                am_util_stdio_printf("  STORED: nothing committed (SAVE FAILED)\n");
            }
            // Keep the pause out of the measurement, or the reported inference
            // time is just the length of this delay.
            uint32_t p0 = am_hal_stimer_counter_get();
            am_util_delay_ms(CKPT_TEST_PAUSE_MS);
            t0 += am_hal_stimer_counter_get() - p0;
        }
#endif
    }

    pwr_end(pwr_prev);

    g_infer_tk   = am_hal_stimer_counter_get() - t0;
    g_infer_sum += g_infer_tk;
    g_infer_n++;
    if (g_infer_tk < g_infer_min) g_infer_min = g_infer_tk;
    if (g_infer_tk > g_infer_max) g_infer_max = g_infer_tk;

    // An inference cut short by the energy policy has NOT produced a result.
    // Everything below assumes it has: nn_scores() would return whatever the
    // output buffer happened to hold, and ckpt_clear() would erase the
    // checkpoint that was just written to survive this very moment. Leave both
    // alone and tell the caller there is no digit.
    //
    // This path was unreachable while power_critical() was a stub returning 0.
    // It became live the moment a real policy was wired in.
    if (stopped_early) {
        am_util_stdio_printf(
            "INFER stopped at layer %u unit %u (%u/%u units) -- checkpoint"
            " kept, no prediction this frame\n",
            (unsigned)g_net.layer, (unsigned)g_net.unit,
            (unsigned)nn_units_done(&g_net), (unsigned)nn_total_units());
        return -1;
    }

    const int8_t *out = nn_scores(&g_net);
    for (int i = 0; i < 10; i++) g_scores[i] = out[i];

    ckpt_clear();          // this inference is finished; nothing to resume
    return nn_argmax(&g_net);
}

int infer_init(void) {
    // What is sitting in the slots, before ckpt_restore() gets a chance to
    // blank a bad one. Silent on a cold start. ckpt_peek is read-only.
    for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
        ckpt_hdr_t h;
        if (ckpt_peek(s, &h)) {
            am_util_stdio_printf("CKPT slot %u holds layer %u unit %u (seq %u)\n",
                                 (unsigned)s, (unsigned)h.layer,
                                 (unsigned)h.unit, (unsigned)h.seq);
        }
    }

    if (ckpt_restore(&g_net) && nn_in_progress(&g_net)) {
        ckpt_hdr_t accepted;
        for (unsigned i = 0; i < sizeof accepted; i++) {
            ((uint8_t *)&accepted)[i] = 0;
        }
        (void)ckpt_last_restore(&accepted);
        g_boot_layer = g_net.layer;
        g_boot_unit  = g_net.unit;
        g_boot_seq   = accepted.seq;
        g_boot_crc   = accepted.crc;
        am_util_stdio_printf(
            "CKPT resume=INFERENCE at layer %u unit %u (%u/%u units done)"
            " seq=%u crc=0x%08X VERIFIED\n",
            (unsigned)g_net.layer, (unsigned)g_net.unit,
            (unsigned)nn_units_done(&g_net), (unsigned)nn_total_units(),
            (unsigned)accepted.seq, (unsigned)accepted.crc);
        return 1;
    }
    return 0;
}

// --- stepwise inference -----------------------------------------------------
// Separate from run_to_completion(), which owns the self-driving path and its
// own energy handling. These do the same work without deciding anything.

// Ticks accumulated across infer_step() calls. Wall clock from begin to end
// would include everything the caller does between steps -- sampling, logging,
// waiting for energy -- so only time actually spent inside the engine counts.
static uint32_t g_step_acc;

int infer_begin(const uint16_t raw[1024]) {
    int8_t in8[1024];
    g_step_acc = 0;
    preprocess(raw, in8);
    if (nn_begin(&g_net, in8) != 0) {
        am_util_stdio_printf("ENGINE INIT FAILED: NN_ACC_LEN too small "
                             "(need %u)\n", (unsigned)nn_acc_required());
        return -1;
    }
    return 0;
}

int infer_step(uint32_t max_units) {
    if (max_units == 0u) max_units = 1u;

    const uint32_t t0 = am_hal_stimer_counter_get();
    const int done = nn_step(&g_net, (int)max_units);
    g_step_acc += am_hal_stimer_counter_get() - t0;

    if (!done) return 0;

    // Same bookkeeping run_to_completion() does, or INFER reports n=0 and the
    // uninitialised min sentinel -- which is exactly what the first cam_lenet2
    // run printed.
    g_infer_tk   = g_step_acc;
    g_infer_sum += g_infer_tk;
    g_infer_n++;
    if (g_infer_tk < g_infer_min) g_infer_min = g_infer_tk;
    if (g_infer_tk > g_infer_max) g_infer_max = g_infer_tk;

    const int8_t *out = nn_scores(&g_net);
    for (int i = 0; i < 10; i++) g_scores[i] = out[i];
    return 1;
}

int infer_digit(void) { return nn_argmax(&g_net); }

int infer_classify(const uint16_t raw[1024]) {
    int8_t in8[1024];
    preprocess(raw, in8);
    // nn_begin refuses if this build's accumulator band is too small for the
    // model, which means the weights header and the engine disagree. Say so --
    // running on regardless would classify whatever was left in the buffers.
    if (nn_begin(&g_net, in8) != 0) {
        am_util_stdio_printf("ENGINE INIT FAILED: NN_ACC_LEN too small "
                             "(need %u) -- regenerate the weights header\n",
                             (unsigned)nn_acc_required());
        return -1;
    }
    return run_to_completion();
}

int infer_finish(void) { return run_to_completion(); }

const int8_t *infer_scores(void) { return g_scores; }

// The engine stops before Softmax, so g_scores holds the last Dense layer's
// raw LOGITS. The old TFLM build printed the SOFTMAX OUTPUT, quantised at
// 1/256, which pins every losing class at exactly -128 -- so the two builds'
// numbers are not comparable as printed. This converts logits to probabilities
// so they are. The engine itself stays softmax-free; nothing here feeds back
// into inference.
#if !defined(NN_OUT_SCALE) || !defined(NN_OUT_ZP)
#error "weights header lacks NN_OUT_SCALE/NN_OUT_ZP -- regenerate it with quantize_keras.py"
#endif

void infer_probs_per_mille(int out[10]) {
    float e[10], mx = -1e30f, sum = 0.0f;
    for (int i = 0; i < 10; i++) {
        e[i] = (float)(g_scores[i] - NN_OUT_ZP) * NN_OUT_SCALE;
        if (e[i] > mx) mx = e[i];
    }
    for (int i = 0; i < 10; i++) { e[i] = expf(e[i] - mx); sum += e[i]; }
    for (int i = 0; i < 10; i++) out[i] = (int)(1000.0f * e[i] / sum + 0.5f);
}

uint32_t infer_last_us(void) { return TICK_TO_US(g_infer_tk); }
uint32_t infer_avg_us(void) {
    return g_infer_n ? TICK_TO_US((uint32_t)(g_infer_sum / g_infer_n)) : 0;
}
uint32_t infer_min_us(void) { return TICK_TO_US(g_infer_min); }
uint32_t infer_max_us(void) { return TICK_TO_US(g_infer_max); }
uint32_t infer_count(void)  { return g_infer_n; }

nn_ctx_t *infer_ctx(void) { return &g_net; }

int      infer_boot_resume_layer(void) { return g_boot_layer; }
int      infer_boot_resume_unit(void)  { return g_boot_unit; }
uint32_t infer_boot_resume_seq(void)   { return g_boot_seq; }
uint32_t infer_boot_resume_crc(void)   { return g_boot_crc; }
