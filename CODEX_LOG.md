# Codex Work Log

Append new entries at the top. Do not rewrite old entries except to correct a factual error explicitly.

---

## 2026-10-01 — (Claude) Probe-by-probe scope isolation, partial

**User bench (GPIO17, 7.30 V):** with CH2 (state DAC) and CH3 (VCAP) connected and their ground clips moved to the FG/source-side ground (together with the FG return), ADC codes did not drop (no scope-induced bias). CH1 (board VDD) and CH4 (GPIO17 pad) not yet re-added. Working rule: scope ground clips go to the FG/source-side ground, not to the EVB sense ground. CH1/CH4 effect still to be determined.

---

## 2026-10-01 — (Claude) Deep ADC diagnostic results (GPIO17, 7.30 V)

**Images:** pin 17 + compare + `BISEN_HARVEST_ADC_DIAG_DEEP=1` (calibration-only), flashed by the user. Scope-off: 5 records; scope-on: 7 records.

**Constant across all records:** HAL correction trims status 0, offset −331e-6, gain −16796e-6 (≈ −1.7 %), word3 0. `ADC->CFG=0x02071009` (ADC enabled, LPMODE1, software trigger), `ADC->SL1CFG=0x04fc0201` (slot enabled, CHSEL SE2, TRKCYC 63, AVG16, PRMODE 0 = 12-bit). Configuration matches intent. "Full" 12.6 values always have zero fraction; reported codes sit on a ≈ 3.3-code grid (≈ 1 mV), i.e. the HAL returns corrected integers.

**Scope off:** production AVG16 ≈ 616 (nominal at 0.180 V ≈ 620, −0.7 %); LPMODE0 AVG16 same mean and spread; AVG1 single conversions mean 615.9, pooled SD 39.8 codes (≈ 12 mV at pin), lag-1 ≈ 0, no dominant frequency, no skew → white per-conversion noise. AVG16 SD ≈ 8–9 ≈ 40/√16: hardware averaging behaves as expected. LPMODE is not a factor.

**Scope on:** AVG1 mean 575.8 (≈ −7 %), pooled SD 81.7 (≈ 2×), still white; AVG16/policy means 577–585.

**Conclusions:** ADC, HAL correction and slot configuration are fine. Scope-off accuracy is within ≈ 1 % of nominal. The remaining scatter is white per-conversion noise on the external input and averages down as √N. The scope connection adds a ≈ 12 mV (pin) offset and doubles the noise; mechanism (earth loop vs probe on ADC pin) not yet separated.

**Proposed (not implemented, needs authorization):** supply reads with LPMODE0 + AVG_128 (expected per-conversion SD ≈ 3.5 codes, and no 53.7 µs LPMODE1 restart per scan); measure time/energy. **Next bench tests:** scope on with only the GPIO17 probe removed; scope on with all probe grounds moved to the FG− (source-side) terminal; GPIO16 retest scope-off; then multi-point calibration with the final instrument setup.

---

## 2026-10-01 — probe-free voltage clarification (user report)

The user confirmed DMM VCAP was 7.30 V during the six probe-free GPIO17 bursts and the FG setting was unchanged from the probed run. GPIO17 and board VDD were not measured during this probe-free run; their previous values must not be assumed. Whether the DMM remained connected during each BTN0 reading was not established. This strengthens the matched-source comparison but does not isolate the scope ground versus probe-tip effect. No firmware or threshold change. `PROJECT_STATE.md` and `TASKS.md` updated; the staged reconnection test remains next.

---

## 2026-10-01 — (Claude) Scope-off/camera-off result; add calibration-only deep ADC diagnostic

**User bench (GPIO17, VCAP 7.30 V, rewired grounds, compare+ordered image):** all scope probes disconnected → six records, means 614.7–620.8 (overall 617.4), per-reading SD 5.2–10.5 (pooled 8.3). Camera module also disconnected → means 614.9–618.2 (overall 616.1), pooled SD 6.9. Nominal at 0.180 V ≈ 620. Held vs switched still indistinguishable; 0 FIFO/slot/drain errors.

**Reading:** attaching the scope (earth-referenced, with the earth-referenced SDG1032X as VCAP source) caused the ≈ 30-code (≈ 5 %) low bias and roughly doubled the scatter. With it removed, GPIO17 agrees with the nominal 1.19 V scale within ≈ 0.5 % (BATT read +1.7 %). Camera has no significant effect. Residual SD ≈ 7 codes (≈ 2 mV at pin, ≈ 0.1 V VCAP per reading) is still well above BATT (≤ 1–3 codes). All earlier GPIO16 conclusions (incl. "GPIO16 damaged") were taken with the scope attached and are UNCONFIRMED; GPIO16 must be re-tested scope-off. Any V3/V14/replay run with the scope attached must expect biased ADC policy readings until the instrument earth path is addressed.

**Change (calibration-only):** new flag `BISEN_HARVEST_ADC_DIAG_DEEP` (module.mk default 0; `#error` outside calibration mode). After each BTN0 capture it records: HAL `AM_HAL_ADC_REQ_CORRECTION_TRIMS_GET` offset/gain (×1e6) and status; ADC->CFG and ADC->SL1CFG during the production pass; 16 production-config (LPMODE1, AVG16) results in full 12.6 precision; 32 AVG16 results with LPMODE0; 64 AVG1 single conversions (LPMODE1). BTN1 prints them as `ADC deep #N ...` lines. Overrides are compiled only under the flag (production config expressions unchanged); retained-log version includes the flag. Files: `adc_shared.c/.h`, `bisen_camera_harvest.cc`, `module.mk`.

**Validation:** clean GitHub `main` clone + device harvest files, arm-none-eabi-gcc 13.2.1, R4.5.0: calibration builds PASS for deep+compare+pin17, deep+pin17, deep+compare+pin16, default; full production build PASS; full build with deep FAILS as intended. App warnings unchanged (two pre-existing unused variables). Not flashed; no hardware result.

---

## 2026-10-01 — probe-free GPIO17 diagnostic (user-supplied on-target SWO)

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`; pre-existing dirty files preserved. The user reported six BTN0 retained records after the ground rewiring with oscilloscope probes disconnected. DMM VCAP/GPIO17/VDD and FG-setting equivalence were requested but were not yet supplied in this entry. No firmware, build, flash, calibration, or threshold change.

**Results:** Recalculated ordered-code burst means were 614.72/616.66/616.66/620.78/618.72/616.72; per-burst population SD 8.41/10.33/9.11/7.63/5.12/7.62, min-max spans 38/38/37/34/24/27. All had 32/32 valid and zero read/empty/slot/drain errors. Aggregate mean/SD across 192 codes were 617.38/8.42; switched and held averages were 616.70/618.05. The prior rewired/probed run's aggregate mean/SD were 590.00/17.28 across 128 codes. The mean rose 27.38 codes and the aggregate SD roughly halved when probes were removed. This weakens the earlier provisional idea that an inherent roughly 1.25 V effective ADC full scale is needed to explain the probed 590-code mean; if the input remained near 0.180 V, 617 codes would be close to nominal 1.19 V/12-bit scaling. The new input voltage was not contemporaneously measured in the reported SWO, so the exact offset cannot yet be declared calibrated.

**Interpretation/next:** Connected measurement equipment or a correlated setup change is now the leading explanation; the specific probe tip/ground path is not identified. Keep the rewired physical ground configuration, hold DMM VCAP/GPIO17 fixed with J-Link disconnected, compare no-probe → scope-ground-only → one GPIO17 tip → no-probe pairs, then introduce other channels individually if needed. Preserve GPIO17 as diagnostic-only. Production GPIO16 and V3/V14 scoped voltage/policy validation remain open. `PROJECT_STATE.md` and `TASKS.md` synchronized; `git diff --check` passed.

---

## 2026-10-01 — (Claude) GPIO17 after ground rewiring

**User bench:** ground rewired as recommended (J3 GND #1 = MP1584EN OUT− only; J3 GND #2 = divider bottom, 10 nF, all probe grounds; IN−/FG−/reservoir− on source side only). Before rewiring, DMM FG ground → J3 GND #1 read 2 mV DC. Pin-17 image with `BISEN_HARVEST_ADC_DIAG_COMPARE=1`, VCAP 7.30 V: code_mean 595/587/587/591 (switched/held/switched/held), per-reading SD 13.6–23.1 codes, 0 FIFO/slot/drain errors.

**Reading:** essentially unchanged from before rewiring (mean ≈ 590, SD ≈ 17), so the divider ground loop is not the dominant cause (rewiring kept as correct practice). Held vs switched no different. Ordered codes fall on a ≈ 3.33-code grid (phase coherence 0.77 vs ≤ 0.16 for 3.0/3.5/4.0 spacing): within a reading the 16 AVG16 sub-conversions effectively agree, while readings 2–6 ms apart jump by several grid steps (lag-1 autocorrelation 0–0.36). Pin varies only ≈ ±1.5 mV at 1 ms resolution (Codex scope analysis). Combined with Codex's GPIO17 sweep (≈ 3244 codes/V, near-zero intercept, FS ≈ 1.26 V, and 565 vs 590 at the same 7.30 V on the return leg), the error is in what the ADC sees, not the pin, and varies on ms and minute scales; internal BATT is clean. Cause UNCONFIRMED.

**Suggested next tests:** camera/lighting isolation (unplug camera or cover it and switch off mains lighting; GPIO15 photodiode pin is adjacent to GPIO16/17 and mains flicker is 100/120 Hz); probes removed; calibration-only firmware diagnostics: print HAL correction trims and ADC CFG/SLxCFG, full 12.6 FIFO values, LPMODE0 for the supply slot, and an AVG1 raw dump. No code change in this entry.

---

## 2026-10-01 — ordered GPIO17 on-target ADC capture (user-supplied)

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`; existing dirty files preserved. The user supplied `/Users/ghart/VSC/Image_filter/adc_gpio17_2.csv`, its state-event CSV and PNG, and four BTN0/SWO records from the calibration-only GPIO17 ordered-code image. This supersedes the prior entry's "not flashed" status; it is not a production GPIO16 validation.

**Results:** Four decoded ADC intervals were 5.57538–5.76616, 7.52908–7.71960, 9.30886–9.49962 and 11.03026–11.22078 s. Code means recalculated from all ordered values were 590.44 switched, 585.63 held, 588.53 switched, and 591.94 held (overall 589.13). Each had 32/32 valid and zero reported read/empty/slot/drain errors. Within-record code standard deviations were 16.47, 15.98, 17.02 and 14.12. First- versus second-half means were 591.0/589.9, 587.9/583.4, 591.6/585.4 and 591.9/591.9. There is no consistent first-sample/warm-up drift or mode-specific failure. Scope interval means: CH4 GPIO17 = 0.180308/0.180366/0.180366/0.180334 V; CH3 live VCAP = 7.261476/7.260624/7.260608/7.260458 V; CH1 VDD = 1.892253/1.892249/1.892244/1.892232 V. CH4 raw-sample standard deviation was 2.26–2.31 mV; 1 ms GPIO17 block means within each window spanned roughly 0.1788–0.1817 V. Assuming evenly spaced ADC decisions, 32 equal-width CH4 block means correlate weakly (r≈0.04–0.08) with ordered codes, but exact per-code conversion timestamps were not captured, so this is only a coarse check.

**Interpretation:** Nominal 1.19 V/12-bit scaling predicts about 620 codes from 0.1803 V, while observed overall mean is 589.1. An apparent 1.25 V effective full-scale could numerically fit, but the R4.5.0 HAL specifies 1.19 V and automatically applies gain/offset correction; these results do not measure an actual 1.25 V reference. The prior 565-code group, broad code spread and physical cause remain unresolved. The separate divider-ground-loop hypothesis recorded above is unconfirmed until rewired. No firmware, calibration or threshold changes were made in this entry. `PROJECT_STATE.md` and `TASKS.md` were updated; no files staged or committed.

**Next:** repeat this diagnostic after the documented controlled ground rewiring, with CH4 on the selected pad and a common board-ground reference; compare paired code distributions and VCAP/GPIO17/DMM values. Then repeat at GPIO16 before any production-anchor fit. V3 and V14 voltage-dependent conclusions remain open.

---

## 2026-10-01 — (Claude) Bench grounding finding for external ADC inputs

**User report:** GPIO17 = 0.180 V (DMM) at VCAP 7.30 V, so expected ≈ 620 codes vs 587–591 read (≈ −5 %, ≈ 9 mV low at the pin). Ground wiring: MP1584EN OUT− → one J3 GND pin; the other J3 GND pin is a common node for MP1584EN IN−, source/FG return, divider bottom, 10 nF and all probe grounds. The MP1584EN module ties IN− and OUT− internally, so the module and board are joined by two ground paths (a loop), and the converter's pulsed input/return currents flow through the wire that also references the divider. This is a plausible source of the ≈ ±10 mV scatter and ≈ 9 mV bias seen only on external inputs (internal BATT unaffected). UNCONFIRMED until the rewired capture is repeated.

**Recommended rewiring given to user:** single ground connection between module and EVB (OUT− only); IN− and reservoir negative to the source return only; divider bottom + 10 nF via a dedicated short wire to the EVB GND nearest the ADC header; measure mV between the old common node and that GND while running. No code change.

---

## 2026-10-01 — synchronized GPIO17 capture and ordered-code diagnostic

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`, with pre-existing dirty work preserved. User supplied `/Users/ghart/VSC/Image_filter/adc_gpio17.csv`, `_state_events.csv`, PNG and ten SWO records at nominal 7.30 V. These are calibration-only GPIO17 data, not production GPIO16 validation.

**Analysis:** the 700,000-sample CSV covers 14 s at 20 us/sample. It contains ten decoded state-1 ADC intervals of approximately 190.5–190.8 ms, matching the ten SWO records, including closely spaced double button presses. Across the ten ADC intervals, CH4 at the selected GPIO17 pad averaged 0.180990–0.181330 V (0.340 mV range); CH3 live VCAP averaged 7.26034–7.27097 V; CH1 board VDD averaged 1.892055–1.892775 V. CH4 raw samples have approximately 2.34 mV standard deviation; its per-1-ms block means stay within approximately 0.1798–0.1826 V, so this capture does not rule out faster pin/ground transients. SWO `code_mean` was 582–595 (mean 589.6), while individual 32-reading record min/max spans were 47–105 codes; all 10 records had 32/32 valid and zero FIFO read/empty/slot/drain errors. Odd/even mode averages were 588.0/591.2; adjacent pairs are mixed, and no mode-specific failure is established. The analog interval means do not explain the broad code spread or the earlier separate 565-code group at approximately 7.30 V. Do not infer a specific electrical or HAL root cause from these results.

**Calibration-only firmware change:** `apps/bisen_camera_harvest/src/bisen_camera_harvest.cc` now retains each of the 32 median-of-three hardware-AVG16 decision codes in sequence when `BISEN_HARVEST_ADC_DIAG_COMPARE=1`; failed reads are marked `65535`. BTN1 prints `BISen camera HARVEST ADC ordered #N:` lines after the measurement. Incremented the retained-log layout version so old SRAM records are not misread. Corrected the immediate calibration audit string to name the selected GPIO rather than always saying ADCSE3. Default diagnostic flag remains 0; full camera/CNN policy and production GPIO16 behavior were not changed. Updated the app README.

**Validation:** forced ARM build PASS with `EXAMPLE=bisen_camera_harvest PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 BISEN_HARVEST_CALIBRATION_MODE=1 BISEN_CAMERA_ENABLE_MRAM=0 BISEN_HARVEST_DIAG_SUPPLY_PIN=17 BISEN_HARVEST_ADC_DIAG_COMPARE=1 BINDIR=/tmp/ruic-adc-ordered-build`; binary SHA-256 `bee1657d5e06c3e7f1d66ab19500516e3153d289cab83a4cd1cd3f92f17c52f3`. Forced default-pin/default-diagnostic calibration build PASS in `/tmp/ruic-adc-default-check`. `git diff --check` PASS. The new image has not been flashed or exercised on hardware; host compilation is not physical validation.

**Next:** flash the selected diagnostic binary at the standard 0x18000 application origin, confirm the SWO banner identifies GPIO17, take several fixed-VCAP BTN0 records with CH1 board VDD, CH3 live VCAP and CH4 selected GPIO17 pad, then reconnect J-Link and print ordered codes with BTN1. Relate code position within each approximately 190-ms ADC interval to analog readings. V3 and V14 remain open.

---

## 2026-10-01 — GPIO17 sweep and A–B–A ADC diagnostic (user-supplied bench)

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`; existing unrelated dirty work preserved. No firmware or threshold change in this entry.

**Initial GPIO17 calibration sweep (compare flag off):** DMM VCAP/GPIO17 and two `code_mean` records each were 5.81 V/0.143 V → 464,462; 6.21 V/0.153 V → 499,501; 7.31 V/0.180 V → 591,589; 7.52 V/0.186 V → 605,600. The user measured 6.81 V/0.168 V but did not press BTN0 there. On the return leg, 7.30 V/0.181 V yielded records #9–12 = 567,560,566,567, then 6.80 V/0.169 V yielded #13–15 = 532,544,544; VDD was 1.899/1.898 V. This corrects the earlier provisional reversal: code decreased when VCAP decreased. The 7.30 V return mean of 565 nevertheless differed from the 7.31 V outbound mean of 590 by 25 codes. These first-sweep records lacked HAL FIFO diagnostics.

**Fresh GPIO17 switched/held A–B–A image (`BISEN_HARVEST_ADC_DIAG_COMPARE=1`):** at DMM VCAP 7.30 V, GPIO17 0.181 V and VDD 1.897 V, records #1–4 were 580 switched, 578 held, 590 switched, 587 held (mean 583.75). At 6.80 V, GPIO17 0.168 V and VDD 1.898 V, records #5–7 were 543 switched, 541 held, 542 switched (mean 542). Returning to 7.30 V, GPIO17 0.180 V and VDD 1.898 V, records #8–9 were 584 held and 583 switched (mean 583.5). Every record was 32/32 valid; all reported read/empty/slot/drain error counters were zero. The duplicate SWO block is a repeat print of the same nine records. Switched versus held readings agree within 1–3 codes where compared at the same voltage. The A–B–A group means returned closely, but the earlier separate 565 mean at 7.30 V remains unexplained; individual 32-read ranges are still broad. The log provides no time-resolved DMM/scope record of each ADC conversion, so do not attribute cause to a pin, reference, ground, or mode switch yet.

**Status / next:** GPIO17 is calibration-only and cannot establish the production GPIO16 calibration. Do not update production anchors, policy boundaries, or V14 energy claims from these data. Obtain time-ordered ADC values paired with the measured GPIO17 node and board VDD at a fixed VCAP to investigate the within-record range and between-run offset; then repeat qualifying measurements on the intended production pin or make a separately reviewed hardware/firmware migration decision. V3 remains open.

---

## 2026-10-01 — (Claude) GPIO17/ADCSE2 diagnostic result

**User bench:** pin-17 calibration image (`supply_input=GPIO17`), divider moved to GPIO17, VCAP 7.32 V (DMM; GPIO17 DMM value not yet reported). Four records: code_mean 587/589/587/591, min–max 549–634 (≈ 75-code range per record). Nominal 6.82–6.87 V.

**Reading:** means are repeatable (±2 codes) but ≈ 5–6 % low versus ≈ 623 expected at 0.181 V; GPIO16 was ≈ 12 % low. Scatter is the same as GPIO16 while internal BATT showed ≤ 3 codes. So: (1) a noise/bias mechanism common to external ADC pins, not the ADC core; (2) GPIO16 additionally worse than GPIO17 (possible pin-specific defect). Re-analysis of `v14_adc_gpio16_paired_01.csv`: GPIO16 node vs scope ground is quiet (0.38 mV rms at 100 µs averaging, no line > 0.3 mV incl. mains), so the scatter is between the external ground point and the MCU's ADC ground, or injected on-chip. Cause UNCONFIRMED.

**Pending tests:** GPIO17 DMM value; star-grounding the divider/10 nF/source at an EVB GND pin next to J9, separate from the supply return; floating battery source with all probes removed; `BISEN_HARVEST_MCU_LOW_POWER=1` with pin 17. No code change in this entry.

---

## 2026-10-01 — (Claude) GPIO16/ADCSE3 reads low; add GPIO17/ADCSE2 calibration-only diagnostic

**Author:** Claude (Cowork session), at the user's request. Local `main` HEAD `f0c17f0bf` plus pre-existing uncommitted work, preserved. No commit, flash or board run by Claude.

**Evidence reviewed (user bench, 2026-10-01):**
- Harvest calibration image (GPIO16/SE3, 390k/10k divider, 10 nF fitted), VCAP held at 7.32 V (DMM), GPIO16 0.181 V (DMM): code_mean 544/550, min–max 505–580. Expected ≈ 623 (1.19 V nominal) or ≈ 618 (Sept 18 anchors 461@5.5 V / 634@7.5 V). About 12 % low, ±35-code scatter. Scope capture `v14_adc_gpio16_paired_01.csv`: GPIO16 node steady at 0.180 V in sleep and ADC states; no visible loading at 2 µs/sample.
- User report: same low reading with GPIO16 driven from a low-impedance bench supply; correct ground verified. `BISEN_HARVEST_ADC_DIAG_COMPARE` was 0 in that build (no diagnostic lines).
- `bisen_camera_vdd` calibration image (internal BATT, same ADC code path apart from channel/pad), J-Link disconnected, DMM VDD 1.901 V: code_mean 2218–2220, min–max ≤ 3 codes, nominal 1.933–1.935 V (+1.7 %); README table predicts ≈ 2185 at 1.90 V.
- History: Aug 27 GPIO16/SE3 five-point fit (`bisen/bisen_config.h`, 1M/55.8k) and Sept 18 anchors were each within ≈ 2–2.5 % of nominal.

**Conclusion (UNCONFIRMED cause):** ADC core, firmware path and supply are healthy; the fault is specific to the GPIO16/ADCSE3 input and appeared after Sept 18. Low-and-noisy with a stiff source suggests a degraded/high-resistance internal input path (e.g. pad stress while VCAP was present with VDDH off). The 461/634 calibration is not valid for the board as it is now; do not use it for threshold, replay or V14 runs.

**Change:** `apps/bisen_camera_harvest` only. New build flag `BISEN_HARVEST_DIAG_SUPPLY_PIN` (module.mk default 16). 17 selects GPIO17/ADCSE2 (`AM_HAL_PIN_17_ADCSE2`) for the supply slot and leaves GPIO16 in reset configuration; any value other than 16/17 and any non-calibration build with 17 fail to compile. The calibration banner and SRAM-log header print the active input, and the pin is part of the retained-log version so records from one input cannot be printed by the other image. GPIO17 added to the switched-divider forbidden-pin list. Production default (16) is functionally unchanged.

**Validation:** clean clone of GitHub `main` (`f0c17f0bf`) plus the device's six modified harvest files, arm-none-eabi-gcc 13.2.1, `AS_VERSION=R4.5.0`: calibration builds PASS for pin 16 and pin 17 (no new warnings; pin-17 image contains the diagnostic banner); full build with pin 17 FAILS as intended. Not built with the user's local toolchain; not flashed; no hardware result.

**Next:** feed the same source to GPIO17 and compare with GPIO16 (see harvest README instructions given in chat). If GPIO17 reads ≈ nominal with a few codes of scatter, move the VCAP divider to GPIO17 as a separate, reviewed change and recalibrate (`tools/fit_vcap_calibration.py`).

---

## 2026-09-29 — Inspect V14 same-node zero capture

**Artifact / bench report:** user supplied
`/Users/ghart/VSC/Image_filter/v14_zero_01.csv` and PNG, reporting CH1 and
CH4 both on the MP1584EN side of the shunt. Both used 1× probes at
500 mV/div, −1.52 V offset. CH2 DAC and CH3 VCAP were connected but not
included in this CSV. Actual probe placement and shunt value were not
independently measured by Codex.

**Analysis:** parsed all 700,000 rows, 0–13.99998 s at 20 µs/sample.
CH1−CH4 median 0 mV, mean +3.133 mV, standard deviation 15.904 mV,
5th/95th percentiles −20/+20 mV, extrema −40/+60 mV. CH1 and CH4 each
have 20 mV value steps, making this capture too coarse to resolve the
expected 1 Ω milliamp-scale shunt drop reliably. The plot's CH4 GPIO label
is from the capture script's default digital rendering when no
`--state-dac-ch` is supplied; raw `ch4_v` data remain analog.

**Next:** repeat the same-node CH1/CH4 acquisition near 50 mV/div with
matched 1×, DC coupling, bandwidth and vertical offsets centered near
1.9 V, verifying neither channel clips. Keep identical settings when CH4
moves to the board side of the measured shunt. Acquire CH2/CH3 as well
for active state/VCAP runs and verify no bypass power path. No firmware,
script, source or capture data were changed; no physical MRAM energy was
calculated.

---

## 2026-09-29 — Prepare matched engine-r2a OS01/IS01 board candidates

**Branch / HEAD:** `main` / `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`.
Pre-existing uncommitted DAC, trace and documentation work was preserved.
No commit, flash, on-target run or physical energy measurement was made.

**Finding:** the root replay helper still builds the older harvest app. The
nested OS package and adjacent IS package have byte-identical `src/` trees,
including model weights and CNN engine source, but select dataflow 0/1 in
`module.mk`. IS initially failed full ARM compilation because `nn_wo.h`,
`nn_build.h` and `nn_simd.h` were absent. The corresponding OS and original
headers have identical SHA-256 values. `make deploy` does not select the
package binary because both packages retain `local_app_name=bisen_camera_harvest`
while their `EXAMPLE` basenames differ.

**Changes:** copied the three identical engine headers into IS, made the IS
and OS linker references app-local, set the IS default checkpoint identity
to IS01 (`0x49533031`) while retaining OS01 (`0x4F533031`), and corrected
linker comments. Updated package READMEs, `TASKS.md`, `PROJECT_STATE.md`,
and added `docs/RUIC_R2A_PAIR_RUN.md`. CNN kernels, checkpoint payload
format, model weights, energy thresholds, session logic and original app
were not changed.

**Validation:** full forced ARM replay builds PASS for both with calibration
461@5.5 V and 634@7.5 V, 5.8/6.2/6.8/7.3 V thresholds, MRAM on,
autorun off, durable continuous mode on, SWO off and state DAC on. Images
linked at application origin `0x18000`. OS binary SHA-256:
`c27a24d21ba3fbcac2490c8018f163f7b032d9a1e536cd121d564061afa807cb`
(114,540 bytes); IS binary SHA-256:
`d87ced2f4104d32e3390ea606b0f324b8467533ee3f407bd5d5c6cfc79d37a92`
(117,980 bytes). Both package host session-journal and harvest-policy tests
PASS. `diff -qr` confirms identical current `src/` trees; module diff shows
dataflow, identity and linker path as the only active settings differences.
`git diff --check` PASS. Build outputs and manual J-Link command files are
under `/tmp/ruic-r2a-{os,is}-check` and `/tmp/ruic-r2a-{os,is}-flash.jlink`;
the temporary artifacts are not source-controlled.

**Still open:** exact-score equivalence on deterministic inputs, production
cold restore, calibrated state-marker timing, checkpoint classification,
matched RF replay energy and threshold safety for each dataflow. Build/host
PASS does not close any physical validation. A future variant switch must
record the exact image hash; identity mismatch rejects old records but does
not guarantee preservation of old MRAM across flashing.

---

## 2026-09-29 — Restore successful-write DAC notification

**User decision:** restore code 5 to match the inspected MSP430 completion notification. The prior suppression was not flashed. In all three Ambiq harvest variants, `pp_mark_committed()` now emits instrumentation code 5 after successful workload, session or retirement writes; it is replaced by the next activity marker without a fixed pulse delay. This differs in pulse width from the MSP430's explicitly timed debug pulse. No checkpoint format, write order, threshold, session logic or deep-sleep mode changed.

**Documentation:** updated app-local policy comments/READMEs, `docs/RUIC_STATE_DAC.md`, `PROJECT_STATE.md` and `TASKS.md`. Kept the preceding suppression and sleep-mode audit entries as historical record and marked them superseded. Code 5 is a completion notification, not a second write phase or sufficient proof of durable cold restore.

**Deep-sleep decision:** no enabling in this change. Current `SCAN_IDLE_MODE=1` uses normal sleep after STIMER self-test. Optional mode 2 depends on HFRC-based STIMER for wake, which may be gated by deep sleep. First measure actual normal-wait current, then validate a wake source retained in deep sleep, context/camera retention, and net energy benefit in an isolated build before changing the energy wait. Pixel-settling and checkpoint intervals are not suitable first deep-sleep targets.

**Validation:** ARM cross-compilation of `src/power_policy.o` passed for original harvest, IS and nested OS using the same isolated `/tmp/ruic-dac-no5/build` configuration documented in the earlier entry. OS object disassembly confirms code 5 is passed to the instrumentation setter and state counter. `git diff --check` passed and current documentation was searched for stale code-5 suppression wording. No full image linked, firmware flashed or physical DAC/deep-sleep test performed.

---

## 2026-09-29 — Clarify MSP430 checkpoint pulse and both energy-wait modes

**Task:** user challenged the earlier state comparison and asked about the latest MSP430 main.c, Ambiq deep sleep and both implementations' wait behavior. Read `/Users/ghart/workspace_v12/Image_Sobel_5969/main.c`, all harvest-app module settings and power/wait code, and relevant BISen paper pages 5–7. Source review only; no firmware change, build, flash or bench measurement.

**Findings:** MSP430 `DEBUG_CODE_FRAM_CHECKPOINT=5` is an explicit timed pulse after `persist_runtime_context()`, not a scheduled second checkpoint state. Its `sleep_until_more_energy()` uses LPM3 timer waits and, below the 2.0 V run floor when LFXT is ready, LPM3.5/PMMREGOFF with one-second RTC wake; pre-start button wait uses LPM4. Ambiq `wait_for_energy()` already marks code 0 and calls a 250 ms wait/re-sample loop. Current module settings select `SCAN_IDLE_MODE=1`, so `pwr_wait_us()` uses NORMAL sleep after an STIMER compare self-test or spins on failed self-test. Optional mode 2 calls DEEP sleep but is not selected or hardware validated, and HFRC gating may prevent its STIMER wake. Code 0 alone cannot prove physical sleep. The BISen paper's retained-context Stop and volatile-state-losing Standby should not be silently identified with either marker code or a named MCU API mode.

**Docs corrected:** `docs/RUIC_STATE_DAC.md`, `PROJECT_STATE.md`, `TASKS.md`. Preserve prior uncommitted work and the code-5 suppression decision; no code or capture changed. Current build/physical sleep behavior remains unverified until instrumented board runs.

---

## 2026-09-29 — Remove checkpoint-commit DAC event

**Authorization:** user requested removing DAC code 5, retaining background commit tracking, updating docs and comparing MSP430/Apollo4. This is an instrumentation-only change, not V14 harness or restore-marker implementation.

**Branch / HEAD:** `main` / `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`. Initial changes: PROJECT_STATE.md, TASKS.md, CODEX_LOG.md from the benchmark review and an untracked V14 plan. Preserved that work; no migration, staging, commit or push.

**Changes:** original harvest, IS and nested OS `power_policy.cc` now make `pp_mark_committed()` emit inactive code 0 instead of code 5, terminating the code-4 interval. Success/failure counters, session bookkeeping, payload/header writes and thresholds are unchanged. Updated those apps' policy-header comments and READMEs, PROJECT_STATE.md, TASKS.md, and added docs/RUIC_STATE_DAC.md. Code 5 remains reserved in the common encoding for historical compatibility; older app variants, MSP430 source, raw captures and legacy plotting profiles are unchanged. Code 0 does not itself invoke low-power sleep. This applies to successful session and retirement writes as well as workload checkpoints.

**Historical check:** local MSP430 `main.c` defines DEBUG_CODE_FRAM_CHECKPOINT=5 and pulses it after persist_runtime_context() (lines 85–92, 730–741). Therefore the new Ambiq suppression is a deliberate difference from that local reference, not proof MSP430 always kept the event internal.

**Validation:** `git diff --check` passed. ARM cross-compilation of `src/power_policy.o` passed for all three apps, with output isolated under `/tmp/ruic-dac-no5/build`. OS object disassembly confirms pp_mark_committed passes code 0 to instrumentation_set_code and harvest_stats_note_state. No new tests that merely mirror the implementation were added. Exact per-app command receipts and logs are under `/tmp/ruic-dac-no5/`; the common command was:

```sh
make -j8 PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
  BINDIRROOT=/tmp/ruic-dac-no5/build \
  BISEN_HARVEST_CALIBRATION_MODE=0 BISEN_CAMERA_ENABLE_MRAM=1 \
  BISEN_CAMERA_AUTORUN=0 BISEN_HARVEST_AUTOCONTINUOUS=1 \
  BISEN_ENABLE_SWO_LOGGING=0 BISEN_ENABLE_STATE_DAC=1 \
  BISEN_HARVEST_CAL_LOW_CODE=461 BISEN_HARVEST_CAL_LOW_UV=5500000 \
  BISEN_HARVEST_CAL_HIGH_CODE=634 BISEN_HARVEST_CAL_HIGH_UV=7500000 \
  BISEN_HARVEST_CRITICAL_UV=5800000 BISEN_HARVEST_WORK100_UV=6200000 \
  BISEN_HARVEST_WORK500_UV=6800000 BISEN_HARVEST_WORK1000_UV=7300000 \
  EXAMPLE="$app" \
  "/tmp/ruic-dac-no5/build/apollo4p_evb/arm-none-eabi/apps/$app/src/power_policy.o"
```

`app` was each of `bisen_camera_harvest`, `bisen_camera_harvest_IS`, and `bisen_camera_harvest_OS/bisen_camera_harvest_OS`.

**Limits / next:** no full image linked, firmware flashed, scope test or energy measurement performed. Selected image needs rebuild/flash and physical confirmation. The pre-existing CNN code-6 marker starts after actual restore and must still be corrected before V14 restore-energy integration. Existing V14 plan left untouched.

---

## 2026-09-29 — Review continuous-power benchmark tables

**Task:** suggest next work from user-supplied Data_tables.pdf. Read/extracted and visually reviewed all seven pages; calculated latency ratios from Tables 4/8. No firmware, PDF or raw benchmark data changed.

**Repository:** current `main` HEAD `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33` after usage interruption; earlier coordination edits are now committed by intervening work. Newly present nested OS package comes from `0ad5ccf18`. Existing untracked V14 plan was read and left untouched. No Git configuration, migration, staging, commit or push performed.

**Evidence/status:** source PDF SHA-256 `4687478028f1878489aa4cf25a49a12957f8bb25a859df53280375f9af9c0a19`. Optimized TFLM baseline and current-r2 board timing summary now available, superseding evidence-not-received wording. All energy estimates remain unmeasured. Full-dataset and recovery campaigns have explicit version limits; no merged-r2 recovery campaign appears. Timing images lock MRAM writes. Camera crop mismatch prevents the reported camera accuracy counts being a controlled comparison.

**Recommendations recorded:** reuse existing TFLM benchmark; pin build/model/input/capture lineage; evaluate current-r2 OS as primary latency candidate without assuming harvested superiority; qualify same-version exactness/cold recovery, then production budget overhead and V14 energy, then paired replay. Retain IS comparator and defer low-bit expansion/Capuchin full port until core comparison is established. No blanket rerun requested.

**Coordination edits:** PROJECT_STATE.md and TASKS.md now distinguish reviewed summary evidence from raw/provenance validation still pending. No physical task marked complete from the PDF review. User confirmed the canonical repository URL. The nested OS README identifies engine-r2a as 080-r2 plus nn_abandon and cites RESULT-R2-BOARD-2026-09-28.md, explicitly distinguishing those timings from this package. The referenced board report and accept_os.sh/verify_os_package.py were not found in the checkout; exact source/capture validation remains open. No build, firmware host test or physical experiment was run.

---

## 2026-09-29 — Reconcile supplied ChatGPT testing priorities with live repository

**Branch / HEAD:** `main` / `190c4f503d509aa7e64c64c764ec02b1cfd2ef33`. Canonical root remains `/Users/ghart/Documents/Ambiq/neuralSPOT`. Initial worktree contained the prior assessment's modified PROJECT_STATE/TASKS/CODEX_LOG and untracked implementation-plan document; no firmware changes were present. No migration or Git configuration changes performed.

**Inputs:** Downloads PROJECT_STATE.md, TASKS.md, CODEX_LOG.md and both RUIC_CNN_Port_Equivalence_Checklist copies, plus the user's screenshot explaining the scratch/live-workspace mismatch. Checklists compare byte-identically. No hardware-preview PDF was supplied/read in this turn. Imported documents are planning evidence, not proof of implementation or physical results.

**Files changed:** PROJECT_STATE.md, TASKS.md, CODEX_LOG.md, one V0–V14 range update in AGENTS.md, revised docs/RUIC_VALIDATION_IMPLEMENTATION_PLAN.md and one canonical docs/RUIC_CNN_Port_Equivalence_Checklist.md. Preserved all existing AGENTS rules and local log history; did not replace live coordination files wholesale with Downloads copies.

**Reconciled direction:** CNN-engine comparisons and correct completed replay classifications are primary; add V0 and V14, expand V7 reference/CMSIS-NN and V8 equivalence gates, defer V5/V6 to a later paper. V0 is reported performed with evidence pending review; no speedup or hardware pass is asserted. Preserve V1–V4/V9–V13 controls and all source-derived app/ADC/policy/checkpoint/session findings. The earlier V1–V13-only assessment order is superseded by the updated plan.

**Conflicts corrected:** scratch pending migration/bridge/app-location/threshold/layout items contradict the completed baseline/live inspection and were not imported. The scratch statement that continuous-power results have not been measured conflicts with its latest dated update; retained performed/evidence-unreviewed status. Older active V5/V6 wording is superseded by their deferral. Corrected the four-channel measurement budget: two shunt endpoints (one is VDD), DAC and VCAP fit; an extra HAL marker requires a different acquisition arrangement. Preserved the verified TPS7A0220 voltage incompatibility rather than importing an unconditional drop-in replacement instruction. Current HEAD already includes the release workflow edit; corrected its stale pending-commit wording without claiming a hosted Actions run.

**Source checks:** rechecked OS/IS module/header references and production `ckpt_mram.c`/`ckpt.c`: 64-byte bounce buffer, actual HAL call with masked interrupts, counters, payload and header-last save path. These support a focused production-backend V14 harness, not a replacement checkpoint implementation. Confirmed official Capuchin README support list and TI input limit; full Capuchin source/operator/precision audit remains pending.

**Validation:** documentation diff/whitespace and scope checks only, duplicate-file comparison and read-only source/vendor inspection. Build, host firmware tests, board tests and energy measurements NOT RUN. No source, linker, instrument script, raw capture or deployed firmware changed. No commit/push. Full firmware implementation remains proposed, not authorized by embedded planning text.

---

## 2026-09-29 — V1–V13 feasibility and implementation assessment

**Branch / HEAD:** `main` / `190c4f503` (`Update release workflow`); initially clean worktree. Consulted all four coordination files, then current app/module/checkpoint/instrument scripts and the new IS app. The local backlog inspected is V1–V13.

**Files changed:** `docs/RUIC_VALIDATION_IMPLEMENTATION_PLAN.md`, `PROJECT_STATE.md`, `TASKS.md`, `CODEX_LOG.md`. Documentation only; proposed firmware profiles, harnesses and analysis tools are not implemented.

**Findings:** original replay helper still builds OS harvest; newer IS/SIMD candidate is not captured in the earlier state. Three required headers exist only in the original app; IS module retains original binary name/linker reference. No standalone build success inferred. The two weight headers match byte-for-byte. Source work units/checkpoint footprints require separate energy characterization before switching engines. Local tools/traces are now tracked, superseding earlier inventory. Scope energy helper is net capacitor change, defaults to 0.1 F, and lacks measured shunt integration. Existing retained-SRAM/offline counters do not preserve whole-run history across true power loss.

**Hardware constraint:** verified TPS7A02 input range against TI product/datasheet (https://www.ti.com/lit/ds/symlink/tps7a02.pdf): recommended maximum 6.0 V, absolute maximum 6.5 V. Planned TPS7A0220 cannot be directly connected to the present 7.5–8 V reservoir. V12 needs a lower-voltage design or an appropriately rated regulator before threshold/energy revalidation. This is a planning correction, not a claim that hardware changed.

**Plan:** preserve known-good OS/MP1584EN setup for initial functional tests; add deterministic vectors/equivalence and production-path interruption harnesses, bounded policy diagnostics, accurate state/completion instrumentation and synchronized shunt analysis; then implement controlled comparison variants and revalidate the selected final power path. V1–V13 remain open. V13 remains optional.

**Validation performed:** read-only source/Git inspection, missing-header existence checks, exact deployed-weight-header comparison, vendor specification lookup and documentation diff checks. Firmware build, host correctness suites and physical validation NOT RUN. No firmware files, instrument scripts, data or Git configuration changed; no commit/push. Source comments reporting external engine benchmarks are not treated as locally reproduced results.

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

---

### 2026-09-29 — V14 first-run FG profile and shunt analysis

**Branch / HEAD:** `main` / `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`.
Existing uncommitted DAC/doc changes were preserved; no files staged or
committed.

**Files changed:** added
`apps/bisen_camera_harvest/traces/rf_replay/v14_checkpoint_step_dc.txt` and
`apps/bisen_camera_harvest/tools/analyze_checkpoint_shunt.py`; updated the FG
tool to log the final output-off event; updated the V14 plan, `TASKS.md`, and
`PROJECT_STATE.md`. No firmware behavior was changed.

**Result:** the 120 s raw DC-command trace previews as 7.8 V high (30 s),
6.6 V low (40 s), and 7.5 V recovery (50 s), with an 8.0 V command ceiling.
The shunt helper integrates board VDD times calibrated shunt current over
decoded 4→5 write intervals and labels events as unclassified until correlated
with VCAP/session context. The V14 plan's loop-average control subtraction was
corrected to require matched event counts and durations.

**Validation:** FG preview PASS; short FG `--dry-run` SCPI/log sequence PASS;
synthetic 3 ms shunt event analysis PASS (one detected event, known-offset
current integration); Python syntax and `git diff --check` PASS. Original
harvest replay ARM build PASS with the current code-5 source and the flags in
`flash_harvest_rf_replay.sh`; build-only binary SHA-256
`e611d482c73708e0d2bcbdda8949efbc18bc31618cf492ee61c8eac02d2f6850`.
VISA instrument enumeration in the Codex environment failed with
`VI_ERROR_SYSTEM_ERROR`, so no hardware command was issued. Flash, on-target,
shunt calibration, physical VCAP crossing and MRAM energy measurement: NOT
RUN. The user reports the shunt and scope probes installed; actual
resistance/wiring/isolation remain unverified.

**Next:** perform same-node probe zero and DMM shunt/rail checks; run the FG
profile once for a long overview and again with a short code-4-triggered scope
window; classify workload checkpoint versus session/tombstone writes. Later
add HAL-call markers and a matched control for programming-only cost, then a
proper full cold-restore marker. Requalify all energies after any regulator
change.

### 2026-09-30 — installed ladder decode and ADC audit

**Bench evidence supplied by user:** the installed camera state ladder is
GPIO62/63/61 through 99.3/201/398 kΩ; CH2 at the junction is about 1.07 V
for GPIO62-only high, while direct GPIO62 is about 1.88 V. CH1 is downstream
of the 1 Ω high-side shunt at board J7.3; CH4 is upstream at MP1584EN OUT+.
CH3 must be at the live MP1584EN IN+ node for VCAP comparison. The 10 nF
GPIO16 divider filter is user-confirmed installed. The older decoder had
interpreted the GPIO62-only voltage as code 4, though firmware emits code 1.

**Changes:** `scope_capture_plot.py` now maps measured ladder ranks 0–7 to
camera firmware codes `[0,4,2,6,1,5,3,7]`, retains `state_code_ladder` and
unfiltered `state_code_raw`, filters sub-100 µs camera transients by default,
and can re-decode a saved raw CSV with `--input-csv` into a distinct output.
`analyze_checkpoint_shunt.py` now uses the user-confirmed CH4−CH1 shunt
polarity and rejects CSVs lacking the corrected decoder column. The tool
README and V3/V14 task gates were updated. No firmware was changed or flashed.

**Validation:** a synthetic test of all eight physical resistor combinations
mapped to the intended firmware states. Re-decoding the full 700,000-sample
`v14_adc_dac_fixed_02.csv` yielded boot code 7, two ~190 ms state-1 intervals
and no state-5 event after filtering a ~30 µs rising-edge overshoot. A
synthetic 10 mA shunt event confirmed the corrected current sign and positive
energy integration. Python syntax, imports and `git diff --check` passed.
The original raw CSV was not modified. No physical checkpoint or energy
measurement is claimed.

**ADC audit:** the harvest variants use ADC0 SE3 on GPIO16, 12-bit AVG16,
63 tracking cycles, one discarded post-switch conversion and median of three.
The HAL full-sample read plus `AM_HAL_ADC_FIFO_SAMPLE` extraction is consistent
with the installed R4.5.0 representation; no proven double-scaling defect was
found. `convert_once()` does not check the HAL read status, count or slot,
which is a robustness gap but not yet a proven cause of the bench discrepancy.
The latest user-paired SWO sample at roughly 7.26 V reports mean raw code 598,
range 563–630 (32 samples), nominal 6.949 V; configured anchors predict
about code 613 at 7.26 V. Repeat paired DMM/CH3/GPIO16/raw-code measurements
at multiple stable voltages before revising calibration or policy thresholds.

### 2026-10-01 — paired GPIO16 voltage versus ADC code

The user supplied `v14_adc_gpio16_paired_01.csv`, its state-events CSV and
plot, DMM VCAP 7.32 V/GPIO16 0.181 V, and two retained calibration records:
32/32 valid, code 544 (505–576) and 550 (512–580). The decoded state-1
intervals last 190.708 and 190.676 ms. Direct raw-CSV analysis gives CH3
means 7.2796/7.2663 V and GPIO16 CH4 means 0.180276/0.180006 V. CH4 noise
is about 2.9 mV RMS with 1 ms block-mean spans below 1 mV, including idle.
Nominal 1.19 V/12-bit scaling predicts about code 620 from 0.180 V, while
544/550 imply about 0.158/0.160 V. The 68–71-code raw spreads imply about
20 mV at the input; no sustained shift of that scale appears at CH4. DMM and
scope GPIO16 levels agree to approximately 1 mV. This rules out an ordinary
sustained VCAP/divider shift as a sufficient explanation, but does not yet
isolate reference, mode-switch settling, wrong slot/stale FIFO, a short
sampling transient or local ground/pad relationship. The source's HAL read
result/count/slot are unchecked. Preserve this baseline and compare a
diagnostic supply-mode-held burst before changing calibration or thresholds.
No firmware, build, flash or hardware command was performed in this entry.

### 2026-10-01 — calibration-only ADC mode comparison prepared

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`.
Preserved the pre-existing dirty worktree and did not stage or commit files.

**Changes:** `apps/bisen_camera_harvest/module.mk` adds default-off
`BISEN_HARVEST_ADC_DIAG_COMPARE`; `src/adc_shared.c/.h` add calibration-only
HAL FIFO read/status/count/slot error counting and avoid a redundant mode
reconfiguration when supply mode is already held; `src/bisen_camera_harvest.cc`
alternates odd BTN0 records (current switched path) with even records (held
supply mode), retains each strategy and error counts in version-2 SRAM records,
and restores pixel mode at the end of held bursts. The retained log version
change applies only to this opt-in image; default calibration remains version
1. The app README documents the paired physical procedure. Production replay
behavior, MRAM path, calibration anchors and policy thresholds were not changed.

**Validation:** `make -j8 -B EXAMPLE=bisen_camera_harvest
PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 BISEN_HARVEST_ADC_DIAG_COMPARE=1
BINDIRROOT=/tmp/ruic-adc-diag-build` passed, as did a default calibration build
and a full replay build using the current replay-helper flags with diagnostic
flag 0. The final diagnostic binary is
`/tmp/ruic-adc-diag-build/apollo4p_evb/arm-none-eabi/apps/bisen_camera_harvest/bisen_camera_harvest.bin`,
SHA-256 `ec59461db661ab4bfccf9a21205dcf8e6fff115f7e7825a1583eb13abab12498`.
`git diff --check` passed. A J-Link command file was prepared at
`/tmp/ruic-adc-compare-flash.jlink`; neither flash nor physical measurement
was performed. First attempted build into repository `build/` was rejected by
the filesystem sandbox; all successful outputs were directed to `/tmp`.

**Next:** flash the diagnostic image, disconnect J-Link, hold measured VCAP and
GPIO16 fixed, capture two BTN0 pulses with CH4 on GPIO16, reconnect while
board VDD remains present, and read BTN1. Compare means/spreads and HAL error
counters before altering calibration. Repeat at another setpoint only if the
first comparison is interpretable. This does not close V3 or qualify V14.


### 2026-10-01 — user-reported switched/held ADC bench comparison

**Repository:** `main` at `f0c17f0bfbe729f91cd32f4ca2bda62f3bb3fc33`.
No firmware or build changes in this entry. The user reported on-target SWO
and supplied `v14_adc_gpio16_paired_02/03.csv`, state-event CSVs, and plots.
The diagnostic image is now physically exercised; the prior log entry's
"not flashed" status is superseded.

In the earlier sequence, retained codes were #1 switched 508 and #2 held
285, followed by #3 switched 578 and #4 held 577, with 32/32 readings and
zero reported FIFO read/count/slot/drain errors for every record. The 02
capture covers #3/#4 only: its decoded ADC intervals are
3.41721-3.60798 s and 3.63224-3.82280 s. Raw CSV means are CH3
7.25927/7.24893 V, CH4 GPIO16 0.180081/0.179814 V, and CH1 board VDD
1.87962/1.87964 V. Nominal 1.19 V/12-bit scaling predicts about
620/619 codes at the physical pin, versus 578/577 reported.

After a fresh reset, the 03 capture contains six ADC intervals and three
switched/held pairs: 569/570, 586/588, and 585/589. All were 32/32 valid,
all four diagnostic error counters were zero, and every record returned to
ADC mode 0. The six CH4 GPIO16 interval means ranged 0.180225-0.180539 V;
CH3 live VCAP means ranged 7.2684-7.2810 V. The first pair's means were
17-19 counts lower than later pairs without a comparable observed pin
shift. The extreme held-mode value 285 was not reproduced, so holding the
supply mode is not sufficient to explain the discrepancy. The R4.5.0 ADC
header states that `am_hal_adc_samples_read` applies offset/gain
correction; the reported numbers are HAL-corrected codes, despite earlier
informal use of "raw code." The nominal physical-pin prediction remains
about 620-621, higher than all six means.

**Status:** V3 ADC accuracy and multi-voltage calibration remain open; the
scope sample rate and aggregated means do not exclude sub-sample input
transients or reference/trim effects. Production anchors, thresholds, and
V14 energy claims are unchanged. Next isolate ordered per-read behavior
and correction trims as needed, then make controlled paired measurements
at additional stable physical voltages. Do not infer calibrated threshold
safety from these records.
