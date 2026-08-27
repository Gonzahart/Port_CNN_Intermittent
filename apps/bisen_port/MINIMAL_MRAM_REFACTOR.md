# Minimal-MRAM-write refactor status

Date: 2026-08-19  
Target: `apollo4p_blue_kxr_evb`  
AmbiqSuite: `R4.5.0`

This file records the implementation state reached while following
`guide.md`. It is intentionally a gate report, not a claim that the entire
refactor is complete.

## Completed locally

### Milestone 1: writer audit

See `MRAM_WRITER_AUDIT.md`. There is one direct MRAM programming call site,
the private `program_mram_blocks()` helper in `bisen_checkpoint.cc`, and one
high-level scheduler call to `save_checkpoint()`. There is no persistent event
log. One logical 80-byte checkpoint currently makes two HAL calls: five
16-byte tuples for the uncommitted record and one 16-byte tuple for the final
commit rewrite. Failed or torn attempts must still be treated as potential
wear.

### Milestones 2 and 3: retained runtime context and RAM log

- `BisenRuntimeContext` is 56 bytes after removing the separate checkpoint
  request latch.
- It uses `runtime_generation != committed_generation` for dirty tracking.
- Recovery-relevant compute/job changes increment the runtime generation.
- ADC, temperature, RTC, policy, and diagnostic-log events do not increment
  it.
- Checkpoint attempts and successes are counted in RAM.
- A failed checkpoint latches a fail-closed condition for the remainder of
  the boot.
- A powered-session limit of 64 checkpoint attempts is enforced.
- The RAM event log is a fixed 128-entry ring: 4,096 bytes of entries plus
  16 bytes of metadata, or 4,112 bytes total.
- `BISEN_LOG_BACKEND_MRAM` is explicitly unsupported.
- Both objects are placed in app-local `.bisen_retained (NOLOAD)` TCM.

The known integrated configuration builds successfully. Its ELF evidence is:

```text
.bisen_retained: [0x10006078, 0x100070c0), 4,168 bytes
  RAM log:        4,112 bytes
  runtime:           56 bytes
.bisen_checkpoint: [0x00021460, 0x00021500), 160 usable bytes
text=37,904 data=80 bss=28,940
binary SHA-256: 0045d769da90d751a87d23d9cf4f919baecda5a50eb48af9065c91c05a60f4a9
```

Host tests passing:

- runtime generation, checkpoint counters, and 64-attempt limit
- RAM-log insertion, field integrity, wraparound, and oldest-entry ordering
- all Sobel chunk sizes, resume, malformed-context rejection, and golden
  digest `8bdd7454`
- direct-threshold voltage policy with compute enabled

## Current gate: Milestone 4 retention proof

The special retention image builds successfully with:

- RTC deep sleep enabled
- deterministic compute enabled
- GPIO state instrumentation and SWO enabled
- physical VCAP ADC disabled (this test does not need it)
- MRAM checkpoints compiled out
- `BISEN_FORCE_DISABLE_MRAM_PROGRAMMING=1`
- RAM logging enabled

The linker places the objects in DTCM, and the image queries the installed
R4.5.0 HAL's actual MCU memory configuration at runtime. The test refuses to
run if the HAL reports no retained DTCM. Static symbol and disassembly checks
show no linked `am_hal_mram_main_program`, `save_checkpoint`, or
`restore_checkpoint` symbol in this test image.

Retention-image evidence:

```text
.bisen_retained: [0x10005858, 0x100068a0), 4,168 bytes
.bisen_checkpoint: [0x0001ea60, 0x0001eb00), 160 usable bytes, NOLOAD
text=27,152 data=72 bss=26,876
binary SHA-256: 2bfd4958c24dc96ed65d05b74f6fafd95dd127cde579a43865d1081d2227e887
```

The retained-RAM proof passed on target. The source has now been changed to
lazy, low-energy-only checkpoint scheduling. The project owner's explicit
requirement supersedes the earlier guide's final-completion checkpoint:
Sobel completion never programs MRAM.

## Build and deploy the retention test

Close the SWO viewer first, then run:

```sh
cd /Users/ghart/Documents/Ambiq/neuralSPOT

make -B deploy \
  EXAMPLE=bisen_port \
  PLATFORM=apollo4p_blue_kxr_evb \
  AS_VERSION=R4.5.0 \
  BISEN_ENABLE_DEBUG_LOGGING=1 \
  BISEN_ENABLE_VCAP_ADC=0 \
  BISEN_VCAP_ADC_PIN_CONFIRMED=0 \
  BISEN_ENABLE_LOW_POWER=1 \
  BISEN_ENABLE_LOW_POWER_TEST=0 \
  BISEN_ENABLE_INTEGRATED_CYCLE_TEST=0 \
  BISEN_ENABLE_MRAM_CHECKPOINTS=0 \
  BISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  BISEN_ENABLE_COMPUTE_RESUME_TEST=0 \
  BISEN_ENABLE_RESET_INJECTION=0 \
  BISEN_ENABLE_CHECKPOINT_ALTERNATION_TEST=0 \
  BISEN_ENABLE_GPIO_INSTRUMENTATION=1 \
  BISEN_LOG_BACKEND=1 \
  BISEN_MRAM_SESSION_ATTEMPT_LIMIT=64 \
  BISEN_FORCE_DISABLE_MRAM_PROGRAMMING=1 \
  BISEN_ENABLE_RETENTION_TEST=1 \
  BISEN_RETENTION_TEST_WAKE_LIMIT=3
```

Require J-Link programming and verification messages ending in `O.K.`; do
not rely only on Make's final exit status.

Start SWO:

```sh
make view \
  EXAMPLE=bisen_port \
  PLATFORM=apollo4p_blue_kxr_evb \
  AS_VERSION=R4.5.0
```

Press RESET once. No BTN0 action is needed. The test performs three
100-pixel compute increments separated by one-second RTC deep sleeps and then
parks.

Required evidence:

1. The MCU memory-config line reports a successful HAL query and nonzero
   `retain_DTCM`.
2. Three lines report `retention wake #1/#2/#3 PASS`.
3. Progress is 100, 200, then 300; generation is 1, 2, then 3.
4. `committed=0`, proving no persistent generation was created.
5. The final line reports `MRAM_attempts=0 MRAM_successes=0`.
6. The GPIO state bus shows compute code 3 and sleep code 0, with zero code-4
   and zero code-5 events.
7. No line contains `retention test FAIL`.

Please preserve and return the complete SWO block, including the memory-config
line and final PASS line. If this test fails, do not enable per-sleep MRAM
writes as a workaround; the retained-memory configuration must be resolved
first.

## Preserved integrated artifact

Before building the retention image, the integrated binary was copied to:

```text
/Users/ghart/workspace_v12/Image_Sobel_5969/backups/
  bisen_port_integrated_pre_lazy_20260819.bin
```

SHA-256:
`771f60ff860270e64e05e0159e36c67e0e8befc5be2bf67b99d4db71b508db5c`

`apps/basic_tf_stub`, the Ambiq SBL, and the generic neuralSPOT linker/build
configuration were not modified.

## Milestone 5: implemented locally

- Temperature and ordinary compute chunks no longer route to MRAM.
- Every normal compute chunk routes through a fresh VCAP ADC measurement before
  RTC sleep.
- Only incomplete, dirty progress for which the next useful state cannot run
  may request a production save, immediately before energy-retry sleep.
- A successful save marks the recovery generation clean. Since low VCAP does
  not permit more compute, no repeated save is possible until useful work has
  produced new progress.
- Sobel completion is marked clean in retained-RAM accounting without an MRAM
  call. The older persistent checkpoint is intentionally retained, so a later
  outage may roll back and recompute the post-checkpoint tail.
- The integrated checkpoint epoch is `0x322`, rejecting records from the
  former delayed 5.7 V gate.
- The production decision matches the MSP430 `main.c`: there is no
  independent checkpoint voltage gate after useful work becomes ineligible.
- Existing alternation, reset-injection, and compute-resume writers remain
  explicit test-only compile-time paths.

Host policy/runtime/log/compute tests and R4.5.0 Apollo4P builds must be rerun
after this pre-sleep checkpoint change before deployment.

## Next step

Deploy the integrated build on stable bench power and perform a controlled
falling-VCAP capture. Verify that the first post-state ADC which disallows the
next useful state routes directly to code 4 without an intervening RTC-sleep
interval. Final capacitor-discharge characterization remains pending for the
replacement converter.
