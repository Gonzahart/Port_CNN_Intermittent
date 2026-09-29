# BISen camera: physical VCAP / MP1584EN build

`bisen_camera_harvest` is a separate Apollo4 Plus BGA EVB Rev. 1 app, forked
from `bisen_camera_trace`. The older `bisen_camera_trace`, `bisen_camera_vdd`,
and `bisen_camera` apps remain available. This app keeps the 32×32 camera,
exact CNN, coherent 100/500/1000-unit scheduling, SRAM wait continuation,
two-slot MRAM checkpoint/recovery, dirty falling-edge save rule, and 0–7
state-DAC codes in the full build. Its energy input is the **physical capacitor
bank ahead of the MP1584EN**. GPIO15 remains the camera pixel ADC input;
GPIO16/ADCSE3 reads a divider from the capacitor bank.

## Electrical connections

| Node | Connection |
|---|---|
| FG output | Diode anode; diode cathode goes only to the VCAP input node, never directly to J7.3 |
| VCAP | Ten correctly polarized, voltage-rated 1000 µF capacitors in parallel (nominal 10 mF); MP1584EN IN+; 390 kΩ divider top |
| Common ground | FG return, capacitor negatives, MP1584EN IN-/OUT-, EVB J3.8 ground |
| MP1584EN OUT+ | J7.3 VDD_EXT, after adjustment and verification under dummy load; preserve the existing J3.3–J3.4 jumper |
| Board rail check | J7.1/J7.2 VDD_MCU |
| VCAP sense | VCAP → 390 kΩ → J9.8/GPIO16/ADCSE3; 10 kΩ and 10 nF from that GPIO16 node to common ground, placed near the header |

```text
FG CH1 center  ── diode anode ──|>|──┬── VCAP ── MP1584 IN+
                                  │
                                  ├── 10 × 1000 µF (+) in parallel
                                  │
                                  └── 390 kΩ ──┬── J9.8 / GPIO16 / ADCSE3
                                               ├── 10 kΩ ── GND
                                               └── 10 nF ── GND
MP1584 OUT+  ─────────────────────────── J7.3 / VDD_EXT
MP1584 IN-/OUT-, capacitors (−), FG shield ── J3.8 / common GND
J7.1/J7.2 = MCU VDD measurement only; J9.10/GPIO15 = camera pixel ADC.
```

Set the FG output-load display to **Hi-Z** and begin at 0 V with a unipolar
trace. The board's POWER-ON switch remains on. No bench-supply positive lead
or USB power is connected during the FG-only run. The FG must charge 10 mF
and supply the run's average input energy; monitor the actual VCAP waveform
and FG output under load rather than relying on the FG's programmed voltage.

The fixed 390 kΩ/10 kΩ divider is this app's proposed bench configuration;
verify the *installed* resistors. At 8 V VCAP its GPIO node is 0.200 V
nominal, 0.220 V using 5% worst-case resistor tolerances. The nominal ADC
codes are about 430 at 5 V and 688 at 8 V, so two-point DMM calibration is
essential. The divider draws about 20 µA at 8 V. Keep **measured** VCAP at
or below 8.0 V, including FG peaks and overshoot; use capacitors rated at
least 16 V. GPIO15/J9.10 remains the camera input and must not be rewired.
Keep J-Link USB and MCU USB disconnected during energy measurements.

**Fixed-divider limitation:** when the MP1584 output and VDDH are off, any
positive VCAP applies a positive voltage to GPIO16. The 0.220 V worst-case
estimate is below the GPIO absolute maximum of VDDH + 0.3 V when VDDH=0,
but the Apollo4 Plus ADC's specified *safe input range* is 0 to VDDH. Thus
this no-switch arrangement is an experimental bench setup; a divider alone
cannot guarantee the ADC's powered-off operating specification or eliminate
all back-powering. The firmware checks the configured fixed-divider ratio
against a 250 mV off-rail bench limit with 5% resistor tolerance, but cannot
verify actual wiring or remove the remaining off-rail condition. For a
datasheet-compliant unattended FG-only system, add hardware isolation later.

The MP1584 is a buck regulator specified for 4.5–28 V input. Measure its
actual output and dropout/turn-off behavior on this module and board before
choosing work thresholds. Keep measured J7.1 VDD at or below the project's
2.20 V experimental ceiling. Capacitor voltage and board VDD are separate.
Use capacitors rated above the highest VCAP, verify polarity, and account for
FG charging current/inrush. The ten capacitors store about 0.195 J between
8 V and 5 V, before converter losses; this does not establish run duration.

## Build 1: ADC calibration (default, no workload or MRAM writes)

From `/Users/ghart/Documents/Ambiq/neuralSPOT` after installing this app:

```sh
make -B EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
make -B deploy EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
```

The default build is a runnable **calibration diagnostic** for the fixed
390 kΩ/10 kΩ bench divider. BTN0 records 32 ADCSE3 readings in volatile SRAM;
BTN1 prints them after
J-Link is reconnected while board power stays on. Record two DMM VCAP points
and corresponding `code_mean` readings separated across the *reliably powered*
VCAP range (one near its lower usable end, one near the highest planned peak).
`VCAP_nominal_mV` is an estimate from the nominal ADC reference and divider;
the raw code/DMM pairs are the calibration evidence. A complete VDD loss
erases this calibration log. Its `HVCC` identity prevents old trace-app
calibration records from being mistaken for physical VCAP measurements.

## Build 2: full camera/CNN/MRAM behavior

Measure all four physical VCAP thresholds first. At minimum, determine the
regulator's reliable operating floor and the capacitor energy necessary to
finish a dirty checkpoint and each work budget. The four thresholds must be
strictly increasing. Then supply the measured values on **every** full build
and deploy command, in microvolts:

```sh
make -B deploy EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
  BISEN_HARVEST_CALIBRATION_MODE=0 BISEN_CAMERA_ENABLE_MRAM=1 \
  BISEN_HARVEST_CAL_LOW_CODE=<measured_code_1> \
  BISEN_HARVEST_CAL_LOW_UV=<measured_vcap_1_uV> \
  BISEN_HARVEST_CAL_HIGH_CODE=<measured_code_2> \
  BISEN_HARVEST_CAL_HIGH_UV=<measured_vcap_2_uV> \
  BISEN_HARVEST_CRITICAL_UV=<critical_uV> \
  BISEN_HARVEST_WORK100_UV=<work100_uV> \
  BISEN_HARVEST_WORK500_UV=<work500_uV> \
  BISEN_HARVEST_WORK1000_UV=<work1000_uV>
```

The full build fails compilation if calibration or physical policy thresholds
are absent. The conversion path is: physical ADCSE3 code → calibrated VCAP
microvolts → virtual BISen code. Exact virtual boundaries remain 2185
(critical), 2333 (100 units), 2441 (500 units/restore), and 2553 (1000
units). These correspond to historical 1.90/2.00/2.10/2.20 V policy
coordinates; they are **not** VCAP voltages. No CNN numerical code was changed.

The full build starts one bounded job after reset and then parks; BTN0 enters
continuous mode. MRAM uses a distinct `HVR1` record identity so it cannot
adopt old `TRC1` artificial-trace records. Its SRAM scan progress uses a
distinct `HVS1` identity too. Reflashing images can still overwrite an app's
MRAM region; use each version's checkpoint tests independently.
Within a job, the default energy wait has no timeout: it sleeps for 250 ms
between VCAP checks until the measured threshold is met or physical power is
lost. `BISEN_CAMERA_MAX_WAIT_CYCLES=<N>` restores a bounded diagnostic wait.

## Optional FG-only offline validation build

`BISEN_HARVEST_OFFLINE_VALIDATE=1` is an opt-in diagnostic on this app. Build
it with `BISEN_CAMERA_AUTORUN=0`; BTN0 runs exactly one job and then latches
the result. BTN0 is ignored after completion so a release edge or switch bounce
cannot overwrite the trial; reset or power-cycle to start another trial. The
firmware keeps a checksummed summary in NOLOAD TCM: first
and maximum paused workload position, wait/resume counts and measured VCAP,
first and last checkpoint phase/position, checkpoint count, last CNN digit,
completion status, and backend program count.
BTN1 prints it after J-Link is reconnected. A debugger-induced reset while VDD
remains present does not erase the record; the `boots` count reports such a
reset. Actual VDD loss erases TCM, yielding a missing/empty record. This SRAM
summary is diagnostic and does not replace an MRAM checkpoint.

The initial validation build keeps the measured ADC calibration and diagnostic
thresholds used in the September 18 capture. It leaves MRAM disabled:

```sh
cd /Users/ghart/Documents/Ambiq/neuralSPOT
make -B EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
  BISEN_HARVEST_CALIBRATION_MODE=0 BISEN_CAMERA_ENABLE_MRAM=0 \
  BISEN_CAMERA_AUTORUN=0 BISEN_HARVEST_OFFLINE_VALIDATE=1 \
  BISEN_HARVEST_CAL_LOW_CODE=461 BISEN_HARVEST_CAL_LOW_UV=5500000 \
  BISEN_HARVEST_CAL_HIGH_CODE=634 BISEN_HARVEST_CAL_HIGH_UV=7500000 \
  BISEN_HARVEST_CRITICAL_UV=5800000 BISEN_HARVEST_WORK100_UV=6200000 \
  BISEN_HARVEST_WORK500_UV=6800000 BISEN_HARVEST_WORK1000_UV=7300000
```

Repeat the same arguments with `make -B deploy` to flash it. Before starting
the trial, verify with **J-Link USB and MCU USB disconnected** that turning the
FG output off and allowing VCAP to discharge eventually brings J7.1 to 0 V
and stops the state DAC; then recharge VCAP. Keep the board power
switch ON for the FG-only trial and the later readout. With measured VCAP near
7.5 V, press BTN0, lower VCAP to about 6.0 V while state 2 is active, hold,
then raise it to at least 7.0-7.2 V until state 3 and the return to state 0 are
seen. After a low-energy stop, the scheduler requires the configured 6.8 V
work500/resume threshold before continuing. This prevents ADC variation around
the 6.2 V work100 edge from generating repeated checkpoint episodes.
Keep the FG supplying the board after completion to preserve TCM. Only then
reconnect J-Link USB, start `make view EXAMPLE=bisen_camera_harvest
PLATFORM=apollo4p_evb AS_VERSION=R4.5.0`, and press BTN1. A valid run reports
`status=2`, `first_checkpoint_position>0`, `resumes>0`, `results=1`, the
expected digit, and `backend_programs=0` in this MRAM-off build. `status=0` or `trial=0`
after reconnection means the offline result did not survive. Do not turn the
board power switch OFF between capture and BTN1 readout: J-Link may keep SWO
alive through a different power path while the FG-powered MCU state is lost.

The J-Link-connected readout is outside the FG-only measurement window. The
scope capture made while USB is absent remains the evidence for VCAP, board
VDD, and timing. The previous full build remains available by leaving
`BISEN_HARVEST_OFFLINE_VALIDATE=0` and using its original build flags.

### MRAM cold-restore validation

After the retained-RAM run passes, build a separately archived MRAM diagnostic
with the same measured calibration and thresholds. This diagnostic uses the
new `HVR2` checkpoint identity, so it cannot adopt a persistent `HVR1` record
left by an older harvest image:

```sh
/Users/ghart/workspace_v12/Image_Sobel_5969/build_harvest_mram_cold_restore.sh
/Users/ghart/workspace_v12/Image_Sobel_5969/flash_harvest_mram_cold_restore.sh
```

First perform one powered smoke run: start at about 7.5 V VCAP, press BTN0,
cross below the 6.2 V work threshold during state 2, wait for state 4 followed
by state 5, then raise VCAP above the 6.8 V resume threshold without allowing
J7.1 to collapse. The final offline report must show the expected digit and a
nonzero MRAM `backend_programs` count.

For the cold-restore run, repeat the start and falling edge, but after the
state-4/state-5 checkpoint commit keep lowering VCAP and turn the FG output
off. Verify J7.1 reaches 0 V and the state DAC stops, then hold the board fully
unpowered for at least five seconds. Recharge VCAP to approximately 7.1--7.5 V.
The MCU cold-boots, validates the MRAM record, and parks; after VCAP is stable,
press BTN0 to continue the recovered job. Do not reflash between the checkpoint
and recovery halves. After completion, keep FG power applied, reconnect J-Link,
start SWO, and press BTN1. A successful camera recovery reports
`storage_restores=1`, `first_storage_restore_phase=1`, the same nonzero pixel
position committed before power loss, `results=1`, and the expected digit.
The post-restore `backend_programs` value normally includes the retirement
tombstone; the original checkpoint writes happened before volatile counters
were lost.

The result is now immutable until reset. If an older diagnostic prints
`trial=2` after a successful restore, use its state-event CSV to separate the
first recovered 1024-pixel workload from the extra workload; the newer build
prevents that second launch and consumes each boot's restore marker once.

## RF-replay experiment build

After cold-restore validation, use the separately archived RF-replay image:

```sh
/Users/ghart/workspace_v12/Image_Sobel_5969/build_harvest_rf_replay.sh
/Users/ghart/workspace_v12/Image_Sobel_5969/flash_harvest_rf_replay.sh
```

This build selects checkpoint identity `HVR3`, so it cannot adopt `HVR1` app
records or `HVR2` validation records. A fresh image parks until BTN0 is pressed.
BTN0 atomically stores a separate durable continuous-run marker and begins the
camera/CNN loop. After each result the next job starts automatically. If board
power is lost while that marker is armed, the next boot restores interrupted
work and resumes continuous operation without another button press.

BTN1 is the clean stop control. At the next coherent pixel/CNN boundary it
checkpoints any dirty workload progress, atomically clears the continuous-run
marker, and parks. A later reset or power cycle remains parked; BTN0 resumes the
saved job and re-arms continuous operation. RESET is therefore a recovery test,
not a stop control: an armed session continues after reset. The validated 6.2 V
work and 6.8 V resume thresholds, external GPIO16 VCAP ADC, state DAC, unlimited
wait, and dirty falling-edge checkpoint rule are unchanged.

The isolated replay image compiles SWO logging out to remove its formatting and
ITM overhead while J-Link is absent. CH2 state DAC remains the authoritative
state record; capture CH4 at physical VCAP and CH1 at J7.1 board VDD. A separate
J-Link-connected diagnostic build should be used when textual CNN scores are
required. Normalize later energy comparisons by the number of completed CNN
state-3 episodes or use BTN1 after a fixed number of jobs; continuous mode
otherwise uses every available energy interval.

## Incremental power options

- `BISEN_HARVEST_PARK_ADC=1` (default) powers ADC0 down during long energy
  waits and while parked. `=0` is the trace-app-compatible baseline. Verify
  ADC wake and exact camera output on hardware before attributing energy savings.
- `BISEN_HARVEST_SWITCHED_DIVIDER=0` (default) measures through the fixed
  390 kΩ/10 kΩ divider. It consumes about 20 µA at 8 V and has the off-rail
  limitation above. For a later isolated revision, `=1` and a verified unused
  `BISEN_HARVEST_SENSE_ENABLE_PIN` enable a separately built VCAP-rated switch
  and level shifter. Do not drive a high-side P-MOSFET gate directly from a
  2 V GPIO at 8 V VCAP.
- `BISEN_ENABLE_STATE_DAC=1` and `BISEN_ENABLE_SWO_LOGGING=1` preserve
  validation observability. Set both to 0 for a separately identified
  low-overhead measurement build, after validating the instrumented version.
  SRAM symbol `g_harvest_stats` records state time/entries, VCAP observations,
  camera activations and scheduled CNN budget counts without per-sample prints.
- `BISEN_CAMERA_SCAN_IDLE_MODE=1` retains the tested STIMER normal-sleep
  settling wait. Deep sleep, SRAM bank changes, and CPU frequency scaling
  remain experimental. `BISEN_HARVEST_MCU_LOW_POWER=1` selects the supported
  96 MHz MCU mode for a separate measured energy-per-job comparison; default
  is the previous 192 MHz high-performance mode.

The external camera has no confirmed power-gate pin. Firmware leaves its pin
state unchanged outside sensing rather than inventing a GPIO that could alter
frame acquisition. The MP1584 module and factory EVB power paths can set a
current floor that firmware alone cannot remove.

## Validation order

1. With GPIO16 and J7.3 disconnected, verify FG/diode/VCAP charging and the
   divider ratio by DMM from 0 to 8 V. At 8 V VCAP, GPIO16 junction must be
   near 0.20 V and below 0.25 V. Test MP1584 output with a dummy load before
   connecting J7.3. Verify J7.1 voltage after USB/debugger removal.
2. Capture two ADC calibration points without J-Link powering the board.
3. Characterize the regulator floor and current/energy of sensing, 100/500/1000
   CNN units, MRAM save, and restore using the 1 Ω shunt. Derive thresholds.
4. Flash full mode and verify exact CNN result, SRAM wait/resume, one-time
   dirty checkpoint, cold restore, and no repeated writes at low VCAP.
5. Compare state 0 versus camera/CNN current using the same supply/FG trace.
   Then compare `PARK_ADC=0/1`, and later the optional switched divider
   and low-overhead instrumentation build.

Scope: CH1 VCAP, CH2 state DAC, CH3 1 Ω shunt differential/current, CH4
J7.1 board VDD. A single-ended scope probe cannot directly ground either
side of a high-side shunt; use two matched ground-referenced channels and
subtract, or a differential probe.

The ADC conversion range and MP1584 input range cited above come from the
[Ambiq Apollo4 Plus datasheet](https://ambiq.com/wp-content/uploads/2022/03/Apollo4-Plus-SoC-Datasheet.pdf)
and [MPS MP1584 product page](https://www.monolithicpower.com/en/products/power-management/switching-converters-controllers/step-down-buck/converters/mp1584.html).
