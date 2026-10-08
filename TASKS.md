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

- [x] **Install the four coordination files at repository root** (2026-09-28; approved and committed)
  - Owner: Codex
  - Files: `AGENTS.md`, `PROJECT_STATE.md`, `TASKS.md`, `CODEX_LOG.md`.
  - Acceptance criteria:
    - files are at the root of the live RUIC/neuralSPOT working repository used by Codex;
    - existing `AGENTS.md` content is preserved/merged rather than blindly overwritten;
    - any conflict with existing agent instructions is documented;
    - commit only after reviewing `git diff` and confirming no unrelated files are staged.

- [x] **Create a clean bridge baseline commit**
  - Owner: Codex
  - Suggested commit message: `docs: add ChatGPT-Codex RUIC coordination state`
  - Acceptance criteria:
    - coordination files and any deliberate merge of an existing `AGENTS.md` are included;
    - no build artifacts, experiment CSVs, or unrelated firmware changes are included;
    - report the resulting hash after commit. Record it in a subsequent log update; do not amend a commit repeatedly to try to embed its own hash.
  - Status: approved and committed as `dc4a95f7ca888b747fe6d2304ac938ee8f157023`; only the four coordination files were included.

## Release automation

- [x] Make inherited release automation manual-only for the research mirror: remove push and PR triggers from `.github/workflows/release.yaml`, preserving manual dispatch and existing jobs/configuration. Included in current HEAD `190c4f503`; configuration checked. No hosted workflow execution is claimed.

## Current hardware confirmation

- [x] **Record current board and power components** (user confirmation, 2026-09-28): AMAP4PEVB Apollo4 Plus BGA Rev. 1.0; 10 mF / 10,000 uF reservoir; MP1584EN; approximately 1.9 V rail; 390 kΩ / 10 kΩ fixed divider.
- [x] **Separate available parts from installed hardware:** TPS7A0220PDBVR fixed 2.0 V / 200 mA LDO is planned soon, not installed. TS5A3167DBVR is an optional future experiment, not installed or adopted.
- [ ] **Record remaining bench details per experiment:** installed divider-filter capacitor, camera hardware, EVB/J-Link/USB/extra-supply isolation and relevant jumper settings. Do not infer these from historical Blue Plus documentation.

## Validation implementation assessment (2026-09-29)

- [x] Inspect the current V1–V13 backlog against firmware, build profiles and bench tooling; record [implementation plan](docs/RUIC_VALIDATION_IMPLEMENTATION_PLAN.md). This completes planning only.
- [ ] Qualify the paired r2a OS/IS apps before comparative claims. Missing IS headers, app-local linker references, distinct OS01/IS01 identities, ARM builds and basic host checkpoint/policy tests are resolved (2026-09-29). Explicit flash paths are documented because `make deploy` does not resolve either package's renamed `EXAMPLE`. Exact-score equivalence, true cold restore, state markers and per-variant work/checkpoint energy remain unvalidated on hardware; preserve the original app as a reference. See `docs/RUIC_R2A_PAIR_RUN.md`.
- [ ] Before V12 assembly, resolve TPS7A0220's 6.0 V recommended-input limit versus the present 7.5–8 V reservoir. Select the voltage range/power path, then change replay ceilings, calibration and safety thresholds together.
- [x] Reconcile the downloaded September 29 planning files against the live state; expand to V0–V14, defer V5/V6 and preserve completed bridge work. The earlier V1–V13-only implementation order is superseded.
- Proposed next package: review existing V0 evidence and freeze model/input/build identity; then add production-path MRAM energy instrumentation and focused harness (V14), completion accounting/replay analysis (V9–V11), and missing TFLM variants (V7). Capuchin remains feasibility-first. Firmware implementation is still proposed, not performed or activated by this document merge.

## State-DAC completion notification (2026-09-29)

- [x] Restore successful-write code 5 in original harvest, IS and OS policy glue, matching the MSP430 completion-notification meaning. The preceding code-5 suppression was superseded before deployment. Internal accounting and code numbers remain unchanged; code 5 is held until the next activity without a fixed pulse delay.
- [ ] Rebuild/flash the selected experimental image and verify code 4 → code 5 after a successful save, then the next activity. Error handling and cold restore still need hardware checks.
- [ ] Before V14 restore-energy integration, move CNN restore instrumentation around actual storage recovery; restoring code 5 does not fix this marker.
- User priority: measure production checkpoint and restore energy next. Measurement harness and power-path changes remain separate work.
- [ ] For the eventual Stop/wait-energy study, record actual sleep entry/current and wake reliability in normal mode before any deep-sleep trial; the current mode-2 path is optional and unvalidated. Keep this distinct from V14 checkpoint-energy measurement and from MSP430 LPM3.5 standby.

## Shared V0–V14 validation program — research backlog

These tasks document the agreed validation strategy. They are **not automatically authorized for Codex implementation** until the user moves them to Active or explicitly assigns them.

### Research priority after supervisor discussion (2026-09-28)

**2026-09-29 user update:** V0's continuous-power comparison has been performed; the Data_tables.pdf summary is now reviewed. Raw captures, exact build/model lineage and uncertainty remain open; energy in the report is estimated. Defer V5 compulsory-Stop and V6 SunSift-style layer-boundary studies to a later paper. Current paper priority is the CNN engine, controlled TFLM/feasible Capuchin comparisons (V7–V8, V11), and MRAM energy (V14). Do not implement or include V5–V6 in the present study.

1. Establish whether the custom CNN engine is faster or lower-energy than TFLM on the **same int8 model and Apollo4 board** under continuous supply (V0, V7). Correct outputs and accuracy are entry gates (V1, V2).
2. Establish whether runtime differences translate into **more correct completed classifications per identical replay** on one final power path (V9, V11). Report inference-only and full camera-plus-inference outcomes separately; equal generator commands may produce different loaded VCAP waveforms.
3. Quantify the full checkpoint and incremental MRAM-program energy (V14), then physically validate restart correctness, safe thresholds and checkpoint behavior (V2–V4).
4. Defer compulsory-Stop and SunSift-style studies (V5–V6) to a later paper. Include Capuchin in direct rankings only after the same-platform/model/precision feasibility gate (V8). Keep regulator and divider studies (V12–V13) identified as hardware revisions.

**Comparison contract:** freeze model file/hash, topology, weights, quantization, preprocessing, image order, clock, compiler flags, camera setting, board, initial VCAP and power path. For every fixed-duration PT1/PT2/PT3 replay, use identical function-generator commands and record actual loaded VCAP, VDD, resets, and completed correct outputs. Repeat paired runs in randomized runtime order, at least five per runtime/trace pair initially; summarize variability and uncertainty. A different CNN model is a separate generality experiment and cannot alone prove this engine faster than TFLM.

### V0 — Continuous-supply CNN-engine speed and energy (new primary test)

**Status:** Summary tables received and reviewed (Data_tables.pdf, September 29). Existing optimized TFLM/CMSIS-NN and multiple proposed-engine timings are reported. Reuse their artifacts; do not automatically recreate the baseline or repeat the entire campaign. Audit raw captures and exact source/binary/model identity before closing criteria. No measured energy evidence is supplied.

- [x] Review all seven pages, separate engine versions and board/host/derived results, and record conclusions in PROJECT_STATE.md.
- [ ] Obtain/map merged-r2 (080-r2/Task 083) and TFLM source snapshots, binary/model/input hashes and sealed captures to the harvest candidates, including the newly added nested OS package.
- [ ] Qualify the chosen current merged-r2 OS/IS builds with full-vector correctness and production checkpoint/cold-restore tests; older-engine campaigns do not close this version-specific gap.
- [ ] Measure production ADC/policy/chunk overhead at the intended 100/500/1000 budgets separately from bare-engine tile 1/8/64/whole results, with clocks and coherent-unit semantics recorded.

- [ ] Build proposed output-stationary and input-stationary engines and TFLM on the same Apollo4 using one exported int8 LeNet model and exactly the same input tensor vectors.
- [ ] Verify all ten output logits, predicted labels and accuracy before timing. Run warm repeated inferences; report setup/tensor allocation separately from steady-state invocation.
- [ ] Freeze MCU clock, compiler optimization, kernel implementation choices, memory placement, debug output, and supply voltage; record binary hash, flash and peak SRAM.
- [ ] Measure latency, cycles, board-input power/energy per inference and variation. Include a proposed-engine build without energy polling/checkpointing to expose intermittent-control cost; compare production configuration separately.
- [ ] Report speedup only as measured TFLM time divided by measured proposed-engine time on the same workload and specify the numerator/denominator. Do not infer it from host correctness or an earlier output-stationary timing session.
- **What this proves:** whether the engine itself provides a meaningful same-model Apollo4 advantage, independently of camera acquisition and interruption policy.

### V1 — Model accuracy and quantization correctness

- [ ] Evaluate the floating-point LeNet model on the complete 10,000-image MNIST test set.
- [ ] Evaluate the deployed int8 model/runtime on the identical test set.
- [ ] Report float accuracy, int8 accuracy, prediction-disagreement rate, and any preprocessing differences.
- [ ] Confirm/document the actual model export and quantization path used to generate the deployed header.
- **What this proves:** the deployed integer model is a valid implementation of the intended classifier, so later runtime/energy comparisons are not confounded by unknown accuracy loss.

### V2 — Exact interrupted/resumed inference correctness

- [ ] Select deterministic camera frames / model inputs.
- [ ] Record uninterrupted reference logits and predicted class.
- [ ] Force interruption/checkpoint at representative camera and CNN progress points.
- [ ] Reset or power-cycle without reflashing.
- [ ] Restore and complete the same job.
- [ ] Verify restored phase/layer/position, no skipped coherent unit, documented recomputation, bit-identical final logits for the same int8 runtime, and matching predicted class.
- **What this proves:** checkpoint/restart changes execution timing, not inference semantics.

### V3 — Scheduler / ADC policy validation

- [ ] Under controlled bench power, verify the live camera/CNN policy around every configured voltage boundary.
- [ ] Confirm the actual 100/500/1000-unit bands and wait/resume behavior from the live firmware after repository reconciliation.
- [ ] Sweep upward and downward near boundaries and repeat measurements.
- [ ] Record DMM VCAP, reconstructed firmware VCAP, chosen plan/chunk, wait state, checkpoint trigger, and resume event.
- [ ] Characterize ADC variation near each boundary.
- [ ] Resolve the September 30 paired bench discrepancy before treating reconstructed VCAP as calibrated: with CH3 on live MP1584EN IN+ and the 10 nF GPIO16 filter installed, the user reported raw mean 598 (range 563–630 over 32 samples) at approximately 7.26 V, while the current 461@5.5 V / 634@7.5 V anchors predict about code 613. Repeat synchronized DMM VCAP, GPIO16, CH3 and retained-code measurements at several stable points with explicit J-Link state; recalibrate only from reproducible paired measurements.
- [ ] Investigate the October 1 paired GPIO16 capture before a voltage sweep: CH4 is about 0.180 V and stable in 1 ms averages through two ADC intervals, but the 32-read raw means are 544/550 with 505–580 combined range. Audit HAL sample-read return/count/slot, reference/trim state, ADC mode-switch settling and GPIO16 ground/pad identity. Use a diagnostic build to compare the existing repeated mode switching with one supply-mode hold over 32 reads under identical bench conditions; retain the unmodified baseline result. Do not change production anchors or thresholds from these data alone.
- [x] Prepare and build a calibration-only switched-versus-held ADC diagnostic with retained HAL FIFO read/count/slot error counters. This is a firmware/tooling milestone only; the paired physical experiment remains open.
- [x] Flash and physically exercise the diagnostic at a stable approximately 7.28 V VCAP with CH3 on live MP1584EN input and CH4 on GPIO16; compare switched/held BTN0 records and read retained data with BTN1. User-supplied `v14_adc_gpio16_paired_02/03` captures show later pairs agreeing within 1-4 counts and no reported FIFO errors; this does not close ADC accuracy validation.
- [ ] Resolve the remaining approximately 30-50-count difference between HAL-corrected ADC readings and the nominal 1.19 V code predicted by a measured approximately 0.180 V GPIO16. Inspect correction trims/reference and ordered per-read code behavior if needed; repeat controlled paired measurements at a second and third stable VCAP before changing production calibration or thresholds.
- [ ] Resolve the GPIO17 diagnostic sweep repeatability and individual-reading spread: the user-supplied 7.30→6.80→7.30 V A–B–A run returned to essentially the same 7.30 V group mean with zero HAL FIFO errors and switched/held agreement, but a previous 7.30 V group was 25 codes lower. Capture ordered readings synchronized with GPIO17 voltage/VDD at fixed VCAP, then qualify the intended production GPIO16 path before fitting anchors or validating policy boundaries. GPIO17 remains calibration-only.
- [x] Add calibration-only ordered 32-code capture to the GPIO17 switched/held diagnostic and build the selected image. This image has now been flashed and physically exercised in the user-supplied `adc_gpio17_2` capture; this does not validate production GPIO16.
- [x] Capture and inspect ordered GPIO17 codes at fixed VCAP alongside selected-pad, live VCAP and board-VDD scope channels (`adc_gpio17_2`, October 1); no consistent within-burst startup trend or switched/held difference was found.
- [x] Repeat the GPIO17 ordered-code diagnostic after controlled removal of the duplicate ground path. User-supplied SWO means stayed about 590 codes and within-burst scatter did not improve; this does not identify the ADC root cause.
- [x] Compare probe-free GPIO17 ADC codes after rewiring: six user-supplied on-target bursts averaged 617.4 codes with aggregate SD 8.4, versus 590.0 and SD 17.3 with probes attached. User confirmed DMM VCAP 7.30 V and unchanged FG setting; GPIO17/VDD were not measured, and no single probe/ground cause is identified.
- [ ] Isolate the measurement-loading path with no-probe → one scope ground lead only → one GPIO17 probe tip → no-probe controls at fixed DMM VCAP/GPIO17, then add remaining probe channels individually if needed. Do not use scoped ADC/policy traces as calibrated evidence until this is resolved.
- [ ] Explain the earlier 565-code group and remaining code spread; qualify production GPIO16 at multiple stable voltages before fitting anchors or testing policy thresholds. Inspect HAL correction/ADC configuration if the instrument-isolated measurements remain inconsistent.
- [x] 2026-10-05 (Claude, user decision): GPIO17/ADCSE2 is the production VCAP pad. `BISEN_HARVEST_SUPPLY_PIN` (default 17; 16 = legacy GPIO16) in the original, IS and OS harvest apps; full builds also require `BISEN_HARVEST_CAL_PIN` equal to the pad, so the invalid 461/634 GPIO16 anchors no longer build. `fit_vcap_calibration.py` reads the pad from SWO and prints both flags. Host syntax checks only; no ARM build or hardware validation. This supersedes "GPIO17 remains calibration-only" above and the GPIO16 qualification item.
- [x] 2026-10-06 GPIO17 calibration sweep (scope disconnected, USB unplugged during BTN0 captures, KM100 DMM on VCAP): 25 records over 8 setpoints 5.6–7.7 V, down and up passes. Fit code = −1.47 + 85.195·VCAP (11.7 mV/code), RMS residual 1.0 code, worst 2.3 codes (27 mV), down/up hysteresis ≤ 3 codes. Anchors 476@5604455 µV, 655@7705526 µV, CAL_PIN=17. Raw files and fit output: `apps/bisen_camera_harvest/traces/calibration/2026-10-06_gpio17_sweep/`. Absolute accuracy is limited by the KM100 (actual readings, 20 V range, 0.01 V resolution). Single policy readings (median-of-3 AVG16) spread 13–34 codes min–max within a point, so a single decision can sit ~±0.1–0.2 V from the mean; threshold verification must account for this.
- [x] 2026-10-07 GPIO17 pin capacitor A/B at 6.20 V: 100 nF cuts policy-reading SD 5.7 → 4.5 codes (−22 %) and single-conversion SD 34 → 24 codes (−30 %) with no mean shift (anchors still valid). 100 nF is now fitted. See `traces/calibration/2026-10-07_pin_cap_ab/`.
- [ ] Optional firmware noise reduction (needs user/Bobby authorization; changes production ADC behaviour and per-read energy): supply slot AVG128 and/or LP0, or 2–3 consecutive-reading debounce before checkpoint. Evaluate together with the VCAP-read polling-cost fix.
- [ ] Fix or document: RESET clears the calibration SRAM log although the banner says a reset is tolerated (user observation 2026-10-07).
- [x] 2026-10-07 threshold verification baseline (GPIO17 anchors, 100 nF, original app via `flash_harvest_rf_replay.sh`, FG staircases `thr_down`/`thr_up`, 3 cycles): stop 6.29–6.38 V (nominal 6.20), resume 6.645–6.775 V (nominal 6.80), one event per crossing, checkpoint written on 2 of 3 automated stops (writes ≈0.5–2.4 ms). Passes the no-chatter/no-repeat limits; edges move inward ~0.1–0.18 V from single-reading noise. See CODEX_LOG 2026-10-07.
- [x] VCAP read-policy change (stop/resume confirmation + reduced polling), user-approved 2026-10-07, per `docs/RUIC_VCAP_Read_Policy_Change_Brief.md`: implemented in the original, IS and OS harvest apps behind `BISEN_HARVEST_STOP_CONFIRM` / `BISEN_HARVEST_RESUME_CONFIRM` / `BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS` (defaults 1/1/1 = original behaviour); candidate helpers `build_/flash_harvest_rf_replay_vcap_policy.sh` (3/3/32, HVR4). ARM builds (default and candidate; IS/OS too) pass with no new warnings; host test `tests/vcap_confirm_test.c` passes. **Build/host only — not flashed, no hardware validation.** See CODEX_LOG 2026-10-07 (Claude Code).
- [x] (done 2026-10-08, see next item) **Hardware rerun of the VCAP read-policy candidate (user):** flash `flash_harvest_rf_replay_vcap_policy.sh`, then `thr arm` and 3 × (`thr down cN_down` capture; `thr up cN_up` capture) with the same scope command as the baseline. Expected if it works: stop ≈ 6.20–6.25 V, resume ≈ 6.75–6.80 V, still one event per crossing, VCAP-ADC (state 1) share of working time ≈ 52 % → a few %, frame period ≈ 7.8 s → ≈ 4 s. Compare against the 2026-10-07 baseline (stop 6.29–6.38 V, resume 6.645–6.775 V). Note HVR4 does not adopt HVR3 records.
- [x] 2026-10-08 hardware rerun with candidate C3/R3/K32 (HVR4): stop 6.24–6.29 V, resume 6.77–6.82 V, one event per crossing; VCAP-ADC share 52 % → 5–10 %, CNN period 7.84 → 4.1–6.0 s. See CODEX_LOG. Open: residual +0.065 V under-load stop offset; band-dependent frame period; ablation builds (C3/R3/K1, C1/R1/K32) only if the paper claims each effect separately.
- [ ] Rerun the same 3-cycle staircase after any further VCAP-read firmware change (averaging, AVG128/LPMODE0) for a before/after comparison.
- [x] 2026-10-06: `build_harvest_rf_replay.sh` (new BASE name `..._gpio17_2026-10-06`), `flash_harvest_rf_replay.sh`, `docs/RUIC_R2A_PAIR_RUN.md` and the three app READMEs now pass `BISEN_HARVEST_SUPPLY_PIN=17 BISEN_HARVEST_CAL_PIN=17` and the 476/655 anchors. Thresholds unchanged (5.8/6.2/6.8/7.3 V). Not yet built on the ARM toolchain.
- **What this proves:** the implementation follows the intended energy-aware policy and hysteresis rather than merely compiling with threshold constants.

### V4 — Production checkpoint trigger and cold-restore validation

- [ ] Create dirty, incomplete camera/CNN progress under the current policy.
- [ ] Drive VCAP into the low-energy condition and observe exactly one production checkpoint episode.
- [ ] Capture state-DAC plus software metadata: slot, sequence, phase/layer, position, dirty/clean state, and CRC/commit outcome.
- [ ] Remove power after the committed checkpoint and verify actual board-rail collapse; a RESET-only test is a separate warm-reset test.
- [ ] Raise energy to the qualified resume region and verify cold restore.
- [ ] Complete the inference and compare against the V2 reference output.
- **What this proves:** the selective low-energy checkpoint policy works end-to-end on the current workload, not only on the older Sobel bring-up.

### V5 — Compulsory-Stop ablation

**Priority:** deferred to a later paper; out of scope for the current CNN study.

- [ ] Build an ablation of the proposed runtime that forces the BISen-style Stop/wait transition between useful application phases while keeping constant the model, input, chunk policy, checkpoint implementation, compiler optimization, MCU clock, hardware, and replay trace.
- [ ] Compare against the direct-transition proposed runtime.
- [ ] Measure completion, latency, total energy, state energy, and non-useful Stop/wait time.
- **What this proves:** isolates the value of removing compulsory Stop transitions from other architectural differences.

### V6 — SunSift-style layer-granular baseline

**Priority:** deferred to a later paper; out of scope for the current CNN study.

- [ ] Implement a controlled same-AMAP4PEVB, same-model policy inspired by SunSift.
- [ ] Restrict energy decisions/checkpoints to layer boundaries.
- [ ] Force application phases through Stop/light-sleep behavior as defined for the controlled baseline.
- [ ] Keep the LeNet model, preprocessing, MCU configuration, and replay inputs identical to the proposed runtime.
- [ ] Measure lost/recomputed work, completion, latency, checkpoints, restores, and energy.
- **What this proves:** isolates fine-grained coherent progress from a layer-granular intermittent policy without conflating platform/model differences.

### V7 — Monolithic TensorFlow Lite Micro (TFLM) baseline

**Port equivalence gate (2026-09-29):** Freeze the deployed model/input hashes and layer manifest. Build separately pinned TFLM reference-int8 and TFLM+CMSIS-NN variants on the AMAP4PEVB, identifying exact linked kernels and fallbacks. Confirm weights, shapes, quantization, ten output scores, labels and held-out accuracy; record tensor-arena use and flash/peak RAM. Separate `AllocateTensors()` setup from warmed `Invoke()` timing. Label variants that need different quantization as task-equivalent, not strict numeric equivalents. During replay account for actual lost invocations after reboot; document any common camera/restart wrapper.

- [ ] Port/run the same int8 LeNet model and inputs in TFLM on the same current AMAP4PEVB Apollo4 Plus Rev. 1.0 platform.
- [ ] Treat the complete invocation according to its actual interruption/restart behavior; do not hide lost work after a power failure.
- [ ] Record flash, peak SRAM, latency/frame, completion, total energy, and energy/frame.
- [ ] First satisfy the V0 identical-model/logit and stable-supply benchmark gates. During replays, document what survives an actual power loss and any added restart wrapper separately from stock TFLM behavior.
- **What this proves:** compares the custom intermittent runtime against a conventional embedded ML deployment on the same MCU/model.

### V8 — Capuchin feasibility / comparison

**Concrete go/no-go order (2026-09-29):** (1) Map the frozen RUIC graph and weight arrays to Capuchin's Keras encoder and generated header; (2) audit operators, especially average versus max pooling; (3) compare fixed-point conversion, accumulator, rounding and outputs; (4) only then isolate MSP430/CCS/LEA/FRAM dependencies and compile the original generated layer logic on Apollo4; (5) measure memory fit, per-layer outputs, accuracy and continuous-power performance; (6) audit true reboot behavior before paired trace replays. Explicitly label a changed precision/model as a task-level tradeoff, not engine-only speedup. Stop before a full port if source weights or operator semantics cannot be made comparable.

- [x] First perform a feasibility investigation; do not assume a direct comparison is valid. 2026-10-06 (Claude, user-assigned): C1–C3 pass at host level with a disclosed weight reconstruction; see `docs/RUIC_V8_Capuchin_Apollo4_Port.md`.
- [x] If the same model/input can be ported defensibly to Apollo4, implement and document that port. 2026-10-06: `apps/bisen_capuchin` (upstream 76b6eb2 + 3 tagged changes incl. AveragePooling2D; both engines in one image; bench / energy-loop / intermittent modes). Build + emulator evidence only.
- [x] Check operator support (including average pooling), parameter conversion, 16-bit versus int8 arithmetic, held-out accuracy and output equivalence. Host, 10,000 MNIST test images: RUIC 98.92 %, Capuchin Q5.10 98.93 % (McNemar p = 1.0); port C == independent NumPy model bit-exact; compiled ARM images bit-exact on 100 vectors in a Cortex-M4 emulator. Tier B. Port adds no intermittent recovery (faithful restart-from-scratch).
- [ ] If not, keep Capuchin native-platform/model results separate from same-platform latency/energy tables. (Not needed for Apollo4; still never rank cells across platforms.)
- [ ] Obtain `lenet_mnist.keras` from Bobby and regenerate with `capuchin_export.py --keras` (current header is an int8-reconstruction, scale chosen on training data).
- [ ] Board B1–B4 (`docs/RUIC_V8_Capuchin_Apollo4_Port.md` §6): self-check PASS, DWT timing for kernels 1/2/0, per-layer profile, SRAM/copy ablations, energy loop with USB/J-Link unplugged. Check `ruic_nn_run` median against V0's 9.60 ms before using any number.
- [ ] Gate X1 with the MSP430 colleague: `apps/bisen_capuchin/msp430/` package (same MODEL_ARRAY, avgpool patch, 16 cross-check vectors); `main_xchk.c` not yet compiled in CCS.
- [ ] B6a intermittent (mode 2, Capuchin vs RUIC engine without checkpoints). B6b vs the RUIC runtime needs a matching inference-only arm with an identical job-completion marker (harvest-app DAC 5/6 mean NV-commit/restore, not completion); not authorised/implemented.
- **What this proves:** provides model-deployment context without making an invalid cross-platform energy ranking.

### V9 — PT1/PT2/PT3 replay calibration and repeatability

- [ ] Establish the provenance and physical meaning of PT1/PT2/PT3.
- [ ] Document original sample interval, units/relative-envelope status, replay mapping equation, number of function-generator commands, replay duration/update interval, source impedance/loaded-path calibration, and whether the command corresponds to a post-rectifier envelope or RF-carrier amplitude.
- [ ] Verify replay repeatability at the actual VCAP path before using the traces for runtime comparisons.
- **What this proves:** the intermittent power stimulus is reproducible and interpretable rather than an arbitrary waveform.

### V10 — State-resolved energy measurement

- [ ] Capture synchronized physical VCAP, regulated board VDD, three-bit state-DAC, and voltage across the 1 Ohm high-side shunt.
- [ ] Decode at least sensing, ADC/policy, compute, save, restore, and wait/boot.
- [ ] Integrate current/power over decoded state intervals.
- [ ] Report energy by state, total energy, joules/completed frame, checkpoint/restore overhead, and time/energy in non-useful states.
- **What this proves:** identifies where the system spends energy and whether scheduling/checkpointing improves useful work.

### V11 — End-to-end runtime comparison under intermittent traces

- [ ] Run the proposed runtime and TFLM under the same PT1/PT2/PT3 conditions; add a same-platform Capuchin port only after V8 passes.
- [ ] Use the same input set, model, preprocessing, compiler optimization, clock, and hardware configuration.
- [ ] Use at least five runs per runtime/trace pair unless a later experimental design explicitly changes this.
- [ ] Report completed frames, completion rate, latency/frame, total energy, joules/completed frame, checkpoint/restore count, lost/recomputed progress, non-useful wait/boot/storage time, and appropriate summary statistics.
- [ ] Count only correct completed classifications in a fixed observation window; state how a partially completed frame at the end of a trace is handled. Also report inference-only completions from preloaded inputs and end-to-end frames with the identical camera path.
- [ ] Pair repeat runs from the same measured initial VCAP and trace commands, randomize runtime order where practical, and record the actual loaded VCAP and rail waveform for each runtime. Do not treat the generator setpoint as the energy delivered to the board.
- **What this proves:** provides the paper's principal apples-to-apples evidence under repeatable intermittent energy.

### V12 — Power-path / regulator revision validation

- [ ] Characterize the present 10 mF + MP1584EN (~1.9 V) path as the current baseline.
- [ ] Before installation, resolve the TPS7A0220 input limit: 6.0 V recommended maximum, 6.5 V absolute maximum; it is incompatible with direct connection to the present 7.5–8 V reservoir. Choose a lower reservoir range with transient margin or a suitably rated regulator. Update replay ceilings, ADC calibration and thresholds together.
- [ ] After installing a qualified revised power path:
  - verify the nominal 2.0 V rail under camera/CNN load;
  - measure quiescent/discharge behavior;
  - measure regulator dropout behavior as VCAP decays;
  - revalidate checkpoint/work/resume thresholds;
  - repeat relevant energy measurements because the rail and regulator behavior have changed.
- [ ] Do not mix MP1584EN and TPS7A0220 results in one comparison table without identifying the power path.
- **What this proves:** separates firmware gains from regulator losses and establishes the energy-safe operating region of the final power path.

### V13 — Optional TS5A3167 switched-divider ablation

- [ ] Before adopting the TS5A3167DBVR, quantify the present 390 kOhm / 10 kOhm divider's static energy cost over representative VCAP levels.
- [ ] If the switch is implemented:
  - verify ADC settling after switch-on;
  - characterize switch leakage/on-resistance impact;
  - measure net energy saved over the complete workload/replay;
  - verify switched sensing does not corrupt threshold decisions.
- [ ] Adopt it only if measured system-level benefit justifies the added hardware/control complexity.
- **What this proves:** determines whether divider isolation materially improves the system rather than assuming it does.

### V14 — Production MRAM program and complete-checkpoint energy (new)

- [ ] Freeze the board/power path and identify the live checkpoint backend's actual MRAM-program calls, record/header sizes, payload packing and interrupt-disabled intervals. Mark both the full save interval and the low-level programming call without changing the production write sequence or adding serial output in the timed region.
- [ ] On regulated board power, capture load-side VDD and a calibrated high-side shunt waveform at enough bandwidth to resolve short calls; use the state marker plus dedicated edge markers or timestamp correlation for start/end. Measure probe gain/offset, shunt resistance, sampling rate and uncertainty. Four analog channels can capture upstream and downstream shunt voltages (the downstream endpoint also supplies VDD), VCAP and state DAC. Adding an independent HAL-call marker then requires digital capture, a differential current channel freeing an analog channel, or a separate focused acquisition with a measured alignment event. Verify all required channels work; the previously reported CH3 issue is unresolved here.
- [ ] Use the corrected `apollo-camera` ladder decode (GPIO62/63/61 through 99.3/201/398 kΩ) and the current probe polarity: CH4 MP1584EN OUT+ upstream, CH1 board J7.3 downstream, CH2 DAC junction, CH3 live MP1584EN IN+. Re-decode old raw captures before interpreting state 1 versus 4; reject edge overshoot and verify code-4→5 intervals against production SWO metadata.
- [ ] Integrate VDD(t) I(t) for the full checkpoint and for the MRAM program interval. Use a matched no-write/dry-run or equivalent idle/control path over the same interval to estimate incremental write energy without pretending that the rail capture isolates only the on-chip MRAM array.
- [ ] Repeat across small/median/maximum actual record lengths and representative camera/CNN cursor positions, both slots, and many trials. Report measured bytes, program-call counts, duration, peak current, median/spread and control-subtracted energy. Keep completion/tombstone writes separate from recovery checkpoints.
- [ ] Repeat or clearly invalidate these measurements if MP1584EN is replaced by TPS7A0220. Use worst-case checkpoint energy plus regulator/dropout margin when qualifying VCAP thresholds.
- **What this proves:** whether the emergency-save policy is affordable in the actual reservoir window and how much full checkpointing costs relative to useful CNN work.

- [ ] TODO (user step 5): V14 bench-harness firmware (plan §8): `BISEN_V14_BENCH` steady-state save loop, `V14_SKIP_PROGRAM` control build, GPIO edge marker around `am_hal_mram_main_program`, `CKPT_MRAM_TRACE=0`; record app/engine identity and binary hash.
- [ ] Decide the V14 instrument. Available now: Siglent SDS1204X-E (8-bit) and KAIWEETS KM100 (2000-count DMM). Neither resolves mA-level current on a 1 Ω shunt with enough accuracy or dynamic range for paper-grade energy; options are a dedicated energy analyzer (Joulescope JS220 / Otii Arc / SMU; neuralSPOT already integrates Joulescope) or a current-sense amplifier ahead of the scope plus a higher-resolution DMM for steady-state loops.
- [x] 2026-10-06: user confirmed SB3 is in its default (closed) state and will stay that way for now. Consequences: never measure with USB/J-Link connected (the onboard LDO then shares the MCU rail); with USB unplugged the MP1584EN back-feeds the LDO (~80 µA divider plus uncharacterized leakage). Differential (loop − control) energy cancels this constant; absolute per-state numbers must disclose it.
- [ ] Optional, deferred by user (2026-10-06): FNIRSI FNB58 inline between reservoir (after the diode/10 mF) and MP1584EN input for reservoir-side steady-state power in V14 bench loops; power the FNB58 externally so it does not load the reservoir, and check its ground path first.

First-run tooling now includes a deterministic 120 s FG DC-command profile and
an offline shunt integrator for decoded code-4→5 write candidates. This is
**not** a completed V14 measurement: confirm actual VCAP crossings, shunt
calibration, write identity and board-rail integrity on the bench. See
`docs/RUIC_V14_MRAM_Checkpoint_Energy_Plan.md` §9.

### Additional repository controls

Preserve the applicable board/revision, power path, reservoir, regulated rail, divider/ADC calibration, thresholds/budgets, model/weights/quantization, inputs and acquisition/preprocessing, MCU clock, compiler/build flags, trace/scaling/timing, initial capacitor energy, and probe/shunt/measurement configuration. Record commit, binary identity, commands, raw artifacts, repetitions, and any deviations. For ablations, vary only the intended feature. Disclose unavoidable differences and their effect on claims. V12 intentionally changes the regulator and requires a new validation configuration; V13 is optional and changes divider switching if adopted.

Existing incremental scheduling, selective MRAM checkpoints, session persistence and 100/500/1000 budgets are implemented. Their presence does not complete physical validation. Reuse them in the new tests. Keep known-good OS firmware and separate candidate build/checkpoint identities. See [port equivalence checklist](docs/RUIC_CNN_Port_Equivalence_Checklist.md).

## Follow-up documentation / reproducibility — not automatically authorized

- [ ] Audit provenance/configuration of the now-tracked replay helpers, tools and trace files. Their earlier untracked status is obsolete; tracking alone does not complete V9.
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
