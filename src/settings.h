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

struct Settings {
  Rs485Settings rs485;
  CanSettings can;
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

bool can_bitrate_supported(uint32_t bps);
extern const uint32_t CAN_BITRATES[];
extern const size_t CAN_BITRATE_COUNT;
