# Validation record — 2026-09-09

New app: `bisen_camera_trace`. Source baseline: `bisen_camera_vdd` from
neuralSPOT commit `8d637afdab016b3e1008d6d1947c0d634bf94175`.
No board deployment or live FG commands were performed.

## Passed checks

1. Full forced ARM build:

   ```sh
   make -B EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
   ```

2. Calibration-only ARM build, in a separate temporary output directory so the
   default full-app binary remains ready:

   ```sh
   make -B EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
     BINDIR=/tmp/bisen_camera_trace_calibration_build \
     BISEN_TRACE_CALIBRATION_MODE=1 BISEN_CAMERA_ENABLE_MRAM=0
   ```

   No `ckpt_save`, `save_live_checkpoint`, or `run_bisen_job` symbols remain in
   the linked calibration ELF. This is a build check, not a measured MRAM trace.

3. Native tests compile the real `bisen_policy.cc` with `trace_input.h`:
   all 4096 ADC inputs, voltage thresholds immediately below/at/above each
   boundary, ascending/descending policy equivalence, calibration anchors,
   monotonic conversion, and the existing pre-sleep checkpoint gate.
   Both nominal calibration and a synthetic two-point calibration passed.
   The latter tests code behavior; it is not a measured board calibration.

4. Byte comparison with the source app: `scan.c`, `sensor.c`, `infer.c`,
   `workload.c`, `nn_engine.c`, `nn_kernels.c`, `lenet_weights.h`, `preprocess.c`,
   `ckpt.c`, `ckpt_mram.c`, `power.c`, and `bisen/bisen_policy.cc` are unchanged.
   `run_bisen_job` differs only in diagnostic string labels.

5. Existing replay script, eight synthetic band steps, DC/Hi-Z/raw scaling,
   one 16-second cycle and two-second rest, completed with `--dry-run`.
   Output commands match the requested levels and finish with output OFF.
   No instrument was opened or driven.

6. Existing `charge_trace2.txt` preview: 10000 raw samples downsampled to 200.
   The guide's normalized 1.90–2.30 V functional replay yields 74 wait,
   121 low, 3 middle and 2 high command steps. These are command-side
   predictions before ADC calibration and firmware sampling effects.

7. Repository status after addition contains only the new untracked app;
   no previously tracked source files were changed. Full and calibration builds
   produced no app-source warnings. Existing dependency warnings (including
   codec warnings) occur in the forced SDK/library build.

## Full-app artifact

`/Users/ghart/Documents/Ambiq/neuralSPOT/build/apollo4p_evb/arm-none-eabi/apps/bisen_camera_trace/bisen_camera_trace.bin`

- Binary length: 113884 bytes.
- SHA-256: `3ed08369738a545e38efd0deb89309cc21946871114480d802c04f798e03e1d9`.
- Linker-reserved checkpoint symbols: 0x00033ce0–0x00037c00,
  16160 bytes, NOLOAD. These addresses belong to this build only.
- Distinct checkpoint identity: TRC1. Separate source does not imply that
  other applications or checkpoints stay installed when this image is flashed.

```text
text	   data	    bss	    dec	    hex	filename
 111180	   2704	  47292	 161176	  27598	/Users/ghart/Documents/Ambiq/neuralSPOT/build/apollo4p_evb/arm-none-eabi/apps/bisen_camera_trace/bisen_camera_trace.axf
```

Logs are saved under
`/Users/ghart/workspace_v12/Image_Sobel_5969/outputs/bisen_camera_trace/`.

## Not yet validated on hardware

External ADC calibration, exact physical threshold crossings, camera operation
under this new ADC multiplexing path, state-DAC traces, dirty checkpoint/SRAM
resume sequences, and separately induced reset/power-loss recovery. Follow
WIRING.md for the first powered test. The test does not validate real harvested
energy, capacitor depletion, MRAM brownout margin or minimum system current.
