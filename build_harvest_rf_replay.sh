#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NS_ROOT=${NEURALSPOT_ROOT:-$SCRIPT_DIR}
OUT_ROOT=${FIRMWARE_OUT_ROOT:-$NS_ROOT/firmware_builds}
BUILD_DIR="$NS_ROOT/build/apollo4p_evb/arm-none-eabi/apps/bisen_camera_harvest"
BASE=bisen_camera_harvest_rf_replay_btn_control_gpio17_2026-10-06

cd "$NS_ROOT"

make -B \
  EXAMPLE=bisen_camera_harvest \
  PLATFORM=apollo4p_evb \
  AS_VERSION=R4.5.0 \
  BISEN_HARVEST_CALIBRATION_MODE=0 \
  BISEN_CAMERA_ENABLE_MRAM=1 \
  BISEN_HARVEST_CHECKPOINT_MAGIC=0x48565233 \
  BISEN_CAMERA_AUTORUN=0 \
  BISEN_HARVEST_AUTOCONTINUOUS=1 \
  BISEN_HARVEST_OFFLINE_VALIDATE=0 \
  BISEN_ENABLE_SWO_LOGGING=0 \
  BISEN_ENABLE_STATE_DAC=1 \
  BISEN_CAMERA_MAX_CHECKPOINTS=0 \
  BISEN_CAMERA_MAX_WAIT_CYCLES=0 \
  BISEN_HARVEST_SUPPLY_PIN=17 \
  BISEN_HARVEST_CAL_PIN=17 \
  BISEN_HARVEST_CAL_LOW_CODE=476 \
  BISEN_HARVEST_CAL_LOW_UV=5604455 \
  BISEN_HARVEST_CAL_HIGH_CODE=655 \
  BISEN_HARVEST_CAL_HIGH_UV=7705526 \
  BISEN_HARVEST_CRITICAL_UV=5800000 \
  BISEN_HARVEST_WORK100_UV=6200000 \
  BISEN_HARVEST_WORK500_UV=6800000 \
  BISEN_HARVEST_WORK1000_UV=7300000

mkdir -p "$OUT_ROOT"
cp "$BUILD_DIR/bisen_camera_harvest.bin" "$OUT_ROOT/$BASE.bin"
cp "$BUILD_DIR/bisen_camera_harvest.axf" "$OUT_ROOT/$BASE.axf"
cp "$BUILD_DIR/bisen_camera_harvest.map" "$OUT_ROOT/$BASE.map"

shasum -a 256 "$OUT_ROOT/$BASE.bin" > "$OUT_ROOT/$BASE.sha256"
cat > "$OUT_ROOT/$BASE.config.txt" <<'EOF'
app=bisen_camera_harvest
platform=apollo4p_evb
as_version=R4.5.0
mode=isolated-rf-replay-btn-controlled-durable-continuous
checkpoint_magic=0x48565233 (HVR3)
mram=1
autorun=0
auto_continuous=1
start_control=BTN0 durable-arm
stop_control=BTN1 checkpoint-then-durable-disarm
power_loss_behavior=auto-resume only while durable-arm is set
offline_validation=0
swo_logging=0
state_dac=1
max_checkpoints=0
max_wait_cycles=0
supply_pin=GPIO17/ADCSE2 cal_pin=17
calibration_codes=476@5604455uV,655@7705526uV (GPIO17 sweep 2026-10-06, 25 points, scope disconnected)
thresholds_uV=critical:5800000,work100:6200000,work500_resume:6800000,work1000:7300000
EOF

echo "Archived RF-replay firmware in $OUT_ROOT"
