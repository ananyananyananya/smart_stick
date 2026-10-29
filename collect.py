import os
import sys
import time

import serial

port, label, secs = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs("data", exist_ok=True)
path = f"data/{label}_{int(time.time())}.csv"

ser = serial.Serial(port, 115200, timeout=1)
time.sleep(2)
ser.reset_input_buffer()

end = time.time() + secs
n = 0
with open(path, "w") as f:
    while time.time() < end:
        line = ser.readline().decode(errors="ignore").strip()
        parts = line.split(",")
        if len(parts) != 6:
            continue
        try:
            [float(v) for v in parts]
        except ValueError:
            continue
        f.write(line + "\n")
        n += 1

print(path, n, "samples")
