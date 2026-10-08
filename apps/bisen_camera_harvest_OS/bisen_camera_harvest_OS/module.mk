local_app_name := bisen_camera_harvest

ifneq ($(PLATFORM),apollo4p_evb)
$(error apps/bisen_camera_harvest requires PLATFORM=apollo4p_evb)
endif

local_src := $(wildcard $(subdirectory)/src/*.c)
local_src += $(wildcard $(subdirectory)/src/*.cc)
local_src += $(wildcard $(subdirectory)/src/bisen/*.cc)
local_src += $(wildcard $(subdirectory)/src/*.cpp)
local_src += $(wildcard $(subdirectory)/src/*.s)
local_bin := $(BINDIR)/$(subdirectory)
LINKER_FILE := ./apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS/bisen_camera_harvest_checkpoint.ld

# FG -> diode -> physical VCAP bank -> MP1584EN -> Apollo board.
# GPIO17 observes VCAP (BISEN_HARVEST_SUPPLY_PIN); GPIO15 remains the camera pixel ADC input.
BISEN_HARVEST_CALIBRATION_MODE ?= 1
BISEN_CAMERA_ENABLE_MRAM ?= 0
# Checkpoint-format identity: OS01, engine-r2a OS with this model/codec.
# Version this value when the model or checkpoint interpretation changes.
# Command-line overrides omit u; the pp_defines assignment below supplies it.
BISEN_HARVEST_CHECKPOINT_MAGIC ?= 0x4F533031
BISEN_HARVEST_DIVIDER_TOP_OHM ?= 390000
BISEN_HARVEST_DIVIDER_BOTTOM_OHM ?= 10000
BISEN_HARVEST_MAX_VCAP_UV ?= 8000000
BISEN_HARVEST_CAL_LOW_CODE ?= 0
BISEN_HARVEST_CAL_LOW_UV ?= 0
BISEN_HARVEST_CAL_HIGH_CODE ?= 4095
# Zero uses the nominal resistor/reference transfer. Supply measured pairs
# for the calibrated full build.
BISEN_HARVEST_CAL_HIGH_UV ?= 0

# Physical task-energy thresholds must come from MP1584/camera measurements.
BISEN_HARVEST_CRITICAL_UV ?= 0
BISEN_HARVEST_WORK100_UV ?= 0
BISEN_HARVEST_WORK500_UV ?= 0
BISEN_HARVEST_WORK1000_UV ?= 0

# Default is the fixed 390k/10k bench divider. The optional switched divider
# remains available for later fully isolated hardware.
BISEN_HARVEST_SWITCHED_DIVIDER ?= 0
BISEN_HARVEST_SENSE_ENABLE_PIN ?= 0
BISEN_HARVEST_SENSE_ACTIVE_HIGH ?= 1
BISEN_HARVEST_SENSE_SETTLE_US ?= 500
BISEN_HARVEST_PARK_ADC ?= 1
BISEN_HARVEST_MCU_LOW_POWER ?= 0
BISEN_ENABLE_STATE_DAC ?= 1
BISEN_ENABLE_SWO_LOGGING ?= 1

BISEN_CAMERA_MAX_CHECKPOINTS ?= 0
BISEN_CAMERA_WAIT_US ?= 250000
# 0 means keep sleeping and rechecking until harvest energy is sufficient.
# A bounded value is available for diagnostic runs.
BISEN_CAMERA_MAX_WAIT_CYCLES ?= 0
BISEN_CAMERA_AUTORUN ?= 1
# Dedicated RF-replay images use a durable BTN0/BTN1 controlled continuous
# session. Keep this disabled by default so existing one-shot and diagnostic
# builds do not change behavior.
BISEN_HARVEST_AUTOCONTINUOUS ?= 0
# Optional one-job, retained-SRAM readout for FG-only validation without USB.
BISEN_HARVEST_OFFLINE_VALIDATE ?= 0
BISEN_CAMERA_APITEST ?= 0
BISEN_CAMERA_SCAN_IDLE_MODE ?= 1
BISEN_CAMERA_SCAN_MAX_UNITS ?= 1
# VCAP read policy (2026-10-07; see src/power_policy.h). Defaults keep the
# single-reading stop/resume decision and a reading before every scan step.
# Candidate images pass STOP/RESUME_CONFIRM=3 and SAMPLE_EVERY_STEPS=32.
BISEN_HARVEST_STOP_CONFIRM ?= 1
BISEN_HARVEST_RESUME_CONFIRM ?= 1
BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS ?= 1
# VCAP divider ADC input: 17 = GPIO17/ADCSE2 (default since 2026-10-05),
# 16 = legacy J9.8/GPIO16/ADCSE3.
BISEN_HARVEST_SUPPLY_PIN ?= 17
# Pad the CAL_* anchors were fitted on; full builds require it to equal
# BISEN_HARVEST_SUPPLY_PIN (fit_vcap_calibration.py prints it). 0 = unset.
BISEN_HARVEST_CAL_PIN ?= 0
BISEN_TRACE_CALIBRATION_SAMPLES ?= 32

pp_defines += WL_EXTERNAL_DRIVER=1 BISEN_ENABLE_COMPUTE_WORKLOAD=1
pp_defines += BISEN_ENABLE_GPIO_INSTRUMENTATION=$(BISEN_ENABLE_STATE_DAC)
pp_defines += PP_BUS_SELFCHECK=$(BISEN_ENABLE_STATE_DAC)
pp_defines += ES_SOURCE=5 BISEN_DIRECT_VDD_POLICY=1
pp_defines += BISEN_CAMERA_ENABLE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM)
pp_defines += CKPT_MAGIC=$(BISEN_HARVEST_CHECKPOINT_MAGIC)u
pp_defines += BISEN_CAMERA_VDD_CHUNK1000_CODE=2553
pp_defines += BISEN_CAMERA_VDD_CHUNK500_CODE=2441
pp_defines += BISEN_CAMERA_VDD_CHUNK100_CODE=2333
pp_defines += BISEN_CAMERA_VDD_SLEEP_CODE=2185
pp_defines += BISEN_CAMERA_VDD_RESUME_CODE=2441
pp_defines += BISEN_CAMERA_MAX_CHECKPOINTS=$(BISEN_CAMERA_MAX_CHECKPOINTS)
pp_defines += BISEN_CAMERA_WAIT_US=$(BISEN_CAMERA_WAIT_US)
pp_defines += BISEN_CAMERA_MAX_WAIT_CYCLES=$(BISEN_CAMERA_MAX_WAIT_CYCLES)
pp_defines += BISEN_CAMERA_AUTORUN=$(BISEN_CAMERA_AUTORUN)
pp_defines += BISEN_HARVEST_AUTOCONTINUOUS=$(BISEN_HARVEST_AUTOCONTINUOUS)
pp_defines += BISEN_HARVEST_OFFLINE_VALIDATE=$(BISEN_HARVEST_OFFLINE_VALIDATE)
pp_defines += WL_APITEST=$(BISEN_CAMERA_APITEST)
pp_defines += SCAN_IDLE_MODE=$(BISEN_CAMERA_SCAN_IDLE_MODE)
pp_defines += BISEN_CAMERA_SCAN_MAX_UNITS=$(BISEN_CAMERA_SCAN_MAX_UNITS)
pp_defines += BISEN_HARVEST_STOP_CONFIRM=$(BISEN_HARVEST_STOP_CONFIRM)u
pp_defines += BISEN_HARVEST_RESUME_CONFIRM=$(BISEN_HARVEST_RESUME_CONFIRM)u
pp_defines += BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS=$(BISEN_HARVEST_VCAP_SAMPLE_EVERY_STEPS)u
pp_defines += BISEN_TRACE_CALIBRATION_MODE=$(BISEN_HARVEST_CALIBRATION_MODE)
pp_defines += BISEN_TRACE_CALIBRATION_SAMPLES=$(BISEN_TRACE_CALIBRATION_SAMPLES)
pp_defines += BISEN_HARVEST_SUPPLY_PIN=$(BISEN_HARVEST_SUPPLY_PIN)
pp_defines += BISEN_HARVEST_CAL_PIN=$(BISEN_HARVEST_CAL_PIN)
pp_defines += CKPT_USE_MRAM=$(BISEN_CAMERA_ENABLE_MRAM) CKPT_MRAM_TRACE=1
pp_defines += BISEN_HARVEST_DIVIDER_TOP_OHM=$(BISEN_HARVEST_DIVIDER_TOP_OHM)
pp_defines += BISEN_HARVEST_DIVIDER_BOTTOM_OHM=$(BISEN_HARVEST_DIVIDER_BOTTOM_OHM)
pp_defines += BISEN_HARVEST_MAX_VCAP_UV=$(BISEN_HARVEST_MAX_VCAP_UV)
pp_defines += BISEN_HARVEST_CAL_LOW_CODE=$(BISEN_HARVEST_CAL_LOW_CODE)
pp_defines += BISEN_HARVEST_CAL_LOW_UV=$(BISEN_HARVEST_CAL_LOW_UV)
pp_defines += BISEN_HARVEST_CAL_HIGH_CODE=$(BISEN_HARVEST_CAL_HIGH_CODE)
pp_defines += BISEN_HARVEST_CAL_HIGH_UV=$(BISEN_HARVEST_CAL_HIGH_UV)
pp_defines += BISEN_HARVEST_CRITICAL_UV=$(BISEN_HARVEST_CRITICAL_UV)
pp_defines += BISEN_HARVEST_WORK100_UV=$(BISEN_HARVEST_WORK100_UV)
pp_defines += BISEN_HARVEST_WORK500_UV=$(BISEN_HARVEST_WORK500_UV)
pp_defines += BISEN_HARVEST_WORK1000_UV=$(BISEN_HARVEST_WORK1000_UV)
pp_defines += BISEN_HARVEST_SWITCHED_DIVIDER=$(BISEN_HARVEST_SWITCHED_DIVIDER)
pp_defines += BISEN_HARVEST_SENSE_ENABLE_PIN=$(BISEN_HARVEST_SENSE_ENABLE_PIN)
pp_defines += BISEN_HARVEST_SENSE_ACTIVE_HIGH=$(BISEN_HARVEST_SENSE_ACTIVE_HIGH)
pp_defines += BISEN_HARVEST_SENSE_SETTLE_US=$(BISEN_HARVEST_SENSE_SETTLE_US)
pp_defines += BISEN_HARVEST_PARK_ADC=$(BISEN_HARVEST_PARK_ADC)
pp_defines += BISEN_HARVEST_MCU_LOW_POWER=$(BISEN_HARVEST_MCU_LOW_POWER)
pp_defines += BISEN_ENABLE_STATE_DAC=$(BISEN_ENABLE_STATE_DAC)
pp_defines += BISEN_ENABLE_SWO_LOGGING=$(BISEN_ENABLE_SWO_LOGGING)

# engine-r2a (2026-09-29): output-stationary, SIMD on, CMAX 6 (LeNet). See README.md.
pp_defines += NN_DATAFLOW=0 NN_SIMD=1 NN_MAX_CONV_IN_C=6

bindirs += $(local_bin)
examples += $(local_bin)/$(local_app_name).axf
examples += $(local_bin)/$(local_app_name).bin
mains += $(local_bin)/src/$(local_app_name).o

$(eval $(call make-axf, $(local_bin)/$(local_app_name), $(local_src)))
