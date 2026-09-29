# RUIC CNN baseline port and equivalence checklist

**Working plan · 29 September 2026.** Target: AMAP4PEVB Apollo4 Plus BGA EVB Rev. 1.0. This is a feasibility and acceptance checklist, not a claim that any port or physical comparison has passed. The continuous-power engine comparison has reportedly been run; enter its conditions and results when available. Compulsory-Stop and layer-boundary ablations are deferred to a later paper.

## 1. What is being compared?

Use **one frozen network and one frozen input sequence** to compare inference implementations. A second CNN architecture can test generality later, but cannot establish an engine speedup by itself. Count only correct, committed classifications during a fixed PT1/PT2/PT3 replay window for the intermittent outcome. Report preloaded-input inference and camera-plus-inference separately. The camera path must be identical for all runtimes.

| Candidate | Role in this paper | Priority and condition |
|---|---|---|
| RUIC input-stationary (IS) and output-stationary (OS) implementations | Proposed engines; report each implementation and the exact build used | Primary |
| TensorFlow Lite Micro (TFLM), reference int8 kernels | Standard embedded interpreter baseline | Primary; same network and board |
| TFLM with CMSIS-NN kernels | Strong optimized ARM Cortex-M4 baseline | Strongly recommended if the exact model operators map to optimized kernels; identify any reference fallbacks |
| Capuchin port | Cross-framework model-generator baseline | Conditional; first pass the model and arithmetic feasibility gates below |
| Direct CMSIS-NN application without TFLM | Optional kernel-library baseline | Lower priority; requires our own graph schedule and memory planner, so disclose application code and avoid calling it a ready-made runtime |
| microTVM ahead-of-time compiled model | Optional compiler baseline | Later only if import, operator coverage and Apollo4 integration work without changing the model |

TFLM and TFLM+CMSIS-NN are two **configurations of one runtime**, not two independent intermittent-computing systems. CMSIS-NN documents int8/int16 operators including convolution, fully connected and average pooling and a DSP path for Cortex-M4; verify that the exact TFLM build actually selects those kernels. [TFLM](https://github.com/tensorflow/tflite-micro), [CMSIS-NN](https://github.com/ARM-software/CMSIS-NN), [TFLM memory management](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/docs/memory_management.md).

## 2. Common freeze sheet: fill this before porting

- [ ] Record RUIC git commit, build target and compiler version; model-header SHA-256; source training/export artifact and exporter script/commit. Do not assume that similarly named LeNet models share weights.
- [ ] Export a layer manifest: input shape/layout; each layer's type, output shape, filter/kernel, stride, padding, pooling type, activation, weight and bias shape, weight order, and any fused operation. Include exact ten output logits or scores and the predicted-class rule (softmax, if present, need not run when argmax is equivalent).
- [ ] Record per-tensor weight and activation type; scales/zero-points or fixed-point Q format; accumulator width; rounding, clipping, saturation and bias scaling. Quantization differences must be visible rather than hidden under “same LeNet.”
- [ ] Freeze image bytes and order, labels, preprocessing (32×32 camera versus MNIST conversion), and a hashed test-vector set. Include ordinary, low-contrast, saturation, near-tie and all-zero/all-high input vectors.
- [ ] Freeze board revision, core clock/power mode, supply and power path, memory placement, optimization/LTO/architecture flags, input delivery, logging and instrumentation. Record exact code and model bytes in flash, static RAM, peak runtime RAM and stack method.
- [ ] Choose two explicit comparison tiers: **Tier A, strict numeric:** same topology, parameters, input tensor and numerically matched arithmetic/output; **Tier B, task equivalent:** same topology/training weights but arithmetic or quantization differs, with separate accuracy and disagreement reporting. Tier B supports a system tradeoff comparison, not a pure engine speedup claim.

## 3. Capuchin feasibility gates (stop early if any critical gate fails)

Capuchin's published repository describes a Keras-model Python encoder producing `neural_network_parameters.h` and an MSP430FR5994 C implementation. Its README lists Conv2D, dense, **MaxPooling2D**, flatten, dropout and LeakyReLU, but does **not** list AveragePooling2D. The current RUIC task backlog flags average pooling for inspection. Confirm the live RUIC graph before declaring incompatibility. Its documented MSP430 memory constraints are native-platform restrictions, not Apollo4 limits. [Capuchin repository](https://github.com/leleonardzhang/Capuchin).

| Gate | Concrete action and evidence | Pass condition |
|---|---|---|
| C1: model source | Locate the training/export model or reconstruct the frozen graph and map every array from the deployed header to named layers; compare per-layer hashes/shapes | Same topology and weights demonstrable; no substituted sample model |
| C2: operators | Compare RUIC manifest with Capuchin encoder and C operators, including average versus max pooling, padding, flatten order, ReLU and bias | Every operator has the same semantics; additions to Capuchin are separately documented and tested |
| C3: numeric representation | Trace encoder's fixed-point conversion, scale, accumulator, right shift, rounding and overflow; implement explicit weight conversion from the same source weights | Tier A if tensor arithmetic/output matches; otherwise Tier B with quantified disagreement and accuracy; no silent claim of int8 equivalence |
| C4: portable implementation | Isolate MSP430/CCS/LEA/FRAM dependencies from the layer math; compile an Apollo4 application locally without changing RUIC's bootloader/application origin or shared BSP | Real Capuchin generated layers execute on the AMAP4PEVB; no MSP430 result substituted |
| C5: resource fit | Measure generated parameters, intermediates, stack, checkpoint/restart state, linker memory map and SRAM peak | Fits the same configured board without off-board computation or hidden serial transfer |
| C6: output validation | Run identical input bytes; dump each layer output and ten final scores from host reference and Apollo4; calculate max absolute layer error and label disagreement; test complete held-out set | Predeclared numeric tolerance and task accuracy gate met; no systematic layer mismatch or changed labels left unexplained |
| C7: stable-power benchmark | Time warmed inference around the complete invocation; separate model setup and input copy, use same board clock and matched measurement window; capture board-rail energy | Reproducible latency/energy with all build and measurement settings recorded |
| C8: interruption behavior | Audit whether the port itself resumes an interrupted inference. If not, restart that invocation after reboot and count wasted work; label any added restart wrapper and its energy | End-to-end comparison represents the actual implementation; recovery additions are separately attributed |
| C9: replay validity | Run same PT1/PT2/PT3 commands, measured initial VCAP and final power path; record loaded VCAP/VDD, reset timing and completed correct outputs | Repeated, paired measurements with identical workload and clearly reported numeric tier |

**Decision after C1–C3:** If the live RUIC model has average pooling and Capuchin lacks it, an equivalent average-pool implementation is necessary. Replacing average pooling with max pooling changes the CNN and fails strict model equivalence. If the original model weights or conversion path cannot be reconstructed, stop the direct speedup claim; Capuchin can still be discussed as related work or a separately labeled task-level comparison. If C4–C6 fail, do not spend bench time on C7–C9.

## 4. TFLM and optimized-kernel gates

- [ ] Generate a `.tflite` graph from the frozen source model; inspect FlatBuffer operator list, tensor shapes, quantization parameters and weights. Compare the `.tflite` input and per-layer outputs against the deployed RUIC representation. If direct export changes quantization, label the result Tier B.
- [ ] Register only required operators. Confirm `AllocateTensors()` succeeds, then record tensor arena used (head/tail if recording build), model storage, stack and flash. Time setup separately from repeated `Invoke()` calls. [TFLM memory management](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/docs/memory_management.md).
- [ ] Build two identifiable variants from pinned commits: TFLM reference kernels and TFLM+CMSIS-NN. Use the same `.tflite` bytes, vectors and invocation boundaries. Log which Conv2D, pooling and fully connected kernel is linked/selected; measure any fallback separately. CMSIS-NN follows TFLM's integer quantization convention, but the linked implementation and output equality must still be checked on this model. [CMSIS-NN](https://github.com/ARM-software/CMSIS-NN).
- [ ] For each variant, compare all ten outputs, predicted class, disagreement rate and held-out accuracy with the RUIC engine; explain numerical differences by layer. Correct classifications alone do not prove numeric equivalence.
- [ ] For intermittent replay, first record what unmodified TFLM does after true power loss. Treat each failed invocation's work as lost unless retained progress is demonstrated. Any common camera/restart wrapper must be described, measured and applied consistently; it must not quietly give TFLM RUIC's incremental cursor.

## 5. Measurements and reporting gates for every admitted baseline

- [ ] Stable rail: same board, input set and clock. Report setup time, steady-state invocation median/spread, cycles, VDD×I integrated energy, flash and peak SRAM; note any optimization that applies only to one baseline.
- [ ] Replay: use the same fixed-duration PT1/PT2/PT3 command sequence, start from measured matched VCAP, randomize run order where practical, and run at least five paired trials per runtime and trace initially. Capture loaded VCAP and VDD since equal generator commands can produce different reservoir histories.
- [ ] Report correct completed classifications, attempts, partials at window end, incorrect completions, resets, lost/recomputed work, checkpoint/restore count, total input energy and energy per correct completion. State the denominators and treat a zero-completion run without dividing by zero.
- [ ] Keep inference-only preloaded-input results distinct from full camera-plus-inference results; use identical camera firmware/settings for the latter.
- [ ] Pin source/model hashes and save raw traces, map/link reports, per-layer output captures and analysis scripts. Physical claims require on-target measurements; host tests and builds are gates only.

## 6. Go/no-go record to fill for each baseline

| Field | Entry |
|---|---|
| Runtime and exact upstream commit | Pending |
| RUIC commit, board and power path | Pending |
| Frozen model/input hashes | Pending |
| Operator coverage / any changed semantics | Pending |
| Tier A or B, with numeric evidence | Pending |
| Held-out accuracy / prediction disagreement | Pending |
| Flash / peak SRAM / stack | Pending |
| Stable-rail time and energy | Pending |
| Real power-loss behavior | Pending |
| PT1/PT2/PT3 correct completions and uncertainty | Pending |
| Decision: direct ranking, qualified tradeoff or related work | Pending |

## Suggested implementation order

1. Audit the already performed continuous-power run against Sections 2, 4 and 5; fill missing provenance without rerunning solely for paperwork.
2. Finish/pin TFLM reference and TFLM+CMSIS-NN on the same Apollo4 model and vectors. These are the strongest conventional runtime comparisons.
3. Inspect Capuchin C1–C3 as a short feasibility spike before committing to its port. Proceed only if an honest numeric tier and operator mapping are possible.
4. Once the final power path and replay calibration are fixed, run paired PT1/PT2/PT3 tests. A changed regulator means new threshold/energy qualification.
5. Consider direct CMSIS-NN or microTVM only if they add a distinct claim that the preceding baselines cannot answer.
