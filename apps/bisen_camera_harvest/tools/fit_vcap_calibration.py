#!/usr/bin/env python3
"""Fit the VCAP divider calibration (GPIO17/ADCSE2 or GPIO16/ADCSE3) from a
calibration-mode sweep.

Inputs
  --swo  : text saved from the BTN1 readout of the calibration image. Lines like
           "BISen camera HARVEST retained point #3: samples=32 valid=32
            code_mean=544 code_min=505 code_max=576 ..."
  --dmm  : CSV you fill in at the bench, one row per BTN0 press:
           record,vcap_v[,pin_v][,note]
           (record = the retained point number; pin_v optional, the supply
           pad voltage measured to EVB ground. gpio17_v / gpio16_v are
           accepted as column names for pin_v.)
  The supply pad is read from the SWO banner ("supply_input=GPIO17" etc.);
  --pin overrides it. The pad is printed as BISEN_HARVEST_CAL_PIN, which full
  builds require to match BISEN_HARVEST_SUPPLY_PIN.

Output: per-point table, least-squares line code = a + b*VCAP, residuals,
pin-referred ADC gain/offset (if pin_v given), and the five build flags.
The flags use two integer codes on the fitted line, so there is no rounding
error in the anchors. Pure Python; no numpy needed.
"""
import argparse, csv, re, sys

PAT = re.compile(r"retained point #(\d+): samples=(\d+) valid=(\d+) "
                 r"code_mean=(\d+) code_min=(\d+) code_max=(\d+)")
PINPAT = re.compile(r"(?:supply_input=GPIO|physical GPIO|calibration input: (?:J9\.8/)?GPIO)(1[67])")
DIAG = re.compile(r"ADC diagnostic #(\d+): mode=(\w+) read_failures=(\d+) "
                  r"empty_reads=(\d+) wrong_slots=(\d+) drain_failures=(\d+)")

def linfit(x, y):
    n = len(x); mx = sum(x) / n; my = sum(y) / n
    sxx = sum((xi - mx) ** 2 for xi in x)
    b = sum((xi - mx) * (yi - my) for xi, yi in zip(x, y)) / sxx
    return my - b * mx, b

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--swo", required=True)
    ap.add_argument("--dmm", required=True)
    ap.add_argument("--low-v", type=float, default=5.6, help="low anchor near this VCAP")
    ap.add_argument("--high-v", type=float, default=7.7, help="high anchor near this VCAP")
    ap.add_argument("--ref-v", type=float, default=1.19, help="nominal ADC reference")
    ap.add_argument("--pin", type=int, choices=(16, 17),
                    help="supply pad the sweep was taken on (default: read from SWO)")
    a = ap.parse_args()

    pts, diag, pins = {}, {}, set()
    for line in open(a.swo, errors="replace"):
        pm = PINPAT.search(line)
        if pm:
            pins.add(int(pm.group(1)))
        m = PAT.search(line)
        if m:  # the log is printed repeatedly; identical repeats are harmless
            r = int(m.group(1))
            pts[r] = dict(valid=int(m.group(3)), mean=int(m.group(4)),
                          lo=int(m.group(5)), hi=int(m.group(6)))
        d = DIAG.search(line)
        if d:
            diag[int(d.group(1))] = (d.group(2), list(map(int, d.group(3, 4, 5, 6))))

    rows = []
    with open(a.dmm, newline="") as f:
        for row in csv.DictReader(f):
            r = int(row["record"])
            if r not in pts:
                sys.exit(f"record {r} is in the DMM sheet but not in the SWO log")
            if pts[r]["valid"] == 0:
                print(f"record {r}: no valid samples, skipped"); continue
            g = next((row[k] for k in ("pin_v", "gpio17_v", "gpio16_v")
                      if row.get(k, "").strip()), "").strip()
            rows.append(dict(rec=r, v=float(row["vcap_v"]),
                             pin=float(g) if g else None,
                             note=row.get("note", ""), **pts[r]))
    if len(pins) > 1:
        sys.exit(f"SWO log mixes supply pads {sorted(pins)}; split it per pad")
    pad = a.pin or (pins.pop() if pins else None)
    if a.pin and pins and a.pin not in pins:
        sys.exit(f"--pin {a.pin} contradicts the SWO log (GPIO{sorted(pins)[0]})")
    if len(rows) < 3:
        sys.exit("need at least 3 points (use 6+ spread over the operating range)")

    x = [p["v"] for p in rows]; y = [p["mean"] for p in rows]
    if max(x) - min(x) < 1.0:
        sys.exit(f"sweep spans only {max(x) - min(x):.2f} V; spread the points"
                 " over at least 1 V (ideally the whole 5.6-7.7 V window)")
    A, B = linfit(x, y)
    print(f"Fit: code = {A:.2f} + {B:.3f} * VCAP[V]   ({len(rows)} points)")
    print(f"     => 1 code = {1000 / B:.1f} mV of VCAP\n")
    print(f"Supply pad: {'GPIO%d' % pad if pad else 'UNKNOWN (pass --pin)'}\n")
    print(" rec  VCAP_V     PIN_V  mean  min-max  spread  resid  mode")
    worst = 0.0
    for p in rows:
        res = p["mean"] - (A + B * p["v"]); worst = max(worst, abs(res))
        mode = diag.get(p["rec"], ("", []))[0]
        pin = f"{p['pin']:.4f}" if p["pin"] is not None else "   -  "
        print(f"{p['rec']:4d}  {p['v']:6.3f}  {pin:>8}  {p['mean']:4d}  {p['lo']:4d}-{p['hi']:<4d}"
              f"  {p['hi'] - p['lo']:5d}  {res:+5.1f}  {mode}")
    print(f"\nWorst residual {worst:.1f} codes = {worst * 1000 / B:.0f} mV of VCAP.")
    spread = max(p["hi"] - p["lo"] for p in rows)
    print(f"Largest within-point min-max spread {spread} codes = {spread * 1000 / B:.0f} mV of VCAP."
          "\n  (Policy decisions use one median-of-3 reading, so this spread, not the mean,"
          "\n   sets how sharply thresholds switch.)")

    bad = {r: e for r, (m, e) in diag.items() if any(e)}
    if bad:
        print(f"\nWARNING: ADC FIFO/slot error counters non-zero for records {sorted(bad)}")

    pin_rows = [p for p in rows if p["pin"] is not None]
    if len(pin_rows) >= 3:
        pa, pb = linfit([p["pin"] for p in pin_rows], [p["mean"] for p in pin_rows])
        print(f"\nPin-referred ADC: code = {pa:.1f} + {pb:.1f} * V_pin")
        print(f"  effective full scale = {4096 / pb:.4f} V (nominal {a.ref_v} V), "
              f"gain error {100 * (pb / (4096 / a.ref_v) - 1):+.1f} %, offset {pa:+.1f} codes")
        div = [p["pin"] / p["v"] for p in pin_rows]
        print(f"  measured divider ratio {sum(div) / len(div):.5f} (nominal 10k/400k = 0.02500;"
              f" VCAP/pin = {len(div) / sum(div):.2f})")

    lo_code = round(A + B * a.low_v); hi_code = round(A + B * a.high_v)
    lo_uv = round((lo_code - A) / B * 1e6); hi_uv = round((hi_code - A) / B * 1e6)
    if not (min(x) - 0.05 <= a.low_v and a.high_v <= max(x) + 0.05):
        print(f"\nWARNING: anchors {a.low_v}/{a.high_v} V extrapolate beyond the measured "
              f"{min(x):.2f}-{max(x):.2f} V; extend the sweep.")
    print("\nBuild flags (anchors lie exactly on the fitted line):")
    if pad:
        print(f"  BISEN_HARVEST_SUPPLY_PIN={pad} BISEN_HARVEST_CAL_PIN={pad} \\")
    else:
        print("  WARNING: supply pad unknown; rerun with --pin 16 or 17 to get"
              " BISEN_HARVEST_CAL_PIN")
    print(f"  BISEN_HARVEST_CAL_LOW_CODE={lo_code} BISEN_HARVEST_CAL_LOW_UV={lo_uv} \\")
    print(f"  BISEN_HARVEST_CAL_HIGH_CODE={hi_code} BISEN_HARVEST_CAL_HIGH_UV={hi_uv}")

if __name__ == "__main__":
    main()
