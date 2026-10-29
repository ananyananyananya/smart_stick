import glob
import os
import sys

import numpy as np

root = sys.argv[1] if len(sys.argv) > 1 else "SisFall_dataset"
os.makedirs("data", exist_ok=True)

ACC = 32 / 2 ** 13
GYRO = 4000 / 2 ** 16

files = sorted(glob.glob(os.path.join(root, "*", "*.txt")))
print("found", len(files), "files")

done = 0
for path in files:
    name = os.path.splitext(os.path.basename(path))[0]
    kind = name[0].upper()
    if kind not in ("D", "F"):
        continue
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().rstrip(";").split(",")
            if len(parts) < 6:
                continue
            try:
                rows.append([float(v) for v in parts[:6]])
            except ValueError:
                continue
    d = np.array(rows)
    n = len(d) // 4 * 4
    if n < 400:
        continue
    d = d[:n].reshape(-1, 4, 6).mean(axis=1)
    d[:, :3] *= ACC
    d[:, 3:] = np.clip(d[:, 3:] * GYRO, -500, 500)
    label = "fall" if kind == "F" else "adl"
    np.savetxt(f"data/{label}_{name}.csv", d, delimiter=",", fmt="%.3f")
    done += 1

print("converted", done, "files into data/")
