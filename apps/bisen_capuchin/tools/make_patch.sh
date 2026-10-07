#!/usr/bin/env bash
# Regenerate patches/capuchin-76b6eb2-apollo4.patch = diff(upstream/, src/capuchin/)
# over the upstream files only (generated header and capuchin_invoke.c excluded).
set -euo pipefail
APP="$(cd "$(dirname "$0")/.." && pwd)"
cd "$APP/upstream/capuchin-MCU"
for f in $(find decoder layers math utils -type f | sort); do
  diff -u --label "a/capuchin-MCU/$f" --label "b/capuchin-MCU/$f" "$f" "$APP/src/capuchin/$f" || true
done > "$APP/patches/capuchin-76b6eb2-apollo4.patch"
echo "hunks: $(grep -c '^@@' "$APP/patches/capuchin-76b6eb2-apollo4.patch")"
