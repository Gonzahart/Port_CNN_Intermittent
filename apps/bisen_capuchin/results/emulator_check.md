# Host-side build and emulator evidence (no board results)

Generated 2026-10-06 in the cloud workspace: arm-none-eabi-gcc 13.2.1 (Ubuntu package;
the bench toolchain is Arm GNU 15.3.Rel1 — rebuild there before flashing), AmbiqSuite R4.5.0,
neuralSPOT upstream 308c4e631 + this app, `-O3` neuralSPOT defaults.

## Builds

| Build | text / data / bss (bytes) | .bin SHA-256 (prefix) |
|---|---|---|
| k1 | 332460 / 72 / 125084 | f0498a12d55ee483 |
| k2 | 332716 / 72 / 128868 | 93894c915595f561 |
| k0 | 331144 / 72 / 121296 | 293dc869a4d2f5ea |
| k1mc | 332076 / 72 / 125084 | 0fe894d87778c4ea |
| k2mc | 332332 / 72 / 128868 | 03d5269130907114 |
| k0mc | 331068 / 72 / 121296 | 705e822d76ad7717 |
| k1sram | 208944 / 123584 / 125084 | 3a9dcef298bc1fdf |
| k1prof | 333256 / 72 / 125156 | 6bae16b351cf7205 |
| k2prof | 333512 / 72 / 128940 | 91b614833f77c4d8 |
| k0prof | 331940 / 72 / 121368 | 18ec635fc194a885 |
| m1cap | 254544 / 72 / 124660 | cd6c397e9a0e8469 |
| m1ruic | 198564 / 72 / 53016 | 3dd72b451e2fd8ad |
| m2cap | 253236 / 44 / 102128 | c74bb2b182fa21f0 |
| m2ruic | 196744 / 44 / 29460 | 84337da221363718 |

Options: k1/k2/k0 = `CAPUCHIN_KERNEL` 1/2/0 (bench mode); `mc` = `CAPUCHIN_COPY_LOOP=0`; `sram` =
`CAPUCHIN_MODEL_IN_SRAM=1`; `prof` = `CAPUCHIN_LAYER_PROFILE=1`; m1/m2 = `BENCH_MODE` 1/2 with
`BENCH_ENGINE` cap (0) or ruic (1). All 14 link with no warnings from this app's or the engine's sources.
`-fwrapv` applies to `dsplib_sw.c` only.

## Bit-exactness of the compiled ARM code (Cortex-M4 ISA emulator)

Unicorn 2.1.4 (QEMU), Cortex-M4 with the DSP extension. `tools/emu_check.py` loads the `.axf`, calls
`capuchin_load_input`/`capuchin_infer` and `ruic_infer` for each of the 100 bench vectors and compares
all ten scores with the host-reference values embedded in `src/bench_vectors.h`. Not a timing model.

```
k1/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k2/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k0/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k1mc/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k2mc/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k0mc/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
k1sram/apollo4p_evb/arm-none-eabi/apps/bisen_capuchin/bisen_capuchin.axf: capuchin bit-exact 100/100; ruic bit-exact 100/100
```

## Executed instructions per inference (vector 0; emulator count, NOT cycles)

`tools/emu_profile.py` on the `CAPUCHIN_LAYER_PROFILE=1` images; the `[PORT H1]` hook marks layer ends.
LeNet work: 416,520 MACs (conv1 117,600 · conv2 240,000 · fc 58,920).

| Layer | k1 dsplib-sw | k2 cmsis-dsp | k0 cpu-ref | k1, memcpy copies |
|---|---:|---:|---:|---:|
| conv1 | 3.479 M | 2.599 M | 3.067 M | 3.693 M |
| avgpool1 [PORT A1] | 0.084 M | 0.084 M | 0.084 M | 0.083 M |
| conv2 | 7.052 M | 5.564 M | 6.151 M | 7.499 M |
| avgpool2 [PORT A1] | 0.030 M | 0.030 M | 0.030 M | 0.030 M |
| flatten | 0.007 M | 0.007 M | 0.007 M | 0.006 M |
| fc1 | 1.101 M | 1.494 M | 2.239 M | 0.967 M |
| fc2 | 0.201 M | 0.137 M | 0.476 M | 0.189 M |
| fc3 | 0.017 M | 0.011 M | 0.040 M | 0.016 M |
| **total** | **11.97 M** | **9.93 M** | **12.09 M** | **12.48 M** (2.43 M inside `memcpy`) |

RUIC engine (engine-r2a OS, SIMD), same vector: **1.384 M** instructions for `ruic_infer` (includes its input copy).
Capuchin/RUIC executed-instruction ratio: 7.2× (k2) to 9.0× (k1 with `memcpy`). Convolution is 76–88 % of Capuchin's
instructions; the added average pooling is about 1 %. This predicts the direction and rough size of the
board result; it is not one. MRAM wait states, caches and multi-cycle instructions differ between the
engines, so only DWT cycles and shunt energy from the board are reportable.

## Static memory (k1 image, from the symbol table)

| | Capuchin | RUIC engine |
|---|---:|---:|
| Model constants in MRAM | 123,516 B (`MODEL_ARRAY`, int16 Q5.10 + layer descriptors) | 64,666 B (int8 weights, int32 bias/mult/shift, layer table) |
| Inference RAM (static) | 77,568 B (upstream fixed buffers: 2×32 KiB layer buffers, padding, filter, LEA scratch, I/O) | 5,888 B (`nn_ctx_t`) |

Capuchin's buffer sizes are upstream constants written by its encoder, kept unchanged. Stack is not included for either.
