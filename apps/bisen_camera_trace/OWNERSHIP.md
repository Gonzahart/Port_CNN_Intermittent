# Peripheral and storage ownership

`bisen_camera_trace` owns ADC0 through `src/adc_shared.c` only. Slot 0 samples
camera GPIO15/ADCSE4; slot 1 samples trace GPIO16/ADCSE3. Only one slot is active
at a time. Both use AVG16, 12-bit precision, 24 MHz HFRC and 63 tracking cycles.
A trace decision discards one conversion and takes the median of three.
The decision path uses bounded software-trigger polling. No BATT measurement
or real board-power estimate is used. Timed energy waits retain the original
scheduler/power implementation; the external trace source does not arm the
legacy hardware-window path.

`energy_source.cc` converts the physical ADC code through `trace_input.h` into
an imposed voltage and a virtual direct-VDD policy code. `bisen_policy.cc`
selects the bands. `bisen_camera_trace.cc` owns scheduling, one-time dirty
checkpoints, waiting, restore and retirement. Camera and CNN code are unchanged.

GPIO16/J9.8 is newly used by this app. The camera and DAC pins are listed in
WIRING.md. No pin is shared between the trace and the photodiode.

The default build enables the copied two-slot MRAM backend in an app-local
NOLOAD region (16160 bytes). Checkpoint magic TRC1 distinguishes this app's
records. Different apps share the physical MCU application address space when
flashed; this source-level isolation does not preserve other images or their
checkpoints on the board.

Calibration mode returns before checkpoint initialization and requires MRAM
disabled. It keeps up to 16 readings in retained SRAM using the distinct TRCC
log identity. BTN0 captures and BTN1 prints. Actual board power loss invalidates
that volatile log.
