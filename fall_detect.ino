#include <Wire.h>
#include <math.h>
#include <string.h>
#include "fall_model.h"

const uint8_t MPU = 0x68;
const int BUZZER = 25;
const int MOTOR = 26;
const int W = 100;
const int STRIDE = 25;
const float THRESH = 0.8f;
const uint32_t ALERT_MS = 5000;
const uint32_t COOLDOWN_MS = 10000;

float buf[W][6];
int head = 0;
int filled = 0;
int sinceRun = 0;
bool alerting = false;
uint32_t alertUntil = 0;
uint32_t quietUntil = 0;

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void mpuInit() {
  Wire.begin(21, 22);
  Wire.setClock(400000);
  mpuWrite(0x6B, 0x00);
  mpuWrite(0x1A, 0x03);
  mpuWrite(0x1B, 0x08);
  mpuWrite(0x1C, 0x18);
}

bool mpuRead(float *a, float *g) {
  Wire.beginTransmission(MPU);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU, (uint8_t)14) != 14) return false;
  int16_t r[7];
  for (int i = 0; i < 7; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    r[i] = (int16_t)((hi << 8) | lo);
  }
  for (int i = 0; i < 3; i++) {
    a[i] = r[i] / 2048.0f;
    g[i] = r[4 + i] / 65.5f;
  }
  return true;
}

void features(float *f) {
  float a[W], g[W];
  float aSum = 0, gSum = 0, tSum = 0;
  float aMax = -1e9f, aMin = 1e9f, gMax = 0;
  int ff = 0;
  for (int k = 0; k < W; k++) {
    float *s = buf[(head + k) % W];
    a[k] = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    g[k] = sqrtf(s[3] * s[3] + s[4] * s[4] + s[5] * s[5]);
    aSum += a[k];
    gSum += g[k];
    if (a[k] > aMax) aMax = a[k];
    if (a[k] < aMin) aMin = a[k];
    if (g[k] > gMax) gMax = g[k];
    if (a[k] < 0.6f) ff++;
    if (k >= W - 25) tSum += a[k];
  }
  float aMean = aSum / W, gMean = gSum / W, tMean = tSum / 25.0f;
  float aVar = 0, gVar = 0, tVar = 0;
  for (int k = 0; k < W; k++) {
    aVar += (a[k] - aMean) * (a[k] - aMean);
    gVar += (g[k] - gMean) * (g[k] - gMean);
    if (k >= W - 25) tVar += (a[k] - tMean) * (a[k] - tMean);
  }
  f[0] = aMean;
  f[1] = sqrtf(aVar / W);
  f[2] = aMax;
  f[3] = aMin;
  f[4] = gMean;
  f[5] = sqrtf(gVar / W);
  f[6] = gMax;
  f[7] = sqrtf(tVar / 25.0f);
  f[8] = tMean;
  f[9] = (float)ff;
}

float predict(const float *x) {
  float in[10], h1[16], h2[8];
  for (int i = 0; i < 10; i++) in[i] = (x[i] - MEAN[i]) / SCALE[i];
  for (int j = 0; j < 16; j++) {
    float s = B1[j];
    for (int i = 0; i < 10; i++) s += in[i] * W1[i][j];
    h1[j] = s > 0 ? s : 0;
  }
  for (int j = 0; j < 8; j++) {
    float s = B2[j];
    for (int i = 0; i < 16; i++) s += h1[i] * W2[i][j];
    h2[j] = s > 0 ? s : 0;
  }
  float s = B3[0];
  for (int i = 0; i < 8; i++) s += h2[i] * W3[i][0];
  return 1.0f / (1.0f + expf(-s));
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  pinMode(MOTOR, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MOTOR, LOW);
  mpuInit();
}

void loop() {
  static uint32_t next = micros();
  uint32_t nowMs = millis();

  if (alerting && (int32_t)(nowMs - alertUntil) >= 0) {
    digitalWrite(BUZZER, LOW);
    digitalWrite(MOTOR, LOW);
    alerting = false;
  }

  if ((int32_t)(micros() - next) < 0) return;
  next += 20000;

  float a[3], g[3];
  if (!mpuRead(a, g)) return;
  memcpy(buf[head], a, sizeof(a));
  memcpy(buf[head] + 3, g, sizeof(g));
  head = (head + 1) % W;
  if (filled < W) filled++;

  if (++sinceRun < STRIDE || filled < W) return;
  sinceRun = 0;

  float f[10];
  features(f);
  float p = predict(f);
  Serial.println(p, 3);

  if (p > THRESH && (int32_t)(nowMs - quietUntil) >= 0) {
    digitalWrite(BUZZER, HIGH);
    digitalWrite(MOTOR, HIGH);
    alerting = true;
    alertUntil = nowMs + ALERT_MS;
    quietUntil = nowMs + COOLDOWN_MS;
  }
}
