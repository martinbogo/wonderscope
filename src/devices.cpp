#include "devices.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static Device *devs;
static uint32_t *pushedVer;
static SemaphoreHandle_t mtx;
static uint32_t persistDueMs;
static bool persistPending;
static bool fsOk;

static const char *DEV_FILE = "/devices.json";

static void lock() { xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
static void unlock() { xSemaphoreGiveRecursive(mtx); }

static void copy_str(char *dst, size_t cap, const char *src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

static int find_idx(uint8_t bus, uint8_t proto, uint8_t addr) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    const Device &d = devs[i];
    if (d.used && d.bus == bus && d.proto == proto && d.addr == addr) return i;
  }
  return -1;
}

static int create_idx(uint8_t bus, uint8_t proto, uint8_t addr) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!devs[i].used) {
      PgnEntry *keep = devs[i].pgns;
      memset(&devs[i], 0, sizeof(Device));
      Device &d = devs[i];
      d.pgns = keep;
      d.used = true;
      d.bus = bus;
      d.proto = proto;
      d.addr = addr;
      d.state = -1;
      d.firstSeenMs = uptime_ms();
      d.ver = 1;
      return i;
    }
  }
  return -1;
}

static void schedule_persist() {
  persistPending = true;
  persistDueMs = uptime_ms() + 5000;
}

void dev_init() {
  devs = (Device *)heap_caps_calloc(MAX_DEVICES, sizeof(Device), MALLOC_CAP_SPIRAM);
  pushedVer = (uint32_t *)heap_caps_calloc(MAX_DEVICES, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  mtx = xSemaphoreCreateRecursiveMutex();
  // Partition label must match partitions.csv ("littlefs"); the library default is "spiffs".
  fsOk = LittleFS.begin(true, "/littlefs", 10, "littlefs");
  if (!fsOk) {
    Serial.println("[dev] LittleFS mount failed; device list will not persist");
    return;
  }
  File f = LittleFS.open(DEV_FILE, "r");
  if (!f) f = LittleFS.open("/devices.tmp", "r");  // interrupted save
  if (!f) return;
  size_t n = f.size();
  char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (buf) {
    f.readBytes(buf, n);
    buf[n] = 0;
    const char *err = dev_import_json(buf, n);
    if (err) Serial.printf("[dev] load failed: %s\n", err);
    free(buf);
  }
  f.close();
  persistPending = false;
}

Device *dev_acquire(uint8_t bus, uint8_t proto, uint8_t addr, bool create) {
  if (!devs) return nullptr;
  lock();
  int i = find_idx(bus, proto, addr);
  if (i < 0 && create) {
    i = create_idx(bus, proto, addr);
    if (i >= 0) schedule_persist();
  }
  if (i < 0) {
    unlock();
    return nullptr;
  }
  return &devs[i];
}

void dev_release(Device *d, bool changed, bool persist) {
  if (!d) return;
  if (changed) d->ver++;
  if (persist) schedule_persist();
  unlock();
}

void dev_seen(uint8_t bus, uint8_t proto, uint8_t addr, bool passive) {
  Device *d = dev_acquire(bus, proto, addr, true);
  if (!d) return;
  bool wasPresent = d->present;
  d->lastSeenMs = uptime_ms();
  d->present = true;
  if (!passive) d->passive = false;
  else if (!wasPresent && d->rx == 0) d->passive = true;
  d->rx++;
  d->consecErr = 0;
  // Pushing every frame would flood the UI; bump version at most ~2x/s unless state changed.
  static uint32_t lastBump[MAX_DEVICES];
  int idx = d - devs;
  bool bump = !wasPresent || (d->lastSeenMs - lastBump[idx] > 500);
  if (bump) lastBump[idx] = d->lastSeenMs;
  dev_release(d, bump);
}

void dev_failed(uint8_t bus, uint8_t proto, uint8_t addr) {
  Device *d = dev_acquire(bus, proto, addr, false);
  if (!d) return;
  d->errs++;
  d->consecErr++;
  dev_release(d, true);
}

String dev_key(uint8_t bus, uint8_t proto, uint8_t addr) {
  return String(bus_name(bus)) + ":" + proto_name(proto) + ":" + addr;
}

bool dev_parse_key(const char *key, uint8_t &bus, uint8_t &proto, uint8_t &addr) {
  if (!key) return false;
  char tmp[48];
  copy_str(tmp, sizeof(tmp), key);
  char *a = strchr(tmp, ':');
  if (!a) return false;
  *a++ = 0;
  char *b = strchr(a, ':');
  if (!b) return false;
  *b++ = 0;
  int bu = bus_from_name(tmp), pr = proto_from_name(a);
  char *end;
  long ad = strtol(b, &end, 0);
  if (bu < 0 || pr < 0 || *end || ad < 0 || ad > 255) return false;
  bus = bu;
  proto = pr;
  addr = (uint8_t)ad;
  return true;
}

static void watch_to_json(const WatchItem &w, JsonObject o, bool withValue) {
  o["name"] = w.name;
  o["fn"] = w.fn;
  o["addr"] = w.addr;
  o["sub"] = w.sub;
  o["count"] = w.count;
  o["fmt"] = w.fmt;
  o["scale"] = w.scale;
  o["unit"] = w.unit;
  if (withValue && w.tsMs) {
    o["raw"] = hex_string(w.raw, w.rawLen);
    o["err"] = w.err;
    if (w.abortCode) o["abort"] = w.abortCode;
    o["ts"] = w.tsMs;
  }
}

void dev_to_json(const Device &d, JsonObject o, bool full) {
  o["key"] = dev_key(d.bus, d.proto, d.addr);
  o["bus"] = bus_name(d.bus);
  o["proto"] = proto_name(d.proto);
  o["addr"] = d.addr;
  o["label"] = d.label;
  o["present"] = d.present;
  o["passive"] = d.passive;
  o["firstSeen"] = d.firstSeenMs;
  o["lastSeen"] = d.lastSeenMs;
  o["rx"] = d.rx;
  o["errs"] = d.errs;
  o["consecErr"] = d.consecErr;
  if (d.state >= 0) o["state"] = d.state;
  if (d.proto == PROTO_MODBUS && d.baud) {
    o["baud"] = d.baud;
    o["parity"] = d.parity == 1 ? "E" : d.parity == 2 ? "O" : "N";
    o["stop"] = d.stop ? d.stop : 1;
  }
  if (d.vendor[0]) o["vendor"] = d.vendor;
  if (d.product[0]) o["product"] = d.product;
  if (d.revision[0]) o["revision"] = d.revision;
  if (d.name[0]) o["name"] = d.name;
  if (d.serial[0]) o["serial"] = d.serial;
  if (d.proto == PROTO_CANOPEN && d.coIdentity) {
    JsonObject c = o["co"].to<JsonObject>();
    c["deviceType"] = d.coDeviceType;
    c["vendorId"] = d.coVendorId;
    c["productCode"] = d.coProductCode;
    c["revision"] = d.coRevision;
    c["serial"] = d.coSerial;
  }
  if (d.emcyCount) {
    JsonObject e = o["emcy"].to<JsonObject>();
    e["code"] = d.emcyCode;
    e["reg"] = d.emcyReg;
    e["ts"] = d.emcyMs;
    e["count"] = d.emcyCount;
  }
  if (d.hasName) {
    char nm[20];
    snprintf(nm, sizeof(nm), "%016llX", (unsigned long long)d.j1939Name);
    o["j1939Name"] = nm;
  }
  o["pollMs"] = d.pollMs;
  o["nWatch"] = d.nWatch;
  if (d.driver[0]) o["driver"] = d.driver;
  if (d.nVals) {
    JsonArray va = o["values"].to<JsonArray>();
    for (int i = 0; i < d.nVals; i++) {
      JsonObject v = va.add<JsonObject>();
      v["n"] = d.vals[i].name;
      v["u"] = d.vals[i].unit;
      v["v"] = d.vals[i].v;
    }
    o["vts"] = d.valsTs;
  }
  if (d.valsErr) o["valsErr"] = d.valsErr;
  if (!full) return;
  o["notes"] = d.notes;
  JsonArray w = o["watch"].to<JsonArray>();
  for (int i = 0; i < d.nWatch; i++) watch_to_json(d.watch[i], w.add<JsonObject>(), true);
  if (d.pgns && d.nPgns) {
    JsonArray p = o["pgns"].to<JsonArray>();
    for (int i = 0; i < d.nPgns; i++) {
      const PgnEntry &e = d.pgns[i];
      JsonObject po = p.add<JsonObject>();
      po["pgn"] = e.pgn;
      po["count"] = e.count;
      po["ts"] = e.lastMs;
      po["data"] = hex_string(e.data, e.len);
    }
  }
}

void dev_list_json(JsonArray arr, int busFilter) {
  lock();
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!devs[i].used || (busFilter >= 0 && devs[i].bus != busFilter)) continue;
    // Leaves pushedVer alone: a listing for one client must not suppress the
    // change broadcast to the others.
    dev_to_json(devs[i], arr.add<JsonObject>(), false);
  }
  unlock();
}

bool dev_get_json(uint8_t bus, uint8_t proto, uint8_t addr, JsonObject o) {
  lock();
  int i = find_idx(bus, proto, addr);
  if (i >= 0) dev_to_json(devs[i], o, true);
  unlock();
  return i >= 0;
}

size_t dev_collect_changed(JsonArray arr, size_t max) {
  // Round-robin start so busy low-index devices cannot starve the rest.
  static int start;
  size_t n = 0;
  lock();
  int i = start;
  for (int k = 0; k < MAX_DEVICES && n < max; k++, i = (i + 1) % MAX_DEVICES) {
    if (!devs[i].used || devs[i].ver == pushedVer[i]) continue;
    // Changed devices are sent in full so open detail panels stay live.
    dev_to_json(devs[i], arr.add<JsonObject>(), true);
    pushedVer[i] = devs[i].ver;
    n++;
  }
  start = i;
  unlock();
  return n;
}

static const char *watch_from_json(WatchItem &w, JsonObjectConst o, uint8_t proto) {
  memset(&w, 0, sizeof(w));
  // Parse as int and range-check before narrowing into the struct.
  long fn = o["fn"] | (proto == PROTO_MODBUS ? 3L : 0L), addr = o["addr"] | 0L, sub = o["sub"] | 0L,
       count = o["count"] | 1L;
  copy_str(w.name, sizeof(w.name), o["name"] | "");
  copy_str(w.fmt, sizeof(w.fmt), o["fmt"] | "u16");
  w.scale = o["scale"] | 1.0f;
  copy_str(w.unit, sizeof(w.unit), o["unit"] | "");
  if (addr < 0 || addr > 0xFFFF) return "watch address must be 0..65535";
  if (proto == PROTO_MODBUS) {
    if (fn < 1 || fn > 4) return "watch fn must be 1..4";
    long maxCount = (fn <= 2) ? 64 : 8;
    if (count < 1 || count > maxCount) return "watch count out of range (regs 1..8, bits 1..64)";
  } else if (proto == PROTO_CANOPEN) {
    if (sub < 0 || sub > 255) return "sub-index must be 0..255";
    fn = 0;
    count = 1;
  } else if (proto == PROTO_I2C || proto == PROTO_SPI) {
    fn = 0;
    if (count < 1 || count > 16) return "watch count must be 1..16 bytes";
    if (proto == PROTO_I2C && sub != 2) sub = 1;
    if (proto == PROTO_SPI) sub = 0;
    if (addr > (sub == 2 ? 0xFFFF : 0xFF)) return "register address out of range";
  } else {
    return "watch lists are not supported for this protocol";
  }
  w.fn = fn;
  w.addr = addr;
  w.sub = sub;
  w.count = count;
  return nullptr;
}

const char *dev_apply_meta(uint8_t bus, uint8_t proto, uint8_t addr, JsonObjectConst m) {
  // Validate everything first: a rejected update must not create or modify a device.
  int pollMs = -1;
  if (!m["pollMs"].isNull()) {
    pollMs = m["pollMs"].as<int>();
    if (pollMs != 0 && (pollMs < 100 || pollMs > 60000)) return "pollMs must be 0 or 100..60000";
  }
  WatchItem tmp[MAX_WATCH];
  int nWatch = -1;
  if (m["watch"].is<JsonArrayConst>()) {
    JsonArrayConst arr = m["watch"];
    if (arr.size() > MAX_WATCH) return "too many watch items (max 16)";
    nWatch = 0;
    for (JsonObjectConst o : arr) {
      const char *err = watch_from_json(tmp[nWatch], o, proto);
      if (err) return err;
      nWatch++;
    }
  }
  uint32_t baud = 0;
  if (proto == PROTO_MODBUS && !m["baud"].isNull()) {
    baud = m["baud"].as<uint32_t>();
    if (baud < 300 || baud > 1000000) return "baud must be 300..1000000";
  }

  Device *d = dev_acquire(bus, proto, addr, true);
  if (!d) return "device table full";
  if (m["label"].is<const char *>()) copy_str(d->label, sizeof(d->label), m["label"]);
  if (m["notes"].is<const char *>()) copy_str(d->notes, sizeof(d->notes), m["notes"]);
  if (pollMs >= 0) {
    d->pollMs = pollMs;
    d->nextPollMs = 0;
  }
  if (nWatch >= 0) {
    memcpy(d->watch, tmp, sizeof(WatchItem) * nWatch);
    d->nWatch = nWatch;
  }
  if (m["driver"].is<const char *>() && d->proto == PROTO_I2C) {
    copy_str(d->driver, sizeof(d->driver), m["driver"]);
    d->nVals = 0;
    d->valsErr = 0;
    if (d->driver[0] && !d->pollMs) d->pollMs = 1000;
  }
  if (m["product"].is<const char *>()) copy_str(d->product, sizeof(d->product), m["product"]);
  if (d->proto == PROTO_MODBUS) {
    if (baud) d->baud = baud;
    if (m["parity"].is<const char *>()) {
      char c = toupper(m["parity"].as<const char *>()[0]);
      d->parity = c == 'E' ? 1 : c == 'O' ? 2 : 0;
    }
    if (!m["stop"].isNull()) d->stop = m["stop"].as<int>() == 2 ? 2 : 1;
  }
  dev_release(d, true, true);
  return nullptr;
}

bool dev_remove(uint8_t bus, uint8_t proto, uint8_t addr) {
  lock();
  int i = find_idx(bus, proto, addr);
  if (i >= 0) {
    devs[i].used = false;
    devs[i].nPgns = 0;
    schedule_persist();
  }
  unlock();
  return i >= 0;
}

int dev_clear(int busFilter) {
  int n = 0;
  lock();
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devs[i].used && (busFilter < 0 || devs[i].bus == busFilter)) {
      devs[i].used = false;
      devs[i].nPgns = 0;
      n++;
    }
  }
  schedule_persist();
  unlock();
  return n;
}

size_t dev_count(int busFilter) {
  size_t n = 0;
  lock();
  for (int i = 0; i < MAX_DEVICES; i++)
    if (devs[i].used && (busFilter < 0 || devs[i].bus == busFilter)) n++;
  unlock();
  return n;
}

bool dev_next_poll(uint8_t bus, uint32_t nowMs, PollTask &out) {
  bool found = false;
  lock();
  for (int i = 0; i < MAX_DEVICES; i++) {
    Device &d = devs[i];
    if (!d.used || d.bus != bus || !d.pollMs || (!d.nWatch && !d.driver[0])) continue;
    if ((int32_t)(nowMs - d.nextPollMs) < 0) continue;
    d.nextPollMs = nowMs + d.pollMs;
    out.bus = d.bus;
    out.proto = d.proto;
    out.addr = d.addr;
    out.baud = d.baud;
    out.parity = d.parity;
    out.stop = d.stop;
    out.nWatch = d.nWatch;
    memcpy(out.watch, d.watch, sizeof(WatchItem) * d.nWatch);
    memcpy(out.driver, d.driver, sizeof(out.driver));
    found = true;
    break;
  }
  unlock();
  return found;
}

void dev_set_values(uint8_t bus, uint8_t proto, uint8_t addr, const DevValue *vals, uint8_t n, int16_t err) {
  Device *d = dev_acquire(bus, proto, addr, false);
  if (!d) return;
  if (!err) {
    if (n > MAX_VALUES) n = MAX_VALUES;
    memcpy(d->vals, vals, sizeof(DevValue) * n);
    d->nVals = n;
    d->valsTs = uptime_ms();
  }
  d->valsErr = err;
  dev_release(d, true);
}

void dev_set_watch_value(uint8_t bus, uint8_t proto, uint8_t addr, uint8_t idx, const uint8_t *raw, uint8_t len,
                         int16_t err, uint32_t abortCode) {
  Device *d = dev_acquire(bus, proto, addr, false);
  if (!d) return;
  if (idx < d->nWatch) {
    WatchItem &w = d->watch[idx];
    if (len > sizeof(w.raw)) len = sizeof(w.raw);
    if (!err) {
      memcpy(w.raw, raw, len);
      w.rawLen = len;
    }
    w.err = err;
    w.abortCode = abortCode;
    w.tsMs = uptime_ms();
  }
  dev_release(d, true);
}

void dev_j1939_pgn(uint8_t sa, uint32_t pgn, const uint8_t *data, uint8_t len, bool passiveCreate) {
  Device *d = dev_acquire(BUS_CAN, PROTO_J1939, sa, passiveCreate);
  if (!d) return;
  bool wasPresent = d->present;
  d->lastSeenMs = uptime_ms();
  if (!d->present) {
    d->present = true;
    if (d->rx == 0) d->passive = true;
  }
  d->rx++;
  if (!d->pgns) d->pgns = (PgnEntry *)heap_caps_calloc(MAX_PGNS, sizeof(PgnEntry), MALLOC_CAP_SPIRAM);
  bool newPgn = false;
  if (d->pgns) {
    int slot = -1;
    for (int i = 0; i < d->nPgns; i++)
      if (d->pgns[i].pgn == pgn) {
        slot = i;
        break;
      }
    if (slot < 0) {
      if (d->nPgns < MAX_PGNS) slot = d->nPgns++;
      else {  // evict least recently seen
        slot = 0;
        for (int i = 1; i < d->nPgns; i++)
          if (d->pgns[i].lastMs < d->pgns[slot].lastMs) slot = i;
        d->pgns[slot].count = 0;
      }
      d->pgns[slot].pgn = pgn;
      newPgn = true;
    }
    PgnEntry &e = d->pgns[slot];
    e.count++;
    e.lastMs = d->lastSeenMs;
    e.len = len > 8 ? 8 : len;
    memcpy(e.data, data, e.len);
  }
  static uint32_t lastBump[256];
  bool bump = newPgn || !wasPresent || (d->lastSeenMs - lastBump[sa] > 1000);
  if (bump) lastBump[sa] = d->lastSeenMs;
  dev_release(d, bump);
}

// ---------------------------------------------------------------- persistence

static void dev_persist_json(JsonDocument &doc) {
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVICES; i++) {
    const Device &d = devs[i];
    if (!d.used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["bus"] = bus_name(d.bus);
    o["proto"] = proto_name(d.proto);
    o["addr"] = d.addr;
    if (d.label[0]) o["label"] = d.label;
    if (d.notes[0]) o["notes"] = d.notes;
    if (d.pollMs) o["pollMs"] = d.pollMs;
    if (d.baud) {
      o["baud"] = d.baud;
      o["parity"] = d.parity == 1 ? "E" : d.parity == 2 ? "O" : "N";
      o["stop"] = d.stop ? d.stop : 1;
    }
    if (d.vendor[0]) o["vendor"] = d.vendor;
    if (d.product[0]) o["product"] = d.product;
    if (d.revision[0]) o["revision"] = d.revision;
    if (d.name[0]) o["name"] = d.name;
    if (d.serial[0]) o["serial"] = d.serial;
    if (d.coIdentity) {
      JsonArray c = o["co"].to<JsonArray>();
      c.add(d.coDeviceType);
      c.add(d.coVendorId);
      c.add(d.coProductCode);
      c.add(d.coRevision);
      c.add(d.coSerial);
    }
    if (d.hasName) {
      char nm[20];
      snprintf(nm, sizeof(nm), "%016llX", (unsigned long long)d.j1939Name);
      o["j1939Name"] = nm;
    }
    if (d.driver[0]) o["driver"] = d.driver;
    if (d.nWatch) {
      JsonArray w = o["watch"].to<JsonArray>();
      for (int k = 0; k < d.nWatch; k++) watch_to_json(d.watch[k], w.add<JsonObject>(), false);
    }
  }
}

String dev_export_json() {
  JsonDocument doc;
  doc["format"] = "wonderscope-devices";
  doc["version"] = 1;
  lock();
  dev_persist_json(doc);
  unlock();
  String s;
  serializeJsonPretty(doc, s);
  return s;
}

const char *dev_import_json(const char *json, size_t len) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, len);
  if (e) return "invalid JSON";
  JsonArrayConst arr = doc["devices"];
  if (arr.isNull()) return "missing \"devices\" array";
  lock();
  for (JsonObjectConst o : arr) {
    int bus = bus_from_name(o["bus"]), proto = proto_from_name(o["proto"]);
    int addr = o["addr"] | -1;
    if (bus < 0 || proto < 0 || addr < 0 || addr > 255) continue;
    const char *err = dev_apply_meta(bus, proto, addr, o);
    if (err) continue;
    Device *d = dev_acquire(bus, proto, addr, false);
    if (!d) continue;
    copy_str(d->vendor, sizeof(d->vendor), o["vendor"] | d->vendor);
    copy_str(d->product, sizeof(d->product), o["product"] | d->product);
    copy_str(d->revision, sizeof(d->revision), o["revision"] | d->revision);
    copy_str(d->name, sizeof(d->name), o["name"] | d->name);
    copy_str(d->serial, sizeof(d->serial), o["serial"] | d->serial);
    JsonArrayConst co = o["co"];
    if (co.size() == 5) {
      d->coIdentity = true;
      d->coDeviceType = co[0];
      d->coVendorId = co[1];
      d->coProductCode = co[2];
      d->coRevision = co[3];
      d->coSerial = co[4];
    }
    const char *nm = o["j1939Name"];
    if (nm) {
      d->j1939Name = strtoull(nm, nullptr, 16);
      d->hasName = true;
    }
    dev_release(d, true);
  }
  schedule_persist();
  unlock();
  return nullptr;
}

bool dev_save_now() {
  if (!fsOk) return false;
  String s;
  {
    JsonDocument doc;
    doc["format"] = "wonderscope-devices";
    doc["version"] = 1;
    lock();
    dev_persist_json(doc);
    unlock();
    serializeJson(doc, s);
  }
  File f = LittleFS.open("/devices.tmp", "w");
  if (!f) return false;
  size_t w = f.print(s);
  f.close();
  if (w != s.length()) return false;
  return LittleFS.rename("/devices.tmp", DEV_FILE);  // LittleFS rename replaces the target atomically
}

void dev_loop() {
  if (persistPending && (int32_t)(uptime_ms() - persistDueMs) >= 0) {
    persistPending = false;
    if (!dev_save_now()) Serial.println("[dev] save failed");
  }
}
