# BISen camera direct-VDD bring-up — AMAP4PEVB Rev. 1

`bisen_camera_vdd` is a separate neuralSPOT application for the Apollo4 Plus
BGA Evaluation Board Rev. 1 (`PLATFORM=apollo4p_evb`). It is derived from the
working `bisen_camera` integration, but this initial milestone is deliberately
limited to measuring `VDD_MCU` through the Apollo4 Plus internal ADC `BATT`
channel. It does not modify or link `apps/bisen_camera` or `apps/bisen_port`.

## Initial milestone scope

The default image:

- keeps the existing camera pin map and shared ADC0 owner;
- switches ADC0 from the GPIO15/ADCSE4 photodiode slot to the internal `BATT`
  slot and back;
- captures up to sixteen 32-sample supply points on BTN0 while J-Link USB is
  disconnected;
- retains those raw-code statistics in volatile `NOLOAD` TCM and prints them
  on BTN1 after J-Link USB is reconnected;
- reports the nominal SDK conversion for each retained point;
- reports the read-only `MCUCTRL->ADCBATTLOAD` value;
- remains in the bounded calibration diagnostic; it never starts the workload.

Camera acquisition, CNN inference, direct-VDD policy thresholds, low-power
cycling, checkpoint restore, and MRAM programming are not enabled in this
milestone. The source contains the inherited camera workload so later stages
can be enabled incrementally, but the build fails if the diagnostic is turned
off before direct-VDD policy work is implemented.

## SDK basis

AmbiqSuite R4.5.0 defines:

```text
AM_HAL_ADC_SLOT_CHSEL_BATT = internal voltage divide-by-3 connection
AM_HAL_ADC_VREFMV          = 1190 mV
AM_HAL_ADC_SAMPLE_DIVISOR  = 4096 for 12-bit samples
```

The nominal diagnostic conversion is therefore:

```text
VDD_MCU_mV = ADC_code * 3 * 1190 / 4096
```

This is only the SDK transfer function. Runtime policy thresholds will use a
fit from raw code to a simultaneous DMM measurement on the physical board.

The `BATTLOAD` register controls an optional battery load resistor. This app
does not write or enable it; the register is printed only as a safety audit.

## Validated bench topology

The current bench wiring is:

| Function | Connection |
|---|---|
| Bench supply positive | J7.3 (`VDD_EXT`) |
| Bench supply return | Board ground at J3.8 |
| Measured MCU rail | J7.1 or J7.2 (`VDD_MCU`) |
| Camera pixel ADC | J9.10 / GPIO15 / ADCSE4 |
| State bus | J12.7/.9/.11 / GPIO62/63/61 |
| BTN0 | GPIO18 |
| BTN1 | GPIO19 |

With J3.3–J3.4 jumpered, the measured `VDD_EXT`, `VDD_MCU`, and camera row and
column supplies tracked within a few millivolts. `VDD_5V` remained near zero.
SB3 remains in its factory-closed state, so future energy results must be
labelled as EVB-as-configured until the final power PCB removes that ambiguity.

Do not infer rail voltage from the bench supply setpoint. The present setup
showed an unexplained 0.22–0.27 V difference between the programmed setpoint
and J7.3. Use a DMM at J7.1/J7.2, verify that the supply is in CV rather than CC
mode, and never let measured `VDD_MCU` exceed 2.20 V.

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

Expected application output includes:

```text
BISen camera/CNN direct-VDD stage 1
BISen camera VDD CALIBRATION MODE: workload=off MRAM=off policy_thresholds=unset
BISen camera VDD offline-capture diagnostic ARMED
BISen camera VDD retained point #...: ... code_mean=... nominal_VDD=... mV
BISen camera VDD calibration audit: ADC_mode_after=0 MCUCTRL_ADCBATTLOAD=0x00000000
```

`ADC_mode_after=0` confirms that the shared ADC owner restored the photodiode
mode after reading VDD. A nonzero `ADCBATTLOAD` value is a stop condition for
this validation because the app never requests the load resistor.

## Physical calibration sequence

First resolve or bound the supply-setpoint drop. Keep the programmed bench
voltage at or below 2.20 V until it is understood, so a suddenly recovered
connection cannot over-voltage the rail.

For each safe point, use the offline-capture flow so the onboard J-Link USB
cannot power or clamp the rail through the factory-closed SB3 path:

1. Flash the diagnostic, open SWO, reset once, and confirm that it is armed.
2. Keep the external supply connected, then close SWO and unplug only the
   onboard J-Link USB. Do not press RESET and do not remove VDD.
3. Set the source and wait for the rail to stabilize.
4. Measure `VDD_MCU` directly between J7.1/J7.2 and J3.8 and record the DMM
   voltage.
5. Press BTN0 once and wait at least one second. The firmware captures one
   numbered point into volatile SRAM; it performs no camera or MRAM work.
6. Reconnect J-Link USB without pressing RESET, run `make view`, and press
   BTN1 once. Record the printed point with the simultaneous DMM voltage.
7. Confirm 32/32 valid samples, `ADC_mode_after=0`, and
   `MCUCTRL_ADCBATTLOAD=0`.
8. For another voltage, unplug J-Link USB again and repeat steps 3-7. The log
   holds the newest sixteen points, enough for three repetitions at five
   voltage setpoints.

The log is in a `NOLOAD` TCM section. It normally survives an incidental MCU
reset as long as VDD remains continuously powered, but it is not nonvolatile
storage and must not be treated as valid after loss of board power. No MRAM
program operation is called by this diagnostic.

Start with approximately 1.80, 1.90, and 2.00 V measured at `VDD_MCU`. Do not
use a bench setpoint above 2.20 V to reach the upper rail until the setpoint
loss has been diagnosed. After the path is stable, add measured 2.10 and
2.20 V points and repeat each point three times to quantify dispersion.

No direct-VDD compute, wait, or checkpoint threshold will be selected from
these points alone. Those thresholds require separate workload energy and
brownout-margin measurements.
