#pragma once
// Device registry: everything discovered on either bus, actively or passively.
// Thread-safe via a single mutex; use dev_acquire()/dev_release() for
// multi-field updates.
#include "common.h"

constexpr int MAX_DEVICES = 256;
constexpr int MAX_WATCH = 16;
constexpr int MAX_PGNS = 32;

struct WatchItem {
  char name[24];
  uint8_t fn;     // Modbus: 1 coils, 2 discrete, 3 holding, 4 input. CANopen, I2C, SPI: 0
  uint16_t addr;  // Modbus start address, CANopen object index, I2C/SPI register
  uint8_t sub;    // CANopen sub-index; I2C register address width in bytes (1 or 2)
  uint8_t count;  // Modbus quantity (registers 1..8 / bits 1..64); I2C/SPI bytes (1..16)
  char fmt[8];    // display format; interpreted by the UI (u16,s16,u32,s32,f32,hex,bool,str)
  float scale;
  char unit[8];
  // live value
  uint8_t raw[16];
  uint8_t rawLen;
  int16_t err;  // 0 ok, -1 timeout, >0 Modbus exception / SDO abort class
  uint32_t abortCode;
  uint32_t tsMs;
};

struct PgnEntry {
  uint32_t pgn;
  uint32_t count;
  uint32_t lastMs;
  uint8_t len;
  uint8_t data[8];
};

constexpr int MAX_VALUES = 8;
struct DevValue {
  char name[16];
  char unit[8];
  float v;
};

struct Device {
  bool used;
  uint8_t bus, proto, addr;
  uint32_t ver;  // bumped on every change; used to push deltas to the UI
  char label[32];
  char notes[96];
  uint32_t firstSeenMs, lastSeenMs;
  bool present;  // heard from since boot
  bool passive;  // only ever seen passively (never answered us)
  uint32_t rx, errs;
  uint16_t consecErr;
  int16_t state;  // CANopen NMT state; -1 unknown
  // Modbus link parameters this device answered at
  uint32_t baud;
  uint8_t parity, stop;
  // identity
  char vendor[40], product[40], revision[24], name[40], serial[24];
  uint32_t coDeviceType, coVendorId, coProductCode, coRevision, coSerial;
  bool coIdentity;
  uint16_t emcyCode;
  uint8_t emcyReg;
  uint32_t emcyMs, emcyCount;
  uint64_t j1939Name;
  bool hasName;
  // polling
  uint16_t pollMs;
  uint32_t nextPollMs;
  WatchItem watch[MAX_WATCH];
  uint8_t nWatch;
  // J1939 PGNs seen from this source address
  PgnEntry *pgns;
  uint8_t nPgns;
  // I2C decoding driver and its latest values
  char driver[12];
  DevValue vals[MAX_VALUES];
  uint8_t nVals;
  uint32_t valsTs;
  int16_t valsErr;
};

void dev_init();
void dev_loop();  // debounced persistence; call from loop()

// Find or create (create=true) and lock. Returns nullptr if not found / full.
Device *dev_acquire(uint8_t bus, uint8_t proto, uint8_t addr, bool create);
void dev_release(Device *d, bool changed, bool persist = false);

// Convenience: mark device seen (creating it). passive=true for sniffed traffic.
void dev_seen(uint8_t bus, uint8_t proto, uint8_t addr, bool passive);
void dev_failed(uint8_t bus, uint8_t proto, uint8_t addr);  // no reply to an active request

bool dev_parse_key(const char *key, uint8_t &bus, uint8_t &proto, uint8_t &addr);
String dev_key(uint8_t bus, uint8_t proto, uint8_t addr);

void dev_to_json(const Device &d, JsonObject o, bool full);
void dev_list_json(JsonArray arr, int busFilter = -1);
bool dev_get_json(uint8_t bus, uint8_t proto, uint8_t addr, JsonObject o);
size_t dev_collect_changed(JsonArray arr, size_t max);  // since last call
const char *dev_apply_meta(uint8_t bus, uint8_t proto, uint8_t addr, JsonObjectConst meta);
bool dev_remove(uint8_t bus, uint8_t proto, uint8_t addr);
int dev_clear(int busFilter);
size_t dev_count(int busFilter = -1);

// Polling: copy the next due device's watch list (values not needed).
struct PollTask {
  uint8_t bus, proto, addr;
  uint32_t baud;
  uint8_t parity, stop;
  uint8_t nWatch;
  WatchItem watch[MAX_WATCH];
  char driver[12];
};
bool dev_next_poll(uint8_t bus, uint32_t nowMs, PollTask &out);
void dev_set_values(uint8_t bus, uint8_t proto, uint8_t addr, const DevValue *vals, uint8_t n, int16_t err);
void dev_set_watch_value(uint8_t bus, uint8_t proto, uint8_t addr, uint8_t idx, const uint8_t *raw, uint8_t len,
                         int16_t err, uint32_t abortCode);

// J1939 helpers
void dev_j1939_pgn(uint8_t sa, uint32_t pgn, const uint8_t *data, uint8_t len, bool passiveCreate);

// Persistence of the whole registry (labels, watch lists, identities)
bool dev_save_now();
String dev_export_json();
const char *dev_import_json(const char *json, size_t len);
