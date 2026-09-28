# bisen_camera_harvest with the new engine (engine-r2a, input-stationary) -- 2026-09-28

This is your whole `apps/bisen_camera_harvest` folder from commit 1298bc669, with only the
inference engine changed. Your checkpoint code (`ckpt.c`, `ckpt.h`, `ckpt_mram.c`), weights
(`lenet_weights.h`), energy policy and thresholds, linker script, tests and app sources are
byte-for-byte what you have.

## What changed

| file | change |
|---|---|
| `src/nn_engine.c`, `src/nn_engine.h`, `src/nn_kernels.c`, `src/nn_kernels.h`, `src/nn_quant.h` | replaced by the new engine |
| `src/nn_build.h`, `src/nn_simd.h`, `src/nn_wo.h` | new engine files |
| `module.mk` | one line added before `bindirs += $(local_bin)`: `pp_defines += NN_DATAFLOW=1 NN_SIMD=1 NN_MAX_CONV_IN_C=6` |
| `CHANGES-r2a.md` | this note |

The new engine is our merged engine (the one tested on the board on 09/28) plus
`nn_abandon`, which your app calls. It runs the input-stationary loop order with SIMD
instructions (SMLAD) inside each unit. The SIMD code is in `src/nn_simd.h` and
`src/nn_kernels.c`.

To go back to output-stationary, change `NN_DATAFLOW=1` to `NN_DATAFLOW=0` in `module.mk`.
To turn SIMD off, set `NN_SIMD=0`.

## How to build and flash

1. Keep a copy of your current `apps/bisen_camera_harvest` (rename it), then put this folder
   in its place. The folder name must stay `bisen_camera_harvest`, because `module.mk` and the
   linker script use that path.
2. Build with your current calibration flags, the same command you use now:
   `make EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 <your flags>`
3. Flash with `make ... deploy` as usual (it loads the `.bin` at 0x18000).

## What was checked before sending (PC and ARM compiler, not on a board)

- Your `ckpt.c` with the new engine at every stop point (run, save, wipe RAM, restore,
  finish): 6,951 positions input-stationary and 24,279 output-stationary, 0 failures,
  with ASan and UBSan. A control that damages the restored input was caught.
- Same outputs: the new engine gives the same score bytes as your engine on all 100 test
  images, with your weights, in both loop orders.
- Your `harvest_policy_test` and `session_journal_test` pass with the new engine.
- This folder builds in 4 configurations (full MRAM, offline validate, default calibration,
  API test). The only warnings are your two existing unused variables.

Not checked yet: running on the board, real MRAM saves, and energy. The first run on your
rig is also the first board run of this engine's checkpointing, so please watch that the
saves and restores behave.

## What to expect compared to your current build

| | your engine (output-stationary, scalar) | new engine (input-stationary, SIMD) |
|---|---|---|
| stop points per inference | 8,094 | 2,318 |
| largest checkpoint | about 4.9 KB | about 7.9 KB (fits your 8,080-byte slots) |
| inference, our 100-image bench, 8 units per call | 23.86 ms | 14.90 ms |

The times are from our LeNet bench at 192 MHz. Your camera loop and chunk sizes will give
different totals, so measure your current build and this one with the same calibration and
the same input to compare energy.
