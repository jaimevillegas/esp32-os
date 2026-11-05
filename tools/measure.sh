#!/usr/bin/env bash
# Usage: tools/measure.sh <label>
# Rebuilds (fresh sdkconfig), flashes, then prints the RADIOS/MEM heap lines of one boot.
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f sdkconfig.dev
pio run -e dev -t upload 2>&1 | grep -E " error|FAILED|undefined" || true
echo "=== $1"
~/.platformio/penv/bin/python -m esp_idf_size .pio/build/dev/firmware.map 2>/dev/null | grep -E "DIRAM|\.text  |\.bss|\.data|Flash Code" | head -6
~/.local/share/pipx/venvs/platformio/bin/python -I - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
t = time.time()
while time.time() - t < 45:
    l = s.readline().decode(errors='replace').rstrip()
    if any(k in l for k in ('RADIOS', 'ui-steady', 'internal free', 'Guru', 'abort', 'Backtrace', 'stack up', 'stack down')):
        print(l)
    if 'settled' in l:
        break
PY
