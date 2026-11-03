#include <stdint.h>

#define HAVE_GSM_GPS 0
const char EMERGENCY_NUMBER[] = "+91XXXXXXXXXX";

const int   CONFIRM_WINDOWS = 1;
const uint32_t COOLDOWN_MS = 10000;
const uint32_t RESPONSE_MS = 15000;

const int WARN_CM = 100;
const int PIT_DELTA_CM = 30;
const int WATER_THRESHOLD = 1200;

#include <Wire.h>
#include <math.h>
#include <string.h>
#include <Arduino.h>
#include "fall_forest.h"
#include "fall_features.h"
#if HAVE_GSM_GPS
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#endif

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

const int W = 100;
const int STRIDE = 25;
static_assert(W == FALL_WINDOW, "window length must equal the one used in fall_features.h / training");

const uint8_t MPU = 0x68;
float buf[W][6];
int head = 0, filled = 0, sinceRun = 0, hits = 0;
uint32_t nextSample = 0, quietUntil = 0, lastPrint = 0;
float lastP = 0;

volatile int  frontCm = 999, downCm = 999, waterRaw = 0;
volatile bool pit = false, nearF = false, wet = false;
volatile bool busy = false;
int groundCm = 80;

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
  mpuWrite(0x6B, 0x00);
  mpuWrite(0x1A, 0x03);
  mpuWrite(0x1B, 0x08);
  mpuWrite(0x1C, 0x18);
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
    a[i] = r[i] / 2048.0f;
    g[i] = r[4 + i] / 65.5f;
  }
  return true;
}

int readCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long d = pulseIn(echo, HIGH, 12000);
  if (d == 0) return 999;
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

void envTask(void *pv) {
  int pitCount = 0;
  for (;;) {
    if (busy) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    int f = readCm(TRIG_F, ECHO_F);
    vTaskDelay(pdMS_TO_TICKS(30));
    int d = readCm(TRIG_D, ECHO_D);
    int w = analogRead(WATER_PIN);

    bool pitNow = (d == 999) || (abs(d - groundCm) > PIT_DELTA_CM);
    pitCount = pitNow ? pitCount + 1 : 0;
    bool p  = (pitCount >= 2);
    bool nf = (f >= 2 && f < WARN_CM);
    bool wt = (w > WATER_THRESHOLD);

    frontCm = f; downCm = d; waterRaw = w;
    pit = p; nearF = nf; wet = wt;

    if (p) {
      for (int i = 0; i < 2 && !busy; i++) {
        pulseOut(400);
        vTaskDelay(pdMS_TO_TICKS(150));
      }
      vTaskDelay(pdMS_TO_TICKS(300));
    } else if (nf) {
      pulseOut(70);
      vTaskDelay(pdMS_TO_TICKS(map(constrain(f, 20, WARN_CM), 20, WARN_CM, 80, 500)));
    } else if (wt) {
      pulseOut(150);
      vTaskDelay(pdMS_TO_TICKS(1050));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

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
  gsmSerial.write(26);
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

  filled = 0; sinceRun = 0; hits = 0;
  quietUntil = millis() + COOLDOWN_MS;
  nextSample = micros();
  busy = false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Smart Stick (learned random-forest fall detector) ===");

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

  beepBlocking(150);
  calibrateGround();
  beepBlocking(150);

  xTaskCreatePinnedToCore(envTask, "env", 4096, NULL, 1, NULL, 0);
  nextSample = micros();
  Serial.println("Ready. Status line prints every 500 ms.\n");
}

void loop() {
  uint32_t nowMs = millis();
  pollGps();

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

  float f[FALL_N_FEATURES];
  fallFeatures(buf, head, f);
  float p = fallProbability(f);
  lastP = p;
  if (p > 0.5f) {
    Serial.print("  window probability = ");
    Serial.println(p, 3);
  }

  if (p > FALL_ALARM_PROB && (int32_t)(nowMs - quietUntil) >= 0) hits++;
  else hits = 0;

  if (hits >= CONFIRM_WINDOWS) {
    hits = 0;
    triggerEmergency();
  }
}