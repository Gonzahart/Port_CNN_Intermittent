# BISen camera trace emulation — AMAP4PEVB Rev. 1

`bisen_camera_trace` is a separate application derived from `bisen_camera_vdd`
at neuralSPOT commit `8d637afdab016b3e1008d6d1947c0d634bf94175`.
The DC supply powers the board and camera continuously. The Siglent supplies
only an external analog signal representing available energy.

Read [WIRING.md](WIRING.md) before connecting the trace input.

## What changes

- GPIO16/ADCSE3 at J9.8 measures a 20 kohm / 10 kohm trace divider.
- ADC0 still has one owner, sharing the trace input with the camera on GPIO15/SE4.
- `trace_input.h` reconstructs the pre-divider voltage, then maps 1.90, 2.00,
  2.10 and 2.20 V to virtual policy codes 2185, 2333, 2441 and 2553.
- `bisen_policy.cc` and the scheduler's work/checkpoint logic are copied unchanged.
  Exact CNN math, one-pixel sensing boundaries, 100/500/1000-unit inference
  budgets, dirty falling-edge checkpoints, retirement and SRAM resume remain.
- Logs explicitly distinguish `policy_code` (virtual) from `code_mean` (physical
  ADCSE3 code in calibration mode). `trace_mV` is the imposed input voltage,
  NOT the physical MCU supply. Internal BATT is not sampled in this app.
- Checkpoint magic is `TRC1`, different from the physical-energy apps. Calibration
  SRAM has a distinct identity too. This avoids adopting their old records.
- ADC scan-completion polling has a 1 ms STIMER timeout per trigger in addition
  to the inherited 24-trigger AVG16 bound. No ADC completion ISR was introduced.

This app has its own source, module and linker extension. It does not link or
edit `bisen_camera_vdd`, `bisen_camera`, `bisen_port`, the bootloader or
`basic_tf_stub`. Flashing any application replaces the currently running app;
separate source directories do not create simultaneously installed firmwares.
MRAM checkpoint addresses depend on image layout. Do not expect checkpoints to
survive switching applications or rebuilding with different options.

## Build and deploy

From `/Users/ghart/Documents/Ambiq/neuralSPOT`:

```sh
make -B EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
make -B deploy EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
make view EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0
```

The startup banner must say `EXTERNAL TRACE EMULATION` and identify
`J9.8/GPIO16/ADCSE3`. Default firmware is the full camera/CNN/MRAM application.

Default behavior matches the current app: reset starts one bounded job and
then parks; BTN0 after parking enters continuous jobs. A bounded energy wait
is 40 polls of 250 ms, approximately 10 seconds plus overhead. In the initial
bounded job, expiry parks until BTN0. Continuous mode retries the retained job
after such an expiry. A trace dropping to zero does not reset the MCU.

## Input transfer and policy

Default calibration is nominal: 12-bit ADC, 1.190 V reference, 3:1 divider.

```text
trace_uV = ADCSE3_code * 3570000 / 4096
trace_mV = trace_uV / 1000
```

Piecewise interpolation in `trace_input.h` maps that voltage onto the existing
policy's code scale. It uses integer truncation so a sample below an anchor is
not rounded up into its band. Above 2.20 V trace input, the virtual code saturates
at the highest band. This numerical saturation is not electrical protection.
The original BATT calibration is NOT reused as an external ADC calibration.

| Trace input, before divider | Policy code at boundary | Decision |
|---|---:|---|
| 2.20 V or higher | 2553 | 1000 CNN units |
| 2.10 to below 2.20 V | 2441 | 500 units; restore/restart permitted |
| 2.00 to below 2.10 V | 2333 | 100 units |
| Below 2.00 V | below 2333 | Wait; checkpoint dirty work once on a falling transition |
| Below 1.90 V | below 2185 | Low-power-floor classification |

Normal SRAM continuation needs the compute-allowed boundary, 2.00 V; work
restored from MRAM additionally needs 2.10 V. This preserves the source
scheduler's distinction. Initial fresh work also uses its existing 2.00 V rule.
There is no new policy hysteresis.

## Calibration diagnostic

To collect physical ADCSE3 readings without executing jobs or writing MRAM:

```sh
make -B deploy EXAMPLE=bisen_camera_trace PLATFORM=apollo4p_evb AS_VERSION=R4.5.0 \
  BISEN_TRACE_CALIBRATION_MODE=1 BISEN_CAMERA_ENABLE_MRAM=0
```

With the board still powered by the DC supply, disconnect debug USB, set a
stable FG level and measure its actual voltage at the divider input. BTN0
captures 32 readings to retained SRAM. Capture two separated nonzero points,
for example near 1.80 V and 2.30 V; avoid confusing them with accidental
repeated button presses. Reconnect J-Link and use BTN1 with the viewer to print
the records. `code_mean` is the real ADC code. Maintain DC board power throughout.

For the two DMM-measured input voltages and corresponding mean ADC codes, use:

```text
BISEN_TRACE_CAL_LOW_CODE  = measured lower-point code
BISEN_TRACE_CAL_HIGH_CODE = measured upper-point code
BISEN_TRACE_CAL_LOW_UV    = measured lower-point voltage in microvolts
BISEN_TRACE_CAL_HIGH_UV   = measured upper-point voltage in microvolts
```

The checked-in defaults are the 2026-09-09 board calibration: code 2041 at
1.803 V and code 2609 at 2.300 V. Command-line values can still override them
for another board or divider.

For this board, subsequent `make -B` / `make -B deploy` commands can use those
defaults. Pass all four assignments only when overriding the calibration for
another board or divider. Use `BISEN_TRACE_CALIBRATION_MODE=0` and
`BISEN_CAMERA_ENABLE_MRAM=1` for normal operation. The input is linearly extrapolated outside the
calibration pair and clamped to zero for negative reconstructed voltage.
Check intermediate points near all four thresholds before relying on precise
crossing voltages. Do not adjust the virtual policy-code constants to calibrate
this sensing path; the build rejects that mismatch.

## Validation scope

This setup exercises real camera work, exact inference, checkpoint decisions,
actual MRAM transactions and SRAM resume under an imposed energy signal. It
does not measure harvested energy, workload-induced capacitor discharge,
brownout margin, or whether MRAM writes would finish before a real power loss.
A separate reset can exercise software restore; real power interruption remains
a separate experiment. The source does not model a capacitor or subtract camera
and CNN consumption from the FG signal.

Host tests compile the actual policy with the new input adapter:

```sh
c++ -std=c++11 -Wall -Wextra -Werror \
  -DBISEN_DIRECT_VDD_POLICY=1 -DBISEN_ENABLE_COMPUTE_WORKLOAD=1 \
  -Iapps/bisen_camera_trace/src \
  apps/bisen_camera_trace/tests/trace_policy_test.cc \
  apps/bisen_camera_trace/src/bisen/bisen_policy.cc \
  -o /tmp/bisen_trace_policy_test
/tmp/bisen_trace_policy_test
```

See [VALIDATION.md](VALIDATION.md) for checks actually performed. Bench operation
of this new app still requires the wiring and captures described in WIRING.md.
