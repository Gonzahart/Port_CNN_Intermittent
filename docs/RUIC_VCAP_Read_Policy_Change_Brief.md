# VCAP read-policy change: confirmation + reduced polling (implementation brief)

Author: Claude (Cowork), 2026-10-07. For implementation in Claude Code on the user's Mac.
Status: approved by the user for implementation; NOT yet implemented, built or hardware-validated.
Follow AGENTS.md: read PROJECT_STATE.md, TASKS.md and CODEX_LOG.md first; log to CODEX_LOG.md
(newest entry at top); do not commit or push; never claim hardware validation from builds or host tests.

## 1. Why (bench evidence, 2026-10-07, see CODEX_LOG)

Threshold verification with the GPIO17 calibration (anchors 476/655, 100 nF pin cap),
original app via `flash_harvest_rf_replay.sh`, 3 automated FG staircase cycles:

| cycle | stop (nominal work100 6.20 V) | resume (nominal work500 6.80 V) |
|---|---|---|
| c1 | 6.38 V | 6.645 V |
| c2 | 6.335 V | 6.73 V |
| c3 | 6.29 V | 6.775 V |

* Every crossing gives exactly one event (no chatter), but both edges move inward by
  0.1–0.18 V. Cause: each decision uses ONE policy reading (median-of-3 AVG16), whose noise
  is ~4.5 codes SD with heavy tails (+13…+17-code outliers in ~0.5–1 % of readings,
  1 code = 11.7 mV of VCAP), and the firmware takes very many readings.
* While working, `run_bisen_job()` calls `pp_sample()` every scheduler step and the scan
  quantum is one pixel (`BISEN_CAMERA_SCAN_MAX_UNITS=1`): ~1,017 VCAP reads per frame,
  4.0 ms each; VCAP reading = ~52 % of working time (frame period 7.84 s).
* While waiting, `wait_for_energy()` takes one reading every 250 ms.

## 2. Goals

1. **Confirmation:** a stop (work → wait) or a resume (wait → work) happens only after
   N consecutive valid readings past the edge. One outlier must not trigger either.
2. **Reduced polling while working:** sample VCAP every K scheduler steps instead of every
   step, without weakening safety.
3. Default build behaviour unchanged (N = 1, K = 1); new behaviour enabled by build flags
   in a separate candidate image, so a before/after comparison is clean.

## 3. Design requirements

### 3.1 Confirmation (power_policy.cc / bisen_policy, whichever owns the decision)
* New build flags (module.mk + pp_defines, defaults keep today's behaviour):
  `BISEN_HARVEST_STOP_CONFIRM` (default 1), `BISEN_HARVEST_RESUME_CONFIRM` (default 1).
* Keep the raw per-reading decision as today; expose *confirmed* values through the
  existing `pp_compute_allowed()` / `pp_restore_allowed()` (or equivalents) so callers in
  `run_bisen_job()`, `wait_for_energy()`, infer.c and scan.c need no logic changes.
* compute_allowed true→false only after STOP_CONFIRM consecutive raw "not allowed" readings;
  restore/compute false→true (the resume condition used by `wait_for_energy(true)`) only after
  RESUME_CONFIRM consecutive raw "allowed" readings.
* An invalid reading (`s_valid=false`, high-sample retry exhaustion) neither confirms nor
  counts; decide and document whether it resets the counter (recommended: reset).
* **Safety bypass:** a reading below the critical level (`ES_LEVEL_CRITICAL` /
  `below_sleep_floor`, 5.8 V) must act immediately, with no confirmation.
* While a stop is pending (1 ≤ votes < N), keep working but limit the chunk to the
  smallest (100-unit) budget.
* Reset counters at session start, after a stop, and after a resume
  (`pp_high_sample_filter_reset()` is a natural hook; check).
* Chunk band selection (100/500/1000) may keep using raw readings; state the choice.

### 3.2 Reduced polling while working (bisen_camera_harvest.cc run loop)
* New flag `BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS` (default 1).
* Call `pp_sample()` when (step counter % K == 0), and ALWAYS: on the first step of a job,
  right after a resume/wait, on a phase change (scan → CNN and back), immediately before any
  checkpoint decision, and on every step while a stop confirmation is pending.
* Audit every other `pp_sample()` caller (scan.c ~304/347/354, infer.c, the wait loop) and
  make the behaviour consistent; do not change the wait-loop period (250 ms).
* Safety budget, to document in code: energy between samples ≈ K × (one scheduler step).
  With ~3.6 ms pixel steps at ~10 mW, K = 32 ≈ 1.2 mJ ≈ 18 mV of VCAP on 10 mF at 6.3 V, small
  against the 6.2 → 5.8 V band. Larger CNN chunks already sample per chunk; keep that.
* The pixel ADC path (SE4) is unaffected.

### 3.3 Instrumentation
* Count, per session, readings rejected by confirmation (stop and resume separately) and
  VCAP samples per frame; print them in the existing SWO summaries, never inside timed regions.
* State DAC codes unchanged.

### 3.4 Scope
* Implement in `apps/bisen_camera_harvest` first (this is what `flash_harvest_rf_replay.sh`
  builds and what the baseline used). Then port identically to `apps/bisen_camera_harvest_IS`
  and `apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS`; check whether their
  power_policy / run-loop files differ before porting.
* No change to ADC configuration (AVG16, LPMODE1), calibration, thresholds, checkpoint
  format or MRAM write sequence. AVG128/LPMODE0 is a separate later change.

## 4. Build helpers
* Add `build_harvest_rf_replay_vcap_policy.sh` and `flash_harvest_rf_replay_vcap_policy.sh`
  (copies of the current helpers) with
  `BISEN_HARVEST_STOP_CONFIRM=3 BISEN_HARVEST_RESUME_CONFIRM=3 BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS=32`,
  a distinct BASE/output name, and a distinct checkpoint magic (e.g. `0x48565234`) so a stale
  record from the baseline image is never adopted. Keep GPIO17 pin flags, anchors 476/655
  and thresholds 5.8/6.2/6.8/7.3 V unchanged.

## 5. Verification (before handing back)
* Build with the ARM toolchain: (a) the default build (flags at defaults), (b) the candidate
  helper. Both must compile with no new warnings.
* If the repo has a host test harness under `apps/bisen_camera_harvest/tests`, add host
  tests for the confirmation logic: single outlier ignored; N consecutive readings switch;
  invalid reading handling; critical bypass is immediate; counters reset on stop/resume.
  Otherwise write a small host test for the extracted pure function.
* Log in CODEX_LOG.md: files changed, flags, defaults, build results, test results,
  explicitly "no hardware validation". Add a TASKS.md item for the hardware rerun.

## 6. Hardware validation the user will run afterwards (same as the baseline)
Flash the candidate helper, then repeat `thr arm`, then 3 × (`thr down cN_down`, capture;
`thr up cN_up`, capture) with the same scope command. Expected if the change works:
stop within ~6.20–6.25 V, resume ~6.75–6.80 V, still one event per crossing,
VCAP-ADC (state 1) share of working time ≈ 52 % → a few %, frame period ≈ 7.8 s → ≈ 4 s.
