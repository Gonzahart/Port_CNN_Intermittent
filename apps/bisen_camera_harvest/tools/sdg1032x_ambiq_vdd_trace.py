#!/usr/bin/env python3
"""Replay charging traces into the Apollo4 Plus direct-VDD BISen setup.

This is a separate Ambiq-specific derivative of
``sdg1032x_charge_loop_modified.py``.  It targets the validated bench path:

    Siglent SDG1032X -> rectifier/diode -> storage capacitor -> J7.3/VDD_EXT

The LM2596 is not part of this setup.  The script deliberately does not assume
that function-generator Vpp equals VDD_MCU.  A live replay normally requires a
CSV calibration measured through the complete physical path with the board and
camera load connected.

Scientific intent:

* Preserve the relative energy envelope from the recorded trace.
* Use the calibrated function-generator ceiling to keep the direct VDD replay
  within the validated board range.
* Permit either the prior MSP430-style DC-envelope replay or an RF carrier.
* Optionally turn the source off for zero/very-low trace regions so the
  capacitor can discharge naturally and exercise checkpoint/restore behavior.
* Log every command and its scheduled time for alignment with scope captures.

Typical workflow:

1. List instruments:

   python3 sdg1032x_ambiq_vdd_trace.py list

2. Collect an explicit, conservative calibration.  The amplitude list has no
   default because it must be chosen for the actual rectifier/capacitor setup:

   python3 sdg1032x_ambiq_vdd_trace.py calibrate \
     --resource "USB0::...::INSTR" \
     --trace-output-mode dc \
     --amp-list <comma-separated-command-voltages-you-have-approved> \
     --max-command-v <highest-approved-command-voltage> \
     --output-csv ambiq_fg_vdd_calibration.csv \
     --arm-output

3. Preview the scaled replay without enabling the generator:

   python3 sdg1032x_ambiq_vdd_trace.py preview-trace \
     --trace-file charge_trace2.txt \
     --calibration-csv ambiq_fg_vdd_calibration.csv \
     --trace-output-mode dc \
     --charge-s 4 \
     --steps 100 \
     --preview-csv ambiq_trace_preview.csv

4. Replay after inspecting the preview:

   python3 sdg1032x_ambiq_vdd_trace.py trace-loop \
     --resource "USB0::...::INSTR" \
     --trace-file charge_trace2.txt \
     --calibration-csv ambiq_fg_vdd_calibration.csv \
     --trace-output-mode dc \
     --charge-s 4 \
     --steps 100 \
     --rest-s 1 \
     --cycles 10 \
     --log-csv ambiq_fg_replay_log.csv \
     --arm-output

During direct-VDD replay, disconnect J-Link USB and MCU USB so they do not
back-power or perturb the rail.  Observe VDD_MCU and the state DAC on the scope.
"""

from __future__ import annotations

import argparse
import csv
import math
import socket
import sys
import time
from contextlib import closing
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_FREQ_HZ = 13.56e6
DEFAULT_MAX_VDD_V = 2.20
DEFAULT_MIN_STEP_S = 0.02


def parse_float(text: object) -> float:
    value = str(text).strip()
    suffix_scale = {
        "n": 1e-9,
        "u": 1e-6,
        "µ": 1e-6,
        "m": 1e-3,
        "k": 1e3,
        "K": 1e3,
        "M": 1e6,
        "G": 1e9,
    }

    for unit in ("Hz", "hz", "Vpp", "vpp", "V", "v", "s", "S"):
        if value.endswith(unit):
            value = value[: -len(unit)]

    if value and value[-1] in suffix_scale:
        return float(value[:-1]) * suffix_scale[value[-1]]

    return float(value)


def parse_float_list(text: object) -> list[float]:
    values = [parse_float(item) for item in str(text).split(",") if item.strip()]
    if not values:
        raise argparse.ArgumentTypeError("Expected at least one numeric value")
    return values


class DryRunInstrument:
    def write(self, command: str) -> None:
        print(f"SCPI> {command}")

    def query(self, command: str) -> str:
        print(f"SCPI? {command}")
        return "DRY-RUN,SIGLENT,SDG1032X,0"

    def close(self) -> None:
        pass


class SocketInstrument:
    def __init__(self, ip_address: str, port: int, timeout_s: float):
        self.sock = socket.create_connection((ip_address, port), timeout=timeout_s)
        self.sock.settimeout(timeout_s)

    def write(self, command: str) -> None:
        self.sock.sendall((command + "\n").encode("ascii"))

    def query(self, command: str) -> str:
        self.write(command)
        chunks: list[bytes] = []
        while True:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            if b"\n" in chunk:
                break
        return b"".join(chunks).decode(errors="replace").strip()

    def close(self) -> None:
        self.sock.close()


class VisaInstrument:
    def __init__(self, resource: str, timeout_ms: int):
        import pyvisa

        self.rm = pyvisa.ResourceManager()
        self.inst = self.rm.open_resource(resource)
        self.inst.timeout = timeout_ms
        self.inst.write_termination = "\n"
        self.inst.read_termination = "\n"

    def write(self, command: str) -> None:
        self.inst.write(command)

    def query(self, command: str) -> str:
        return str(self.inst.query(command)).strip()

    def close(self) -> None:
        self.inst.close()
        self.rm.close()


def list_visa_resources() -> int:
    try:
        import pyvisa
    except ImportError:
        print("pyvisa is not installed in this Python environment.", file=sys.stderr)
        return 1

    rm = pyvisa.ResourceManager()
    try:
        resources = rm.list_resources()
        if not resources:
            print("No VISA resources found.")
        else:
            for resource in resources:
                print(resource)
    finally:
        rm.close()
    return 0


def connect(args: argparse.Namespace):
    if args.dry_run:
        return DryRunInstrument()
    if args.ip:
        return SocketInstrument(args.ip, args.port, args.timeout_s)
    if args.resource:
        return VisaInstrument(args.resource, int(args.timeout_s * 1000))
    raise SystemExit("Provide --resource for VISA/USB or --ip for LAN control.")


def output_command(channel: int, state: str, load: str) -> str:
    return f"C{channel}:OUTP {state},LOAD,{load},PLRT,NOR"


def set_output(inst, channel: int, enabled: bool, load: str) -> None:
    inst.write(output_command(channel, "ON" if enabled else "OFF", load))


def configure_carrier(
    inst,
    channel: int,
    freq_hz: float,
    amp_vpp: float,
    offset_v: float,
    phase_deg: float,
    load: str,
) -> None:
    set_output(inst, channel, False, load)
    inst.write(
        f"C{channel}:BSWV "
        f"WVTP,SINE,"
        f"FRQ,{freq_hz:.12g},"
        f"AMP,{max(amp_vpp, 1e-6):.12g},"
        f"OFST,{offset_v:.12g},"
        f"PHSE,{phase_deg:.12g}"
    )


def set_amplitude(inst, channel: int, amp_vpp: float) -> None:
    inst.write(f"C{channel}:BSWV AMP,{max(amp_vpp, 1e-6):.12g}")


def set_offset(inst, channel: int, offset_v: float) -> None:
    inst.write(f"C{channel}:BSWV OFST,{offset_v:.12g}")


def configure_dc(inst, channel: int, command_v: float, offset_v: float, load: str) -> None:
    """Configure a positive DC/envelope command, as in the prior MSP430 replay."""
    set_output(inst, channel, False, load)
    inst.write(f"C{channel}:BSWV WVTP,DC,OFST,{offset_v + command_v:.12g}")


def configure_trace_output(inst, args: argparse.Namespace, command_v: float) -> None:
    command_v = max(0.0, command_v)
    if args.trace_output_mode == "dc":
        configure_dc(inst, args.channel, command_v, args.offset, args.load)
    elif args.trace_output_mode == "unipolar-sine":
        configure_carrier(
            inst,
            args.channel,
            args.freq,
            command_v,
            args.offset + command_v / 2.0,
            args.phase,
            args.load,
        )
    else:
        configure_carrier(
            inst,
            args.channel,
            args.freq,
            command_v,
            args.offset,
            args.phase,
            args.load,
        )


def set_trace_command(inst, args: argparse.Namespace, command_v: float) -> None:
    """Apply one scaled trace command in the selected output mode."""
    command_v = max(0.0, command_v)
    if args.trace_output_mode == "dc":
        set_offset(inst, args.channel, args.offset + command_v)
    elif args.trace_output_mode == "unipolar-sine":
        set_amplitude(inst, args.channel, command_v)
        set_offset(inst, args.channel, args.offset + command_v / 2.0)
    else:
        set_amplitude(inst, args.channel, command_v)
        set_offset(inst, args.channel, args.offset)


def command_units(output_mode: str) -> str:
    return "Vpp" if output_mode == "rf-carrier" else "V"


def percentile(values: Sequence[float], pct: float) -> float:
    if not values:
        raise ValueError("Cannot compute a percentile of an empty sequence")
    if not 0.0 <= pct <= 100.0:
        raise ValueError("Percentile must be between 0 and 100")

    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]

    pos = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def load_trace_file(trace_file: str | Path) -> list[float]:
    values: list[float] = []
    with open(trace_file, "r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [part for part in line.replace(",", " ").split() if part]
            try:
                values.append(max(0.0, float(parts[-1])))
            except (IndexError, ValueError) as exc:
                raise ValueError(
                    f"Could not parse a numeric trace value on line {line_number}: {line!r}"
                ) from exc
    if not values:
        raise ValueError(f"No numeric samples found in {trace_file}")
    return values


def downsample_trace(values: Sequence[float], steps: int, method: str) -> list[float]:
    if steps <= 0 or steps >= len(values):
        return list(values)

    result: list[float] = []
    count = len(values)
    for index in range(steps):
        start = int(round(index * count / steps))
        end = int(round((index + 1) * count / steps))
        if end <= start:
            end = min(start + 1, count)
        chunk = values[start:end]
        if method == "max":
            result.append(max(chunk))
        elif method == "median":
            result.append(percentile(chunk, 50.0))
        else:
            result.append(sum(chunk) / len(chunk))
    return result


@dataclass(frozen=True)
class CalibrationPoint:
    fg_command_v: float
    measured_vdd_v: float
    output_mode: str | None = None
    frequency_hz: float | None = None
    load: str | None = None
    offset_v: float | None = None


def load_calibration(calibration_csv: str | Path) -> list[CalibrationPoint]:
    points: list[CalibrationPoint] = []
    with open(calibration_csv, "r", newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fieldnames = set(reader.fieldnames or [])
        command_column = "fg_command_v" if "fg_command_v" in fieldnames else "fg_vpp"
        if command_column not in fieldnames or "measured_vdd_v" not in fieldnames:
            raise ValueError(
                "Calibration CSV must contain measured_vdd_v and either fg_command_v "
                f"or legacy fg_vpp; got {sorted(fieldnames)}"
            )
        for row_number, row in enumerate(reader, start=2):
            try:
                frequency_hz = float(row["frequency_hz"]) if row.get("frequency_hz") else None
                load = row.get("load") or None
                offset_v = float(row["offset_v"]) if row.get("offset_v") else None
                point = CalibrationPoint(
                    float(row[command_column]),
                    float(row["measured_vdd_v"]),
                    row.get("output_mode") or None,
                    frequency_hz,
                    load,
                    offset_v,
                )
            except (TypeError, ValueError) as exc:
                raise ValueError(f"Invalid calibration value on CSV row {row_number}") from exc
            if point.fg_command_v < 0.0 or point.measured_vdd_v < 0.0:
                raise ValueError(f"Calibration values must be nonnegative on row {row_number}")
            points.append(point)

    if len(points) < 2:
        raise ValueError("Calibration CSV requires at least two measured points")
    points.sort(key=lambda point: point.fg_command_v)
    for previous, current in zip(points, points[1:]):
        if current.fg_command_v <= previous.fg_command_v:
            raise ValueError("Calibration FG command values must be strictly increasing")
        if current.measured_vdd_v < previous.measured_vdd_v:
            raise ValueError(
                "Measured VDD must be nondecreasing with the FG command. "
                "Repeat noisy or inconsistent points."
            )
    return points


def conservative_fg_ceiling(points: Sequence[CalibrationPoint], max_vdd_v: float) -> float:
    safe = [point for point in points if point.measured_vdd_v <= max_vdd_v]
    if not safe:
        raise ValueError(
            f"No calibration point is at or below the VDD ceiling of {max_vdd_v:.3f} V"
        )
    ceiling = max(safe, key=lambda point: point.fg_command_v)
    if ceiling is points[-1] and ceiling.measured_vdd_v < max_vdd_v:
        print(
            "Note: calibration never reached the VDD ceiling. "
            "Using the largest measured FG command without extrapolation.",
            file=sys.stderr,
        )
    return ceiling.fg_command_v


def interpolate_fg_for_vdd(
    points: Sequence[CalibrationPoint], target_vdd_v: float
) -> float | None:
    for point in points:
        if math.isclose(point.measured_vdd_v, target_vdd_v, abs_tol=1e-9):
            return point.fg_command_v
    for lower, upper in zip(points, points[1:]):
        if lower.measured_vdd_v <= target_vdd_v <= upper.measured_vdd_v:
            span = upper.measured_vdd_v - lower.measured_vdd_v
            if span <= 0.0:
                return lower.fg_command_v
            fraction = (target_vdd_v - lower.measured_vdd_v) / span
            return lower.fg_command_v + fraction * (
                upper.fg_command_v - lower.fg_command_v
            )
    return None


@dataclass(frozen=True)
class ReplayStep:
    index: int
    trace_value: float
    normalized: float
    fg_command_v: float
    output_enabled: bool


def build_replay_steps(
    trace_values: Sequence[float],
    reference_percentile: float,
    fg_floor_v: float,
    fg_ceiling_v: float,
    off_below_fraction: float,
    off_below_command_v: float,
) -> tuple[list[ReplayStep], float]:
    reference = percentile(trace_values, reference_percentile)
    if reference <= 0.0:
        raise ValueError("The selected trace reference percentile is zero")
    if not 0.0 <= off_below_fraction < 1.0:
        raise ValueError("--off-below-fraction must be in [0, 1)")
    if fg_floor_v < 0.0 or fg_ceiling_v <= 0.0:
        raise ValueError("FG floor/ceiling values must be nonnegative and the ceiling positive")
    if fg_floor_v > fg_ceiling_v:
        raise ValueError("The FG floor cannot exceed the calibrated FG ceiling")

    span = fg_ceiling_v - fg_floor_v
    steps: list[ReplayStep] = []
    for index, value in enumerate(trace_values):
        normalized = min(1.0, max(0.0, value / reference))
        fg_command_v = fg_floor_v + normalized * span
        enabled = fg_command_v > off_below_command_v
        if off_below_fraction > 0.0 and normalized <= off_below_fraction:
            enabled = False
        steps.append(ReplayStep(index, value, normalized, fg_command_v, enabled))
    return steps, reference


def trace_stats(values: Sequence[float]) -> dict[str, float]:
    return {
        "samples": float(len(values)),
        "min": min(values),
        "max": max(values),
        "mean": sum(values) / len(values),
        "p95": percentile(values, 95.0),
        "p99": percentile(values, 99.0),
        "nonzero_percent": 100.0 * sum(value > 0.0 for value in values) / len(values),
    }


def resolve_fg_ceiling(args: argparse.Namespace, live: bool) -> tuple[float, list[CalibrationPoint]]:
    points: list[CalibrationPoint] = []
    if args.calibration_csv:
        points = load_calibration(args.calibration_csv)
        calibration_modes = {point.output_mode for point in points if point.output_mode is not None}
        if calibration_modes and calibration_modes != {args.trace_output_mode}:
            raise SystemExit("Replay output mode does not match the calibration CSV")
        if live:
            if not calibration_modes:
                raise SystemExit(
                    "Live replay requires a mode-tagged calibration CSV generated by this "
                    "Ambiq script; legacy fg_vpp calibration files are preview-only."
                )
            calibration_frequencies = {point.frequency_hz for point in points if point.frequency_hz is not None}
            calibration_loads = {point.load for point in points if point.load is not None}
            calibration_offsets = {point.offset_v for point in points if point.offset_v is not None}
            if args.trace_output_mode != "dc" and calibration_frequencies and any(
                not math.isclose(value, args.freq, rel_tol=1e-9, abs_tol=1e-6)
                for value in calibration_frequencies
            ):
                raise SystemExit("Replay frequency does not match the calibration CSV")
            if calibration_loads and calibration_loads != {args.load}:
                raise SystemExit("Replay load setting does not match the calibration CSV")
            if calibration_offsets and any(
                not math.isclose(value, args.offset, rel_tol=0.0, abs_tol=1e-9)
                for value in calibration_offsets
            ):
                raise SystemExit("Replay DC offset does not match the calibration CSV")
        ceiling = conservative_fg_ceiling(points, args.max_vdd_v)
        if args.fg_max_v is not None:
            ceiling = min(ceiling, args.fg_max_v)
        return ceiling, points

    if args.fg_max_v is None:
        raise SystemExit("Provide --calibration-csv, or --trace-max-vpp for preview only.")
    if live:
        raise SystemExit(
            "Live direct-VDD replay requires --calibration-csv measured with the final hardware path."
        )
    return args.fg_max_v, points


def prepare_replay(args: argparse.Namespace, live: bool):
    raw_values = load_trace_file(args.trace_file)
    step_values = downsample_trace(raw_values, args.steps, args.step_method)
    fg_ceiling, calibration = resolve_fg_ceiling(args, live=live)
    steps, reference = build_replay_steps(
        step_values,
        args.trace_percentile,
        args.fg_floor_v,
        fg_ceiling,
        args.off_below_fraction,
        args.off_below_command_v,
    )

    if args.max_vdd_v > DEFAULT_MAX_VDD_V:
        raise SystemExit(
            f"--max-vdd-v exceeds the validated {DEFAULT_MAX_VDD_V:.2f} V ceiling. "
            "Update the script only after a separate hardware review."
        )
    if args.duration_s <= 0.0:
        raise SystemExit("--charge-s/--duration-s must be positive and sets one full replay duration.")
    step_s = args.duration_s / len(steps)
    if step_s < args.min_step_s:
        raise SystemExit(
            f"Requested {step_s:.6g} s updates are below --min-step-s={args.min_step_s:g}. "
            "Use fewer --steps or a longer --charge-s."
        )
    return raw_values, step_values, steps, calibration, fg_ceiling, reference, step_s


def print_replay_summary(
    args: argparse.Namespace,
    raw_values: Sequence[float],
    step_values: Sequence[float],
    steps: Sequence[ReplayStep],
    calibration: Sequence[CalibrationPoint],
    fg_ceiling: float,
    reference: float,
    step_s: float,
) -> None:
    raw = trace_stats(raw_values)
    downsampled = trace_stats(step_values)
    on_steps = [step.fg_command_v for step in steps if step.output_enabled]
    units = command_units(args.trace_output_mode)
    print("Ambiq direct-VDD trace replay summary")
    print(f"  trace: {args.trace_file}")
    print(f"  raw samples: {int(raw['samples'])}")
    print(f"  raw min/max: {raw['min']:.9g}, {raw['max']:.9g}")
    print(f"  raw p95/p99: {raw['p95']:.9g}, {raw['p99']:.9g}")
    print(f"  downsampled steps: {int(downsampled['samples'])} ({args.step_method})")
    print(f"  duration: {args.duration_s:g} s; update interval: {step_s:.6g} s")
    print(f"  normalization reference: p{args.trace_percentile:g}={reference:.9g}")
    print(f"  output mode: {args.trace_output_mode}")
    print(f"  calibrated FG command ceiling: {fg_ceiling:.6g} {units}")
    print(f"  FG command floor: {args.fg_floor_v:.6g} {units}")
    print(f"  output-off command threshold: {args.off_below_command_v:.6g} {units}")
    print(f"  output-off steps: {sum(not step.output_enabled for step in steps)}/{len(steps)}")
    if on_steps:
        print(f"  active FG command range: {min(on_steps):.6g} to {max(on_steps):.6g} {units}")
    print(f"  VDD ceiling used for calibration selection: {args.max_vdd_v:.3f} V")
    if calibration:
        print(f"  calibration: {args.calibration_csv} ({len(calibration)} points)")
        for target in (1.90, 2.00, 2.10, 2.20):
            command = interpolate_fg_for_vdd(calibration, target)
            if command is not None:
                print(f"  static reference: VDD {target:.2f} V ~= FG {command:.6g} {units}")
        print(
            "  Static references are descriptive only; the storage capacitor and changing load "
            "determine the dynamic VDD trajectory."
        )
    else:
        print("  WARNING: preview uses an explicit uncalibrated FG ceiling; live replay is blocked.")
    if all(step.output_enabled for step in steps):
        if args.off_below_command_v < 0.0:
            print(
                "  Note: --off-below-vpp is negative, so the output intentionally remains "
                "enabled during 0 V trace steps; it still turns off during --rest-s.",
                file=sys.stderr,
            )
        else:
            print(
                "  WARNING: downsampling produced no output-off steps. The capacitor may never "
                "reach a true no-input interval. Use more --steps or explicitly document a "
                "small --off-below-fraction.",
                file=sys.stderr,
            )


def write_preview_csv(
    path_value: str | None, steps: Sequence[ReplayStep], step_s: float
) -> None:
    if not path_value:
        return
    path = Path(path_value)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "step",
                "scheduled_time_s",
                "trace_value",
                "normalized_trace",
                "fg_command_v",
                "output_enabled",
            ]
        )
        for step in steps:
            writer.writerow(
                [
                    step.index,
                    step.index * step_s,
                    step.trace_value,
                    step.normalized,
                    step.fg_command_v,
                    int(step.output_enabled),
                ]
            )
    print(f"Saved preview CSV: {path}")


def run_preview_trace(args: argparse.Namespace) -> None:
    prepared = prepare_replay(args, live=False)
    raw_values, step_values, steps, calibration, fg_ceiling, reference, step_s = prepared
    print_replay_summary(
        args, raw_values, step_values, steps, calibration, fg_ceiling, reference, step_s
    )
    write_preview_csv(args.preview_csv, steps, step_s)


def require_live_arm(args: argparse.Namespace) -> None:
    if args.dry_run:
        return
    if not args.arm_output:
        raise SystemExit(
            "Live output is disarmed. Re-run with --arm-output only after verifying: "
            "LM2596 absent; J-Link/MCU USB disconnected; scope on VDD_MCU; calibration matches "
            "this exact rectifier, capacitor, wiring, frequency, and load."
        )


def run_trace_loop(args: argparse.Namespace) -> None:
    prepared = prepare_replay(args, live=True)
    raw_values, step_values, steps, calibration, fg_ceiling, reference, step_s = prepared
    print_replay_summary(
        args, raw_values, step_values, steps, calibration, fg_ceiling, reference, step_s
    )
    write_preview_csv(args.preview_csv, steps, step_s)
    require_live_arm(args)

    log_handle = None
    log_writer = None
    if args.log_csv:
        log_handle = open(args.log_csv, "w", newline="", encoding="utf-8")
        log_writer = csv.writer(log_handle)
        log_writer.writerow(
            [
                "unix_time_s",
                "monotonic_time_s",
                "cycle",
                "step",
                "scheduled_time_s",
                "trace_value",
                "normalized_trace",
                "fg_command_v",
                "output_mode",
                "output_enabled",
            ]
        )

    with closing(connect(args)) as inst:
        output_enabled = False
        try:
            identity = inst.query("*IDN?")
            print(f"Connected to: {identity}")
            configure_trace_output(inst, args, max(args.fg_floor_v, 0.0))

            cycle = 0
            while args.cycles == 0 or cycle < args.cycles:
                cycle += 1
                cycle_start = time.monotonic()
                print(
                    f"cycle={cycle} start steps={len(steps)} step_s={step_s:.6g}",
                    flush=True,
                )
                for step in steps:
                    if step.output_enabled:
                        set_trace_command(inst, args, step.fg_command_v)
                        if not output_enabled:
                            set_output(inst, args.channel, True, args.load)
                            output_enabled = True
                    elif output_enabled:
                        set_output(inst, args.channel, False, args.load)
                        output_enabled = False

                    now = time.monotonic()
                    if log_writer:
                        log_writer.writerow(
                            [
                                time.time(),
                                now,
                                cycle,
                                step.index,
                                step.index * step_s,
                                step.trace_value,
                                step.normalized,
                                step.fg_command_v,
                                args.trace_output_mode,
                                int(step.output_enabled),
                            ]
                        )

                    deadline = cycle_start + (step.index + 1) * step_s
                    remaining = deadline - time.monotonic()
                    if remaining > 0.0:
                        time.sleep(remaining)

                set_output(inst, args.channel, False, args.load)
                output_enabled = False
                elapsed = time.monotonic() - cycle_start
                print(f"cycle={cycle} end elapsed={elapsed:.6g} s rest={args.rest_s:g} s", flush=True)
                if args.rest_s > 0.0:
                    time.sleep(args.rest_s)
        except KeyboardInterrupt:
            print("\nInterrupted; turning the generator output off.", file=sys.stderr)
            raise
        finally:
            set_output(inst, args.channel, False, args.load)
            if log_handle:
                log_handle.close()


def save_calibration(path_value: str | Path, points: Iterable[CalibrationPoint]) -> None:
    path = Path(path_value)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "fg_command_v",
                "measured_vdd_v",
                "output_mode",
                "frequency_hz",
                "load",
                "offset_v",
            ]
        )
        for point in points:
            writer.writerow(
                [
                    point.fg_command_v,
                    point.measured_vdd_v,
                    point.output_mode or "",
                    point.frequency_hz if point.frequency_hz is not None else "",
                    point.load or "",
                    point.offset_v if point.offset_v is not None else "",
                ]
            )
    print(f"Saved calibration CSV: {path}")


def run_calibrate(args: argparse.Namespace) -> None:
    amplitudes = args.amp_list
    if any(value <= 0.0 for value in amplitudes):
        raise SystemExit("Calibration amplitudes must be positive")
    if amplitudes != sorted(set(amplitudes)):
        raise SystemExit("--amp-list must contain unique, increasing values")
    if amplitudes[-1] > args.max_command_v:
        raise SystemExit("An --amp-list value exceeds --max-command-v")
    if args.max_vdd_v > DEFAULT_MAX_VDD_V:
        raise SystemExit(
            f"--max-vdd-v exceeds the validated {DEFAULT_MAX_VDD_V:.2f} V ceiling"
        )
    require_live_arm(args)

    print("Ambiq direct-VDD calibration")
    print(
        "  Use the exact rectifier, capacitor, wiring, output mode, and board load "
        "intended for replay."
    )
    print("  LM2596 absent. J-Link USB and MCU USB disconnected during each measurement.")
    print(
        f"  Output mode: {args.trace_output_mode}; "
        f"command units: {command_units(args.trace_output_mode)}"
    )
    print(f"  Hard-stop VDD entry: {args.max_vdd_v:.3f} V")
    print("  The generator output turns off after every entered point.")

    measured: list[CalibrationPoint] = []
    with closing(connect(args)) as inst:
        try:
            print(f"Connected to: {inst.query('*IDN?')}")
            configure_trace_output(inst, args, amplitudes[0])
            units = command_units(args.trace_output_mode)
            for command_v in amplitudes:
                if not args.dry_run:
                    response = input(
                        f"Press Enter to apply {command_v:g} {units}, or type q to stop calibration: "
                    ).strip().lower()
                    if response == "q":
                        break

                set_trace_command(inst, args, command_v)
                set_output(inst, args.channel, True, args.load)
                time.sleep(args.settle_s)

                if args.dry_run:
                    print("Dry run: no DMM value recorded")
                    set_output(inst, args.channel, False, args.load)
                    continue

                entry = input(
                    "Measure VDD_MCU at J7.1/J7.2 and enter volts immediately: "
                ).strip()
                set_output(inst, args.channel, False, args.load)
                try:
                    measured_vdd = parse_float(entry)
                except ValueError:
                    print("Invalid measurement; point not recorded.", file=sys.stderr)
                    continue

                point = CalibrationPoint(
                    command_v,
                    measured_vdd,
                    args.trace_output_mode,
                    args.freq,
                    args.load,
                    args.offset,
                )
                measured.append(point)
                save_calibration(args.output_csv, measured)
                print(
                    f"Recorded: FG={command_v:g} {units}, "
                    f"VDD_MCU={measured_vdd:.4f} V"
                )
                if measured_vdd >= args.max_vdd_v:
                    print(
                        "VDD ceiling reached. Calibration stopped; do not increase generator amplitude.",
                        file=sys.stderr,
                    )
                    break
        finally:
            set_output(inst, args.channel, False, args.load)

    if len(measured) < 2 and not args.dry_run:
        raise SystemExit("Calibration ended with fewer than two valid points")


def run_off(args: argparse.Namespace) -> None:
    with closing(connect(args)) as inst:
        print(f"Connected to: {inst.query('*IDN?')}")
        set_output(inst, args.channel, False, args.load)


def add_connection_args(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--resource", help="VISA resource such as USB0::...::INSTR")
    group.add_argument("--ip", help="LAN socket IP address")
    parser.add_argument("--port", type=int, default=5025)
    parser.add_argument("--timeout-s", type=float, default=3.0)
    parser.add_argument("--dry-run", action="store_true", help="Print SCPI commands without opening an instrument")


def add_generator_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--channel", type=int, choices=(1, 2), default=1)
    parser.add_argument("--freq", type=parse_float, default=DEFAULT_FREQ_HZ)
    parser.add_argument("--offset", type=parse_float, default=0.0)
    parser.add_argument("--phase", type=float, default=0.0)
    parser.add_argument("--load", choices=("HZ", "50"), default="HZ")


def add_live_safety_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--arm-output",
        action="store_true",
        help="Required acknowledgement before enabling a real generator output",
    )
    parser.add_argument(
        "--max-vdd-v",
        type=parse_float,
        default=DEFAULT_MAX_VDD_V,
        help="Select the conservative FG ceiling from calibration points at or below this VDD",
    )


def add_trace_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--trace-file", required=True)
    parser.add_argument(
        "--trace-output-mode",
        choices=("dc", "unipolar-sine", "rf-carrier"),
        default="dc",
        help=(
            "dc reproduces the prior MSP430 envelope replay; unipolar-sine ranges from "
            "offset to offset+command; rf-carrier uses command as Vpp"
        ),
    )
    parser.add_argument(
        "--duration-s",
        "--charge-s",
        dest="duration_s",
        type=parse_float,
        required=True,
        help="Duration of one complete trace playback; --charge-s is the legacy name",
    )
    parser.add_argument("--steps", type=int, default=600)
    parser.add_argument("--step-method", choices=("mean", "max", "median"), default="mean")
    parser.add_argument("--trace-percentile", type=float, default=99.0)
    parser.add_argument(
        "--calibration-csv",
        help="Mode-matched CSV with FG command and measured_vdd_v columns; required live",
    )
    parser.add_argument(
        "--fg-max-v",
        "--fg-max-vpp",
        "--trace-max-vpp",
        dest="fg_max_v",
        type=parse_float,
        help=(
            "Optional command cap (DC volts in dc mode, Vpp in rf-carrier mode), or an "
            "uncalibrated ceiling for preview only"
        ),
    )
    parser.add_argument(
        "--fg-floor-v",
        "--fg-floor-vpp",
        "--trace-min-vpp",
        dest="fg_floor_v",
        type=parse_float,
        default=0.0,
        help="Command floor. Default zero adds no DC/RF bias.",
    )
    parser.add_argument(
        "--off-below-fraction",
        type=float,
        default=0.0,
        help="Optional normalized cutoff; zero disables this extra cutoff",
    )
    parser.add_argument(
        "--off-below-vpp",
        dest="off_below_command_v",
        type=parse_float,
        default=0.0,
        help=(
            "Turn output off at/below this command. Use -1 to keep the output enabled at a "
            "0 V trace step as in the prior MSP430 command; use 0 for true source-off zeros. "
            "The legacy option name is retained."
        ),
    )
    parser.add_argument("--min-step-s", type=parse_float, default=DEFAULT_MIN_STEP_S)
    parser.add_argument("--preview-csv")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Safely replay RF traces into the Apollo4 Plus direct-VDD BISen setup."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("list", help="List VISA resources")

    off_parser = subparsers.add_parser("off", help="Turn a generator channel off")
    add_connection_args(off_parser)
    off_parser.add_argument("--channel", type=int, choices=(1, 2), default=1)
    off_parser.add_argument("--load", choices=("HZ", "50"), default="HZ")

    calibrate_parser = subparsers.add_parser(
        "calibrate", help="Interactively measure FG command to loaded VDD_MCU"
    )
    add_connection_args(calibrate_parser)
    add_generator_args(calibrate_parser)
    add_live_safety_args(calibrate_parser)
    calibrate_parser.add_argument(
        "--trace-output-mode",
        choices=("dc", "unipolar-sine", "rf-carrier"),
        default="dc",
    )
    calibrate_parser.add_argument("--amp-list", type=parse_float_list, required=True)
    calibrate_parser.add_argument(
        "--max-command-v",
        "--max-command-vpp",
        dest="max_command_v",
        type=parse_float,
        required=True,
    )
    calibrate_parser.add_argument("--settle-s", type=parse_float, default=3.0)
    calibrate_parser.add_argument("--output-csv", required=True)

    preview_parser = subparsers.add_parser(
        "preview-trace", help="Analyze the Ambiq-scaled replay without controlling the generator"
    )
    add_trace_args(preview_parser)
    preview_parser.add_argument("--max-vdd-v", type=parse_float, default=DEFAULT_MAX_VDD_V)

    replay_parser = subparsers.add_parser(
        "trace-loop", help="Replay a calibrated DC envelope or RF amplitude trace"
    )
    add_connection_args(replay_parser)
    add_generator_args(replay_parser)
    add_live_safety_args(replay_parser)
    add_trace_args(replay_parser)
    replay_parser.add_argument("--cycles", type=int, default=1, help="0 repeats until Ctrl-C")
    replay_parser.add_argument("--rest-s", type=parse_float, default=10.0)
    replay_parser.add_argument("--log-csv")

    args = parser.parse_args()
    if args.command == "list":
        return list_visa_resources()
    if args.command == "off":
        run_off(args)
    elif args.command == "calibrate":
        run_calibrate(args)
    elif args.command == "preview-trace":
        run_preview_trace(args)
    elif args.command == "trace-loop":
        run_trace_loop(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
