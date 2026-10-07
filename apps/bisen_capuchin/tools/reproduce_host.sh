#!/usr/bin/env bash
# Regenerate every host-side artifact of the Capuchin port from scratch.
#   usage: tools/reproduce_host.sh MNIST_IDX_DIR [RUIC_HEADER]
# Requires: gcc, python3 with numpy, tensorflow + tf_keras, fxpmath.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(dirname "$HERE")"
MNIST="${1:?MNIST IDX directory}"
HDR="${2:-$APP/../bisen_camera_harvest_OS/bisen_camera_harvest_OS/src/lenet_weights.h}"
export TF_USE_LEGACY_KERAS=1 TF_CPP_MIN_LOG_LEVEL=3
cd "$HERE"
mkdir -p "$APP/results"

echo "== 1. provenance of upstream sources and patch"
python3 verify_provenance.py

echo "== 2. scale selection on TRAINING data (baseline-favourable, test set untouched)"
python3 capuchin_scale_sweep.py --ruic-header "$HDR" --mnist-dir "$MNIST" --out "$APP/results/scale_sweep.json"
R=$(python3 -c "import json;print(json.load(open('$APP/results/scale_sweep.json'))['selected']['act_range'])")

echo "== 3. Capuchin headers with upstream encoder.py (+AveragePooling2D), act_range=$R"
python3 capuchin_export.py --ruic-header "$HDR" --act-range "$R"

echo "== 4. host builds (same sources as the board)"
make -C host -B RUIC_SRC="$(dirname "$HDR")"

echo "== 5. bench vectors + expected outputs (host C cross-checked against NumPy model)"
python3 make_bench_vectors.py --mnist-dir "$MNIST" --ruic-host host/host_ruic_os --n 100

echo "== 6. 10,000-image accuracy / agreement"
python3 compare_accuracy.py --mnist-dir "$MNIST" --ruic-header "$HDR"

echo "== 7. provenance again (generated headers vs manifest)"
python3 verify_provenance.py
