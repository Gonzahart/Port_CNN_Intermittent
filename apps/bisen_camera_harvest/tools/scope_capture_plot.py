import matplotlib as mpl

mpl.rcParams["agg.path.chunksize"] = 10000

import argparse
import math
import re
import time
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import pyvisa


STATE_DAC_LABEL_PROFILES = {
    "apollo-camera": {
        0: "Sleep / inactive",
        1: "VCAP ADC",
        2: "Camera / pixel sensing",
        3: "CNN compute",
        4: "MRAM write",
        5: "Checkpoint committed",
        6: "Context restore",
        7: "Boot / error",
    },
    "apollo-port": {
        0: "Sleep / inactive",
        1: "VCAP ADC",
        2: "Die temperature",
        3: "Sobel compute",
        4: "MRAM write",
        5: "Checkpoint committed",
        6: "Context restore",
        7: "Boot / error",
    },
    "msp430": {
        0: "Sleep / inactive",
        1: "ADC",
        2: "Sense",
        3: "Compute",
        4: "Write FRAM log",
        5: "FRAM checkpoint",
        6: "Context restore",
        7: "Boot / error",
    },
}

DEFAULT_STATE_DAC_PROFILE = "apollo-camera"

# The installed Apollo camera ladder uses GPIO62/63/61 (firmware bits 0/1/2)
# through 99.3/201/398 kOhm. Its analog weights are therefore 4/2/1, not
# 1/2/4. The scope's normalized seven-level code reverses bits 0 and 2.
STATE_DAC_LADDER_TO_FIRMWARE = {
    "apollo-camera": np.array([0, 4, 2, 6, 1, 5, 3, 7], dtype=np.int16),
    "apollo-port": np.arange(8, dtype=np.int16),
    "msp430": np.arange(8, dtype=np.int16),
}

STATE_DAC_COLORS = {
    0: "#d9d9d9",
    1: "tab:blue",
    2: "tab:green",
    3: "tab:red",
    4: "tab:purple",
    5: "tab:brown",
    6: "tab:pink",
    7: "tab:gray"
}

DEFAULT_MIN_SAMPLES = 100
DEFAULT_ALL_DESCRIPTOR_LEN = 346
SCOPE_IDN_TOKENS = ("SDS", "OSCILLOSCOPE")


def close_resource(resource):
    """Close a VISA object without leaving PyVISA to retry a failed close."""
    try:
        resource.close()
    except Exception:
        # Some NI-VISA failures invalidate the native handle before PyVISA's
        # close bookkeeping runs. Mark the Python object closed so __del__
        # does not issue a second operation against that invalid handle.
        resource.session = None


def open_scope(resource_manager, requested_resource=None):
    """Open a responding Siglent/SDS scope instead of trusting VISA order."""
    if requested_resource is None:
        resources = list(resource_manager.list_resources())
    else:
        resources = [requested_resource]

    if not resources:
        raise RuntimeError("No VISA resources found. Check USB connection and drivers.")

    failures = []

    for resource in resources:
        instrument = None

        try:
            instrument = resource_manager.open_resource(resource)
            instrument.timeout = 5000
            instrument.chunk_size = 50 * 1024 * 1024

            # Clear a stale USBTMC transfer left by an interrupted waveform
            # read. Some VISA backends do not implement clear(), so failure is
            # non-fatal and the ID query remains the actual connectivity test.
            try:
                instrument.clear()
            except Exception:
                pass

            identity = instrument.query("*IDN?").strip()

            if not any(token in identity.upper() for token in SCOPE_IDN_TOKENS):
                failures.append(f"{resource}: not an oscilloscope ({identity})")
                close_resource(instrument)
                continue

            instrument.timeout = 30000
            return instrument, resource, identity
        except Exception as exc:
            failures.append(f"{resource}: {exc}")
            if instrument is not None:
                close_resource(instrument)

    detail = "\n  ".join(failures)
    raise RuntimeError(
        "No responding SDS oscilloscope was found. Tried:\n  " + detail +
        "\nRun with --list, then pass the scope explicitly with --resource."
    )


def parse_float(response):
    text = str(response).strip()
    matches = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)
    if not matches:
        raise ValueError(f"Could not parse float from response: {response!r}")
    return float(matches[-1])


def query_float(scope, command, default=None):
    try:
        response = scope.query(command)
        value = parse_float(response)
        return value
    except Exception as exc:
        if default is None:
            raise
        print(f"Warning: {command} failed, using default {default}. Error: {exc}")
        return default


def extract_ieee_block(raw):
    if isinstance(raw, str):
        raw = raw.encode(errors="ignore")

    hash_index = raw.find(b"#")

    if hash_index < 0:
        text = raw.decode(errors="ignore")
        nums = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)
        if len(nums) > 20:
            return np.array([float(x) for x in nums], dtype=np.float64), "ascii"

        preview = raw[:300]
        raise ValueError(
            "No binary block marker found and ASCII fallback failed. "
            f"First bytes returned by scope were: {preview!r}"
        )

    n_digits = int(chr(raw[hash_index + 1]))
    length_start = hash_index + 2
    length_end = length_start + n_digits
    payload_length = int(raw[length_start:length_end].decode())

    payload_start = length_end
    payload_end = payload_start + payload_length

    payload = raw[payload_start:payload_end]
    return payload, "binary"


def parse_siglent_all_payload(payload):
    """Extract signed acquisition-memory samples from SDS `WF? ALL`."""
    marker = b"WAVEDESC"
    descriptor_start = payload.find(marker)

    if descriptor_start < 0:
        raise ValueError("WF? ALL payload does not contain WAVEDESC")

    def read_u32(offset, default=0):
        start = descriptor_start + offset
        end = start + 4
        if end > len(payload):
            return default
        return int.from_bytes(payload[start:end], byteorder="little", signed=False)

    descriptor_len = read_u32(36, DEFAULT_ALL_DESCRIPTOR_LEN)
    if descriptor_len < 64 or descriptor_len > len(payload):
        descriptor_len = DEFAULT_ALL_DESCRIPTOR_LEN

    data_start = descriptor_start + descriptor_len
    if data_start >= len(payload):
        raise ValueError(
            f"WF? ALL descriptor length {descriptor_len} leaves no waveform data"
        )

    descriptor_points = read_u32(60, 0)
    available_points = len(payload) - data_start
    if descriptor_points <= 0 or descriptor_points > available_points:
        sample_count = available_points
    else:
        sample_count = descriptor_points

    data_end = data_start + sample_count
    adc = np.frombuffer(
        payload[data_start:data_end], dtype=np.int8
    ).astype(np.float64)
    info = {
        "descriptor_start": descriptor_start,
        "descriptor_len": descriptor_len,
        "descriptor_points": int(descriptor_points),
        "payload_bytes": len(payload),
    }
    return adc, info


def query_acquisition_points(scope, channel):
    """Return the number of acquired samples currently stored for a channel."""
    points = int(round(query_float(scope, f"SANU? C{channel}")))

    if points <= 0:
        raise ValueError(
            f"SANU? C{channel} returned a non-positive acquisition size: {points}"
        )

    return points


def make_memory_setup(scope, channel, points, sparsing, debug=False):
    """Build WFSU with an explicit NP value that works around SDS NP,0."""
    if points < 0:
        raise ValueError("--points must be zero (automatic) or a positive integer")
    if sparsing < 0:
        raise ValueError("--sparsing must be zero or a positive integer")

    if points > 0:
        transfer_points = points
        acquisition_points = None
    else:
        acquisition_points = query_acquisition_points(scope, channel)
        sample_interval = max(1, sparsing)
        transfer_points = int(math.ceil(acquisition_points / sample_interval))

    if debug:
        print(f"\nCH{channel} memory transfer plan")
        if acquisition_points is not None:
            print(f"  acquisition points (SANU): {acquisition_points}")
        print(f"  sparsing interval: {sparsing}")
        print(f"  requested transfer points (NP): {transfer_points}")

    return f"WFSU SP,{sparsing},NP,{transfer_points},FP,0"


def capture_channel(
    scope,
    channel,
    points,
    manual_vdiv=None,
    manual_offset=None,
    debug=False,
    waveform_source="auto",
    sparsing=0,
    min_samples=DEFAULT_MIN_SAMPLES
):
    scope.write("CHDR OFF")
    scope.write(f"C{channel}:TRA ON")
    time.sleep(0.2)

    queried_vdiv = query_float(scope, f"C{channel}:VDIV?", default=1.0)
    queried_offset = query_float(scope, f"C{channel}:OFST?", default=0.0)

    vdiv = queried_vdiv if manual_vdiv is None else manual_vdiv
    offset = queried_offset if manual_offset is None else manual_offset

    memory_setup = None
    if waveform_source in ("memory", "auto"):
        memory_setup = make_memory_setup(
            scope=scope,
            channel=channel,
            points=points,
            sparsing=sparsing,
            debug=debug
        )

    if waveform_source == "display":
        attempts = [
            ("DAT2", None),      # displayed waveform, usually easiest
        ]
    elif waveform_source == "memory":
        attempts = [
            ("ALL", memory_setup),
        ]
    else:
        attempts = [
            ("DAT2", None),      # displayed waveform, usually easiest
            ("ALL", memory_setup),
        ]

    previews = []

    for source, setup_cmd in attempts:
        if setup_cmd is not None:
            scope.write(setup_cmd)
            time.sleep(0.2)

        cmd = f"C{channel}:WF? {source}"
        scope.write(cmd)
        raw = scope.read_raw()
        previews.append((source, setup_cmd, raw[:300]))

        try:
            data, mode = extract_ieee_block(raw)
        except Exception as exc:
            if debug:
                print(f"\nCH{channel} {source} failed parse")
                print(f"  setup: {setup_cmd}")
                print(f"  error: {exc}")
                print(f"  raw preview: {raw[:300]!r}")
            continue

        if mode == "ascii":
            volts = np.asarray(data, dtype=np.float64)
            if len(volts) > 0:
                if len(volts) < min_samples:
                    if debug:
                        print(f"\nCH{channel} {source} returned only {len(volts)} ASCII samples; ignoring.")
                        print(f"  setup: {setup_cmd}")
                    continue

                if debug:
                    print(f"\nCH{channel} debug")
                    print(f"  source used: {source}")
                    print(f"  mode: ascii")
                    print(f"  points returned: {len(volts)}")
                    print(f"  volts min/max: {float(np.min(volts))}, {float(np.max(volts))}")
                return volts

        memory_info = {}
        if source == "ALL":
            try:
                adc, memory_info = parse_siglent_all_payload(data)
            except Exception as exc:
                if debug:
                    print(f"\nCH{channel} ALL descriptor parse failed")
                    print(f"  setup: {setup_cmd}")
                    print(f"  error: {exc}")
                    print(f"  payload preview: {data[:300]!r}")
                continue
        else:
            adc = np.frombuffer(data, dtype=np.int8).astype(np.float64)

        if len(adc) == 0:
            if debug:
                print(f"\nCH{channel} {source} returned 0 samples")
                print(f"  setup: {setup_cmd}")
                print(f"  raw preview: {raw[:300]!r}")
            continue

        if len(adc) < min_samples:
            if debug:
                print(f"\nCH{channel} {source} returned only {len(adc)} binary samples; ignoring.")
                print(f"  setup: {setup_cmd}")
                print(f"  raw preview: {raw[:300]!r}")
            continue

        volts = (adc * vdiv / 25.0) - offset

        if debug:
            print(f"\nCH{channel} debug")
            print(f"  source used: {source}")
            print(f"  setup: {setup_cmd}")
            print(f"  mode: binary")
            print(f"  queried VDIV: {queried_vdiv}")
            print(f"  queried OFST: {queried_offset}")
            print(f"  used VDIV: {vdiv}")
            print(f"  used OFST: {offset}")
            print(f"  points returned: {len(volts)}")
            print(f"  raw ADC min/max: {float(np.min(adc))}, {float(np.max(adc))}")
            print(f"  volts min/max: {float(np.min(volts))}, {float(np.max(volts))}")
            for key, value in memory_info.items():
                print(f"  {key}: {value}")

        return volts

    print(f"\nCH{channel} all waveform attempts failed.")
    for source, setup_cmd, preview in previews:
        print(f"  source={source}, setup={setup_cmd}, preview={preview!r}")

    raise RuntimeError(f"CH{channel} returned no usable waveform data.")


def get_time_axis(scope, n_points, origin="capture"):
    tdiv = query_float(scope, "TDIV?", default=1.0)

    total_time = 14.0 * tdiv
    dt = total_time / float(n_points)

    if origin == "trigger":
        return (np.arange(n_points) - (n_points / 2.0)) * dt

    return np.arange(n_points) * dt


def estimate_energy(df, cap_f, vcap_col, marker_col, threshold):
    marker = df[marker_col].to_numpy()
    return estimate_energy_from_mask(
        df=df,
        cap_f=cap_f,
        vcap_col=vcap_col,
        active=marker > threshold
    )


def estimate_energy_from_mask(df, cap_f, vcap_col, active):
    vcap = df[vcap_col].to_numpy()
    time_values = df["time_s"].to_numpy()

    active = np.asarray(active, dtype=bool)
    edges = np.diff(active.astype(np.int8))

    starts = np.where(edges == 1)[0] + 1
    ends = np.where(edges == -1)[0] + 1

    if len(active) and active[0]:
        starts = np.insert(starts, 0, 0)

    if len(active) and active[-1]:
        ends = np.append(ends, len(active) - 1)

    rows = []

    for i, start in enumerate(starts):
        possible_ends = ends[ends > start]

        if len(possible_ends) == 0:
            continue

        end = possible_ends[0]

        v_start = float(vcap[start])
        v_end = float(vcap[end])
        t_start = float(time_values[start])
        t_end = float(time_values[end])

        energy_j = 0.5 * cap_f * ((v_start * v_start) - (v_end * v_end))

        rows.append({
            "event": i,
            "start_s": t_start,
            "end_s": t_end,
            "duration_s": t_end - t_start,
            "v_start": v_start,
            "v_end": v_end,
            "energy_j": energy_j,
            "energy_mj": energy_j * 1000.0
        })

    return pd.DataFrame(rows)


def suppress_short_state_runs(code, min_run_samples=2):
    """Remove sub-sample GPIO/DAC transition codes without hiding real runs."""
    filtered = np.asarray(code, dtype=np.int16).copy()
    if min_run_samples <= 1 or len(filtered) == 0:
        return filtered

    while True:
        starts = np.r_[0, np.where(np.diff(filtered) != 0)[0] + 1]
        ends = np.r_[starts[1:], len(filtered)]
        changed = False

        for start, end in zip(starts, ends):
            if end - start >= min_run_samples:
                continue

            if start > 0:
                replacement = filtered[start - 1]
            elif end < len(filtered):
                replacement = filtered[end]
            else:
                continue

            filtered[start:end] = replacement
            changed = True

        if not changed:
            return filtered


def firmware_codes_from_ladder(ladder_codes, state_profile):
    """Convert normalized ladder levels to the firmware's GPIO bit codes."""
    return STATE_DAC_LADDER_TO_FIRMWARE[state_profile][
        np.asarray(ladder_codes, dtype=np.intp)
    ]


def add_state_dac_decode(
    df,
    state_col,
    vcc_col=None,
    vcc_volts=None,
    min_vcc=0.5,
    min_run_samples=2,
    state_profile=DEFAULT_STATE_DAC_PROFILE,
    min_run_us=None,
):
    state_v = df[state_col].to_numpy()
    if vcc_volts is not None:
        vcc = np.full_like(state_v, float(vcc_volts), dtype=np.float64)
    elif vcc_col is not None:
        vcc = df[vcc_col].to_numpy()
    else:
        raise ValueError("State-DAC decode requires a VCC column or fixed VCC")
    valid = vcc > min_vcc

    ratio = np.zeros_like(state_v, dtype=np.float64)
    ratio[valid] = state_v[valid] / vcc[valid]

    ladder_code = np.rint(ratio * 7.0).astype(np.int16)
    ladder_code = np.clip(ladder_code, 0, 7)
    ladder_code[~valid] = 0
    code = firmware_codes_from_ladder(ladder_code, state_profile)

    df["state_dac_ratio"] = ratio
    df["state_code_ladder"] = ladder_code
    df["state_code_raw"] = code
    # At the installed camera ladder, a GPIO edge can overshoot into an
    # adjacent analog band for tens of microseconds. Do not report that edge
    # transient as a checkpoint notification. Keep the raw decode for audit.
    if min_run_us is None:
        min_run_us = 100.0 if state_profile == "apollo-camera" else 0.0
    if min_run_us > 0 and len(df) > 1:
        sample_period_s = float(np.median(np.diff(df["time_s"].to_numpy())))
        if sample_period_s > 0:
            min_run_samples = max(
                min_run_samples,
                int(math.ceil(min_run_us * 1e-6 / sample_period_s)),
            )
    df["state_code"] = suppress_short_state_runs(
        code,
        min_run_samples=min_run_samples
    )


def state_events_from_codes(
    df,
    code_col="state_code",
    vcap_col=None,
    state_v_col=None,
    state_labels=None,
):
    if state_labels is None:
        state_labels = STATE_DAC_LABEL_PROFILES[DEFAULT_STATE_DAC_PROFILE]
    code = df[code_col].to_numpy(dtype=np.int16)
    time_values = df["time_s"].to_numpy()

    if len(code) == 0:
        return pd.DataFrame()

    change = np.diff(code) != 0
    starts = np.r_[0, np.where(change)[0] + 1]
    ends = np.r_[starts[1:], len(code)]
    sample_period = (
        float(np.median(np.diff(time_values)))
        if len(time_values) > 1
        else 0.0
    )

    rows = []

    for event, (start, end) in enumerate(zip(starts, ends)):
        if end <= start:
            continue

        event_code = int(code[start])
        end_s = (
            float(time_values[end])
            if end < len(time_values)
            else float(time_values[-1] + sample_period)
        )
        row = {
            "event": event,
            "start_s": float(time_values[start]),
            "end_s": end_s,
            "duration_s": float(end_s - time_values[start]),
            "state_code": event_code,
            "state": state_labels.get(event_code, "Unknown")
        }

        if vcap_col is not None and vcap_col in df.columns:
            vcap = df[vcap_col].to_numpy()
            row["v_start"] = float(vcap[start])
            row["v_end"] = float(vcap[end - 1])
            row["v_mean"] = float(np.mean(vcap[start:end]))

        if state_v_col is not None and state_v_col in df.columns:
            state_v = df[state_v_col].to_numpy()
            row["state_v_mean"] = float(np.mean(state_v[start:end]))

        rows.append(row)

    return pd.DataFrame(rows)


def downsample_for_plot(df, max_points=20000, preserve_cols=None):
    if len(df) <= max_points:
        return df

    step = int(np.ceil(len(df) / max_points))
    keep = set(range(0, len(df), step))

    for col in preserve_cols or []:
        if col not in df.columns:
            continue

        values = df[col].to_numpy()
        changes = np.where(np.diff(values) != 0)[0]

        for idx in changes:
            keep.add(max(0, idx - 1))
            keep.add(idx)
            keep.add(min(len(df) - 1, idx + 1))
            keep.add(min(len(df) - 1, idx + 2))

    return df.iloc[sorted(keep)].copy()


def plot_capture(
    df,
    channels,
    output_png,
    vcap_ylim=None,
    gpio_ylim=None,
    max_plot_points=20000,
    state_dac_ch=None,
    state_labels=None,
    state_profile=DEFAULT_STATE_DAC_PROFILE,
):
    if state_labels is None:
        state_labels = STATE_DAC_LABEL_PROFILES[DEFAULT_STATE_DAC_PROFILE]
    plot_df = downsample_for_plot(
        df,
        max_points=max_plot_points,
        preserve_cols=None,
    )

    # Plot the analog traces with a bounded number of points, but represent the
    # decoded state as one row per transition. Keeping four neighbouring analog
    # samples around every state transition can turn a nominal 50k-point plot
    # back into hundreds of thousands of points on a long, noisy capture.
    state_plot_df = plot_df
    if state_dac_ch is not None and "state_code" in df.columns and len(df) > 0:
        state_code_full = df["state_code"].to_numpy()
        transition_rows = np.r_[
            0,
            np.where(np.diff(state_code_full) != 0)[0] + 1,
            len(df) - 1,
        ]
        state_plot_df = df.iloc[np.unique(transition_rows)]

    print(f"Plotting {len(plot_df)} points out of {len(df)} total samples")

    channel_colors = {
        1: "tab:blue",
        2: "tab:orange",
        3: "tab:green",
        4: "tab:red"
    }

    vcap_channel = channels[0]

    if state_dac_ch is not None:
        secondary_channels = [
            ch for ch in channels[1:] if ch != vcap_channel
        ]
        n_axes = 3 if secondary_channels else 2
        height_ratios = [2, 2, 1.3] if secondary_channels else [2, 1.3]

        fig, axes = plt.subplots(
            n_axes,
            1,
            figsize=(12, 9 if secondary_channels else 6.5),
            sharex=True,
            gridspec_kw={"height_ratios": height_ratios}
        )

        ax_v = axes[0]
        ax_state = axes[-1]

        voltage_label = (
            f"CH{vcap_channel} state DAC"
            if vcap_channel == state_dac_ch
            else f"CH{vcap_channel} Vcap / VCC"
        )
        ax_v.plot(
            plot_df["time_s"],
            plot_df[f"ch{vcap_channel}_v"],
            label=voltage_label,
            linewidth=1.2,
            color=channel_colors.get(vcap_channel, "tab:blue")
        )

        ax_v.set_ylabel("Voltage (V)")
        ax_v.grid(True)
        ax_v.legend(loc="upper right")

        if vcap_ylim is not None:
            ax_v.set_ylim(vcap_ylim[0], vcap_ylim[1])

        if secondary_channels:
            ax_a = axes[1]

            for ch in secondary_channels:
                channel_label = (
                    f"CH{ch} state DAC"
                    if ch == state_dac_ch
                    else f"CH{ch}"
                )
                ax_a.plot(
                    plot_df["time_s"],
                    plot_df[f"ch{ch}_v"],
                    label=channel_label,
                    linewidth=1.0,
                    color=channel_colors.get(ch, None)
                )

            ax_a.set_ylabel("Voltage (V)")
            ax_a.grid(True)
            ax_a.legend(loc="upper right")

        if "state_code" in state_plot_df.columns:
            state_code = state_plot_df["state_code"].to_numpy()
            state_time = state_plot_df["time_s"]
        else:
            state_v = plot_df[f"ch{state_dac_ch}_v"].to_numpy()
            vcc = plot_df[f"ch{vcap_channel}_v"].to_numpy()
            ratio = np.divide(
                state_v,
                vcc,
                out=np.zeros_like(state_v, dtype=np.float64),
                where=vcc > 0.5
            )
            ladder_code = np.clip(np.rint(ratio * 7.0), 0, 7)
            state_code = firmware_codes_from_ladder(ladder_code, state_profile)
            state_time = plot_df["time_s"]

        ax_state.step(
            state_time,
            state_code,
            where="post",
            linewidth=1.2,
            color="black",
            label=f"CH{state_dac_ch} decoded state DAC"
        )

        for code, color in STATE_DAC_COLORS.items():
            ax_state.axhspan(code - 0.45, code + 0.45, color=color, alpha=0.08)

        ax_state.set_xlabel("Time (s)")
        ax_state.set_ylabel("Decoded state")
        ax_state.set_yticks(list(state_labels.keys()))
        ax_state.set_yticklabels([
            f"{code}: {state_labels[code]}" for code in state_labels
        ])
        ax_state.set_ylim(-0.5, 7.5)
        ax_state.grid(True)
        ax_state.legend(loc="upper right")

        fig.tight_layout()
        fig.savefig(output_png, dpi=200)
        plt.close(fig)
        return

    fig, axes = plt.subplots(
        2,
        1,
        figsize=(12, 7),
        sharex=True,
        gridspec_kw={"height_ratios": [2, 1]}
    )

    ax_v = axes[0]
    ax_d = axes[1]

    ax_v.plot(
        plot_df["time_s"],
        plot_df[f"ch{vcap_channel}_v"],
        label=f"CH{vcap_channel} Vcap / VCC",
        linewidth=1.2,
        color=channel_colors.get(vcap_channel, "tab:blue")
    )

    ax_v.set_ylabel("Voltage (V)")
    ax_v.grid(True)
    ax_v.legend(loc="upper right")

    if vcap_ylim is not None:
        ax_v.set_ylim(vcap_ylim[0], vcap_ylim[1])

    digital_channels = channels[1:]
    digital_threshold = 1.5

    yticks = []
    ylabels = []

    for i, ch in enumerate(digital_channels):
        raw = plot_df[f"ch{ch}_v"].to_numpy()
        digital = (raw > digital_threshold).astype(float)

        offset = float(i)

        ax_d.step(
            plot_df["time_s"],
            digital + offset,
            where="post",
            linewidth=1.2,
            label=f"CH{ch}",
            color=channel_colors.get(ch, None)
        )

        yticks.append(offset + 0.5)

        if ch == 2:
            ylabels.append("CH2 state bit 2 / P2.5")
        elif ch == 3:
            ylabels.append("CH3 state bit 0 / P3.5")
        elif ch == 4:
            ylabels.append("CH4 state bit 1 / P3.6")
        else:
            ylabels.append(f"CH{ch}")

    ax_d.set_xlabel("Time (s)")
    ax_d.set_ylabel("Digital GPIO")
    ax_d.set_yticks(yticks)
    ax_d.set_yticklabels(ylabels)
    ax_d.grid(True)
    ax_d.legend(loc="upper right")

    if len(digital_channels) > 0:
        ax_d.set_ylim(-0.25, len(digital_channels) + 0.25)

    fig.tight_layout()
    fig.savefig(output_png, dpi=200)
    plt.close(fig)


def parse_channel_overrides(text):
    if text is None:
        return {}

    result = {}

    for item in text.split(","):
        item = item.strip()

        if not item:
            continue

        ch_text, value_text = item.split(":")
        result[int(ch_text)] = float(value_text)

    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--resource", default=None)
    parser.add_argument("--channels", nargs="+", type=int, default=[1, 2])
    parser.add_argument(
        "--points",
        type=int,
        default=14000,
        help=(
            "Maximum waveform points to transfer; 0 queries SANU and requests "
            "the full acquisition after sparsing."
        )
    )
    parser.add_argument("--out", default="scope_capture")
    parser.add_argument(
        "--input-csv",
        type=Path,
        help="Re-decode a saved raw capture without connecting to the scope; --out must differ",
    )
    parser.add_argument("--cap-f", type=float, default=0.1)
    parser.add_argument("--marker-ch", type=int, default=2)
    parser.add_argument("--marker-threshold", type=float, default=1.0)
    parser.add_argument("--state-dac-ch", type=int, default=None)
    parser.add_argument(
        "--state-profile",
        choices=sorted(STATE_DAC_LABEL_PROFILES),
        default=DEFAULT_STATE_DAC_PROFILE,
        help=(
            "State-DAC label and resistor-wiring map. Use apollo-camera for "
            "the installed GPIO62/63/61 99.3/201/398 kOhm ladder, "
            "apollo-port for the Sobel bring-up app, or msp430 for the "
            "original experiment."
        ),
    )
    parser.add_argument("--state-vcc-ch", type=int, default=None)
    parser.add_argument(
        "--state-vcc-volts",
        type=float,
        default=None,
        help="Use a fixed measured MCU-rail voltage instead of a scope channel."
    )
    parser.add_argument("--state-min-vcc", type=float, default=0.5)
    parser.add_argument(
        "--state-min-run-samples",
        type=int,
        default=2,
        help=(
            "Suppress decoded state runs shorter than this many samples; "
            "the unfiltered result remains in state_code_raw."
        )
    )
    parser.add_argument(
        "--state-min-run-us",
        type=float,
        default=None,
        help="Minimum decoded state duration in microseconds; default 100 for apollo-camera, 0 otherwise",
    )
    parser.add_argument("--no-energy", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--no-stop", action="store_true")
    parser.add_argument("--run-after", action="store_true")
    parser.add_argument(
        "--waveform-source",
        choices=["auto", "display", "memory"],
        default="auto",
        help="auto tries DAT2 first; memory requires SDS acquisition-memory WF? ALL."
    )
    parser.add_argument(
        "--sparsing",
        type=int,
        default=0,
        help="SDS acquisition-memory sparse interval; 0 returns unsparsed samples."
    )
    parser.add_argument(
        "--time-origin",
        choices=["capture", "trigger"],
        default="capture",
        help="capture starts plots/CSVs at 0 s; trigger keeps the scope-style negative-to-positive axis."
    )
    parser.add_argument("--manual-vdiv", default=None, help="Example: 1:1.0,2:1.0,3:1.0")
    parser.add_argument("--manual-offset", default=None, help="Example: 1:0.0,2:0.0,3:0.0")
    parser.add_argument("--vcap-ylim", nargs=2, type=float, default=None)
    parser.add_argument("--gpio-ylim", nargs=2, type=float, default=None)
    parser.add_argument("--max-plot-points", type=int, default=20000)
    parser.add_argument(
        "--min-samples",
        type=int,
        default=DEFAULT_MIN_SAMPLES,
        help="Reject waveform transfers shorter than this many samples."
    )
    args = parser.parse_args()
    if args.state_min_run_us is not None and args.state_min_run_us < 0:
        parser.error("--state-min-run-us must be nonnegative")

    manual_vdivs = parse_channel_overrides(args.manual_vdiv)
    manual_offsets = parse_channel_overrides(args.manual_offset)

    scope = None
    if args.input_csv is not None:
        if args.list or args.run_after:
            parser.error("--input-csv cannot be combined with --list or --run-after")
        if Path(args.out + ".csv").resolve() == args.input_csv.resolve():
            parser.error("--out must not overwrite the input capture")
        df = pd.read_csv(args.input_csv)
        required = {"time_s"} | {f"ch{ch}_v" for ch in args.channels}
        missing = sorted(required - set(df.columns))
        if missing:
            parser.error(f"input capture is missing: {', '.join(missing)}")
        print(f"Re-decoding saved capture: {args.input_csv}")
    else:
        rm = pyvisa.ResourceManager()

        if args.list:
            print("Available VISA resources:")
            for resource in rm.list_resources():
                print(resource)
            return

        scope, resource, identity = open_scope(
            resource_manager=rm,
            requested_resource=args.resource
        )
        print(f"Connected to {resource}: {identity}")

        if not args.no_stop:
            print("Stopping scope acquisition before transfer...")
            scope.write("STOP")
            time.sleep(0.3)

        data = {}

        for ch in args.channels:
            print(f"Capturing CH{ch}")

            data[f"ch{ch}_v"] = capture_channel(
                scope=scope,
                channel=ch,
                points=args.points,
                manual_vdiv=manual_vdivs.get(ch),
                manual_offset=manual_offsets.get(ch),
                debug=args.debug,
                waveform_source=args.waveform_source,
                sparsing=args.sparsing,
                min_samples=args.min_samples
            )

        min_len = min(len(values) for values in data.values())

        if min_len == 0:
            raise RuntimeError(
                "All captured channels returned 0 samples. "
                "Make sure the scope has a captured waveform on screen, channels are ON, "
                "and try using DAT2/DAT1 waveform source fallback."
            )

        if min_len < args.min_samples:
            raise RuntimeError(
                f"Scope transfer returned only {min_len} samples. "
                "That is too short to be a valid capture; try --waveform-source auto "
                "or --waveform-source display, check the scope memory/depth setting, "
                "and rerun with --debug."
            )

        for key in data:
            data[key] = data[key][:min_len]

        time_axis = get_time_axis(scope, min_len, origin=args.time_origin)

        df = pd.DataFrame({"time_s": time_axis})

        for key, values in data.items():
            df[key] = values

    state_events_path = Path(args.out + "_state_events.csv")

    if args.state_dac_ch is not None:
        state_col = f"ch{args.state_dac_ch}_v"

        if state_col not in df.columns:
            raise RuntimeError(f"--state-dac-ch CH{args.state_dac_ch} was not captured.")

        state_vcc_col = None
        if args.state_vcc_volts is None:
            state_vcc_ch = (
                args.state_vcc_ch
                if args.state_vcc_ch is not None
                else args.channels[0]
            )
            state_vcc_col = f"ch{state_vcc_ch}_v"
            if state_vcc_col not in df.columns:
                raise RuntimeError(
                    f"--state-vcc-ch CH{state_vcc_ch} was not captured."
                )

        add_state_dac_decode(
            df=df,
            state_col=state_col,
            vcc_col=state_vcc_col,
            vcc_volts=args.state_vcc_volts,
            min_vcc=args.state_min_vcc,
            min_run_samples=args.state_min_run_samples,
            state_profile=args.state_profile,
            min_run_us=args.state_min_run_us,
        )

    csv_path = Path(args.out + ".csv")
    png_path = Path(args.out + ".png")
    energy_path = Path(args.out + "_energy.csv")

    df.to_csv(csv_path, index=False)
    print("Saved CSV:", csv_path)

    plot_capture(
        df=df,
        channels=args.channels,
        output_png=png_path,
        vcap_ylim=args.vcap_ylim,
        gpio_ylim=args.gpio_ylim,
        max_plot_points=args.max_plot_points,
        state_dac_ch=args.state_dac_ch,
        state_labels=STATE_DAC_LABEL_PROFILES[args.state_profile],
        state_profile=args.state_profile,
    )

    print("Saved plot:", png_path)

    marker_col = f"ch{args.marker_ch}_v"
    vcap_col = f"ch{args.channels[0]}_v"

    if args.state_dac_ch is not None:
        state_events_df = state_events_from_codes(
            df=df,
            code_col="state_code",
            vcap_col=vcap_col,
            state_v_col=f"ch{args.state_dac_ch}_v",
            state_labels=STATE_DAC_LABEL_PROFILES[args.state_profile],
        )

        state_events_df.to_csv(state_events_path, index=False)
        print("Saved decoded state events:", state_events_path)

    if args.no_energy:
        print("Skipped energy summary because --no-energy was set.")
    elif args.state_dac_ch is not None:
        energy_df = estimate_energy_from_mask(
            df=df,
            cap_f=args.cap_f,
            vcap_col=vcap_col,
            active=df["state_code"].to_numpy() > 0
        )

        energy_df.to_csv(energy_path, index=False)
        print("Saved energy summary from decoded state DAC:", energy_path)

        if len(energy_df) > 0:
            print(energy_df)
        else:
            print("No nonzero state-DAC activity detected. Check ladder wiring and --state-dac-ch.")
    elif marker_col in df.columns:
        energy_df = estimate_energy(
            df=df,
            cap_f=args.cap_f,
            vcap_col=vcap_col,
            marker_col=marker_col,
            threshold=args.marker_threshold
        )

        energy_df.to_csv(energy_path, index=False)
        print("Saved energy summary:", energy_path)

        if len(energy_df) > 0:
            print(energy_df)
        else:
            print("No marker pulses detected. Try lowering --marker-threshold or check CH marker.")

    if args.run_after and scope is not None:
        scope.write("RUN")

    if scope is not None:
        close_resource(scope)


if __name__ == "__main__":
    main()
