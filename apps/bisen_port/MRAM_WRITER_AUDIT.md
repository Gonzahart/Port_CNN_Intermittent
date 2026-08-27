# BISen MRAM Writer Audit — Pre-refactor Baseline

**Audit date:** 2026-08-19  
**Baseline banner:** `BISen run-mode guard v4`  
**Scope:** `apps/bisen_port`  
**Purpose:** Milestone 1 of the minimal-MRAM-write refactor

The untracked application was backed up before this audit to:

```text
/Users/ghart/workspace_v12/Image_Sobel_5969/backups/bisen_port_pre_minimal_mram_20260819.tar.gz
SHA-256: 0a2d4eb91a2c04e786f468c43aef49d900ffc78e8c4744a80c676d6796157313
```

This document records the writer topology before runtime behavior is changed.
Line numbers describe this baseline and may move during the refactor.

## Direct MRAM programming call sites

The complete source search found exactly one direct MRAM programming call:

```text
src/bisen_checkpoint.cc:178
am_hal_mram_main_program(...)
```

It is wrapped by the private `program_mram_blocks()` helper at lines 173–182.
No source file calls `am_hal_mram_main_words_program()`. No other Apollo MRAM
programming function call exists under `apps/bisen_port`.

Therefore, the pre-refactor application already satisfies the source-isolation
part of the production rule: only `bisen_checkpoint.cc` directly programs
MRAM. The endurance problem is scheduling frequency, not multiple independent
writer implementations.

## High-level call topology

There is exactly one high-level save call outside the checkpoint module:

```text
src/bisen_port.cc:410
bisen::save_checkpoint(scheduler->context, &info)
```

It is reached only through scheduler state
`State::kNonvolatileCheckpoint`. Entering that state sets instrumentation code
4 (`MRAM write`). A successful validated return sets code 5 (`checkpoint
committed`); any failure sets code 7 and parks fail-closed.

`restore_checkpoint()` reads, validates, and selects slots. It performs cache
maintenance and MRAM reads only; it does not program MRAM.

## Runtime paths that currently request a save

### 1. Temperature-complete path

After a successful die-temperature conversion, the scheduler marks temperature
complete and routes through `kNonvolatileCheckpoint` whenever MRAM is enabled.
This creates one logical checkpoint at progress zero for every newly created
job.

### 2. Every compute chunk

Every successful bounded Sobel chunk unconditionally routes through
`kNonvolatileCheckpoint`. With MRAM enabled, this eagerly persists every dirty
chunk boundary before the next ADC/RTC transition.

This is the dominant routine writer and is the path the refactor must remove.

### 3. Emergency low-energy path

After VCAP measurement, a dirty context with
`emergency_checkpoint_requested` routes to `kNonvolatileCheckpoint`. This path
is level-sensitive in the baseline; normal eager chunk saves often make the
context clean before it is reached.

### 4. Explicit test-only paths

Checkpoint alternation and reset-injection builds can explicitly route to the
checkpoint state under their existing mutually constrained compile-time gates.
These paths are required regression tests and must remain impossible in the
normal production configuration.

### 5. Compute-resume test

The compute-resume build uses the same eager per-chunk save, then parks after
one persisted chunk so manual reset can verify exact restore behavior.

## No separate persistent event logger

The source search found no Apollo MRAM event-log backend. `BISEN_LOG(...)` is
SWO/ITM diagnostic output, and the three-bit GPIO instrumentation is volatile
observation. Neither programs MRAM.

The frequent writes therefore come from recovery checkpoint scheduling, mainly
the temperature-complete record and every compute boundary, not from an event
history log.

## Programming activity inside one logical checkpoint

`CheckpointRecord` is 80 bytes and occupies five aligned 16-byte tuples.
`save_checkpoint()` performs:

1. One HAL call for the complete 80-byte inactive-slot image with an
   uncommitted marker: 20 words = five 16-byte tuples.
2. Readback and payload/CRC validation.
3. One HAL call that rewrites the final 16-byte integrity block with the commit
   marker: 4 words = one 16-byte tuple.
4. Final cache maintenance, readback, commit/CRC validation, and success return.

Actual baseline cost per successful logical checkpoint is therefore:

```text
2 calls to am_hal_mram_main_program()
6 programmed 16-byte tuples
96 total programmed bytes, including the final-block rewrite
```

Do not convert that count into a lifetime estimate. The public `PCYC=100,000`
specification does not identify the independently worn physical unit.

Reset-injection consequences:

- Phase 1 resets before the first HAL call: zero HAL calls in that attempt.
- Phase 2 resets after the 80-byte call: one HAL call / five tuples may have
  consumed wear even though the record remains uncommitted.
- Phase 3 resets after the final-block call: two HAL calls / six tuples may
  have consumed wear even if final software readback did not run.

## Post-refactor production scheduling update

The direct writer topology is unchanged: `bisen_checkpoint.cc` still contains
the only `am_hal_mram_main_program()` call, and `bisen_port.cc` contains the
only high-level `save_checkpoint()` call.

The normal integrated scheduler no longer reaches that save after temperature,
after an ordinary adequate-energy compute chunk, or at Sobel completion. It
remeasures VCAP after each useful state and reaches the checkpoint state only
for incomplete dirty progress when the next useful state cannot run and the
scheduler is about to enter its energy-retry sleep. A successful save marks
that progress clean; without new compute, sustained low energy cannot request
another write. The alternation, reset-injection, and
compute-resume paths remain
compile-gated destructive/regression tests and are not production paths.

## Baseline logical checkpoint counts per fresh job

A fresh job performs one temperature checkpoint plus one checkpoint per Sobel
chunk:

| Chunk plan | Compute chunks | Temperature checkpoint | Total logical checkpoints |
|---:|---:|---:|---:|
| 1000 pixels | 4 | 1 | 5 |
| 500 pixels | 8 | 1 | 9 |
| 100 pixels | 39 | 1 | 40 |

If a job is restored after its temperature-complete checkpoint, only the
remaining compute-chunk checkpoints occur. This is why some earlier validation
counts appear as 4, 8, or 39 rather than 5, 9, or 40.

For a new 500-pixel job completed without interruption, the baseline can issue
18 HAL calls and program 54 aligned tuples. The earlier draft target of one
final stable-high checkpoint was superseded by the MSP430/BISen scheduling
requirement: an uninterrupted stable-high job now performs zero production
MRAM writes. Only dirty, incomplete progress at the low-energy transition is
a production checkpoint candidate.

## Instrumentation limitation to address

Baseline state code 4 identifies one logical checkpoint-state visit, not the
two individual HAL calls inside it. Code 5 identifies only a final validated
commit. The refactor must retain a countable code-4 indication for every
logical checkpoint attempt and must document whether individual HAL-call
pulses are needed for the final measurement protocol.

## Post-refactor proof required

Repeat these searches after the refactor:

```sh
rg -n "am_hal_mram_main_program|am_hal_mram_main_words_program" \
  apps/bisen_port --glob '!README.md' --glob '!HANDOFF.md' \
  --glob '!MRAM_WRITER_AUDIT.md'

rg -n "save_checkpoint\\s*\\(" \
  apps/bisen_port --glob '!README.md' --glob '!HANDOFF.md' \
  --glob '!MRAM_WRITER_AUDIT.md'
```

The acceptable production result is:

- direct MRAM HAL calls remain confined to `bisen_checkpoint.cc`;
- normal ADC, temperature, policy, chunk, log, RTC, and sleep paths cannot
  program MRAM;
- scheduler saves arise only from dirty incomplete recovery state immediately
  before energy-driven sleep, or from explicit test-only gates; normal
  completion remains retained-RAM state and does not program MRAM;
- duplicate triggers for one runtime generation collapse into one commit;
- failed/torn attempts are included in RAM attempt counts.
