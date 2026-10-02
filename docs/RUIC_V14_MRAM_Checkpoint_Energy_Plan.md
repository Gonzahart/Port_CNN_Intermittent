# V14 — MRAM checkpoint energy: measurement plan

**Status:** DIRECTION (plan only, 2026-09-29, prepared by Claude). Nothing here is implemented or measured. Firmware changes listed in §8 are proposals for an authorized task; see `TASKS.md` V14.

**Chosen setup (user, 2026-09-29):** Siglent SDS scope + 1 Ω high-side shunt; replay power path (10 mF reservoir → MP1584EN → ~1.9 V); measure both dataflows of the r2a engine: IS in `apps/bisen_camera_harvest_IS` (commit `d96a1d14d`, `NN_DATAFLOW=1`) and OS in `apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS` (commit `0ad5ccf18`, `NN_DATAFLOW=0`); both `NN_SIMD=1 NN_MAX_CONV_IN_C=6`, identical engine sources. Use the package's checkpoint identities: OS01 `0x4F533031` (OS module default) and IS01 `0x49533031` (pass on the IS command line; its module default is still HVR1).

## 1. What we want to know

| Quantity | Definition | Why it matters |
|---|---|---|
| E_save | ∫ V_board·I_board dt over the whole `ckpt_save()` / `ckpt_save_scan()` call (state DAC 4→5) | What the reservoir reserve must cover when a checkpoint fires |
| E_prog | E_save minus the same save with the MRAM program call skipped (control build) | Cost of MRAM programming itself, vs. CRC/packing/copy |
| E_save,VCAP | Energy drawn from the reservoir for one save (includes MP1584EN loss) | The number the threshold/reserve calculation actually needs |
| t_save, t_prog | Durations (STIMER + scope) | Cross-check, and time-in-state for V10 |
| Cost vs size | Fit E = a + b·bytes per dataflow | Fixed overhead vs. per-byte cost; predicts any record size |

Report separately: CNN recovery saves (OS and IS), scan saves, completion tombstones (`ckpt_retire`), session records (arm/disarm). Rail-level measurements include the whole MCU; they do not isolate the MRAM array.

**Scale (estimate, not a measurement):** a few ms at roughly 10 mW is on the order of 30 µJ per save. On 10 mF at 6.5 V that moves VCAP by ≈ E/(C·V) ≈ 0.5 mV — below ripple. So capacitor ΔE (`scope_capture_plot.py`'s energy mode) cannot measure this; it must be current-based. For comparison, 0.5·C·(6.2² − 5.8²) = 24 mJ sits between the 6.2 V save band and the 5.8 V critical classification.

## 2. Freeze before measuring

Board AMAP4PEVB Rev 1.0; MP1584EN, ~1.9 V (measure actual); 10 mF; divider 390k/10k; engine commit and build flags; `lenet_weights.h` SHA-256 `f2d631c3…4a138f`; MCU mode (`BISEN_HARVEST_MCU_LOW_POWER=0`, source says 192 MHz — confirm); compiler flags; binary SHA-256 of each build flashed.

Must be off in all timed builds: `CKPT_MRAM_TRACE=0` (the `_IS` module currently sets 1 — it prints inside `ckpt_dev_write`), `BISEN_ENABLE_SWO_LOGGING=0`, no `printf` inside the timed region. Note `PP_BUS_SELFCHECK` adds a read-back to every state-DAC write; keep it identical across all builds or disable it.

## 3. Power path and measurement boundary

- Shunt: 1 Ω between MP1584EN output and the EVB rail input. Measured energy = board-input energy at ~1.9 V.
- **Nothing else may feed the MCU rail.** USB unplugged, or onboard LDO disconnected (SB3 open), and J-Link level-shifter reference handled (SB84/SB85). See the Claude project doc `hardware/apollo4-plus-evb-external-power.md`. PROJECT_STATE lists this wiring as UNCONFIRMED — record it (photo + continuity checks) before the first run.
- For characterization, replace the reservoir with a **bench supply at fixed VCAP** (7.3, 6.8, 6.2 V) into the same MP1584EN. Same power path, but steady input, and the input current can be measured for E_save,VCAP. Then spot-check a few saves in situ on the real reservoir during a replay (§6).
- Per-save rail drop across 1 Ω (a few mV at mA currents) is acceptable at 1.9 V, but record it.

## 4. Method A (primary): steady-state averaging with a DMM

Works without the scope's resolution limits or the CH3 fault.

1. Bench profile loops `ckpt_save()` back-to-back on a context frozen at a chosen (layer, unit), for a fixed T (e.g. 10 s), counting N saves. Same two-slot alternation, payload-first/header-last sequence and 64-byte bounce buffer as production.
2. Control profile: identical loop, identical packing/CRC/copy, with `am_hal_mram_main_program` skipped. Compile-gated into the bench profile only, never into production or recovery experiments.
3. Baseline: same loop body minus the save (CPU spinning in the same power mode).
4. Measure mean current with a DMM (average/integration mode, window ≫ one save): across the 1 Ω shunt (mV = mA) on the 1.9 V side, and on the bench-supply input for the VCAP side. Record V at both points.
5. Prefer time-resolved integration over each marked save. If averaging loop
   power instead, match the number of events, initial/final charge state, and
   observation duration across real and control runs; report the event time
   distribution. A skipped program call changes loop throughput, so
   `(P_loop − P_control)·T/N` is **not valid** when the two runs have different
   `N` or schedules. Pad the control to matched timing or subtract power in
   equal per-event windows. Report whole-save board energy separately from
   baseline-subtracted or control-subtracted incremental energy.

Check MRAM endurance in the Apollo4 Plus datasheet and budget N accordingly; the loop rewrites the same two slots.

## 5. Method B (waveform, timing split): scope

Markers:
- Full save: state DAC 4→5 is already emitted around every production save (`infer.c`, `scan.c`, `bisen_camera_harvest.cc`).
- Program call: add one **dedicated GPIO** high during each `am_hal_mram_main_program` call in `program_words()`. Choose a free pin from the AMAP4PEVB schematic/BSP; do not reuse GPIO61–63 and do not guess.

Resolution problem to solve first: two single-ended probes at 1.9 V subtracted to get a ~5–15 mV shunt drop is not resolvable on an 8-bit scope. In order of preference:
1. A high-side current-sense amplifier across the shunt (gain 50–100) → one ground-referenced channel.
2. A differential probe across the shunt.
3. Both probes at the finest V/div with DC offset ≈ −1.9 V (check the Siglent offset range at that V/div), plus **average acquisition triggered on the marker** over ≥64 identical saves from the Method A loop.

Suggested channels (with option 1): CH1 current-amp output, CH2 state DAC, CH4 program-call marker, CH3 VDD if working (else take VDD from a DMM, it is quasi-static). Use the capture to split t_save into programming vs CPU work, find peak current, and cross-check Method A within stated uncertainty. Cross-check durations against firmware STIMER (6 MHz; `harvest_stats` already accumulates per-state ticks).

## 6. Test matrix

| Axis | Levels |
|---|---|
| Dataflow | OS, IS (same r2a source, separate builds, separate checkpoint magic) |
| Record size | small / median / max of real live payloads, chosen with `ckpt_bytes()` at real (layer, unit) positions (IS max ≈ 7.9 KB, OS max ≈ 4.9 KB per `CHANGES-r2a.md`; last dense layer ≈ 128 B + header) |
| Other writes | scan save at next_pixel ≈ 64 / 512 / 1024; tombstone; session record |
| Slot | both (alternation happens naturally; log slot) |
| VCAP (bench supply) | 7.3, 6.8, 6.2 V |
| Repeats | ≥5 independent runs per cell (reflash/power-cycle between runs), N saves each |
| In-situ check | a few production saves captured during a PT replay on the real reservoir, compared to the characterized value at the same VCAP |

Log per cell: bytes, `g_ckpt_dev_hal_program_calls`, `g_ckpt_dev_program_units`, STIMER t_save, N, T, V and I at both sides, instrument ranges, binary hash.

## 7. Analysis and reporting

Per dataflow: E_save, E_prog, E_save,VCAP (median, spread, uncertainty from DMM accuracy, shunt tolerance and N); linear fit vs bytes with residuals; peak current; t_prog/t_save. Compare timing with the source's 1.5 bytes/µs assumption (`ckpt_write_us`) and with Data_tables Table 16 (old engine IS: 3.16 ms save, 2.10 ms programming — timing only, different engine/CRC). Then express worst-case save energy against the reservoir window between the save band and brownout, including MP1584EN loss and dropout margin. Label everything MP1584EN; invalid after a regulator change (V12).

## 8. Firmware/tooling changes this plan needs (not implemented)

- Compile-gated bench profile in `_IS` (e.g. `BISEN_V14_BENCH=1`): deterministic input, freeze context at a requested (layer, unit), loop saves / control / baseline for T, print N and counters after the loop only.
- `V14_SKIP_PROGRAM` control switch, bench profile only.
- Dedicated program-call GPIO marker, compile-gated.
- `CKPT_MRAM_TRACE=0` for timed builds.
- Analysis script: DMM readings + STIMER → per-save energies; optional scope-average import. Do not use `scope_capture_plot.py` energy mode for this (net ΔE, 0.1 F default).
- Prerequisites: the `_OS` package is self-contained but nested one level deep and, like `_IS`, still names its binary `bisen_camera_harvest` and uses the original app's linker path (its README says to install it by renaming it over `apps/bisen_camera_harvest`). `_IS` still lacks `nn_build.h`, `nn_simd.h`, `nn_wo.h` in its `src/`. Both modules set `CKPT_MRAM_TRACE=1`. The OS README records that its packaged native/ARM acceptance (`accept_os.sh`) has not yet run and no board run was done.

## 9. First controlled shunt run (2026-09-29; tools present, bench result pending)

The user reports that the 1 Ω shunt and scope probes have been installed. The
actual resistance, CH1/CH4 polarity, bypass-power isolation, and VDD under
peak load remain to be measured. The new
`apps/bisen_camera_harvest/traces/rf_replay/v14_checkpoint_step_dc.txt` contains
12 ten-second **FG command** levels: 7.8 V for 30 s, 6.6 V for 40 s, and 7.5 V
for 50 s. Replay with the existing reservoir-mode FG tool in DC/raw mode,
`--steps 12 --charge-s 120 --cycles 1 --rest-s 0 --trace-max-vpp 8.0
--max-command-v 8.0 --arm-output`, and save its command log. Output turns off
after 120 s. These are not claims about loaded VCAP: the diode prevents the
FG from actively discharging the bank. Confirm a measured VCAP crossing below
6.20 V and later above 6.80 V while the board-side rail remains valid.

For the first run, use the current original harvest build as a checkpoint
*smoke test*, then separately qualify the OS and IS r2a builds. CH1 measures
the upstream shunt side, CH4 board VDD downstream, CH2 state DAC, and optional
CH3 VCAP. At identical scope settings, first capture CH1 and CH4 with both tips
on the same node to measure their differential zero. For short triggered
captures, `tools/analyze_checkpoint_shunt.py` uses that zero and the measured
shunt resistance to integrate `V_CH4·(V_CH1−V_CH4−V_zero)/R` over each decoded
4→5 write interval. It reports **unclassified write candidates**: a session
record or retirement tombstone can also show 4→5. Identify workload saves
from the synchronized VCAP transition and firmware/session context. The
scope script's capacitor-energy mode is not a substitute for this analysis.
This first pass does not isolate HAL programming energy or complete cold
restore energy; those still require the V14 instrumentation and control path.
