#!/usr/bin/env python3
"""Replay a harvested-energy trace into the Apollo4 VCAP reservoir.

Current physical path:

    Siglent SDG1032X -> series diode -> 10 mF VCAP bank -> MP1584EN -> Apollo4

GPIO16 observes VCAP through the calibrated divider.  DC/envelope mode is the
appropriate mode for a post-rectifier harvested-voltage trace.  The older
``sdg1032x_ambiq_vdd_trace.py`` targets a direct-VDD circuit and must not be
used for this regulator/reservoir topology.

Main uses:
  1. Output a fixed RF-like sine wave.
  2. Loop output on/off for charge/rest tests.
  3. Read an RF/harvested-energy trace text file and replay it either as:
     - a positive-only DC/envelope voltage,
     - a positive-only unipolar sine, or
     - a bipolar RF sine carrier amplitude envelope.

The charge_trace*.txt files contain positive millivolt-scale samples, so the
trace must be scaled to function-generator command volts.  For the validated
bench path, an 8.0 V FG command produced about 7.5 V VCAP.  Live trace replay
therefore has a default 8.0 V command ceiling and requires ``--arm-output``.

Examples:
  python3 sdg1032x_charge_loop_modified.py list

  python3 sdg1032x_charge_loop_modified.py on \
    --resource "USB0::0xF4EC::..." \
    --freq 13.56M \
    --amp-vpp 1.0

  python3 sdg1032x_charge_loop_modified.py loop \
    --resource "USB0::0xF4EC::..." \
    --freq 13.56M \
    --amp-list 0.5,1.0,1.5 \
    --charge-s 10 \
    --rest-s 2 \
    --cycles 3

  python3 sdg1032x_charge_loop_modified.py preview-trace \
    --trace-file charge_trace2.txt \
    --trace-max-vpp 2.0 \
    --steps 200

  Positive-only DC/envelope replay, recommended when the txt file is a rectified
  harvested voltage or capacitor-input voltage trace:

  python3 sdg1032x_charge_loop_modified.py trace-loop \
    --resource "USB0::0xF4EC::..." \
    --trace-file charge_trace2.txt \
    --trace-output-mode dc \
    --trace-max-vpp 2.0 \
    --steps 200 \
    --charge-s 10 \
    --rest-s 2 \
    --cycles 5 \
    --log-csv fg_trace_log.csv

  Positive-only sine replay, useful when you still want a carrier but no negative
  generator output. For each trace step, the sine ranges approximately 0 to Vstep:

  python3 sdg1032x_charge_loop_modified.py trace-loop \
    --resource "USB0::0xF4EC::..." \
    --trace-file charge_trace2.txt \
    --trace-output-mode unipolar-sine \
    --freq 1M \
    --trace-max-vpp 2.0 \
    --steps 200 \
    --charge-s 10
"""

import argparse
import csv
import math
import socket
import sys
import time
from contextlib import closing
from pathlib import Path


DEFAULT_MAX_COMMAND_V = 8.0


def parse_float(text):
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
            value = value[:-len(unit)]

    if value and value[-1] in suffix_scale:
        return float(value[:-1]) * suffix_scale[value[-1]]

    return float(value)


def parse_amp_list(text):
    return [parse_float(item) for item in str(text).split(",") if item.strip()]


class DryRunInstrument:
    def write(self, command):
        print(f"SCPI> {command}")

    def query(self, command):
        print(f"SCPI? {command}")
        return "DRY-RUN,SIGLENT,SDG1032X,0"

    def close(self):
        pass


class SocketInstrument:
    def __init__(self, ip_address, port, timeout_s):
        self.sock = socket.create_connection((ip_address, port), timeout=timeout_s)
        self.sock.settimeout(timeout_s)

    def write(self, command):
        self.sock.sendall((command + "\n").encode("ascii"))

    def query(self, command):
        self.write(command)
        chunks = []

        while True:
            chunk = self.sock.recv(4096)

            if not chunk:
                break

            chunks.append(chunk)

            if b"\n" in chunk:
                break

        return b"".join(chunks).decode(errors="replace").strip()

    def close(self):
        self.sock.close()


class VisaInstrument:
    def __init__(self, resource, timeout_ms):
        import pyvisa

        self.rm = pyvisa.ResourceManager()
        self.inst = self.rm.open_resource(resource)
        self.inst.timeout = timeout_ms
        self.inst.write_termination = "\n"
        self.inst.read_termination = "\n"

    def write(self, command):
        self.inst.write(command)

    def query(self, command):
        return str(self.inst.query(command)).strip()

    def close(self):
        self.inst.close()
        self.rm.close()


def list_visa_resources():
    try:
        import pyvisa
    except ImportError:
        print("pyvisa is not installed in this Python environment.", file=sys.stderr)
        return 1

    rm = pyvisa.ResourceManager()
    resources = rm.list_resources()

    if not resources:
        print("No VISA resources found.")
    else:
        for resource in resources:
            print(resource)

    rm.close()
    return 0


def connect(args):
    if args.dry_run:
        return DryRunInstrument()

    if args.ip:
        return SocketInstrument(args.ip, args.port, args.timeout_s)

    if args.resource:
        return VisaInstrument(args.resource, int(args.timeout_s * 1000))

    raise SystemExit("Provide --resource for VISA/USB or --ip for LAN socket control.")


def output_command(channel, state, load):
    return f"C{channel}:OUTP {state},LOAD,{load},PLRT,NOR"


def configure_basic_wave(inst, channel, wave, freq_hz, amp_vpp, offset_v, phase_deg, load):
    inst.write(output_command(channel, "OFF", load))
    inst.write(
        f"C{channel}:BSWV "
        f"WVTP,{wave},"
        f"FRQ,{freq_hz:.12g},"
        f"AMP,{amp_vpp:.12g},"
        f"OFST,{offset_v:.12g},"
        f"PHSE,{phase_deg:.12g}"
    )


def set_amplitude(inst, channel, amp_vpp):
    inst.write(f"C{channel}:BSWV AMP,{amp_vpp:.12g}")


def set_frequency(inst, channel, freq_hz):
    inst.write(f"C{channel}:BSWV FRQ,{freq_hz:.12g}")


def set_offset(inst, channel, offset_v):
    inst.write(f"C{channel}:BSWV OFST,{offset_v:.12g}")


def configure_dc_wave(inst, channel, dc_v, load):
    """Configure the output as a DC level.

    Siglent SDG1000X generators support a DC basic waveform. In this mode the
    trace value is treated as the output voltage level, not as Vpp.
    """
    inst.write(output_command(channel, "OFF", load))
    inst.write(f"C{channel}:BSWV WVTP,DC,OFST,{dc_v:.12g}")


def configure_trace_wave(inst, args, initial_level):
    """Configure the generator for one of the trace replay modes."""
    level = max(0.0, initial_level)

    if args.trace_output_mode == "dc":
        configure_dc_wave(inst, args.channel, args.offset + level, args.load)
    elif args.trace_output_mode == "unipolar-sine":
        # Output range is approximately offset to offset + level.
        configure_basic_wave(
            inst,
            args.channel,
            "SINE",
            args.freq,
            max(level, 1e-6),
            args.offset + (level / 2.0),
            args.phase,
            args.load,
        )
    else:
        # rf-carrier mode: traditional bipolar sine centered around args.offset.
        # Output range is approximately offset ± level/2.
        configure_basic_wave(
            inst,
            args.channel,
            args.wave,
            args.freq,
            max(level, 1e-6),
            args.offset,
            args.phase,
            args.load,
        )


def set_trace_level(inst, args, level):
    """Update the output for one trace step.

    level is the scaled trace command. In dc/unipolar-sine modes it represents
    the maximum positive output voltage. In rf-carrier mode it represents Vpp.
    """
    level = max(0.0, level)

    if args.trace_output_mode == "dc":
        set_offset(inst, args.channel, args.offset + level)
    elif args.trace_output_mode == "unipolar-sine":
        set_amplitude(inst, args.channel, max(level, 1e-6))
        set_offset(inst, args.channel, args.offset + (level / 2.0))
    else:
        set_amplitude(inst, args.channel, max(level, 1e-6))
        set_offset(inst, args.channel, args.offset)


def set_output(inst, channel, enabled, load):
    inst.write(output_command(channel, "ON" if enabled else "OFF", load))


def warn_for_high_amplitude(amp_vpp, offset_v, safe_vpp):
    if amp_vpp > safe_vpp or abs(offset_v) > 1.0:
        print(
            "Warning: large function-generator setting for an MSP430 energy-harvesting node. "
            "Verify the rectifier/regulator/storage-cap voltage on the scope before connecting VCC.",
            file=sys.stderr,
        )


def percentile(values, pct):
    if not values:
        raise ValueError("Cannot compute percentile of empty trace")

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


def load_trace_file(trace_file):
    values = []

    with open(trace_file, "r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()

            if not line or line.startswith("#"):
                continue

            line = line.replace(",", " ")
            parts = [part for part in line.split() if part]

            # Accept one-column files or files where the last column is the voltage/envelope sample.
            try:
                values.append(float(parts[-1]))
            except ValueError as exc:
                raise ValueError(f"Could not parse numeric value on line {line_number}: {line!r}") from exc

    if not values:
        raise ValueError(f"No numeric samples found in {trace_file}")

    return values


def clip_negative_to_zero(values):
    return [value if value > 0.0 else 0.0 for value in values]


def downsample_trace(values, steps, method):
    if steps <= 0 or steps >= len(values):
        return list(values)

    result = []
    n = len(values)

    for i in range(steps):
        start = int(round(i * n / steps))
        end = int(round((i + 1) * n / steps))

        if end <= start:
            end = min(start + 1, n)

        chunk = values[start:end]

        if method == "max":
            result.append(max(chunk))
        elif method == "median":
            result.append(percentile(chunk, 50.0))
        else:
            result.append(sum(chunk) / len(chunk))

    return result


def scale_trace_to_amplitudes(values, args):
    cleaned = clip_negative_to_zero(values)

    if args.trace_scale_mode == "raw":
        amps = cleaned

    elif args.trace_scale_mode == "multiplier":
        amps = [value * args.trace_multiplier for value in cleaned]

    elif args.trace_scale_mode == "normalized":
        raw_min = min(cleaned)
        raw_max = max(cleaned)

        if raw_max <= raw_min:
            amps = [args.trace_min_vpp for _ in cleaned]
        else:
            span = raw_max - raw_min
            out_span = args.trace_max_vpp - args.trace_min_vpp
            amps = [args.trace_min_vpp + ((value - raw_min) / span) * out_span for value in cleaned]

    else:
        ref = percentile(cleaned, args.trace_percentile)

        if ref <= 0.0:
            raise ValueError("Trace scale reference is zero; cannot auto-scale. Try --trace-scale-mode raw or multiplier.")

        scale = args.trace_max_vpp / ref
        amps = [value * scale for value in cleaned]

    amps = [max(args.trace_min_vpp, min(args.trace_max_vpp, value)) for value in amps]
    return amps


def trace_stats(values):
    cleaned = clip_negative_to_zero(values)

    return {
        "samples": len(cleaned),
        "min": min(cleaned),
        "max": max(cleaned),
        "mean": sum(cleaned) / len(cleaned),
        "p95": percentile(cleaned, 95.0),
        "p99": percentile(cleaned, 99.0),
        "nonzero_percent": 100.0 * sum(1 for value in cleaned if value > 0.0) / len(cleaned),
    }


def print_trace_stats(raw_values, step_values, amp_values, args):
    raw = trace_stats(raw_values)
    steps = trace_stats(step_values)
    amps = trace_stats(amp_values)

    print("Trace statistics")
    print(f"  file: {args.trace_file}")
    print(f"  raw samples: {raw['samples']}")
    print(f"  raw min/max: {raw['min']:.9g} V, {raw['max']:.9g} V")
    print(f"  raw p95/p99: {raw['p95']:.9g} V, {raw['p99']:.9g} V")
    print(f"  raw mean: {raw['mean']:.9g} V")
    print(f"  raw nonzero: {raw['nonzero_percent']:.2f}%")
    print(f"  downsampled steps: {steps['samples']}")
    print(f"  step method: {args.step_method}")
    print(f"  scaling mode: {args.trace_scale_mode}")
    if args.trace_output_mode == "rf-carrier":
        unit = "Vpp carrier amplitude"
    else:
        unit = "V positive output"

    print(f"  trace output mode: {args.trace_output_mode}")
    print(f"  output command min/max: {amps['min']:.9g} {unit}, {amps['max']:.9g} {unit}")
    print(f"  output command p95/p99: {amps['p95']:.9g} {unit}, {amps['p99']:.9g} {unit}")
    print(f"  output command mean: {amps['mean']:.9g} {unit}")


def build_trace_amplitudes(args):
    raw_values = load_trace_file(args.trace_file)
    step_values = downsample_trace(raw_values, args.steps, args.step_method)
    amp_values = scale_trace_to_amplitudes(step_values, args)
    return raw_values, step_values, amp_values


def write_trace_preview_csv(args, raw_values, step_values, amp_values):
    if not args.preview_csv:
        return

    with open(args.preview_csv, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["step", "raw_or_downsampled_value_v", "amp_vpp"])

        for i, (trace_value, amp) in enumerate(zip(step_values, amp_values)):
            writer.writerow([i, trace_value, amp])

    print(f"Saved preview CSV: {args.preview_csv}")


def run_on(args):
    warn_for_high_amplitude(args.amp_vpp, args.offset, args.safe_vpp)

    with closing(connect(args)) as inst:
        print(inst.query("*IDN?"))
        configure_basic_wave(inst, args.channel, args.wave, args.freq, args.amp_vpp, args.offset, args.phase, args.load)
        set_output(inst, args.channel, True, args.load)

        if args.duration_s > 0:
            time.sleep(args.duration_s)
            set_output(inst, args.channel, False, args.load)


def run_off(args):
    with closing(connect(args)) as inst:
        print(inst.query("*IDN?"))
        set_output(inst, args.channel, False, args.load)


def build_amplitudes(args):
    if args.amp_list:
        return parse_amp_list(args.amp_list)

    if args.amp_start is None:
        return [args.amp_vpp]

    if args.amp_stop is None or args.amp_step is None:
        raise SystemExit("--amp-start requires --amp-stop and --amp-step.")

    amps = []
    value = args.amp_start

    if args.amp_step == 0:
        raise SystemExit("--amp-step cannot be zero.")

    if args.amp_step > 0:
        while value <= args.amp_stop + 1e-12:
            amps.append(value)
            value += args.amp_step
    else:
        while value >= args.amp_stop - 1e-12:
            amps.append(value)
            value += args.amp_step

    return amps


def run_loop(args):
    amplitudes = build_amplitudes(args)

    for amp in amplitudes:
        warn_for_high_amplitude(amp, args.offset, args.safe_vpp)

    with closing(connect(args)) as inst:
        print(inst.query("*IDN?"))
        configure_basic_wave(inst, args.channel, args.wave, args.freq, amplitudes[0], args.offset, args.phase, args.load)

        cycle = 0

        try:
            while args.cycles == 0 or cycle < args.cycles:
                cycle += 1

                for amp in amplitudes:
                    set_amplitude(inst, args.channel, amp)
                    print(f"cycle={cycle} amp_vpp={amp:g} output=on charge_s={args.charge_s:g}", flush=True)
                    set_output(inst, args.channel, True, args.load)
                    time.sleep(args.charge_s)

                    print(f"cycle={cycle} amp_vpp={amp:g} output=off rest_s={args.rest_s:g}", flush=True)
                    set_output(inst, args.channel, False, args.load)

                    if args.rest_s > 0:
                        time.sleep(args.rest_s)

        except KeyboardInterrupt:
            print("\nInterrupted; turning output off.", file=sys.stderr)
            set_output(inst, args.channel, False, args.load)
            raise

        set_output(inst, args.channel, False, args.load)


def run_preview_trace(args):
    raw_values, step_values, amp_values = build_trace_amplitudes(args)
    print_trace_stats(raw_values, step_values, amp_values, args)
    write_trace_preview_csv(args, raw_values, step_values, amp_values)


def run_trace_loop(args):
    raw_values, step_values, amp_values = build_trace_amplitudes(args)
    print_trace_stats(raw_values, step_values, amp_values, args)
    write_trace_preview_csv(args, raw_values, step_values, amp_values)

    for amp in amp_values:
        warn_for_high_amplitude(amp, args.offset, args.safe_vpp)

    if args.trace_output_mode == "dc":
        peak_command_v = args.offset + max(amp_values)
    elif args.trace_output_mode == "unipolar-sine":
        peak_command_v = args.offset + max(amp_values)
    else:
        peak_command_v = args.offset + max(amp_values) / 2.0

    if peak_command_v > args.max_command_v + 1e-12:
        raise SystemExit(
            f"Peak generator output {peak_command_v:.6g} V exceeds "
            f"--max-command-v={args.max_command_v:.6g} V."
        )

    if not args.dry_run and not args.arm_output:
        raise SystemExit(
            "Live output is disarmed. Re-run with --arm-output after verifying "
            "the diode/10 mF/MP1584 wiring and CH4 VCAP limit."
        )

    if args.trace_output_mode in ("dc", "unipolar-sine") and args.offset < 0.0:
        print(
            "Warning: a negative --offset can make a positive-only trace go below 0 V.",
            file=sys.stderr,
        )

    step_s = args.charge_s / len(amp_values)

    if step_s < args.min_step_s:
        raise SystemExit(
            f"Requested update interval is {step_s:.6g} s, which is below --min-step-s={args.min_step_s:g}. "
            "Use fewer --steps or a longer --charge-s. SCPI amplitude updates are not fast enough for RF-rate updates."
        )

    log_handle = None
    log_writer = None

    if args.log_csv:
        log_handle = open(args.log_csv, "w", newline="", encoding="utf-8")
        log_writer = csv.writer(log_handle)
        log_writer.writerow(
            [
                "unix_time_s",
                "monotonic_elapsed_s",
                "cycle",
                "step",
                "trace_command",
                "output_enabled",
                "step_s",
                "lateness_s",
            ]
        )
        # Keep the evidence file useful even if a long hardware run is
        # interrupted before the normal close at the end of the experiment.
        log_handle.flush()

    with closing(connect(args)) as inst:
        print(inst.query("*IDN?"))
        configure_trace_wave(inst, args, max(amp_values[0], args.off_below_vpp))

        if args.precharge_s > 0.0:
            precharge_command = max(amp_values)
            print(
                f"precharge: command={precharge_command:g} V "
                f"duration={args.precharge_s:g} s",
                flush=True,
            )
            set_trace_level(inst, args, precharge_command)
            set_output(inst, args.channel, True, args.load)
            if log_writer:
                log_writer.writerow(
                    [time.time(), 0.0, 0, -1, precharge_command, 1, args.precharge_s, 0.0]
                )
                log_handle.flush()
            time.sleep(args.precharge_s)
            set_output(inst, args.channel, False, args.load)

        cycle = 0

        try:
            while args.cycles == 0 or cycle < args.cycles:
                cycle += 1
                print(f"cycle={cycle} trace output start: steps={len(amp_values)} step_s={step_s:.6g}", flush=True)

                output_enabled = False
                cycle_start = time.monotonic()
                max_lateness_s = 0.0

                for step, amp in enumerate(amp_values):
                    # Schedule every update from one absolute cycle start.  This
                    # includes SCPI transfer time in --charge-s instead of adding
                    # a full step sleep after every (potentially slow) command.
                    target_time = cycle_start + step * step_s
                    remaining_s = target_time - time.monotonic()
                    if remaining_s > 0:
                        time.sleep(remaining_s)

                    lateness_s = max(0.0, time.monotonic() - target_time)
                    max_lateness_s = max(max_lateness_s, lateness_s)

                    if amp <= args.off_below_vpp:
                        if output_enabled:
                            set_output(inst, args.channel, False, args.load)
                            output_enabled = False

                        if log_writer:
                            log_writer.writerow(
                                [time.time(), time.monotonic() - cycle_start, cycle, step, amp, 0, step_s, lateness_s]
                            )
                            log_handle.flush()
                    else:
                        set_trace_level(inst, args, amp)

                        if not output_enabled:
                            set_output(inst, args.channel, True, args.load)
                            output_enabled = True

                        if log_writer:
                            log_writer.writerow(
                                [time.time(), time.monotonic() - cycle_start, cycle, step, amp, 1, step_s, lateness_s]
                            )
                            log_handle.flush()

                # Hold the final value until the requested trace duration has
                # elapsed. If command traffic fell behind, do not add more delay.
                remaining_s = cycle_start + args.charge_s - time.monotonic()
                if remaining_s > 0:
                    time.sleep(remaining_s)

                print(f"cycle={cycle} trace output end: output=off rest_s={args.rest_s:g}", flush=True)
                set_output(inst, args.channel, False, args.load)
                if log_writer:
                    log_writer.writerow(
                        [time.time(), time.monotonic() - cycle_start, cycle,
                         len(amp_values), 0.0, 0, args.rest_s, 0.0]
                    )
                    log_handle.flush()

                if max_lateness_s >= step_s:
                    print(
                        f"Warning: cycle={cycle} missed an update deadline by up to "
                        f"{max_lateness_s:.3f} s; use fewer --steps if this persists.",
                        file=sys.stderr,
                        flush=True,
                    )

                if args.rest_s > 0:
                    rest_deadline = cycle_start + args.charge_s + args.rest_s
                    remaining_s = rest_deadline - time.monotonic()
                    if remaining_s > 0:
                        time.sleep(remaining_s)

        except KeyboardInterrupt:
            print("\nInterrupted; turning output off.", file=sys.stderr)
            set_output(inst, args.channel, False, args.load)
            raise

        finally:
            set_output(inst, args.channel, False, args.load)

            if log_handle:
                log_handle.close()


def add_connection_args(parser):
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--resource", help="VISA resource string, such as USB0::...::INSTR or TCPIP0::...::INSTR")
    group.add_argument("--ip", help="LAN socket IP address for instruments with open socket support")
    parser.add_argument("--port", type=int, default=5025, help="LAN socket port")
    parser.add_argument("--timeout-s", type=float, default=3.0)
    parser.add_argument("--dry-run", action="store_true", help="Print SCPI commands without opening an instrument")


def add_wave_args(parser):
    parser.add_argument("--channel", type=int, choices=(1, 2), default=1)
    parser.add_argument("--wave", choices=("SINE", "SQUARE", "PULSE"), default="SINE")
    parser.add_argument("--freq", type=parse_float, default=13.56e6, help="Carrier frequency in Hz, e.g. 1e6 or 13.56M")
    parser.add_argument("--amp-vpp", type=parse_float, default=1.0, help="Amplitude in Vpp")
    parser.add_argument("--offset", type=parse_float, default=0.0, help="DC offset in V")
    parser.add_argument("--phase", type=float, default=0.0, help="Phase in degrees")
    parser.add_argument("--load", choices=("HZ", "50"), default="HZ", help="Generator output load setting")
    parser.add_argument("--safe-vpp", type=parse_float, default=5.0, help="Warn if commanded amplitude exceeds this Vpp")


def add_trace_args(parser):
    parser.add_argument("--trace-file", required=True, help="Text file containing one voltage/envelope sample per line")
    parser.add_argument(
        "--trace-output-mode",
        choices=("dc", "unipolar-sine", "rf-carrier"),
        default="dc",
        help=(
            "dc: replay trace as positive DC/envelope voltage; "
            "unipolar-sine: sine ranges from offset to offset+trace level; "
            "rf-carrier: bipolar carrier centered at offset using trace level as Vpp"
        ),
    )
    parser.add_argument("--steps", type=int, default=200, help="Downsample trace to this many SCPI amplitude updates")
    parser.add_argument("--step-method", choices=("mean", "max", "median"), default="mean")
    parser.add_argument(
        "--trace-scale-mode",
        choices=("max-vpp", "raw", "multiplier", "normalized"),
        default="max-vpp",
        help=(
            "max-vpp: scale trace so selected percentile maps to --trace-max-vpp; "
            "raw: use trace values directly as Vpp; "
            "multiplier: amp_vpp = trace_value * --trace-multiplier; "
            "normalized: map trace min/max to --trace-min-vpp/--trace-max-vpp"
        ),
    )
    parser.add_argument("--trace-percentile", type=float, default=99.0, help="Percentile used by --trace-scale-mode max-vpp")
    parser.add_argument(
        "--trace-max-vpp",
        type=parse_float,
        default=2.0,
        help="Maximum scaled command. In rf-carrier mode this is Vpp; in dc/unipolar-sine modes this is positive output volts.",
    )
    parser.add_argument(
        "--trace-min-vpp",
        type=parse_float,
        default=0.0,
        help="Minimum scaled command. In rf-carrier mode this is Vpp; in dc/unipolar-sine modes this is positive output volts.",
    )
    parser.add_argument("--trace-multiplier", type=float, default=1.0, help="Multiplier used by --trace-scale-mode multiplier")
    parser.add_argument("--preview-csv", help="Save downsampled/scaled trace preview CSV")


def main():
    parser = argparse.ArgumentParser(description="Program a Siglent SDG1032X/SDG1000X for RF charging tests.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("list", help="List VISA resources")

    on_parser = subparsers.add_parser("on", help="Configure an RF-like waveform and turn output on")
    add_connection_args(on_parser)
    add_wave_args(on_parser)
    on_parser.add_argument("--duration-s", type=parse_float, default=0.0, help="Turn output off after this many seconds; 0 leaves it on")

    off_parser = subparsers.add_parser("off", help="Turn output off")
    add_connection_args(off_parser)
    off_parser.add_argument("--channel", type=int, choices=(1, 2), default=1)
    off_parser.add_argument("--load", choices=("HZ", "50"), default="HZ")

    loop_parser = subparsers.add_parser("loop", help="Loop fixed RF output on/off for capacitor charging")
    add_connection_args(loop_parser)
    add_wave_args(loop_parser)
    loop_parser.add_argument("--charge-s", type=parse_float, default=5.0)
    loop_parser.add_argument("--rest-s", type=parse_float, default=1.0)
    loop_parser.add_argument("--cycles", type=int, default=1, help="Number of cycles; 0 loops until Ctrl-C")
    loop_parser.add_argument("--amp-list", help="Comma-separated Vpp values, e.g. 0.5,1.0,1.5")
    loop_parser.add_argument("--amp-start", type=parse_float)
    loop_parser.add_argument("--amp-stop", type=parse_float)
    loop_parser.add_argument("--amp-step", type=parse_float)

    preview_parser = subparsers.add_parser("preview-trace", help="Analyze and scale a trace file without controlling the generator")
    add_trace_args(preview_parser)

    trace_loop_parser = subparsers.add_parser("trace-loop", help="Use a trace file as a time-varying RF amplitude envelope")
    add_connection_args(trace_loop_parser)
    add_wave_args(trace_loop_parser)
    add_trace_args(trace_loop_parser)
    trace_loop_parser.add_argument("--charge-s", type=parse_float, default=10.0, help="Duration for one full trace playback")
    trace_loop_parser.add_argument("--rest-s", type=parse_float, default=1.0, help="Output-off time between trace playbacks")
    trace_loop_parser.add_argument("--cycles", type=int, default=1, help="Number of trace playbacks; 0 loops until Ctrl-C")
    trace_loop_parser.add_argument(
        "--precharge-s",
        type=parse_float,
        default=0.0,
        help="Before cycle 1, hold the maximum scaled command for this many seconds",
    )
    trace_loop_parser.add_argument(
        "--max-command-v",
        type=parse_float,
        default=DEFAULT_MAX_COMMAND_V,
        help="Hard ceiling for the positive generator output; default 8.0 V",
    )
    trace_loop_parser.add_argument(
        "--arm-output",
        action="store_true",
        help="Required acknowledgement before a real generator output is enabled",
    )
    trace_loop_parser.add_argument(
        "--off-below-vpp",
        type=parse_float,
        default=-1.0,
        help=(
            "Turn output off for trace steps at or below this command value. "
            "Default -1 keeps the output enabled at 0 V for zero-valued trace samples; "
            "use 0 to turn output off during zero-valued samples."
        ),
    )
    trace_loop_parser.add_argument("--min-step-s", type=parse_float, default=0.02, help="Minimum allowed SCPI amplitude update interval")
    trace_loop_parser.add_argument("--log-csv", help="Save a CSV log of commanded amplitude updates")

    args = parser.parse_args()

    if args.command == "list":
        return list_visa_resources()

    if args.command == "on":
        run_on(args)
    elif args.command == "off":
        run_off(args)
    elif args.command == "loop":
        run_loop(args)
    elif args.command == "preview-trace":
        run_preview_trace(args)
    elif args.command == "trace-loop":
        run_trace_loop(args)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
