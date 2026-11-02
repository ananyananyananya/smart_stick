/*
  Smart Assistive Stick - FULL PROJECT (classic ESP32 DevKit)
  Uses YOUR trained neural network from the repo (fall_model.h: 10 inputs -> 16 -> 8 -> 1, sigmoid).
  Keep this .ino and fall_model.h in the same folder (folder name = smart_stick_full).

  Pins (GPIO numbers = the "Dxx" labels on the DevKit)
    Front ultrasonic  Trig D21, Echo D22   (echo via 1k/2k divider)
    Down  ultrasonic  Trig D4,  Echo D5    (echo via 1k/2k divider, angled 30-45 deg down)
    Water sensor      Signal D34 (analog)
    MPU6050           SDA D18, SCL D19
    Buzzer D25, Vibration motor D26 (both via transistor), Cancel button D27 (to GND)
    GPS  (optional)   GPS TX -> D16, GPS RX <- D17
    GSM  (optional)   SIM800L TX -> D32, SIM800L RX <- D33

  How it works
    - IMU runs at a fixed 50 Hz into a 100-sample (2 s) window, exactly like fall_detect.ino.
    - Every 25 samples (0.5 s) the 10 features are computed, normalised with MEAN/SCALE and fed to the network.
    - probability > THRESH (0.8) -> fall -> 15 s alarm with cancel button -> SMS with GPS link.
    - Pit / obstacle / water alerts run in a separate task so the 50 Hz sampling is never disturbed.
    - Serial Monitor (115200) prints a status line every 500 ms.
*/

#include <stdint.h>

// ---------------- USER SETTINGS ----------------
#define HAVE_GSM_GPS 0            // 0 = no GPS/GSM yet: the SMS is only printed. 1 = real SMS (needs TinyGPSPlus library)
const char EMERGENCY_NUMBER[] = "+91XXXXXXXXXX";

const float THRESH = 0.8f;        // fall probability threshold (same as your fall_detect.ino)
const int   CONFIRM_WINDOWS = 1;  // consecutive windows above THRESH needed (use 2 to cut false alarms)
const uint32_t COOLDOWN_MS = 10000;
const uint32_t RESPONSE_MS = 15000;

const int WARN_CM = 100;          // front obstacle warning distance
const int PIT_DELTA_CM = 30;      // down reading differing from ground distance by more than this = pit / step
const int WATER_THRESHOLD = 1200; // 12-bit ADC, compare the printed dry vs wet values
// -----------------------------------------------

#include <Wire.h>
#include <math.h>
#include <string.h>
#include <Arduino.h>
// The ESP32 core defines B1, B2, B3 as binary-number macros (B1 = 1 ...), which clash with the
// bias arrays B1/B2/B3 in fall_model.h. Remove those macros before including the model.
#undef B1
#undef B2
#undef B3
#include "fall_model.h"
#if HAVE_GSM_GPS
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#endif

// ---------- pins ----------
const uint8_t TRIG_F = 21, ECHO_F = 22;
const uint8_t TRIG_D = 4,  ECHO_D = 5;
const uint8_t WATER_PIN = 34;
const uint8_t SDA_PIN = 18, SCL_PIN = 19;
const uint8_t BUZZER = 25, VIBRATION = 26, BUTTON = 27;
#if HAVE_GSM_GPS
const uint8_t GPS_RX = 16, GPS_TX = 17;
const uint8_t GSM_RX = 32, GSM_TX = 33;
HardwareSerial gpsSerial(2);
HardwareSerial gsmSerial(1);
TinyGPSPlus gps;
#endif

// ---------- model window (must match training) ----------
const int W = 100;                // 2 s @ 50 Hz
const int STRIDE = 25;            // run the network every 0.5 s

const uint8_t MPU = 0x68;
float buf[W][6];                  // ax ay az (g)  gx gy gz (deg/s)
int head = 0, filled = 0, sinceRun = 0, hits = 0;
uint32_t nextSample = 0, quietUntil = 0, lastPrint = 0;
float lastP = 0;

// shared with the environment task
volatile int  frontCm = 999, downCm = 999, waterRaw = 0;
volatile bool pit = false, nearF = false, wet = false;
volatile bool busy = false;       // true during the emergency so the environment task stays quiet
int groundCm = 80;

// ---------------- MPU6050 (same settings as your fall_detect.ino) ----------------
void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool mpuInit() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.beginTransmission(MPU);
  if (Wire.endTransmission() != 0) return false;
  mpuWrite(0x6B, 0x00);   // wake
  mpuWrite(0x1A, 0x03);   // low-pass filter
  mpuWrite(0x1B, 0x08);   // gyro  +-500 deg/s
  mpuWrite(0x1C, 0x18);   // accel +-16 g
  return true;
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
    a[i] = r[i] / 2048.0f;        // +-16 g
    g[i] = r[4 + i] / 65.5f;      // +-500 deg/s
  }
  return true;
}

// ---------------- features + network (copied from your fall_detect.ino) ----------------
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

// ---------------- ultrasonic / water ----------------
int readCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long d = pulseIn(echo, HIGH, 12000);   // ~2 m max
  if (d == 0) return 999;                          // 999 = no echo
  return (int)(d / 58);
}

void calibrateGround() {
  Serial.println("Calibrating ground distance - hold the stick in its normal walking position...");
  long sum = 0; int n = 0;
  for (int i = 0; i < 10; i++) {
    int d = readCm(TRIG_D, ECHO_D);
    if (d != 999) { sum += d; n++; }
    delay(100);
  }
  groundCm = n ? (int)(sum / n) : 80;
  Serial.print("Ground distance = "); Serial.print(groundCm); Serial.println(" cm");
}

void pulseOut(unsigned long ms) {
  digitalWrite(BUZZER, HIGH); digitalWrite(VIBRATION, HIGH);
  vTaskDelay(pdMS_TO_TICKS(ms));
  digitalWrite(BUZZER, LOW);  digitalWrite(VIBRATION, LOW);
}

// runs on core 0 so the 50 Hz IMU loop on core 1 is never delayed
void envTask(void *pv) {
  int pitCount = 0;
  for (;;) {
    if (busy) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    int f = readCm(TRIG_F, ECHO_F);
    vTaskDelay(pdMS_TO_TICKS(30));               // avoid echo cross-talk between the two sensors
    int d = readCm(TRIG_D, ECHO_D);
    int w = analogRead(WATER_PIN);

    bool pitNow = (d == 999) || (abs(d - groundCm) > PIT_DELTA_CM);
    pitCount = pitNow ? pitCount + 1 : 0;
    bool p  = (pitCount >= 2);                   // two readings in a row, ignores stick wobble
    bool nf = (f >= 2 && f < WARN_CM);
    bool wt = (w > WATER_THRESHOLD);

    frontCm = f; downCm = d; waterRaw = w;
    pit = p; nearF = nf; wet = wt;

    if (p) {                                     // priority 1: pit / stairs / step
      for (int i = 0; i < 2 && !busy; i++) {
        pulseOut(400);
        vTaskDelay(pdMS_TO_TICKS(150));
      }
      vTaskDelay(pdMS_TO_TICKS(300));
    } else if (nf) {                             // priority 2: front obstacle
      pulseOut(70);
      vTaskDelay(pdMS_TO_TICKS(map(constrain(f, 20, WARN_CM), 20, WARN_CM, 80, 500)));
    } else if (wt) {                             // priority 3: water
      pulseOut(150);
      vTaskDelay(pdMS_TO_TICKS(1050));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ---------------- emergency ----------------
void beepBlocking(unsigned long ms) {
  digitalWrite(BUZZER, HIGH); digitalWrite(VIBRATION, HIGH);
  delay(ms);
  digitalWrite(BUZZER, LOW);  digitalWrite(VIBRATION, LOW);
}

void pollGps() {
#if HAVE_GSM_GPS
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
#endif
}

void sendEmergencySms() {
#if HAVE_GSM_GPS
  while (gsmSerial.available()) gsmSerial.read();
  gsmSerial.println(F("AT"));          delay(500);
  gsmSerial.println(F("AT+CMGF=1"));   delay(500);
  while (gsmSerial.available()) gsmSerial.read();

  gsmSerial.print(F("AT+CMGS=\""));
  gsmSerial.print(EMERGENCY_NUMBER);
  gsmSerial.println(F("\""));
  delay(1500);

  gsmSerial.print(F("FALL DETECTED! The user may need help. "));
  if (gps.location.isValid()) {
    gsmSerial.print(F("Location: https://maps.google.com/?q="));
    gsmSerial.print(gps.location.lat(), 6);
    gsmSerial.print(',');
    gsmSerial.print(gps.location.lng(), 6);
    if (gps.location.age() > 60000UL) gsmSerial.print(F(" (last known)"));
  } else {
    gsmSerial.print(F("Location unavailable (no GPS fix)."));
  }
  gsmSerial.write(26);                            // Ctrl+Z sends the SMS
  delay(5000);
  Serial.println(">>> Emergency SMS sent");
#else
  Serial.println(">>> No response: EMERGENCY SMS WOULD BE SENT HERE (GPS/GSM not enabled)");
#endif
}

void triggerEmergency() {
  busy = true;
  Serial.println("*** FALL DETECTED *** alarm on - press the cancel button within 15 s");
  uint32_t start = millis(), lastToggle = 0, lastSec = 0;
  bool on = false, cancelled = false;

  while (millis() - start < RESPONSE_MS) {
    if (millis() - lastToggle >= 250) {
      on = !on;
      digitalWrite(BUZZER, on);
      digitalWrite(VIBRATION, on);
      lastToggle = millis();
    }
    if (millis() - lastSec >= 1000) {
      lastSec = millis();
      Serial.print("  time left: ");
      Serial.print((RESPONSE_MS - (millis() - start)) / 1000);
      Serial.println(" s");
    }
    pollGps();
    if (digitalRead(BUTTON) == LOW) { cancelled = true; break; }
  }
  digitalWrite(BUZZER, LOW);
  digitalWrite(VIBRATION, LOW);

  if (cancelled) {
    Serial.println(">>> Alert CANCELLED by user");
    beepBlocking(100); delay(150); beepBlocking(100);
  } else {
    sendEmergencySms();
  }

  filled = 0; sinceRun = 0; hits = 0;             // fresh window after the event
  quietUntil = millis() + COOLDOWN_MS;
  nextSample = micros();                          // do not try to catch up the blocked time
  busy = false;
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Smart Stick (trained neural network) ===");

  pinMode(TRIG_F, OUTPUT); pinMode(ECHO_F, INPUT);
  pinMode(TRIG_D, OUTPUT); pinMode(ECHO_D, INPUT);
  pinMode(WATER_PIN, INPUT);
  pinMode(BUZZER, OUTPUT); pinMode(VIBRATION, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  digitalWrite(BUZZER, LOW); digitalWrite(VIBRATION, LOW);
  analogReadResolution(12);

  if (mpuInit()) Serial.println("MPU6050 found");
  else Serial.println("ERROR: MPU6050 not found at 0x68 - check SDA=D18, SCL=D19, power and GND");

#if HAVE_GSM_GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
#endif

  beepBlocking(150);                              // power-on beep
  calibrateGround();                              // hold the stick in its normal walking position now
  beepBlocking(150);                              // ready

  xTaskCreatePinnedToCore(envTask, "env", 4096, NULL, 1, NULL, 0);
  nextSample = micros();
  Serial.println("Ready. Status line prints every 500 ms.\n");
}

void loop() {
  uint32_t nowMs = millis();
  pollGps();

  // status line for testing
  if (nowMs - lastPrint >= 500) {
    lastPrint = nowMs;
    Serial.print("FRONT=");  Serial.print(frontCm == 999 ? -1 : (int)frontCm); Serial.print("cm");
    Serial.print(" | DOWN="); Serial.print(downCm == 999 ? -1 : (int)downCm);
    Serial.print("cm (ground "); Serial.print(groundCm); Serial.print(")");
    Serial.print(" | WATER="); Serial.print((int)waterRaw); Serial.print(wet ? " WET" : " dry");
    Serial.print(" | FALLp="); Serial.print(lastP, 3);
    Serial.print(" | ");
    if (pit)        Serial.print("PIT/STEP!");
    else if (nearF) Serial.print("OBSTACLE!");
    else if (wet)   Serial.print("WATER!");
    else            Serial.print("clear");
    Serial.println();
  }

  // fixed 50 Hz IMU sampling
  if ((int32_t)(micros() - nextSample) < 0) return;
  nextSample += 20000;

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
  lastP = p;
  if (p > 0.5f) {                                  // only print interesting windows immediately
    Serial.print("  window probability = ");
    Serial.println(p, 3);
  }

  if (p > THRESH && (int32_t)(nowMs - quietUntil) >= 0) hits++;
  else hits = 0;

  if (hits >= CONFIRM_WINDOWS) {
    hits = 0;
    triggerEmergency();
  }
}
