# bisen_capuchin — Capuchin ported to the Apollo4 Plus EVB, benchmarked against the RUIC engine

Capuchin ([upstream](https://github.com/leleonardzhang/Capuchin), pinned at
`76b6eb2`) ported to the AMAP4PEVB without the MSP430 LEA accelerator, running
the **same RUIC LeNet** (same deployed weights, same inputs) as the RUIC engine.
Both engines are linked into one firmware image and timed by one harness.

The experiment design, fairness controls, gates and results so far are in
[`docs/RUIC_V8_Capuchin_Apollo4_Port.md`](../../docs/RUIC_V8_Capuchin_Apollo4_Port.md).
This README covers the build and run steps only.

## Layout

| Path | What it is |
|---|---|
| `upstream/` | Pristine upstream files used by the port (hashes in `UPSTREAM.lock.json`) |
| `src/capuchin/` | Upstream sources + `patches/capuchin-76b6eb2-apollo4.patch` (3 tagged changes), generated `neural_network_parameters.h`, and `capuchin_invoke.c` (upstream `main.c`'s inference sequence as a function) |
| `src/port/` | GCC/Apollo4 shims: forced-include header, `msp430.h`/`DSPLib.h` stand-ins, TI DSPLib generic-C routines, CMSIS-DSP variant |
| `src/bisen_capuchin.cc` | Bench / energy-loop / intermittent firmware |
| `src/ruic_bench.c` | The RUIC engine (OS package sources, unmodified) behind `ruic_infer()` |
| `src/bench_vectors.h` | Generated: 100 MNIST test vectors + host-reference outputs of both engines |
| `msp430/` | Package for the native MSP430FR5994 run: same `MODEL_ARRAY` (upstream header format), AveragePooling patch, cross-check vectors, optional `main_xchk.c` |
| `tools/` | Encoder wrapper, scale sweep, NumPy reference model, host builds, emulator check, provenance check |
| `results/` | Host and emulator results (no board results yet) |

The port changes to upstream are exactly three, tagged in the source:
`[PORT P1]` MSP430 DMA block copy → CPU copy (inline loop, or `memcpy` with `CAPUCHIN_COPY_LOOP=0`); `[PORT A1]` AveragePooling2D (layer class 6; upstream has max pooling only);
`[PORT H1]` an empty-by-default per-layer timing hook. `tools/verify_provenance.py` proves that
`src/capuchin/` is upstream plus that patch.

## Build (from the neuralSPOT root)

```sh
make -j8 -B EXAMPLE=bisen_capuchin PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
     BINDIRROOT=/tmp/bisen-capuchin-k1 CAPUCHIN_KERNEL=1 BENCH_MODE=0
```

Image: `$BINDIRROOT/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.bin`, linked at the
standard application origin `0x18000` (default neuralSPOT linker script; no MRAM reservation, no
non-volatile writes). Flash it with J-Link as in `docs/RUIC_R2A_PAIR_RUN.md`.

| Variable | Values |
|---|---|
| `BENCH_MODE` | `0` bench: bit-exact self-check of 100 vectors × both engines, then interleaved timing (SWO) · `1` energy loop: spin reference window (DAC 0) then a burst of `BENCH_LOOP_COUNT` inferences (DAC 3); use gross burst energy / count · `2` intermittent: restart-from-scratch job stream, completion on the DAC |
| `BENCH_ENGINE` | modes 1/2: `0` Capuchin, `1` RUIC engine (in mode 2 this is the RUIC engine **without** checkpoints, an ablation) |
| `CAPUCHIN_KERNEL` | `1` LEA path served by TI DSPLib's generic (non-LEA) C — TI's reference for the LEA operation; equality with the native LEA is gate X1 · `2` LEA path served by CMSIS-DSP `arm_mat_mult_q15` (Cortex-M4 DSP extension) · `0` upstream non-MSP C path |
| `CAPUCHIN_MODEL_IN_SRAM` | `0` weights `const` in MRAM, like the RUIC weights (default) · `1` SRAM ablation |
| `CAPUCHIN_LAYER_PROFILE` | `1` prints a per-layer cycle breakdown in mode 0 |
| `CAPUCHIN_COPY_LOOP` | `1` `[PORT P1]` copies as an inline word loop (default) · `0` as `memcpy` |
| `BENCH_REPS`, `BENCH_PRINT_ALL`, `BENCH_LOOP_COUNT`, `BENCH_IDLE_MS`, `BENCH_EVENT_HOLD_US`, `BENCH_STATE_DAC`, `BENCH_MCU_LOW_POWER` | see `module.mk` |

Clock and power bring-up are copied from the harvest apps: `ns_power_config(&ns_development_default)`,
then `AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE` (192 MHz).

## Mode 0 output (SWO)

```
CHECK,capuchin,bitexact=100/100,correct=100/100,crc32=........
CHECK,ruic,bitexact=100/100,correct=100/100,crc32=........
CHECK,PASS
STAT,capuchin_invoke,n=1000,min=..,p05=..,median=..,p95=..,max=..,mean=.. cycles,median_us=..
STAT,capuchin_load_input,...
STAT,ruic_nn_run,...
DONE,valid
```

Timings are valid only after `CHECK,PASS`. `capuchin_invoke` includes staging the input into Capuchin's
`input_buffer` (`capuchin_load_input`, also reported alone); `ruic_nn_run` includes the engine's own input copy.
Engine order alternates every inference. Median = mean of the two middle values (n even); p05/p95 are nearest-rank.

## State DAC (GPIO 62/63/61 = bit 0/1/2, same pins and pad setup as the harvest apps)

| Code | Mode 0 | Mode 1 | Mode 2 |
|---|---|---|---|
| 7 | boot, and the whole self-check | boot | boot |
| 3 | Capuchin inference | inference burst | job in progress |
| 2 | RUIC inference | — | — |
| 0 | between inferences | spin reference window (busy-wait + SWO log; **not** idle) | — |
| 5 | end, valid | — | completed, correct label |
| 6 | end, INVALID | — | completed, wrong label |

Code 4 never appears: this firmware performs no non-volatile writes. **Codes 5 and 6 do not mean what they mean in the
harvest apps** (there: NV write committed / context restore); decode captures of the two firmwares separately.

## Host-side reproduction (no board needed)

```sh
cd apps/bisen_capuchin/tools
pip install tensorflow tf_keras fxpmath unicorn pyelftools   # TF_USE_LEGACY_KERAS=1 is set by the scripts
./reproduce_host.sh /path/to/mnist-idx-dir                  # official IDX .gz files, MD5-checked
```

`reproduce_host.sh` regenerates both headers, the scale selection, the vectors, the 10,000-image
accuracy/agreement table and the provenance check. `emu_check.py <axf>` runs the compiled ARM image's
inference functions in a Cortex-M4 emulator against the same expected outputs.
