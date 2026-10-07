# V8 — Capuchin on the Apollo4: port, comparison design and evidence

Prepared by Claude (Cowork) on 2026-10-06 at the user's explicit request ("build a Capuchin port for the
Ambiq board … paper-ready"). Repository read at local `main` `b2d650ceb`; cloud builds used AmbiqAI neuralSPOT
`308c4e631` (the merge base; `extern/`, `make/`, `neuralspot/` unchanged since) plus the OS package's engine sources.
App: `apps/bisen_capuchin`.
Status labels follow `AGENTS.md`. **No board run has been made. Every result below is host or emulator
evidence; none is physical validation.**

## 1. The argument this enables

Your colleague's MSP430 port and this port together give a 2×2 design:

| | MSP430FR5994 (Capuchin's native platform) | Apollo4 Plus EVB (RUIC's platform) |
|---|---|---|
| **Capuchin** | native, LEA on (colleague) | this port: `apps/bisen_capuchin` |
| **RUIC engine/runtime** | colleague's port | existing OS/IS apps |

- **Same-platform rows (V0/V7-style):** on each MCU, both systems run the same network, the same weights and the
  same inputs, built by the same compiler at the same optimisation level (per-file differences in §5). On the Apollo4 they are even in **one image** and
  timed by one harness, interleaved. The difference is the inference system, not the platform.
- **The link between the two columns (gate X1):** the Apollo4 port is fed the *same* `MODEL_ARRAY` as the native
  build, and both are checked against the same expected Q5.10 logits. If native == port on the cross-check
  vectors, the Apollo4 Capuchin is provably the same program as native Capuchin. Cross-platform differences in
  Capuchin's numbers are then platform effects, not port artefacts.
- **The interaction:** if RUIC's advantage over Capuchin holds on *both* MCUs, it is a property of the
  framework, not of the Apollo4's MRAM/M4F or of the MSP430's FRAM/LEA. That is the claim reviewers find hardest
  to dismiss, and only the factorial design supports it.

What the comparison does **not** support: a ranking that mixes cells across platforms (for example "RUIC on
Apollo4 vs Capuchin on MSP430" as an engine comparison), or a pure engine-arithmetic speedup claim (the numeric
tier is B — section 3.4).

## 2. What was ported

**Upstream:** <https://github.com/leleonardzhang/Capuchin> at `76b6eb223f1b6da27520064a8f52b9df6fa4cab5`.
Pristine copies of the files used are in `apps/bisen_capuchin/upstream/` (hashes in `UPSTREAM.lock.json`).

### 2.1 What upstream Capuchin is

A Python encoder (`encoder.py`) flattens a Keras `Sequential` model into one `int16` array, `MODEL_ARRAY`: layer
codes, shapes and Q5.10 weights (Fxp 16-bit word, 10 fractional bits, truncation toward zero, saturation). The
MSP430 C code decodes that array layer by layer (`decoder.c`) and calls layer routines (`layers.c`,
`matrix_ops.c`). On the FR5994, `IS_MSP` selects the LEA paths: convolution is per-filter, per-channel im2col
followed by a q15 matrix multiply on the LEA (`filter_im2col`); dense layers use `msp_matrix_mpy_q15` on the LEA
when operands fit 1892 words, splitting rows recursively otherwise. Everything else (shifts, channel sums,
bias, activation, pooling, flatten, layer copies) is CPU code. **Capuchin has no intermittence support:** buffers
are FRAM-persistent but there is no progress cursor; after a reboot `main()` runs the model from layer 0.

### 2.2 Mapping from MSP430 to Apollo4

| MSP430 mechanism | Used by Capuchin for | Apollo4 port |
|---|---|---|
| LEA `MPYMATRIXROW` via DSPLib `msp_matrix_mpy_q15` | every conv MAC and every dense MAC | `CAPUCHIN_KERNEL=1`: TI DSPLib's own generic C (32-bit accumulate, `>>15`, saturate) · `=2`: CMSIS-DSP `arm_mat_mult_q15` on the M4 DSP extension · `=0`: upstream non-MSP C path instead of the LEA path |
| DSPLib `msp_matrix_shift_q15` (positive shifts) | Q15→Q10 rescale | TI's code unchanged (it is plain C on LEA devices too) |
| DMA channel 0 (`dma_load`) | im2col gathers (mostly 5 words), layer copies | CPU copy of the same words `[PORT P1]`: inline word loop (default) or `memcpy` (`CAPUCHIN_COPY_LOOP=0`); measure both, report the faster |
| FRAM `#pragma PERSISTENT/LOCATION` | weights, activations | weights `const` in MRAM (same placement as the RUIC weights); activations in TCM SRAM |
| LEA RAM (`DSPLIB_DATA`) | 1892-word scratch | ordinary SRAM, same size |
| 16-bit `int` (MSP430) | index/overflow arithmetic | reviewed for this model: index products stay below 65,536 (largest 49,040 in `matrix_multiply_reduce`); int16 narrowing wraps on both compilers; `-fwrapv` on `dsplib_sw.c` for TI's wrapping int32 accumulator |

Upstream C source changes are exactly three (the encoder is extended by a wrapper, §3.2), tagged in the code and collected in
`patches/capuchin-76b6eb2-apollo4.patch` (7 hunks); `tools/verify_provenance.py` proves
`src/capuchin/ = upstream + patch`:

1. `[PORT P1]` `dma_load` → CPU copy of the same n words. MSP430 DMA block transfers also stall the CPU, so a CPU copy is the
   equivalent; the default inline loop avoids a library call per 5-word im2col row (memcpy variant kept for the board to decide).
2. `[PORT A1]` **AveragePooling2D** (layer class 6). Upstream supports MaxPooling2D only; the RUIC LeNet uses
   average pooling, and substituting max pooling would change the network (checklist C2). The new functions mirror
   `maxpooling`/`maxpooling_filters` line for line; the reduction is an `int32` sum divided by the window size with
   C integer division. It is CPU code on both platforms. Cost: ≈1 % of Capuchin's executed instructions
   (`results/emulator_check.md`). The same change is supplied for the native build as
   `msp430/capuchin-76b6eb2-avgpool.patch`.
3. `[PORT H1]` an empty-by-default hook at the end of each decoded layer, for the per-layer cycle breakdown.

Everything else GCC needs (prototypes TI's compiler supplied implicitly, `<string.h>`, stand-ins for `msp430.h`
and `DSPLib.h`) is in `src/port/`, force-included, and adds no arithmetic. Upstream `main.c`'s inference sequence
is reproduced verbatim in `src/capuchin/capuchin_invoke.c`. Upstream's own non-MSP configuration does not link as
shipped (`dma_load` exists only under `IS_MSP`); the port supplies it for `CAPUCHIN_KERNEL=0`.

### 2.3 Which Capuchin number to report

All three kernels compute Capuchin's arithmetic (kernels 1 and 2 bit-identically; kernel 0 is upstream's other
code path). Report all three; designate the **fastest kernel that passes the self-check** as "Capuchin on Apollo4"
in headline comparisons. Kernel 2 is the closest analogue of the native configuration (vendor DSP library on the
SoC's MAC hardware, the same instruction class the RUIC engine's `NN_SIMD` path uses), which answers the
"you took away its accelerator" objection directly.

## 3. Model equivalence (checklist C1–C3)

### 3.1 Topology

`Input 32×32×1 → Conv2D 6@5×5 ReLU → AvgPool 2 → Conv2D 16@5×5 ReLU → AvgPool 2 → Flatten (HWC) → Dense 120 ReLU →
Dense 84 ReLU → Dense 10`, from the descriptor table in `lenet_weights.h` (SHA-256 `f2d631c3…4a138f`). Capuchin's
flatten emits channels-last order, matching the RUIC FC1 weight order; conv weights are re-laid
`[oc][ky][kx][ic]` → Keras HWIO and transposed by Capuchin's own encoder.

### 3.2 Weights — CURRENT, with a disclosed reconstruction

`lenet_mnist.keras` is still not in the repository. The port therefore reconstructs a float model from the
deployed int8 header (`tools/ruic_model.py`): weights `q_w·M·s_out/s_in`, biases `q_b·M·s_out`, with `M` from the
per-channel (mult, shift). The header does not separate `s_w` from the hidden activation scales; for a
conv/dense + ReLU + average-pool network those are a free positive factor per layer, and every choice gives the
same classifier in exact arithmetic (confirmed: float accuracy is identical at every scale in the sweep, `float_train_acc` in
`results/scale_sweep.json`; 98.93 % on the test set). The choice matters
only to Capuchin's fixed-point range, so it was **selected in Capuchin's favour on training data only**
(`tools/capuchin_scale_sweep.py`, first 10,000 MNIST training images, Capuchin's exact integer arithmetic; ties →
smaller range):

| act_range R | 0.5 | 1 | 2 | 3 | **4** | 6 | 8 | 12 | 16 | 24 | 32 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| train acc. | 98.97 | 99.25 | 99.27 | 99.26 | **99.28** | 99.28 | 99.27 | 99.26 | 99.28 | 82.05 | 84.27 |

Q5.10 is accuracy-neutral over a 16× range of scalings and collapses beyond it (16-bit wraparound). Latency
and energy do not depend on R (no data-dependent control flow), so R affects only the accuracy column.
**When Bobby supplies `lenet_mnist.keras`, regenerate with `capuchin_export.py --keras` and report that model
instead;** the reconstruction then becomes a footnote.

The encoder is upstream `encoder.py` itself (hash-checked), loaded unmodified; the only extension is the class-6
branch for AveragePooling2D. Upstream `encode()` silently stops at an unsupported layer, which would emit a
truncated model; the wrapper refuses that instead. `MODEL_ARRAY` SHA-256 `0b60b805…cee7a36b`, 61,758 words, no
saturated weights. Both headers (MSP430 template, byte-for-byte as `export_model()` writes it, and the Apollo4
flavour) carry the same values.

### 3.3 Inputs

MNIST test digits centred in 32×32 with a 2-pixel zero border. This reproduces the engine's reported held-out
accuracy (host: 9,892/10,000; `Data_tables.pdf`: 9,891 — the RUIC output has 9 exact top-score ties, the likely
cause; UNCONFIRMED). RUIC input: `p − 128` (bit-exact with `preprocess.c` quantization). Capuchin input:
Capuchin's documented encoding, `Fxp(p/255, 16, 10)`, as a 256-entry table.

### 3.4 Arithmetic and accuracy — Tier B (host, 10,000 images, VALIDATED host-only)

| System | Arithmetic | Accuracy |
|---|---|---|
| RUIC engine-r2a OS | int8 per-channel, int32 accumulate, TFLite requantisation | 98.92 % |
| Capuchin, LEA path (kernels 1, 2) | Q5.10 int16, q15 MAC, `>>15` sat, `<<2`/`<<5` wrap, int16 adds | 98.93 % |
| Capuchin, CPU path (kernel 0) | Q5.10, per-product `>>10`, int16 accumulate | 98.89 % |
| Float reconstruction | float32 | 98.93 % |

Label agreement RUIC vs Capuchin (LEA) 99.95 %; discordant pairs 2 vs 3, McNemar exact p = 1.0. The two systems are
statistically indistinguishable in accuracy, so the comparison is about time, energy and intermittent behaviour
at equal quality. Numeric audit over all 10,000 inferences: 145.18 M q15 multiply outputs, **0** 32-bit accumulator
overflows, **0** saturations, **0** left-shift wraps — which is also why kernels 1 and 2 agree bit for bit.

## 4. Verification evidence so far

| Check | Result | Label |
|---|---|---|
| Port = upstream + 3 tagged changes | `verify_provenance.py`: all PASS | VALIDATED (source) |
| Ported C vs an independent NumPy model of Capuchin written from upstream | bit-exact, 10,000/10,000, LEA and CPU paths | VALIDATED (host) |
| Compiled ARM images (k1, k2, k0) in a Cortex-M4 ISA emulator vs host expected outputs | Capuchin 100/100 and RUIC 100/100 bit-exact per image | VALIDATED (emulator; not timing) |
| Builds: k0/k1/k2, profile, SRAM ablation, modes 1 and 2 × both engines | 14/14 link, no warnings from this app or the engine | CURRENT (cloud GCC 13.2.1; your toolchain is 15.3) |
| Deterministic regeneration (`reproduce_host.sh`) | headers and vectors byte-identical | VALIDATED (host) |
| Executed instructions/inference (emulator) | Capuchin 9.93 M (k2, loop copy) – 12.48 M (k1, `memcpy` copy) vs RUIC 1.38 M; conv 76–88 % of Capuchin's | sanity check only, **not a result** |
| Static memory | Capuchin 123.5 KB model constants + 77.6 KB RAM buffers vs RUIC 64.7 KB + 5.9 KB | CURRENT (symbol table) |
| Board self-check, timing, energy, X1, replay | not run | open |

## 5. Fairness controls and threats to validity

| Threat | Control |
|---|---|
| Different build/clock/harness | One image, both engines; same GCC and `-O3` (neuralSPOT defaults). Disclosed per-file differences: `-fwrapv` on `dsplib_sw.c` (TI's accumulator semantics), `-fno-tree-loop-distribute-patterns` on `matrix_ops.c` (keeps the P1 copy a loop), and kernel 2 links CMSIS-DSP from neuralSPOT's prebuilt `libCMSISDSP-m4-gcc.a`; 192 MHz HP mode with the harvest apps' bring-up; DWT cycles; engine order alternates every inference; timings only after a bit-exact self-check of all 100 vectors × both engines |
| Accelerator removed | Kernel 2 serves exactly the operation Capuchin offloads to the LEA with CMSIS-DSP on the M4 DSP extension; kernel 1 is TI's own reference code for that operation; report all and headline the fastest |
| Port slowed Capuchin | Diff is 3 tagged changes; the DMA→CPU copy is the only change on a hot path, offered in two forms (loop / `memcpy`) so the faster is reported. Capuchin's structural costs (per-filter-per-channel im2col, the zero column it carries for the LEA, layer copies) are upstream design and exist natively — not "fixed", because then it would not be Capuchin. Per-layer breakdown explains where time goes |
| Memory placement | Weights in MRAM for both (Capuchin native: FRAM). Activations in SRAM — faster than native FRAM, i.e. favourable to Capuchin. SRAM-weights ablation (`CAPUCHIN_MODEL_IN_SRAM=1`) bounds the effect |
| Weight provenance | Disclosed reconstruction; scale chosen for Capuchin on training data; accuracy plateau shows insensitivity; swap in the Keras model when available |
| Operator addition | AvgPool mirrors upstream max-pool code; ≈1 % of executed instructions; same patch used natively |
| Numeric tier | Tier B stated; accuracy equal (McNemar p = 1.0); no "pure arithmetic speedup" claim |
| Cross-platform conflation | X1 gate; only same-platform cells are ranked |
| Intermittence semantics | Capuchin keeps no progress, natively or here; FRAM-resident activations without a cursor give no resume. Mode 2 reproduces exactly that, with no extra checkpointing or energy gating. The RUIC engine *without* checkpoints (mode 2, engine 1) is an ablation separating engine speed from the runtime's progress preservation |

## 6. Bench protocol (Apollo4)

Record Git HEAD, worktree diff, full make command, `.bin` SHA-256, `.map`, SWO log, scope/shunt settings for every
capture. Build with your 15.3 toolchain; the cloud build only established that the code links. Flash at `0x18000`
with J-Link as in `docs/RUIC_R2A_PAIR_RUN.md`.

| Step | Build | Acceptance / output |
|---|---|---|
| B1 continuous timing | `BENCH_MODE=0`, `CAPUCHIN_KERNEL=1`, then `2`, then `0` | `CHECK,PASS`; `STAT` lines. **Harness consistency:** `ruic_nn_run` median should be close to the V0 OS-whole 9.60 ms (≈1.84 M cycles); investigate a large gap before using any number |
| B2 breakdown | `CAPUCHIN_LAYER_PROFILE=1` (each kernel) | `LAYER` lines; explains the ratio |
| B3 ablations | `CAPUCHIN_MODEL_IN_SRAM=1`; `CAPUCHIN_COPY_LOOP=0` | MRAM weight-fetch share; copy form (keep the faster as the headline) |
| B4 energy/inference | `BENCH_MODE=1`, `BENCH_ENGINE=0` (best kernel) and `=1` | integrate shunt current × VDD over DAC-3 bursts; E/inf = gross burst energy / count; ≥5 bursts each, same supply/probe setup, **USB/J-Link unplugged** (SB3 is closed — TASKS V14, 2026-10-06). The DAC-0 window is a busy-wait **spin** reference (plus the SWO log), not an idle baseline — do not subtract it |
| B5 cross-platform X1 | colleague (section 7) | native == port on all cross-check vectors |
| B6a intermittent, same firmware | `BENCH_MODE=2`, engine 0 (Capuchin) vs engine 1 (RUIC engine, no checkpoints) | same PT1/PT2/PT3 commands, measured initial VCAP, ≥5 paired runs, randomized order; count DAC-5 events per fixed window; completions, rate, energy per correct completion; zero reported as zero. Identical job definition on both arms |
| B6b vs the RUIC runtime | needs a matching RUIC arm (open) | the harvest apps' code 5 means "NV write committed" and code 6 "context restore" — **not** a job completion — and their job includes camera scan, ADC and energy-gated waits. Comparing their DAC-5 counts with mode 2's is invalid. Required first: an inference-only, preloaded-vector mode of the RUIC runtime with a job-completion marker defined as here (e.g. a dedicated completion GPIO), or a camera path for Capuchin. Not implemented; needs authorisation in `TASKS.md` |

Mode 2 has no SWO (no USB on harvested power). This firmware's codes 5/6 mean job completed (correct/wrong) — different from the harvest apps — so decode its captures with its own table. Codes 5/6 are held 20 µs (`BENCH_EVENT_HOLD_US`) so a scope sees
them; the same hold applies to both engines.

## 7. Gate X1 — native MSP430 equivalence (with your colleague)

Hand over `apps/bisen_capuchin/msp430/`:

1. `git apply capuchin-76b6eb2-avgpool.patch` on upstream `capuchin-MCU` (applies cleanly; verified).
2. Copy `neural_network_parameters.h` (upstream template, `MODEL_ARRAY` SHA-256 `0b60b805…`) and
   `capuchin_vectors.h` (16 vectors, the first 16 of the Apollo4 set, with expected logits).
3. Optionally use `main_xchk.c` (upstream `main.c` loop body, plus comparison; P1.0 marks inference).
4. Pass: `xchk_pass == 16`. Use the **same header** for the colleague's native Capuchin timing/energy runs.

`LENGTH_OF_INPUT + model = 62,782 words`, within Capuchin's documented 68 K limit; conv1 output 4,704 ≤ 16,384.
If X1 fails, the cause is in the LEA's rounding/accumulation (DSPLib's generic C is TI's reference, but LEA
hardware equivalence is not documented); record the mismatching vector/logit and treat Apollo4 Capuchin as
"Capuchin's arithmetic as specified by TI's reference" rather than "identical to native".

## 8. What to expect, and how to read it

The emulator executes Capuchin in 9.93–12.48 M instructions against RUIC's 1.38 M (7.2–9.0×). Most of Capuchin's
cost is convolution bookkeeping, not MACs: each of the 16 conv2 filters re-gathers all 6 input channels through
im2col one output row at a time, and every LEA multiply also computes a second output column that Capuchin discards
(zero inputs in the dense layers, unused scratch in the convolutions). Expect the board ratio in this range but **do not quote it until B1/B4 exist** — MRAM wait
states and cache behaviour can move it either way.

For intermittent operation the expectation is qualitative and testable: a Capuchin job completes only when one
power-on interval delivers boot energy plus a whole inference. Its completion count should drop to zero once
on-time falls below that threshold, while the RUIC runtime keeps completing jobs; the RUIC no-checkpoint ablation
shows how much of the gap is speed and how much is progress preservation. Measure the thresholds from B4 and the
replay; do not derive them from the configured 6.2/6.8/7.3 V anchors.

## 9. Paper text (draft, fill numbers after B1–B6)

> **Baseline.** We compare against Capuchin [ref], a neural-network model generator for 16-bit MSP430 MCUs, on
> its native MSP430FR5994 and ported to our Apollo4 Plus. The port uses Capuchin's encoder (its layer encoders
> unmodified, plus one dispatch branch for average pooling) and its C sources (commit 76b6eb2) with three documented changes: DMA block copies become CPU copies, an average-pooling layer
> mirroring Capuchin's max-pooling code is added because our LeNet uses it, and an inactive profiling hook. The
> operation Capuchin offloads to the MSP430's LEA is executed by TI DSPLib's reference C code or by CMSIS-DSP on
> the Cortex-M4 DSP extension; we report the faster. Both systems run the same weights and inputs; the Apollo4
> port produces logits identical to native Capuchin on [16] cross-check inputs [only after gate X1 passes; otherwise
> state that the port follows TI's reference implementation of the LEA operation]. Capuchin's Q5.10 arithmetic and
> our int8 engine reach 98.93 % and 98.92 % on the MNIST test set (McNemar p = 1.0), so differences below are
> not bought with accuracy.

Table templates: (i) per-platform latency median [p05–p95], energy/inference, flash, SRAM, accuracy;
(ii) per-layer cycles; (iii) PT1/PT2/PT3 correct completions per window, energy per correct completion, for
RUIC runtime, RUIC engine without checkpoints, and Capuchin.

## 10. Open items

- Obtain `lenet_mnist.keras` (and the quantizer) from Bobby; regenerate with `--keras`; re-run `reproduce_host.sh`.
- B1–B6 on the board; X1 with the colleague.
- The same Apollo4 image could host an IS build of the RUIC engine (`RUIC_ENGINE_FLAGS`) for an OS/IS/Capuchin
  three-way table; not done.
- Camera-plus-inference end-to-end runs for Capuchin are not implemented (inference-only from preloaded inputs).
- B6b needs an RUIC-runtime arm with the same job definition and completion marker (see §6).
- `msp430/main_xchk.c` has not been compiled in CCS; its FRAM fit is unverified.
- Mode 0 holds DAC code 7 through the multi-second self-check; mode 1's DAC-0 window is a spin reference.
