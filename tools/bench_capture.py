"""Reset the board over RTS and print BENCH/MEM/ui-up lines until the benchmark summary."""
import re
import sys
import time

import serial

port, limit = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
t0 = time.time()
while time.time() - t0 < limit:
    line = s.readline().decode(errors="replace").rstrip()
    if re.search(r"BENCH|ui up|Guru|abort\(\)|rst:0x(?!15)", line):
        print(line, flush=True)
    if "BENCH SUMMARY" in line:
        break
else:
    print("TIMEOUT waiting for BENCH SUMMARY")
