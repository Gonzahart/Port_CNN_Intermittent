# BISen camera/CNN integration — AMAP4PEVB Rev. 1

`bisen_camera` is a separate neuralSPOT application for the Apollo4 Plus BGA
Evaluation Board Rev. 1 (`PLATFORM=apollo4p_evb`). It combines the supplied
32x32 photodiode-camera/LeNet workload with the BISen energy policy, three-bit
state instrumentation, bounded execution, and two-slot MRAM recovery. It does
not modify or link `apps/bisen_port`.

The app deliberately refuses to build for any other `PLATFORM`; the same GPIO
numbers reach different header contacts and peripherals on the Blue KXR EVB.

## Board and wiring map

The camera connections remain those used by the supplied CNN project. The
additional BISen signals use pins that do not overlap them.

| Function | AMAP4PEVB Rev. 1 connection |
|---|---|
| Camera column A0..A4 | J9.1/.3/.5/.7/.9 = GPIO96/95/98/99/102 |
| Camera row A0..A4 | J11.1/.3/.5/.7/.11 = GPIO9/8/10/11/91 |
| Camera CS / WR / EN | J9.11/.13/.15 = GPIO36/35/34 |
| Camera OUTCOL | J7.13 = GPIO100 |
| Photodiode ADC | J9.10 = GPIO15/ADCSE4 |
| Existing board LEDs | GPIO90/30/97 |
| BTN0 | J9.4 / onboard SW1 = GPIO18 |
| BISen VCAP divider output | J9.8 = GPIO16/ADCSE3 |
| BISen STATE0 / STATE1 / STATE2 | J12.7/.9/.11 = GPIO62/63/61 |

The state bus is **not** on J9.7/.9/.11 on this board. Those J9 contacts are
camera signals. Move the working network's `STATE0`, `STATE1`, and `STATE2`
inputs to J12.7, J12.9, and J12.11 respectively. Preserve the existing
resistor-to-state association; do not infer it from the approximately
99.3 kOhm / 201 kOhm / 398 kOhm resistance order.

Connect the existing BISen 1 MOhm / 55.8 kOhm divider and 10 nF filter output
to J9.8/GPIO16. Keep the photodiode on J9.10/GPIO15. The divider ground, camera
ground, regulator ground, scope ground, and EVB ground must share the same
reference.

These mappings come from the installed AmbiqSuite R4.5.0
`boards/apollo4p_evb/bsp/am_bsp_pins.h` and the official
[Apollo4 Plus EVB schematic](https://ambiq.com/wp-content/uploads/2022/11/Apollo4-Plus-EVB-Schematic.pdf),
especially the MCU and GPIO-header sheets. The target's board definition is
`FAMILY=apollo4p`, `PACKAGE=AM_PACKAGE_BGA`, `BOARD=apollo4p_evb`.

## VCAP conversion and thresholds

The previous Blue KXR calibration is intentionally not reused. It included an
additional onboard pulldown and was measured on GPIO15/ADCSE4. AMAP4PEVB J9.8
is a direct GPIO16/ADCSE3 header path. Its nominal external-divider transfer
function is:

```text
divider scale = 1 + 1000 kOhm / 55.8 kOhm = 18.921146953
VCAP/code     = (1.19 V / 4095) * scale = 5.498453 mV/code
offset        = 0 mV
```

The physical GPIO16/SE3 path was then measured with 32 ADC readings at each DMM
VCAP point:

| DMM VCAP | Mean ADC code | Fitted VCAP | Residual |
|---:|---:|---:|---:|
| 5.000 V | 903 | 5.012 V | +12 mV |
| 6.100 V | 1097 | 6.106 V | +6 mV |
| 7.500 V | 1337 | 7.458 V | -42 mV |
| 8.500 V | 1521 | 8.495 V | -5 mV |
| 8.900 V | 1598 | 8.929 V | +29 mV |

The least-squares fit is `VCAP_mV = 5.635588398 * code - 76.671740`
(`R^2=0.999739`, RMS residual 23.7 mV). Firmware stores the rounded integer
coefficients `5,635,588 nV/code` and `-76,671,740 nV`; the startup banner now
reports `bench-calibrated`. The scheduling thresholds themselves are unchanged:

| VCAP policy voltage | Action |
|---:|---|
| at least 8.500 V | compute, budget 1,000 units |
| 6.400 to 8.499 V | compute, budget 500 units |
| 5.900 to 6.399 V | compute, budget 100 units |
| below 5.900 V | stop at a coherent boundary, checkpoint dirty progress, wait |
| at least 6.100 V | permit checkpoint restore/resume |
| below 5.500 V | sleep-floor classification |

There is no policy hysteresis and no second safe-write voltage floor.
Completing a frame does not persist its image or CNN result. If that job had a
recovery checkpoint, completion writes one 16-byte sequence-ordered tombstone
so a later cold boot cannot resurrect already-consumed work. Jobs that never
checkpointed incur no completion write.

## Build, deploy, and view

From the neuralSPOT root:

```sh
make -B \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0

make -B deploy \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0

make view \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0
```

The default image uses the physical VCAP source because this target assumes the
transferred BISen divider is connected to J9.8. For a no-divider smoke test,
compile the modelled source and MRAM out explicitly:

```sh
make -B \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0 \
  BISEN_CAMERA_ENERGY_SOURCE=0 \
  BISEN_CAMERA_VCAP_GPIO16_CONFIRMED=0 \
  BISEN_CAMERA_ENABLE_MRAM=0
```

The default run starts one bounded camera/CNN job after reset, then parks.
Press BTN0/SW1 to enter continuous operation. MRAM uses two alternating
app-local 8,080-byte slots. Dirty state is tracked by per-job runtime and
committed generations. There is no artificial powered-session checkpoint cap
by default, matching the MSP430 behavior. A nonzero
`BISEN_CAMERA_MAX_CHECKPOINTS` may be supplied as a research guard; reaching it
parks the target rather than advancing unprotected work.

Boot-time restore is read-only. A newest slot whose payload CRC fails is
ignored in RAM and the older same-phase slot is tried; firmware does not erase
or repair MRAM before VCAP has been qualified. A newer record belonging to a
different phase, or a retirement tombstone, prevents stale cross-job fallback.

## Calibration override

Calibration has a physical measurement step and a firmware coefficient step;
it does not require changing the divider. Build the one-shot diagnostic below,
set a stable VCAP, reset, and record the printed `code_mean` together with the
DMM voltage measured directly across the capacitor. Repeat at 5.0, 6.1, 7.5,
8.5, and approximately 8.9 V. The diagnostic returns before camera work,
checkpoint initialisation, or any MRAM write.

```sh
make -B \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0 \
  BISEN_CAMERA_VCAP_CALIBRATION_MODE=1
```

After collecting the DMM-confirmed pairs of `(code_mean, VCAP millivolts)`,
fit `VCAP_nV = slope_nV_per_code * code + offset_nV` and build:

```sh
make -B \
  EXAMPLE=bisen_camera \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0 \
  BISEN_CAMERA_VCAP_CALIBRATED=1 \
  BISEN_CAMERA_VCAP_NANOVOLTS_PER_CODE=<measured_integer_slope> \
  BISEN_CAMERA_VCAP_OFFSET_NANOVOLTS=<measured_integer_offset>
```

Do not set the calibrated flag merely to suppress the warning. Preserve the
working external 1.90 V MCU rail arrangement and do not parallel it with an
onboard source. Any Rev. 1 power-jumper or solder-bridge change remains a
separate physical decision and is not implied by this firmware target.

## Checkpoint behavior to verify on hardware

For a controlled stable-supply run, begin above 6.1 V and let at least one
coherent camera/CNN step finish. Then cross below 5.9 V. SWO must show exactly
one `checkpoint committed` line with `edge=1`, live progress, and different
runtime/committed generations. Holding VCAP low must not add writes. Raise VCAP
above 6.1 V: the same SRAM position resumes without an MRAM restore because no
reset occurred. After new progress, another high-to-low crossing may commit a
new checkpoint. On job completion, expect one 16-byte retirement tombstone only
if that job had a durable checkpoint; after reset, the tombstone must prevent a
restore of the completed job.

The SWO summary distinguishes scheduler checkpoint attempts, high-level backend
writes, actual HAL program calls, 16-byte program units, and bytes. These are
separate counts: one checkpoint can require several payload programs plus its
header-last commit.

The workload interface is documented in `API.md`; ADC and GPIO ownership are
documented in `OWNERSHIP.md`.
