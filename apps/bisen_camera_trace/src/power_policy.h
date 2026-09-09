#ifndef POWER_POLICY_H
#define POWER_POLICY_H

#include <stdint.h>

// power_policy.h -- the glue between this workload and the BISen energy policy.
//
// The files bisen_config.h / bisen_adc.* / bisen_energy.* / bisen_policy.cc are
// copied in VERBATIM from the bisen_port application and MUST NOT be edited.
// The direct threshold table and pre-sleep checkpoint condition come from the
// validated port. This file only:
//
//   * supplies a VCAP reading from AMAP4PEVB J9.8/GPIO16/ADCSE3
//   * hands their policy our "is there dirty work" answer, from workload.h
//   * caches the decision so the hot loops can ask about it for free
//
// WHY THE DECISION IS CACHED
// Their scheduler measures once per wake and acts in later states. Ours would
// otherwise ask inside nn_step's loop, which runs about a thousand times per
// inference -- and a real VCAP reading costs a 5 ms divider settle. So sampling
// is explicit (pp_sample) and the query is a cached compare (pp_should_*).

#ifdef __cplusplus
extern "C" {
#endif

// ⚠ SUPERSEDED by ES_SOURCE in energy_source.h, which covers the cases this
// boolean could not: an internal-rail reading, and a PMU interrupt that reports
// no voltage at all. Kept only so existing references still compile.
//
// Legacy compatibility selector. ES_SOURCE in energy_source.h is authoritative.
#ifndef PP_USE_REAL_ADC
#define PP_USE_REAL_ADC 0
#endif

// The simulated ramp: fall by PP_SIM_STEP_MV per SAMPLE, and once past the
// floor jump back to the start, as a harvester recharging would.
//
// ⚠ The step is per sample, NOT per frame, so it is coupled to how often
// pp_sample() is called. At PP_SCAN_POLL_PIXELS 64 that is 1024/64 = 16 reads
// during the scan plus 1 at the start of the inference = 17 per frame:
//
//     17 samples x 40 mV = 680 mV per frame
//     range 9000 -> 5400 = 3600 mV
//     => one full discharge/recharge cycle every ~5.3 frames
//
// which puts a checkpoint roughly every fifth frame -- often enough to watch
// the bands step down and a save actually fire, rare enough that the MRAM
// endurance budget is ~150 hours rather than ~30. If PP_SCAN_POLL_PIXELS is
// changed, re-derive this: at the earlier 150 mV the ramp completed every 1.4
// frames and wrote to MRAM almost every frame.
#ifndef PP_SIM_START_MV
#define PP_SIM_START_MV 9000u
#endif
#ifndef PP_SIM_STEP_MV
#define PP_SIM_STEP_MV 40u
#endif
#ifndef PP_SIM_FLOOR_MV
#define PP_SIM_FLOOR_MV 5400u
#endif

// Take a reading and run their policy on it. Call at phase boundaries and
// periodically inside long phases -- not in a tight loop.
void pp_sample(void);

// Per-job observability for the camera-only high-sample filter. A physical
// VCAP result at or above BISEN_CAMERA_VCAP_IGNORE_AT_MV is never fed into the
// BISen threshold table; pp_sample() retries immediately. If every bounded
// retry is still high, the sample remains unavailable and the scheduler uses
// its ordinary wait path instead of hanging in the ADC routine.
void     pp_high_sample_filter_reset(void);
uint32_t pp_high_samples_ignored(void);
uint32_t pp_high_retry_exhaustions(void);

// Cached: does their policy want a checkpoint written right now? Combines their
// low-energy condition with our workload's dirty flag. Cheap; safe to call per
// tile. Returns 0 until the first pp_sample().
int pp_should_checkpoint(void);

// Cached: units of work their policy currently permits, or 0 in the wait band.
// Their chunk sizes are 1000 / 500 / 100.
uint32_t pp_chunk_units(void);

// Cached: does the policy permit useful computation right now? FALSE is
// BISen's Th_safe condition -- leave the active state, retain SRAM, wait. It is
// NOT a reason to checkpoint: the whole point of that state is to ride out a
// dip without touching NVM, which is where the paper's 86.4% reduction in NVM
// operations comes from.
int pp_compute_allowed(void);

// Enter / leave the low-power wait. These tell the energy model what the
// machine is drawing and drive the state bus, so a trace shows the wait.
void pp_enter_wait(void);

// ---------------------------------------------------------------------------
// HARDWARE-WATCHED WAIT
//
// The wait state used to be: sleep a bit, take a full supply reading, ask the
// policy, repeat. The reading was the expensive part, and it had to happen on
// every single poll just to find out that nothing had changed yet.
//
// These arm the ADC's window comparator on the band the rail is currently in,
// so the comparison happens in hardware and a poll costs one status bit. The
// band is bounded BELOW by the level at which a checkpoint becomes urgent and
// ABOVE by the level at which computing may resume, so leaving it in either
// direction is the event worth waking for -- the rail recovered, or it is
// still falling and something must be saved.
//
// pp_wait_arm() returns 1 if the hardware took it, 0 if it refused. A caller
// that gets 0 MUST keep using the old sample-every-poll loop; nothing here is
// allowed to turn a refusal into a wait that never wakes.
int  pp_wait_arm(uint32_t low_mv, uint32_t high_mv);

// His two band edges, for the self-driving loops -- which are C and cannot read
// a C++ constexpr table. Lower edge: below it a checkpoint is urgent. Upper
// edge: the RESUME level. There is deliberately no band hysteresis.
uint32_t bisen_checkpoint_trigger_mv(void);
uint32_t bisen_resume_mv(void);

// Has the rail left the armed band? Cheap enough to call in a tight loop.
// Says nothing about WHICH way -- take one pp_sample() afterwards and let the
// policy read the result.
int  pp_wait_left_band(void);

// Stop watching and hand the ADC back to the camera. Safe to call unarmed.
void pp_wait_disarm(void);

// Whether the ADC is genuinely free-running while armed, or whether each poll
// has to ask for the next conversion. Reported rather than assumed, so a serial
// trace shows which mechanism actually ran.
int  pp_wait_self_repeats(void);
void pp_leave_wait(void);

// Cached: is a restore permitted at this energy level? Their resume threshold
// sits ABOVE the lowest compute band on purpose -- restoring state you cannot
// then act on is wasted energy.
int pp_restore_allowed(void);

// Latch a failed save. Their rule stops retrying once a write has failed, so a
// dying capacitor is not spent on an attempt that cannot complete.
void pp_note_checkpoint_failed(void);

// Last reading, and the band name, for logging.
uint32_t pp_vcap_mv(void);
uint32_t pp_adc_code(void);
const char *pp_band_name(void);

// One line per frame: voltage, band, chunk, and what the policy decided.
void pp_report(void);

// State-bus self-check.
//
// The three GPIOs cannot be verified from software by looking at a scope, but
// the Apollo4 output data register CAN be read back, which proves the HAL
// actually drove the pads rather than the call merely returning. This counts
// every code set, re-reads the three pins immediately, and counts any that did
// not land. It also counts how many times each code was driven, so the totals
// can be cross-checked against what the phases should have produced.
//
// It does NOT prove the pins reach the header, or their level -- that is what
// the scope is for. It proves everything up to the pad.
uint32_t pp_bus_marks(void);
uint32_t pp_bus_mismatches(void);
uint32_t pp_bus_last_code(void);
// How many times each code was driven since the last pp_bus_reset().
uint32_t pp_bus_code_count(uint32_t code);
void     pp_bus_reset(void);

// Three-bit GPIO state bus, for an oscilloscope or logic analyser.
//
// bisen_instrumentation.cc is copied in verbatim and keeps the MSP430 BISen
// encoding: 0=sleep 1=VCAP ADC 2=sense/camera 3=compute 4=MRAM write
// 5=checkpoint committed 6=context restore 7=boot/error, on GPIO 62/63/61
// (AMAP4PEVB J12 pins 7/9/11). Those pins are free in this application.
//
// Their scheduler drives it from its own State enum; this one has no such
// state machine, so our phases are mapped onto the same codes through
// instrumentation_set_code(). A trace captured here decodes with the same
// key as one captured on the bisen_port build.
//
// Codes 5-7 are event markers -- they insert no delay and are replaced by
// whatever runs next. The three pins pass briefly through zero when the bank
// changes, so decode stable intervals rather than treating that sub-instruction
// transition as a sleep event.
void pp_instr_init(void);
void pp_mark_boot(void);
void pp_mark_sleep(void);
void pp_mark_adc(void);
void pp_mark_camera(void);
void pp_mark_compute(void);
void pp_mark_nvm_write(void);
void pp_mark_committed(void);
void pp_mark_restore(void);

#ifdef __cplusplus
}
#endif

#endif  // POWER_POLICY_H
