import glob
import os
import re
import sys

import numpy as np
import pandas as pd
from numpy.lib.stride_tricks import sliding_window_view

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_fall_model as T

STRIDE = 25
COOLDOWN = 20


def load_forest(path="fall_forest.h"):
    lines = []
    alarm = 0.9
    names = []
    depth = 0
    for raw in open(path):
        s = raw.strip()
        m = re.match(r"#define FALL_ALARM_PROB ([-\d.eE+]+)f", s)
        if m:
            alarm = float(m.group(1))
            continue
        m = re.match(r"static inline float (fallTree\d+)\(const float \*f\) \{", s)
        if m:
            names.append(m.group(1))
            lines.append(f"def {m.group(1)}(f):")
            depth = 1
            continue
        if s.startswith("static inline float fallProbability"):
            break
        m = re.match(r"if \(f\[(\d+)\] <= ([-\d.eE+]+)f\) \{", s)
        if m:
            lines.append("    " * depth + f"if f[{m.group(1)}] <= {m.group(2)}:")
            depth += 1
            continue
        if s == "} else {":
            depth -= 1
            lines.append("    " * depth + "else:")
            depth += 1
            continue
        m = re.match(r"return ([-\d.eE+]+)f;", s)
        if m:
            lines.append("    " * depth + f"return {m.group(1)}")
            continue
        if s == "}":
            depth -= 1
    scope = {}
    exec("\n".join(lines), scope)
    trees = [scope[n] for n in names]
    return trees, alarm


def probabilities(trees, feats):
    return np.array([sum(t(f) for t in trees) / len(trees) for f in feats])


def file_probs(trees, path):
    d = pd.read_csv(path, header=None).values[:, :6].astype(np.float64)
    if len(d) < T.W:
        return None
    wins = sliding_window_view(d, (T.W, 6), axis=(0, 1))[::STRIDE, 0]
    return probabilities(trees, T.window_features(wins)), len(d) / T.FS


def main():
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            paths += sorted(glob.glob(os.path.join(arg, "*.csv")))
        else:
            paths.append(arg)
    if not paths:
        sys.exit("usage: python test_forest.py <folder or csv files>   (fall_forest.h must be in the current folder)")

    trees, alarm = load_forest()
    print(f"loaded {len(trees)} trees, alarm cutoff {alarm:.3f}\n")

    rows = []
    for path in paths:
        name = os.path.basename(path)
        res = file_probs(trees, path)
        if res is None:
            continue
        probs, secs = res
        is_fall = name.lower().startswith("fall")
        rows.append((name, is_fall, probs, secs))
        print(f"{name:40s} {'FALL' if is_fall else 'ok  '}  windows={len(probs):4d}  max prob={probs.max():.3f}")

    falls = [r for r in rows if r[1]]
    others = [r for r in rows if not r[1]]
    hours = sum(r[3] for r in others) / 3600

    cuts = sorted(set([0.5, 0.7, 0.8, 0.9, 0.95, round(alarm, 3)]))
    print("\ncutoff   falls caught    false alarms per hour (non-fall files)")
    for t in cuts:
        caught = sum(bool((r[2] > t).any()) for r in falls)
        alarms = 0
        for r in others:
            last = -10 ** 9
            for j in np.flatnonzero(r[2] > t):
                if j - last >= COOLDOWN:
                    alarms += 1
                    last = j
        fa = alarms / hours if hours else 0.0
        mark = "  <- device cutoff" if abs(t - alarm) < 1e-3 else ""
        print(f"{t:6.3f}   {caught:3d}/{len(falls):<3d}         {fa:8.2f}{mark}")


if __name__ == "__main__":
    main()
