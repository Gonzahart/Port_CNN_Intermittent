#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stdint.h>

// workload.h -- what a storage layer needs in order to checkpoint this
// workload, and nothing more.
//
// This module OWNS NO STORAGE. It never programs MRAM, never decides when to
// save, and never reads a voltage. It answers four questions:
//
//     what is live right now?      -> wl_state()
//     where are we?                -> wl_state_t.phase + .position
//     what would saving cost?      -> .payload_bytes / .write_us
//     where do the bytes go back?  -> wl_restore_plan() / wl_restore_commit()
//
// The caller owns the policy and the media. That split is deliberate: the
// energy state machine and the NVM backend belong to the scheduler, and the
// only thing it cannot work out for itself is which bytes matter.
//
// WHY THE REGIONS ARE NOT ONE BLOCK
// Live state is not contiguous and its extent changes as work progresses. Part
// of the input buffer is already consumed and will never be read again; part of
// the output buffer is not written yet. Reporting regions rather than "the
// whole context" is what keeps a checkpoint at 4864 bytes instead of 5888, and
// mid-scan it is what makes an early brownout cheaper than a late one.
//
// WHY POSITION IS A SINGLE uint32
// So it fits an existing scalar field in a caller's record without widening it.
// The encoding is this module's business; a caller stores it and hands it back.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WL_PHASE_IDLE  = 0,   // nothing in flight -- nothing worth saving
    WL_PHASE_SCAN  = 1,   // sensor scan partway through a frame
    WL_PHASE_INFER = 2,   // inference partway through the network
} wl_phase_t;

// Regions are already padded to WL_ALIGN, so a backend can program them
// directly without a bounce buffer. Addresses are aligned at least as well as
// the buffers they point into.
#define WL_ALIGN       16
#define WL_MAX_REGIONS 4

typedef struct {
    void    *addr;
    // The TRUE number of meaningful bytes at addr -- NOT rounded up. A region
    // can end exactly at the end of its buffer, so a rounded length would read
    // past it on save and write past it on restore.
    //
    // MRAM programs in WL_ALIGN-byte blocks, so a storage layer still has to
    // pad the final partial block through a bounce buffer. wl_state_t's
    // payload_bytes already accounts for that padding, so reserve by that and
    // copy by this.
    uint32_t bytes;
} wl_region_t;

typedef struct {
    wl_phase_t  phase;
    uint32_t    position;       // opaque; store it and hand it back verbatim
    uint8_t     dirty;          // 0 = nothing to save, skip the write entirely
    uint32_t    payload_bytes;  // sum of region bytes
    uint32_t    write_us;       // estimated, see wl_write_us()
    uint32_t    region_count;
    wl_region_t region[WL_MAX_REGIONS];
} wl_state_t;

// Describe what is live at this instant. Safe to call at any time; it reads
// state and allocates nothing. With phase IDLE or dirty == 0 there is nothing
// to write and the caller should skip the save -- completion is not a
// persistence event.
void wl_state(wl_state_t *out);

// Cheap answers for a scheduler that only wants to size an energy reserve and
// does not need the region list. Equivalent to the matching wl_state fields.
int      wl_dirty(void);
uint32_t wl_payload_bytes(void);

// Estimated microseconds to program wl_payload_bytes().
//
// ⚠ Derived from the Apollo4 datasheet's 1.5 kB/ms burst figure, which sits in
// the table's *Max* column. A rough measurement on this board suggests about
// 750 B/ms, i.e. roughly TWICE this estimate. Size reserves with margin, and do
// not quote this number as measured.
uint32_t wl_write_us(void);

// Restore, in two steps so the caller keeps control of the media.
//
//   MRAM -> wl_restore_plan() says "these bytes belong HERE"
//        -> the caller does the read
//        -> wl_restore_commit() -> wl_step() continues
//
// plan() COPIES NOTHING. It moves no data and touches no buffer; it only
// reports where the saved bytes belong. The caller owns the media read.
//
//   1. wl_restore_plan(phase, position, &s) fills s.region with the addresses
//      and lengths the stored bytes must be read back into. The regions are a
//      pure function of (phase, position), which is why nothing but the
//      position has to be stored alongside the payload.
//   2. the caller reads its payload into those regions, in order
//   3. wl_restore_commit(phase, position) makes the workload adopt it
//
// Both return 0 on success, -1 if the position is not one this build can
// resume from -- which a caller should treat as "start this frame over",
// never as a hard error.
int wl_restore_plan(wl_phase_t phase, uint32_t position, wl_state_t *out);
int wl_restore_commit(wl_phase_t phase, uint32_t position);

// ---------------------------------------------------------------------------
// SUSPEND / RESUME CONTROL
//
// For a caller that owns both the decision and the media: it drives the work,
// stops it when energy runs low, persists whatever wl_state() reports, and
// starts it again when energy returns. This module decides nothing and stores
// nothing.
//
// A "unit" is one pixel during the scan and one output element during
// inference. The caller does not have to know which -- wl_step() handles the
// transition from scanning to inferring internally, and wl_state() reports the
// phase if it wants to know.
//
// BETWEEN ANY TWO wl_step() CALLS THE STATE IS COHERENT. That is the contract:
// stop wherever you like, and wl_state() describes exactly what has to survive.
// Nothing is half-written, and no partial sum is stranded -- see the note on
// output-stationary in nn_engine.h for why that holds.

// WHO OWNS THE SCHEDULE
//
// Two scheduling modes. Exactly one may be in charge at a time -- two owners
// of the checkpoint decision would race.
//
//   0 = SELF-DRIVING    the workload decides when to poll energy, wait,
//                       and checkpoint. It also owns the media.
//   1 = EXTERNAL        the caller decides when to run, stop, sleep, resume
//                       and save. It also owns the media.
//
// The mode is set per application in its module.mk -- cam_lenet1 defines 0,
// cam_lenet2 defines 1 -- so this header stays byte-identical between the two
// and syncing them is a plain copy. The default below only covers a build that
// forgets to say.
//
// The detail, if you need it:
//
//   WL_EXTERNAL_DRIVER 0  (default)  SELF-DRIVING.
//       scan_full() and infer_classify() own their loops, sample the energy
//       source at their own interval (PP_SCAN_POLL_PIXELS, INFER_POLL_TILES),
//       apply the BISen policy themselves and write MRAM through ckpt.c.
//       Everything runs on one board with no external scheduler. This is what
//       has been tested.
//
//   WL_EXTERNAL_DRIVER 1             EXTERNALLY DRIVEN.
//       Something else calls wl_step() and owns the decision AND the media.
//       The self-driving polls, waits and saves are compiled out, so
//       PP_SCAN_POLL_PIXELS and INFER_POLL_TILES stop meaning anything -- the
//       granularity is whatever max_units the caller passes. power_policy.cc
//       and ckpt.c's writers become unused.
//
// The two are not two ways of calling the same loop. In mode 1 the application
// drives everything through wl_step(); in mode 0 it does not call wl_step() at
// all -- scan_full() and infer_classify() run their own loops and the
// equivalent stop-and-save happens inside them. The API below is compiled in
// either way, but it is only the application's path in mode 1.
//
// THE WHOLE CYCLE IN MODE 1
//
//     wl_step()            do some whole units
//         -> stops at a unit boundary, never inside one
//     wl_state()           what has to survive, and what it costs
//         -> the caller saves it, wherever it likes
//         -> sleep / brownout
//     wl_resume()          position was never lost; just clear the flag
//     wl_step()            continues from the same unit
//
// After a power cut the middle changes: wl_restore_plan() -> the caller's read
// -> wl_restore_commit(), and then wl_step() as before.
#ifndef WL_EXTERNAL_DRIVER
#define WL_EXTERNAL_DRIVER 0
#endif

typedef enum {
    WL_STEP_PROGRESS = 0,   // work remains; call again when energy allows
    WL_STEP_STOPPED,        // stopped because a stop was requested
    WL_STEP_COMPLETE,       // the frame is finished; wl_result() is valid
    WL_STEP_ERROR,
} wl_step_result_t;

// Do up to max_units units of work, then return. The budget is a ceiling, not
// a target: the call returns when the budget is spent, a stop is requested, a
// phase completes, or an error occurs -- whichever comes first.
//
// A unit is never split across calls. Whatever the reason for returning, the
// unit in progress is finished first, which is why the state is always
// coherent on the way out.
wl_step_result_t wl_step(uint32_t max_units);

// Ask the workload to stop at the next unit boundary.
//
// ISR-SAFE. Sets a flag that wl_step() checks between units; it never
// interrupts a unit already in progress. Work already done is kept and the
// position is preserved, so
// this is "pause", never "abandon". wl_step() returns WL_STEP_STOPPED, and
// keeps returning it until wl_resume() is called -- so a caller that saved and
// then lost power finds the same state waiting on the way back.
void wl_request_stop(void);

// Has a stop been requested and not yet cleared?
int wl_stop_requested(void);

// Clear the request and allow wl_step() to make progress again.
//
// The scan pixel, or the (layer, unit) pair, is already held in this module's
// own state -- it never left. wl_resume() does not reload or reconstruct
// anything and touches no buffers; it only clears the stop request, so the
// next wl_step() carries on from that state. (Reloading after a power cut is
// wl_restore_plan()/wl_restore_commit(); this is the no-power-cut path.)
void wl_resume(void);

// Which phase the stepper is in, without building a whole wl_state_t. After a
// successful wl_restore_commit() this reports the restored phase, which is how
// a caller can confirm the restore was actually adopted.
wl_phase_t wl_current_phase(void);

// The classification, valid only after WL_STEP_COMPLETE. Negative on error.
int wl_result(void);

// Abandon the current frame and start over from a fresh scan. For a caller
// that decides a partially-done frame is no longer worth finishing -- after a
// restore whose position it could not honour, for instance.
void wl_reset(void);

// Microseconds the last completed instance of each phase took, so a caller can
// feed its own energy model rather than assuming. Scan and inference only;
// WL_PHASE_IDLE returns 0.
uint32_t wl_phase_last_us(wl_phase_t phase);

// ---------------------------------------------------------------------------
// SIZING A STEP
//
// wl_step()'s budget is in UNITS, and a unit is not a fixed amount of work: one
// pixel during the scan, one output element during inference. On this model
// that is about 3.6 ms against 2.8 us -- a factor of ~1300. So the same
// max_units means wildly different amounts of time depending on the phase, and
// a caller that maps an energy band straight onto a unit count without checking
// the phase can accidentally ask for the whole frame in one uninterruptible
// call.
//
// These two turn budgeting from guesswork into arithmetic:
//
//     uint32_t cost = wl_unit_cost_us();
//     uint32_t want = cost ? (my_microseconds_of_energy / cost) : MY_DEFAULT;
//     uint32_t left = wl_units_remaining();
//     wl_step(want < left ? want : left);
//
// They exist so a caller never has to know that this model has 1024 pixels and
// 8094 inference units. Those numbers change with the model, which is exactly
// why the position encoding is opaque.

// How many units are left in the phase that is running now.
//
// While idle this reports the full scan, because a scan is what the next
// wl_step() begins -- returning 0 would be literally true and useless, since a
// caller sizing its next call would compute a budget of zero.
uint32_t wl_units_remaining(void);

// Measured microseconds per unit in the phase running now, or 0 if that phase
// has not completed once yet and there is nothing to measure.
//
// This is an AVERAGE over the last completed instance of the phase, not a
// prediction for the next unit. Units within a phase are not uniform -- the
// convolution layers are heavier than the dense ones -- so treat it as a
// planning figure, and note that 0 means "unknown", never "free".
uint32_t wl_unit_cost_us(void);

// Human-readable phase name, for logging.
const char *wl_phase_name(wl_phase_t phase);

// ---------------------------------------------------------------------------
// WATCHING THE SUPPLY
//
// These exist because this application OWNS THE ADC and a caller therefore
// cannot reach it. Apollo4 has one general-purpose ADC; the photodiode array
// and the supply divider both need it, so a single owner holds it and hands out
// conversions (see adc_shared.h). A caller that calls am_hal_adc_initialize()
// itself gets "already in use" and no reading.
//
// NONE OF THESE DECIDE ANYTHING. They report a voltage, or report that a
// voltage crossed a boundary the CALLER chose. Whether that means "checkpoint",
// "keep waiting" or "resume" is entirely the caller's policy. This module has
// no thresholds of its own and does not want any.
//
// They are implemented in power_policy.cc rather than workload.c, because
// converting a raw ADC code to millivolts uses the bench calibration in
// bisen_config.h, which is C++.

// One supply reading, in millivolts. Returns 0 and writes *out_mv on success,
// -1 if this build has no supply source wired (in which case *out_mv is
// untouched and the caller must use its own means).
//
// Costs about 214 us: two averaged conversions and two ADC mode changes. It
// does NOT include a divider settle -- that is paid once at boot -- which is
// what makes this affordable to call inside a running inference.
int wl_supply_mv(uint32_t *out_mv);

// Ask the ADC's window comparator to watch the supply, so that waiting costs a
// register read instead of a conversion.
//
// low_mv / high_mv are YOUR band, in millivolts, and the rail should be inside
// it when you arm. The hardware then flags when the value leaves that band in
// EITHER direction -- so one armed comparator covers both "it recovered" and
// "it is still falling", and a single wl_supply_mv() afterwards tells you which.
//
// Returns 1 if armed, 0 if refused -- no supply source in this build, a band
// that is not a band, or hardware that would not take it. ⚠ A CALLER THAT GETS
// 0 MUST USE ITS OWN WAIT. Treating a refusal as "armed" produces a wait that
// never ends.
int wl_wait_arm(uint32_t low_mv, uint32_t high_mv);

// Has the supply left the armed band? 1 = yes, 0 = not yet (also 0 if nothing
// is armed, so this can never strand a loop that forgot to check wl_wait_arm).
//
// ⚠ POLLED, NOT AN INTERRUPT. This does not wake anything. You sleep on your
// own timer, wake, call this, and go back to sleep. What the hardware does for
// you is the CONVERTING and the COMPARING, unattended, while you are asleep --
// it does not summon you. The ADC interrupt vector is not available to this
// module, so there is no version of this that calls you back.
int wl_wait_changed(void);

// Stop watching and give the ADC back to the camera. Safe to call unarmed, and
// safe to call twice. Call it before resuming work.
void wl_wait_disarm(void);

#ifdef __cplusplus
}
#endif

#endif  // WORKLOAD_H
