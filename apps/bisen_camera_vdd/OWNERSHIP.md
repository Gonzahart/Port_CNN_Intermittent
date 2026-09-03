# Peripheral and storage ownership

This file describes the initial `bisen_camera_vdd` milestone for
`apollo4p_evb` / AMAP4PEVB Rev. 1.

## ADC0

Apollo4 Plus has one general-purpose ADC. `src/adc_shared.c` remains its only
owner and switches between mutually exclusive slot configurations:

```text
ADC_MODE_PIXEL   slot 0 = J9.10/GPIO15/ADCSE4 photodiode, LPMODE0
ADC_MODE_SUPPLY  slot 1 = internal BATT (VDD_MCU/3),       LPMODE1
ADC_MODE_WATCH   reserved for a later measured policy milestone
```

The direct-VDD measurement uses no external ADC pin, so J9.8/GPIO16/ADCSE3 and
the previous VCAP divider are not part of this app. The code restores
`ADC_MODE_PIXEL` after every supply sample.

## Camera and state GPIOs

The copied camera integration keeps the verified AMAP4PEVB Rev. 1 mapping:

```text
columns  GPIO96,95,98,99,102   J9.1,.3,.5,.7,.9
rows     GPIO9,8,10,11,91      J11.1,.3,.5,.7,.11
CS/WR/EN GPIO36,35,34          J9.11,.13,.15
OUTCOL   GPIO100               J7.13
pixel    GPIO15/ADCSE4         J9.10
state    GPIO62,63,61          J12.7,.9,.11
BTN0     GPIO18                onboard SW1 / J9.4
```

The initial diagnostic only drives boot, ADC, and parked state instrumentation;
it does not run a camera job.

## MRAM

The stage-1 build sets `CKPT_USE_MRAM=0` and returns from the one-shot
diagnostic before `ckpt_init()`. There are no checkpoint restores, retirement
records, or MRAM program operations in this milestone. The app-local linker
extension remains isolated from the existing applications so checkpointing can
be introduced only after VDD policy thresholds are measured and approved.
