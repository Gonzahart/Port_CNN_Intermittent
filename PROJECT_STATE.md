# RUIC Project State

**Status date:** 2026-09-29
**Project:** Apollo4 intermittent camera-inference framework / BISen-derived RUIC research  
**Purpose of this file:** shared, concise state for ChatGPT and Codex. It intentionally distinguishes verified history from current direction and unresolved timeline conflicts.

## 1. Research objective

**CURRENT/DIRECTION**

Develop and experimentally evaluate an energy-aware intermittent camera-inference framework on Ambiq Apollo4. The current camera/harvest software targets Apollo4 Plus (`apollo4p_evb`); the older Sobel bring-up targeted Blue Plus KXR. The current physical platform is **AMAP4PEVB Apollo4 Plus BGA Evaluation Board, Rev. 1.0**, explicitly confirmed by the user on 2026-09-28. The primary paper direction is the custom incremental CNN engine, controlled same-model speed/energy comparisons, correct completed classifications under intermittent replay, and checkpoint/MRAM energy. The implemented scheduler omits compulsory Stop between useful phases, samples energy at fine-grained boundaries, executes bounded work and selectively checkpoints; these are supporting mechanisms. Dedicated Stop and layer-boundary comparisons are deferred to a later paper.

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

State bus output bits 0/1/2 use GPIO62/63/61 (documented intended header J12.7/.9/.11). `power_policy.cc` maps: 0 sleep/inactive/wait; 1 ADC; 2 camera; 3 compute; 4 nonvolatile write/save; 5 successful-write completion notification; 6 restore marker; 7 boot/error. These are instrumentation codes, not a full description of policy state. Code 5 is emitted after successful checkpoint, session and retirement writes and remains until the next activity; it is not a second checkpoint operation or a fixed-width pulse. Code 3 also includes preprocessing, and `pp_leave_wait()` briefly emits it. Code 6 for inference is emitted after `infer_init()` has read the checkpoint, so marker duration is not a complete restore-energy interval. Internal commit/session accounting and error handling remain intact. This applies to original harvest, IS and nested OS sources; the original replay ARM build passed on 2026-09-29, while IS/OS rebuilds, flashing and bench confirmation remain pending. A state-3 episode alone is not a reliable completed-frame counter.

**CURRENT sleep/wait correction (source and active module settings):** The current
MSP430 `Image_Sobel_5969/main.c` emits an explicit code-5 FRAM checkpoint
pulse after persistence, then uses an energy wait: LPM3 timer sleep or,
below the 2.0 V run floor with LFXT available, LPM3.5 (`PMMREGOFF`) with RTC
wake. These are not separate checkpoint processing states. Apollo4
`wait_for_energy()` also marks code 0 and sleeps/rechecks VCAP, with
`BISEN_CAMERA_WAIT_US=250000` by default. Active harvest modules specify
`BISEN_CAMERA_SCAN_IDLE_MODE=1`, using NORMAL sleep after a working STIMER
self-test or spin fallback. Apollo4 DEEP sleep exists behind mode 2, but is
not the current build selection and its STIMER/HFRC wake and current are not
physically validated. DAC code 0 identifies inactive/wait, not a proven MCU
sleep mode. Apollo4 power-off/standby equivalent remains real loss of board
power with MRAM restore, not an implemented voltage-triggered deep-standby
transition. See [the source-based comparison](docs/RUIC_STATE_DAC.md).

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

**USER-CONFIRMED:** the 10 nF divider-filter capacitor is installed at the GPIO16 junction. **UNCONFIRMED:** exact camera hardware and current EVB/J-Link/USB/extra-supply isolation and jumper details still need an experiment-specific bench record. The confirmed board and component values above do not prove those remaining wiring details or energy-safe thresholds. Source defaults and configured calibration anchors remain implementation evidence only.

**User-reported bench update (2026-09-29):** a shunt and corresponding scope
probes have been added for the MRAM energy test. The planned value is 1 Ω;
actual resistance, placement/polarity, probe mapping and bypass-power
isolation require direct measurement before interpreting energy. A controlled
FG-command profile and shunt-analysis helper are present in the repository;
no V14 bench capture has yet been validated.

**Same-node scope zero diagnostic (2026-09-29):** user reports CH1 and CH4
tips both on the MP1584EN side of the shunt, with matched 1× probes,
500 mV/div and −1.52 V offsets. The provided `v14_zero_01.csv` has 700,000
CH1/CH4 samples over 14 s (20 µs spacing), but no CH2 DAC or CH3 VCAP
columns. Direct CSV analysis gives CH1−CH4 median 0 mV, mean +3.13 mV,
standard deviation 15.90 mV, and 5th/95th percentiles −20/+20 mV;
the recorded voltage levels change in 20 mV steps. This is diagnostic
channel mismatch/quantization evidence, **not** a validated shunt-current
zero for milliamp-scale energy integration. Repeat at matched narrower
volts/div without clipping, then retain the same settings for the active
shunt run. The generic plot labels CH4 as digital GPIO because no state-DAC
channel was selected; the CSV still contains analog CH4 voltage samples.

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

The September 29 downloaded planning update supersedes the older comparison priorities:

- Custom output-stationary (OS) and input-stationary (IS) engines versus same-model Apollo4 TFLM; separately identify reference-int8 and CMSIS-NN configurations and actual kernel fallbacks.
- V0 continuous-power comparison has **summary evidence reviewed in `Data_tables.pdf` (received September 29)**. Board latency, accuracy and earlier-engine recovery results are reported with engine-specific scope; raw captures and exact source/binary/model lineage remain to be audited. Energy is explicitly estimated, not physically measured. See the evidence update below.
- V11 counts correct, committed classifications in fixed PT1/PT2/PT3 windows, with inference-only and full camera-plus-inference results separate.
- V14 measures full production-checkpoint energy and low-level MRAM-program intervals with matched control measurements; board-rail measurements do not isolate the MRAM array itself.
- Capuchin remains conditional on model/operator/arithmetic equivalence and an actual same-Apollo4 port. The deployed RUIC graph uses average pooling; [Capuchin's published support list](https://github.com/leleonardzhang/Capuchin) includes max pooling, not average pooling. This requires source-level feasibility work, not an automatic rejection or silent operator substitution.
- V5 compulsory-Stop and V6 SunSift-style layer-boundary studies are **DEFERRED/out of scope for this paper**. V13 switched divider remains optional, not adopted.

For V0–V14 preserve board/revision, power path, reservoir, rail, divider/calibration, thresholds/budgets, model/weights/quantization, input bytes/order, acquisition/preprocessing, clock, compiler flags, memory placement, logging, trace/scaling/timing, measured initial VCAP, and probe/shunt configuration. Record source/binary/model/input hashes. Pair replay runs and initially collect at least five per runtime/trace, randomizing runtime order where practical. Identical FG commands do not imply identical delivered energy or loaded VCAP. Changed model or precision requires a separately qualified task-level comparison. The detailed gates are in `TASKS.md` and `docs/RUIC_CNN_Port_Equivalence_Checklist.md`.

## 9. Validation status and open work

**CURRENT — repository reconciliation completed**

- Camera/CNN app location, build recipe, ADC path, thresholds, checkpoint layout, session behavior, and state mapping are identified above.
- Existing harvest policy-conversion and session-journal host tests passed on 2026-09-28. The policy test uses synthetic anchors; it does not validate bench calibration or the whole scheduler. Session tests exercise persistent arm/disarm and torn-session-write handling in simulated storage.
- Cold scan/CNN restore is implemented. User-reported September cold-restore/replay results are prior functional evidence in the conversation, but a versioned camera validation report with its raw evidence and exact binary identity was not found in the inspected repository. Do not label cold-restore implementation absent, or declare a new on-target pass from source/tests alone.
- App README calibration/cold-restore/replay sections are newer context than the historical `bisen_port/HANDOFF.md`. The user confirmation in section 6 now establishes the current board and listed power components; remaining wiring and measurement details still need bench records.

**OPEN / DEFERRED — shared V0–V14 validation program**

Implementation is not validation: incremental CNN scheduling, selective checkpointing, persistent sessions, and 100/500/1000 work budgets already exist. Their physical and end-to-end validation remains open. Build success is not physical validation, and host tests are not hardware validation. Detailed acceptance evidence and controlled variables are recorded in `TASKS.md`.

| ID | Validation scope | Current evidence / remaining work |
|---|---|---|
| V0 | Continuous-power engine comparison | Data_tables.pdf summary reviewed: 192 MHz board timings, including merged-r2 OS vs optimized TFLM. Raw evidence/build lineage and uncertainty remain open; no measured energy comparison provided. |
| V1 | Float/int8 MNIST accuracy and model/export lineage | Deployed weights exist; source model, exporter lineage, held-out accuracy and disagreement evidence remain unverified. No `export_weights.py` was found in the reconciliation inventory. |
| V2 | Uninterrupted vs interrupted/restored inference equivalence | Incremental exact execution and restore are implemented; validate equivalent outputs/progress across representative interruptions using identical inputs. Separate scene changes during scanning from arithmetic correctness. |
| V3 | Live ADC/scheduler thresholds | GPIO16 sensing, calibrated conversion, budgets and resume gate are implemented; validate physical readings and decisions at boundaries. |
| V4 | Production checkpoint → true cold restore | Production MRAM and session persistence exist, with prior user-reported functional evidence; archive exact binary/raw evidence and validate cold restore and interrupted writes at representative scan/CNN positions. |
| V5 | Compulsory-Stop ablation | DEFERRED to a later paper; no current implementation task. |
| V6 | Same-platform SunSift-style layer-granular baseline | DEFERRED to a later paper; no current implementation task. |
| V7 | Same-platform TFLM reference/CMSIS-NN variants | Review V0 artifacts before deciding what is missing; pin model/operators/kernels and validate ten scores, accuracy, memory, stable-power and reboot behavior. SDK support alone is insufficient. |
| V8 | Capuchin feasibility/common-model comparison | CURRENT (2026-10-06): `apps/bisen_capuchin` ports Capuchin 76b6eb2 to Apollo4 (+AveragePooling2D) on the same LeNet weights/inputs; VALIDATED host-only/emulator: accuracy parity (98.93 % vs 98.92 %, Tier B), bit-exact port vs independent model, compiled images bit-exact in an M4 emulator. UNCONFIRMED: board timing/energy, native-MSP430 equivalence (X1). Weights are a disclosed int8 reconstruction until `lenet_mnist.keras` is supplied. See `docs/RUIC_V8_Capuchin_Apollo4_Port.md`. |
| V9 | PT1/PT2/PT3 provenance and replay calibration | Local replay assets exist; PT mapping, provenance, units, timing, scaling and loaded-path calibration remain unconfirmed. |
| V10 | Synchronized state-resolved energy | Acquire synchronized VCAP/VDD/current/state, audit marker coverage, and measure per-state energy and joules/completed frame. |
| V11 | End-to-end runtime under identical intermittent traces | Repeated completion/latency/progress-loss comparison remains open; count results independently of short state markers. |
| V12 | MP1584EN → TPS7A0220 power-path revalidation | Planned LDO is not installed; repeat threshold/reserve/discharge/dropout/energy validation after replacement. |
| V13 | Optional TS5A3167 switched-divider ablation | Switch is available but not installed/adopted; optional future validation only. |
| V14 | Production MRAM and full-checkpoint energy | Existing backend has logical-write/byte/HAL-call counters and a 64-byte bounce buffer; focused interval instrumentation, matched controls and physical energy distributions remain to be established. |

Checkpoint-header integrity and full marker timing coverage remain review items for V4/V10 before broad atomicity or per-state energy claims. No firmware behavior was changed in reconciliation or this synchronization.

### 2026-09-29 assessment addendum — implementation planning

Inspected local `main` at `190c4f503` with a clean worktree. See [updated V0–V14 implementation assessment](docs/RUIC_VALIDATION_IMPLEMENTATION_PLAN.md) for proposed changes, prerequisites and sequence. No new firmware or hardware validation is claimed.

- **CURRENT:** `apps/bisen_camera_harvest_IS` and nested `apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS` are paired engine-r2a candidates. Their `src/` trees, including weights, are byte-identical; module settings select IS (dataflow 1) or OS (dataflow 0), both with SIMD 1 and CMAX 6. On 2026-09-29 the missing IS headers were copied from the byte-identical OS/original headers, both packages were switched to app-local linker references, and IS default identity became IS01 while OS remains OS01. Both full ARM replay builds and basic host checkpoint/policy tests pass; no firmware was flashed or physical variant validation performed. The root replay helper still selects the older `bisen_camera_harvest` app, and top-level `make deploy` does not resolve either package's binary name. The CHANGES-r2a engine-replacement note does not match the original app's current engine/module; do not infer deployment or new test passes from it. See `docs/RUIC_R2A_PAIR_RUN.md`.
- **CURRENT correction to earlier inventory:** replay helpers, tools and trace files are now tracked. Their earlier untracked status remains historical. PT1/PT2/PT3 mapping and calibration are still unresolved.
- **V12 constraint:** TPS7A0220 supports recommended input only through 6.0 V (absolute maximum 6.5 V), so it cannot directly accept the current 7.5–8 V reservoir. [TI datasheet](https://www.ti.com/lit/ds/symlink/tps7a02.pdf). The replacement plan must first choose a lower reservoir range with transient margin or a regulator rated for the existing range. Recalibrate policy gates, FG ceiling and energy reserve together; installed hardware remains MP1584EN.
- **Measurement gap:** the capture script estimates reservoir energy change, not shunt-integrated board energy; its default capacitance is 0.1 F rather than installed 0.01 F. Use `--no-energy` for state-only runs, or explicit `--cap-f 0.01` for labeled reservoir-change calculations. Simultaneous charging prevents treating net reservoir change as consumed board energy.
- **DIRECTION:** deterministic input/score checks, interruption harnesses, bounded policy diagnostics, full restore-marker coverage, independent cross-outage completion accounting and synchronized shunt analysis precede final comparative runs. Existing SRAM statistics/offline summaries do not provide durable experiment history across power loss. Changing CNN dataflow requires work/checkpoint energy recharacterization even if numerical chunk budgets remain unchanged.

## 10. Coordination and documentation caveats

The repository is the file-based handoff. ChatGPT must receive updated files or an identified revision explicitly; no automatic project synchronization has been established.

Source and local build flags establish implementation/configuration, not installed hardware, flashed binary identity, or energy-safe operation. Preserve the older Sobel results as historical, and preserve the paper's unimplemented research directions as DIRECTION.

Known documentation conflicts recorded without editing firmware or existing app READMEs:

- Root replay helpers now live in this repository, but the harvest README still references `Image_Sobel_5969` helper paths; its cold-restore helper references also point outside this checkout.
- The RF-replay README section uses CH4=VCAP / CH1=VDD / CH2=state DAC, while its final generic scope paragraph reverses CH1/CH4. The replay workflow uses the former mapping and normalizes state DAC by board VDD; confirm probes per experiment.
- Legacy divider/voltage constants and self-driving inference comments remain in shared/copied files. Follow the compiled `ES_SOURCE=5` and `WL_EXTERNAL_DRIVER=1` paths, not isolated comments.
- Claims such as “validated thresholds,” exact-resume suite totals in source comments, and MRAM atomicity must be tied to named evidence before inclusion as paper results.
- The original coordination baseline excluded then-untracked replay assets; these are now tracked in later commits. Their historical exclusion does not describe the current inventory.

The approved documentation baseline was committed as `dc4a95f7ca888b747fe6d2304ac938ee8f157023` (`docs: add ChatGPT-Codex RUIC coordination state`). Later repository revisions supersede that baseline; this release-workflow update was prepared against `main` at `a8f8fec65de056dbbfd4ada75e41bbfdc22213da`.

**Release automation decision:** `Port_CNN_Intermittent` is a research firmware mirror, not the upstream neuralSPOT release repository. `.github/workflows/release.yaml` is configured for manual `workflow_dispatch` only; automatic push-to-main and PR triggers have been removed. Manual dispatch still permits inherited release-please and conditional GitHub Pages publication. Release configuration/manifest and `docs.yaml` remain unchanged. The change is included in current HEAD `190c4f503`; no hosted workflow run was performed.


### 2026-09-29 downloaded-plan reconciliation

The Downloads copies were planning/scratch exports, not newer live repository snapshots. Their pending migration/bridge tasks, unresolved app/threshold/layout findings and absent-baseline assumptions were not imported over the completed reconciliation. The duplicate port checklists are byte-identical; one canonical copy is retained in `docs/`. The imported statement that the continuous-power comparison has not been measured conflicts with its own latest dated update; the reconciled status is performed, evidence pending review. No hardware installation, firmware deployment or experimental pass is implied by this synchronization.

### 2026-09-29 — Continuous-power benchmark summary received

**Evidence reviewed, not independently rerun:** `/Users/ghart/Downloads/Data_tables.pdf`, dated September 28, seven pages, SHA-256 `4687478028f1878489aa4cf25a49a12957f8bb25a859df53280375f9af9c0a19`. Read all tables and notes. The report labels board measurements at 192 MHz, 100-image timing bench (300 timed inferences per tile for proposed engines; 1,000 for TFLM), full-dataset UART streams, host estimates and historical engine versions separately. Timing images had MRAM writes locked. All energy numbers are explicitly estimates.

- TFLM is reported as neuralSPOT/helia with CMSIS-NN SIMD kernels. Reuse/audit this existing baseline before proposing another port; exact version, flags, operators/fallbacks and model hashes still need source/capture provenance.
- Tables 4/8: merged-r2 OS LeNet tile 8/64/whole = 11.66/9.90/9.60 ms versus TFLM 14.83 ms; Fashion-MNIST = 44.47/39.38/38.07 ms versus TFLM 44.53 ms. These support OS as a performance candidate; whole-inference timing is not evidence of bounded energy checks or harvested throughput. The 0.1% Fashion tile-8 difference is not an established advantage without uncertainty.
- Full-dataset exactness/accuracy results belong to the specifically named older/SIMD/merged-080 variants, not automatically to merged-r2 OS/IS. Equal aggregate accuracy does not prove equal scores or per-image predictions. Reported TFLM score differences from the reference need arithmetic-level explanation; retain the optimized comparator rather than substituting a slower one to force byte equality.
- Table 16: old-engine IS, 349 saves/192 restores, mean save 3.16 ms, programming 2.10 ms, restore 0.86 ms. These are useful historical timing evidence, not current-r2 energy/safety data. Table 17 has older/SIMD recovery campaigns and explicitly no merged-engine campaign.
- Camera Table 20 changes crop ON/OFF between variants; accuracy counts 54/60 and 42/60 are not a controlled engine comparison. Low-bit results are separate precision/size/latency tradeoffs, not evidence for faster exact-int8 replay.
- Recommended next work: pin benchmark sources/artifacts and candidate build; qualify current-r2 exactness and production cold recovery; measure production scheduler overhead at intended budgets plus actual checkpoint energy; then paired inference-only and matched-camera replay comparisons. These are recommendations, not new firmware implementation or validated passes.

**Repository update:** inspection after the usage interruption found `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`, including OS package commit `0ad5ccf18` at `apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS`. This supersedes the earlier inventory that listed only the original and IS apps. Its README identifies engine-r2a as 080-r2 plus `nn_abandon`, cites the same timing values and `RESULT-R2-BOARD-2026-09-28.md`, and explicitly says these are not packaged-application measurements. The referenced board report and acceptance scripts were not found locally. Exact source/binary/capture identity remains unverified. Existing untracked `docs/RUIC_V14_MRAM_Checkpoint_Energy_Plan.md` was read and left unchanged; its proposed bench configuration is not independent proof of installation or new authorization.


### 2026-09-29 — DAC commit marker history and current setting

**HISTORICAL:** user briefly requested suppressing code 5. That edit was not
flashed and has now been superseded by an explicit request to restore the
completion notification. **CURRENT (source only):** all three harvest variants
emit code 5 after successful writes. Checkpoint serialization, thresholds,
session persistence and error paths are unchanged. See [state-DAC mapping and
MSP430 comparison](docs/RUIC_STATE_DAC.md). The inspected historical MSP430
`main.c` emits a timed code-5 pulse after FRAM checkpointing. Ambiq emits
code 5 until the next activity, with no fixed pulse delay. Both treat it as a
notification rather than a second write operation; source does not establish
what was flashed for any archived capture.
**DIRECTION:** checkpoint and restore energy are the next experimental priority
(V14); full restore-marker coverage and controlled measurement harness remain
pending. This marker-only edit does not complete energy or physical validation.

### 2026-09-30 — State-DAC wiring and paired ADC observation

**USER-REPORTED CURRENT BENCH:** the camera state-DAC resistors are GPIO62
(firmware bit 0) 99.3 kΩ, GPIO63 (bit 1) 201 kΩ, and GPIO61 (bit 2) 398 kΩ
to the analog junction. GPIO62-only high gives about 1.07 V at that junction
on a 1.88 V rail. The normalized analog ladder ranks and firmware state codes
are different: ranks 0–7 correspond to codes `[0,4,2,6,1,5,3,7]`. The
`apollo-camera` scope decoder now applies this mapping, preserves raw and
ladder codes, and suppresses observed sub-100 µs junction-edge overshoot by
default. Earlier plots that label ~1.07 V as state 4 must be re-decoded from
raw CSV before using them as checkpoint evidence. CH1 is board-side of the
shunt, CH4 is MP1584EN-output side, CH2 is the DAC junction, and CH3 is live
MP1584EN input when performing VCAP/ADC comparisons. The capacitor/divider
filter is user-confirmed installed; probe positions must still be recorded
per run.

**OPEN V3 CALIBRATION ISSUE:** the latest paired user observation had live
CH3 around 7.26 V and retained ADC mean code 598 (minimum 563, maximum 630,
32 samples); firmware's configured 461@5.5 V and 634@7.5 V anchors predict
about code 613 at 7.26 V. The observed code spread is wider than the CH3
movement in that capture. Code inspection found no proven extraction-scale
bug, but the ADC conversion routine does not check sample-read status/count/
slot. This is an audit finding, not proof of the discrepancy's cause. No
policy thresholds or firmware were changed; paired multi-voltage bench
validation is required before claiming calibrated decisions or V14 energy.

**Interpretation correction (2026-10-01):** the calibration image's printed
`VCAP_nominal_mV=6949` at raw code 598 uses the ideal 1.19 V reference and
390k/10k ratio because `BISEN_HARVEST_CAL_HIGH_UV=0` in the default module
configuration. It does not apply the production replay build's 461@5.5 V /
634@7.5 V fit. Applying that fit to raw 598 gives about 7.084 V, leaving
about 0.176 V versus the approximately 7.26 V live-input observation, not
the full 0.311 V suggested by comparing DMM voltage directly with the
calibration image's nominal display. The raw-code spread and remaining
discrepancy are unresolved; this does not prove that the capacitor changed
the ADC calibration or that the production binary currently flashed uses
these anchors.

**2026-10-01 paired GPIO16 capture (user-supplied, not yet physical validation):**
`v14_adc_gpio16_paired_01.csv` has two state-1 ADC intervals, 0.424736–0.615444 s
and 0.639714–0.830390 s. CH3 mean was 7.2796/7.2663 V and CH4 at GPIO16
mean was 0.180276/0.180006 V for the intervals, consistent with the user's
DMM readings of VCAP 7.32 V and GPIO16 0.181 V. CH4 1 ms block-mean span
was below 1 mV in each interval, comparable to idle; its sample noise was
about 2.9 mV RMS. The two retained SRAM records had 32/32 valid readings,
raw-code means 544/550 and ranges 505–576 / 512–580. A 0.180 V pin level
would nominally correspond to approximately code 620 with 1.19 V/12-bit
scaling; the observed raw means imply approximately 0.158/0.160 V. The code
range would represent roughly 20 mV at the pin, not seen as a sustained
change in CH4. These observations localize the discrepancy to the ADC-side
measurement, reference, sampling, pin/ground relationship or software path;
they do not identify one cause. Do not refit anchors from these records.

**CURRENT diagnostic tool and user-reported bench result (2026-10-01):** the
original `apps/bisen_camera_harvest` calibration image supports optional
`BISEN_HARVEST_ADC_DIAG_COMPARE=1`. Consecutive BTN0 records alternate the
existing per-reading ADC mode switch and a held supply-mode burst, with HAL
FIFO read/count/slot error counters retained for BTN1 readout. The default
flag is 0; full replay/MRAM mode rejects the diagnostic flag. The user flashed
and exercised the diagnostic. In `v14_adc_gpio16_paired_02`, the later
switched/held records were 578/577 at CH4 GPIO16 means 0.180081/0.179814 V;
the earlier retained records were 508/285, but were not paired with this
analog capture. A fresh reset and `v14_adc_gpio16_paired_03` yielded three
switched/held pairs 569/570, 586/588, and 585/589, all 32/32 valid with zero
reported FIFO read/count/slot/drain errors. Across the six captured ADC
intervals, CH4 GPIO16 means stayed 0.180225-0.180539 V and CH3 live VCAP
means 7.2684-7.2810 V. The very low held record 285 did not reproduce.
Mode strategy is not a sufficient explanation; the first pair in the fresh
run was about 17-19 counts lower than later pairs despite stable GPIO16.
The R4.5.0 HAL states that `am_hal_adc_samples_read` applies gain/offset
correction, so the logged codes are HAL-corrected FIFO codes, not independent
uncorrected ADC conversions. Nominal 1.19 V/12-bit scaling predicts about
620-621 counts at the observed 0.180 V GPIO16, still above the later
585-589 codes. A causal ADC diagnosis and multi-voltage calibration are
open. Neither production anchors nor policy thresholds changed; V3 and
the V14 quantitative energy gate remain open.

**2026-10-01 GPIO17 sweep and A–B–A bench evidence (user-supplied):** the
calibration-only `BISEN_HARVEST_DIAG_SUPPLY_PIN=17` image was used with the
divider on GPIO17/ADCSE2. Initial ascending DMM VCAP/GPIO17/code-mean pairs
were 5.81 V/0.143 V/463, 6.21 V/0.153 V/500, 7.31 V/0.180 V/590, and
7.52 V/0.186 V/602.5; 6.81 V was measured but not captured. On the return
leg, 7.30 V/0.181 V produced four-record mean 565, then 6.80 V/0.169 V
produced three-record mean 540. This is monotonic in the correct direction,
but the two 7.3 V groups differed by 25 codes. In a fresh switched/held
A–B–A diagnostic run, 7.30 V/0.181 V produced four-record mean 583.75,
6.80 V/0.168 V produced three-record mean 542, and returning to
7.30 V/0.180 V produced two-record mean 583.5; DMM board VDD was
1.897–1.898 V. All nine records had 32/32 valid readings and zero reported
FIFO read/empty/slot/drain errors, with same-voltage switched/held means
within 1–3 codes. The earlier 565-code group and broad individual 32-read
ranges remain unexplained. GPIO17 is not the production GPIO16 input; no
production calibration, policy threshold, or V14 validation status changed.

**2026-10-01 synchronized GPIO17 capture (user-supplied):**
`adc_gpio17.csv` contains ten state-1 ADC intervals matching ten SWO records
at nominal 7.30 V. During those intervals, scope CH4 GPIO17 mean varied
0.180990–0.181330 V, CH3 live VCAP mean 7.26034–7.27097 V, and CH1 board
VDD mean 1.892055–1.892775 V. The ten HAL-corrected code means ranged
582–595, while within-record extrema span 47–105 codes. All records were
32/32 valid with zero reported FIFO read/empty/slot/drain errors. Scope
sample interval was 20 us; brief analog disturbances or ground/reference
shifts remain possible. Source now has a **calibration-only, opt-in** ordered
32-code SRAM/SWO diagnostic under `BISEN_HARVEST_ADC_DIAG_COMPARE=1` with a
layout-version bump; its selected GPIO17 image builds but has not been
flashed or physically checked. No production threshold or calibration change.

**2026-10-01 ordered GPIO17 bench capture (user-supplied):** The later
`adc_gpio17_2.csv` and on-target SWO show that the ordered-code diagnostic
image was flashed and exercised at nominal VCAP 7.30 V. Four state-1 ADC
windows lasted 190.5–190.8 ms. Their 32-code means were 590.4/585.6/588.5/
591.9 for switched/held/switched/held; every burst was 32/32 valid with zero
reported FIFO read/empty/slot/drain errors. Within-burst standard deviations
were 14.1–17.0 codes and the ordered sequences show no consistent startup
trend. During the windows, CH4 GPIO17 means were 0.18031–0.18037 V, CH3
live VCAP means 7.26046–7.26148 V, and CH1 board VDD means
1.89223–1.89225 V. These are scoped voltage means, not a resolved
sample-by-sample causal correlation. Nominal 1.19 V/12-bit scaling predicts
about 620 codes from the observed pad voltage, versus 589.1 overall observed;
this does not establish the physical ADC reference voltage because HAL gain/
offset correction is applied. The earlier 565-code group and the code spread
remain unexplained. A bench-ground-loop hypothesis documented in `CODEX_LOG.md`
is UNCONFIRMED pending controlled rewiring. Production GPIO16 calibration and
V3/V14 voltage-dependent validation remain open; no anchors or thresholds changed.

**2026-10-01 rewired-ground diagnostic (user-supplied on-target SWO):**
After removing the reported duplicate EVB/source ground path, four GPIO17
ordered bursts averaged 595.0/586.8/587.3/590.8 codes (overall 590.0), versus
589.1 overall before rewiring. Within-burst standard deviations were
14.5/13.4/22.7/15.7 codes, versus 16.5/16.0/17.0/14.1 before. All 32/32
reads succeeded in every burst with zero reported HAL FIFO errors. The
rewiring did not materially improve the code mean or scatter; this argues
against the removed loop as the dominant cause. A new synchronized pin-voltage
and scope capture was not yet provided with this SWO, so exact ADC input
voltage and offset after rewiring remain unconfirmed. Do not infer that the
ADC reference, HAL correction, camera coupling, or scope probing is the cause.
Production GPIO16 and threshold calibration remain unqualified.

**2026-10-01 probe-free GPIO17 result (user-supplied on-target SWO):**
With the rewired ground and oscilloscope probes disconnected, six 32-code
GPIO17 bursts averaged 614.7/616.7/616.7/620.8/618.7/616.7 codes
(overall 617.4); within-burst standard deviations were 8.4/10.3/9.1/
7.6/5.1/7.6 codes. All records were 32/32 valid with zero reported FIFO
read/empty/slot/drain errors. The immediately preceding rewired/probed run
averaged 590.0 codes with aggregate 17.3-code standard deviation; this
probe-free run averaged 27.4 codes higher with aggregate 8.4-code standard
deviation. The comparison strongly implicates connected measurement equipment
or an associated physical-condition change. The user subsequently confirmed
DMM VCAP 7.30 V and an unchanged FG setting for the probe-free run; GPIO17
and board VDD were not measured then. Do not assign the effect
specifically to scope grounding, one probe tip, or ADC reference. Reconnect
probes one at a time, including a ground-only control, before using scoped
ADC/policy traces for V3/V14 claims. Production GPIO16 remains unqualified.
