#pragma once
#include "common.h"

enum CanMode : uint8_t { CAN_MODE_NORMAL = 0, CAN_MODE_LISTEN = 1 };

struct Rs485Settings {
  bool enabled = true;
  uint32_t baud = 9600;
  uint8_t parity = 0;  // 0 none, 1 even, 2 odd
  uint8_t stop = 1;    // 1 or 2
  uint16_t timeoutMs = 200;     // Modbus response timeout
  uint16_t scanTimeoutMs = 80;  // per-address timeout during scans
};

struct CanSettings {
  bool enabled = true;
  uint32_t bitrate = 250000;
  uint8_t mode = CAN_MODE_LISTEN;  // listen-only is the safe default
  bool autoRecover = true;
  bool canopenPassive = true;  // create CANopen devices from heartbeat/boot-up
  bool j1939Passive = true;    // create J1939 devices from 29-bit traffic
  uint8_t j1939Sa = 0xF9;      // our J1939 source address (off-board service tool)
  uint16_t sdoTimeoutMs = 300;
  uint16_t scanTimeoutMs = 40;
};

struct WifiSettings {
  char apSsid[33] = "";
  char apPass[65] = "wonderscope";
  char staSsid[33] = "";
  char staPass[65] = "";
  char hostname[33] = "wonderscope";
};

struct AuthSettings {
  char user[17] = "admin";
  char pass[33] = "";  // empty = no login required
};

struct I2cBusSettings {
  bool enabled = false;
  uint32_t hz = 100000;
  bool autoScan = true;  // periodic probe for added / removed devices
};

constexpr int SPI_MAX_CS = 4;
struct SpiSettings {
  bool enabled = false;
  uint32_t hz = 1000000;
  uint8_t mode = 0;
  uint8_t cs[SPI_MAX_CS] = {10};
  uint8_t nCs = 1;
  bool readBit = true;  // register reads set bit 7 of the address byte
};

struct Settings {
  Rs485Settings rs485;
  CanSettings can;
  I2cBusSettings qwiic{true, 100000, true};
  I2cBusSettings i2c;  // pin header IO8/IO9
  SpiSettings spi;
  WifiSettings wifi;
  AuthSettings auth;
};

extern Settings g_settings;

void settings_load();
void settings_save();
void settings_reset();
// JSON views (passwords are never sent back; "hasPass" flags instead)
void settings_to_json(JsonObject o);
void rs485_settings_to_json(const Rs485Settings &s, JsonObject o);
void can_settings_to_json(const CanSettings &s, JsonObject o);
// Apply a partial update; returns error string or nullptr.
const char *rs485_settings_from_json(Rs485Settings &s, JsonObjectConst o);
const char *can_settings_from_json(CanSettings &s, JsonObjectConst o);
const char *wifi_settings_from_json(WifiSettings &s, JsonObjectConst o);
const char *auth_settings_from_json(AuthSettings &s, JsonObjectConst o);

void i2c_settings_to_json(const I2cBusSettings &s, JsonObject o);
void spi_settings_to_json(const SpiSettings &s, JsonObject o);
const char *i2c_settings_from_json(I2cBusSettings &s, JsonObjectConst o);
const char *spi_settings_from_json(SpiSettings &s, JsonObjectConst o);
const char *xbus_validate(const Settings &s);  // pin conflicts between expansion buses
bool spi_cs_allowed(uint8_t gpio);

bool can_bitrate_supported(uint32_t bps);
extern const uint32_t CAN_BITRATES[];
extern const size_t CAN_BITRATE_COUNT;
