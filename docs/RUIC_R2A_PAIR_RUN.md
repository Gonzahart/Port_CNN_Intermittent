# Paired r2a output- and input-stationary board runs

The two candidate applications are `apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS`
(OS01) and `apps/bisen_camera_harvest_IS` (IS01). Their complete `src/`
trees, including the quantized weights, are byte-identical. Their module
files select `NN_DATAFLOW=0` or `1`; both select `NN_SIMD=1` and
`NN_MAX_CONV_IN_C=6`. OS01 and IS01 use distinct checkpoint/session magic.
The original `apps/bisen_camera_harvest` and its replay helper remain a
separate, older reference build.

## Build identity

From the repository root, build both with the **same** bench settings:

```sh
for app in bisen_camera_harvest_IS bisen_camera_harvest_OS/bisen_camera_harvest_OS; do
  make -j8 -B \
    EXAMPLE="$app" PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
    BINDIRROOT="/tmp/ruic-r2a-${app##*/}-build" \
    BISEN_HARVEST_CALIBRATION_MODE=0 BISEN_CAMERA_ENABLE_MRAM=1 \
    BISEN_CAMERA_AUTORUN=0 BISEN_HARVEST_AUTOCONTINUOUS=1 \
    BISEN_HARVEST_OFFLINE_VALIDATE=0 BISEN_ENABLE_SWO_LOGGING=0 \
    BISEN_ENABLE_STATE_DAC=1 BISEN_CAMERA_MAX_CHECKPOINTS=0 \
    BISEN_CAMERA_MAX_WAIT_CYCLES=0 \
    BISEN_HARVEST_SUPPLY_PIN=17 BISEN_HARVEST_CAL_PIN=17 \
    BISEN_HARVEST_CAL_LOW_CODE=476 BISEN_HARVEST_CAL_LOW_UV=5604455 \
    BISEN_HARVEST_CAL_HIGH_CODE=655 BISEN_HARVEST_CAL_HIGH_UV=7705526 \
    BISEN_HARVEST_CRITICAL_UV=5800000 BISEN_HARVEST_WORK100_UV=6200000 \
    BISEN_HARVEST_WORK500_UV=6800000 BISEN_HARVEST_WORK1000_UV=7300000
done
```

The binary for each build is under
`/tmp/ruic-r2a-${app##*/}-build/apollo4p_evb/arm-none-eabi/apps/$app/bisen_camera_harvest.bin`.
Record the Git HEAD, worktree diff, full command, SHA-256, linker map and
scope/FG settings with each capture. The package `EXAMPLE` names do not
match the shared `local_app_name`, so the top-level `make deploy` selector
does not resolve these images. Flash an explicitly selected `.bin` with
J-Link at the existing application origin `0x18000`; inspect the generated
J-Link command file before running it. Never use the root
`flash_harvest_rf_replay.sh` to claim an r2a run: it builds the original app.

## Matched experiment

1. Flash OS01 and record its image hash. Disconnect J-Link/USB for energy
   measurement. Establish the same measured initial VCAP, rail voltage,
   camera stimulus, probe/shunt calibration and FG replay used for IS01.
   Press BTN0 to arm a fresh session. Acquire VCAP, board VDD, calibrated
   shunt voltage/current and state DAC on a common timebase.
2. Run a stable-power deterministic-input check and a forced checkpoint/
   true power-loss/restore check for OS01 before treating it as qualified.
   Then acquire repeated RF replay and short, high-rate write windows.
3. End that session, preserve raw captures, flash IS01, and repeat the
   same checks and FG profile. IS01 starts as a separate checkpoint/session
   identity; do not switch variants during a restore trial. Flashing may
   overwrite prior MRAM records despite the identity guard.
4. Repeat enough paired trials to report a distribution, not one selected
   trace. Alternate run order if bench drift is visible. Record which
   variant, image hash and session produced every file.

Compare completed inference result and all ten scores on identical inputs;
then compare time, board-input joules per completed job, checkpoint count,
payload bytes, write duration/energy and cold-restore behavior. For each
state-4 to state-5 event, identify workload checkpoint versus session or
retirement write before aggregating. A code-5 notification indicates the
write returned successfully; it is not a second write interval. Current
code-6 placement misses part of inference restore, so a restore-energy
number requires the planned marker correction or a separate storage-call
trigger.

An OS unit and an IS unit are different amounts of work. Equal 100/500/1000
numeric budgets or equal DAC pulse counts are **not** equal computational
progress. Compare at equal input/job, equivalent logical checkpoint phase
and progress, and actual measured voltage. The present policy thresholds
were calibrated for the installed MP1584EN setup and are not yet proven
energy-safe for both r2a dataflows. Requalify them if the power path changes.

Build and basic host-test success establish only that the candidates are
ready for controlled board checks; they do not establish numerical
equivalence, cold recovery or physical energy advantage.
