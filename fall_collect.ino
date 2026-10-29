#include <Wire.h>

const uint8_t MPU = 0x68;

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

void setup() {
  Serial.begin(115200);
  mpuInit();
}

void loop() {
  static uint32_t next = micros();
  if ((int32_t)(micros() - next) < 0) return;
  next += 20000;
  float a[3], g[3];
  if (!mpuRead(a, g)) return;
  Serial.printf("%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n", a[0], a[1], a[2], g[0], g[1], g[2]);
}
