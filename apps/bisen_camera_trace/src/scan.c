// scan.c -- reading the array, and deciding how much of it to read.
//
// All three modes poll box centres and compare against the background; they
// differ only in what they do about a change. NORMAL reads everything anyway,
// BOX re-reads that one box, CLASSIFICATION waits until enough boxes change
// at once. The shared half is here.
#include "scan.h"
#include "sensor.h"
#include "ckpt.h"
#include "power.h"
#include "power_policy.h"
#include "workload.h"
#include "am_mcu_apollo.h"
#include "am_util.h"

// Scan progress, in persistent RAM. Not the same thing as the inference
// checkpoint. The scan takes seconds, so it saves after every pixel; that only
// costs a store and a barrier since the frame is already in persistent RAM.
#define NS_PERSIST __attribute__((section(".persist")))
#define SCAN_CKPT_MAGIC 0xCA3EF00Du
enum { SCAN_IDLE = 0, SCAN_RUNNING = 1 };

typedef struct {
    uint32_t magic;
    uint8_t  stage;          // SCAN_IDLE / SCAN_RUNNING
    uint16_t next_pixel;     // next pixel index 0..1024
} scan_ckpt_t;

NS_PERSIST static scan_ckpt_t g_scan_ckpt;
NS_PERSIST static uint16_t    g_frame[1024];

// Set to 1 to write one MRAM scan checkpoint per scan, at a varying pixel, and
// print where it landed. For proving power-loss resume by hand: the scan takes
// about 3.6 s, so there is time to pull the plug after the line appears.
// 0 for normal operation -- there is no automatic trigger yet, so with this off
// nothing ever writes a scan checkpoint.
//
// ONE MRAM program per scan. At ~4.6 s per frame that is ~780/hour against a
// 100,000-cycle endurance, so roughly 128 hours of continuous testing. Do not
// leave it on.
// How often to take an energy reading during a scan.
//
// The bound is not cost, it is MISSING THE WINDOW. The 5900 -> 5700 mV
// checkpoint band holds 0.5*C*(5.9^2-5.7^2) = 2.32 mJ, and this application
// draws about 7.77 mW while scanning, so the supply crosses that band in
// roughly 300 ms on our own load alone -- sooner if the harvester has stopped.
// Polling less often than that lets the rail fall from "fine" to below the
// sleep floor between two reads, with no checkpoint written.
//
//   every 128 px = 452 ms between polls  -- LONGER than the crossing, unsafe
//   every  64 px = 226 ms between polls  -- fits, with ~25% margin
//   every  32 px = 113 ms between polls  -- comfortable, but 4.6% of the scan
//
// 64 is the compromise. With PP_USE_REAL_ADC 0 the read is pure arithmetic and
// this costs nothing; with the real ADC each read is 5.21 ms (a 5 ms divider
// settle that am_hal_delay_us SPINS through), so 16 reads add 83 ms and 648 uJ
// to a ~1500 uJ frame. If that becomes the limit, the fix is on the ADC side --
// see the note in docs/OWNERSHIP.md -- not a coarser interval here.
// BISen's Stop state, mirrored from infer.c -- see the note there.
#ifndef SP_WAIT_US
#define SP_WAIT_US 20000u
#endif
#ifndef SP_MAX_WAITS
#define SP_MAX_WAITS 250u
#endif

#ifndef PP_SCAN_POLL_PIXELS
#define PP_SCAN_POLL_PIXELS 64
#endif

#ifndef SCAN_CKPT_TEST
#define SCAN_CKPT_TEST 0
#endif

#if SCAN_CKPT_TEST
static uint32_t g_scan_n;   // scans since boot, only to vary the save point
#endif

// How long the last scan took. STIMER runs at 6 MHz off the HFRC and is not in
// the debug power domain, so unlike DWT->CYCCNT it keeps counting here.
static uint32_t g_scan_us;
// Ticks accumulated by scan_step() across a frame. scan_full() does not use
// this -- it times its own loop in one span.
static uint32_t g_scan_step_acc;

static inline void ckpt_commit(void) { __DSB(); }

static inline int ckpt_present(void) { return g_scan_ckpt.magic == SCAN_CKPT_MAGIC; }

static void ckpt_begin(void) {
    g_scan_ckpt.magic = SCAN_CKPT_MAGIC;
    g_scan_ckpt.stage = SCAN_RUNNING;
    g_scan_ckpt.next_pixel = 0;
    ckpt_commit();
}

static inline void ckpt_mark(uint16_t next) {
    g_scan_ckpt.next_pixel = next;
    ckpt_commit();
}

static void ckpt_done(void) {
    g_scan_ckpt.stage = SCAN_IDLE;
    ckpt_commit();
    // The frame is complete, so any scan record on media now describes a frame
    // that no longer needs resuming. Left there, the next cold boot would
    // reload its first pixels and scan the rest on top -- two frames in one
    // buffer. This reads first and only programs when a record exists, so a
    // scan that never checkpointed costs nothing.
#if !WL_EXTERNAL_DRIVER
    (void)ckpt_scan_invalidate();
#endif
}

// Background reference
static uint16_t g_bg[GRID_SIZE][GRID_SIZE];   // baseline box-centre values
static uint16_t g_cur[GRID_SIZE][GRID_SIZE];  // most recently polled centres

static inline int abs_diff(uint16_t a, uint16_t b) {
    int d = (int)a - (int)b;
    return d < 0 ? -d : d;
}

// Read only the centre pixel of every box. This is the cheap poll the whole
// event-driven scheme rests on: GRID_SIZE^2 pixels instead of 1024.
static void capture_centers(uint16_t centers[GRID_SIZE][GRID_SIZE]) {
    for (int br = 0; br < GRID_SIZE; br++) {
        int y = br * BOX_SIZE + BOX_SIZE / 2;
        for (int bc = 0; bc < GRID_SIZE; bc++) {
            int x = bc * BOX_SIZE + BOX_SIZE / 2;
            centers[br][bc] = read_pixel(x, y);
        }
    }
}

// Re-read one whole box into the frame.
static void scan_box(int br, int bc) {
    int sx = bc * BOX_SIZE, sy = br * BOX_SIZE;
    for (int dy = 0; dy < BOX_SIZE; dy++)
        for (int dx = 0; dx < BOX_SIZE; dx++) {
            int col = sx + dx, row = sy + dy;
            g_frame[row * 32 + col] = read_pixel(col, row);
        }
}

// Public
uint16_t *scan_frame(void) { return g_frame; }

// --- stepwise scanning ------------------------------------------------------
// Deliberately separate from scan_full()'s loop rather than shared with it.
// scan_full() is the self-driving path that already works on hardware; these
// are for an external scheduler. The duplicated three lines are cheaper than
// the risk of restructuring a proven loop.

void scan_begin(void) {
    if (scan_resumable()) {
        // Resuming: keep whatever has already been accumulated for this frame.
        if (g_scan_ckpt.next_pixel < 1024)
            am_util_stdio_printf("RESUME scan at pixel %d/1024\n",
                                 g_scan_ckpt.next_pixel);
        return;                      // carry on from where it stopped
    }
    g_scan_step_acc = 0u;            // a fresh frame times from zero
    ckpt_begin();
}

void scan_discard(void) {
    g_scan_ckpt.stage = SCAN_IDLE;
    g_scan_ckpt.next_pixel = 0;
    ckpt_commit();
#if !WL_EXTERNAL_DRIVER
    (void)ckpt_scan_invalidate();   // self-driving store owns invalidation
#endif
}

int scan_complete(void) {
    return !ckpt_present() || g_scan_ckpt.stage == SCAN_IDLE ||
           g_scan_ckpt.next_pixel >= 1024;
}

int scan_step(int max_pixels) {
    if (max_pixels <= 0 || scan_complete()) return 0;
    // Accumulate across calls, the way infer_step() does. scan_full() times
    // itself with one span because it owns the whole loop; the stepper does
    // not, so without this g_scan_us stays 0 in an externally driven build and
    // wl_unit_cost_us() cannot cost the scan -- which is 84% of the frame.
    //
    // Found by the API conformance test: "the scan reported a duration" failed
    // after a frame that had plainly just run.
    const uint32_t t0 = am_hal_stimer_counter_get();
    int idx = (int)g_scan_ckpt.next_pixel;
    int n = 0;
    while (idx < 1024 && n < max_pixels) {
        g_frame[idx] = read_pixel(idx % 32, idx / 32);
        ckpt_mark((uint16_t)(idx + 1));
        idx++;
        n++;
    }
    g_scan_step_acc += am_hal_stimer_counter_get() - t0;
    if (idx >= 1024) {
        // Same completion bookkeeping scan_full() does: retire any scan record
        // on media, because a complete frame's record would splice two frames
        // together on a later restore.
        g_scan_us = g_scan_step_acc / 6u;   // 6 ticks == 1 us
        ckpt_done();
    }
    return n;
}

int scan_resumable(void) {
    return ckpt_present() && g_scan_ckpt.stage == SCAN_RUNNING &&
           g_scan_ckpt.next_pixel <= 1024;
}

int scan_next_pixel(void) { return g_scan_ckpt.next_pixel; }

// The frame has to fit the payload area the inference already reserves. It does
// by a wide margin -- 2048 bytes against NN_LIVE_MAX -- but fail the build
// rather than the board if a larger sensor ever changes that.
typedef char scan_frame_fits_slot[
    (1024u * CKPT_SCAN_PIXEL_SZ <= CKPT_SLOT_SZ - CKPT_HDR_SZ) ? 1 : -1];

uint32_t scan_save_bytes(void) {
    if (!ckpt_present() || g_scan_ckpt.stage != SCAN_RUNNING) return 0;
    return ckpt_scan_bytes(g_scan_ckpt.next_pixel);
}

uint32_t scan_save_us(void) {
    if (!ckpt_present() || g_scan_ckpt.stage != SCAN_RUNNING) return 0;
    return ckpt_scan_write_us(g_scan_ckpt.next_pixel);
}

// FNV-1a over the first `pixels` entries. Not a CRC and not used for
// integrity -- ckpt.c does that. This only has to change when the data changes.
uint32_t scan_frame_crc(uint16_t pixels) {
    uint32_t h = 2166136261u;
    if (pixels > 1024u) pixels = 1024u;
    for (uint16_t i = 0; i < pixels; i++) {
        h ^= (uint32_t)g_frame[i];
        h *= 16777619u;
    }
    return h;
}

int scan_save_mram(void) {
    // Nothing in flight means nothing to preserve: a finished scan has already
    // been consumed into the inference buffers, and those are ckpt_save's job.
    if (!ckpt_present() || g_scan_ckpt.stage != SCAN_RUNNING) return 0;
    return ckpt_save_scan(g_frame, g_scan_ckpt.next_pixel) == 0;
}

int scan_adopt_position(uint16_t next_pixel) {
    if (next_pixel > 1024u) return -1;
    // Rebuild the SRAM progress record so scan_full() resumes through its
    // normal path and nothing else in this file needs to know where the frame
    // came from.
    g_scan_ckpt.magic = SCAN_CKPT_MAGIC;
    g_scan_ckpt.stage = SCAN_RUNNING;
    g_scan_ckpt.next_pixel = next_pixel;
    ckpt_commit();
    return 0;
}

uint32_t scan_last_us(void) { return g_scan_us; }

int scan_restore_mram(void) {
    uint16_t next = 0;
    if (!ckpt_restore_scan(g_frame, (uint32_t)sizeof(g_frame), &next)) return 0;
    return scan_adopt_position(next) == 0;
}

void scan_full(void) {
    int start = 0;
    if (scan_resumable()) {
        start = g_scan_ckpt.next_pixel;   // pixels [0,start) already valid
        if (start < 1024)
            am_util_stdio_printf("RESUME scan at pixel %d/1024\n", start);
    } else {
        ckpt_begin();
    }
#if SCAN_CKPT_TEST
    // Vary the save point per scan. A fixed one makes the resumed position a
    // constant, which carries no information -- the same mistake the first
    // inference-resume test made.
    const int test_at = 100 + (int)((g_scan_n++ * 137u) % 800u);
#endif
    const uint32_t t0 = am_hal_stimer_counter_get();
    int policy_saved = 0;
    for (int idx = start; idx < 1024; idx++) {
        g_frame[idx] = read_pixel(idx % 32, idx / 32);
        ckpt_mark((uint16_t)(idx + 1));

        // A scan runs for seconds, so one reading at the start would be stale
        // long before the end. Sampling every pixel would be wasteful, so poll
        // periodically and write at most one checkpoint per scan -- the policy
        // stays in the low-energy region once it gets there, and rewriting on
        // every subsequent pixel would spend endurance for nothing.
        // Self-driving only. With an external driver the caller decides when
        // to look and when to save, and doing it here as well would mean two
        // owners of the same decision.
        if (!WL_EXTERNAL_DRIVER &&
            !policy_saved && ((idx + 1) % PP_SCAN_POLL_PIXELS) == 0) {
            pp_sample();

            // Th_safe during the scan. Same rule as the inference: stop
            // sampling pixels, hold the partial frame in RAM, and wait. The
            // frame is already in .persist, so nothing needs writing to survive
            // a wait -- only a real power loss needs NVM.
            uint32_t waits = 0;
            if (!pp_compute_allowed() && !pp_should_checkpoint()) {
                am_util_stdio_printf(
                    "WAIT (Th_safe) SCAN at pixel %d/1024  VCAP=%u mV"
                    " band=%s -- holding in RAM, nothing written\n",
                    idx + 1, (unsigned)pp_vcap_mv(), pp_band_name());
            }

            // Hand the watching to the ADC's window comparator if it will take
            // it. This is the wait that actually fires in practice -- the scan
            // is 3.6 s and the inference 23 ms -- so it is the one where not
            // taking a full supply reading on every poll is worth the most.
            //
            // Armed, the hardware signals when the rail leaves the
            // [checkpoint, resume] band in either direction. Unarmed (a
            // simulated supply, or hardware that refused) this falls straight
            // back to sampling every poll, exactly as it always did.
            const int armed = pp_wait_arm(
                bisen_checkpoint_trigger_mv(), bisen_resume_mv());
            if (armed) {
                am_util_stdio_printf(
                    "WAIT: window comparator armed, self-repeating=%d\n",
                    pp_wait_self_repeats());
            }

            while (!pp_compute_allowed() && !pp_should_checkpoint() &&
                   waits < SP_MAX_WAITS) {
                pp_enter_wait();
                pwr_wait_us(SP_WAIT_US);
                pp_leave_wait();

                if (armed) {
                    // Inside the band nothing has changed and there is nothing
                    // worth reading. That is the saving.
                    if (!pp_wait_left_band()) { waits++; continue; }
                    // It left. ONE real reading tells the policy which way.
                    pp_wait_disarm();
                    pp_sample();
                    if (!pp_compute_allowed() && !pp_should_checkpoint() &&
                        waits + 1u < SP_MAX_WAITS) {
                        (void)pp_wait_arm(bisen_checkpoint_trigger_mv(),
                                          bisen_resume_mv());  // transient
                    }
                } else {
                    pp_sample();
                }
                waits++;
            }
            pp_wait_disarm();
            if (waits > 0u) {
                am_util_stdio_printf("WAIT ended after %u x %u us  VCAP=%u mV band=%s\n",
                                     (unsigned)waits, (unsigned)SP_WAIT_US,
                                     (unsigned)pp_vcap_mv(), pp_band_name());
            }

            if (pp_should_checkpoint()) {
                pp_mark_nvm_write();
                const int ok = scan_save_mram();
                if (ok) pp_mark_committed(); else pp_note_checkpoint_failed();
                policy_saved = 1;
                am_util_stdio_printf(
                    "CKPT %s (policy) SCAN at pixel %d/1024  VCAP=%u mV"
                    " band=%s  %u B ~%u us\n",
                    ok ? "saved" : "FAILED", idx + 1,
                    (unsigned)pp_vcap_mv(), pp_band_name(),
                    (unsigned)scan_save_bytes(), (unsigned)scan_save_us());
            }
        }
#if SCAN_CKPT_TEST
        if (idx + 1 == test_at) {
            const int ok = scan_save_mram();
            am_util_stdio_printf(
                "SCAN SAVED %s at pixel %d/1024  frame_crc=0x%08X"
                "  %u B  ~%u us  -- CUT POWER NOW\n",
                ok ? "ok" : "FAILED", idx + 1,
                (unsigned)scan_frame_crc((uint16_t)(idx + 1)),
                (unsigned)scan_save_bytes(), (unsigned)scan_save_us());
        }
#endif
    }
    g_scan_us = (am_hal_stimer_counter_get() - t0) / 6u;   // 6 ticks == 1 us
    ckpt_done();
}

void scan_reset_background(void) {
    capture_centers(g_bg);
    scan_full();   // seed the image so untouched regions are valid
}

int scan_refresh_changed_boxes(int *maxdiff, int *nover) {
    capture_centers(g_cur);
    int changed = 0, mx = 0, n = 0;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int diff = abs_diff(g_cur[r][c], g_bg[r][c]);
            if (diff > mx) mx = diff;
            if (diff > BOX_PRECISION) {
                n++;
                scan_box(r, c);            // refresh only this region
                g_bg[r][c] = g_cur[r][c];  // baseline follows the scene
                changed = 1;
            }
        }
    }
    if (maxdiff) *maxdiff = mx;
    if (nover)   *nover = n;
    return changed;
}

int scan_change_percent(int *maxdiff) {
    capture_centers(g_cur);
    int changed = 0, mx = 0;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int diff = abs_diff(g_cur[r][c], g_bg[r][c]);
            if (diff > mx) mx = diff;
            if (diff > CLS_PRECISION) changed++;
        }
    }
    if (maxdiff) *maxdiff = mx;
    return (changed * 100) / (GRID_SIZE * GRID_SIZE);
}

void scan_accept_baseline(void) {
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            g_bg[r][c] = g_cur[r][c];
}
