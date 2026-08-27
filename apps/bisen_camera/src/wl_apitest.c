//
// wl_apitest.c -- an on-device conformance test for the workload API.
//
// WHAT THIS IS FOR
// Every claim workload.h makes is checked here against the running build, on
// real hardware, with the real camera and the real engine. A caller integrating
// against this API should be able to run it once and know the contract holds,
// rather than discovering at 3 a.m. that STOPPED does not persist or that a
// restored position was quietly ignored.
//
// It is deliberately NOT a unit test of the engine -- the host suites do that.
// It tests the PROMISES: that a stop is a pause, that COMPLETE outranks a stop,
// that regions describe what they claim, that the budgeting numbers are
// self-consistent, and that the supply doors fail honestly when there is no
// supply wired.
//
// Cost: one full frame, about 4.3 seconds, once at boot.
//
// It leaves the workload reset, so the application's own loop starts from a
// clean state whether the test passed or failed.
//
#include "wl_apitest.h"
#include "workload.h"
#include "am_util.h"

static uint32_t s_pass, s_fail;

static void check(int ok, const char *what) {
    if (ok) { s_pass++; }
    else    { s_fail++; am_util_stdio_printf("  APITEST FAIL: %s\n", what); }
}

// A region must point somewhere and have a length. Zero-length regions are a
// bug rather than a harmless no-op: a storage layer that reserves by
// payload_bytes and copies by region bytes would silently save nothing.
static int regions_sane(const wl_state_t *s) {
    if (s->region_count > WL_MAX_REGIONS) return 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < s->region_count; i++) {
        if (s->region[i].addr == 0) return 0;
        if (s->region[i].bytes == 0) return 0;
        sum += s->region[i].bytes;
    }
    // payload_bytes rounds each region up to the NVM block, so it is never less
    // than the true sum and never absurdly more.
    if (s->payload_bytes < sum) return 0;
    if (s->payload_bytes > sum + s->region_count * WL_ALIGN) return 0;
    return 1;
}

int wl_apitest_run(void) {
    wl_state_t s;
    s_pass = 0; s_fail = 0;
    am_util_stdio_printf("APITEST: starting (one frame, ~4.3 s)\n");

    // ---- 1. a reset workload is idle and has nothing to say -----------------
    wl_reset();
    check(wl_current_phase() == WL_PHASE_IDLE, "reset -> phase IDLE");
    check(wl_dirty() == 0,                     "reset -> not dirty");
    check(wl_result() < 0,                     "reset -> no result");
    check(wl_stop_requested() == 0,            "reset -> no stop pending");

    // Idle must still answer the budgeting questions usefully, or a caller
    // sizing its first call computes a budget of zero.
    check(wl_units_remaining() > 0,            "idle -> units_remaining > 0");

    // ---- 2. stepping starts a scan -----------------------------------------
    wl_step_result_t r = wl_step(8);
    check(r == WL_STEP_PROGRESS,               "first step -> PROGRESS");
    check(wl_current_phase() == WL_PHASE_SCAN, "first step -> phase SCAN");

    // ---- 3. remaining decreases, and never increases ------------------------
    uint32_t prev_remaining = wl_units_remaining();
    for (int i = 0; i < 4; i++) {
        (void)wl_step(8);
        const uint32_t now = wl_units_remaining();
        check(now <= prev_remaining,           "units_remaining is monotonic");
        prev_remaining = now;
    }

    // ---- 4. a stop is honoured, and it is a PAUSE ---------------------------
    const uint32_t at_stop = wl_units_remaining();
    wl_request_stop();
    check(wl_stop_requested() == 1,            "request_stop -> pending");
    r = wl_step(8);
    check(r == WL_STEP_STOPPED,                "step after stop -> STOPPED");
    check(wl_units_remaining() == at_stop,     "a stop consumes no units");

    // STOPPED must persist. A caller that saves, sleeps, and wakes must find the
    // same answer rather than the work having crept forward.
    r = wl_step(8);
    check(r == WL_STEP_STOPPED,                "STOPPED persists until resume");
    check(wl_units_remaining() == at_stop,     "still no units consumed");

    // ---- 5. what a stop reports is coherent --------------------------------
    wl_state(&s);
    check(s.phase == WL_PHASE_SCAN,            "stopped mid-scan -> phase SCAN");
    check(s.dirty == 1,                        "stopped mid-scan -> dirty");
    check(regions_sane(&s),                    "stopped mid-scan -> regions sane");
    check(wl_payload_bytes() == s.payload_bytes, "payload_bytes agrees with wl_state");
    check(wl_dirty() == (int)s.dirty,          "wl_dirty agrees with wl_state");

    // ---- 6. restore_plan describes the same place --------------------------
    // The regions are documented as a pure function of (phase, position), so
    // planning a restore of where we already are must reproduce what wl_state
    // just reported. If this diverges, a real restore would read bytes back
    // into the wrong buffers.
    wl_state_t plan;
    const int planned = wl_restore_plan(s.phase, s.position, &plan);
    check(planned == 0,                        "restore_plan accepts the current position");
    if (planned == 0) {
        check(plan.region_count == s.region_count, "restore_plan: same region count");
        int same = 1;
        for (uint32_t i = 0; i < s.region_count; i++) {
            if (plan.region[i].addr  != s.region[i].addr ||
                plan.region[i].bytes != s.region[i].bytes) same = 0;
        }
        check(same,                            "restore_plan: same addresses and lengths");
    }

    // A position this build cannot resume from must be refused, not guessed at.
    check(wl_restore_plan(WL_PHASE_SCAN, 0xFFFFFFu, &plan) == -1,
                                               "restore_plan rejects a bad position");

    // ---- 7. resume continues from the same unit ----------------------------
    wl_resume();
    check(wl_stop_requested() == 0,            "resume clears the request");
    r = wl_step(8);
    check(r == WL_STEP_PROGRESS,               "resume -> PROGRESS again");
    check(wl_units_remaining() < at_stop,      "resume actually made progress");

    // ---- 8. the supply doors fail honestly ---------------------------------
    // With no divider wired these must say so rather than inventing a number.
    // With one wired they must produce a plausible reading. Either is a pass;
    // what would be a failure is claiming success while writing nothing.
    uint32_t mv = 0xDEADu;
    const int sup = wl_supply_mv(&mv);
    if (sup == 0) {
        check(mv != 0xDEADu,                   "supply_mv wrote a value on success");
        am_util_stdio_printf("  APITEST: supply reads %u mV\n", (unsigned)mv);
    } else {
        check(mv == 0xDEADu,                   "supply_mv left the output alone on failure");
        am_util_stdio_printf("  APITEST: no supply source in this build (expected under SIM)\n");
    }

    // An unarmed watcher must never claim the rail moved -- that would send a
    // caller straight past its wait.
    check(wl_wait_changed() == 0,              "wait_changed is 0 when unarmed");

    const int armed = wl_wait_arm(5700u, 6100u);
    check(armed == 0 || armed == 1,            "wait_arm returns a boolean");
    if (armed) {
        am_util_stdio_printf("  APITEST: window comparator armed\n");
        wl_wait_disarm();
        check(wl_wait_changed() == 0,          "wait_changed is 0 after disarm");
    } else {
        am_util_stdio_printf("  APITEST: comparator refused (expected without a divider)\n");
    }
    // Disarming twice, and while unarmed, must be harmless.
    wl_wait_disarm();
    wl_wait_disarm();

    // A band that is not a band must be refused.
    check(wl_wait_arm(6100u, 5700u) == 0,      "wait_arm rejects an inverted band");
    wl_wait_disarm();

    // ---- 9. run the frame out ----------------------------------------------
    // Large steps so the scan's 3.6 s does not become 3.6 s of call overhead.
    uint32_t guard = 0;
    int saw_infer = 0;
    for (;;) {
        r = wl_step(256);
        if (wl_current_phase() == WL_PHASE_INFER) saw_infer = 1;
        if (r == WL_STEP_COMPLETE || r == WL_STEP_ERROR) break;
        if (++guard > 20000u) break;
    }
    check(r == WL_STEP_COMPLETE,               "the frame completes");
    check(saw_infer,                           "the frame passed through inference");
    check(guard <= 20000u,                     "completed without hitting the guard");

    // ---- 10. completion is not a persistence event -------------------------
    const int digit = wl_result();
    check(digit >= 0 && digit <= 9,            "result is a digit");
    wl_state(&s);
    check(s.dirty == 0,                        "a completed frame is not dirty");
    check(wl_units_remaining() == 0,           "a completed frame has no units left");

    // COMPLETE must outrank a pending stop, or a caller would persist state for
    // work that no longer exists.
    wl_request_stop();
    check(wl_step(8) == WL_STEP_COMPLETE,      "COMPLETE outranks a pending stop");
    wl_resume();

    // COMPLETE repeats until reset, so the result can be read at leisure.
    check(wl_step(8) == WL_STEP_COMPLETE,      "COMPLETE repeats until reset");
    check(wl_result() == digit,                "the result is stable");

    // ---- 11. cost figures are self-consistent ------------------------------
    const uint32_t infer_us = wl_phase_last_us(WL_PHASE_INFER);
    const uint32_t scan_us  = wl_phase_last_us(WL_PHASE_SCAN);
    check(infer_us > 0,                        "inference reported a duration");
    check(scan_us  > 0,                        "the scan reported a duration");
    check(wl_phase_last_us(WL_PHASE_IDLE) == 0, "IDLE has no duration");
    check(wl_phase_name(WL_PHASE_INFER) != 0,  "phase_name returns something");

    // ---- 12. back to a clean slate -----------------------------------------
    wl_reset();
    check(wl_current_phase() == WL_PHASE_IDLE, "reset after COMPLETE -> IDLE");
    check(wl_result() < 0,                     "reset clears the result");
    check(wl_dirty() == 0,                     "reset clears dirty");

    am_util_stdio_printf("APITEST: %u passed, %u failed  (scan %u us, infer %u us,"
                         " digit %d)\n",
                         (unsigned)s_pass, (unsigned)s_fail,
                         (unsigned)scan_us, (unsigned)infer_us, digit);
    return (int)s_fail;
}
