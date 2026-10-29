import os

import numpy as np

rng = np.random.default_rng(1)
os.makedirs("data", exist_ok=True)


def walk(n):
    t = np.arange(n) / 50
    az = 1 + 0.35 * np.sin(2 * np.pi * 1.8 * t) + rng.normal(0, 0.05, n)
    ax = 0.2 * np.sin(2 * np.pi * 0.9 * t) + rng.normal(0, 0.05, n)
    ay = 0.2 * np.cos(2 * np.pi * 1.8 * t) + rng.normal(0, 0.05, n)
    g = rng.normal(0, 15, (n, 3)) + 20 * np.sin(2 * np.pi * 1.8 * t)[:, None]
    return np.column_stack([ax, ay, az, g])


def stand(n):
    return rng.normal([0, 0, 1, 0, 0, 0], [0.02, 0.02, 0.02, 2, 2, 2], (n, 6))


def fall(n=300):
    d = np.column_stack([rng.normal(0, 0.02, n), rng.normal(0, 0.02, n),
                         1 + rng.normal(0, 0.02, n), rng.normal(0, 2, (n, 3))])
    p = rng.integers(100, 160)
    d[p - 8:p, :3] *= 0.2
    d[p:p + 3, :3] += rng.normal(0, 1, (3, 3)) * 6 + 6
    d[p:p + 10, 3:] += rng.normal(0, 200, (10, 3))
    return d


def save(name, d):
    np.savetxt(f"data/{name}.csv", d, delimiter=",", fmt="%.3f")


for i in range(8):
    save(f"walk_{i}", walk(6000))
for i in range(6):
    save(f"stand_{i}", stand(3000))
for i in range(30):
    save(f"fall_{i}", fall())

print("wrote", len(os.listdir("data")), "files to data/")
