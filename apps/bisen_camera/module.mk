local_app_name := bisen_camera

# This integration is wired for the Apollo4 Plus BGA EVB (AMAP4PEVB Rev. 1).
# Fail at configuration time instead of silently compiling the camera/state-bus
# pin map for a different carrier board.
ifneq ($(PLATFORM),apollo4p_evb)
$(error apps/bisen_camera requires PLATFORM=apollo4p_evb (AMAP4PEVB Apollo4 Plus BGA EVB Rev. 1))
endif

local_src := $(wildcard $(subdirectory)/src/*.c)
local_src += $(wildcard $(subdirectory)/src/*.cc)
local_src += $(wildcard $(subdirectory)/src/bisen/*.cc)
local_src += $(wildcard $(subdirectory)/src/*.cpp)
local_src += $(wildcard $(subdirectory)/src/*.s)
local_bin := $(BINDIR)/$(subdirectory)

# App-local linker extension. It changes no other target or boot image.
LINKER_FILE := ./apps/bisen_camera/bisen_camera_checkpoint.ld

# The camera workload is externally scheduled. It owns neither the energy
# policy nor the checkpoint decision.
pp_defines += WL_EXTERNAL_DRIVER=1
pp_defines += BISEN_ENABLE_COMPUTE_WORKLOAD=1
pp_defines += BISEN_ENABLE_GPIO_INSTRUMENTATION=1

# AMAP4PEVB Rev. 1 hardware defaults:
# - physical 1 MOhm / 55.8 kOhm / 10 nF VCAP divider on J9.8/GPIO16/ADCSE3
# - one bounded camera+CNN job after reset, then BTN0 starts continuous jobs
# - two-slot MRAM recovery checkpoints with an optional fail-closed guard
# - normal sleep for photodiode settle waits; deep sleep remains opt-in
BISEN_CAMERA_ENERGY_SOURCE ?= 1
BISEN_CAMERA_VCAP_GPIO16_CONFIRMED ?= 1
# Bench fit from the physical GPIO16/ADCSE3 path and transferred divider:
#   DMM_mV = 5.635588398 * ADC_code - 76.671740
# Five 32-sample points at 5.0, 6.1, 7.5, 8.5, and 8.9 V produced
# R^2=0.999739 and 23.7 mV RMS residual. Keep integer nanovolt coefficients so
# the runtime conversion remains deterministic and floating-point-free.
BISEN_CAMERA_VCAP_CALIBRATED ?= 1
BISEN_CAMERA_VCAP_NANOVOLTS_PER_CODE ?= 5635588
BISEN_CAMERA_VCAP_OFFSET_NANOVOLTS ?= -76671740
BISEN_CAMERA_ENABLE_MRAM ?= 1
# Zero matches the MSP430 behavior: no artificial powered-session checkpoint
# cap. A nonzero research guard is supported and fails closed if reached.
BISEN_CAMERA_MAX_CHECKPOINTS ?= 0
BISEN_CAMERA_WAIT_US ?= 250000
BISEN_CAMERA_MAX_WAIT_CYCLES ?= 40
BISEN_CAMERA_AUTORUN ?= 1
BISEN_CAMERA_APITEST ?= 0
BISEN_CAMERA_SCAN_IDLE_MODE ?= 1
BISEN_CAMERA_SCAN_MAX_UNITS ?= 1
# One-shot raw-code acquisition for fitting the GPIO16/ADCSE3 conversion.
# It returns before checkpoint initialisation, camera work, or any MRAM write.
BISEN_CAMERA_VCAP_CALIBRATION_MODE ?= 0
BISEN_CAMERA_VCAP_CALIBRATION_SAMPLES ?= 32
# User-defined ADC-filter behavior for the 9 V bench point. These are not BISen
# energy thresholds: a reading at or above the cutoff is discarded, and the
# policy immediately asks for another reading. The bound prevents an actually
# over-range input from trapping the firmware forever in the ADC path.
BISEN_CAMERA_VCAP_IGNORE_AT_MV ?= 9000
BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES ?= 32

pp_defines += ES_SOURCE=$(BISEN_CAMERA_ENERGY_SOURCE)
pp_defines += BISEN_CAMERA_VCAP_GPIO16_CONFIRMED=$(BISEN_CAMERA_VCAP_GPIO16_CONFIRMED)
pp_defines += BISEN_CAMERA_VCAP_CALIBRATED=$(BISEN_CAMERA_VCAP_CALIBRATED)
pp_defines += BISEN_CAMERA_VCAP_NANOVOLTS_PER_CODE=$(BISEN_CAMERA_VCAP_NANOVOLTS_PER_CODE)
pp_defines += BISEN_CAMERA_VCAP_OFFSET_NANOVOLTS=$(BISEN_CAMERA_VCAP_OFFSET_NANOVOLTS)
pp_defines += BISEN_CAMERA_ENABLE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM)
pp_defines += BISEN_CAMERA_MAX_CHECKPOINTS=$(BISEN_CAMERA_MAX_CHECKPOINTS)
pp_defines += BISEN_CAMERA_WAIT_US=$(BISEN_CAMERA_WAIT_US)
pp_defines += BISEN_CAMERA_MAX_WAIT_CYCLES=$(BISEN_CAMERA_MAX_WAIT_CYCLES)
pp_defines += BISEN_CAMERA_AUTORUN=$(BISEN_CAMERA_AUTORUN)
pp_defines += WL_APITEST=$(BISEN_CAMERA_APITEST)
pp_defines += SCAN_IDLE_MODE=$(BISEN_CAMERA_SCAN_IDLE_MODE)
pp_defines += BISEN_CAMERA_SCAN_MAX_UNITS=$(BISEN_CAMERA_SCAN_MAX_UNITS)
pp_defines += BISEN_CAMERA_VCAP_CALIBRATION_MODE=$(BISEN_CAMERA_VCAP_CALIBRATION_MODE)
pp_defines += BISEN_CAMERA_VCAP_CALIBRATION_SAMPLES=$(BISEN_CAMERA_VCAP_CALIBRATION_SAMPLES)
pp_defines += BISEN_CAMERA_VCAP_IGNORE_AT_MV=$(BISEN_CAMERA_VCAP_IGNORE_AT_MV)
pp_defines += BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES=$(BISEN_CAMERA_VCAP_HIGH_SAMPLE_RETRIES)
pp_defines += CKPT_USE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM)
pp_defines += CKPT_MRAM_TRACE=1
pp_defines += ES_SIM_C_UF=1000.0f

bindirs   += $(local_bin)
examples  += $(local_bin)/$(local_app_name).axf
examples  += $(local_bin)/$(local_app_name).bin
mains     += $(local_bin)/src/$(local_app_name).o

$(eval $(call make-axf, $(local_bin)/$(local_app_name), $(local_src)))
