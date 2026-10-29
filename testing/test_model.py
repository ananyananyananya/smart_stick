import glob
import os
import re
import sys

import numpy as np

W = 100
STRIDE = 25
THRESHOLDS = [0.5, 0.7, 0.8, 0.9, 0.95]


def load_header(path="fall_model.h"):
    txt = open(path).read()
    out = {}
    pattern = r"const float (\w+)((?:\[\d+\])+)=\{(.*?)\};"
    for name, dims, body in re.findall(pattern, txt, re.S):
        shape = tuple(int(d) for d in re.findall(r"\d+", dims))
        vals = [float(v) for v in re.findall(r"-?\d+\.\d+(?=f)", body)]
        out[name] = np.array(vals).reshape(shape)
    return out


def feats(w):
    a = np.linalg.norm(w[:, :3], axis=1)
    g = np.linalg.norm(w[:, 3:], axis=1)
    t = a[-25:]
    return np.array([a.mean(), a.std(), a.max(), a.min(),
                     g.mean(), g.std(), g.max(),
                     t.std(), t.mean(), float((a < 0.6).sum())])


def predict(f, p):
    x = (f - p["MEAN"]) / p["SCALE"]
    h1 = np.maximum(0, x @ p["W1"] + p["B1"])
    h2 = np.maximum(0, h1 @ p["W2"] + p["B2"])
    z = float((h2 @ p["W3"] + p["B3"])[0])
    return 1 / (1 + np.exp(-z))


paths = []
for arg in sys.argv[1:]:
    if os.path.isdir(arg):
        paths += sorted(glob.glob(os.path.join(arg, "*.csv")))
    else:
        paths.append(arg)
if not paths:
    sys.exit("usage: python test_model.py <folder or csv files>")

params = load_header()
rows = []
for path in paths:
    name = os.path.basename(path)
    d = np.loadtxt(path, delimiter=",")
    probs = np.array([predict(feats(d[s:s + W]), params)
                      for s in range(0, len(d) - W + 1, STRIDE)])
    if len(probs) == 0:
        continue
    is_fall = name.split("_")[0] == "fall"
    rows.append((name, is_fall, probs))
    print(f"{name:40s} {'FALL' if is_fall else 'ok  '}  windows={len(probs):4d}  max prob={probs.max():.3f}")

falls = [r for r in rows if r[1]]
others = [r for r in rows if not r[1]]
minutes = sum(len(r[2]) for r in others) * STRIDE / 50 / 60

print()
print("threshold  falls caught        non-fall files with alarm   alarms per minute")
for t in THRESHOLDS:
    caught = sum(r[2].max() > t for r in falls)
    bad_files = sum(r[2].max() > t for r in others)
    alarms = sum(int((r[2] > t).sum()) for r in others)
    per_min = alarms / minutes if minutes else 0
    print(f"{t:9.2f}  {caught:3d}/{len(falls):<3d}             "
          f"{bad_files:3d}/{len(others):<3d}                     {per_min:.2f}")
