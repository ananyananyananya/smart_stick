import os
import sys
import time

import serial

port, label, secs = sys.argv[1], sys.argv[2], int(sys.argv[3])
out_dir = sys.argv[4] if len(sys.argv) > 4 else "data"
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, f"{label}_{int(time.time())}.csv")

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()
ser.reset_input_buffer()

rows = []
t0 = None
waiting_since = time.time()
while t0 is None or time.time() - t0 < secs:
    if t0 is None and time.time() - waiting_since > 20:
        sys.exit("no data received: check the COM port, close the Serial Monitor, and check DATA_COLLECT is 1")
    parts = ser.readline().decode(errors="ignore").strip().split(",")
    if len(parts) != 6:
        continue
    try:
        [float(v) for v in parts]
    except ValueError:
        continue
    if t0 is None:
        t0 = time.time()
        print("recording...")
    rows.append(",".join(parts))

with open(path, "w") as f:
    f.write("\n".join(rows) + "\n")

print("saved", path, len(rows), "samples (expected about", secs * 50, ")")
