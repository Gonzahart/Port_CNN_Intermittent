# Peripheral and storage ownership

This file describes the `apollo4p_evb` / AMAP4PEVB Rev. 1 integration. The
board mapping is sourced from AmbiqSuite R4.5.0's `apollo4p_evb` BSP and the
official Apollo4 Plus EVB schematic linked from `README.md`.

## ADC0

Apollo4 Plus has one general-purpose ADC. `src/adc_shared.c` is its only owner
and keeps the handle for the lifetime of the app. It switches between mutually
exclusive configurations:

```text
ADC_MODE_PIXEL  slot 0 = J9.10/GPIO15/ADCSE4 photodiode, LPMODE0
ADC_MODE_VCAP   slot 1 = J9.8/GPIO16/ADCSE3 divider,     LPMODE1
ADC_MODE_WATCH  slot 1 = GPIO16/ADCSE3 + window comparator
```

Only one slot is enabled at a time, so camera conversions cannot be mistaken
for VCAP conversions and VCAP monitoring remains available while compute is
stopped. The 10 nF divider filter settles once at startup. Both paths use the
24 MHz HFRC and 63 tracking cycles required by Apollo4 ADC errata ERR091 and
ERR113.

The VCAP conversion is board-specific. The old Blue KXR calibration is not
valid here because that path included a different GPIO and an additional board
pulldown. AMAP4PEVB defaults to the nominal direct-divider coefficient and
prints an explicit uncalibrated warning until new GPIO16/SE3 DMM data is fitted.

## Camera GPIOs

`src/sensor.c` owns the supplied CNN project's established connections:

```text
columns  GPIO96,95,98,99,102   J9.1,.3,.5,.7,.9
rows     GPIO9,8,10,11,91      J11.1,.3,.5,.7,.11
CS/WR/EN GPIO36,35,34          J9.11,.13,.15
OUTCOL   GPIO100               J7.13
pixel    GPIO15/ADCSE4         J9.10
LEDs     GPIO90,30,97          existing EVB LEDs
```

## BISen GPIOs

`src/bisen/bisen_instrumentation.cc` owns GPIO62/63/61. On AMAP4PEVB Rev. 1
these are J12.7/.9/.11, not the J9 locations used on the Blue KXR board. The
three-bit code remains:

```text
0 sleep/inactive     1 VCAP ADC       2 camera/sense
3 CNN compute        4 MRAM write     5 checkpoint committed
6 context restore    7 boot/error
```

BTN0 is the BSP's GPIO18 (onboard SW1 and J9.4). The app uses the BSP symbol,
not a hardcoded Blue KXR button number.

## MRAM

`bisen_camera_checkpoint.ld` reserves two app-local 8,080-byte checkpoint slots
after the linked image as a `NOLOAD` region. `ckpt.c` and `ckpt_mram.c` are the
only writers. The secure bootloader and `apps/bisen_port` regions are not
modified by this app's source or linker extension.

Checkpoint writes occur only on an allowed-to-wait transition when coherent
progress has advanced the current job's runtime generation beyond its last
committed generation. A completed CNN frame does not persist its result. If it
has a live recovery checkpoint, it commits one header-only retirement
tombstone; otherwise completion writes nothing. The default has no artificial
powered-session limit, matching the MSP430. If a nonzero research guard is
configured, reaching it fails closed.

Restore never programs MRAM. CRC-failed slots are excluded in RAM while the
older slot is considered, so cold-boot fallback cannot create an unqualified
write or consume endurance.

## Board-target boundary

`module.mk` rejects any platform other than `apollo4p_evb`. This prevents the
AMAP4PEVB header map from being silently compiled for `apollo4p_blue_kxr_evb`,
where several of the same GPIO numbers have different board-level loading and
header labels.
