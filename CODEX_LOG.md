# Codex Work Log

Append new entries at the top. Do not rewrite old entries except to correct a factual error explicitly.

---

## 2026-09-28 — Make inherited release automation manual-only

**Branch / starting HEAD:** `main` / `a8f8fec65de056dbbfd4ada75e41bbfdc22213da`; working tree/index clean before this change.

**Task and authorization:** user approved the proposed manual-only release workflow for the Port_CNN_Intermittent research firmware mirror and accompanying coordination update.

**Files changed:** `.github/workflows/release.yaml`, `PROJECT_STATE.md`, `TASKS.md`, `CODEX_LOG.md`.

- Removed only the `push` (main) and `pull_request` event triggers. Kept `workflow_dispatch`, all job definitions/permissions, release-please configuration/manifest, and the reusable documentation workflow unchanged.
- Automatic pushes/PR events will no longer start this release workflow after the change reaches GitHub. Explicit manual dispatch can still create releases and conditionally publish Pages. The retained PR-only lockfile job cannot run with the manual-only trigger.
- Recorded the baseline commit `dc4a95f7ca888b747fe6d2304ac938ee8f157023` and completed its task; earlier pending-review log entries are historical. Current HEAD has advanced beyond that baseline. No firmware reconciliation or experimental-status promotion was performed.

**Validation:** compare the workflow with starting HEAD to verify the exact trigger-only edit and identical jobs; check JSON configuration/manifest parsing and `git diff --check`. No firmware build, host firmware test, physical validation or hosted Actions run is needed or claimed for this configuration change. No commit or push performed in this task; remote automation is unchanged until deployment of the edit.

---

## 2026-09-28 — User-confirmed hardware and V1–V13 synchronization (documentation only; approval pending)

**Scope:** apply remaining project-state updates, preserving the completed repository reconciliation and canonical `/Users/ghart/Documents/Ambiq/neuralSPOT` configuration. No repository migration, firmware edits, build, flash or new hardware test was performed.

**Preserved baseline:** branch `main`, inspected HEAD `1298bc669bb9bbce9368fc059f5d3b38d6a17b7f`; repository remotes, app paths, build configuration, ADC conversion anchors, scheduler thresholds/budgets, checkpoint layout, session behavior and state-DAC findings remain as documented in the previous entry. This synchronization does not repeat that inspection or assert a newly flashed binary.

### Changes and evidence

- `PROJECT_STATE.md`: the user's explicit current confirmation supersedes the previous UNCONFIRMED board/power inventory. Current board is AMAP4PEVB Apollo4 Plus BGA Evaluation Board Rev. 1.0, consistent with `apollo4p_evb`. AMAP4BPXEVB Blue Plus KXR Rev. 2.0/Sobel stays historical; its wiring/ADC/power assumptions do not transfer without independent AMAP4PEVB verification.
- Current installed power components are 10 mF / 10,000 uF reservoir, MP1584EN, approximately 1.9 V regulated rail and 390 kΩ / 10 kΩ fixed divider. These are user-confirmed physical facts, not inferred from source or newly measured by Codex.
- TPS7A0220PDBVR fixed 2.0 V / 200 mA LDO is available and planned soon, but not installed. TS5A3167DBVR is available, not installed, and only an optional experiment, not an adopted design.
- Replacing the regulator requires new threshold/reserve/discharge/dropout/energy validation. Existing results retain their original power-path identity.
- Remaining filter/camera/isolation/jumper details stay UNCONFIRMED. Hardware confirmation does not itself validate safe thresholds or energy results.
- `PROJECT_STATE.md` and `TASKS.md`: reconciled the shared V1–V13 program, retained implemented functionality as implementation evidence, and left physical/end-to-end validation open. Added evidence expectations and controlled variables. Hardware-inventory confirmation is complete only for the explicitly confirmed items.
- `AGENTS.md`: added only the missing explicit power-path revalidation, V1–V13 controlled-variable and build/host-versus-hardware rules; preserved existing instructions.
- `CODEX_LOG.md`: added this entry without rewriting the prior reconciliation history. Earlier UNCONFIRMED statements remain historical records of what was known then.

### Review and validation

Documentation-only checks: verify all V1–V13 identifiers in both state and task files, preserve the repository-derived implementation sections, check patch whitespace, and review Git status/index for unintended changes. No new firmware build, host test or physical validation is claimed. The earlier host-test results remain scoped as recorded in the reconciliation entry.

The four coordination files are the only intended baseline documentation changes. Existing untracked tools/traces/build/flash helpers remain excluded. Updated incremental and full baseline diffs are provided for review. **No staging or baseline commit is authorized until the user approves the diff; no commit has been created.**

---

## 2026-09-28 — Live repository reconciliation (documentation only; review pending)

**Repository:** `/Users/ghart/Documents/Ambiq/neuralSPOT`.
**Branch / inspected HEAD:** `main` / `1298bc669bb9bbce9368fc059f5d3b38d6a17b7f` (`update`, 2026-09-24).

### Initial Git state and remotes

Tracked working tree and index were clean. Initial `git status --short`:

```text
?? apps/bisen_camera_harvest/tools/
?? apps/bisen_camera_harvest/traces/rf_replay/
?? build_harvest_rf_replay.sh
?? flash_harvest_rf_replay.sh
```

Fetch/push remotes: origin `git@github.com:Gonzahart/Port_CNN_Intermittent.git`; fork `https://github.com/Gonzahart/BISen_CNN_Ambiq.git`; upstream `https://github.com/AmbiqAI/neuralSPOT.git`. Remotes were inspected locally, not fetched. Those untracked assets predate this task and are excluded from the baseline.

### Evidence inspected and findings

- Root `AGENTS.md`, general README, `apps/bisen_port/HANDOFF.md`, and READMEs for `bisen_camera`, `bisen_camera_vdd`, `bisen_camera_trace`, and `bisen_camera_harvest`; relevant Git history.
- Active source: `apps/bisen_camera_harvest/src/bisen_camera_harvest.cc`; module enforces `apollo4p_evb`, unlike historical `bisen_port`/Blue Plus KXR. App documentation identifies an AMAP4PEVB Rev. 1 intended target; physical board is unconfirmed.
- Camera/CNN: `scan.c`, `sensor.c/.h`, `workload.c`, `infer.c`, `preprocess.c`, `nn_engine.c/.h`, `nn_kernels.c`, generated `lenet_weights.h`. Camera cursor is 0..1024; default CNN work unit is an output element, with an optional input-stationary mode. Seven-layer integer LeNet (6/16 conv channels; FC120/84/10), blur/Otsu/vertical flip; preprocessing and input initialization are indivisible. Approximate checkpointing defaults off.
- ADC: `adc_shared.c/.h`, `energy_source.cc`, `trace_input.h`, `bisen/bisen_config.h`. ADC0 shares pixel GPIO15/SE4 and VCAP GPIO16/SE3; AVG16, 12-bit, tracking 63, median of three after one discarded result. Active conversion anchors in local replay helper: 461@5.5 V and 634@7.5 V; configured fixed divider 390k/10k, park ADC on, switched divider off. Old 1M/55.8k calibration in the copied config header is not the harvest source path. Values above reconstructed 8 V become invalid, not electrically clamped.
- Policy: `bisen/bisen_policy.cc`, `power_policy.cc`, application `run_bisen_job`/`wait_for_energy`. 100/500/1000 CNN units at 6.2/6.8/7.3 V; wait below 6.2; execution after an actual wait or storage restore requires 6.8. Classifier has no prior-band retention but caller has resume hysteresis. 5.8 V is critical classification, not an independent write-permission floor. Poll 250 ms, unlimited waits/checkpoints in replay flags; one complete camera pixel per quantum; policy sampled before each quantum.
- Persistence: `ckpt.h`, `ckpt.c`, `ckpt_mram.c`, app linker. Two 8,080-byte slots (16-byte header plus 8,064-byte maximum payload), 16-byte alignment. Header magic/sequence/layer/unit/CRC, live payload CRC, payload first/header last. Scan layer sentinel FFFF with two-byte acquired pixels; inference saves live buffers (no accumulator payload in default output-stationary mode). Completion retires a live checkpoint via FFFE tombstone. No new atomicity claim was inferred from source comments.
- Separate session journal: two 16-byte magic/sequence/armed/CRC records, CRC on first 12 bytes. Replay HVR3 workload identity, HVC3 session identity. BTN0 persists arm; BTN1 checkpoints dirty work then disarms; armed reset/power recovery continues. Boot reads/restores storage before the run-time energy gate. A fresh disarmed boot parks.
- Linker: `apps/bisen_camera_harvest/bisen_camera_harvest_checkpoint.ld` includes Apollo4P base script with origin 0x18000. Workload NOLOAD reservation 16,160 bytes plus 32 session bytes. Existing local map reports workload `[0x32250,0x36170)` and session `[0x36170,0x36190)`; these are an observed unrebuilt artifact's addresses, not permanent addresses or proof of the current flash. TCM calibration/validation storage is volatile despite NOLOAD.
- State DAC: GPIO62/63/61 bits 0/1/2, intended J12.7/.9/.11. 0 idle/wait, 1 ADC, 2 camera, 3 compute, 4 write, 5 commit, 6 restore, 7 boot/error. Codes 4/5 also wrap session changes and retirement. Code 3 includes preprocessing and a wait-exit mark. Inference's restore marker follows the storage read, so it is not a complete restore-time interval. Marker count is not automatically a job/checkpoint count.

### Build configuration (inspected, not executed)

Root `build_harvest_rf_replay.sh` runs `make -B` with:

```text
EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
BISEN_HARVEST_CALIBRATION_MODE=0 BISEN_CAMERA_ENABLE_MRAM=1
BISEN_HARVEST_CHECKPOINT_MAGIC=0x48565233 BISEN_CAMERA_AUTORUN=0
BISEN_HARVEST_AUTOCONTINUOUS=1 BISEN_HARVEST_OFFLINE_VALIDATE=0
BISEN_ENABLE_SWO_LOGGING=0 BISEN_ENABLE_STATE_DAC=1
BISEN_CAMERA_MAX_CHECKPOINTS=0 BISEN_CAMERA_MAX_WAIT_CYCLES=0
BISEN_HARVEST_CAL_LOW_CODE=461 BISEN_HARVEST_CAL_LOW_UV=5500000
BISEN_HARVEST_CAL_HIGH_CODE=634 BISEN_HARVEST_CAL_HIGH_UV=7500000
BISEN_HARVEST_CRITICAL_UV=5800000 BISEN_HARVEST_WORK100_UV=6200000
BISEN_HARVEST_WORK500_UV=6800000 BISEN_HARVEST_WORK1000_UV=7300000
```

`flash_harvest_rf_replay.sh` uses the same configuration with `make -B deploy`. Module defaults include scan max 1, wait 250000 us, ADC parking 1, fixed divider, MCU_LOW_POWER=0 (high-performance mode selected in source; comment states 192 MHz). GNU flags include -O3/-ffast-math. Bare app build instead defaults to calibration mode, MRAM off and no physical calibration/threshold anchors. Helpers are pre-existing untracked inputs and require separate versioning. No build, deployment, or hardware interaction was performed.

### Reconciliation changes versus ChatGPT proposal

1. Replaced unresolved branch/remotes/app information with inspected values and explicit initial untracked status; local repository path is current, not merely historical.
2. Distinguished the current Apollo4 Plus camera target from historical Blue Plus KXR Sobel target; did not infer the installed board.
3. Promoted verified camera/CNN, scheduler, ADC, checkpoint and session mechanisms from partially implemented direction to CURRENT, with source paths.
4. Confirmed numerical work/resume thresholds as the local replay recipe, and documented critical/max limits, virtual-code mapping, defaults versus recipe, and classifier versus post-wait hysteresis.
5. Added exact checkpoint sizes/fields, CRC coverage, shared scan/inference slots, tombstones, separate journal, NOLOAD reservation and image-dependent addressing; distinguished implementation from on-target atomicity proof.
6. Documented output-stationary default and exact checkpoint mode; preprocessing is indivisible, not checked at every internal operation.
7. Added actual BTN0/BTN1/reset behavior and boot-time restore versus execution gating.
8. Added state-DAC pins, overloaded event meanings and timing limitations; prevented claiming every compute pulse is a completed frame or every commit is a workload save.
9. Kept installed power hardware UNCONFIRMED; described README wiring only as intended configuration. No paper plan was converted into a bench fact.
10. Changed app/layout reconciliation tasks to complete. Cold restore remains an evidence-packaging/final-hardware validation task because code exists and prior user reports exist; no new hardware result was invented.
11. Added missing model/export/accuracy provenance and unconfirmed PT1/PT2/PT3 mapping despite local traces; baseline comparison plans remain DIRECTION.
12. Recorded stale README old-workspace paths, conflicting scope mappings, inherited constants/comments, and evidence gaps. Existing READMEs/firmware were left unchanged.

### AGENTS merge decisions

Preserved existing sections 1–12 verbatim (AutoDeploy templates, PMU, toolchain, testing and commit rules). Appended RUIC bridge rules; explicitly added authorized `apps/bisen_*` scope. Clarified that project-evidence precedence cannot override system/developer instructions and that CURRENT code differs from VALIDATED bench results. Defined file-based handoff explicitly: installing files does not automatically synchronize the ChatGPT Project. No conflict required firmware edits or a build.

### Commands and validation

Read-only inspection used `git status --short`, `git branch --show-current`, `git rev-parse HEAD`, `git remote -v`, `git log`, `git ls-files`, `rg`, and source reads. ZIP contained only the four proposed coordination Markdown files.

Existing host tests were compiled/run into a temporary directory, from repo root:

```sh
tmpcheck=$(mktemp -d /tmp/ruic-reconcile.XXXXXX)
cc -std=c11 -Wall -Wextra apps/bisen_camera_harvest/tests/harvest_policy_test.c -o "$tmpcheck/harvest_policy_test"
"$tmpcheck/harvest_policy_test"
cc -std=c11 -Wall -Wextra -Iapps/bisen_camera_harvest/src   apps/bisen_camera_harvest/tests/session_journal_test.c   apps/bisen_camera_harvest/src/ckpt.c   apps/bisen_camera_harvest/src/nn_engine.c   apps/bisen_camera_harvest/src/nn_kernels.c -o "$tmpcheck/session_journal_test"
"$tmpcheck/session_journal_test"
sh -n build_harvest_rf_replay.sh flash_harvest_rf_replay.sh
```

Both tests and helper syntax checks PASS (exit 0). These tests do not establish energy safety, complete CNN correctness, or on-target brownout behavior. Firmware build: NOT RUN. On-target/physical validation: NOT RUN. Documentation checks: verify only the four coordination files change, preserve all initial untracked files, and check whitespace/links before presentation.

### Commit review gate

Files changed: only `AGENTS.md`, `PROJECT_STATE.md`, `TASKS.md`, `CODEX_LOG.md`. Proposed commit: `docs: add ChatGPT-Codex RUIC coordination state`. Commit is PENDING USER REVIEW under the explicit task instruction to show the diff first. No files staged. A commit hash cannot be embedded in the same commit without changing it; report the baseline hash after approval and record it in a later log entry rather than a self-referential amend.

---

## 2026-09-28 — ChatGPT bridge initialization

### Scope

ChatGPT created the initial shared coordination layer from the RUIC project history available in ChatGPT. No live repository inspection was performed in this chat.

### Files prepared

- `AGENTS.md`
- `PROJECT_STATE.md`
- `TASKS.md`
- `CODEX_LOG.md`

### State reconciled so far

The initial project state deliberately separates:

- the validated August Apollo4 stable-bench/Sobel bring-up;
- the September camera/CNN framework and paper direction;
- unresolved physical-hardware and live-repository details.

### Known conflicts that Codex must not guess through

1. Older functional bench: 1000 uF + LM2596 + Sobel-era policy.
2. September paper description: nominal 10 mF + MP1584EN + camera/incremental CNN + newer voltage policy.
3. Later discussion: lower-IQ ~1.9 V regulator/LDO, 10,000 uF reservoir, switchable VCAP divider, and EVB/J-Link isolation for final energy work.
4. Older repository handoff centered on `apps/bisen_port`; later camera/CNN work may have moved or expanded beyond it.

### Required next Codex action

Perform the read-only repository reconciliation task in `TASKS.md` before implementing new research features. Record:

- branch / HEAD / remotes / git status;
- relevant app paths;
- actual build configuration;
- live scheduler thresholds and chunk policy;
- live checkpoint layout;
- current ADC/state-DAC configuration;
- newer repository documentation.

Then update `PROJECT_STATE.md` only where the repository provides direct evidence.

### Validation

- Repository build: NOT RUN by ChatGPT.
- On-target validation: NOT RUN by ChatGPT.
- Bench hardware confirmation: PENDING USER CONFIRMATION.

---

## Template for future Codex entries

### YYYY-MM-DD — Short task name

**Branch / HEAD:**

**Task:**

**Files changed:**

- `path/to/file`

**What changed:**

- ...

**Commands / validation:**

- Build command: ...
- Host tests: PASS / FAIL / NOT RUN
- On-target test: PASS / FAIL / NOT RUN
- Physical energy validation: PASS / FAIL / NOT RUN

**Observed result:**

- ...

**Project-state updates:**

- ...

**Unresolved / follow-up:**

- ...
