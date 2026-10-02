#!/usr/bin/env python3
"""Measure board-input energy of state-DAC 4->5 write episodes.

Input CSVs come from scope_capture_plot.py. CH4 is upstream of the high-side
shunt at the MP1584EN output, CH1 is board VDD downstream, and CH2 carries
the decoded state DAC.
The report labels writes as candidates: state 4 also covers session and
retirement records, so experiment context must identify workload checkpoints.
"""

import argparse
import csv
from pathlib import Path

import numpy as np
import pandas as pd


def load_capture(path, required):
    frame = pd.read_csv(path, usecols=lambda name: name in required)
    missing = sorted(set(required) - set(frame.columns))
    if missing:
        raise ValueError(f"{path}: missing columns {', '.join(missing)}")
    if len(frame) < 3:
        raise ValueError(f"{path}: at least three samples are required")
    for name in required:
        frame[name] = pd.to_numeric(frame[name], errors="raise")
    if not np.isfinite(frame[list(required)].to_numpy(dtype=float)).all():
        raise ValueError(f"{path}: contains non-finite samples")
    return frame


def analyze(frame, zero_v, shunt_ohms, min_samples):
    time_s = frame.time_s.to_numpy(dtype=float)
    if not np.all(np.diff(time_s) > 0):
        raise ValueError("time_s must be strictly increasing")

    state = frame.state_code.to_numpy(dtype=int)
    upstream_v = frame.ch4_v.to_numpy(dtype=float)
    board_v = frame.ch1_v.to_numpy(dtype=float)
    current_a = (upstream_v - board_v - zero_v) / shunt_ohms

    starts = np.flatnonzero((state == 4) & np.r_[True, state[:-1] != 4])
    events = []
    for start in starts:
        end = start + 1
        while end < len(state) and state[end] == 4:
            end += 1
        if end >= len(state) or state[end] != 5 or end - start < min_samples:
            continue
        segment = slice(start, end + 1)
        event_time = time_s[segment]
        event_board_v = board_v[segment]
        event_current = current_a[segment]
        power_w = event_board_v * event_current
        energy_j = np.sum(0.5 * (power_w[:-1] + power_w[1:]) *
                          np.diff(event_time))
        events.append({
            "start_s": float(time_s[start]),
            "end_s": float(time_s[end]),
            "duration_ms": float((time_s[end] - time_s[start]) * 1e3),
            "samples": int(end - start + 1),
            "vcap_v_at_start": (float(frame.ch3_v.iloc[start])
                                if "ch3_v" in frame else ""),
            "vcap_v_at_end": (float(frame.ch3_v.iloc[end])
                              if "ch3_v" in frame else ""),
            "board_v_min": float(np.min(event_board_v)),
            "board_v_mean": float(np.mean(event_board_v)),
            "current_mean_mA": float(np.mean(event_current) * 1e3),
            "current_peak_mA": float(np.max(event_current) * 1e3),
            "board_input_energy_uJ": float(energy_j * 1e6),
        })
    return events, float(np.median(np.diff(time_s)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_csv", type=Path)
    parser.add_argument("--zero-csv", type=Path, required=True,
                        help="Same-node CH1/CH4 capture at identical scope settings")
    parser.add_argument("--shunt-ohms", type=float, required=True,
                        help="Measured resistance, including relevant lead resistance")
    parser.add_argument("--min-samples", type=int, default=20)
    parser.add_argument("--out-csv", type=Path)
    args = parser.parse_args()

    if args.shunt_ohms <= 0 or args.min_samples < 2:
        parser.error("shunt resistance must be positive and min-samples >= 2")

    zero = load_capture(args.zero_csv, ("ch1_v", "ch4_v"))
    zero_v = float(np.median(zero.ch4_v - zero.ch1_v))
    capture_columns = pd.read_csv(args.capture_csv, nrows=0).columns
    if "state_code_ladder" not in capture_columns:
        parser.error(
            "capture has the old state-DAC decode; re-decode its raw CSV with "
            "the corrected apollo-camera profile before shunt analysis"
        )
    columns = ("time_s", "ch1_v", "ch4_v", "state_code", "state_code_ladder")
    if "ch3_v" in capture_columns:
        columns += ("ch3_v",)
    frame = load_capture(args.capture_csv, columns)
    events, dt_s = analyze(frame, zero_v, args.shunt_ohms, args.min_samples)

    print(f"same-node CH1-CH4 offset: {zero_v * 1e3:.3f} mV")
    print(f"median sample interval: {dt_s * 1e6:.2f} us")
    print(f"successful state 4->5 write candidates: {len(events)}")
    if not events:
        print("No events met the sample count; use a shorter scope timebase or check code 5.")
        return

    for index, event in enumerate(events, 1):
        print(f"{index}: t={event['start_s']:.6f}s, "
              f"duration={event['duration_ms']:.3f}ms, "
              f"peak={event['current_peak_mA']:.3f}mA, "
              f"board interval={event['board_input_energy_uJ']:.3f}uJ")

    if args.out_csv:
        with args.out_csv.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(events[0]))
            writer.writeheader()
            writer.writerows(events)
        print(f"Saved {args.out_csv}")


if __name__ == "__main__":
    main()
