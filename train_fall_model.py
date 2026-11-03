#!/usr/bin/env python3
"""
Train a data-driven fall detector and export it as C code for the ESP32 / Arduino.

What is different from the old pipeline
  * No hand-set impact gate (the old train_stick.py only looked at moments where acc > 2.5 g).
    Every 2-second window of every recording is a training example.
  * No hand-picked "free-fall if a < 0.6 g" count. The forest learns where the useful split
    points are from the data (min acceleration, peak position, post-impact stillness, ...).
  * The alarm cutoff is NOT typed in by hand. It is chosen from subject-held-out predictions so
    that the false-alarm rate stays under a budget you set (--max-fa-per-hour).
  * Features are orientation-independent (vector magnitudes and angles between vectors), so it does
    not matter which way the MPU6050 is mounted on the stick.

Usage
  python3 -I train_fall_model.py --data /path/to/training/data_sisfall --out out/
  python3 -I train_fall_model.py --data .../data_sisfall --extra my_recordings/ --out out/

Input CSVs: 6 columns (ax,ay,az in g ; gx,gy,gz in deg/s), 50 Hz, no header.
File name decides the class: names starting with "fall" are falls, everything else is not a fall.
Subject ids (SA01, SE05, ...) are read from the file name so that no person is in both train and test.
"""
import argparse
import glob
import json
import os
import re
import sys
import time

import numpy as np
import pandas as pd
from numpy.lib.stride_tricks import sliding_window_view
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import GroupKFold

# ---------------------------------------------------------------- constants (must match fall_features.h)
FS = 50                      # samples per second
W = 100                      # window = 2 s
STRIDE = 5                   # stride used for training/evaluation data
DEVICE_STRIDE = 25           # the sketch classifies every 25 samples (0.5 s)
PHASES = DEVICE_STRIDE // STRIDE
POS_LO, POS_HI = 40, 80      # a window is a "fall" example if the impact sits 0.8 s .. 1.6 s into it
COOLDOWN_WINDOWS = 20        # after an alarm the device stays quiet for 10 s (20 windows)
N_EDGE = 10                  # samples averaged to get the start / end orientation

FEATURE_NAMES = [
    "a_max", "a_min", "a_mean", "a_std",
    "w_max", "w_mean",
    "jerk_max", "peak_pos",
    "post_std", "post_dev", "pre_min",
    "tilt_change", "w_post_mean",
]
N_FEATURES = len(FEATURE_NAMES)


# ---------------------------------------------------------------- features
def window_features(win):
    """win: (n_windows, W, 6) -> (n_windows, N_FEATURES). The C version in fall_features.h does the same maths."""
    win = win.astype(np.float64)
    a = np.linalg.norm(win[:, :, :3], axis=2)          # acceleration magnitude (g)
    w = np.linalg.norm(win[:, :, 3:], axis=2)          # gyro magnitude (deg/s)
    n = a.shape[0]
    idx = np.arange(W)[None, :]

    a_mean = a.mean(1)
    p = a.argmax(1)                                     # sample index of the strongest impact in the window
    ps = np.minimum(p + 10, W - 5)                      # "after the impact" starts 0.2 s later
    post = idx >= ps[:, None]
    cnt = post.sum(1)
    post_mean = (a * post).sum(1) / cnt
    post_var = (((a - post_mean[:, None]) ** 2) * post).sum(1) / cnt
    pre_min = np.where(idx <= p[:, None], a, np.inf).min(1)

    u = win[:, :N_EDGE, :3].mean(1)                     # average gravity direction at the start ...
    v = win[:, -N_EDGE:, :3].mean(1)                    # ... and at the end of the window
    cosang = (u * v).sum(1) / (np.linalg.norm(u, axis=1) * np.linalg.norm(v, axis=1) + 1e-9)
    tilt = np.degrees(np.arccos(np.clip(cosang, -1.0, 1.0)))

    f = np.empty((n, N_FEATURES))
    f[:, 0] = a.max(1)
    f[:, 1] = a.min(1)
    f[:, 2] = a_mean
    f[:, 3] = a.std(1)
    f[:, 4] = w.max(1)
    f[:, 5] = w.mean(1)
    f[:, 6] = np.abs(np.diff(a, axis=1)).max(1)
    f[:, 7] = p / (W - 1)
    f[:, 8] = np.sqrt(post_var)
    f[:, 9] = np.abs(post_mean - 1.0)
    f[:, 10] = pre_min
    f[:, 11] = tilt
    f[:, 12] = (w * post).sum(1) / cnt
    return f


# ---------------------------------------------------------------- data loading
def subject_of(name):
    m = re.search(r"(S[AE]\d+)", name)
    return m.group(1) if m else os.path.splitext(name)[0]


def load_folder(folder, tag):
    """Return a list of per-file dicts with features, labels and bookkeeping for event-level scoring."""
    files = sorted(glob.glob(os.path.join(folder, "*.csv")))
    if not files:
        sys.exit(f"No CSV files in {folder}")
    out = []
    for path in files:
        name = os.path.basename(path)
        d = pd.read_csv(path, header=None).values
        if d.ndim != 2 or d.shape[1] < 6 or len(d) < W:
            continue
        d = d[:, :6].astype(np.float64)
        is_fall = name.lower().startswith("fall")
        starts = np.arange(0, len(d) - W + 1, STRIDE)
        wins = sliding_window_view(d, (W, 6), axis=(0, 1))[::STRIDE, 0]
        feats = window_features(wins)
        label = np.zeros(len(starts), dtype=np.int8)
        if is_fall:
            peak = int(np.linalg.norm(d[:, :3], axis=1).argmax())     # strongest impact of the whole trial
            pos = peak - starts
            inside = (pos >= 0) & (pos < W)
            label[inside] = -1                                         # impact visible but not a clean example
            label[(pos >= POS_LO) & (pos <= POS_HI)] = 1               # clean "fall happening" example
        out.append(dict(name=tag + name, subject=tag + subject_of(name), fall=is_fall,
                        X=feats.astype(np.float32), y=label, seconds=len(d) / FS))
    return out


def stack(files):
    X = np.concatenate([f["X"] for f in files])
    y = np.concatenate([f["y"] for f in files])
    fid = np.concatenate([np.full(len(f["y"]), i) for i, f in enumerate(files)])
    k = np.concatenate([np.arange(len(f["y"])) for f in files])
    return X, y, fid, k


# ---------------------------------------------------------------- event-level scoring (what the stick would do)
def build_sequences(files, fid, k, prob):
    """Split probabilities into per-file, per-phase sequences (a phase = which 0.5 s grid the stick happens to run on)."""
    seqs = []
    for i, f in enumerate(files):
        sel = np.flatnonzero(fid == i)
        for ph in range(PHASES):
            s = prob[sel][(k[sel] % PHASES) == ph]
            if len(s):
                seqs.append((f["fall"], s))
    return seqs


def event_scores(seqs, hours, thr):
    fall_n = fall_hit = alarms = 0
    for is_fall, s in seqs:
        if is_fall:
            fall_n += 1
            fall_hit += bool((s > thr).any())
        else:
            last = -10 ** 9
            for j in np.flatnonzero(s > thr):
                if j - last >= COOLDOWN_WINDOWS:
                    alarms += 1
                    last = j
    # every ADL file was split into PHASES sequences, so average the alarm count back over the phases
    return fall_hit / max(fall_n, 1), alarms / PHASES / hours


def choose_threshold(seqs, hours, max_fa):
    grid = np.round(np.arange(0.05, 0.96, 0.01), 2)
    table = [(t,) + event_scores(seqs, hours, t) for t in grid]
    ok = [r for r in table if r[2] <= max_fa]
    chosen = ok[0] if ok else table[-1]            # lowest cutoff that still respects the false-alarm budget
    return chosen[0], table, bool(ok)


# ---------------------------------------------------------------- export to C
def cfloat(x):
    s = f"{float(np.float32(x)):.8g}"
    if not any(c in s for c in ".en"):
        s += ".0"
    return s + "f"


def emit_tree(t, node, depth, lines):
    pad = "  " * depth
    if t.children_left[node] == -1:
        v = t.value[node][0]
        lines.append(f"{pad}return {cfloat(v[1] / v.sum())};")
        return
    lines.append(f"{pad}if (f[{t.feature[node]}] <= {cfloat(t.threshold[node])}) {{")
    emit_tree(t, t.children_left[node], depth + 1, lines)
    lines.append(f"{pad}}} else {{")
    emit_tree(t, t.children_right[node], depth + 1, lines)
    lines.append(f"{pad}}}")


def export_header(path, clf, threshold, info):
    L = ["#pragma once",
         "// AUTO-GENERATED by train_fall_model.py - do not edit by hand.",
         f"// {info}",
         "// Input: the 13 features from fall_features.h, in this order:",
         "//   " + ", ".join(f"[{i}] {n}" for i, n in enumerate(FEATURE_NAMES)),
         f"#define FALL_N_FEATURES {N_FEATURES}",
         f"#define FALL_N_TREES {len(clf.estimators_)}",
         f"#define FALL_ALARM_PROB {cfloat(threshold)}   // chosen from held-out subjects, not by hand",
         ""]
    for i, est in enumerate(clf.estimators_):
        L.append(f"static inline float fallTree{i}(const float *f) {{")
        emit_tree(est.tree_, 0, 1, L)
        L.append("}")
        L.append("")
    L.append("// Probability (0..1) that the 2-second window contains a fall = average vote of all trees.")
    L.append("static inline float fallProbability(const float *f) {")
    L.append("  float s = 0.0f;")
    for i in range(len(clf.estimators_)):
        L.append(f"  s += fallTree{i}(f);")
    L.append("  return s / FALL_N_TREES;")
    L.append("}")
    with open(path, "w") as fh:
        fh.write("\n".join(L) + "\n")


# ---------------------------------------------------------------- main
def make_model(args, seed):
    return RandomForestClassifier(n_estimators=args.trees, max_depth=args.depth,
                                  min_samples_leaf=args.min_leaf, max_features="sqrt",
                                  class_weight="balanced_subsample", bootstrap=True,
                                  max_samples=0.5, n_jobs=-1, random_state=seed)


def training_rows(y, rng, neg_cap):
    pos = np.flatnonzero(y == 1)
    neg = np.flatnonzero(y == 0)
    if len(neg) > neg_cap:
        neg = rng.choice(neg, neg_cap, replace=False)
    return np.concatenate([pos, neg])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="folder of converted SisFall CSVs (training/data_sisfall)")
    ap.add_argument("--extra", help="optional folder with your own recordings (fall_*.csv / walk_*.csv ...)")
    ap.add_argument("--out", default="out")
    ap.add_argument("--trees", type=int, default=20)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--min-leaf", type=int, default=20)
    ap.add_argument("--max-fa-per-hour", type=float, default=1.0,
                    help="false-alarm budget used to pick the alarm cutoff")
    ap.add_argument("--neg-cap", type=int, default=150000, help="max non-fall windows used per fit (speed)")
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    t0 = time.time()
    files = load_folder(args.data, "")
    if args.extra:
        files += load_folder(args.extra, "own:")
    X, y, fid, k = stack(files)
    subj = np.array([files[i]["subject"] for i in fid])
    n_fall_files = sum(f["fall"] for f in files)
    hours = sum(f["seconds"] for f in files if not f["fall"]) / 3600
    print(f"loaded {len(files)} recordings ({n_fall_files} fall, {len(files) - n_fall_files} non-fall, "
          f"{hours:.2f} h of non-fall data), {len(y)} windows, {len(set(subj))} subjects  [{time.time() - t0:.0f}s]")
    print(f"training examples: {(y == 1).sum()} fall windows, {(y == 0).sum()} non-fall windows, "
          f"{(y == -1).sum()} ambiguous (left out of training, still used when scoring)")

    # ---- subject-held-out cross-validation: nobody is in both train and test
    oof = np.zeros(len(y))
    for fold, (tr, te) in enumerate(GroupKFold(n_splits=args.folds).split(X, y, subj), 1):
        assert set(subj[tr]).isdisjoint(subj[te])
        rows = tr[training_rows(y[tr], rng, args.neg_cap)]
        clf = make_model(args, args.seed + fold).fit(X[rows], (y[rows] == 1).astype(int))
        oof[te] = clf.predict_proba(X[te])[:, 1]
        print(f"  fold {fold}: trained on {len(set(subj[tr]))} subjects, tested on {len(set(subj[te]))}  [{time.time() - t0:.0f}s]")

    clean = y >= 0
    auc = roc_auc_score(y[clean], oof[clean])
    seqs = build_sequences(files, fid, k, oof)
    thr, table, met_budget = choose_threshold(seqs, hours, args.max_fa_per_hour)
    det, fa = event_scores(seqs, hours, thr)

    print(f"\nwindow-level AUC (held-out subjects): {auc:.4f}")
    print(f"chosen alarm cutoff: {thr:.2f}  ->  falls caught {det * 100:.1f}%   false alarms {fa:.2f}/hour"
          + ("" if met_budget else "   (!) budget not reachable, using the strictest cutoff"))
    print("\n cutoff | falls caught | false alarms / hour")
    for t, d, f in table:
        if abs(t * 100 % 5) < 1e-6 or abs(t - thr) < 1e-9:
            print(f"  {t:.2f}  |    {d * 100:5.1f}%    |   {f:6.2f}")

    # ---- per-subject-group view (young SA vs elderly SE) so weak spots are visible
    per = {}
    for pref in ("SA", "SE"):
        ids = [i for i, f in enumerate(files) if f["subject"].startswith(pref)]
        if not ids:
            continue
        mask = np.isin(fid, ids)
        sq = build_sequences([files[i] for i in ids], np.searchsorted(ids, fid[mask]), k[mask], oof[mask])
        h = sum(files[i]["seconds"] for i in ids if not files[i]["fall"]) / 3600
        d_, f_ = event_scores(sq, h, thr)
        per[pref] = (d_, f_)
        print(f"  {pref}: falls caught {d_ * 100:.1f}%, false alarms {f_:.2f}/h")

    # ---- final model on everyone, exported to C
    rows = training_rows(y, rng, args.neg_cap * 2)
    final = make_model(args, args.seed).fit(X[rows], (y[rows] == 1).astype(int))
    info = (f"RandomForest trees={args.trees} depth<={args.depth}; held-out-subject result at cutoff {thr:.2f}: "
            f"falls caught {det * 100:.1f}%, false alarms {fa:.2f}/h")
    export_header(os.path.join(args.out, "fall_forest.h"), final, thr, info)

    imp = sorted(zip(FEATURE_NAMES, final.feature_importances_), key=lambda z: -z[1])
    print("\nfeature importance (what the forest actually relies on):")
    for n, v in imp:
        print(f"  {n:12s} {v:.3f}")

    # ---- save material for the C parity test + a report
    sample = rng.choice(len(y), 3000, replace=False)
    np.save(os.path.join(args.out, "parity_features.npy"), X[sample])
    np.save(os.path.join(args.out, "parity_prob.npy"),
            np.mean([e.predict_proba(X[sample])[:, 1] for e in final.estimators_], axis=0))
    report = dict(threshold=float(thr), falls_caught=det, false_alarms_per_hour=fa, window_auc=float(auc),
                  per_group={k_: dict(falls_caught=v[0], false_alarms_per_hour=v[1]) for k_, v in per.items()},
                  trees=args.trees, depth=args.depth, features=FEATURE_NAMES,
                  curve=[dict(cutoff=t, falls_caught=d, false_alarms_per_hour=f) for t, d, f in table])
    with open(os.path.join(args.out, "report.json"), "w") as fh:
        json.dump(report, fh, indent=1)
    size = os.path.getsize(os.path.join(args.out, "fall_forest.h"))
    print(f"\nwrote {args.out}/fall_forest.h ({size / 1024:.0f} KB of source), report.json   [{time.time() - t0:.0f}s total]")


if __name__ == "__main__":
    main()
