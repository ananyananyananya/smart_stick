import glob
import os

import numpy as np
import pandas as pd
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import GroupShuffleSplit
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler

W = 100
STEP = 25
HALF = W // 2


def feats(w):
    a = np.linalg.norm(w[:, :3], axis=1)
    g = np.linalg.norm(w[:, 3:], axis=1)
    t = a[-25:]
    return [a.mean(), a.std(), a.max(), a.min(),
            g.mean(), g.std(), g.max(),
            t.std(), t.mean(), float((a < 0.6).sum())]


X, y, grp = [], [], []
for i, f in enumerate(sorted(glob.glob("data/*.csv"))):
    label = os.path.basename(f).split("_")[0]
    d = pd.read_csv(f, header=None).values.astype(float)
    if label == "fall":
        p = int(np.linalg.norm(d[:, :3], axis=1).argmax())
        for s in range(-20, 21, 5):
            c = p + s
            if c - HALF >= 0 and c + HALF <= len(d):
                X.append(feats(d[c - HALF:c + HALF]))
                y.append(1)
                grp.append(i)
    else:
        for s in range(0, len(d) - W + 1, STEP):
            X.append(feats(d[s:s + W]))
            y.append(0)
            grp.append(i)

X, y, grp = np.array(X), np.array(y), np.array(grp)
print("windows:", len(y), "falls:", int(y.sum()), "non-falls:", int((y == 0).sum()))

tr, te = next(GroupShuffleSplit(test_size=0.25, random_state=0).split(X, y, grp))
Xt, yt = X[tr], y[tr]

pos = np.where(yt == 1)[0]
extra = (yt == 0).sum() - len(pos)
if len(pos) and extra > 0:
    idx = np.random.default_rng(0).choice(pos, extra)
    Xt = np.vstack([Xt, Xt[idx]])
    yt = np.concatenate([yt, yt[idx]])

sc = StandardScaler().fit(Xt)
clf = MLPClassifier(hidden_layer_sizes=(16, 8), activation="relu",
                    max_iter=3000, random_state=0)
clf.fit(sc.transform(Xt), yt)

pred = clf.predict(sc.transform(X[te]))
print(confusion_matrix(y[te], pred))
print(classification_report(y[te], pred, labels=[0, 1],
                            target_names=["not_fall", "fall"], zero_division=0))


def vec(name, v):
    return f"const float {name}[{len(v)}]={{" + ",".join(f"{x:.6f}f" for x in v) + "};\n"


def mat(name, m):
    rows = ",\n".join("{" + ",".join(f"{x:.6f}f" for x in r) + "}" for r in m)
    return f"const float {name}[{m.shape[0]}][{m.shape[1]}]={{\n{rows}\n}};\n"


h = "#pragma once\n" + vec("MEAN", sc.mean_) + vec("SCALE", sc.scale_)
for i, (w, b) in enumerate(zip(clf.coefs_, clf.intercepts_), 1):
    h += mat(f"W{i}", w) + vec(f"B{i}", b)

with open("fall_model.h", "w") as f:
    f.write(h)
print("wrote fall_model.h")
