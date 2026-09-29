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
- [ ] Before adopting the newer IS/SIMD app, resolve missing local headers/build-deploy identity, run equivalence/restore tests and recharacterize unit/checkpoint energy; retain the existing OS reference.
- [ ] Before V12 assembly, resolve TPS7A0220's 6.0 V recommended-input limit versus the present 7.5–8 V reservoir. Select the voltage range/power path, then change replay ceilings, calibration and safety thresholds together.
- [x] Reconcile the downloaded September 29 planning files against the live state; expand to V0–V14, defer V5/V6 and preserve completed bridge work. The earlier V1–V13-only implementation order is superseded.
- Proposed next package: review existing V0 evidence and freeze model/input/build identity; then add production-path MRAM energy instrumentation and focused harness (V14), completion accounting/replay analysis (V9–V11), and missing TFLM variants (V7). Capuchin remains feasibility-first. Firmware implementation is still proposed, not performed or activated by this document merge.

## Shared V0–V14 validation program — research backlog

These tasks document the agreed validation strategy. They are **not automatically authorized for Codex implementation** until the user moves them to Active or explicitly assigns them.

### Research priority after supervisor discussion (2026-09-28)

**2026-09-29 user update:** V0's continuous-power engine comparison has been performed; results and exact conditions are pending receipt and review, so its evidence checkboxes remain open. Defer V5 compulsory-Stop and V6 SunSift-style layer-boundary studies to a later paper. Current paper priority is the CNN engine, controlled TFLM/feasible Capuchin comparisons (V7–V8, V11), and MRAM energy (V14). Do not implement or include V5–V6 in the present study.

1. Establish whether the custom CNN engine is faster or lower-energy than TFLM on the **same int8 model and Apollo4 board** under continuous supply (V0, V7). Correct outputs and accuracy are entry gates (V1, V2).
2. Establish whether runtime differences translate into **more correct completed classifications per identical replay** on one final power path (V9, V11). Report inference-only and full camera-plus-inference outcomes separately; equal generator commands may produce different loaded VCAP waveforms.
3. Quantify the full checkpoint and incremental MRAM-program energy (V14), then physically validate restart correctness, safe thresholds and checkpoint behavior (V2–V4).
4. Defer compulsory-Stop and SunSift-style studies (V5–V6) to a later paper. Include Capuchin in direct rankings only after the same-platform/model/precision feasibility gate (V8). Keep regulator and divider studies (V12–V13) identified as hardware revisions.

**Comparison contract:** freeze model file/hash, topology, weights, quantization, preprocessing, image order, clock, compiler flags, camera setting, board, initial VCAP and power path. For every fixed-duration PT1/PT2/PT3 replay, use identical function-generator commands and record actual loaded VCAP, VDD, resets, and completed correct outputs. Repeat paired runs in randomized runtime order, at least five per runtime/trace pair initially; summarize variability and uncertainty. A different CNN model is a separate generality experiment and cannot alone prove this engine faster than TFLM.

### V0 — Continuous-supply CNN-engine speed and energy (new primary test)

**Status:** User reports this comparison has been run; numerical results, methods, and raw evidence pending. Review against the controls below before marking individual criteria complete.

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

- [ ] First perform a feasibility investigation; do not assume a direct comparison is valid.
- [ ] If the same model/input can be ported defensibly to Apollo4, implement and document that port.
- [ ] Check operator support (including average pooling), parameter conversion, 16-bit versus int8 arithmetic, held-out accuracy and output equivalence. Record any changes to model semantics or intermittent recovery added by the port.
- [ ] If not, keep Capuchin native-platform/model results separate from same-platform latency/energy tables.
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
- [ ] Integrate VDD(t) I(t) for the full checkpoint and for the MRAM program interval. Use a matched no-write/dry-run or equivalent idle/control path over the same interval to estimate incremental write energy without pretending that the rail capture isolates only the on-chip MRAM array.
- [ ] Repeat across small/median/maximum actual record lengths and representative camera/CNN cursor positions, both slots, and many trials. Report measured bytes, program-call counts, duration, peak current, median/spread and control-subtracted energy. Keep completion/tombstone writes separate from recovery checkpoints.
- [ ] Repeat or clearly invalidate these measurements if MP1584EN is replaced by TPS7A0220. Use worst-case checkpoint energy plus regulator/dropout margin when qualifying VCAP thresholds.
- **What this proves:** whether the emergency-save policy is affordable in the actual reservoir window and how much full checkpointing costs relative to useful CNN work.

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
