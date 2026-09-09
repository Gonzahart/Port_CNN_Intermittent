local_app_name := bisen_camera_trace

# This integration is wired for the Apollo4 Plus BGA EVB (AMAP4PEVB Rev. 1).
# Fail at configuration time instead of silently compiling the camera/state-bus
# pin map for a different carrier board.
ifneq ($(PLATFORM),apollo4p_evb)
$(error apps/bisen_camera_trace requires PLATFORM=apollo4p_evb (AMAP4PEVB Apollo4 Plus BGA EVB Rev. 1))
endif

local_src := $(wildcard $(subdirectory)/src/*.c)
local_src += $(wildcard $(subdirectory)/src/*.cc)
local_src += $(wildcard $(subdirectory)/src/bisen/*.cc)
local_src += $(wildcard $(subdirectory)/src/*.cpp)
local_src += $(wildcard $(subdirectory)/src/*.s)
local_bin := $(BINDIR)/$(subdirectory)

# App-local linker extension. It changes no other target or boot image.
LINKER_FILE := ./apps/bisen_camera_trace/bisen_camera_trace_checkpoint.ld

# The camera workload is externally scheduled. It owns neither the energy
# policy nor the checkpoint decision.
pp_defines += WL_EXTERNAL_DRIVER=1
pp_defines += BISEN_ENABLE_COMPUTE_WORKLOAD=1
pp_defines += BISEN_ENABLE_GPIO_INSTRUMENTATION=1

# Physical board power is independent. Only ADCSE3 drives emulated energy.
BISEN_CAMERA_ENERGY_SOURCE := 4
BISEN_CAMERA_ENABLE_MRAM ?= 1
# Direct-VDD raw-code anchors from the offline 32-sample calibration:
#   DMM 2.20 V -> mean code 2553 (1000-unit band)
#   DMM 2.10 V -> mean code 2441 (500-unit band and restart/restore)
#   DMM 2.00 V -> mean code 2333 (100-unit band; falling below checkpoints)
#   DMM 1.90 V -> mean code 2185 (low-power wait floor)
BISEN_CAMERA_VDD_CHUNK1000_CODE ?= 2553
BISEN_CAMERA_VDD_CHUNK500_CODE ?= 2441
BISEN_CAMERA_VDD_CHUNK100_CODE ?= 2333
BISEN_CAMERA_VDD_SLEEP_CODE ?= 2185
BISEN_CAMERA_VDD_RESUME_CODE ?= 2441
# Zero matches the MSP430 behavior: no artificial powered-session checkpoint
# cap. A nonzero research guard is supported and fails closed if reached.
BISEN_CAMERA_MAX_CHECKPOINTS ?= 0
BISEN_CAMERA_WAIT_US ?= 250000
BISEN_CAMERA_MAX_WAIT_CYCLES ?= 40
BISEN_CAMERA_AUTORUN ?= 1
BISEN_CAMERA_APITEST ?= 0
BISEN_CAMERA_SCAN_IDLE_MODE ?= 1
BISEN_CAMERA_SCAN_MAX_UNITS ?= 1
# Set to 1 only to return to the bounded, MRAM-off calibration diagnostic.
BISEN_TRACE_CALIBRATION_MODE ?= 0
BISEN_TRACE_CALIBRATION_SAMPLES ?= 32

pp_defines += ES_SOURCE=$(BISEN_CAMERA_ENERGY_SOURCE)
pp_defines += BISEN_CAMERA_ENABLE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM)
pp_defines += BISEN_DIRECT_VDD_POLICY=1
pp_defines += BISEN_CAMERA_VDD_CHUNK1000_CODE=$(BISEN_CAMERA_VDD_CHUNK1000_CODE)
pp_defines += BISEN_CAMERA_VDD_CHUNK500_CODE=$(BISEN_CAMERA_VDD_CHUNK500_CODE)
pp_defines += BISEN_CAMERA_VDD_CHUNK100_CODE=$(BISEN_CAMERA_VDD_CHUNK100_CODE)
pp_defines += BISEN_CAMERA_VDD_SLEEP_CODE=$(BISEN_CAMERA_VDD_SLEEP_CODE)
pp_defines += BISEN_CAMERA_VDD_RESUME_CODE=$(BISEN_CAMERA_VDD_RESUME_CODE)
pp_defines += BISEN_CAMERA_MAX_CHECKPOINTS=$(BISEN_CAMERA_MAX_CHECKPOINTS)
pp_defines += BISEN_CAMERA_WAIT_US=$(BISEN_CAMERA_WAIT_US)
pp_defines += BISEN_CAMERA_MAX_WAIT_CYCLES=$(BISEN_CAMERA_MAX_WAIT_CYCLES)
pp_defines += BISEN_CAMERA_AUTORUN=$(BISEN_CAMERA_AUTORUN)
pp_defines += WL_APITEST=$(BISEN_CAMERA_APITEST)
pp_defines += SCAN_IDLE_MODE=$(BISEN_CAMERA_SCAN_IDLE_MODE)
pp_defines += BISEN_CAMERA_SCAN_MAX_UNITS=$(BISEN_CAMERA_SCAN_MAX_UNITS)
pp_defines += BISEN_TRACE_CALIBRATION_MODE=$(BISEN_TRACE_CALIBRATION_MODE)
pp_defines += BISEN_TRACE_CALIBRATION_SAMPLES=$(BISEN_TRACE_CALIBRATION_SAMPLES)
pp_defines += CKPT_USE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM)
pp_defines += CKPT_MRAM_TRACE=1

bindirs   += $(local_bin)
examples  += $(local_bin)/$(local_app_name).axf
examples  += $(local_bin)/$(local_app_name).bin
mains     += $(local_bin)/src/$(local_app_name).o


# Optional two-point calibration of the new external sensing path.
BISEN_TRACE_CAL_LOW_CODE ?= 0
BISEN_TRACE_CAL_HIGH_CODE ?= 4096
BISEN_TRACE_CAL_LOW_UV ?= 0
BISEN_TRACE_CAL_HIGH_UV ?= 3570000
pp_defines += BISEN_TRACE_CAL_LOW_CODE=$(BISEN_TRACE_CAL_LOW_CODE)
pp_defines += BISEN_TRACE_CAL_HIGH_CODE=$(BISEN_TRACE_CAL_HIGH_CODE)
pp_defines += BISEN_TRACE_CAL_LOW_UV=$(BISEN_TRACE_CAL_LOW_UV)
pp_defines += BISEN_TRACE_CAL_HIGH_UV=$(BISEN_TRACE_CAL_HIGH_UV)

$(eval $(call make-axf, $(local_bin)/$(local_app_name), $(local_src)))
