# Harvest state-DAC mapping and MSP430 comparison

Updated 2026-09-29. Applies to `apps/bisen_camera_harvest`,
`apps/bisen_camera_harvest_IS`, and the nested
`apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS` package.
This is source behavior, not confirmation of the image installed on the board.

| DAC code | Historical MSP430 Sobel reference | Current Apollo4 harvest sources |
|---|---|---|
| 0 | Inactive/energy wait: LPM3 timer sleep or LPM3.5 standby below the run floor; also LPM4 before the start button | Inactive/energy wait: normal CPU sleep by default, or spin fallback; also after successful writes |
| 1 | Supply ADC | External reservoir VCAP ADC |
| 2 | Temperature sensing | 32×32 camera/pixel sensing |
| 3 | Chunked Sobel compute | Preprocessing / incremental CNN, OS or IS; also a brief leave-wait marker |
| 4 | FRAM-write work state | MRAM save/write, including commit-header programming |
| 5 | Explicit brief FRAM checkpoint-complete pulse after persistence (debug notification, not a scheduled work state) | Successful-write completion notification, held until the next marked activity; not a second write phase |
| 6 | Context-restored notification | Context-restore marker (coverage caveat below) |
| 7 | Boot / error | Boot / error |

These codes mix activities and events; they are not eight separate scheduler
states. Both systems select useful work according to measured voltage and
preserve progress before low-energy waits. Their workloads, storage mechanisms,
thresholds and work-unit meanings differ; matching codes do not prove equivalent
energy behavior. MSP430 evidence here is the inspected current local
`/Users/ghart/workspace_v12/Image_Sobel_5969/main.c`; it does not identify
which binary was flashed for any particular archived capture.

## Wait and physical sleep behavior

The MSP430 reference has an explicit energy wait: `main()` reads VCC, then
calls `sleep_until_more_energy()` if no useful work is permitted. This routine
uses LPM3 timer waits to periodically recheck energy; if VCC is below the
2.0 V run floor and LFXT is ready, it saves dirty progress and enters
LPM3.5 (`PMMREGOFF`) with a one-second RTC wake. If LFXT is unavailable,
it falls back to LPM3. A separate pre-start S2 button wait uses LPM4. These
are distinct behaviors that all appear as DAC code 0 or cleared pins. The
FRAM checkpoint notification is an explicit brief code-5 pulse after
`persist_runtime_context()`; it is not a second scheduled checkpoint phase.

The Apollo4 harvest implementation also has an energy wait. When work is
forbidden, the scheduler may save dirty work, then `wait_for_energy()` marks
code 0, calls `pwr_wait_us(BISEN_CAMERA_WAIT_US)` (250 ms by default), samples
VCAP and repeats until it may resume. `pwr_wait_us()` verifies an STIMER
compare wake before sleeping; on failure it spins rather than risking an
unwakeable wait. The active app modules set `BISEN_CAMERA_SCAN_IDLE_MODE=1`,
so the wait uses `AM_HAL_SYSCTRL_SLEEP_NORMAL` when the self-test passes.
The same helper also serves camera pixel settling and bounded inference pauses.
`BISEN_HARVEST_MCU_LOW_POWER=0` selects the performance run mode; it does not
turn off these waits.

Apollo4 deep sleep is implemented as optional `SCAN_IDLE_MODE=2`, which calls
`AM_HAL_SYSCTRL_SLEEP_DEEP`. It is not selected in the current build. Its
STIMER wake depends on HFRC, which may be gated in deeper sleep; the awake
self-test does not establish reliable deep-sleep wake. The code's estimated
deep-sleep current is not a board measurement. An Apollo4 deep-sleep option
must not be equated automatically with the MSP430's LPM3.5 power-off standby:
that path is not currently implemented as an energy-threshold-controlled
Apollo4 hardware mode. Actual full-power loss and MRAM recovery are separate.

The BISen paper distinguishes a retained-context Stop state from a more
severe Standby state that loses volatile state. The current Ambiq firmware
implements a retained-context low-energy wait and conditional MRAM save,
while real power removal is recovered via boot and MRAM. Its chosen sleep
mode and state durations must be verified electrically before claiming the
paper's Stop or Standby energy behavior.

## Successful writes

`pp_mark_committed()` emits code 5 after a successful code-4 write. It does
not perform another checkpoint, and no instrumentation-only delay is added.
Unlike the MSP430's explicitly timed debug pulse, Ambiq's code 5 remains on
the bus until the next activity marker, so its observed width is not a fixed
commit-operation duration. Existing callers still track successful
checkpoints, update session/retirement bookkeeping and check failures. MRAM
payload/header ordering is unchanged. Code 5 can follow workload saves,
session arm/disarm and retirement, so it is not exclusively a workload
checkpoint counter. The code-4 interval includes success bookkeeping before
this marker.

The preceding edit that suppressed code 5 was superseded by the user's request
to restore the completion notification to match the MSP430 reference. Do not
renumber the other codes or rewrite archived captures. GPIO switching and
analog settling can produce transient intermediate levels; decode stable
intervals rather than interpreting every transition sample as deliberate.

## Energy measurement limits

The full save marker covers software and storage work, not just the MRAM array.
A separate HAL-program interval and matched control are needed for incremental
programming-energy estimates. The CNN restore marker currently starts after
`infer_init()` has performed storage recovery; it must be corrected before code
6 durations are used for full restore energy. That correction is not part of
this commit-event removal. Boot, energy waits, and decoupling recharge tails
must be treated explicitly in the eventual measurement protocol.

## Bench confirmation

Rebuild and flash the chosen variant. Capture a successful checkpoint and verify
code 4 ends at code 5 after a successful save, then yields to the next activity.
Verify the success count and restored output separately: code 5 alone cannot
prove cold-restore integrity, and session/retirement writes can also emit it. Compile/source
checks alone do not validate the DAC wiring, checkpoint energy or cold recovery.
