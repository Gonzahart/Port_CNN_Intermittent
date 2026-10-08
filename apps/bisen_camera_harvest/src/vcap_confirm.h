#ifndef VCAP_CONFIRM_H
#define VCAP_CONFIRM_H

#include <stdint.h>

// vcap_confirm.h -- consecutive-reading confirmation of the VCAP policy edges.
//
// Pure logic, no HAL: power_policy.cc feeds it the raw per-reading decision
// from bisen_policy.cc (unchanged) and reads the confirmed flags back through
// pp_compute_allowed() / pp_restore_allowed(). Host tests compile this file
// directly (tests/vcap_confirm_test.c).
//
// WHY
// One policy reading (median-of-3 AVG16) has ~4.5 codes SD with heavy tails
// (+13...+17-code outliers in ~0.5-1 % of readings; 1 code = 11.7 mV VCAP).
// With ~1,000 readings per frame, a single-reading decision fires on an
// outlier, which moved the bench stop/resume edges inward by 0.1-0.18 V
// (CODEX_LOG 2026-10-07).
//
// RULES (stop_n = BISEN_HARVEST_STOP_CONFIRM, resume_n = ..._RESUME_CONFIRM)
//   compute  true -> false  after stop_n consecutive raw "not allowed".
//   compute  false -> true  after resume_n consecutive raw "allowed".
//   restore  false -> true  after resume_n consecutive raw "allowed".
//   restore  true -> false  immediately. Restore only gates STARTING work
//                           (the post-wait resume edge), so dropping it early
//                           can only delay a resume, never extend work.
//   critical (below_sleep_floor, 5.8 V) forces compute and restore false
//                           immediately, with no confirmation.
//   invalid reading         is not a vote either way and clears every vote
//                           run; confirmed flags are unchanged. The caller
//                           still reports "not allowed" while the latest
//                           reading is invalid (existing fail-safe).
//   unprimed (boot, or after vcap_confirm_forget()): the first valid reading
//                           sets compute directly, exactly as the
//                           single-reading policy did; restore starts false
//                           and needs resume_n readings like any resume.
//   A confirmed stop clears every vote run; a confirmed rise clears its own
//   run and any outstanding stop run (compute and restore rise on the same
//   readings, so one rising does not restart the other's count).
//
// With stop_n = resume_n = 1 the confirmed flags equal the raw flags after
// every valid reading, i.e. the original single-reading behaviour.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t stop_n;
    uint32_t resume_n;
    uint8_t  primed;
    uint8_t  compute;        // confirmed compute_allowed
    uint8_t  restore;        // confirmed restore_allowed
    uint32_t stop_votes;     // consecutive raw compute=false while compute=1
    uint32_t compute_votes;  // consecutive raw compute=true  while compute=0
    uint32_t restore_votes;  // consecutive raw restore=true  while restore=0
} vcap_confirm_t;

// Events reported by vcap_confirm_update(), for instrumentation only.
enum {
    VC_EV_PRIMED      = 1u << 0,  // first valid reading after init/forget
    VC_EV_STOP_VOTE   = 1u << 1,  // raw stop reading not (yet) confirmed
    VC_EV_STOP        = 1u << 2,  // compute confirmed true -> false
    VC_EV_RESUME_VOTE = 1u << 3,  // raw compute&&restore, confirmed pair not
    VC_EV_RESUME      = 1u << 4,  // confirmed compute&&restore false -> true
    VC_EV_CRITICAL    = 1u << 5,  // critical reading forced compute 1 -> 0
};

// Unprimed state with the given confirmation counts (values < 1 act as 1).
void vcap_confirm_init(vcap_confirm_t *c, uint32_t stop_n, uint32_t resume_n);

// Return to the unprimed state (session start). Keeps stop_n / resume_n.
void vcap_confirm_forget(vcap_confirm_t *c);

// One valid reading. Returns a VC_EV_* bitmask.
uint32_t vcap_confirm_update(vcap_confirm_t *c, int raw_compute,
                             int raw_restore, int raw_critical);

// One invalid reading: clears the vote runs, leaves the confirmed flags.
void vcap_confirm_invalid(vcap_confirm_t *c);

// Working, but at least one consecutive stop reading is outstanding.
int vcap_confirm_stop_pending(const vcap_confirm_t *c);

#ifdef __cplusplus
}
#endif

#endif  // VCAP_CONFIRM_H
