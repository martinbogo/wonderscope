#include "rtc.h"

#include <Wire.h>
#include <sys/time.h>
#include <time.h>

static const uint8_t ADDR = 0x51;
static bool present;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtc_present() { return present; }

bool rtc_begin() {
  Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 400000);
  Wire.beginTransmission(ADDR);
  present = Wire.endTransmission() == 0;
  if (!present) return false;
  Wire.beginTransmission(ADDR);
  Wire.write(0x04);  // seconds register
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR, 7) != 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) return false;  // OS flag: oscillator stopped, time invalid
  struct tm t = {};
  t.tm_sec = bcd2bin(r[0] & 0x7F);
  t.tm_min = bcd2bin(r[1] & 0x7F);
  t.tm_hour = bcd2bin(r[2] & 0x3F);
  t.tm_mday = bcd2bin(r[3] & 0x3F);
  t.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
  t.tm_year = bcd2bin(r[6]) + 100;
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t secs = mktime(&t);
  if (secs < 1700000000) return false;
  struct timeval tv = {secs, 0};
  settimeofday(&tv, nullptr);
  return true;
}

void rtc_write_now() {
  if (!present || !time_valid()) return;
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  Wire.beginTransmission(ADDR);
  Wire.write(0x04);
  Wire.write(bin2bcd(t.tm_sec));  // clears OS flag
  Wire.write(bin2bcd(t.tm_min));
  Wire.write(bin2bcd(t.tm_hour));
  Wire.write(bin2bcd(t.tm_mday));
  Wire.write(t.tm_wday);
  Wire.write(bin2bcd(t.tm_mon + 1));
  Wire.write(bin2bcd(t.tm_year % 100));
  Wire.endTransmission();
}
