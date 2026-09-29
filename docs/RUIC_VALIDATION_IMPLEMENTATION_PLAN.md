# RUIC testing implementation plan — reconciled September 29

Canonical repository: `/Users/ghart/Documents/Ambiq/neuralSPOT`; branch `main`, inspected HEAD `190c4f503d509aa7e64c64c764ec02b1cfd2ef33`. This replaces the earlier V1–V13-only assessment with the supplied September 29 research priorities. It is a proposal and source assessment, not implementation or physical validation.

## Current priorities

1. Review the already performed continuous-power engine comparison (V0); recover its exact inputs, builds and results before deciding which conditions need additional runs.
2. Establish same-model OS/IS and TFLM reference/CMSIS-NN comparisons (V1/V7), then correct completions under paired replays (V9/V11).
3. Measure production full-checkpoint and incremental MRAM-program energy (V14), supporting threshold/reserve qualification (V3/V4).
4. Capuchin remains a short model/operator/arithmetic feasibility investigation before a full port (V8). V5 and V6 are deferred to a later paper; V13 is optional.

The local reconciliation and bridge baseline are already complete. Do not restart migration, replace AGENTS or discard verified implementation details because the imported scratch copies predate them. The paper preview is not required for this source/measurement assessment and was not supplied or reviewed here.

## Repository findings retained

1. The existing replay helper still builds `apps/bisen_camera_harvest`, whose default is output-stationary. A newer `apps/bisen_camera_harvest_IS` has input-stationary/SIMD source and selects `NN_DATAFLOW=1 NN_SIMD=1 NN_MAX_CONV_IN_C=6`. These are separate variants, not evidence that the bench is running the new engine.
2. The IS app needs integration review: `nn_engine.h` includes `nn_wo.h` and `nn_build.h`, and `nn_kernels.c` includes `nn_simd.h`, but these three headers are present only in the original app's source directory. Its module retains the original app binary name and references the original app linker file. Give the variant explicit dependency paths, a coherent build/deploy identity and checked linker reservation before treating it as independently runnable. No full build was attempted here. The CHANGES-r2a note in the original app describes engine replacement that does not match that app's current module/engine; reported external test totals are not new validation evidence.
3. The two apps have byte-identical deployed weight headers. This does not establish model/export lineage, accuracy or engine equivalence. Work-unit cost and live checkpoint size change with dataflow: retaining the numbers 100/500/1000 does not retain the same work or energy between OS and IS. Recharacterize execution and checkpoint bounds before carrying over safety thresholds. Keep old checkpoints isolated across variants/model/configuration changes.
4. TPS7A0220 is not a direct replacement on the existing reservoir. TI specifies recommended input up to 6.0 V and absolute maximum 6.5 V: the present 7.5–8 V reservoir exceeds both. Source: https://www.ti.com/lit/ds/symlink/tps7a02.pdf . V12 must first choose a lower-voltage reservoir with transient margin and newly calibrated thresholds, or a regulator rated for the existing voltage range. The present 6.2/6.8/7.3 V work gates cannot simply remain in a <=6 V LDO system. A pre-regulator is a different power path with its own losses, not a like-for-like swap. Installed hardware remains MP1584EN until the user confirms otherwise.
5. Replay tools and traces are now tracked. Their presence does not resolve PT1/PT2/PT3 provenance or calibration. Existing tools already support bounded cycles, playback duration, precharge and command logging; extend those instead of rewriting them.
6. `scope_capture_plot.py` estimates net reservoir energy change using 0.5*C*(Vstart²−Vend²). It does not integrate measured shunt current. Under simultaneous charging this is not board energy. Its capacitance default is 0.1 F, versus the installed 0.01 F; use `--no-energy` for state-only captures, or explicit `--cap-f 0.01` for clearly labeled reservoir-change analysis. Final energy analysis must use measured current and a stated measurement boundary.
7. The offline summary is retained SRAM and cannot preserve complete experiment history across true power loss. Continuous replay disables both offline validation and SWO, so the existing completion printout is not an independent cross-outage completed-job counter. `harvest_stats` provides RAM timing/counters, not persistent experiment accounting. State code 5 also marks session/tombstone operations, and code 6 follows part of the inference restore work; raw marker counts and intervals are not sufficient completion/restore-energy ground truth.

## Hardware changes actually needed

Keep the current AMAP4PEVB Rev. 1.0, MP1584EN, 10 mF reservoir and fixed 390 kΩ/10 kΩ divider for initial replay/instrumentation development. A switched divider is not required. Do not infer current jumpers, camera hardware or isolation details from the historical Blue Plus board.

For V0/V14, use a separately documented stable regulated board-power configuration and a calibrated high-side shunt/current instrument. Ensure no other source bypasses the measured path. Then validate the instrumented production configuration on the replay power path. Stable-power microbenchmarks and harvested replay answer different questions; label each supply boundary and rail voltage.

A four-channel scope can capture:

| Channel | Signal |
|---|---|
| CH1 | Shunt downstream / board VDD |
| CH3, if operational | Shunt upstream |
| CH2 | State DAC |
| CH4 | Reservoir VCAP |

Compute I=(V_CH3−V_CH1)/R and board energy as integral(V_CH1*I dt). This measures board-input energy, excluding upstream regulator/divider losses. Total-system/input energy needs a separately defined measurement boundary. Ground clips go to common circuit ground, not opposite shunt terminals. Verify channel gain/offset matching and current resolution; the previous CH3 fault remains unconfirmed. Choose shunt value and instrument range from measured burden and resolution, not an assumption that 1 Ω is always adequate.

For a dedicated MRAM timing edge, use a short stable-power capture that repurposes CH4, an external digital recorder, or a differential current measurement freeing a channel. Full save and program-call boundaries need distinguishable events; do not silently replace the existing state-DAC encoding. Long multi-minute overview captures cannot establish that brief program calls are resolved. Document sample rate, bandwidth, alignment and marker overhead.

TPS7A0220 cannot directly replace MP1584EN at current VCAP: TI specifies 6.0 V maximum recommended input and 6.5 V absolute maximum. Choose a lower reservoir range with transient margin or a suitably rated regulator before V12. Update all voltage limits, calibration and work/resume/reserve qualification as a named new configuration. No hardware change is asserted here. [TI datasheet](https://www.ti.com/lit/ds/symlink/tps7a02.pdf).

## Firmware and tooling packages proposed

| Package | Reuse | Add or resolve |
|---|---|---|
| Evidence freeze / V0 review | Existing OS and IS code, identical deployed weight headers, any already recorded benchmark artifacts | Recover exact benchmark build and model/input hashes, ten outputs and timing/energy conditions. Reconcile IS dependency/build identity before new target runs. Do not rebuild a benchmark already available without first reviewing it. |
| Model/engine equivalence | Current LeNet and preprocessing | Deterministic vector mode, per-layer/ten-score output checks, exporter/layer/quantization manifest and interruption tests. Distinguish strict numeric equivalence from task-equivalent arithmetic. |
| V7 TFLM baseline | Existing SDK integration | Separate identifiable reference-int8 and CMSIS-NN app/build variants using identical `.tflite` bytes. Pin actual kernels/fallbacks, arena/stack/flash and allocation vs warmed Invoke timings. Model/export availability remains unresolved; no matched baseline is established by generic SDK support. |
| V14 MRAM measurement | Production `ckpt_save`, `ckpt_save_scan`, two slots and `ckpt_mram.c` | Compile-gated full-save and HAL-call timing events; a focused test app/profile reusing the production backend, real live payloads and linker rules; matched control profile and short-capture energy analysis. |
| Replay outcomes / V9–V11 | Existing session arm/disarm, restore, bounded FG replay and state decoder | Identifiable completion events with job/input ID and correctness metadata, robust across outages without duplicates. External capture is preferable where practical; durable accounting changes require measurement of their own cost. Add manifests, synchronization and calibrated current integration. |
| Capuchin / V8 | Frozen RUIC graph and weights | Gate source-weight mapping, operator semantics and numeric representation first. Only then port actual generated layer logic and verify resources/outputs before bench comparison. |

TFLM reference and CMSIS-NN are configurations of the same runtime. Verify selected kernels rather than inferring optimization from a library being present. The Capuchin README documents MSP430FR5994 and max pooling; the live RUIC graph uses average pooling. An average-pool addition needs explicit implementation/equivalence evidence; swapping to max pooling changes the model. See the [equivalence checklist](RUIC_CNN_Port_Equivalence_Checklist.md), [Capuchin source](https://github.com/leleonardzhang/Capuchin), and [CMSIS-NN documentation](https://github.com/ARM-software/CMSIS-NN).

## V14 implementation detail

`apps/bisen_camera_harvest/src/ckpt_mram.c::program_words` calls `am_hal_mram_main_program` with interrupts masked and tracks call/success/program-unit counters. Logical writes use a 64-byte bounce buffer. `ckpt.c` performs payload/CRC/packing and writes the 16-byte header last. The measurement must preserve that sequence and distinguish complete save cost from HAL-call cost. Existing backend tracing must be disabled in timed regions.

Use real scan/CNN contexts at small, median and maximum encountered payloads, both slots, multiple cursor/layer positions and many trials. Record actual bytes, HAL calls, elapsed time, peak current and energy median/spread. Keep session writes and completion/tombstone retirement separate from recovery saves. Exercise a separate matched no-write/control profile; never substitute the dry-run backend into a claimed durable-recovery experiment. Control-subtracted board energy is an estimate of incremental write activity, not isolated MRAM-cell energy. Include marker/instrument uncertainty and revalidate energy reserve with the selected engine and power path.

## Implementation order and acceptance

1. Review V0 artifacts, freeze source/model/input/build identities and establish which TFLM variants already exist. Preserve existing benchmarks rather than automatically repeating them.
2. Add V14 instrumentation/harness and completion/energy tooling in separately named profiles while preserving the current production app. Gate correctness with existing host checks plus new meaningful output/restore checks; then obtain hardware evidence.
3. Complete missing TFLM variants and verify scores/accuracy before speed comparisons. Run a short Capuchin feasibility audit independently of committing to a port.
4. Qualify actual ADC/work/checkpoint/resume behavior for the chosen engine and power path. Decide any regulator change before final paired energy comparisons.
5. Run at least five paired trials per admitted runtime and PT trace initially, fixed duration and matched measured initial VCAP, randomized order where practical. Report correct committed results, failed/partial jobs, resets, recomputation and energy per correct completion; handle zero completions explicitly. Keep preloaded-input and camera-plus-inference results separate.

No firmware code, hardware setting or test result changed during this reconciliation. No new firmware build or host/on-target test was run. The immediate missing evidence is the already performed V0 run: artifact location, tested engines and their exact build/input/model identity. Measurement equipment and remaining bench isolation details also need an experiment record.
