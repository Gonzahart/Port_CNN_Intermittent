# RUIC Tasks

This file contains authorized shared tasks for ChatGPT and Codex. Codex should not treat the research backlog as implementation authorization unless a task is moved into **Active** or the user explicitly requests it.

## Active — bridge/reconciliation

- [x] **Reconcile the live repository with `PROJECT_STATE.md`** (2026-09-28; source inspection and two existing host tests)
  - Owner: Codex
  - Goal: determine where the repository actually is relative to the August handoff and September paper draft without changing firmware behavior.
  - Inspect/report:
    - current branch and HEAD commit;
    - `git status` including untracked project files;
    - remotes;
    - relevant Apollo4/RUIC application directories;
    - current build target and commands;
    - active camera/CNN implementation location;
    - active VCAP ADC path/configuration;
    - current scheduler thresholds/chunk sizes;
    - current checkpoint record/slot layout and linker reservation;
    - current continuous/session behavior;
    - current state-DAC mapping;
    - any README/handoff files that are newer than the states summarized here.
  - Acceptance criteria:
    - no functional source changes;
    - append findings to `CODEX_LOG.md`;
    - update only repository-verifiable sections of `PROJECT_STATE.md`;
    - leave bench-only conflicts marked `UNCONFIRMED`.

- [x] **Install the four coordination files at repository root** (2026-09-28; documentation review pending)
  - Owner: Codex
  - Files: `AGENTS.md`, `PROJECT_STATE.md`, `TASKS.md`, `CODEX_LOG.md`.
  - Acceptance criteria:
    - files are at the root of the live RUIC/neuralSPOT working repository used by Codex;
    - existing `AGENTS.md` content is preserved/merged rather than blindly overwritten;
    - any conflict with existing agent instructions is documented;
    - commit only after reviewing `git diff` and confirming no unrelated files are staged.

- [ ] **Create a clean bridge baseline commit**
  - Owner: Codex
  - Suggested commit message: `docs: add ChatGPT-Codex RUIC coordination state`
  - Acceptance criteria:
    - coordination files and any deliberate merge of an existing `AGENTS.md` are included;
    - no build artifacts, experiment CSVs, or unrelated firmware changes are included;
    - report the resulting hash after commit. Record it in a subsequent log update; do not amend a commit repeatedly to try to embed its own hash.
  - Status: awaiting user review of the documentation diff; no commit or staging performed yet.

## Current hardware confirmation

- [x] **Record current board and power components** (user confirmation, 2026-09-28): AMAP4PEVB Apollo4 Plus BGA Rev. 1.0; 10 mF / 10,000 uF reservoir; MP1584EN; approximately 1.9 V rail; 390 kΩ / 10 kΩ fixed divider.
- [x] **Separate available parts from installed hardware:** TPS7A0220PDBVR fixed 2.0 V / 200 mA LDO is planned soon, not installed. TS5A3167DBVR is an optional future experiment, not installed or adopted.
- [ ] **Record remaining bench details per experiment:** installed divider-filter capacitor, camera hardware, EVB/J-Link/USB/extra-supply isolation and relevant jumper settings. Do not infer these from historical Blue Plus documentation.

## Shared V1–V13 validation program — not automatically authorized for implementation

All V items remain open. Existing incremental CNN scheduling, selective checkpointing, session persistence and 100/500/1000 work budgets are implemented; validate those paths rather than treating them as missing features. Compilation and host tests cannot close hardware or end-to-end tasks. Prior functional runs must be archived with their exact configuration before they support paper claims.

### Controlled variables and evidence for all V items

Preserve the applicable board/revision, power path, reservoir, regulated rail, divider/ADC calibration, thresholds/budgets, model/weights/quantization, inputs and acquisition/preprocessing, MCU clock, compiler/build flags, trace/scaling/timing, initial capacitor energy, and probe/shunt/measurement configuration. Record commit, binary identity, commands, raw artifacts, repetitions, and any deviations. For ablations, vary only the intended feature. Disclose unavoidable differences and their effect on claims. V12 intentionally changes the regulator and requires a new validation configuration; V13 is optional and changes divider switching if adopted.

- [ ] **V1 — Float/int8 MNIST accuracy and model/export lineage.** Identify the source model, dataset/split, preprocessing, quantization and export path to deployed weights; report held-out float/int8 accuracy and prediction disagreement. Deployed weights alone do not establish lineage or accuracy.
- [ ] **V2 — Uninterrupted vs interrupted/restored inference equivalence.** Validate existing incremental execution and restore across representative scan/CNN positions and work budgets. Compare final outputs and preserved progress against uninterrupted runs on identical inputs; freeze acquisition or account separately for scene changes during interrupted scanning.
- [ ] **V3 — Live ADC/scheduler threshold validation.** Compare GPIO16-derived VCAP with physical measurements while sweeping work bands, wait and resume boundaries. Verify 100/500/1000 budgets, hysteresis, calibration error/noise and observed decisions; characterize safe work/checkpoint/resume reserves on the installed power path.
- [ ] **V4 — Production checkpoint → true cold restore.** Archive prior evidence and validate existing production MRAM/session paths with true rail collapse, representative scan/CNN checkpoints and interrupted writes. Verify saved/restored position, session arm/disarm persistence and correct completion; review header integrity and fallback behavior. Retained-RAM tests and host journal tests are insufficient.
- [ ] **V5 — Compulsory-Stop ablation.** Establish a matched variant with compulsory Stop transitions and compare against the current direct-transition scheduler; preserve model, useful work, checkpoint policy and power/trace controls.
- [ ] **V6 — Same-platform SunSift-style layer-granular baseline.** Establish and validate a common-model Apollo4 baseline with documented layer-boundary scheduling/checkpoint semantics, then compare under the shared controls. Label it SunSift-style unless fidelity to the original implementation is demonstrated.
- [ ] **V7 — Same-platform monolithic TFLM baseline.** Establish and validate the same model/preprocessing on Apollo4 TFLM, document quantization/kernel differences, and compare uninterrupted and intermittent behavior. SDK support alone does not complete the baseline.
- [ ] **V8 — Capuchin feasibility/common-model comparison.** Document feasibility of the common model/platform and deployment assumptions; perform direct energy/runtime comparisons only if defensible, otherwise report the limitation explicitly.
- [ ] **V9 — PT1/PT2/PT3 provenance and replay calibration.** Map each named trace to source data and immutable file identity; record units, sampling, scaling, duration and FG command settings. Calibrate commanded versus actual loaded charging/VCAP behavior. Existing local charge traces are not yet confirmed PT1/PT2/PT3 equivalents.
- [ ] **V10 — Synchronized state-resolved energy measurement.** Capture synchronized VCAP, board VDD, state DAC and calibrated shunt current. Verify instrument alignment, measurement offsets and full state-marker coverage (especially restore/preprocessing). Report state energies and joules/completed frame with measurement limitations and independent completion counts.
- [ ] **V11 — End-to-end runtime under identical intermittent traces.** Repeat matched traces for the proposed system and applicable baselines; report completed frames, latency/runtime, checkpoint/restore counts and lost/recomputed progress, with initial conditions and repetitions preserved.
- [ ] **V12 — MP1584EN → TPS7A0220 power-path revalidation.** After installing the planned LDO, revalidate work/checkpoint/resume thresholds, capacitor energy reserve, discharge, dropout and relevant energy measurements. Keep MP1584EN results distinct; do not automatically transfer threshold-safety or energy conclusions to TPS7A0220.
- [ ] **V13 — Optional TS5A3167 switched-divider ablation.** Not adopted or required for the current fixed-divider baseline. If authorized later, compare divider energy plus ADC settling/accuracy and resulting scheduler behavior against the fixed divider with other variables controlled.

## Follow-up documentation / reproducibility — not automatically authorized

- [ ] Review/version the pre-existing untracked replay helpers, instrument tools, and trace files in a separate commit.
- [ ] Correct old-workspace helper links and conflicting CH1/CH4 mappings in the harvest README.
- [ ] Audit state-marker timing before using decoded interval lengths for restore/compute energy or completed-frame counts.
- [ ] Preserve/export raw validation artifacts separately from the documentation baseline.

## Completed / historical

- [x] Apollo4 stable-supply bring-up of energy-aware scheduling mechanisms.
- [x] External VCAP ADC characterization for the earlier bring-up.
- [x] Two-slot atomic MRAM checkpoint backend validated on target for the earlier Sobel implementation.
- [x] Deep-sleep / RTC wake and retained-RAM progress validated.
- [x] Three-bit state-DAC instrumentation validated.
- [x] Production-style low-energy dirty-checkpoint trigger observed in the earlier continuous Sobel bench run.
