#!/usr/bin/env bash
# Usage: tools/bench_matrix.sh <label> [KCONFIG_LINE ...]
# Builds the `bench` env with the given sdkconfig overrides, flashes it and
# captures the LVGL benchmark summary from the serial port.
set -euo pipefail
cd "$(dirname "$0")/.."
label=$1; shift
printf '%s\n' "$@" > sdkconfig.defaults.bench
rm -f sdkconfig.bench
pio run -e bench -t upload 2>&1 | grep -E "error:|FAILED|undefined" || true
echo "=== $label: $*"
for kv in "$@"; do grep -E "^${kv%%=*}=" sdkconfig.bench || echo "WARNING: ${kv%%=*} not applied"; done
~/.local/share/pipx/venvs/platformio/bin/python -I tools/bench_capture.py /dev/ttyACM0 "${BENCH_TIMEOUT:-300}"
