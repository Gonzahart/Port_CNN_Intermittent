# BISen camera direct-VDD capture — AMAP4PEVB Rev. 1

`bisen_camera_vdd` is a separate neuralSPOT application for the Apollo4 Plus
BGA Evaluation Board Rev. 1 (`PLATFORM=apollo4p_evb`). It combines the working
camera/LeNet workload with direct `VDD_MCU` energy sensing through the Apollo4
Plus internal ADC `BATT` channel. It does not modify or link
`apps/bisen_camera` or `apps/bisen_port`.

## Full-capture scope

The default image enables:

- GPIO15/ADCSE4 photodiode acquisition and the existing resumable LeNet job;
- internal `BATT` (`VDD/3`) sampling through the shared ADC0 owner;
- energy-dependent 100, 500, and 1000-unit CNN scheduling;
- one-pixel camera scan boundaries so VDD is sampled between coherent pixels;
- retained SRAM progress while waiting for more energy;
- falling-edge, dirty-progress-only two-slot MRAM checkpoints;
- boot-time scan or CNN restore with CRC/torn-write rejection;
- one tombstone only when a completed job has a live durable checkpoint;
- low-power timed polling while VDD is below the work threshold;
- the 3-bit weighted-resistor state bus on GPIO62/63/61;
- one bounded job after reset, then park; BTN0 enters continuous-job mode and
  reset exits continuous mode.

The secure bootloader, `basic_tf_stub`, `apps/bisen_port`, and
`apps/bisen_camera` are outside this app and are unchanged.

## Direct-VDD policy used for this capture milestone

Scheduling compares the raw internal-BATT ADC code. This avoids moving a state
boundary with the residual error of a global code-to-voltage fit. The DMM or
scope measurement at J7.1/J7.2 remains the physical voltage reference.

| Decision | Raw-code boundary | Calibration anchor |
|---|---:|---:|
| 1000 CNN units | `>= 2553` | about 2.20 V DMM |
| 500 CNN units | `>= 2441` | about 2.10 V DMM |
| 100 CNN units | `>= 2333` | about 2.00 V DMM |
| Wait; checkpoint once on a dirty falling transition | `< 2333` | below about 2.00 V |
| Restart/restore after a durable interruption | `>= 2441` | about 2.10 V DMM |
| Low-power floor | `< 2185` | about 1.90 V DMM |

There is no band hysteresis. The separate restart/restore boundary is the
Apollo equivalent of the MSP430 reference's distinct start/continue decision.
These values are provisional behavior-capture thresholds anchored to this
board's offline calibration. They are not final harvested-energy thresholds or
proof that every MRAM write completes under an arbitrary RF-trace slew rate.
Those claims require workload-energy and brownout-margin characterization.

AmbiqSuite R4.5.0 defines the nominal readback conversion as:

```text
nominal_VDD_mV = ADC_code * 3 * 1190 / 4096
```

The firmware prints that nominal value for diagnostics, but makes the policy
decision from raw code.

## State-DAC codes

| Code | Meaning |
|---:|---|
| 0 | Sleep / inactive / energy wait |
| 1 | VDD ADC |
| 2 | Camera / pixel sensing |
| 3 | CNN compute |
| 4 | MRAM write |
| 5 | Checkpoint committed or retired |
| 6 | Context restore |
| 7 | Boot / error |

## Validated bench topology

| Function | Connection |
|---|---|
| Direct supply/harvester output | J7.3 (`VDD_EXT`) |
| Return | Board ground at J3.8 |
| Measured MCU rail | J7.1 or J7.2 (`VDD_MCU`) |
| Camera pixel ADC | J9.10 / GPIO15 / ADCSE4 |
| State bus | J12.7/.9/.11 / GPIO62/63/61 |
| BTN0 | GPIO18 |

The weighted state-DAC uses GPIO62 through about 99.3 kOhm, GPIO63 through
about 201 kOhm, and GPIO61 through about 398 kOhm into the common scope node.
J3.3-J3.4 remain jumpered and SB3 remains factory-closed, so energy results
must be labelled EVB-as-configured. Do not infer `VDD_MCU` from generator or
bench-supply setpoint; measure at J7.1/J7.2. Keep the established 2.20 V
measured-rail ceiling for this milestone.

## Build, deploy, and view

From the neuralSPOT root:

```sh
make -B \
  EXAMPLE=bisen_camera_vdd \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0

make -B deploy \
  EXAMPLE=bisen_camera_vdd \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0

make view \
  EXAMPLE=bisen_camera_vdd \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0
```

The default reset path runs one bounded job and parks. Press BTN0 after it
parks to enter continuous mode. A reset exits continuous mode and again runs
one bounded job.

## First full multi-state bench capture

1. Power at a measured `VDD_MCU` near 2.10 V, flash the image, and open SWO.
2. Confirm the startup banner says `direct-VDD full capture`, source
   `internal BATT (VDD/3)`, `two-slot app-local MRAM`, and the raw-code table
   above. Stop if it says calibration mode or retained RAM.
3. Confirm a reset-bounded job completes at stable 2.10 V and the CNN output is
   plausible. This is the known-good flashable check before changing voltage.
4. Start a long scope acquisition of J7.1/J7.2 and the state-DAC node. Press
   BTN0 to enter continuous mode.
5. Hold near 2.20 V to show code 1/2/3 activity and the 1000-unit band, then
   near 2.10 V for the 500-unit band, and near 2.00 V for the 100-unit band.
6. Lower slowly through 2.00 V while a job is active. With dirty progress, the
   expected one-time sequence is state 4 then state 5, followed by state 0.
   Remaining below the threshold must not repeatedly rewrite the checkpoint.
7. Raise above the raw-code restart boundary (about 2.10 V). Work should
   continue from SRAM without state 6 if VDD never collapsed.
8. In a separate power-fail test, first create a durable checkpoint, then let
   VDD fall far enough to reset the MCU. Recharge above about 2.10 V. Expected
   sequence: state 7 at boot, state 6 on MRAM restore, then state 2 or 3 from
   the saved position. This is the recovery demonstration.

Use a deliberately slow bench ramp for the first checkpoint capture. A fast
RF replay is a later validation because the time from threshold crossing to
brownout must be measured against the actual MRAM transaction duration.

## Returning to calibration-only mode

The earlier offline diagnostic remains available explicitly:

```sh
make -B \
  EXAMPLE=bisen_camera_vdd \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0 \
  BISEN_CAMERA_VDD_CALIBRATION_MODE=1 \
  BISEN_CAMERA_ENABLE_MRAM=0
```
