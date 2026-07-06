/*
 * I2C Scanner for ESP32 — finds the address of the OLED.
 * Open Serial Monitor at 115200 baud after uploading.
 * Expected OLED address is 0x3C (sometimes 0x3D).
 */
#include <Wire.h>

#define I2C_SDA 21
#define I2C_SCL 22

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("\nI2C scanner started.");
}

void loop() {
  byte count = 0;
  Serial.println("Scanning...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      count++;
    }
  }
  if (count == 0) Serial.println("  No I2C devices found — check wiring/power.");
  Serial.println();
  delay(3000);
}
