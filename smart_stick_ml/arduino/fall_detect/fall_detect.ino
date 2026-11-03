#include <Wire.h>
#include <math.h>
#include <string.h>
#include "fall_forest.h"
#include "fall_features.h"

const uint8_t MPU = 0x68;
const int BUZZER = 25;
const int MOTOR = 26;
const int W = FALL_WINDOW;           // 100 samples = 2 s at 50 Hz
const int STRIDE = 25;               // classify every 0.5 s
const uint32_t ALERT_MS = 5000;
const uint32_t COOLDOWN_MS = 10000;

float buf[W][6];                     // circular buffer: ax ay az (g)  gx gy gz (deg/s)
int head = 0;                        // index of the oldest sample
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
  mpuWrite(0x6B, 0x00);   // wake up
  mpuWrite(0x1A, 0x03);   // low-pass filter
  mpuWrite(0x1B, 0x08);   // gyro  +-500 deg/s
  mpuWrite(0x1C, 0x18);   // accel +-16 g
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

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  pinMode(MOTOR, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MOTOR, LOW);
  mpuInit();
  Serial.print("Random-forest fall detector. Trees: ");
  Serial.print(FALL_N_TREES);
  Serial.print("  alarm cutoff: ");
  Serial.println(FALL_ALARM_PROB, 3);
}

void loop() {
  static uint32_t next = micros();
  uint32_t nowMs = millis();

  if (alerting && (int32_t)(nowMs - alertUntil) >= 0) {
    digitalWrite(BUZZER, LOW);
    digitalWrite(MOTOR, LOW);
    alerting = false;
  }

  if ((int32_t)(micros() - next) < 0) return;      // wait for the next 50 Hz tick
  next += 20000;

  float a[3], g[3];
  if (!mpuRead(a, g)) return;
  memcpy(buf[head], a, sizeof(a));
  memcpy(buf[head] + 3, g, sizeof(g));
  head = (head + 1) % W;
  if (filled < W) filled++;

  if (++sinceRun < STRIDE || filled < W) return;
  sinceRun = 0;

  float f[FALL_N_FEATURES];
  fallFeatures(buf, head, f);
  float p = fallProbability(f);
  Serial.println(p, 3);

  if (p > FALL_ALARM_PROB && (int32_t)(nowMs - quietUntil) >= 0) {
    digitalWrite(BUZZER, HIGH);
    digitalWrite(MOTOR, HIGH);
    alerting = true;
    alertUntil = nowMs + ALERT_MS;
    quietUntil = nowMs + COOLDOWN_MS;
  }
}
