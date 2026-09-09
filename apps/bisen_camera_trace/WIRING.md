# Wiring: DC-powered camera/CNN with an FG energy-control signal

Target: **AMAP4PEVB Apollo4 Plus BGA EVB Rev. 1**, application
**bisen_camera_trace**, platform **apollo4p_evb**.

This guide changes the source of the energy measurement. The FG never connects
to VDD_EXT or supplies camera/MCU power in this arrangement.

## Parts

- Existing WPS3010H DC supply, existing series diode and 1000 uF reservoir.
- Siglent SDG1032X, CH1, DC waveform, load display **Hi-Z**.
- Three **10 kohm, 1%** resistors (two in series form the 20 kohm upper leg).
- One **10 nF ceramic** capacitor across the lower divider resistor.
- Common ground connections, DMM and scope with high-impedance inputs.

Use these divider values for the nominal firmware transfer. The old
1 Mohm / 55.8 kohm divider is not the divider specified for this app.

## Connections with outputs disabled

Disable the FG and bench output. Disconnect both board USB cables. Let the
reservoir discharge before moving wires. Remove the FG connection from the
old diode/capacitor power input.

Keep the proven board power path, now driven only by the DC supply:

```text
WPS3010H + ---- diode anode --|>|-- diode cathode (striped end) --+-- J7.3 VDD_EXT
                                                             |
                                                      1000 uF capacitor
                                                             |
WPS3010H - ---------------------------------------------------+-- J3.8 GND
```

Capacitor positive goes to the diode cathode/J7.3 node; its negative goes to
ground. The existing camera supply wiring stays on the board-powered rail.
Keep J3.3–J3.4 jumpered and the solder bridges in their present configuration.
No LM2596 or additional regulator is needed for this fixed low-voltage supply.

Wire the independent trace signal:

```text
SDG CH1 center/+ -- 10k -- 10k --+---------------- J9.8 / GPIO16 / ADCSE3
                               |
                               +-- 10k ---------- common ground
                               |
                               +-- 10 nF -------- common ground

SDG CH1 shield/- -------------------------------- common ground / J3.8
```

There is **no diode and no 1000 uF capacitor in the trace-signal branch**.
The 10 nF capacitor is a small ADC input filter, not an energy reservoir.
Place the lower resistor and filter close to the ADC connection.

| Connection | Exact destination |
|---|---|
| DC supply positive | Existing diode anode |
| Diode cathode and reservoir positive | J7.3, VDD_EXT |
| DC return, reservoir negative and FG return | Common ground at J3.8 |
| FG CH1 center conductor | Top of the two series 10k resistors |
| Divider junction | J9.8, GPIO16/ADCSE3 |
| Lower 10k and 10 nF returns | Common ground |
| Camera pixel wire | Leave on J9.10, GPIO15/ADCSE4 |
| Actual MCU voltage measurement | J7.1 or J7.2, VDD_MCU |
| State DAC | Existing common weighted-resistor node |

J9.8 and J9.10 are adjacent even-numbered contacts on the same header. Use the
board's pin-1 marking and the established header orientation; do not substitute
J9.7 or J9.9. Do not connect the FG center conductor directly to GPIO16.

## Voltage limits and first electrical check

1. Leave the divider-junction lead disconnected from J9.8 initially. Grounds
   may be connected. Keep FG output disabled.
2. Turn on the DC supply and board POWER-ON switch. Adjust the supply until
   **J7.1/J7.2 measures about 2.10 V**. Do not exceed **2.20 V actual MCU VDD**.
   The earlier diode-path measurements suggest around 2.36 V at the supply,
   but that is a starting estimate, not a guaranteed setpoint.
3. Configure FG CH1 for **DC**, **Hi-Z**, **0 V**. Set channel inversion to normal.
4. With the divider still disconnected from the ADC, enable FG and measure
   the divider junction: at 2.10 V measured FG output it should be about
   **0.700 V**; at 2.40 V it should be about **0.800 V**. Confirm both the
   resistor ratio and actual voltage, not only the display setting.
5. Return FG to 0 V and disable its output. Connect the divider junction to
   J9.8 while the board remains powered.
6. Operate the FG only over **0–2.40 V measured at the divider input** for this
   guide. The ADC node should remain **0–0.80 V nominal** (allow small resistor
   tolerance); stop if the wiring does not produce that ratio. The ADC's
   normal conversion range is 0–1.19 V. Avoid negative waveforms.
7. Disable FG output before switching off the DC supply or unplugging board
   power. The board must be powered whenever the external signal is applied.

The firmware assumes a 3:1 divider, so **2.10 V at the FG input to the divider
means 2.10 V emulated energy voltage**, while the ADC pin sees about 0.70 V.
The independent board supply stays near 2.10 V even when the trace is 0 V or
2.30 V. A 2.30 V trace does not mean the MCU is powered at 2.30 V.

Do not use the old FG-to-loaded-VDD calibration CSVs. Those describe a different
power circuit. Hi-Z does not remove the FG's 50 ohm source impedance, but this
30 kohm divider loads it only lightly (about 80 uA at 2.4 V).

## Firmware and first functional capture

Build/deploy commands are in README.md. Flash only after the wiring above is
ready. During flashing, keep external board VDD stable and FG off. Connect
J-Link temporarily for deployment/banner verification; disconnect both USB
cables before the standalone capture. Recheck the actual MCU rail after USB
is removed.

The banner must say `EXTERNAL TRACE EMULATION`, `J9.8/GPIO16/ADCSE3`, and
`two-slot app-local MRAM`. For the full app it must not say `CALIBRATION MODE`.

1. Set FG to **2.15 V DC** and enable output. Reset starts one bounded job.
   At default nominal calibration this selects the 500-unit band. Let the job
   complete and park. If reset occurred while FG was off, the initial bounded
   wait may already have expired; pressing BTN0 after parking starts continuous
   work without needing to reset again.
2. Start the scope, then press **BTN0/SW1** after parking for continuous jobs.
3. Use **2.25 V**, **2.15 V** and **2.05 V** to exercise the 1000-, 500- and
   100-unit bands, respectively. These are interior test points; exact
   crossings require external-input calibration.
4. While work is active, reduce FG to **1.95 V**. Expect a dirty-work checkpoint
   (state 4 then 5) followed by state 0 with periodic state 1 observations.
   Holding it low must not repeatedly commit unchanged progress.
5. Return FG to **2.15 V**. Expect retained-SRAM continuation without state 6
   because physical board power never failed.
6. A **1.85 V** signal exercises the low-floor classification. Even a **0 V**
   signal leaves the MCU powered; it does not generate a hardware brownout.

State 5 can also be a completed job's retirement record. Use surrounding
states and, if needed, firmware logs to distinguish it from a dirty checkpoint.
No inference budget is encoded separately on the DAC: code 3 represents all
CNN budgets. Confirm budget selection through the known input band/logs.

## Oscilloscope

| Suggested channel | Probe tip |
|---|---|
| CH1 | FG output / top of divider (imposed energy envelope) |
| CH2 | Existing weighted state-DAC node |
| CH4 | J7.1 or J7.2 (actual MCU VDD, should stay steady) |
| Optional fourth channel | J9.8 divider node |

All scope grounds go to common ground. Use DC coupling, high-impedance inputs
and preferably 10x probes with matching probe settings. Start with 1–5 s/div
for manual steps, then use a long memory capture for replay. Capture narrow
ADC/MRAM events at adequate sample rate; the displayed overview may hide them.
On this EVB the state bus is GPIO62/63/61 at J12.7/.9/.11 through the existing
approximately 99.3k/201k/398k resistors. Preserve those associations.

## Replay an existing trace

For an automatic version of the manual band test, the app includes
`traces/band_steps.txt`: 2.25, 2.15, 2.05, 1.95, 1.95, 2.15, 1.85, 2.15 V.
The repeated 1.95 V level allows observation of a clean held-low state.
Each step in this command lasts two seconds:

```sh
python3 /Users/ghart/VSC/Image_filter/sdg1032x_charge_loop_modified.py trace-loop \
  --resource 'USB0::0xF4EC::0x1103::SDG1XDDQ802757::INSTR' \
  --trace-file /Users/ghart/Documents/Ambiq/neuralSPOT/apps/bisen_camera_trace/traces/band_steps.txt \
  --trace-output-mode dc --load HZ --offset 0 \
  --trace-scale-mode raw --trace-max-vpp 2.30 --trace-min-vpp 0 \
  --steps 8 --charge-s 16 --rest-s 2 --cycles 2 --off-below-vpp 0 \
  --log-csv /Users/ghart/workspace_v12/Image_Sobel_5969/trace_band_steps_fg_log.csv \
  --dry-run
```

Remove `--dry-run` only after the divider and stable board supply checks.

The existing MSP430 script has a DC-envelope mode and does not need the
loaded-VDD calibration. The following is an **initial functional test**, using
an explicit amplitude/time scale, not a claim to reproduce original RF power.
For this first test, map the downsampled file's minimum/maximum linearly to
**1.90–2.30 V** so the envelope traverses the policy bands. This adds an offset
and changes the amplitude scale; it is intentionally a functional emulation.
Zeros in the file therefore map to a nonzero control voltage; the between-cycle
output-off period still gives a zero control input. Record this transformation
alongside results.
Review it first with `--dry-run` (this prints commands and still follows timing):

```sh
python3 /Users/ghart/VSC/Image_filter/sdg1032x_charge_loop_modified.py trace-loop \
  --resource 'USB0::0xF4EC::0x1103::SDG1XDDQ802757::INSTR' \
  --trace-file /Users/ghart/VSC/Image_filter/charge_trace2.txt \
  --trace-output-mode dc --load HZ --offset 0 \
  --trace-scale-mode normalized \
  --trace-max-vpp 2.30 --trace-min-vpp 1.90 \
  --steps 200 --charge-s 30 --rest-s 2 --cycles 5 \
  --off-below-vpp 0 \
  --log-csv /Users/ghart/workspace_v12/Image_Sobel_5969/trace_emulation_fg_log.csv \
  --dry-run
```

Offline preview of the specified `charge_trace2.txt` command gives 74 wait,
121 low-budget, 3 mid-budget and 2 high-budget steps out of 200, before ADC
calibration and timing effects. The earlier zero-based, 99th-percentile scaling
to 2.30 V gives 195 wait steps and only 5 work-allowed steps; use that version
only when such sparse activity is intended. To request it, replace the scaling
options with `--trace-scale-mode max-vpp --trace-percentile 99
--trace-min-vpp 0 --trace-max-vpp 2.30`.

After the voltage checks, ensure the app is in continuous mode (BTN0 after
parking). Remove only `--dry-run` to replay. The script switches the output off
at/below zero and in rest periods; the lower divider resistor pulls the ADC
node to ground. The default wait polling interval is 250 ms, so narrow trace
excursions can be missed while waiting. For initial validation, use features
lasting at least roughly one second; later preserve the original playback
settings and document any timing changes. `--charge-s` sets the duration of the
whole file, not its original acquisition sample period.

If using the FG manually, return it to 2.15 V for the initial job/BTN0 setup
before handing control to the script. Never connect the FG back to the power
reservoir while this wiring is in use.

## Sources and remaining validation

Pin assignments and the power path are the experimentally established Rev. 1
connections in `handover2.md` sections 5, 7 and 10, corroborated by the existing
`bisen_camera` ADCSE3 implementation and installed AmbiqSuite R4.5.0 GPIO mux
(`AM_HAL_PIN_16_ADCSE3`). This is not a new solder-bridge configuration.

The [Apollo4 Plus datasheet](https://ambiq.com/wp-content/uploads/2022/03/Apollo4-Plus-SoC-Datasheet.pdf),
Table 41, specifies the 0-to-reference ADC input range and nominal 1.19 V
reference. The publicly linked
[EVB schematic](https://ambiq.com/wp-content/uploads/2022/11/Apollo4-Plus-EVB-Schematic.pdf)
currently identifies Rev. 2; this guide uses the user's validated Rev. 1 wiring
rather than assuming that schematic's power configuration applies unchanged.

This new circuit and app have been compiled and host-tested, but not bench
validated. Verify physical voltage ratios and capture state behavior before
using the results as experimental evidence.
