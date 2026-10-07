local_app_name := bisen_capuchin

ifneq ($(PLATFORM),apollo4p_evb)
$(error apps/bisen_capuchin requires PLATFORM=apollo4p_evb)
endif

# ---------------------------------------------------------------- options
# BENCH_MODE      0 bench (self-check + A/B timing) | 1 energy loop | 2 intermittent
# BENCH_ENGINE    modes 1/2: 0 Capuchin | 1 RUIC engine (no checkpoints: ablation)
# CAPUCHIN_KERNEL 1 LEA path on TI DSPLib's generic (non-LEA) C (default; equal to the
#                 native LEA result only once gate X1 passes)
#                 2 LEA path on CMSIS-DSP arm_mat_mult_q15 (Cortex-M4 DSP extension)
#                 0 upstream non-MSP C path
# CAPUCHIN_MODEL_IN_SRAM  0 MODEL_ARRAY const in MRAM (as the RUIC weights) | 1 SRAM ablation
# CAPUCHIN_LAYER_PROFILE  1 adds the per-layer hook (mode 0 prints a breakdown)
BENCH_MODE ?= 0
BENCH_ENGINE ?= 0
BENCH_REPS ?= 10
BENCH_PRINT_ALL ?= 0
BENCH_LOOP_COUNT ?= 200
BENCH_IDLE_MS ?= 1000
BENCH_EVENT_HOLD_US ?= 20
BENCH_STATE_DAC ?= 1
BENCH_MCU_LOW_POWER ?= 0
CAPUCHIN_KERNEL ?= 1
CAPUCHIN_MODEL_IN_SRAM ?= 0
CAPUCHIN_LAYER_PROFILE ?= 0
# [PORT P1] DMA block copy as: 1 inline word loop (default) | 0 memcpy
CAPUCHIN_COPY_LOOP ?= 1

# The RUIC engine is compiled from the OS package, unmodified, with its own flags.
RUIC_ENGINE_DIR ?= apps/bisen_camera_harvest_OS/bisen_camera_harvest_OS/src
RUIC_ENGINE_FLAGS := -DNN_DATAFLOW=0 -DNN_SIMD=1 -DNN_MAX_CONV_IN_C=6

# ---------------------------------------------------------------- sources
cap_dir := $(subdirectory)/src/capuchin
port_dir := $(subdirectory)/src/port

local_src := $(subdirectory)/src/bisen_capuchin.cc
local_src += $(subdirectory)/src/ruic_bench.c
local_src += $(cap_dir)/decoder/decoder.c
local_src += $(cap_dir)/layers/layers.c
local_src += $(cap_dir)/math/fixed_point_ops.c
local_src += $(cap_dir)/math/matrix_ops.c
local_src += $(cap_dir)/capuchin_invoke.c
local_src += $(port_dir)/capuchin_port.c
local_src += $(port_dir)/dsplib_sw.c
local_src += $(port_dir)/dsplib_cmsis.c
local_src += $(RUIC_ENGINE_DIR)/nn_engine.c
local_src += $(RUIC_ENGINE_DIR)/nn_kernels.c
local_bin := $(BINDIR)/$(subdirectory)

includes_api += $(subdirectory)/src

pp_defines += BENCH_MODE=$(BENCH_MODE) BENCH_ENGINE=$(BENCH_ENGINE) BENCH_REPS=$(BENCH_REPS)
pp_defines += BENCH_PRINT_ALL=$(BENCH_PRINT_ALL) BENCH_LOOP_COUNT=$(BENCH_LOOP_COUNT)
pp_defines += BENCH_IDLE_MS=$(BENCH_IDLE_MS) BENCH_EVENT_HOLD_US=$(BENCH_EVENT_HOLD_US)
pp_defines += BENCH_STATE_DAC=$(BENCH_STATE_DAC) BENCH_MCU_LOW_POWER=$(BENCH_MCU_LOW_POWER)
pp_defines += CAPUCHIN_KERNEL=$(CAPUCHIN_KERNEL) CAPUCHIN_COPY_LOOP=$(CAPUCHIN_COPY_LOOP)
ifeq ($(CAPUCHIN_LAYER_PROFILE),1)
pp_defines += CAPUCHIN_LAYER_PROFILE
endif

# Capuchin translation units only: port header forced in, upstream include
# layout, and the diagnostics GCC >= 14 raises on TI-compiler-accepted code
# demoted to warnings. Optimisation is the image-wide -O3 for both engines.
# Per-file code-generation differences (disclosed in the V8 doc):
#   dsplib_sw.c   -fwrapv  TI's generic C relies on int32 wrap in its accumulator
#   matrix_ops.c  -fno-tree-loop-distribute-patterns  keeps the [PORT P1] copy a loop
#   CAPUCHIN_KERNEL=2 links arm_mat_mult_q15 from neuralSPOT's prebuilt
#   libCMSISDSP-m4-gcc.a (vendor library build flags, not this image's).
CAPUCHIN_CFLAGS := -include $(port_dir)/capuchin_port.h -I$(cap_dir) -I$(port_dir) \
  -Wno-unknown-pragmas -Wno-discarded-qualifiers -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types \
  -Wno-error=int-conversion -Wno-int-conversion -Wno-error=implicit-function-declaration \
  -Wno-maybe-uninitialized -Wno-pedantic -Wno-missing-braces
ifeq ($(CAPUCHIN_MODEL_IN_SRAM),1)
CAPUCHIN_CFLAGS += -DCAPUCHIN_MODEL_QUAL=
endif
$(BINDIR)/$(cap_dir)/%.o: CFLAGS += $(CAPUCHIN_CFLAGS)
$(BINDIR)/$(port_dir)/%.o: CFLAGS += $(CAPUCHIN_CFLAGS)
# keep the [PORT P1] copy loop a loop (GCC would otherwise turn it back into a memcpy call)
$(BINDIR)/$(cap_dir)/math/matrix_ops.o: CFLAGS += -fno-tree-loop-distribute-patterns
$(BINDIR)/$(port_dir)/dsplib_sw.o: CFLAGS += -fwrapv
$(BINDIR)/$(RUIC_ENGINE_DIR)/%.o: CFLAGS += $(RUIC_ENGINE_FLAGS) -I$(RUIC_ENGINE_DIR)
$(BINDIR)/$(subdirectory)/src/ruic_bench.o: CFLAGS += $(RUIC_ENGINE_FLAGS) -I$(RUIC_ENGINE_DIR)

bindirs += $(local_bin)
examples += $(local_bin)/$(local_app_name).axf
examples += $(local_bin)/$(local_app_name).bin
mains += $(local_bin)/src/$(local_app_name).o

$(eval $(call make-axf, $(local_bin)/$(local_app_name), $(local_src)))
