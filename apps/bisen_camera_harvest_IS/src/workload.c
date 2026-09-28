// workload.c -- see workload.h. Reports what is live; stores nothing.
#include "workload.h"

#include "infer.h"
#include "nn_engine.h"
#include "scan.h"

// Position encoding. One uint32 so it fits a caller's existing scalar field.
//
//   SCAN   pixel index, 0..1024
//   INFER  layer in the top 8 bits, unit in the low 24
//
// The 8/24 split rather than 16/16 on purpose: layers are bounded by the
// descriptor table (7 here, 256 is generous for anything), while units scale
// with the network -- 8094 for this LeNet but 3.2 million for a VGG-16 first
// convolution. Splitting the other way would cap a layer at 65,535 units.
#define POS_INFER(layer, unit) (((uint32_t)(layer) << 24) | ((unit) & 0xFFFFFFu))
#define POS_LAYER(p)           ((uint16_t)((p) >> 24))
#define POS_UNIT(p)            ((uint32_t)((p) & 0xFFFFFFu))

// Where the frame is. Kept here rather than inferred from scan/infer state
// because the gap between "scan finished" and "inference started" is a real
// position a caller can stop in, and nothing else records it.
typedef enum {
    WLS_IDLE = 0,   // nothing started
    WLS_SCAN,       // reading pixels
    WLS_INFER,      // running the network
    WLS_DONE,       // finished; result available
} wl_internal_t;

static wl_internal_t s_phase;

// volatile: wl_request_stop() may be called from an interrupt while wl_step()
// is running in the foreground.
static volatile uint8_t s_stop_requested;

static uint32_t align_up(uint32_t n) {
    return (n + (WL_ALIGN - 1u)) & ~(uint32_t)(WL_ALIGN - 1u);
}

static void add_region(wl_state_t *s, void *addr, uint32_t bytes) {
    if (bytes == 0u || s->region_count >= WL_MAX_REGIONS) return;
    // `bytes` is the TRUE length -- the number of meaningful bytes at `addr`,
    // and never more than the buffer holds. Do NOT round it up here: a length
    // can equal its buffer exactly (conv2's input is 1176 bytes and buf0 is
    // 1176 bytes), so rounding to 1184 would read 8 bytes past the end on save
    // and, far worse, WRITE 8 bytes past it on restore, into the start of the
    // next buffer.
    //
    // payload_bytes does round up, because that is a question about the slot
    // rather than about the source: MRAM programs in 16-byte blocks, so the
    // storage layer must pad the final partial block through a bounce buffer
    // and the space has to be reserved for it. ckpt.c's write_padded() is that
    // pattern.
    s->region[s->region_count].addr = addr;
    s->region[s->region_count].bytes = bytes;
    s->region_count++;
    s->payload_bytes += align_up(bytes);
}

// The live set of an inference: the part of the input still to be read, the
// output produced so far, and any open accumulators. nn_live() derives all
// three from (layer, unit) -- which is why the position is the only thing that
// has to be stored beside the bytes.
static void fill_infer(wl_state_t *s) {
    nn_ctx_t *c = infer_ctx();
    nn_live_t lv;
    nn_live(c, &lv);
    s->phase = WL_PHASE_INFER;
    s->position = POS_INFER(c->layer, c->unit);
    add_region(s, lv.in, lv.in_len);
    add_region(s, lv.out, lv.out_len);
    add_region(s, lv.acc, lv.acc_len);
}

// The live set of a scan: the pixels already read. Everything past the cursor
// has not been sampled yet and will be overwritten on resume.
static void fill_scan(wl_state_t *s) {
    const uint32_t px = (uint32_t)scan_next_pixel();
    s->phase = WL_PHASE_SCAN;
    s->position = px;
    add_region(s, scan_frame(), px * sizeof(uint16_t));
}

void wl_state(wl_state_t *out) {
    if (out == 0) return;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    out->phase = WL_PHASE_IDLE;

    // Inference first. It is the later phase, so if one is in flight the frame
    // has already been scanned and the scan's own state is spent.
    if (nn_in_progress(infer_ctx())) {
        fill_infer(out);
    } else if (scan_resumable()) {
        fill_scan(out);
    }

    out->dirty = (out->phase != WL_PHASE_IDLE && out->payload_bytes > 0u);
    out->write_us = (out->payload_bytes * 2u) / 3u;   // 1.5 B/us, see the header
}

int wl_dirty(void) {
    wl_state_t s;
    wl_state(&s);
    return s.dirty != 0u;
}

uint32_t wl_payload_bytes(void) {
    wl_state_t s;
    wl_state(&s);
    return s.payload_bytes;
}

uint32_t wl_write_us(void) {
    wl_state_t s;
    wl_state(&s);
    return s.write_us;
}

int wl_restore_plan(wl_phase_t phase, uint32_t position, wl_state_t *out) {
    if (out == 0) return -1;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    out->phase = phase;
    out->position = position;

    if (phase == WL_PHASE_SCAN) {
        if (position > 1024u) return -1;
        add_region(out, scan_frame(), position * sizeof(uint16_t));
    } else if (phase == WL_PHASE_INFER) {
        const uint16_t layer = POS_LAYER(position);
        const uint32_t unit = POS_UNIT(position);
        if (layer > NN_NUM_LAYERS) return -1;
        // Point the context at the stored position so nn_live() reports the
        // same regions the save reported, then describe them. The buffers are
        // static, so the addresses match the ones that were written.
        nn_ctx_t *c = infer_ctx();
        c->layer = layer;
        c->unit = (uint16_t)unit;
        nn_live_t lv;
        nn_live(c, &lv);
        add_region(out, lv.in, lv.in_len);
        add_region(out, lv.out, lv.out_len);
        add_region(out, lv.acc, lv.acc_len);
    } else {
        return -1;
    }

    out->dirty = (out->payload_bytes > 0u);
    out->write_us = (out->payload_bytes * 2u) / 3u;
    return 0;
}

int wl_restore_commit(wl_phase_t phase, uint32_t position) {
    if (phase == WL_PHASE_SCAN) {
        if (scan_adopt_position((uint16_t)position) != 0) return -1;
        // Without this the next wl_step() would fall into WLS_IDLE and begin a
        // new scan. It happens to recover for a scan, because scan_begin()
        // notices the adopted position -- but relying on that is luck, and for
        // an inference there is nothing equivalent to notice.
        s_phase = WLS_SCAN;
        return 0;
    }
    if (phase == WL_PHASE_INFER) {
        // wl_restore_plan already positioned the context and the caller has
        // filled the regions, so there is nothing left to move. Re-check rather
        // than assume the two calls were paired.
        const nn_ctx_t *c = infer_ctx();
        if (c->layer != POS_LAYER(position) ||
            c->unit != (uint16_t)POS_UNIT(position)) {
            return -1;
        }
        // THE critical line. Without it s_phase stays WLS_IDLE and the next
        // wl_step() calls scan_begin(), throwing the restored inference away
        // and silently producing a result for a different frame.
        s_phase = WLS_INFER;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Suspend / resume.

void wl_request_stop(void) { s_stop_requested = 1u; }
int  wl_stop_requested(void) { return s_stop_requested != 0u; }
void wl_resume(void) { s_stop_requested = 0u; }

void wl_reset(void) {
    // Retire any in-flight scan record too. Without this a reset mid-scan
    // leaves the progress record at SCAN_RUNNING, so the next scan_begin()
    // RESUMES the abandoned frame instead of starting a new one -- the exact
    // opposite of what a reset means.
    if (!scan_complete()) scan_discard();

    // And the inference context, for the same reason and with worse
    // consequences. wl_state() answers "what is live?" by asking the modules,
    // not by reading s_phase -- deliberately, so that it stays true across a
    // warm reset where s_phase is gone but the buffers survive. The cost is
    // that clearing s_phase alone does not make a frame go away: a context left
    // in progress keeps being reported, so a caller that reset and then asked
    // what to save would be handed the abandoned frame's regions and would
    // store them under a fresh position.
    //
    // Found by the API conformance test: "reset -> not dirty" and
    // "stopped mid-scan -> phase SCAN" both failed on a warm boot, because the
    // previous run's context was still in progress and fill_infer() outranks
    // fill_scan().
    nn_abandon(infer_ctx());

    s_phase = WLS_IDLE;
    s_stop_requested = 0u;
}

int wl_result(void) {
    return (s_phase == WLS_DONE) ? infer_digit() : -1;
}

wl_phase_t wl_current_phase(void) {
    switch (s_phase) {
        case WLS_SCAN:  return WL_PHASE_SCAN;
        case WLS_INFER: return WL_PHASE_INFER;
        default:        return WL_PHASE_IDLE;
    }
}

wl_step_result_t wl_step(uint32_t max_units) {
    // Report completion ahead of a pending stop. A caller that requested a stop
    // and then finds the frame already finished needs to hear COMPLETE, or it
    // would persist state for work that no longer exists.
    if (s_phase == WLS_DONE) return WL_STEP_COMPLETE;
    if (s_stop_requested) return WL_STEP_STOPPED;
    if (max_units == 0u) max_units = 1u;

    switch (s_phase) {
        case WLS_IDLE:
            scan_begin();
            s_phase = WLS_SCAN;
            // fall through: doing nothing here would make an empty call
            // indistinguishable from a stop.
            /* FALLTHROUGH */

        case WLS_SCAN:
            if (!scan_complete()) {
                (void)scan_step((int)max_units);
                return WL_STEP_PROGRESS;
            }
            // The scan just finished. Preprocessing and nn_begin are one
            // indivisible step -- there is no coherent position part-way
            // through turning a frame into a network input, so it happens
            // whole or not at all.
            if (infer_begin(scan_frame()) != 0) {
                s_phase = WLS_IDLE;
                return WL_STEP_ERROR;
            }
            s_phase = WLS_INFER;
            return WL_STEP_PROGRESS;

        case WLS_INFER:
            if (infer_step(max_units)) {
                s_phase = WLS_DONE;
                return WL_STEP_COMPLETE;
            }
            return WL_STEP_PROGRESS;

        case WLS_DONE:
            // A finished frame is not a resumable position. Start the next one
            // only when asked, so a caller can read the result first.
            return WL_STEP_COMPLETE;
    }
    return WL_STEP_ERROR;
}

// The scan is a fixed 1024 pixels for this sensor. Named rather than repeated
// so the two places that need it cannot drift apart.
#define WL_SCAN_UNITS 1024u

uint32_t wl_units_remaining(void) {
    switch (s_phase) {
        case WLS_IDLE: {
            // Nothing is in flight, but the next wl_step() starts a scan, and a
            // caller asking this is sizing that call.
            return WL_SCAN_UNITS;
        }
        case WLS_SCAN: {
            const uint32_t px = (uint32_t)scan_next_pixel();
            return px >= WL_SCAN_UNITS ? 0u : WL_SCAN_UNITS - px;
        }
        case WLS_INFER: {
            const uint32_t total = nn_total_units();
            const uint32_t done  = nn_units_done(infer_ctx());
            return done >= total ? 0u : total - done;
        }
        default:
            return 0u;   // WLS_DONE: the frame is finished, nothing remains
    }
}

uint32_t wl_unit_cost_us(void) {
    // Averaged over the last completed instance of the phase. Zero means the
    // phase has not run yet, which a caller must read as "unknown" and fall
    // back on its own default -- not as "costs nothing".
    switch (s_phase) {
        case WLS_IDLE:
        case WLS_SCAN: {
            const uint32_t us = scan_last_us();
            return us ? us / WL_SCAN_UNITS : 0u;
        }
        case WLS_INFER: {
            const uint32_t us = infer_last_us();
            const uint32_t n  = nn_total_units();
            return (us && n) ? us / n : 0u;
        }
        default:
            return 0u;
    }
}

uint32_t wl_phase_last_us(wl_phase_t phase) {
    if (phase == WL_PHASE_INFER) return infer_last_us();
    if (phase == WL_PHASE_SCAN) return scan_last_us();
    return 0u;
}

const char *wl_phase_name(wl_phase_t phase) {
    switch (phase) {
        case WL_PHASE_SCAN:  return "scan";
        case WL_PHASE_INFER: return "inference";
        case WL_PHASE_IDLE:  return "idle";
    }
    return "unknown";
}
