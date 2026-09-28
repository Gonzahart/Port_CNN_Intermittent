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
