# RUIC Project State

**Status date:** 2026-09-28  
**Project:** Apollo4 intermittent camera-inference framework / BISen-derived RUIC research  
**Purpose of this file:** shared, concise state for ChatGPT and Codex. It intentionally distinguishes verified history from current direction and unresolved timeline conflicts.

## 1. Research objective

**CURRENT/DIRECTION**

Develop and experimentally evaluate an energy-aware intermittent camera-inference framework on Ambiq Apollo4. The current camera/harvest software targets Apollo4 Plus (`apollo4p_evb`); the older Sobel bring-up targeted Blue Plus KXR. The current physical platform is **AMAP4PEVB Apollo4 Plus BGA Evaluation Board, Rev. 1.0**, explicitly confirmed by the user on 2026-09-28. The current research direction removes BISen's compulsory Stop transition between useful application phases, samples stored energy at fine-grained scheduler boundaries, executes bounded coherent work, and checkpoints only when low energy makes durable progress necessary.

Energy-aware scheduling should preserve inference semantics: energy state changes the amount of exact work performed before the next energy decision, not the model quality, unless a separate approximation experiment is explicitly defined.

## 2. Platform and repository baseline

**CURRENT — inspected 2026-09-28 before documentation edits**

- Working checkout: `/Users/ghart/Documents/Ambiq/neuralSPOT` (the local folder name is unchanged by the remote migration).
- Branch: `main`; inspected HEAD: `1298bc669bb9bbce9368fc059f5d3b38d6a17b7f` (`update`, 2026-09-24).
- `origin`: `git@github.com:Gonzahart/Port_CNN_Intermittent.git`.
- `fork`: `https://github.com/Gonzahart/BISen_CNN_Ambiq.git`.
- `upstream`: `https://github.com/AmbiqAI/neuralSPOT.git`.
- Tracked working tree and index initially clean. Pre-existing untracked work: `apps/bisen_camera_harvest/tools/`, `apps/bisen_camera_harvest/traces/rf_replay/`, `build_harvest_rf_replay.sh`, `flash_harvest_rf_replay.sh`. These are local replay assets/helpers and are excluded from the bridge baseline commit.
- Active reservoir-replay app: `apps/bisen_camera_harvest/`; all 48 app files tracked at inspected HEAD precede the copied tools/traces. Main: `src/bisen_camera_harvest.cc`; build module: `module.mk`.
- Current camera build requires `PLATFORM=apollo4p_evb`; local replay helpers select `AS_VERSION=R4.5.0`. The app README target agrees with the current AMAP4PEVB Rev. 1.0 hardware explicitly confirmed by the user on 2026-09-28. Hardware confirmation is user-reported; no physical inspection was performed during documentation synchronization.
- Application origin remains `0x00018000` in `neuralspot/ns-core/src/apollo4p/gcc/linker_script.ld`; checkpoint reservation is app-local.

| Preserved app | Role documented in its README |
|---|---|
| `apps/bisen_port` | Historical Blue Plus KXR Sobel bring-up |
| `apps/bisen_camera` | Camera/CNN with earlier external VCAP calibration/policy |
| `apps/bisen_camera_vdd` | Camera/CNN governed by internal BATT/VDD sampling |
| `apps/bisen_camera_trace` | Artificial external energy input with independently powered board |
| `apps/bisen_camera_harvest` | Physical reservoir VCAP input and dedicated replay/session mode |

**HISTORICAL:** AMAP4BPXEVB Apollo4 Blue Plus KXR Rev. 2.0, `apollo4p_blue_kxr_evb`, and the Sobel-era handoff describe the earlier bring-up platform. Do not carry its GPIO, ADC, jumper, or power-path assumptions into the current camera/CNN system unless independently verified for AMAP4PEVB. The root README remains general neuralSPOT documentation; app-local READMEs provide RUIC-specific context.

## 3. Validated Apollo4 bring-up history

**VALIDATED/HISTORICAL**

The August bring-up established the core intermittent-computing mechanisms under stable bench power:

- external VCAP sensing and calibrated policy decisions;
- internal Apollo4 temperature sensing;
- bounded/continuous scheduler behavior;
- resumable deterministic Sobel workload;
- retained-RAM progress across deep sleep;
- two-slot atomic MRAM checkpointing with CRC/commit ordering and interrupted-write recovery tests;
- LFRC RTC deep sleep/wake;
- three-bit resistive state-DAC instrumentation for oscilloscope decoding;
- low-energy dirty-context checkpoint triggering with redundant-write suppression.

This stage proved firmware mechanisms and state behavior. It did **not** establish final harvested-energy thresholds or final system energy efficiency.

### Historical bench configuration

**HISTORICAL — DO NOT TREAT AS CURRENT WITHOUT CONFIRMATION**

The validated functional bench setup used approximately:

- 1000 uF external VCAP;
- LM2596 regulation to about 1.9 V;
- GPIO15/ADCSE4 VCAP sensing with the then-characterized divider/board loading;
- provisional Sobel-era scheduler thresholds around the 5.5–8.5 V region.

These values are retained only to interpret old validation artifacts.

## 4. Current camera/CNN implementation and energy policy

**CURRENT — source verified; energy safety remains UNCONFIRMED**

The September framework is substantially implemented in `apps/bisen_camera_harvest/src/`:

- `scan.c` / `sensor.c`: 32 × 32 sequential camera acquisition, GPIO15/ADCSE4, `uint16_t` frame, resumable next-pixel position. External scheduler caps scan work at one complete pixel (`BISEN_CAMERA_SCAN_MAX_UNITS=1`).
- `preprocess.c`: enabled 3 × 3 Gaussian blur, Otsu binarization, vertical flip, and quantization using the deployed model's input scale/zero point. `workload.c` performs preprocessing plus `nn_begin` as one indivisible transition; there are no ADC policy checks inside it.
- `nn_engine.c`, `nn_kernels.c`, `nn_quant.h`, `lenet_weights.h`: custom incremental integer LeNet-style runtime, not a TFLM Invoke path. Seven layers: Conv(5×5,6) → average pool(2×2) → Conv(5×5,16) → average pool(2×2) → FC120 → FC84 → FC10.
- Default `NN_DATAFLOW=NN_DATAFLOW_OUTPUT` means one completed output element per CNN unit. An alternative input-stationary path exists; the replay helper does not select it. `CKPT_APPROX=0` preserves exact checkpoint data by default. Header comments about input-stationary units are not evidence that this build uses that mode.
- `WL_EXTERNAL_DRIVER=1`: `run_bisen_job` calls `pp_sample()` before each scheduler quantum, then runs the workload phase without a compulsory sleep/Stop between useful quanta. Camera versus CNN state is selected by workload phase; energy selects permission and budget.

**VCAP path:** `adc_shared.c` owns ADC0 for GPIO15/SE4 pixels and GPIO16/SE3 reservoir sensing. It uses 12-bit slots, AVG16, 24 MHz HFRC, tracking-cycle setting 63. A policy reading discards one averaged result after mode switching and returns the median of three accepted results. `trace_input.h` converts physical ADC codes to reservoir microvolts; `energy_source.cc` maps those to virtual policy codes. The old divider calibration constants still present in `bisen/bisen_config.h` are not the active harvest conversion.

The local RF replay build helpers explicitly supply calibration code 461 at 5,500,000 µV and code 634 at 7,500,000 µV. These are configured anchors, not a new calibration performed during reconciliation. Divider defaults are 390 kΩ/10 kΩ; switched-divider mode is off, ADC parking is on. Firmware rejects reconstructed VCAP above the configured 8.0 V maximum; this cannot electrically clamp VCAP.

| Configured reservoir VCAP | Current replay action |
|---|---|
| ≥7.30 V, within accepted range | Up to 1000 CNN output elements |
| 6.80–<7.30 V | Up to 500 CNN output elements |
| 6.20–<6.80 V | Up to 100 CNN output elements if not waiting for resume |
| <6.20 V | Stop useful work at boundary, save dirty progress on a qualifying falling transition, wait |
| ≥6.80 V | Allow execution after low-energy wait or storage restore |
| <5.80 V | Critical/sleep-floor classification; not a separate safe-write cutoff |

Virtual boundaries are 2185 / 2333 / 2441 / 2553. They are policy coordinates, not raw GPIO16 codes or voltages at the capacitor. Classification in `bisen_policy.cc` uses the latest reading without retaining the previous band; the application separately enforces a higher resume gate after every low-energy stop. Thus the full application does have post-wait hysteresis even though the classifier comment says “no hysteresis.” Wait polling is 250 ms; zero maximum wait/checkpoint counts mean unlimited in the replay configuration.

**Build distinction:** bare `make ... EXAMPLE=bisen_camera_harvest` defaults to calibration-only, MRAM off, no supplied physical threshold anchors. The current replay recipe is in the pre-existing untracked root helpers, `build_harvest_rf_replay.sh` and `flash_harvest_rf_replay.sh`: calibration=0, MRAM=1, HVR3 magic, AUTORUN=0, AUTOCONTINUOUS=1, offline validation=0, SWO=0, state DAC=1. Defaults retained by this recipe include one-pixel scanning, ADC parking, fixed divider, and high-performance MCU mode (source comment: 192 MHz; actual clock not measured here). GNU toolchain settings include `-O3 -ffast-math`. Build artifacts are not evidence of what is currently flashed.

These thresholds describe the configured functional baseline; final energy-safe limits require bench characterization.

## 5. Current persistence, session control, and state DAC

**CURRENT — source verified**

- `ckpt.h` / `ckpt.c`: two alternating workload slots, each 8,080 bytes = 16-byte header + `NN_LIVE_MAX=8064`. This reservation accommodates the alternative input-stationary maximum too; actual default output-stationary payloads vary by live position.
- Header fields: `uint32_t magic`, `uint32_t seq`, `uint16_t layer`, `uint16_t unit`, `uint32_t crc`. Payload is written first and header last as one 16-byte commit operation. Workload CRC covers live payload, not the header fields; header plausibility/sequence checks are separate.
- Scan and inference share the two slots. Scan records use layer `0xFFFF`, next-pixel position in `unit`, and acquired pixels only (2 bytes per pixel, padded to 16-byte alignment). Inference records store remaining live input, completed output, and any live accumulator band. Default output-stationary mode has no live accumulator band.
- Restore reads newest eligible records, checks payload CRC, and can fall back without programming storage. Completion writes a sequence-ordered header-only tombstone (`layer=0xFFFE`) only if the job had a durable checkpoint; completed results themselves are not persisted.
- App scheduler saves on a work-to-wait transition only for dirty/incomplete progress not already committed. Remaining at low energy does not repeatedly save the same generation. BTN1 stopping and session/tombstone writes are additional MRAM operations; “only low energy causes any MRAM write” would be inaccurate.
- `ckpt_mram.c`: backend checks linker bounds/alignment and uses a 64-byte bounce buffer, calling `am_hal_mram_main_program` with interrupts masked. Logical backend writes and individual HAL calls have separate attempted/success counters. Header-last/CRC is an implemented protocol; this reconciliation does not newly prove power-fail atomicity on silicon.
- `bisen_camera_harvest_checkpoint.ld`: app-local NOLOAD MRAM reservation, 16-byte aligned, 16,160 bytes for workload slots plus 32 bytes for session records = 16,192 bytes total. Addresses depend on linked image; source symbols are authoritative, not a hard-coded address. NOLOAD TCM calibration/validation records remain volatile across full power loss.
- Separate session journal: two 16-byte records (`magic`, `seq`, `armed`, `crc`), CRC covering first 12 bytes. Default workload identity HVR1; replay helper uses HVR3 (`0x48565233`), with corresponding session identity HVC3 (`CKPT_MAGIC XOR 0x1100`).
- Replay mode starts disarmed on fresh storage. BTN0 durably arms continuous jobs; BTN1 stops at a coherent boundary, saves dirty progress, durably disarms, and parks. An armed session survives reset/power loss. RESET is therefore not a stop in this mode. A later BTN0 re-arms/resumes a deliberately stopped session.
- At startup, `restore_pending_workload()` reads/adopts storage before the scheduler's voltage gate; the higher 6.8 V gate controls subsequent useful execution. Do not describe all storage reads as deferred until 6.8 V.

State bus output bits 0/1/2 use GPIO62/63/61 (documented intended header J12.7/.9/.11). `power_policy.cc` maps: 0 sleep/inactive/wait; 1 ADC; 2 camera; 3 compute; 4 nonvolatile write; 5 committed/retired/session update; 6 restore marker; 7 boot/error. These are instrumentation codes, not a full description of policy state. Code 3 also includes preprocessing, and `pp_leave_wait()` briefly emits it. Code 6 for inference is emitted after `infer_init()` has read the checkpoint, so marker duration is not a complete restore-energy interval. Brief code 5/6 events can be missed by coarse scope sampling; a state-3 episode alone is not a reliable completed-frame counter.

## 6. Current physical hardware and power-path plans

**CURRENT — explicitly confirmed by the user on 2026-09-28**

| Installed item | Current configuration |
|---|---|
| Board | AMAP4PEVB Apollo4 Plus BGA Evaluation Board, Rev. 1.0 |
| Reservoir | 10 mF / 10,000 uF |
| Regulator | MP1584EN |
| Regulated rail | Approximately 1.9 V |
| VCAP divider | 390 kΩ / 10 kΩ, continuously connected; no divider switch installed |

**DIRECTION — available parts, NOT installed**

| Available part | Intended status |
|---|---|
| TPS7A0220PDBVR fixed 2.0 V / 200 mA LDO | Planned to replace the MP1584EN soon; not the current regulator |
| TS5A3167DBVR SPST analog switch | Possible future switched-divider experiment only; not adopted |

**Required revalidation:** after replacing MP1584EN with TPS7A0220, existing threshold-safety and energy results do not automatically carry over. V12 must revalidate work/checkpoint/resume thresholds, capacitor energy reserve, discharge behavior, dropout behavior, and relevant energy measurements on the new power path. Preserve the MP1584EN results with their original configuration identity; do not relabel them as TPS7A0220 results.

**HISTORICAL:** the August 1000 uF / LM2596 / Blue Plus Sobel setup remains separate from this current camera/CNN configuration. Earlier discussion of lower-IQ regulation and divider switching was planning, not evidence of installation.

**UNCONFIRMED:** the installed divider-filter capacitor (the harvest README proposes 10 nF), exact camera hardware, and current EVB/J-Link/USB/extra-supply isolation and jumper details still need an experiment-specific bench record. The confirmed board and component values above do not prove those remaining wiring details or energy-safe thresholds. Source defaults and configured calibration anchors remain implementation evidence only.

## 7. Experimental methodology direction

**DIRECTION**

The current paper plan uses repeatable harvested-energy replay and controlled same-platform comparisons.

Planned measurement path:

- replay three recorded energy-envelope traces (`PT1`, `PT2`, `PT3`) using the function generator;
- observe physical VCAP and regulated board VDD;
- decode scheduler state from the three-bit state-DAC;
- measure current using a 1 Ohm high-side shunt;
- disconnect/isolate bench-supply/J-Link/USB paths as required for valid energy runs;
- integrate synchronized voltage/current samples over decoded state intervals.

Primary end-to-end metrics planned:

- completed frames / completion rate;
- total energy and joules per completed frame;
- checkpoint and restore count;
- time/energy in sensing, ADC/policy, compute, save, restore, wait/boot;
- runtime/latency per frame;
- progress lost or recomputed after interruption.

## 8. Comparison plan

**DIRECTION**

The working paper proposes controlled comparisons against:

- monolithic TensorFlow Lite Micro (TFLM);
- a SunSift-style layer-granular policy port on the same Apollo4/common model;
- an ablation of the proposed runtime that restores compulsory Stop transitions;
- Capuchin as a model-deployment baseline, with direct energy/latency comparison only if the same model/input/platform can be ported defensibly.

For V1–V13, preserve controlled variables as applicable: board/revision, power path, reservoir capacitance, regulated rail, divider and ADC calibration, thresholds and work budgets, model/weights/quantization, inputs and acquisition/preprocessing, MCU clock, compiler/build flags, trace and replay scaling/timing, initial capacitor energy, and measurement/probe/shunt configuration. Record firmware identity and every deliberate variation. Change only the intended ablation variable; when a variable must differ, disclose it and limit comparison claims. V12 changes the power path deliberately; V13 changes divider switching only if adopted.

## 9. Validation status and open work

**CURRENT — repository reconciliation completed**

- Camera/CNN app location, build recipe, ADC path, thresholds, checkpoint layout, session behavior, and state mapping are identified above.
- Existing harvest policy-conversion and session-journal host tests passed on 2026-09-28. The policy test uses synthetic anchors; it does not validate bench calibration or the whole scheduler. Session tests exercise persistent arm/disarm and torn-session-write handling in simulated storage.
- Cold scan/CNN restore is implemented. User-reported September cold-restore/replay results are prior functional evidence in the conversation, but a versioned camera validation report with its raw evidence and exact binary identity was not found in the inspected repository. Do not label cold-restore implementation absent, or declare a new on-target pass from source/tests alone.
- App README calibration/cold-restore/replay sections are newer context than the historical `bisen_port/HANDOFF.md`. The user confirmation in section 6 now establishes the current board and listed power components; remaining wiring and measurement details still need bench records.

**OPEN — shared V1–V13 validation program**

Implementation is not validation: incremental CNN scheduling, selective checkpointing, persistent sessions, and 100/500/1000 work budgets already exist. Their physical and end-to-end validation remains open. Build success is not physical validation, and host tests are not hardware validation. Detailed acceptance evidence and controlled variables are recorded in `TASKS.md`.

| ID | Validation scope | Current evidence / remaining work |
|---|---|---|
| V1 | Float/int8 MNIST accuracy and model/export lineage | Deployed weights exist; source model, exporter lineage, held-out accuracy and disagreement evidence remain unverified. No `export_weights.py` was found in the reconciliation inventory. |
| V2 | Uninterrupted vs interrupted/restored inference equivalence | Incremental exact execution and restore are implemented; validate equivalent outputs/progress across representative interruptions using identical inputs. Separate scene changes during scanning from arithmetic correctness. |
| V3 | Live ADC/scheduler thresholds | GPIO16 sensing, calibrated conversion, budgets and resume gate are implemented; validate physical readings and decisions at boundaries. |
| V4 | Production checkpoint → true cold restore | Production MRAM and session persistence exist, with prior user-reported functional evidence; archive exact binary/raw evidence and validate cold restore and interrupted writes at representative scan/CNN positions. |
| V5 | Compulsory-Stop ablation | Controlled comparison remains open; do not infer its completion from the current direct-transition scheduler. |
| V6 | Same-platform SunSift-style layer-granular baseline | Common-model baseline setup and comparative validation remain open. |
| V7 | Same-platform monolithic TFLM baseline | General SDK TFLM support is not a validated RUIC baseline; common-model comparison remains open. |
| V8 | Capuchin feasibility/common-model comparison | Establish whether a defensible common-model/platform comparison is feasible before performance claims. |
| V9 | PT1/PT2/PT3 provenance and replay calibration | Local replay assets exist; PT mapping, provenance, units, timing, scaling and loaded-path calibration remain unconfirmed. |
| V10 | Synchronized state-resolved energy | Acquire synchronized VCAP/VDD/current/state, audit marker coverage, and measure per-state energy and joules/completed frame. |
| V11 | End-to-end runtime under identical intermittent traces | Repeated completion/latency/progress-loss comparison remains open; count results independently of short state markers. |
| V12 | MP1584EN → TPS7A0220 power-path revalidation | Planned LDO is not installed; repeat threshold/reserve/discharge/dropout/energy validation after replacement. |
| V13 | Optional TS5A3167 switched-divider ablation | Switch is available but not installed/adopted; optional future validation only. |

Checkpoint-header integrity and full marker timing coverage remain review items for V4/V10 before broad atomicity or per-state energy claims. No firmware behavior was changed in reconciliation or this synchronization.

## 10. Coordination and documentation caveats

The repository is the file-based handoff. ChatGPT must receive updated files or an identified revision explicitly; no automatic project synchronization has been established.

Source and local build flags establish implementation/configuration, not installed hardware, flashed binary identity, or energy-safe operation. Preserve the older Sobel results as historical, and preserve the paper's unimplemented research directions as DIRECTION.

Known documentation conflicts recorded without editing firmware or existing app READMEs:

- Root replay helpers now live in this repository, but the harvest README still references `Image_Sobel_5969` helper paths; its cold-restore helper references also point outside this checkout.
- The RF-replay README section uses CH4=VCAP / CH1=VDD / CH2=state DAC, while its final generic scope paragraph reverses CH1/CH4. The replay workflow uses the former mapping and normalizes state DAC by board VDD; confirm probes per experiment.
- Legacy divider/voltage constants and self-driving inference comments remain in shared/copied files. Follow the compiled `ES_SOURCE=5` and `WL_EXTERNAL_DRIVER=1` paths, not isolated comments.
- Claims such as “validated thresholds,” exact-resume suite totals in source comments, and MRAM atomicity must be tied to named evidence before inclusion as paper results.
- A coordination-only baseline excludes the already-untracked tools, traces, and build/flash scripts. Versioning those reproducibility inputs is a separate reviewed change.

The approved documentation baseline was committed as `dc4a95f7ca888b747fe6d2304ac938ee8f157023` (`docs: add ChatGPT-Codex RUIC coordination state`). Later repository revisions supersede that baseline; this release-workflow update was prepared against `main` at `a8f8fec65de056dbbfd4ada75e41bbfdc22213da`.

**Release automation decision:** `Port_CNN_Intermittent` is a research firmware mirror, not the upstream neuralSPOT release repository. `.github/workflows/release.yaml` is configured for manual `workflow_dispatch` only; automatic push-to-main and PR triggers have been removed. Manual dispatch still permits inherited release-please and conditional GitHub Pages publication. Release configuration/manifest and `docs.yaml` remain unchanged. This local change takes effect on GitHub once committed and pushed; no hosted workflow run was performed.

