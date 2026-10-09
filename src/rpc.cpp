#include "rpc.h"

#include <esp_heap_caps.h>
#include <sys/time.h>

#include "can.h"
#include "cli.h"
#include "devices.h"
#include "rs485.h"
#include "rtc.h"
#include "settings.h"
#include "trace.h"
#include "web.h"
#include "wifi_mgr.h"

static uint32_t rebootAtMs, wifiApplyAtMs;
static bool factoryPending;

void status_json(JsonObject o) {
  o["up"] = uptime_ms();
  o["epoch"] = epoch_ms();
  o["heap"] = ESP.getFreeHeap();
  o["minHeap"] = ESP.getMinFreeHeap();
  o["psram"] = ESP.getFreePsram();
  o["clients"] = web_client_count();
  o["dropped"] = out_dropped();
  wifi_status_json(o["wifi"].to<JsonObject>());
  rs485_status_json(o["rs485"].to<JsonObject>());
  can_status_json(o["can"].to<JsonObject>());
  o["devices"] = dev_count();
  o["traceHead"] = trace_head();
}

void state_changed() {
  JsonDocument e;
  e["ev"] = "state";
  status_json(e["s"].to<JsonObject>());
  settings_to_json(e["settings"].to<JsonObject>());
  event_send(e);
}

void hello_json(JsonObject o) {
  o["fw"] = FW_NAME;
  o["version"] = FW_VERSION;
  o["build"] = __DATE__ " " __TIME__;
  o["board"] = "Waveshare ESP32-S3-RS485-CAN";
  o["mac"] = wifi_mac();
  o["chip"] = ESP.getChipModel();
  o["flash"] = ESP.getFlashChipSize();
  o["psramSize"] = ESP.getPsramSize();
  JsonArray br = o["canBitrates"].to<JsonArray>();
  for (size_t i = 0; i < CAN_BITRATE_COUNT; i++) br.add(CAN_BITRATES[i]);
  settings_to_json(o["settings"].to<JsonObject>());
  status_json(o["status"].to<JsonObject>());
}

static Job *make_job(JsonDocument &req, const ReplyTo &rt) {
  Job *j = new Job();
  j->cmd = req["cmd"].as<const char *>();
  j->args = req;
  j->rt = rt;
  return j;
}

static bool key_arg(JsonDocument &req, const ReplyTo &rt, uint8_t &bus, uint8_t &proto, uint8_t &addr) {
  if (req["key"].is<const char *>()) {
    if (dev_parse_key(req["key"], bus, proto, addr)) return true;
    reply_err(rt, "bad device key '%s' (expected e.g. rs485:modbus:17)", req["key"].as<const char *>());
    return false;
  }
  int b = bus_from_name(req["bus"]), p = proto_from_name(req["proto"]);
  int a = req["addr"] | -1;
  if (b < 0 || p < 0 || a < 0 || a > 255) {
    reply_err(rt, "need key, or bus+proto+addr");
    return false;
  }
  bus = b;
  proto = p;
  addr = a;
  return true;
}

static void dev_removed_event(uint8_t bus, uint8_t proto, uint8_t addr) {
  JsonDocument e;
  e["ev"] = "devdel";
  e["keys"].add(dev_key(bus, proto, addr));
  event_send(e);
}

void rpc_dispatch(JsonDocument &req, const ReplyTo &rt) {
  const char *cmd = req["cmd"] | "";
  String c(cmd);

  // ---- system / info
  if (c == "hello") {
    JsonDocument r;
    hello_json(r.to<JsonObject>());
    return reply_ok(rt, r);
  }
  if (c == "status") {
    JsonDocument r;
    status_json(r.to<JsonObject>());
    return reply_ok(rt, r);
  }
  if (c == "time.set") {
    int64_t ms = req["epoch"] | (int64_t)0;
    if (ms < 1700000000000LL) return reply_err(rt, "epoch (ms since 1970) required");
    // NTP time takes precedence over browser time unless forced.
    if (!wifi_ntp_synced() || (req["force"] | false)) {
      struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
      settimeofday(&tv, nullptr);
      rtc_write_now();
    }
    JsonDocument r;
    r["epoch"] = epoch_ms();
    r["source"] = wifi_ntp_synced() ? "ntp" : "client";
    return reply_ok(rt, r);
  }
  if (c == "settings.get") {
    JsonDocument r;
    settings_to_json(r.to<JsonObject>());
    return reply_ok(rt, r);
  }
  if (c == "rs485.config") {
    const char *err = rs485_settings_from_json(g_settings.rs485, req.as<JsonObjectConst>());
    if (err) return reply_err(rt, "%s", err);
    settings_save();
    Job *j = make_job(req, rt);
    j->cmd = "rs485.apply";
    rs485_submit(j);
    return;
  }
  if (c == "can.config") {
    const char *err = can_settings_from_json(g_settings.can, req.as<JsonObjectConst>());
    if (err) return reply_err(rt, "%s", err);
    settings_save();
    Job *j = make_job(req, rt);
    j->cmd = "can.apply";
    can_submit(j);
    return;
  }
  if (c == "wifi.config") {
    const char *err = wifi_settings_from_json(g_settings.wifi, req.as<JsonObjectConst>());
    if (err) return reply_err(rt, "%s", err);
    settings_save();
    wifiApplyAtMs = uptime_ms() + 1500;  // after the reply is sent
    state_changed();
    JsonDocument r;
    r["note"] = "Wi-Fi settings saved; restarting Wi-Fi";
    return reply_ok(rt, r);
  }
  if (c == "auth.config") {
    const char *err = auth_settings_from_json(g_settings.auth, req.as<JsonObjectConst>());
    if (err) return reply_err(rt, "%s", err);
    settings_save();
    state_changed();
    JsonDocument r;
    r["enabled"] = g_settings.auth.pass[0] != 0;
    r["user"] = g_settings.auth.user;
    return reply_ok(rt, r);
  }
  if (c == "sys.reboot") {
    rebootAtMs = uptime_ms() + 800;
    JsonDocument r;
    r["note"] = "rebooting";
    return reply_ok(rt, r);
  }
  if (c == "sys.factory") {
    if (!(req["confirm"] | false)) return reply_err(rt, "factory reset needs confirm:true");
    factoryPending = true;
    rebootAtMs = uptime_ms() + 800;
    JsonDocument r;
    r["note"] = "erasing settings and device list, then rebooting";
    return reply_ok(rt, r);
  }
  if (c == "sub") {
    uint8_t flags = sub_get(rt.client);
    JsonObjectConst t = req["topics"];
    auto set = [&](const char *k, uint8_t bit) {
      if (t[k].is<bool>()) flags = t[k].as<bool>() ? (flags | bit) : (flags & ~bit);
    };
    set("trace", SUB_TRACE);
    set("ids", SUB_IDS);
    set("textTrace", SUB_TTRACE);
    set("events", SUB_EVENTS);
    sub_set(rt.client, flags);
    if (!t["textBus"].isNull()) sub_set_tbus(rt.client, t["textBus"] | 0);
    JsonDocument r;
    r["flags"] = flags;
    r["traceHead"] = trace_head();
    return reply_ok(rt, r);
  }
  if (c == "cli") {
    cli_exec(req["line"] | "", rt.client, rt.id);
    return;
  }

  // ---- bus jobs
  if (c == "scan") {
    const char *bus = req["bus"] | "";
    if (!strcmp(bus, "rs485")) return (void)rs485_submit(make_job(req, rt));
    if (!strcmp(bus, "can")) return (void)can_submit(make_job(req, rt));
    return reply_err(rt, "scan: bus must be rs485 or can");
  }
  if (c == "scan.cancel") {
    const char *bus = req["bus"] | "";
    if (!*bus || !strcmp(bus, "rs485")) rs485_cancel();
    if (!*bus || !strcmp(bus, "can")) can_cancel();
    return reply_ok(rt);
  }
  if (c.startsWith("mb.")) return (void)rs485_submit(make_job(req, rt));
  if (c.startsWith("co.") || c.startsWith("j1939.") || c == "can.send" || c == "can.autobaud" ||
      c == "can.recover" || c == "can.selftest")
    return (void)can_submit(make_job(req, rt));

  // ---- registry
  if (c == "dev.list") {
    JsonDocument r;
    int bus = req["bus"].is<const char *>() ? bus_from_name(req["bus"]) : -1;
    dev_list_json(r["devices"].to<JsonArray>(), bus);
    r["now"] = uptime_ms();
    return reply_ok(rt, r);
  }
  uint8_t bus, proto, addr;
  if (c == "dev.get") {
    if (!key_arg(req, rt, bus, proto, addr)) return;
    JsonDocument r;
    if (!dev_get_json(bus, proto, addr, r.to<JsonObject>())) return reply_err(rt, "no such device");
    r["now"] = uptime_ms();
    return reply_ok(rt, r);
  }
  if (c == "dev.update" || c == "dev.add") {
    if (!key_arg(req, rt, bus, proto, addr)) return;
    if (bus == BUS_RS485 && (proto != PROTO_MODBUS || addr < 1 || addr > 247))
      return reply_err(rt, "RS485 devices are modbus:1..247");
    if (bus == BUS_CAN && proto == PROTO_MODBUS) return reply_err(rt, "CAN devices are canopen or j1939");
    if (proto == PROTO_CANOPEN && (addr < 1 || addr > 127)) return reply_err(rt, "CANopen node must be 1..127");
    if (proto == PROTO_J1939 && addr > 253) return reply_err(rt, "J1939 address must be 0..253");
    const char *err = dev_apply_meta(bus, proto, addr, req.as<JsonObjectConst>());
    if (err) return reply_err(rt, "%s", err);
    JsonDocument r;
    dev_get_json(bus, proto, addr, r.to<JsonObject>());
    return reply_ok(rt, r);
  }
  if (c == "dev.remove") {
    if (!key_arg(req, rt, bus, proto, addr)) return;
    if (!dev_remove(bus, proto, addr)) return reply_err(rt, "no such device");
    dev_removed_event(bus, proto, addr);
    return reply_ok(rt);
  }
  if (c == "dev.clear") {
    int b = req["bus"].is<const char *>() ? bus_from_name(req["bus"]) : -1;
    int n = dev_clear(b);
    JsonDocument e;
    e["ev"] = "devclear";
    if (b >= 0) e["bus"] = bus_name(b);
    event_send(e);
    JsonDocument r;
    r["removed"] = n;
    return reply_ok(rt, r);
  }
  if (c == "can.ids") {
    JsonDocument r;
    can_ids_json(r["ids"].to<JsonArray>(), req["max"] | 768);
    return reply_ok(rt, r);
  }
  if (c == "can.ids.clear") {
    can_ids_clear();
    return reply_ok(rt);
  }
  if (c == "trace.clear") {
    trace_clear();
    JsonDocument r;
    r["traceHead"] = trace_head();
    return reply_ok(rt, r);
  }
  reply_err(rt, "unknown command '%s'", cmd);
}

void rpc_loop() {
  uint32_t now = uptime_ms();
  if (wifiApplyAtMs && (int32_t)(now - wifiApplyAtMs) >= 0) {
    wifiApplyAtMs = 0;
    wifi_apply();
  }
  if (rebootAtMs && (int32_t)(now - rebootAtMs) >= 0) {
    if (factoryPending) {
      settings_reset();
      dev_clear(-1);
      dev_save_now();
    } else {
      dev_save_now();
    }
    delay(100);
    ESP.restart();
  }
}
