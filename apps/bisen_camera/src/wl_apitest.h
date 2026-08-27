#ifndef WL_APITEST_H
#define WL_APITEST_H

// On-device conformance test for the workload API. Checks the promises
// workload.h makes -- stop is a pause, COMPLETE outranks a stop, regions
// describe what they claim, budgeting numbers are self-consistent, the supply
// doors fail honestly -- against the running build.
//
// Costs one full frame (~4.3 s), once. Leaves the workload reset either way, so
// the application's own loop starts clean whether it passed or failed.
//
// Returns the number of failures; 0 means the contract holds.

#ifdef __cplusplus
extern "C" {
#endif

int wl_apitest_run(void);

#ifdef __cplusplus
}
#endif

#endif  // WL_APITEST_H
