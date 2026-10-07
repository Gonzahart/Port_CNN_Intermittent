# GPIO17 pin-capacitor A/B, 2026-10-07

Calibration image with `BISEN_HARVEST_ADC_DIAG_COMPARE=1 BISEN_HARVEST_ADC_DIAG_DEEP=1`, GPIO17,
scope disconnected, USB unplugged for BTN0 captures, VCAP = 6.20 V (KM100) for every press.
A = 10 nF (8 records, second attempt; first attempt lost), B = 100 nF (7 records).
No 100 nF calibration spot-check at 5.6/6.8/7.7 V was taken (all presses at 6.20 V).

`codes_*.txt`: parsed from the SWO readout pasted in chat (the tee files were empty because the
SWO viewer buffers output into a pipe). Line prefixes: O = 32 ordered policy readings
(median-of-3 AVG16; odd records switched/production, even held), F = LP1-AVG16 full-precision pass,
L = LP0-AVG16 pass, A = LP1-AVG1 single conversions. Trims and CFG/SL1CFG identical in all records.

| Pooled within-record SD (codes; 1 code = 11.7 mV VCAP) | 10 nF | 100 nF | change |
|---|---|---|---|
| Policy reading, production (switched) | 5.48 | 4.60 | -16 % |
| Policy reading, all records | 5.70 | 4.47 | -22 % |
| LP1-AVG16 | 8.02 | 5.59 | -30 % |
| LP0-AVG16 | 7.05 | 5.55 | -21 % |
| LP1-AVG1 (single conversion) | 34.1 | 23.8 | -30 % |
| Policy mean at 6.20 V | 525.4 | 525.7 | +0.3 code |

Sweep fit (2026-10-06) predicts 526.7 at 6.20 V; both means are within ~1.3 codes, so the
476/655 anchors stay valid with 100 nF. All FIFO/slot error counters were zero.

Interpretation: a 10x larger capacitor cut single-conversion noise by only ~30 %. If the
pin-side noise were broadband and fully filtered by the larger RC, it would fall by about 3x;
a simple two-component split (internal + pin) gives roughly 22 codes of noise that the
capacitor cannot reach (assumption-dependent estimate). Further gains need firmware
averaging (e.g. AVG128 ~ 23.8/sqrt(128) ~ 2 codes per reading) or decision debouncing.

Bench note: pressing RESET erased the retained SRAM log (contrary to the banner text
"a reset is tolerated while VDD remains powered").
