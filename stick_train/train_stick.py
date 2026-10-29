import glob
import os
import sys

import numpy as np
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import GroupKFold, cross_val_predict
from sklearn.tree import DecisionTreeClassifier

ACC_LSB = 4096.0
GYRO_LSB = 65.5
IMPACT_G = 2.5
WIN = 150
PRE = 50
POST_AFTER = 99
COOLDOWN = 500

NAMES = ["peak", "minPre", "postMean", "postStd", "gPeak", "gPostMean",
         "tiltPre", "tiltPost", "tiltDelta"]


def load(path):
    d = np.atleast_2d(np.loadtxt(path, delimiter=","))
    a = d[:, :3] / ACC_LSB
    g = d[:, 3:6] / GYRO_LSB
    acc = np.linalg.norm(a, axis=1)
    gyr = np.linalg.norm(g, axis=1)
    keep = acc >= 0.01
    a, acc, gyr = a[keep], acc[keep], gyr[keep]
    tilt = np.degrees(np.arccos(np.clip(np.abs(a[:, 2]) / acc, 0, 1)))
    return acc, gyr, tilt


def triggers(acc):
    out = []
    i = PRE
    while i < len(acc) - POST_AFTER:
        if acc[i] > IMPACT_G:
            out.append(i)
            i += POST_AFTER + COOLDOWN
        else:
            i += 1
    return out


def features(acc, gyr, tilt, i):
    s, e = i - PRE, i + POST_AFTER + 1
    a, g, t = acc[s:e], gyr[s:e], tilt[s:e]
    post = a[60:]
    return [a.max(), a[:PRE].min(), post.mean(), post.std(),
            g.max(), g[60:].mean(),
            t[:40].mean(), t[-30:].mean(), t[-30:].mean() - t[:40].mean()]


def build(folder):
    X, y, grp, missed = [], [], [], []
    for gi, path in enumerate(sorted(glob.glob(os.path.join(folder, "*.csv")))):
        label = os.path.basename(path).split("_")[0]
        acc, gyr, tilt = load(path)
        idx = triggers(acc)
        if label == "fall":
            if not idx:
                missed.append(os.path.basename(path))
                continue
            idx = idx[:1]
        for i in idx:
            X.append(features(acc, gyr, tilt, i))
            y.append(1 if label == "fall" else 0)
            grp.append(gi)
    return np.array(X), np.array(y), np.array(grp), missed


def emit(t, node, depth):
    pad = "  " * depth
    if t.children_left[node] == -1:
        return f"{pad}return {int(np.argmax(t.value[node]))};\n"
    s = f"{pad}if (f[{t.feature[node]}] <= {t.threshold[node]:.6f}f) {{\n"
    s += emit(t, t.children_left[node], depth + 1)
    s += f"{pad}}} else {{\n"
    s += emit(t, t.children_right[node], depth + 1)
    return s + f"{pad}}}\n"


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "stick_data"
    X, y, grp, missed = build(folder)
    print("impact windows:", len(y), " falls:", int(y.sum()), " non-falls:", int((y == 0).sum()))
    if missed:
        print(f"falls that never exceeded {IMPACT_G} g (the stick would miss these):")
        for m in missed:
            print("  ", m)
    if y.sum() < 10 or (y == 0).sum() < 10:
        sys.exit("need at least 10 windows of each class, record more data")

    clf = DecisionTreeClassifier(max_depth=4, min_samples_leaf=3,
                                 class_weight="balanced", random_state=0)
    splits = min(5, len(set(grp)))
    pred = cross_val_predict(clf, X, y, groups=grp, cv=GroupKFold(n_splits=splits))
    print(confusion_matrix(y, pred))
    print(classification_report(y, pred, labels=[0, 1],
                                target_names=["not_fall", "fall"], zero_division=0))
    caught = int(((pred == 1) & (y == 1)).sum())
    print(f"overall falls caught: {caught} / {int(y.sum()) + len(missed)}")

    clf.fit(X, y)
    for n, v in zip(NAMES, clf.feature_importances_):
        print(f"{n:10s} {v:.2f}")

    h = "#pragma once\ninline int predictFall(const float *f) {\n" + emit(clf.tree_, 0, 1) + "}\n"
    with open("fall_model.h", "w") as f:
        f.write(h)
    print("wrote fall_model.h")


if __name__ == "__main__":
    main()
