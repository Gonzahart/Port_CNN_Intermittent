# BISen Apollo4 Port

This is the isolated BISen target for `apollo4p_blue_kxr_evb` with
AmbiqSuite R4.5.0. It does not modify `basic_tf_stub`, the bootloader, or the
repository-wide linker configuration.

## Safe defaults

The flashable default target leaves the physical VCAP ADC disabled. The
no-EVB-solder-modification hookup uses J9 pin 10 / GPIO15 / ADCSE4. On Rev. 2.0,
that net also reaches U15 ON2 through fitted SB60 and has a fitted 1 Mohm
pull-down at R51. Keep VCAP at or below 9.04 V during this validation and verify
the ADC node stays below 0.5 V. The build fails if a physical ADC read is
requested before the explicit `BISEN_VCAP_ADC_PIN_CONFIRMED=1` acknowledgment.

GPIO12 / ADCSE7 is rejected for VCAP sensing on the assembled board: its fitted
SB81 connection to the onboard J-Link VCOM receiver pulled a nominal 0.265 V
divider node to 1.614 V and saturated the 1.19 V ADC reference.

The nominal VCAP conversion implements the specified 1.000 Mohm upper
resistor, measured 55.8 kohm lower resistor, and the board's fitted 1 Mohm R51
pull-down: `VCAP_nom = ADC volts * 19.921146953`. It uses a 5 ms
divider-settle delay, 12-bit/16-sample general ADC scan, and the Apollo4 Plus
1.19 V internal ADC reference. A nominal physical reading above either the
0.5 V shared-pin limit or the 9.04 V validation ceiling fails closed before
energy policy selection.

Policy decisions use a separate, fixed-point bench calibration derived from
every corrected ADC sample at DMM-confirmed 5.00, 5.70, 6.10, 7.50, and
9.00 V VCAP. The threshold-focused least-squares fit is
`VCAP_policy_mV = round(5.680275 * corrected_code + 213.104123)`. Keeping the
two values separate is intentional: `VCAP_nom` retains the component/reference
safety check, while `VCAP_policy` makes the exact integer-millivolt scheduler
thresholds correspond to this EVB's measured ADC behavior. The firmware logs
both values and never applies the calibration to the ADC-node or maximum-range
safety gates.

## Builds

```sh
make EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0
```

For the confirmed J9 pin 10 physical hookup, build with the ADC gate enabled
and low-power behavior still disabled:

```sh
make -B EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_LOW_POWER=0
```

The non-hardware simulation below exercises policy, real internal die
temperature sensing, two-slot MRAM programming, CRC/commit validation, and
the resumable 64x64 Sobel workload without configuring any external ADC pin:

```sh
make -B EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_TEST_VCAP_MV=9000 BISEN_ENABLE_LOW_POWER=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 BISEN_ENABLE_COMPUTE_WORKLOAD=1
```

The exact chunk-policy boundaries and deterministic workload can be
checked on the host without hardware:

```sh
c++ -std=c++11 -Wall -Wextra -Werror \
  -DBISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  -Iapps/bisen_port/src \
  apps/bisen_port/src/bisen_policy.cc \
  apps/bisen_port/tests/bisen_policy_host_test.cc \
  -o /tmp/bisen_policy_host_test && /tmp/bisen_policy_host_test

c++ -std=c++11 -Wall -Wextra -Werror -Iapps/bisen_port/src \
  apps/bisen_port/src/bisen_compute.cc \
  apps/bisen_port/tests/bisen_compute_host_test.cc \
  -o /tmp/bisen_compute_host_test && /tmp/bisen_compute_host_test
```

## Three-bit GPIO state instrumentation

The Rev. 2.0 schematic-confirmed instrumentation bus preserves the MSP430
BISen three-bit debug encoding. It is disabled by default and uses the
R4.5.0 HAL's lowest-drive-strength push-pull GPIO configuration when enabled:

| Bit | Apollo GPIO | Rev. 2.0 header |
|---|---:|---:|
| `STATE0` / LSB | 62 | J9 pin 7 |
| `STATE1` | 63 | J9 pin 9 |
| `STATE2` / MSB | 61 | J9 pin 11 |

The codes are `0=sleep`, `1=VCAP ADC`, `2=temperature`, `3=compute`,
`4=MRAM write`, `5=checkpoint committed`, `6=context restore/validation`, and
`7=boot/error`. Codes 5-7 are event markers and do not add delay loops. The
three pins briefly pass through zero while the GPIO bank changes codes; decode
stable intervals rather than treating that sub-instruction transition as a
sleep event.

First validate the state bus with SWO still enabled:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_LOW_POWER=1 BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=1 \
  BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT=50 \
  BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT=3 \
  BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_GPIO_INSTRUMENTATION=1 BISEN_ENABLE_DEBUG_LOGGING=1
```

Connect only high-impedance oscilloscope or logic-analyzer inputs plus a common
ground. The GPIO high level follows the externally regulated MCU rail (about
1.90 V in the validated setup), so confirm the instrument accepts that logic
level. Correlate codes with SWO before using the GPIO-only build. After that
passes, rebuild the same command with `BISEN_ENABLE_DEBUG_LOGGING=0`; the
integrated-cycle gate permits logging-off operation only because GPIO
instrumentation remains enabled.

The production scheduler deliberately performs no stable-high-energy or
temperature-only checkpoint. The separately gated tests below remain only for
backend/atomicity regression and intentionally force writes outside the
production energy policy. Do not use their traces as evidence of production
checkpoint timing.

### ADC FIFO/register diagnostic

If both GPIO15/ADCSE4 and the internal temperature channel return code zero,
use the isolated diagnostic build below. It forces MRAM programming, compute,
and RTC sleep off, uses the production LPMODE1, and prints the ADC configuration,
FIFO count/data, interrupt, peripheral-power, and analog-power registers around
each trigger. The diagnostic follows the installed neuralSPOT Apollo4
`adc_trigger_wait()` behavior by retrying a software trigger after the SDK's
1 us interval while the FIFO remains empty, but bounds the experiment at 32
attempts. It also observes `CNVCMP` separately from `SCNCMP`. The ADC helper
rejects an empty hardware FIFO instead of allowing it to be interpreted as a
valid slot-0/code-0 sample.

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_DEBUG_LOGGING=1 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_ADC_DIAGNOSTIC=1 \
  BISEN_ENABLE_LOW_POWER=0 BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=0 BISEN_ENABLE_COMPUTE_WORKLOAD=0 \
  BISEN_FORCE_DISABLE_MRAM_PROGRAMMING=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_RETENTION_TEST=0 BISEN_ENABLE_GPIO_INSTRUMENTATION=1
```

After opening SWO, press reset once and capture every `BISen ADC regs` line.
No BTN0 press is used in this one-shot diagnostic.

After that test passes, the separately gated alternation build requests one
benign checkpoint rewrite per manual reset. It refuses to compile if compute,
low power, reset injection, or logging-off mode is selected:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=1 \
  BISEN_ENABLE_COMPUTE_WORKLOAD=0 BISEN_ENABLE_LOW_POWER=0 \
  BISEN_ENABLE_RESET_INJECTION=0
```

Because deploy starts the target before the viewer attaches, it will normally
create slot 0 / sequence 1 unseen. The next two manual resets should restore
slot 0 / sequence 1 and save slot 1 / sequence 2, then restore slot 1 /
sequence 2 and save slot 0 / sequence 3. Do not repeatedly reset after those
two captures; return to the normal build to avoid needless MRAM writes.

After stable-power alternation passes, phase 2 of the one-shot reset test
programs the inactive payload with an uncommitted final block and immediately
resets. The next boot must retain sequence 1, report the incomplete sequence-2
slot, and park without retrying:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_COMPUTE_WORKLOAD=0 BISEN_ENABLE_LOW_POWER=0 \
  BISEN_ENABLE_RESET_INJECTION=1 BISEN_RESET_INJECTION_PHASE=2
```

Only after phase 2 passes, rebuild the same command with phase 3. That image
resets after the final commit block but before final readback; the next boot
must restore slot 1 / sequence 2. Phase 2 and phase 3 use distinct record
epochs, so one phase cannot accept residue left by the other after reflashing.
The reset-test gates reject phases other than 2 or 3 and reject alternation,
compute, low power, or logging-off combinations.

After both reset phases pass, the stable-power compute-resume test processes
exactly one energy-policy-sized Sobel chunk per boot, commits that progress,
and parks. It uses a separate checkpoint epoch, so residue from the preceding
checkpoint tests is rejected. Low power and reset injection remain off:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=1 BISEN_ENABLE_LOW_POWER=0 \
  BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0
```

At 7.5 V VCAP the exact 500-pixel plan is selected. Deploy normally performs
the initial temperature checkpoint and first 500-pixel chunk before SWO
attaches. Start the viewer and press reset once: the application should restore
progress 500, run to 1000, save the next sequence in the alternate slot, and
print `one persisted chunk completed` before parking. On each subsequent
manual reset, the restored progress and digest must exactly equal the prior
boot's saved chunk result. Progress should advance 500 pixels at a time, with
the final short chunk advancing from 3500 to 3844. Final output must report
digest `8bdd7454` matching golden. One more reset must restore progress 3844
and park without printing another Sobel chunk, proving completion is not
duplicated. Stop after that confirmation to avoid needless MRAM rewrites.

The workload refuses malformed progress (`output_progress` must equal the
Sobel index, completion state must be internally consistent, and completed
state must carry the golden digest). A failed checkpoint save parks the target
instead of continuing with unpersisted computation.

The VCAP and die-temperature states share an interrupt-driven R4.5.0 general
ADC helper. With `AVG_16`, each LPMODE1 software trigger sleeps the CPU in
normal sleep until scan-complete; the helper repeats the installed neuralSPOT
1 us trigger/FIFO sequence until the averaged sample is emitted. The ISR
records completion/error status only, and the scheduler reads the FIFO in
thread context after invalidating DAXI. Both paths discard the first averaged
sample after ADC wake. The temperature path then uses the factory-trim
`AM_HAL_ADC_REQ_TEMP_CELSIUS_GET` conversion. This mirrors the MSP430 design:
interrupt-driven conversion with scheduling outside the ISR.

If a DMM confirms voltage at J9 pin 10 / GPIO15 while the production LPMODE1
path reports ADC code zero on both ADCSE4 and the internal temperature channel,
use the isolated diagnostic below. It performs three LPMODE1 ADCSE4 samples,
BSP and read-back pad configuration, then the internal temperature channel
through the same ADC core and acquisition sequence used by production.
MRAM is forcibly disabled; compute, RTC, BTN0, and all other test modes are
compile-gated off:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_DEBUG_LOGGING=1 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_ADC_DIAGNOSTIC=1 \
  BISEN_ENABLE_LOW_POWER=0 BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=0 BISEN_ENABLE_COMPUTE_WORKLOAD=0 \
  BISEN_FORCE_DISABLE_MRAM_PROGRAMMING=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_RETENTION_TEST=0 BISEN_ENABLE_GPIO_INSTRUMENTATION=1
```

The preceding LPMODE0 v4 audit established that `AVG_16` needs 16 software
triggers on this target: attempts 1 through 15 completed scans with an empty
FIFO, and attempt 16 produced `SCNCMP|CNVCMP` plus a FIFO sample on both SE4 and
TEMP. The v5 hardware run verified that exact pattern again in production
LPMODE1, with valid external readings and a 27.31 C die-temperature result.
Production therefore uses an exact 16-trigger bound for its two `AVG_16` slots;
it does not use an estimated or unbounded retry policy. A nonzero temperature
code with zero ADCSE4 codes isolates the external pad/channel path. Zero on
both channels isolates common ADC setup, clock, power, interrupt, or FIFO
behavior. This diagnostic is not a production or energy-measurement image.

The subsequent non-diagnostic, MRAM-off one-shot image also passed. With DMM
readings of 7.41 V at VCAP and 0.372 V at J9 pin 10, the firmware reported
0.3684 V / 7.3387 V and 0.3655 V / 7.2808 V on its two measurements, followed
by a valid 27.31 C die-temperature result and safe parking. The measured
divider ratio is 19.919 versus the configured 19.921, so divider scaling is
confirmed. The remaining approximately 1.0 to 1.75 percent ADC-reading error
motivated the multi-point, repeated bench calibration described above; it was
not corrected by changing the documented ADC reference or physical-divider
model.

The v6 diagnostic changes the external portion to 16 independent production-
style measurements per reset and reports valid count, mean corrected code
scaled by 1000, minimum, maximum, and spread. It prints full register snapshots
only for samples 1 and 16, then checks temperature once and parks. The hardware
runs produced 16/16 valid samples at the DMM 5.70 V calibration point (mean
966.938, range 946--980) and the 6.10 V resume boundary (mean 1038.688, range
1021--1056). Together with the prior three readings at each point, the
calibration dataset contains 19 samples per voltage.

The calibrated, MRAM-forced-off one-shot image was then bracket-tested with
three manual resets at each DMM-confirmed voltage. The original scheduler's
now-removed 5.7 V checkpoint gate classified 5.60 V below and 5.80 V above that
gate. At 6.00 V all three readings deferred restore. At 6.20 V two readings
allowed restore and one conservatively deferred at
`VCAP_policy=6.092 V`, 8 mV below the exact 6.100 V comparison. Refitting all
65 accumulated samples changes that result by only about 2 mV, so the existing
calibration coefficients and resume threshold remain fixed. The 5.70 V data
remain calibration evidence; they are no longer a checkpoint trigger.

MRAM writes and compute are compile-gated off by default so this temperature
milestone parks after the internal measurement. Enable them only in their
later validation milestones with `BISEN_ENABLE_MRAM_CHECKPOINTS=1` and
`BISEN_ENABLE_COMPUTE_WORKLOAD=1`, respectively.

For the first RTC deep-sleep validation, use the bounded three-wake test below.
It requires the confirmed physical ADC and logging, and refuses to compile if
MRAM, compute, reset injection, or checkpoint alternation is enabled:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_LOW_POWER=1 BISEN_ENABLE_LOW_POWER_TEST=1 \
  BISEN_LOW_POWER_TEST_WAKE_LIMIT=3 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=0 BISEN_ENABLE_COMPUTE_WORKLOAD=0 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0
```

The implementation uses the R4.5.0 LFRC RTC, its one-second repeating alarm,
the `RTC_IRQn` vector, and neuralSPOT's Apollo4 `ns_deep_sleep()` wrapper. That
wrapper disables the active ITM transport and powers crypto down before calling
the HAL with `AM_HAL_SYSCTRL_SLEEP_DEEP`; the first post-wake log restores SWO.
The RTC interrupt is explicitly cleared, enabled in the NVIC, and globally
unmasked. RTC setup and ISR failures report the exact stage and HAL status, and
a non-RTC interrupt causes a fail-closed unexpected-wake report.

On stable VCAP, the application measures temperature once, then must report RTC
wake 1, 2, and 3 with a fresh VCAP reading after each wake. It parks after the
third reading without entering a fourth sleep. This validates functional deep
sleep and retained scheduler state; it does not yet claim minimum sleep current
because the known-good neuralSPOT development power profile remains selected.
Do not disconnect J-Link or alter the EVB power configuration for this test.

After the bounded RTC test passes, the integrated stable-power image combines
the confirmed ADC path, energy policy, die temperature, two-slot MRAM,
resumable Sobel workload, BTN0, and the one-second LFRC wake source. Runtime
always starts in bounded mode. Bounded mode performs exactly one selected chunk
per wake, remeasures VCAP immediately afterward, and retains progress in DTCM
through normal RTC sleep. It does not checkpoint temperature, ordinary chunks,
or Sobel completion. A separate checkpoint epoch rejects the former eager
per-chunk/completion records.

The integrated scheduler now follows the control flow in the MSP430 `main.c`:
after every useful state it remeasures energy, determines whether the next
useful state may run, and, if not, saves dirty incomplete recovery progress
before entering the RTC energy-retry sleep. There is no independent checkpoint
voltage gate between "cannot work" and "checkpoint." Completed-job state
remains in retained DTCM and the RAM event ring only; normal completion does
not program MRAM.

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_VCAP_ADC=1 BISEN_VCAP_ADC_PIN_CONFIRMED=1 \
  BISEN_ENABLE_LOW_POWER=1 BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=1 BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=1 \
  BISEN_INTEGRATED_CYCLE_TEST_WAKE_LIMIT=50 \
  BISEN_BOUNDED_NO_PROGRESS_WAKE_LIMIT=3 \
  BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0
```

At stable high VCAP, every intermediate chunk and golden completion must
produce zero code-4/code-5 events. In bounded mode the expected sequence is
compute code 3, ADC code 1, then RTC sleep code 0; that laboratory-imposed
sleep retains adequate-energy progress in DTCM and does not request MRAM. In
continuous mode, an incomplete dirty context enters code 4 immediately when
the post-state ADC says the next useful state cannot run. A successful
validated save emits code 5 and marks that progress clean. Without new compute,
remaining energy-limited cannot issue another save. After a
real power interruption, a qualified boot emits code 6 while restoring the
last committed progress and digest, then recomputes any work performed after
that checkpoint. Completing Sobel does not update or invalidate the older MRAM
record, so such rollback is deliberate.

The BTN0/SW1 interrupt remains disabled while the initial bounded job runs. Once
that job completes and parks, or after three consecutive RTC wakes make no
compute progress under an otherwise valid energy-policy result, firmware must
observe BTN0 stably released before it arms GPIO17 and prints `BISen BTN0
armed`. The short no-progress bound avoids waiting for the 50-wake hard ceiling
when VCAP is below the exact 100-pixel threshold. It does not bypass policy:
continuous mode continues to measure VCAP and will not compute or write a dirty
checkpoint until the same thresholds authorize those actions. Press BTN0 only
after the armed message;
the IRQ is treated as a candidate and GPIO17 must remain physically low across
a 20 ms debounce interval before a one-way volatile transition to continuous
mode. Boot-time levels, configuration-time edges, GPIO glitches, and presses
during the bounded job are deliberately discarded, so reset cannot select an
unbounded run. Continuous mode remeasures
VCAP after every compute chunk, starts the next numbered job automatically
after golden completion, and sleeps on the RTC only when policy does not permit
useful work. Reset always returns to a new bounded job. The button selection is
intentionally not checkpointed, so an intermittent-power reboot cannot silently
resume an unbounded laboratory run.

The qualitative policy names have been removed. Exact plans and provisional
stable-bench boundaries are:

| Falling VCAP plan/action | Bench threshold |
|---|---:|
| 1000-pixel chunk | `>= 8.5 V` |
| 500-pixel chunk | `>= 6.4 V` |
| 100-pixel chunk | `>= 5.9 V` |
| checkpoint incomplete dirty progress | immediately before an energy-driven sleep when no next useful state is allowed |
| explicit sleep request | `< 5.5 V` |
| permit boot-time restore and temperature | `>= 6.1 V` |

Chunk transitions use direct comparisons against the latest calibrated VCAP
measurement, matching the MSP430 reference; the previous plan is not retained.
The former 7.3 V boundary was removed because both adjacent branches selected
the same 500-pixel chunk and therefore represented no scheduler behavior.
Between 5.5 V and 5.9 V, no new compute chunk is eligible. If incomplete
progress is dirty, the scheduler checkpoints it once before RTC sleep; if the
context is already clean, it sleeps without another MRAM write. The separately
reported `below_sleep_floor` action becomes true only below 5.5 V.

## Persistent checkpoint layout

`bisen_checkpoint.ld` includes the normal Apollo4P linker script and reserves
two 80-byte slots as a `NOLOAD` section immediately after this app's loadable
MRAM image. It has no fixed address and is excluded from the `.bin`. Verify
the ELF/map each time the linker or application layout changes before allowing
MRAM writes. The record has a CRC and a final 16-byte commit block, so an
interrupted inactive-slot update does not supersede the previously committed
slot. The firmware rejects a storage region that is not exactly two aligned
record slots or that lies outside the R4.5.0 Apollo4 Plus MRAM range. Each
16-byte-multiple program operation uses the interrupt save/restore sequence
shown by AmbiqSuite's Apollo4 persistence examples, followed by DAXI flush,
MRAM-cache invalidation, and readback validation. Runtime SWO output reports
the linker-resolved half-open region, selected slot, and sequence number.

This remains a separate target and does not modify `apps/basic_tf_stub`, the
secure bootloader, or the repository-wide linker configuration.

## Current validation record

As of 2026-08-14, using `apollo4p_blue_kxr_evb` and AmbiqSuite R4.5.0:

The checkpoint entries below record the earlier explicit eager-write
regression images. The current production scheduler uses a new record epoch,
rejects those records, and has not yet received on-target validation of the
new low-energy-only trigger.

- GPIO15 / ADCSE4 reconstruction passed against DMM readings at 5.02 V,
  7.03 V, and 9.00 V VCAP;
- the scheduler additionally selected 100 pixels at 6.20 V and 500 pixels at
  7.50 V, completing bench coverage of the previously implemented runnable
  policy bands;
- internal die-temperature measurements passed at 27.99, 28.07, and 28.24 C;
- a committed slot 0 / sequence 1 checkpoint was restored on two successive
  manual resets, with completed temperature work correctly skipped and no
  unintended sequence increment in the normal parked build;
- stable-power alternation selected and verified slot 1 / sequence 2 and then
  slot 0 / sequence 3 on successive manual resets;
- the phase-2 atomicity test produced system-reset status `0x8`, restored the
  committed slot 0 / sequence 1 record, detected and rejected the uncommitted
  slot 1 / sequence 2 payload, and parked without retrying;
- the phase-3 atomicity test produced system-reset status `0x8` and restored
  the newly committed slot 1 / sequence 2 record after resetting before final
  readback, completing the two-slot checkpoint fault matrix;
- deterministic host tests passed for 100-, 500-, 1000-, and 3844-pixel Sobel
  chunking, a copied checkpoint/resume context, golden digest `8bdd7454`, and
  malformed-context rejection;
- the gated stable-power compute-resume R4.5.0 cross-build passed; its
  29,892-byte binary ends at `0x0001f4c4`, and the runtime checkpoint slots
  occupy `[0x0001f4d0, 0x0001f570)` as a zero-file-size `NOLOAD` region;
- the first on-target stable-power workload resume passed at 7.50 V VCAP: the
  500-pixel plan restored slot 1 / sequence 2 at progress 500 with digest
  `25e20392`, advanced to progress 1000 with digest `c575a53f`, committed slot
  0 / sequence 3, and parked with low power disabled;
- subsequent stable-power resets restored and advanced the deterministic
  workload through progress/digest pairs 1500/`1fa608a5`, 2000/`38cd86de`,
  2500/`b092609c`, 3000/`170f4a3c`, and 3500/`a37f0a84`; every restored digest
  exactly matched the preceding boot's persisted result and slots alternated
  through slot 1 / sequence 8;
- the final 344-pixel chunk reached exactly 3844 pixels with digest `8bdd7454`,
  matched the host golden result, committed slot 0 / sequence 9, and parked;
  a final reset then restored that exact completed record and parked without
  entering the compute state or issuing another MRAM write, proving completed
  work is neither duplicated nor unnecessarily checkpointed;
- the compute-resume ELF is 29,820 bytes text, 72 bytes data, and 22,696 bytes
  BSS;
- the bounded LFRC/RTC deep-sleep image passed its R4.5.0 cross-build with a
  strong `am_rtc_isr` and linked neuralSPOT Apollo4 deep-sleep wrapper; its
  30,292-byte binary ends at `0x0001f654`, with the unused `NOLOAD` checkpoint
  reservation remaining outside the binary at `[0x0001f660, 0x0001f700)`;
- the bounded deep-sleep ELF is 30,220 bytes text, 72 bytes data, and 22,704
  bytes BSS; its on-target test passed three consecutive LFRC RTC wakes, fresh
  VCAP measurements after every wake, retained temperature completion, and
  bounded parking without any MRAM or compute activity;
- the integrated-cycle R4.5.0 cross-build passed with strong RTC/deep-sleep
  symbols; its 32,676-byte binary ends at `0x0001ffa4`, followed by a 12-byte
  gap and two 80-byte `NOLOAD` checkpoint slots at
  `[0x0001ffb0, 0x00020050)`. Its ELF is 32,604 bytes text, 72 bytes data, and
  22,704 bytes BSS;
- the on-target integrated stable-power test passed at the 500-pixel operating
  point. Boot restored slot 1 / sequence 2 at progress 500 with digest
  `25e20392`, immediately advanced and committed progress 1000, then completed
  six LFRC RTC sleep/wake cycles with a fresh VCAP measurement after every
  wake. Checkpoints alternated through slot 0 / sequence 9, every save reported
  status 1 and layout 1, and the final 344-pixel chunk reached progress 3844
  with golden digest `8bdd7454`. The target reported integrated completion at
  wake count 6 and parked without another sleep;
- the compile-gated three-bit GPIO instrumentation image passed clean R4.5.0
  integrated-cycle rebuilds both with SWO logging enabled and with GPIO-only
  observability. The GPIO-only ELF is 28,776 bytes text, 44 bytes data, and
  22,700 bytes BSS; its 28,820-byte binary retains strong instrumentation
  entry points;
- checkpoint-enabled, checkpoint-disabled, and preserved `basic_tf_stub`
  clean rebuilds passed;
- the checkpoint-enabled image starts at `0x00018000`, its 30,300-byte binary
  ends at `0x0001f65c`, and its linker-resolved slots occupy
  `[0x0001f660, 0x0001f700)` as a zero-file-size `NOLOAD` segment;
- the checkpoint-enabled ELF size is 30,228 bytes text, 72 bytes data, and
  22,688 bytes BSS.
- the gated alternation-test build also passed; its 30,416-byte binary ends at
  `0x0001f6d0`, immediately before its linker-resolved
  `[0x0001f6d0, 0x0001f770)` two-slot region.

The full rebuilds still emit pre-existing neuralSPOT/AmbiqSuite codec and heap
warnings; no warning originates from `apps/bisen_port`.

## Still intentionally gated

- capacitor-discharge behavior and energy-safe write thresholds require real
  voltage/current characterization; the present thresholds are bench policy,
  not Apollo4 electrical limits;
- minimum-current configuration and measurement are later work;
- final capacitor-discharge behavior with the intended low-consumption
  converter remains uncharacterized;
- the GPIO instrumentation mapping and decoder have been confirmed on-target;
  final energy claims still require captures made with the intended converter.

## Scope-visible retained-RAM experiment

The normal retained-RAM proof deliberately performs only one 100-pixel Sobel
chunk per RTC wake. Apollo4 completes that real work quickly enough that a
long-record scope acquisition can miss the state-3 interval. The optional
`BISEN_RETENTION_TRACE_WORKLOAD_REPEATS` gate executes additional complete,
golden-verified 3,844-pixel Sobel workloads while code 3 remains asserted.
It does not stretch the GPIO with a delay, modify retained progress, or touch
MRAM. Because it adds test-only compute work, its timing and energy are not
production measurements.

This bounded 20-wake build provides repeated scope-visible activity while
retaining the original RAM-retention checks and forcing MRAM programming off:

```sh
make -B deploy \
  EXAMPLE=bisen_port PLATFORM=apollo4p_blue_kxr_evb AS_VERSION=R4.5.0 \
  BISEN_ENABLE_DEBUG_LOGGING=1 \
  BISEN_ENABLE_VCAP_ADC=0 BISEN_VCAP_ADC_PIN_CONFIRMED=0 \
  BISEN_ENABLE_LOW_POWER=1 BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=0 BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_GPIO_INSTRUMENTATION=1 BISEN_LOG_BACKEND=1 \
  BISEN_MRAM_SESSION_ATTEMPT_LIMIT=64 \
  BISEN_FORCE_DISABLE_MRAM_PROGRAMMING=1 \
  BISEN_ENABLE_RETENTION_TEST=1 BISEN_RETENTION_TEST_WAKE_LIMIT=20 \
  BISEN_RETENTION_TRACE_WORKLOAD_REPEATS=64
```

Expected GPIO codes are one startup code 7, twenty code-3 real-compute bursts,
and code 0 during each one-second RTC sleep and after the bounded PASS. Codes 4
and 5 must never appear. If a code-3 burst is still narrower than the chosen
scope record interval, increase only the compile-time repeat count and rebuild;
do not add an instrumentation delay or enable MRAM.
