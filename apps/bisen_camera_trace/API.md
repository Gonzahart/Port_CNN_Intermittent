# Trace-emulation API notes

The unchanged workload contract is declared in `src/workload.h`. Camera work,
exact inference, checkpoint payload selection and restore positioning are
copied from `bisen_camera_vdd` at commit 8d637afda.

| Interface | Meaning in this app |
|---|---|
| `adc_shared_read_supply(&code)` | Physical median ADCSE3 code, 0–4095; -1 on failure |
| `trace_input_microvolts(code)` | Reconstructed trace voltage BEFORE the divider, using nominal or user-calibrated transfer |
| `trace_policy_code(uv)` | Virtual code in the original direct-VDD policy scale |
| `adc_shared_supply_nominal_millivolts(code)` | Inherited API name; reconstructed trace mV, not physical VDD |
| `es_read(&reading)` | Imposed-energy observation; `simulated=1`, `raw_code` contains the virtual policy code |
| `pp_adc_code()` | Last virtual policy code (not a physical BATT/SE3 reading) |
| `pp_vcap_mv()` / `wl_supply_mv(&mv)` | Imposed trace voltage in mV |
| `es_write_fits(us)` | No modeled deadline; returns true. Independent DC power provides the write supply |

Calibration logs' `code_mean`/`code_min`/`code_max` are physical ADCSE3 readings.
Runtime `policy_code` is virtual. No claim of real energy reserve is made.

The policy and scheduler make the same work, stop, dirty-write and resume
choices as the source app for an equivalent policy code. Only the measurement
adapter determines the policy code from the external trace.
