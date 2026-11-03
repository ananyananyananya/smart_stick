#pragma once

#include <math.h>

#define FALL_WINDOW 100
#define FALL_EDGE   10

static inline void fallFeatures(const float (*buf)[6], int head, float *f) {
  float a[FALL_WINDOW], w[FALL_WINDOW];
  float aSum = 0, wSum = 0, aMax = -1e9f, aMin = 1e9f, wMax = 0, jerk = 0;
  float ux = 0, uy = 0, uz = 0, vx = 0, vy = 0, vz = 0;
  int p = 0;

  for (int k = 0; k < FALL_WINDOW; k++) {
    const float *s = buf[(head + k) % FALL_WINDOW];
    a[k] = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    w[k] = sqrtf(s[3] * s[3] + s[4] * s[4] + s[5] * s[5]);
    aSum += a[k];
    wSum += w[k];
    if (a[k] > aMax) { aMax = a[k]; p = k; }
    if (a[k] < aMin) aMin = a[k];
    if (w[k] > wMax) wMax = w[k];
    if (k > 0) {
      float d = fabsf(a[k] - a[k - 1]);
      if (d > jerk) jerk = d;
    }
    if (k < FALL_EDGE)               { ux += s[0]; uy += s[1]; uz += s[2]; }
    if (k >= FALL_WINDOW - FALL_EDGE) { vx += s[0]; vy += s[1]; vz += s[2]; }
  }
  float aMean = aSum / FALL_WINDOW;
  float wMean = wSum / FALL_WINDOW;
  float aVar = 0;
  for (int k = 0; k < FALL_WINDOW; k++) aVar += (a[k] - aMean) * (a[k] - aMean);

  int ps = p + 10;
  if (ps > FALL_WINDOW - 5) ps = FALL_WINDOW - 5;
  int n = FALL_WINDOW - ps;
  float pm = 0, pw = 0;
  for (int k = ps; k < FALL_WINDOW; k++) { pm += a[k]; pw += w[k]; }
  pm /= n;
  pw /= n;
  float pv = 0;
  for (int k = ps; k < FALL_WINDOW; k++) pv += (a[k] - pm) * (a[k] - pm);

  float preMin = 1e9f;
  for (int k = 0; k <= p; k++) if (a[k] < preMin) preMin = a[k];

  ux /= FALL_EDGE; uy /= FALL_EDGE; uz /= FALL_EDGE;
  vx /= FALL_EDGE; vy /= FALL_EDGE; vz /= FALL_EDGE;
  float c = (ux * vx + uy * vy + uz * vz) /
            (sqrtf(ux * ux + uy * uy + uz * uz) * sqrtf(vx * vx + vy * vy + vz * vz) + 1e-9f);
  if (c > 1.0f) c = 1.0f;
  if (c < -1.0f) c = -1.0f;

  f[0]  = aMax;
  f[1]  = aMin;
  f[2]  = aMean;
  f[3]  = sqrtf(aVar / FALL_WINDOW);
  f[4]  = wMax;
  f[5]  = wMean;
  f[6]  = jerk;
  f[7]  = (float)p / (FALL_WINDOW - 1);
  f[8]  = sqrtf(pv / n);
  f[9]  = fabsf(pm - 1.0f);
  f[10] = preMin;
  f[11] = acosf(c) * 57.29578f;
  f[12] = pw;
}