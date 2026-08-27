# Apollo4 Blue Plus BISen Port — Current Handoff

**Status date:** 2026-08-17  
**Board:** Ambiq AMAP4BPXEVB Apollo4 Blue Plus KXR EVB, PCB Rev. 2.0  
**neuralSPOT platform:** `apollo4p_blue_kxr_evb`  
**AmbiqSuite:** `R4.5.0`  
**Application:** `apps/bisen_port`  
**Current run-mode banner:** `BISen run-mode guard v4`

This handoff records what is implemented, what has actually been confirmed on
the bench, what has only been build-validated, and what remains intentionally
unresolved. It is intended to be sufficient context for planning the next
experimental phase without reopening already settled firmware questions.

## 1. Non-negotiable scope and safety constraints

- Keep BISen isolated in `apps/bisen_port`.
- Preserve `apps/basic_tf_stub`, the existing bootloader, and the known-good
  VS Code/J-Link workflow.
- Build for exactly `PLATFORM=apollo4p_blue_kxr_evb` and
  `AS_VERSION=R4.5.0`.
- Do not guess board pins, MRAM addresses, HAL APIs, voltage thresholds, or
  Rev. 2.0 jumper/solder-bridge behavior.
- Do not power the Apollo4 directly from the changing capacitor voltage.
- Do not parallel the external regulator with the onboard MCU supply.
- The present voltage thresholds are controlled-bench scheduler policy. They
  are not yet proven energy-safe thresholds for capacitor discharge.
- Real energy claims must wait for the intended lower-consumption converter.
  The LM2596 is acceptable for functional stable-supply testing but its own
  consumption makes it unsuitable for final small-capacitor energy results.
- Energy modes change chunk size and scheduling only. They do not change the
  Sobel algorithm or silently reduce output quality.

The repository instructions are in `/Users/ghart/Documents/Ambiq/neuralSPOT/AGENTS.md`.

## 2. Repository and preservation status

Repository:

```text
/Users/ghart/Documents/Ambiq/neuralSPOT
```

The BISen application currently appears to Git as an untracked directory:

```text
?? apps/bisen_port/
```

This is important: back up or commit the application before any broad cleanup,
checkout, or reset operation. Build products under `build/` are generated and
must not be committed.

The port does not alter the repository-wide linker configuration. Its
checkpoint reservation is app-local in `apps/bisen_port/bisen_checkpoint.ld`.
`basic_tf_stub` was preserved and passed earlier clean rebuilds. The current
changes are confined to `apps/bisen_port`.

The existing Ambiq SBL remains present. Observed boot identity:

```text
Ambiq Secure BootLoader!
SBL_ap4p_v0.2 ... @ 0x8000
SecureBoot disabled - image @ 0x18000
```

The firmware does not erase or replace that bootloader. `SecureBoot disabled`
is the board's reported security-policy state; it should not be paraphrased as
secure boot being enabled.

## 3. Current physical bench configuration

### Power architecture

The fixed bench architecture is:

```text
stable DC supply
      |
      +---- VCAP ---- 1000 uF, 16 V capacitor ---- GND
      |
      +---- LM2596 ---- regulated approximately 1.90 V ---- EVB MCU rail

VCAP ---- external divider ---- GPIO15 / ADCSE4
```

User-confirmed measurements while the EVB was operating:

- LM2596 output: approximately `1.903 V`.
- J7 pins 1/2 measurement: approximately `1.901 V`.
- J3 pins 3–4 are currently shunted.
- The EVB booted, J-Link/SWO worked, and the integrated workload ran on the
  externally regulated rail.

Do not infer a new power-injection procedure from the old Rev. 1 guide. Before
any wiring change, use the Rev. 2.0 schematic/assembly files and document the
current physical connections. In particular, do not remove solder bridges or
move power shunts merely to reproduce a Rev. 1 instruction.

### VCAP divider and ADC input

Final working input:

| Function | Confirmed connection |
|---|---|
| VCAP ADC | J9 pin 10 / GPIO15 / ADCSE4 |
| External upper resistor | `1.000 Mohm` |
| External lower resistor | measured `55.8 kohm` |
| Board loading included by firmware | fitted `R51 = 1 Mohm` to ground |
| External filter capacitor | `10 nF` at the ADC node |

Firmware scale:

```text
VCAP = ADC_volts * 19.921146953
```

The implementation also enforces the present validation bounds:

```text
ADC node <= 0.5 V
VCAP     <= 9.04 V
```

GPIO15 is shared through fitted SB60 with U15 ON2; that board fact and the
fitted R51 load are included in the design. Do not extend the voltage range
without a new hardware review.

GPIO12 / ADCSE7 was tested and rejected. Its fitted connection to the onboard
J-Link VCOM receiver drove the divider node to approximately `1.614 V` and
saturated the 1.19 V ADC path. No solder bridges were opened; the final design
returned to GPIO15.

### State instrumentation

The implemented three-bit state bus is:

| Code bit | GPIO | Header |
|---|---:|---:|
| `STATE0` / LSB | 62 | J9 pin 7 |
| `STATE1` | 63 | J9 pin 9 |
| `STATE2` / MSB | 61 | J9 pin 11 |

Code meanings:

| Code | Meaning |
|---:|---|
| 0 | sleep / inactive |
| 1 | VCAP ADC |
| 2 | die temperature |
| 3 | compute |
| 4 | MRAM write |
| 5 | checkpoint committed |
| 6 | checkpoint restore/validation |
| 7 | boot/error |

The three GPIOs feed one analog oscilloscope node through a resistor-summing
network. User-reported measured resistor values are approximately `99.3 kOhm`,
`201 kOhm`, and `398 kOhm`. Record the exact resistor-to-GPIO association from
the current wiring before disturbing it; do not infer that association from
resistance order alone.

Scope and capture setup that produced a valid decoded capture:

- Oscilloscope: Siglent SDS1204X-E, firmware `8.3.6.1.37R17`.
- CH2: regulated MCU rail, `1 V/div`, zero offset.
- CH4: resistor-DAC node, `500 mV/div`, approximately `-1.40 V` offset,
  10x selected both on the scope and the physical probe.
- Capture script:
  `/Users/ghart/VSC/Image_filter/scope_capture_plot.py`.
- Confirmed full-memory capture:
  `/Users/ghart/VSC/Image_filter/ambiq_state_dac_fullres_ch2.csv` and its
  associated PNG/state-events CSV.

Known-good capture command:

```sh
cd /Users/ghart/VSC/Image_filter

python3 scope_capture_plot.py \
  --channels 2 4 \
  --state-dac-ch 4 \
  --state-vcc-ch 2 \
  --state-min-vcc 1.0 \
  --manual-vdiv 2:1.0,4:0.5 \
  --manual-offset 2:0,4:-1.40 \
  --waveform-source memory \
  --points 0 \
  --sparsing 1 \
  --max-plot-points 50000 \
  --no-energy \
  --debug \
  --out ambiq_state_dac_fullres_ch2
```

The successful transfer returned 1,400,000 points per channel. The earlier
display-waveform capture returned only 14,000 points and visually collapsed
the short DAC states; use memory capture for state timing.

## 4. Implemented firmware

Source files are split by responsibility:

| File | Responsibility |
|---|---|
| `src/bisen_port.cc` | explicit scheduler and bounded/continuous run control |
| `src/bisen_adc.*` | shared interrupt-driven general ADC helper |
| `src/bisen_energy.*` | VCAP measurement and decision structure |
| `src/bisen_policy.cc` | exact direct-threshold voltage/chunk policy |
| `src/bisen_temperature.*` | factory-trim die-temperature conversion |
| `src/bisen_checkpoint.*` | two-slot MRAM records and recovery |
| `src/bisen_compute.*` | deterministic resumable 64x64 Sobel workload |
| `src/bisen_power.*` | LFRC RTC and Apollo4 deep sleep/wake |
| `src/bisen_instrumentation.*` | SWO-correlated three-bit GPIO state bus |
| `src/bisen_config.h` | all compile gates, pin facts, and bench policy |
| `bisen_checkpoint.ld` | app-local linker-resolved `NOLOAD` reservation |

### ADC behavior

- Apollo4 Plus general ADC, not audio ADC.
- R4.5.0 HAL and BSP APIs.
- 1.19 V internal reference.
- 12-bit result with 16-sample averaging.
- Maximum six-bit tracking-cycle setting for the high-impedance divider.
- 5 ms divider/ADC settling delay.
- First post-wake conversion is discarded; the following conversion is used.
- Scan completion is interrupt-driven. The CPU waits in normal sleep.
- The ISR records completion/error only. FIFO processing and scheduler work
  remain in thread context.
- DAXI is invalidated before consuming the completed scan.

The VCAP and temperature paths share this helper but use separate ADC slots.

### Die temperature

The temperature state uses the internal ADC temperature channel and the
factory-trim R4.5.0 `AM_HAL_ADC_REQ_TEMP_CELSIUS_GET` control request. It does
not hard-code a sensor slope and it is correctly described as die temperature,
not precision ambient temperature.

### Scheduler

Explicit states remain:

```text
Wake / qualification
VCAP ADC
Die temperature
Resumable Sobel
MRAM checkpoint
RTC deep sleep
```

Every wake remeasures VCAP before new work. Work is bounded into exact chunk
sizes. Dirty incomplete progress is committed before an energy-driven sleep
when the next useful state cannot run; adequate-energy bounded-test sleeps use
retained DTCM and do not program MRAM.

### Current exact bench policy

The qualitative labels `aggressive`, `medium`, and `mild` were removed. Every
branch runs the same deterministic Sobel kernel; only chunk size changes.

Base classification and actions in `src/bisen_config.h`:

| Action | Current controlled-bench value |
|---|---:|
| 1000-pixel chunk | `VCAP >= 8.5 V` |
| 500-pixel chunk | `VCAP >= 6.4 V` |
| 100-pixel chunk | `VCAP >= 5.9 V` |
| no new chunk | below the 100-pixel policy |
| recovery checkpoint request | dirty incomplete progress immediately before an energy-driven sleep when no next useful state is allowed |
| explicit sleep floor | `VCAP < 5.5 V` |
| restore and temperature allowed | `VCAP >= 6.1 V` |

Chunk plans are selected directly from the latest calibrated VCAP result.
Rising and falling transitions therefore use the same exact boundaries: 5.9,
6.4, and 8.5 V. The previous energy band does not alter the decision.

`compute_allowed` additionally requires measured `VCAP >= 5.9 V` and a
non-wait plan. This matters on a falling sweep: the retained 100-pixel plan can
remain visible between 5.90 V and 5.65 V while new computation is disallowed.

These are exact software expectations, not measured capacitor-energy limits.

### Two-slot MRAM checkpointing

- Storage is placed by the app-local linker extension, not a hard-coded raw
  MRAM address.
- The reservation is `NOLOAD` and excluded from the `.bin`.
- Two alternating slots, 80 bytes each, aligned for 16-byte MRAM operations.
- Records include version/epoch, sequence, job/progress, digest, CRC, and final
  aligned commit data.
- The inactive slot is written first; its final commit block is programmed
  last. A torn inactive record cannot supersede the prior committed slot.
- Writes use `am_hal_mram_main_program`, interrupt save/restore, cache/DAXI
  maintenance, and readback validation.
- Boot validates both slots and selects the newest valid committed sequence.
- Different destructive/test milestones use distinct epochs so stale records
  from one test are rejected by another.

The current v4 artifact resolves its checkpoint region to:

```text
[0x000211a0, 0x00021240)
```

Always verify these symbols in the current ELF/map after an application or
linker change. Older SWO logs contain different correct addresses for older
images and must not be copied into new code.

There is no persistent MRAM wear-budget counter. Bounded mode limits writes by
work progress and stops. Continuous mode intentionally starts new jobs until
RESET; at compute-qualified VCAP it can therefore keep writing checkpoints.
Do not leave continuous high-energy operation unattended, and do not use the
current integrated continuous mode for a long threshold sweep.

### Resumable workload

- Deterministic 64x64 Sobel-style image workload.
- Exactly 3,844 output pixels.
- Exact resumable progress and rolling digest in the checkpoint.
- Golden final digest: `8bdd7454`.
- Tested chunk sizes: 100, 500, 1000, and one-shot/full remainder.
- Malformed progress/digest/completion combinations are rejected.
- A checkpoint failure parks instead of continuing with unpersisted work.

### RTC and low power

- Apollo4 R4.5.0 LFRC RTC.
- One-second repeating RTC alarm.
- Strong `am_rtc_isr` clears the alarm and updates only wake state/count.
- Uses neuralSPOT's Apollo4 `ns_deep_sleep()` wrapper.
- ADC is re-run after every RTC wake before work resumes.
- RTC/HAL failures report a stage/status and park fail-closed.

This proves functional deep sleep/wake. Minimum sleep current has not been
claimed; the known-good neuralSPOT development power profile remains selected.

### Bounded and continuous mode (v4)

- Every boot/reset initializes volatile run mode to **bounded**.
- BTN0 is SW1 / GPIO17. Target RESET is SW3 / nRST.
- BTN0 interrupt remains disabled while bounded work is active.
- A completed bounded job parks and then arms BTN0.
- If an otherwise valid policy produces no compute progress for three
  consecutive RTC wakes, bounded mode parks early and still arms BTN0.
- ADC, temperature, compute, checkpoint, RTC, or hard wake-limit failures
  remain fail-closed and do not arm BTN0.
- Before arming, GPIO17 must remain released/high for 20 ms.
- An interrupt is only a candidate; GPIO17 must remain pressed/low for 20 ms
  before continuous mode is accepted.
- Continuous mode does not bypass energy policy. At low VCAP it only continues
  ADC/sleep checks. At qualified VCAP it resumes/executes/checkpoints jobs.
- RESET clears the volatile continuous choice and returns to bounded mode.
- Run mode is deliberately not stored in MRAM.

Confirmed current behavior reported by the user:

1. The board no longer starts in continuous mode.
2. Bounded wake activity works.
3. At no-progress VCAP it parks after three wakes instead of 50.
4. BTN0 actually enters continuous mode after the armed message.
5. RESET stops continuous mode and starts bounded mode again.

## 5. On-target validation completed

### VCAP ADC calibration

Final GPIO15/divider comparison:

| DMM VCAP | DMM ADC node | Firmware ADC | Firmware VCAP |
|---:|---:|---:|---:|
| 5.02 V | 0.252 V | 0.2519 V | 5.0179 V |
| 7.03 V | 0.353 V | 0.3524 V | 7.0204 V |
| 9.00 V | 0.452 V | 0.4521 V | 9.0056 V |

This validates the divider conversion over the current 5–9 V bench range.

### Die temperature

Representative on-target results:

| VCAP | Reported die temperature |
|---:|---:|
| 7.03 V | 27.99 C |
| 6.20 V | 28.24 C |
| 7.50 V | 28.07 C |

### Policy branch coverage already observed

- Around 5.02 V: wait / no chunk.
- Around 6.20 V: 100-pixel chunk.
- Around 7.03–7.50 V: 500-pixel chunk.
- Around 9.00 V: 1000-pixel chunk.

This proves all chunk branches execute. It does **not** yet characterize exact
physical transition voltages under a controlled sweep.

### MRAM persistence and atomicity

Confirmed on target:

- Normal committed checkpoint restored across successive manual resets.
- Completed temperature work was not repeated after restore.
- Stable-power alternation changed slot 0 -> slot 1 -> slot 0 with increasing
  sequence values.
- Phase-2 reset injection interrupted the inactive record before final commit;
  the next boot rejected it and restored the older committed slot.
- Phase-3 reset injection occurred after final commit but before final
  readback; the next boot restored the newly committed record.

This completes the intended two-slot torn-write fault matrix under controlled
reset injection.

### Workload resume and golden output

At approximately 7.50 V, repeated manual resets restored and advanced exact
500-pixel chunks through:

```text
500  -> digest 25e20392
1000 -> digest c575a53f
1500 -> digest 1fa608a5
2000 -> digest 38cd86de
2500 -> digest b092609c
3000 -> digest 170f4a3c
3500 -> digest a37f0a84
3844 -> digest 8bdd7454
```

Slots alternated and sequences increased. The final digest matched the host
golden value. One additional reset restored completed progress 3844 and parked
without another compute chunk or MRAM write, proving completed work was not
duplicated.

### Deep sleep and integrated cycles

- Bounded low-power test passed three LFRC RTC wake cycles with a fresh VCAP
  reading after each wake and then parked.
- Integrated stable-power operation passed repeated ADC -> compute -> MRAM ->
  RTC cycles through golden workload completion.
- Every observed checkpoint reported success/layout validity and alternated
  slots.
- A functional external-power interruption/reapplication test stopped the
  target and later resumed from a valid committed checkpoint. This is useful
  functional evidence only; it was not a controlled brownout-threshold or
  energy-reserve measurement.

### GPIO/DAC observability

- Each GPIO passed direct logic-level testing.
- The resistor-summed one-node output was visible on the SDS1204X-E.
- Full-memory capture recovered the brief state levels that display-memory
  capture had collapsed.
- The decoded scope events correlate with SWO state transitions.

### Current run-mode behavior

The final user confirmation for v4 is:

```text
Intended wake activity now works.
BTN0 actually enters continuous mode.
RESET stops it.
```

## 6. Current build and artifact evidence

Current full rebuild configuration:

```sh
cd /Users/ghart/Documents/Ambiq/neuralSPOT

make -B \
  EXAMPLE=bisen_port \
  PLATFORM=apollo4p_blue_kxr_evb \
  AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 \
  BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_LOW_POWER=1 \
  BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 \
  BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=1 \
  BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT=50 \
  BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT=3 \
  BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_GPIO_INSTRUMENTATION=1 \
  BISEN_ENABLE_DEBUG_LOGGING=1
```

Result: clean link/copy success for `apollo4p_blue_kxr_evb` with R4.5.0.
Only pre-existing neuralSPOT/AmbiqSuite heap/codec warnings were emitted.

Current local artifact:

```text
File: build/apollo4p_blue_kxr_evb/arm-none-eabi/apps/bisen_port/bisen_port.bin
Size: 37,268 bytes
SHA-256: 1991c600d83851364ec2b3fa559d89d943c132d3b55ac71dcd2fba6d62a51888
ELF: text=37,188 data=80 bss=24,784 total=62,052 bytes
Application origin: 0x00018000
Binary end: 0x00021194
Checkpoint region: [0x000211a0, 0x00021240)
```

The host policy test passed with `BISEN_ENABLE_COMPUTE_WORKLOAD=1`. The host
compute test passed for all chunk sizes, resume behavior, golden digest, and
malformed-context rejection.

Deploy the exact same configuration by changing `make -B` above to
`make -B deploy`. Close the SWO viewer first. The repository's deploy recipe
has previously printed a Make error as ignored when J-Link could not attach, so
do not accept Make's final status alone: require J-Link programming and verify
messages ending in `O.K.`.

Viewer command:

```sh
make view \
  EXAMPLE=bisen_port \
  PLATFORM=apollo4p_blue_kxr_evb \
  AS_VERSION=R4.5.0
```

Current image identity must include:

```text
BISen run-mode guard v4: bounded first; no-progress_wakes=3; BTN0 requires a debounced press after park
```

## 7. What remains unresolved or unvalidated

1. Exact threshold transitions have not been physically swept.
2. The current threshold values are not proven energy-safe write/compute
   limits for a discharging capacitor.
3. Energy per ADC, temperature read, chunk size, checkpoint phase, boot, and
   sleep/wake transition has not been measured with the final converter.
4. Minimum sleep current has not been established; the development power
   profile and debug setup remain active.
5. The intended lower-consumption converter has not yet replaced the LM2596.
6. Real RF-trace replay has not begun.
7. Power-failure behavior has not been swept against controlled voltage ramps
   or characterized for converter dropout/restart hysteresis.
8. The current GPIO/DAC captures validate observability and timing shape, not
   energy, because no calibrated current/energy channel was included.
9. Long continuous operation at compute-qualified VCAP can produce repeated
   MRAM writes; no automatic wear quota currently stops it.

## 8. Recommended next planning decision: threshold characterization

Do not perform a long voltage sweep with the current integrated continuous
image. At compute-qualified VCAP it repeatedly computes and checkpoints, which
adds unnecessary MRAM writes and couples workload activity into a test whose
first purpose is policy verification.

Recommended next firmware milestone:

```text
policy-only threshold-sweep mode
  - physical GPIO15 VCAP ADC enabled
  - one-second RTC sample cadence
  - exact existing direct-threshold policy
  - SWO reporting enabled
  - optional state-DAC output repurposed/documented for plan code
  - temperature disabled
  - Sobel disabled
  - MRAM writes disabled
  - finite sample/time bound plus explicit start/stop behavior
```

Recommended physical procedure after that mode exists:

1. Keep the regulated MCU rail near 1.90 V and monitor it independently.
2. Start near 5.4 V VCAP.
3. Sweep upward monotonically in 0.10 V steps, reducing to 0.02–0.05 V near an
   expected transition.
4. Hold each point for 5–10 readings and record the firmware VCAP median as
   well as DMM VCAP. The firmware decision uses its reconstructed value.
5. Reach no more than the already validated 9.0 V region.
6. Reverse direction without resetting so the previous-band state is retained.
7. Repeat the full rising/falling sweep at least three times.
8. Record direction, DMM VCAP, firmware VCAP, plan/chunk, restore, temperature,
   compute, emergency, and sleep-floor flags.

The planning discussion should decide whether this first sweep is intended to
validate only software transition correctness or also characterize ADC noise,
transition repeatability, and debounce/hold-time statistics. Energy-safe
threshold selection is a later experiment with the final converter and current
measurement.

## 9. Primary local references

Original handoffs:

```text
/Users/ghart/Downloads/Apollo4_VSCode_BISen_Handover.md
/Users/ghart/Downloads/Apollo4_BISen_Firmware_and_Bench_Setup_Handoff.md
```

Rev. 2.0 board files:

```text
/Users/ghart/Downloads/Apollo4-Blue-Plus-KXR-EVB-Design-Files-v2-0/Release/Schematic Print/AMAP4BPXEVBv2.0 Schematic Prints.PDF
/Users/ghart/Downloads/Apollo4-Blue-Plus-KXR-EVB-Design-Files-v2-0/Release/PCB Print/AMAP4BPXEVBv2.0 Assembly Drawings.PDF
```

Installed authoritative SDK/HAL/BSP:

```text
/Users/ghart/Documents/Ambiq/neuralSPOT/extern/AmbiqSuite/R4.5.0
```

Detailed implementation and historical validation notes:

```text
/Users/ghart/Documents/Ambiq/neuralSPOT/apps/bisen_port/README.md
```

## 10. Short status summary

The initial Apollo4 BISen port is functionally complete under stable bench
power: external VCAP sensing, calibrated die temperature, exact chunk policy,
two-slot fault-safe MRAM persistence, RTC deep sleep/wake, deterministic
resumable Sobel, GPIO/DAC state instrumentation, bounded default operation,
BTN0-selected continuous operation, and RESET-to-bounded behavior have all
been demonstrated. The next stage is not more basic porting. It is controlled
threshold characterization followed later by energy characterization with the
intended converter.
