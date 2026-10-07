# Host build of the Capuchin port sources (same files as the Apollo4 app).
#   make            -> host_capuchin_k1 (DSPLib generic C = LEA path) and host_capuchin_k0 (upstream CPU path)
APP  := ../..
SRC  := $(APP)/src
CAP  := $(SRC)/capuchin
CSRC := $(CAP)/decoder/decoder.c $(CAP)/layers/layers.c $(CAP)/math/fixed_point_ops.c \
        $(CAP)/math/matrix_ops.c $(CAP)/capuchin_invoke.c \
        $(SRC)/port/dsplib_sw.c $(SRC)/port/capuchin_port.c
# Same Capuchin-specific flags as module.mk; -m32 is not used: Capuchin's types are fixed-width.
CAPFLAGS := -std=gnu11 -O2 -fwrapv -include $(SRC)/port/capuchin_port.h \
            -I$(CAP) -I$(SRC)/port -I$(SRC) \
            -Wno-unknown-pragmas -Wno-discarded-qualifiers -Wno-incompatible-pointer-types \
            -Wno-int-conversion -Wno-unused-variable
all: host_capuchin_k1 host_capuchin_k0 host_capuchin_k1_audit
host_capuchin_k1: host_capuchin.c $(CSRC)
	$(CC) $(CAPFLAGS) -DCAPUCHIN_KERNEL=1 -o $@ $^
host_capuchin_k0: host_capuchin.c $(CSRC)
	$(CC) $(CAPFLAGS) -DCAPUCHIN_KERNEL=0 -o $@ $^
host_capuchin_k1_audit: host_capuchin.c $(CSRC)
	$(CC) $(CAPFLAGS) -DCAPUCHIN_KERNEL=1 -DCAPUCHIN_NUMERIC_AUDIT -o $@ $^
clean:
	rm -f host_capuchin_k* host_ruic_os

# RUIC engine reference (the OS package's sources, unmodified; SIMD emulated on host).
RUIC_SRC ?= $(APP)/../bisen_camera_harvest_OS/bisen_camera_harvest_OS/src
all: host_ruic_os
host_ruic_os: host_ruic.c $(RUIC_SRC)/nn_engine.c $(RUIC_SRC)/nn_kernels.c
	$(CC) -std=gnu11 -O2 -DNN_DATAFLOW=0 -DNN_SIMD=1 -DNN_SIMD_EMULATE=1 -DNN_MAX_CONV_IN_C=6 -I$(RUIC_SRC) -o $@ $^
