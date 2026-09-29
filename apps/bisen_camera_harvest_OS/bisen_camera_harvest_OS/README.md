Output-stationary package, engine-r2a, 2026-09-29

This is 080-r2 plus nn_abandon, using your weights and checkpoint code.
It selects NN_DATAFLOW=0 NN_SIMD=1 NN_MAX_CONV_IN_C=6. CMAX is retained
for the LeNet build contract; it limits IS convolution capacity, not OS.

Apply

Back up apps/bisen_camera_harvest first. Put bisen_camera_harvest_OS in
its place, renamed to bisen_camera_harvest. The build and linker paths
require that name. README-original.md preserves your application notes.
Use your measured calibration flags; this package changes none of them.
Force a rebuild when switching engines so old objects cannot be reused:

  make -B EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 <your calibration flags>

For a source-only update, copy all eight deliver-r2a-OS/src files into
your src directory. Replace any previous engine-selection line with the
line in module.mk.add, before bindirs. Change the existing default to:

  BISEN_HARVEST_CHECKPOINT_MAGIC ?= 0x4F533031

Keep the existing CKPT_MAGIC=$(BISEN_HARVEST_CHECKPOINT_MAGIC)u line.
Do not add a second CKPT_MAGIC definition.

Checkpoint identity

OS uses 0x4F533031 (OS01): version 01 of this engine-r2a OS package,
with this exact quantized model and checkpoint interpretation. For an IS
rebuild of the September 28 package, retain its IS build line and add:

  BISEN_HARVEST_CHECKPOINT_MAGIC=0x49533031

For example, on the IS tree:
  make -B EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 BISEN_HARVEST_CHECKPOINT_MAGIC=0x49533031 <your calibration flags>

IS01, OS01 and the older HVR1 default are distinct. Omit the trailing u
on command-line values: module.mk supplies it. Version the identity again
when model bytes or checkpoint interpretation change, not just when a
variant gets a new name.

Switching variants is not a resume operation. A different identity rejects
old inference, scan and retirement records and the old continuous-session
armed journal. A new identity starts disarmed unless it already has its
own valid journal. Re-arm continuous execution when appropriate. Magic
rejects records; it does not erase them. Switching back can expose surviving
old records, although flashing/layout changes can overwrite them.

The unchanged linker and README-original.md still mention HVR1. The active
OS identity is the module.mk setting above. Compatibility isolation is not
corruption hardening: the existing restore path does not validate a
same-identity cursor before deriving live regions.

Storage stays unchanged: this model's NN_LIVE_MAX is 8,064 bytes for both
engine-r2a dataflows; each checkpoint slot is 8,080 bytes, with two slots
reserving 16,160 bytes, plus the existing 32-byte session journal. A smaller
OS live payload is not permission to shrink the reservation.

What is checked

RAN (Python, preparation): verify_os_package.py checks the exact file/build
allowlist, all 40 untouched files in the September 28 46-file source record,
the eight engine files, zip entries/content and SHA-256 receipt. Its planted
file, manifest, build and archive faults must be rejected for their stated
reasons. README-original.md and the trace are checked against the old zip.
The original run had local changes; a commit identifier alone is not the
source identity. See the original-source record used by the verifier.

READ (September 28 run.log, not a new OS-package run): engine-r2a OS passed
24,279 interior restore positions on three inputs, and matched all ten
score bytes on all 100 inputs against your frozen engine. That old sweep
did not test position zero or completion. Its corruption controls, host
tests and four ARM builds were IS, not OS evidence.

ASSUMED until helper runs accept_os.sh: this exact extracted OS archive
passes ASan/UBSan at 24,279 interior positions plus three position-zero and
three completion records, exact ten-score equality on 100 inputs, your two
host tests, diagnostic-specific controls and all four ARM configurations.
The acceptance script requires explicit expected/observed counts. Completion
is a checkpoint-codec test, not application-level recovery of a delivered
prediction. Native timing is not Apollo4 timing. API-test compilation is not
an on-device API-test pass.

No board run, physical MRAM save/restore or energy measurement was done for
this package. Energy adequacy remains unmeasured. Do not distribute it as
accepted until helper has produced a successful native/ARM receipt.

Timing in your log

READ: infer_step accumulates STIMER intervals around each nn_step call.
The printed inference time includes repeated engine-call overhead. It
excludes gaps between chunks, scheduler energy sampling and waits,
checkpoint writes, camera scanning, preprocessing and nn_begin. A cold
restore does not recover the old timing accumulator, so this is not total
work across boots and not end-to-end latency.

For a comparison, use fresh uninterrupted jobs at the same clock. Record
the model, engine, chunk setting and cold/warm status. Prior reference only:
080-r2, our LeNet bench, 192 MHz: OS 11.66 ms with tile 8 and 9.60 ms whole;
IS 14.90 ms with tile 8 and 13.64 ms whole. These are not measurements of
your packaged application or promises for your quantization and scheduler.
Source: paperbase review 093, section 5, citing RESULT-R2-BOARD-2026-09-28.md.

Chunks (engine-r2a, your LeNet shapes)

Fixed inference budget    OS: 8,094 units    IS: 2,318 units
100 units/call           81 calls          24 calls
500 units/call           17 calls           5 calls
1,000 units/call          9 calls           3 calls

RAN: the table is ceil(total/budget), for a fixed band without interruption.
Units are backend-specific work, not milliseconds or joules. Equal policy
thresholds therefore give different energy-observation intervals in OS and
IS. Camera work remains separately capped at the existing default of one
pixel. No thresholds or calibration values were changed.

Helper handoff (outside the app zip)

Run bash accept_os.sh from this directory in WSL. It verifies and extracts
the zip first; both native tests and ARM builds use the extracted sources.
The default BASELINE_APP is the original coworker WSL-mounted app. Its five
engine files and weights must match the frozen September 28 source record;
a changed checkout is refused, not silently used as a new oracle. The newly
run frozen-engine scores must also match the archived September 28 scores.
Set BASELINE_APP to a preserved copy of those bytes if needed.

NS defaults to /home/bobbylabonite/neuralSPOT. G defaults to
/mnt/c/Users/bobby/Documents/Camobs/neuralSPOT. These are read-only inputs:
the runner copies the neuralSPOT tree (without build or .git directories)
inside its new run folder before ARM work. It uses G's base linker as the
September 28 harness did; its hash is recorded. Only the scratch linker
INCLUDE is redirected. The reserved sizes and NOLOAD section are checked.
This needs disk space for that SDK copy, gcc, make, Python and the ARM tools.
A missing dependency fails the run; no compiler or network install is done.

Run directories and all scratch work stay below this output folder. Native
runs may take several minutes. The helper must run them in an executable
WSL environment. For local preparation only:

  python3 verify_os_package.py --self-test
  bash accept_os.sh --compile-only

Compile-only does not execute the native tests or ARM builds and never
prints the full acceptance PASS marker. The compile receipt lists warnings.
The full runner records commands, return codes, log and artifact hashes.
No deploy/flash command is run.
