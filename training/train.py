import glob
import os
import re

import numpy as np
import pandas as pd

from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import GroupKFold
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler

DATA_DIR = "data_sisfall"

W = 100
STEP = 25
HALF = W // 2

def feats(w):
    a = np.linalg.norm(w[:, :3], axis=1)
    g = np.linalg.norm(w[:, 3:], axis=1)
    t = a[-25:]

    return [a.mean(), a.std(), a.max(), a.min(), g.mean(), g.std(), g.max(), t.std(), t.mean(), float((a < 0.6).sum())]


X, y, groups = [], [], []

files = sorted(glob.glob(os.path.join(DATA_DIR, "*.csv")))

if not files:
    raise SystemExit(f"No CSV files found in {DATA_DIR}. Run convert_sisfall.py first.")

print("Found", len(files), "converted recordings.")

for f in files:
    filename = os.path.basename(f)
    label = filename.split("_")[0].lower()
    if label not in ("fall", "adl"):
        continue
    match = re.search(r"(SA\d+|SE\d+)", filename)
    if not match:
        raise ValueError(f"Could not identify participant in filename: {filename}")
    subject = match.group(1)
    d = pd.read_csv(f, header=None).values.astype(float)
    if d.ndim != 2 or d.shape[1] < 6:
        print("Skipping invalid file:", filename)
        continue
    d = d[:, :6]
    if label == "fall":
        p = int(np.linalg.norm(d[:, :3], axis=1).argmax())
        for s in range(-20, 21, 5):
            c = p + s
            if c - HALF >= 0 and c + HALF <= len(d):
                X.append(feats(d[c - HALF:c + HALF]))
                y.append(1)
                groups.append(subject)
    else:
        for s in range(0, len(d) - W + 1, STEP):
            X.append(feats(d[s:s + W]))
            y.append(0)
            groups.append(subject)


X = np.array(X)
y = np.array(y)
groups = np.array(groups)

if len(y) == 0:
    raise SystemExit("No valid windows extracted.")

print("\nTotal windows:", len(y))
print("Fall windows:", int(y.sum()))
print("Non-fall windows:", int((y == 0).sum()))
print("Participants:", sorted(set(groups)))

from sklearn.metrics import f1_score

cv = GroupKFold(n_splits=5)

oof_pred = np.empty(len(y), dtype=int)
oof_prob = np.empty(len(y), dtype=float)
fold_f1 = []

for fold, (tr, te) in enumerate(cv.split(X, y, groups=groups), start=1):

    Xt, yt = X[tr].copy(), y[tr].copy()

    train_subjects = sorted(set(groups[tr]))
    test_subjects = sorted(set(groups[te]))

    print(f"\n FOLD {fold} ")
    print("Training participants:", train_subjects)
    print("Testing participants:", test_subjects)

    assert set(train_subjects).isdisjoint(set(test_subjects))
    pos = np.where(yt == 1)[0]
    extra = (yt == 0).sum() - len(pos)
    if len(pos) == 0:
        raise ValueError(f"No fall samples in fold {fold} training data.")
    if extra > 0:
        rng = np.random.default_rng(100 + fold)
        idx = rng.choice(pos, extra)
        Xt = np.vstack([Xt, Xt[idx]])
        yt = np.concatenate([yt, yt[idx]])
    sc_fold = StandardScaler().fit(Xt)
    clf_fold = MLPClassifier(hidden_layer_sizes=(16, 8), activation="relu", max_iter=3000, random_state=0)

    clf_fold.fit(sc_fold.transform(Xt), yt)
    prob = clf_fold.predict_proba(sc_fold.transform(X[te]))[:, 1]

    oof_prob[te] = prob

    pred = (prob >= 0.8).astype(int)
    oof_pred[te] = pred

    score = f1_score(y[te], pred, pos_label=1, zero_division=0)

    fold_f1.append(score)

    print("Fall F1-score:", round(score, 4))
    print("Confusion matrix:")
    print(confusion_matrix(y[te], pred, labels=[0, 1]))


print("\n5-Fold Results")

print("Fall F1-score per fold:", fold_f1)
print("Mean fall F1-score:", np.mean(fold_f1))
print("Standard deviation:", np.std(fold_f1))

print("\nOverall confusion matrix:")
print(confusion_matrix(y, oof_pred, labels=[0, 1]))

print("\nOverall classification report:")
print(classification_report(y, oof_pred, labels=[0, 1], target_names=["not_fall", "fall"], zero_division=0))

print("\nThreshold Comparison")

for threshold in [0.5, 0.6, 0.7, 0.8, 0.9]:
    pred_t = (oof_prob >= threshold).astype(int)
    print(f"\nThreshold: {threshold}")
    print("Confusion Matrix:")
    print(confusion_matrix(y, pred_t, labels=[0, 1]))
    print("Classification Report:")
    print(classification_report(y, pred_t, labels=[0, 1], target_names=["not_fall", "fall"], zero_division=0))

Xt, yt = X.copy(), y.copy()

pos = np.where(yt == 1)[0]
extra = (yt == 0).sum() - len(pos)

if extra > 0:
    rng = np.random.default_rng(0)
    idx = rng.choice(pos, extra)

    Xt = np.vstack([Xt, Xt[idx]])
    yt = np.concatenate([yt, yt[idx]])

sc = StandardScaler().fit(Xt)

clf = MLPClassifier(
    hidden_layer_sizes=(16, 8),
    activation="relu",
    max_iter=3000,
    random_state=0
)

clf.fit(sc.transform(Xt), yt)

def vec(name, v):
    return (
        f"const float {name}[{len(v)}]={{"
        + ",".join(f"{x:.6f}f" for x in v)
        + "};\n"
    )


def mat(name, m):
    rows = ",\n".join(
        "{" + ",".join(f"{x:.6f}f" for x in r) + "}"
        for r in m
    )

    return (
        f"const float {name}[{m.shape[0]}][{m.shape[1]}]={{\n"
        f"{rows}\n}};\n"
    )


h = "#pragma once\n"
h += vec("MEAN", sc.mean_)
h += vec("SCALE", sc.scale_)

for i, (w, b) in enumerate(zip(clf.coefs_, clf.intercepts_), 1):
    h += mat(f"W{i}", w)
    h += vec(f"B{i}", b)

with open("fall_model_sisfall_full.h", "w") as f:
    f.write(h)

print("\nExported final model: fall_model_sisfall_full.h")
