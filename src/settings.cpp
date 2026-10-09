#include "settings.h"

#include <Preferences.h>
#include <esp_mac.h>

Settings g_settings;

const uint32_t CAN_BITRATES[] = {10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
const size_t CAN_BITRATE_COUNT = sizeof(CAN_BITRATES) / sizeof(CAN_BITRATES[0]);

bool can_bitrate_supported(uint32_t bps) {
  for (size_t i = 0; i < CAN_BITRATE_COUNT; i++)
    if (CAN_BITRATES[i] == bps) return true;
  return false;
}

static void copy_str(char *dst, size_t cap, const char *src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

static void default_ap_ssid(char *out, size_t cap) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, cap, "WonderScope-%02X%02X", mac[4], mac[5]);
}

void rs485_settings_to_json(const Rs485Settings &s, JsonObject o) {
  o["enabled"] = s.enabled;
  o["baud"] = s.baud;
  o["parity"] = s.parity == 1 ? "E" : s.parity == 2 ? "O" : "N";
  o["stop"] = s.stop;
  o["timeoutMs"] = s.timeoutMs;
  o["scanTimeoutMs"] = s.scanTimeoutMs;
}

void can_settings_to_json(const CanSettings &s, JsonObject o) {
  o["enabled"] = s.enabled;
  o["bitrate"] = s.bitrate;
  o["mode"] = s.mode == CAN_MODE_LISTEN ? "listen" : "normal";
  o["autoRecover"] = s.autoRecover;
  o["canopenPassive"] = s.canopenPassive;
  o["j1939Passive"] = s.j1939Passive;
  o["j1939Sa"] = s.j1939Sa;
  o["sdoTimeoutMs"] = s.sdoTimeoutMs;
  o["scanTimeoutMs"] = s.scanTimeoutMs;
}

void settings_to_json(JsonObject o) {
  const Settings &s = g_settings;
  rs485_settings_to_json(s.rs485, o["rs485"].to<JsonObject>());
  can_settings_to_json(s.can, o["can"].to<JsonObject>());
  JsonObject w = o["wifi"].to<JsonObject>();
  w["apSsid"] = s.wifi.apSsid;
  w["apHasPass"] = s.wifi.apPass[0] != 0;
  w["staSsid"] = s.wifi.staSsid;
  w["staHasPass"] = s.wifi.staPass[0] != 0;
  w["hostname"] = s.wifi.hostname;
  JsonObject a = o["auth"].to<JsonObject>();
  a["user"] = s.auth.user;
  a["enabled"] = s.auth.pass[0] != 0;
}

static int parse_parity(JsonVariantConst v) {
  if (v.is<int>()) {
    int p = v.as<int>();
    return (p >= 0 && p <= 2) ? p : -1;
  }
  const char *s = v.as<const char *>();
  if (!s || !*s) return -1;
  switch (toupper(s[0])) {
    case 'N': return 0;
    case 'E': return 1;
    case 'O': return 2;
  }
  return -1;
}

const char *rs485_settings_from_json(Rs485Settings &s, JsonObjectConst o) {
  Rs485Settings n = s;
  if (o["enabled"].is<bool>()) n.enabled = o["enabled"];
  if (!o["baud"].isNull()) {
    uint32_t b = o["baud"].as<uint32_t>();
    if (b < 300 || b > 1000000) return "baud must be 300..1000000";
    n.baud = b;
  }
  if (!o["parity"].isNull()) {
    int p = parse_parity(o["parity"]);
    if (p < 0) return "parity must be N, E or O";
    n.parity = p;
  }
  if (!o["stop"].isNull()) {
    int st = o["stop"].as<int>();
    if (st != 1 && st != 2) return "stop must be 1 or 2";
    n.stop = st;
  }
  if (!o["timeoutMs"].isNull()) {
    int t = o["timeoutMs"].as<int>();
    if (t < 10 || t > 5000) return "timeoutMs must be 10..5000";
    n.timeoutMs = t;
  }
  if (!o["scanTimeoutMs"].isNull()) {
    int t = o["scanTimeoutMs"].as<int>();
    if (t < 10 || t > 2000) return "scanTimeoutMs must be 10..2000";
    n.scanTimeoutMs = t;
  }
  s = n;
  return nullptr;
}

const char *can_settings_from_json(CanSettings &s, JsonObjectConst o) {
  CanSettings n = s;
  if (o["enabled"].is<bool>()) n.enabled = o["enabled"];
  if (!o["bitrate"].isNull()) {
    uint32_t b = o["bitrate"].as<uint32_t>();
    if (b > 0 && b <= 1000) b *= 1000;  // accept kbit/s
    if (!can_bitrate_supported(b)) return "unsupported bitrate (10k,20k,50k,100k,125k,250k,500k,800k,1M)";
    n.bitrate = b;
  }
  if (!o["mode"].isNull()) {
    const char *m = o["mode"];
    if (m && !strcmp(m, "normal")) n.mode = CAN_MODE_NORMAL;
    else if (m && (!strcmp(m, "listen") || !strcmp(m, "listen-only"))) n.mode = CAN_MODE_LISTEN;
    else return "mode must be normal or listen";
  }
  if (o["autoRecover"].is<bool>()) n.autoRecover = o["autoRecover"];
  if (o["canopenPassive"].is<bool>()) n.canopenPassive = o["canopenPassive"];
  if (o["j1939Passive"].is<bool>()) n.j1939Passive = o["j1939Passive"];
  if (!o["j1939Sa"].isNull()) {
    int sa = o["j1939Sa"].as<int>();
    if (sa < 0 || sa > 253) return "j1939Sa must be 0..253";
    n.j1939Sa = sa;
  }
  if (!o["sdoTimeoutMs"].isNull()) {
    int t = o["sdoTimeoutMs"].as<int>();
    if (t < 10 || t > 5000) return "sdoTimeoutMs must be 10..5000";
    n.sdoTimeoutMs = t;
  }
  if (!o["scanTimeoutMs"].isNull()) {
    int t = o["scanTimeoutMs"].as<int>();
    if (t < 5 || t > 2000) return "scanTimeoutMs must be 5..2000";
    n.scanTimeoutMs = t;
  }
  s = n;
  return nullptr;
}

const char *wifi_settings_from_json(WifiSettings &s, JsonObjectConst o) {
  WifiSettings n = s;
  if (o["apSsid"].is<const char *>()) {
    const char *v = o["apSsid"];
    if (strlen(v) < 1 || strlen(v) > 32) return "AP SSID must be 1..32 chars";
    copy_str(n.apSsid, sizeof(n.apSsid), v);
  }
  if (o["apPass"].is<const char *>()) {
    const char *v = o["apPass"];
    if (*v && (strlen(v) < 8 || strlen(v) > 63)) return "AP password must be 8..63 chars (or empty for open)";
    copy_str(n.apPass, sizeof(n.apPass), v);
  }
  if (o["staSsid"].is<const char *>()) copy_str(n.staSsid, sizeof(n.staSsid), o["staSsid"]);
  if (o["staPass"].is<const char *>()) copy_str(n.staPass, sizeof(n.staPass), o["staPass"]);
  if (o["hostname"].is<const char *>()) {
    const char *v = o["hostname"];
    size_t l = strlen(v);
    if (l < 1 || l > 32) return "hostname must be 1..32 chars";
    for (size_t i = 0; i < l; i++)
      if (!isalnum((unsigned char)v[i]) && v[i] != '-') return "hostname may contain only letters, digits and '-'";
    copy_str(n.hostname, sizeof(n.hostname), v);
  }
  s = n;
  return nullptr;
}

const char *auth_settings_from_json(AuthSettings &s, JsonObjectConst o) {
  AuthSettings n = s;
  if (o["user"].is<const char *>()) {
    const char *v = o["user"];
    if (strlen(v) < 1 || strlen(v) > 16) return "user must be 1..16 chars";
    copy_str(n.user, sizeof(n.user), v);
  }
  if (o["pass"].is<const char *>()) {
    const char *v = o["pass"];
    if (strlen(v) > 32) return "password must be at most 32 chars";
    copy_str(n.pass, sizeof(n.pass), v);
  }
  s = n;
  return nullptr;
}

void settings_load() {
  Settings s;
  default_ap_ssid(s.wifi.apSsid, sizeof(s.wifi.apSsid));
  Preferences p;
  if (p.begin("wonderscope", true)) {
    String js = p.getString("cfg", "");
    p.end();
    if (js.length()) {
      JsonDocument d;
      if (!deserializeJson(d, js)) {
        rs485_settings_from_json(s.rs485, d["rs485"]);
        can_settings_from_json(s.can, d["can"]);
        wifi_settings_from_json(s.wifi, d["wifi"]);
        auth_settings_from_json(s.auth, d["auth"]);
      }
    }
  }
  g_settings = s;
}

void settings_save() {
  const Settings &s = g_settings;
  JsonDocument d;
  rs485_settings_to_json(s.rs485, d["rs485"].to<JsonObject>());
  can_settings_to_json(s.can, d["can"].to<JsonObject>());
  JsonObject w = d["wifi"].to<JsonObject>();
  w["apSsid"] = s.wifi.apSsid;
  w["apPass"] = s.wifi.apPass;
  w["staSsid"] = s.wifi.staSsid;
  w["staPass"] = s.wifi.staPass;
  w["hostname"] = s.wifi.hostname;
  JsonObject a = d["auth"].to<JsonObject>();
  a["user"] = s.auth.user;
  a["pass"] = s.auth.pass;
  String js;
  serializeJson(d, js);
  Preferences p;
  if (p.begin("wonderscope", false)) {
    p.putString("cfg", js);
    p.end();
  }
}

void settings_reset() {
  Preferences p;
  if (p.begin("wonderscope", false)) {
    p.clear();
    p.end();
  }
  settings_load();
}
