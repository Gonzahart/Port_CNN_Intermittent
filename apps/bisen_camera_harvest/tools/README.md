# BISen harvest bench tools

This directory contains copies of the host-side instruments used with
`apps/bisen_camera_harvest`. The original working copies remain in
`/Users/ghart/VSC/Image_filter`.

## Tools

- `sdg1032x_charge_loop_modified.py` replays charging/RF-envelope traces on the
  Siglent SDG1032X. This is the current reservoir-replay tool.
- `sdg1032x_ambiq_vdd_trace.py` contains the earlier direct Ambiq VDD replay
  workflow and is retained for comparison.
- `scope_capture_plot.py` captures Siglent scope memory, exports CSV data, and
  decodes the BISen state DAC.
- `analyze_checkpoint_shunt.py` integrates board-input energy across decoded
  state-4 to state-5 episodes after a same-node probe-zero measurement.

## Apollo camera state ladder and V14 probe convention

The installed state-DAC wiring is GPIO62 (firmware bit 0) through 99.3 kΩ,
GPIO63 (bit 1) through 201 kΩ, and GPIO61 (bit 2) through 398 kΩ to the
CH2 DAC junction. Thus the analog ladder weights are 4/2/1 while firmware
bits are 1/2/4. The `apollo-camera` profile reverses bits 0 and 2 before
labelling or exporting `state_code`; `state_code_ladder` retains the measured
voltage rank. A GPIO62-only high at about 1.07 V with a 1.88 V rail is
firmware state 1 (VCAP ADC), not state 4. Captures whose CH2 tip is directly
on a GPIO pad cannot be decoded as junction voltage.
For `apollo-camera`, transitions shorter than 100 µs are suppressed by
default because the observed junction edge overshoots into adjacent bands;
`state_code_raw` remains available. Override with `--state-min-run-us` when
valid, shorter firmware markers are established on the bench.

For the current high-side shunt, CH4 is at MP1584EN OUT+ (upstream) and CH1 is
at board J7.3 (downstream). Both probe tips must be on the same node for the
zero capture, then returned to their respective shunt ends without changing
scope settings. CH3 monitors live VCAP at MP1584EN IN+; CH2 monitors the DAC
junction. `analyze_checkpoint_shunt.py` requires a corrected capture with a
`state_code_ladder` column and computes current from CH4−CH1 after zero
subtraction. Its reported energy is board-input interval energy, not isolated
MRAM-array energy.

To re-decode an older raw scope capture without overwriting it, run:

```sh
python3 apps/bisen_camera_harvest/tools/scope_capture_plot.py \
  --input-csv /absolute/path/old_capture.csv \
  --channels 3 1 2 4 --state-dac-ch 2 --state-profile apollo-camera \
  --state-vcc-ch 1 --no-energy --out /absolute/path/old_capture_redecoded
```

The corresponding replay inputs are stored in `../traces/rf_replay/`:

- `charge_trace.txt`
- `charge_trace2.txt`
- `charge_trace3.txt`

These files were copied on 2026-09-26 without modifying their contents.

## Example paths

Run the reservoir replay tool from the Ambiq project root with an input such as:

```sh
python3 apps/bisen_camera_harvest/tools/sdg1032x_charge_loop_modified.py \
  trace-loop \
  --trace-file apps/bisen_camera_harvest/traces/rf_replay/charge_trace.txt \
  --help
```

Run the capture tool with:

```sh
python3 apps/bisen_camera_harvest/tools/scope_capture_plot.py --help
```

Use an absolute output path outside the source tree for experimental CSV and
PNG results.
