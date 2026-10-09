#include "i2c_drivers.h"

const DriverInfo DRIVERS[] = {
    {"bme280", "BME280 temperature, humidity, pressure", "0x76 0x77"},
    {"bmp280", "BMP280 temperature, pressure", "0x76 0x77"},
    {"sht3x", "SHT3x temperature, humidity", "0x44 0x45"},
    {"sht4x", "SHT4x temperature, humidity", "0x44 0x45"},
    {"aht20", "AHT20 / AHT21 temperature, humidity", "0x38"},
    {"bh1750", "BH1750 ambient light", "0x23 0x5C"},
    {"tmp102", "TMP102 temperature", "0x48-0x4B"},
    {"mcp9808", "MCP9808 temperature", "0x18-0x1F"},
    {"ina219", "INA219 bus voltage, current (0.1 ohm shunt)", "0x40-0x4F"},
    {"mpu6050", "MPU-6050 accelerometer, gyroscope", "0x68 0x69"},
    {"mpu6500", "MPU-6500 / MPU-9250 accelerometer, gyroscope", "0x68 0x69"},
    {"scd4x", "SCD40 / SCD41 CO2, temperature, humidity", "0x62"},
};
const size_t DRIVER_COUNT = sizeof(DRIVERS) / sizeof(DRIVERS[0]);

bool driver_known(const char *id) {
  if (!id || !*id) return true;  // empty = no driver
  for (size_t i = 0; i < DRIVER_COUNT; i++)
    if (!strcmp(DRIVERS[i].id, id)) return true;
  return false;
}

// ------------------------------------------------------------------ helpers

static uint8_t crc8_sensirion(const uint8_t *d, size_t n) {
  uint8_t c = 0xFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x31 : (c << 1);
  }
  return c;
}

static bool cmd16(const I2cDev &d, uint16_t cmd) {
  uint8_t b[2] = {(uint8_t)(cmd >> 8), (uint8_t)cmd};
  return xi2c_write(d, b, 2);
}

static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void set(DevValue &v, const char *name, const char *unit, float x) {
  strlcpy(v.name, name, sizeof(v.name));
  strlcpy(v.unit, unit, sizeof(v.unit));
  v.v = x;
}

// Per-device driver state (calibration data, one-time init)
struct DrvState {
  bool used;
  uint8_t bus, addr;
  char drv[12];
  bool init;
  uint8_t data[40];
};
static DrvState states[16];

static DrvState &state_for(const I2cDev &d, const char *drv) {
  for (auto &s : states)
    if (s.used && s.bus == d.bus && s.addr == d.addr && !strcmp(s.drv, drv)) return s;
  for (auto &s : states)
    if (s.used && s.bus == d.bus && s.addr == d.addr) {  // driver changed
      memset(&s, 0, sizeof(s));
      s.used = true;
      s.bus = d.bus;
      s.addr = d.addr;
      strlcpy(s.drv, drv, sizeof(s.drv));
      return s;
    }
  static int next;
  DrvState *s = nullptr;
  for (auto &x : states)
    if (!x.used) {
      s = &x;
      break;
    }
  if (!s) s = &states[next++ % 16];
  memset(s, 0, sizeof(*s));
  s->used = true;
  s->bus = d.bus;
  s->addr = d.addr;
  strlcpy(s->drv, drv, sizeof(s->drv));
  return *s;
}

void driver_reset(const I2cDev &d) {
  for (auto &s : states)
    if (s.used && s.bus == d.bus && s.addr == d.addr) s.used = false;
}

// ------------------------------------------------------------------ BME280 / BMP280

struct BmeCal {
  uint16_t T1;
  int16_t T2, T3;
  uint16_t P1;
  int16_t P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t H1, H3;
  int16_t H2, H4, H5;
  int8_t H6;
};
static_assert(sizeof(BmeCal) <= 40, "calibration must fit DrvState");

static int bme280_read(const I2cDev &d, DevValue *out, int max, bool humidity) {
  DrvState &st = state_for(d, humidity ? "bme280" : "bmp280");
  BmeCal &c = *(BmeCal *)st.data;
  if (!st.init) {
    uint8_t b[26];
    if (!xi2c_reg_read(d, 0x88, b, 26)) return -1;
    c.T1 = le16(b);
    c.T2 = le16(b + 2);
    c.T3 = le16(b + 4);
    c.P1 = le16(b + 6);
    c.P2 = le16(b + 8);
    c.P3 = le16(b + 10);
    c.P4 = le16(b + 12);
    c.P5 = le16(b + 14);
    c.P6 = le16(b + 16);
    c.P7 = le16(b + 18);
    c.P8 = le16(b + 20);
    c.P9 = le16(b + 22);
    c.H1 = b[25];
    if (humidity) {
      uint8_t h[7];
      if (!xi2c_reg_read(d, 0xE1, h, 7)) return -1;
      c.H2 = le16(h);
      c.H3 = h[2];
      c.H4 = (int16_t)((int8_t)h[3] * 16 | (h[4] & 0x0F));
      c.H5 = (int16_t)((int8_t)h[5] * 16 | (h[4] >> 4));
      c.H6 = (int8_t)h[6];
    }
    st.init = true;
  }
  // Humidity oversampling x1 (rewritten each time: survives a sensor reset).
  // ctrl_hum takes effect on the following ctrl_meas write.
  if (humidity && !xi2c_reg_write(d, 0xF2, 0x01)) return -1;
  // forced mode, temperature and pressure oversampling x1
  if (!xi2c_reg_write(d, 0xF4, 0x25)) return -1;
  delay(10);
  uint8_t r[8];
  if (!xi2c_reg_read(d, 0xF7, r, humidity ? 8 : 6)) return -1;
  int32_t adcP = (r[0] << 12) | (r[1] << 4) | (r[2] >> 4);
  int32_t adcT = (r[3] << 12) | (r[4] << 4) | (r[5] >> 4);
  if (adcT == 0x80000) return -2;  // measurement skipped / not ready
  double v1 = (adcT / 16384.0 - c.T1 / 1024.0) * c.T2;
  double v2 = (adcT / 131072.0 - c.T1 / 8192.0) * (adcT / 131072.0 - c.T1 / 8192.0) * c.T3;
  double tFine = v1 + v2;
  double T = tFine / 5120.0;
  v1 = tFine / 2.0 - 64000.0;
  v2 = v1 * v1 * c.P6 / 32768.0;
  v2 = v2 + v1 * c.P5 * 2.0;
  v2 = v2 / 4.0 + c.P4 * 65536.0;
  v1 = (c.P3 * v1 * v1 / 524288.0 + c.P2 * v1) / 524288.0;
  v1 = (1.0 + v1 / 32768.0) * c.P1;
  double P = 0;
  if (v1 != 0) {
    P = 1048576.0 - adcP;
    P = (P - v2 / 4096.0) * 6250.0 / v1;
    v1 = c.P9 * P * P / 2147483648.0;
    v2 = P * c.P8 / 32768.0;
    P = P + (v1 + v2 + c.P7) / 16.0;
  }
  int n = 0;
  if (n < max) set(out[n++], "temperature", "°C", T);
  if (n < max) set(out[n++], "pressure", "hPa", P / 100.0);
  int32_t adcH = humidity ? (r[6] << 8) | r[7] : 0;
  if (humidity && adcH != 0x8000 && n < max) {  // 0x8000: humidity measurement skipped
    double h = tFine - 76800.0;
    h = (adcH - (c.H4 * 64.0 + c.H5 / 16384.0 * h)) *
        (c.H2 / 65536.0 * (1.0 + c.H6 / 67108864.0 * h * (1.0 + c.H3 / 67108864.0 * h)));
    h = h * (1.0 - c.H1 * h / 524288.0);
    if (h < 0) h = 0;
    if (h > 100) h = 100;
    set(out[n++], "humidity", "%", h);
  }
  return n;
}

// ------------------------------------------------------------------ Sensirion / Aosong humidity sensors

static int sht_decode(const uint8_t *b, DevValue *out, int max, bool sht4) {
  if (crc8_sensirion(b, 2) != b[2] || crc8_sensirion(b + 3, 2) != b[5]) return -3;
  float t = -45.0f + 175.0f * be16(b) / 65535.0f;
  float rh = sht4 ? (-6.0f + 125.0f * be16(b + 3) / 65535.0f) : (100.0f * be16(b + 3) / 65535.0f);
  if (rh < 0) rh = 0;
  if (rh > 100) rh = 100;
  int n = 0;
  if (n < max) set(out[n++], "temperature", "°C", t);
  if (n < max) set(out[n++], "humidity", "%", rh);
  return n;
}

static int sht3x_read(const I2cDev &d, DevValue *out, int max) {
  if (!cmd16(d, 0x2400)) return -1;  // single shot, high repeatability, no clock stretching
  delay(16);
  uint8_t b[6];
  if (!xi2c_read(d, b, 6)) return -1;
  return sht_decode(b, out, max, false);
}

static int sht4x_read(const I2cDev &d, DevValue *out, int max) {
  uint8_t c = 0xFD;  // high precision measurement
  if (!xi2c_write(d, &c, 1)) return -1;
  delay(10);
  uint8_t b[6];
  if (!xi2c_read(d, b, 6)) return -1;
  return sht_decode(b, out, max, true);
}

static int aht20_read(const I2cDev &d, DevValue *out, int max) {
  DrvState &st = state_for(d, "aht20");
  if (!st.init) {
    uint8_t s;
    if (!xi2c_read(d, &s, 1)) return -1;
    if (!(s & 0x08)) {  // not calibrated: send initialization
      uint8_t ini[3] = {0xBE, 0x08, 0x00};
      xi2c_write(d, ini, 3);
      delay(10);
    }
    st.init = true;
  }
  uint8_t trig[3] = {0xAC, 0x33, 0x00};
  if (!xi2c_write(d, trig, 3)) return -1;
  delay(80);
  uint8_t b[6];
  if (!xi2c_read(d, b, 6)) return -1;
  if (b[0] & 0x80) return 0;  // still busy
  uint32_t rh = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t t = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  int n = 0;
  if (n < max) set(out[n++], "temperature", "°C", t * 200.0f / 1048576.0f - 50.0f);
  if (n < max) set(out[n++], "humidity", "%", rh * 100.0f / 1048576.0f);
  return n;
}

static int scd4x_read(const I2cDev &d, DevValue *out, int max) {
  DrvState &st = state_for(d, "scd4x");
  if (!st.init) {
    // The sensor ignores commands while measuring (e.g. after an ESP32 reboot
    // with the sensor still powered): stop first, then start.
    cmd16(d, 0x3F86);  // stop periodic measurement
    delay(500);
    if (!cmd16(d, 0x21B1)) return -1;  // start periodic measurement (5 s interval)
    st.init = true;
    return 0;
  }
  uint8_t r[9];
  if (!cmd16(d, 0xE4B8)) return -1;  // get data ready status
  delay(1);
  if (!xi2c_read(d, r, 3) || crc8_sensirion(r, 2) != r[2]) return -1;
  if ((be16(r) & 0x07FF) == 0) return 0;
  if (!cmd16(d, 0xEC05)) return -1;  // read measurement
  delay(1);
  if (!xi2c_read(d, r, 9)) return -1;
  for (int i = 0; i < 9; i += 3)
    if (crc8_sensirion(r + i, 2) != r[i + 2]) return -3;
  int n = 0;
  if (n < max) set(out[n++], "CO2", "ppm", be16(r));
  if (n < max) set(out[n++], "temperature", "°C", -45.0f + 175.0f * be16(r + 3) / 65535.0f);
  if (n < max) set(out[n++], "humidity", "%", 100.0f * be16(r + 6) / 65535.0f);
  return n;
}

// ------------------------------------------------------------------ light, temperature, power

static int bh1750_read(const I2cDev &d, DevValue *out, int max) {
  DrvState &st = state_for(d, "bh1750");
  if (!st.init) {
    uint8_t on = 0x01, cont = 0x10;  // power on, continuous high resolution
    if (!xi2c_write(d, &on, 1) || !xi2c_write(d, &cont, 1)) return -1;
    st.init = true;
    delay(180);
  }
  uint8_t b[2];
  if (!xi2c_read(d, b, 2)) return -1;
  if (max > 0) set(out[0], "light", "lx", be16(b) / 1.2f);
  return max > 0 ? 1 : 0;
}

static int tmp102_read(const I2cDev &d, DevValue *out, int max) {
  uint8_t b[2];
  if (!xi2c_reg_read(d, 0x00, b, 2)) return -1;
  int16_t raw = (int16_t)be16(b);
  float t = (b[1] & 0x01) ? (raw >> 3) * 0.0625f : (raw >> 4) * 0.0625f;  // extended mode bit
  if (max > 0) set(out[0], "temperature", "°C", t);
  return max > 0 ? 1 : 0;
}

static int mcp9808_read(const I2cDev &d, DevValue *out, int max) {
  uint8_t b[2];
  if (!xi2c_reg_read(d, 0x05, b, 2)) return -1;
  uint8_t hi = b[0] & 0x1F;
  float t = (hi & 0x0F) * 16.0f + b[1] / 16.0f;
  if (hi & 0x10) t -= 256.0f;
  if (max > 0) set(out[0], "temperature", "°C", t);
  return max > 0 ? 1 : 0;
}

static int ina219_read(const I2cDev &d, DevValue *out, int max) {
  uint8_t s[2], b[2];
  if (!xi2c_reg_read(d, 0x01, s, 2) || !xi2c_reg_read(d, 0x02, b, 2)) return -1;
  float shunt_mV = (int16_t)be16(s) * 0.01f;
  float bus_V = (be16(b) >> 3) * 0.004f;
  int n = 0;
  if (n < max) set(out[n++], "bus voltage", "V", bus_V);
  if (n < max) set(out[n++], "shunt voltage", "mV", shunt_mV);
  if (n < max) set(out[n++], "current", "mA", shunt_mV / 0.1f);
  return n;
}

static int mpu_read(const I2cDev &d, DevValue *out, int max, bool is6500) {
  DrvState &st = state_for(d, is6500 ? "mpu6500" : "mpu6050");
  if (!st.init) {
    if (!xi2c_reg_write(d, 0x6B, 0x00)) return -1;  // wake from sleep
    delay(50);
    st.init = true;
  }
  uint8_t b[14];
  if (!xi2c_reg_read(d, 0x3B, b, 14)) return -1;
  int16_t ax = be16(b), ay = be16(b + 2), az = be16(b + 4), tr = be16(b + 6);
  int16_t gx = be16(b + 8), gy = be16(b + 10), gz = be16(b + 12);
  const char *names[] = {"accel x", "accel y", "accel z", "gyro x", "gyro y", "gyro z"};
  float vals[] = {ax / 16384.0f, ay / 16384.0f, az / 16384.0f, gx / 131.0f, gy / 131.0f, gz / 131.0f};
  int n = 0;
  for (int i = 0; i < 6 && n < max; i++) set(out[n++], names[i], i < 3 ? "g" : "°/s", vals[i]);
  if (n < max) set(out[n++], "temperature", "°C", is6500 ? tr / 333.87f + 21.0f : tr / 340.0f + 36.53f);
  return n;
}

int driver_read(const char *drv, const I2cDev &d, DevValue *out, int max) {
  if (!strcmp(drv, "bme280")) return bme280_read(d, out, max, true);
  if (!strcmp(drv, "bmp280")) return bme280_read(d, out, max, false);
  if (!strcmp(drv, "sht3x")) return sht3x_read(d, out, max);
  if (!strcmp(drv, "sht4x")) return sht4x_read(d, out, max);
  if (!strcmp(drv, "aht20")) return aht20_read(d, out, max);
  if (!strcmp(drv, "bh1750")) return bh1750_read(d, out, max);
  if (!strcmp(drv, "tmp102")) return tmp102_read(d, out, max);
  if (!strcmp(drv, "mcp9808")) return mcp9808_read(d, out, max);
  if (!strcmp(drv, "ina219")) return ina219_read(d, out, max);
  if (!strcmp(drv, "mpu6050")) return mpu_read(d, out, max, false);
  if (!strcmp(drv, "mpu6500")) return mpu_read(d, out, max, true);
  if (!strcmp(drv, "scd4x")) return scd4x_read(d, out, max);
  return -4;
}

// ------------------------------------------------------------------ identification

static void ident(I2cIdent &o, const char *drv, const char *product, const char *vendor, bool confirmed) {
  strlcpy(o.driver, drv, sizeof(o.driver));
  strlcpy(o.product, product, sizeof(o.product));
  strlcpy(o.vendor, vendor, sizeof(o.vendor));
  o.confirmed = confirmed;
}

void i2c_identify(const I2cDev &d, I2cIdent &o) {
  memset(&o, 0, sizeof(o));
  uint8_t a = d.addr, b[9];
  if (a == 0x76 || a == 0x77) {
    if (xi2c_reg_read(d, 0xD0, b, 1)) {
      if (b[0] == 0x60) return ident(o, "bme280", "BME280", "Bosch", true);
      if (b[0] == 0x58 || b[0] == 0x56 || b[0] == 0x57) return ident(o, "bmp280", "BMP280", "Bosch", true);
      if (b[0] == 0x61) return ident(o, "", "BME680", "Bosch", true);
      if (b[0] == 0x55) return ident(o, "", "BMP180", "Bosch", true);
    }
    return ident(o, "", "BME/BMP280 or MS5611 (unconfirmed)", "", false);
  }
  if (a == 0x44 || a == 0x45) {
    uint8_t c = 0x89;  // SHT4x: read serial number
    if (xi2c_write(d, &c, 1)) {
      delay(2);
      if (xi2c_read(d, b, 6) && crc8_sensirion(b, 2) == b[2] && crc8_sensirion(b + 3, 2) == b[5])
        return ident(o, "sht4x", "SHT4x", "Sensirion", true);
    }
    if (cmd16(d, 0x3780)) {  // SHT3x: read serial number
      delay(2);
      if (xi2c_read(d, b, 6) && crc8_sensirion(b, 2) == b[2] && crc8_sensirion(b + 3, 2) == b[5])
        return ident(o, "sht3x", "SHT3x", "Sensirion", true);
    }
    // Not an SHT: 0x44/0x45 are also INA219 addresses (checked below).
    if (!(xi2c_reg_read(d, 0x00, b, 2) && be16(b) == 0x399F))
      return ident(o, "sht3x", "SHT3x / SHT4x (unconfirmed)", "Sensirion", false);
  }
  if (a == 0x62) {
    cmd16(d, 0x3F86);  // stop periodic measurement; serial number is only readable when idle
    delay(500);
    driver_reset(d);   // the driver restarts measurement on its next read
    if (cmd16(d, 0x3682)) {  // SCD4x: get serial number
      delay(2);
      if (xi2c_read(d, b, 9) && crc8_sensirion(b, 2) == b[2]) return ident(o, "scd4x", "SCD4x", "Sensirion", true);
    }
    return ident(o, "", "SCD4x (unconfirmed)", "Sensirion", false);
  }
  if (a == 0x38) {
    if (xi2c_read(d, b, 1) && (b[0] & 0x18) == 0x18) return ident(o, "aht20", "AHT20 / AHT21", "Aosong", true);
    return ident(o, "aht20", "AHT20 (unconfirmed)", "Aosong", false);
  }
  if (a >= 0x18 && a <= 0x1F && xi2c_reg_read(d, 0x06, b, 2) && be16(b) == 0x0054)
    return ident(o, "mcp9808", "MCP9808", "Microchip", true);
  if (a >= 0x40 && a <= 0x4F && xi2c_reg_read(d, 0x00, b, 2) && be16(b) == 0x399F)
    return ident(o, "ina219", "INA219", "Texas Instruments", true);
  if (a >= 0x48 && a <= 0x4B && xi2c_reg_read(d, 0x01, b, 2)) {
    uint16_t cfg = be16(b);
    if ((cfg & 0xFF00) == 0x6000 || cfg == 0x60A0) return ident(o, "tmp102", "TMP102", "Texas Instruments", true);
    if (cfg == 0x8583 || cfg == 0x0583) return ident(o, "", "ADS1015 / ADS1115 ADC", "Texas Instruments", true);
    return ident(o, "tmp102", "TMP102 / ADS1x15 (unconfirmed)", "", false);
  }
  if (a == 0x68 || a == 0x69) {
    if (xi2c_reg_read(d, 0x75, b, 1)) {
      if (b[0] == 0x68) return ident(o, "mpu6050", "MPU-6050", "InvenSense", true);
      if (b[0] == 0x70) return ident(o, "mpu6500", "MPU-6500", "InvenSense", true);
      if (b[0] == 0x71 || b[0] == 0x73) return ident(o, "mpu6500", "MPU-9250", "InvenSense", true);
    }
    if (a == 0x68) return ident(o, "", "DS3231 / DS1307 RTC (unconfirmed)", "", false);
  }
  if (a == 0x23 || a == 0x5C) return ident(o, "bh1750", "BH1750 (unconfirmed)", "Rohm", false);
  struct { uint8_t lo, hi; const char *what; } guesses[] = {
      {0x3C, 0x3D, "SSD1306 / SH1106 OLED"}, {0x50, 0x57, "24Cxx EEPROM"}, {0x70, 0x70, "TCA9548A I2C multiplexer"},
      {0x20, 0x27, "PCF8574 / MCP23017 I/O expander"}, {0x29, 0x29, "VL53L0X / TSL2591"}, {0x39, 0x39, "TSL2561 / APDS-9960"},
      {0x5A, 0x5A, "MLX90614 / CCS811"}, {0x10, 0x10, "VEML7700"}, {0x36, 0x36, "MAX17048 fuel gauge"},
      {0x0D, 0x0D, "QMC5883L magnetometer"}, {0x1E, 0x1E, "HMC5883L magnetometer"}, {0x57, 0x57, "MAX30102"},
  };
  for (auto &g : guesses)
    if (a >= g.lo && a <= g.hi) return ident(o, "", g.what, "", false);
}
