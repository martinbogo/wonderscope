// CAN engine (ESP32-S3 TWAI controller + TJA1051 transceiver).
//
// One task owns the TWAI driver: it receives every frame (trace, ID table,
// passive CANopen/J1939 discovery) and runs jobs synchronously. Jobs that need
// a reply call can_wait(), which keeps processing all other traffic while
// waiting for the matching frame.
//
// Default mode is listen-only: the controller never ACKs or sends error
// frames; connecting at the wrong bitrate does not disturb the bus.

#include "can.h"

#include <driver/twai.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "devices.h"
#include "settings.h"
#include "trace.h"

static QueueHandle_t jobQ;
static bool installed;
static uint8_t curMode = CAN_MODE_LISTEN;
static uint32_t curBitrate;
static volatile bool cancelReq;
static const char *volatile busy = "";
static twai_status_info_t twaiStat;
static bool recovering;
static uint32_t lastStatusMs, lastRateMs;

// ------------------------------------------------------------------ ID table

struct IdEntry {
  uint32_t key;  // id | ext<<31 ; EMPTY when unused
  uint8_t dlc, rtr;
  uint8_t data[8];
  uint32_t count, prevCount, firstMs, lastMs;
  float rate;
};
constexpr uint32_t EMPTY = 0xFFFFFFFF;
constexpr int ID_SLOTS = 1024;  // open addressing, power of two
constexpr int ID_MAX = 768;     // keep load factor sane
static IdEntry *ids;
static int idCount;
static SemaphoreHandle_t idMtx;

static void ids_reset() {
  for (int i = 0; i < ID_SLOTS; i++) ids[i].key = EMPTY;
  idCount = 0;
}

static void ids_update(const twai_message_t &m) {
  uint32_t key = m.identifier | (m.extd ? 0x80000000u : 0);
  uint32_t h = (key * 2654435761u) & (ID_SLOTS - 1);
  xSemaphoreTake(idMtx, portMAX_DELAY);
  for (int probe = 0; probe < ID_SLOTS; probe++) {
    IdEntry &e = ids[(h + probe) & (ID_SLOTS - 1)];
    if (e.key == key || (e.key == EMPTY && idCount < ID_MAX)) {
      uint32_t now = uptime_ms();
      if (e.key == EMPTY) {
        memset(&e, 0, sizeof(e));
        e.key = key;
        e.firstMs = now;
        idCount++;
      }
      e.dlc = m.data_length_code;
      e.rtr = m.rtr;
      memcpy(e.data, m.data, m.data_length_code > 8 ? 8 : m.data_length_code);
      e.count++;
      e.lastMs = now;
      break;
    }
    if (e.key == EMPTY) break;  // table full
  }
  xSemaphoreGive(idMtx);
}

static void ids_rates() {
  uint32_t now = uptime_ms();
  float dt = (now - lastRateMs) / 1000.0f;
  if (dt <= 0) return;
  xSemaphoreTake(idMtx, portMAX_DELAY);
  for (int i = 0; i < ID_SLOTS; i++) {
    IdEntry &e = ids[i];
    if (e.key == EMPTY) continue;
    e.rate = (e.count - e.prevCount) / dt;
    e.prevCount = e.count;
  }
  xSemaphoreGive(idMtx);
  lastRateMs = now;
}

size_t can_ids_json(JsonArray arr, size_t max) {
  static IdEntry *tmp;
  if (!tmp) tmp = (IdEntry *)heap_caps_malloc(sizeof(IdEntry) * ID_MAX, MALLOC_CAP_SPIRAM);
  if (!tmp) return 0;
  int n = 0;
  xSemaphoreTake(idMtx, portMAX_DELAY);
  for (int i = 0; i < ID_SLOTS && n < ID_MAX; i++)
    if (ids[i].key != EMPTY) tmp[n++] = ids[i];
  xSemaphoreGive(idMtx);
  std::sort(tmp, tmp + n, [](const IdEntry &a, const IdEntry &b) { return a.key < b.key; });
  uint32_t now = uptime_ms();
  size_t out = 0;
  for (int i = 0; i < n && out < max; i++, out++) {
    const IdEntry &e = tmp[i];
    JsonArray r = arr.add<JsonArray>();
    r.add(e.key & 0x1FFFFFFF);
    r.add((e.key >> 31) ? 1 : 0);
    r.add(e.dlc);
    r.add(e.count);
    r.add(roundf(e.rate * 10) / 10);
    r.add(now - e.lastMs);
    r.add(hex_string(e.data, e.rtr ? 0 : (e.dlc > 8 ? 8 : e.dlc)));
    r.add(e.rtr);
  }
  return out;
}

void can_ids_clear() {
  xSemaphoreTake(idMtx, portMAX_DELAY);
  ids_reset();
  xSemaphoreGive(idMtx);
}

size_t can_ids_count() { return idCount; }

// ------------------------------------------------------------------ driver

static bool timing_for(uint32_t bps, twai_timing_config_t *t) {
  switch (bps) {
#ifdef TWAI_TIMING_CONFIG_10KBITS
    case 10000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_10KBITS(); *t = x; return true; }
#endif
#ifdef TWAI_TIMING_CONFIG_20KBITS
    case 20000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_20KBITS(); *t = x; return true; }
#endif
    case 50000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_50KBITS(); *t = x; return true; }
    case 100000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_100KBITS(); *t = x; return true; }
    case 125000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_125KBITS(); *t = x; return true; }
    case 250000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_250KBITS(); *t = x; return true; }
    case 500000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_500KBITS(); *t = x; return true; }
    case 800000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_800KBITS(); *t = x; return true; }
    case 1000000: { twai_timing_config_t x = TWAI_TIMING_CONFIG_1MBITS(); *t = x; return true; }
  }
  return false;
}

static void drv_uninstall() {
  if (!installed) return;
  twai_stop();
  twai_driver_uninstall();
  installed = false;
  recovering = false;
  memset(&twaiStat, 0, sizeof(twaiStat));
}

enum { MODE_SELFTEST = 2 };

static bool drv_install(uint32_t bps, uint8_t mode) {
  drv_uninstall();
  twai_timing_config_t t;
  if (!timing_for(bps, &t)) return false;
  twai_mode_t m = mode == CAN_MODE_LISTEN ? TWAI_MODE_LISTEN_ONLY
                  : mode == MODE_SELFTEST ? TWAI_MODE_NO_ACK
                                          : TWAI_MODE_NORMAL;
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)pins::CAN_TX, (gpio_num_t)pins::CAN_RX, m);
  g.rx_queue_len = 128;
  g.tx_queue_len = 16;
  g.alerts_enabled = TWAI_ALERT_NONE;
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  installed = true;
  curMode = mode;
  curBitrate = bps;
  return true;
}

static void apply_settings() {
  const CanSettings &s = g_settings.can;
  if (s.enabled) {
    if (!drv_install(s.bitrate, s.mode)) Serial.println("[can] TWAI init failed");
  } else {
    drv_uninstall();
  }
}

static const char *state_name() {
  if (!installed) return "off";
  switch (twaiStat.state) {
    case TWAI_STATE_STOPPED: return "stopped";
    case TWAI_STATE_BUS_OFF: return "bus-off";
    case TWAI_STATE_RECOVERING: return "recovering";
    default: break;
  }
  if (twaiStat.tx_error_counter >= 128 || twaiStat.rx_error_counter >= 128) return "error-passive";
  if (twaiStat.tx_error_counter >= 96 || twaiStat.rx_error_counter >= 96) return "warning";
  return "running";
}

static void poll_status() {
  if (!installed) return;
  twai_get_status_info(&twaiStat);
  if (twaiStat.state == TWAI_STATE_BUS_OFF && !recovering && g_settings.can.autoRecover) {
    twai_initiate_recovery();
    recovering = true;
  } else if (twaiStat.state == TWAI_STATE_STOPPED && recovering) {
    twai_start();
    recovering = false;
  }
}

// ------------------------------------------------------------------ J1939 transport (BAM receive)

uint32_t j1939_pgn_of(uint32_t id) {
  uint8_t pf = (id >> 16) & 0xFF, ps = (id >> 8) & 0xFF;
  uint32_t dp = (id >> 24) & 3;  // EDP + DP
  return (dp << 16) | ((uint32_t)pf << 8) | (pf >= 240 ? ps : 0);
}

struct TpSession {
  bool active;
  uint8_t sa;
  uint32_t pgn;
  uint16_t size;
  uint8_t packets, next;
  uint32_t ms;
  uint8_t buf[1785];
};
constexpr int TP_SLOTS = 6;
static TpSession *tp;

// Last completed multi-packet message, for jobs waiting on a TP reply.
static struct {
  uint32_t seq;
  uint8_t sa;
  uint32_t pgn;
  uint16_t len;
  uint8_t data[256];
} tpLast;

static void split_star_fields(const uint8_t *d, size_t n, char fields[][40], int maxFields) {
  int f = 0;
  size_t o = 0;
  for (int k = 0; k < maxFields; k++) fields[k][0] = 0;
  for (size_t i = 0; i < n && f < maxFields; i++) {
    if (d[i] == '*') {
      fields[f][o] = 0;
      f++;
      o = 0;
      continue;
    }
    if (o < 39 && d[i] >= 32 && d[i] < 127) fields[f][o++] = d[i];
    fields[f][o] = 0;
  }
}

static void tp_complete(uint8_t sa, uint32_t pgn, const uint8_t *d, uint16_t len) {
  tpLast.sa = sa;
  tpLast.pgn = pgn;
  tpLast.len = len > sizeof(tpLast.data) ? sizeof(tpLast.data) : len;
  memcpy(tpLast.data, d, tpLast.len);
  tpLast.seq++;
  char f[5][40];
  Device *dev = nullptr;
  if (pgn == 65259) {  // Component Identification: make*model*serial*unit*
    split_star_fields(d, len, f, 4);
    dev = dev_acquire(BUS_CAN, PROTO_J1939, sa, true);
    if (dev) {
      if (f[0][0]) strlcpy(dev->vendor, f[0], sizeof(dev->vendor));
      if (f[1][0]) strlcpy(dev->product, f[1], sizeof(dev->product));
      if (f[2][0]) strlcpy(dev->serial, f[2], sizeof(dev->serial));
    }
  } else if (pgn == 65242 && len > 1) {  // Software Identification: n, field*...
    split_star_fields(d + 1, len - 1, f, 1);
    dev = dev_acquire(BUS_CAN, PROTO_J1939, sa, true);
    if (dev && f[0][0]) strlcpy(dev->revision, f[0], sizeof(dev->revision));
  } else if (pgn == 64965) {  // ECU Identification: part*serial*location*type*manufacturer*
    split_star_fields(d, len, f, 5);
    dev = dev_acquire(BUS_CAN, PROTO_J1939, sa, true);
    if (dev) {
      if (f[0][0] && !dev->product[0]) strlcpy(dev->product, f[0], sizeof(dev->product));
      if (f[1][0] && !dev->serial[0]) strlcpy(dev->serial, f[1], sizeof(dev->serial));
      if (f[3][0]) strlcpy(dev->name, f[3], sizeof(dev->name));
      if (f[4][0] && !dev->vendor[0]) strlcpy(dev->vendor, f[4], sizeof(dev->vendor));
    }
  }
  if (dev) dev_release(dev, true, true);
  dev_j1939_pgn(sa, pgn, d, len > 8 ? 8 : len, g_settings.can.j1939Passive);
}

static void tp_frame(uint8_t sa, uint32_t pgn, const twai_message_t &m) {
  if (!tp) return;
  uint32_t now = uptime_ms();
  if (pgn == 0xEC00 && m.data_length_code == 8 && m.data[0] == 0x20) {  // TP.CM_BAM
    int slot = -1;
    for (int i = 0; i < TP_SLOTS; i++)
      if (tp[i].active && tp[i].sa == sa) slot = i;
    for (int i = 0; slot < 0 && i < TP_SLOTS; i++)
      if (!tp[i].active || now - tp[i].ms > 2000) slot = i;
    if (slot < 0) return;
    TpSession &s = tp[slot];
    s.active = true;
    s.sa = sa;
    s.size = m.data[1] | (m.data[2] << 8);
    s.packets = m.data[3];
    s.pgn = m.data[5] | (m.data[6] << 8) | ((uint32_t)m.data[7] << 16);
    s.next = 1;
    s.ms = now;
    if (s.size > sizeof(s.buf) || s.packets == 0) s.active = false;
  } else if (pgn == 0xEB00 && m.data_length_code == 8) {  // TP.DT
    for (int i = 0; i < TP_SLOTS; i++) {
      TpSession &s = tp[i];
      if (!s.active || s.sa != sa) continue;
      if (m.data[0] != s.next) {
        s.active = false;
        return;
      }
      size_t off = (size_t)(s.next - 1) * 7;
      for (int k = 0; k < 7 && off + k < s.size; k++) s.buf[off + k] = m.data[1 + k];
      s.ms = now;
      if (s.next == s.packets) {
        s.active = false;
        tp_complete(sa, s.pgn, s.buf, s.size);
      } else {
        s.next++;
      }
      return;
    }
  }
}

// ------------------------------------------------------------------ passive decoding

static volatile int claimCollect;  // >0 while a J1939 scan collects claims
static uint8_t claimSas[64];
static int nClaims;

static void passive(const twai_message_t &m) {
  if (m.rtr) return;
  const CanSettings &s = g_settings.can;
  if (!m.extd) {
    uint32_t id = m.identifier;
    uint8_t fc = id >> 7, node = id & 0x7F;
    if (!node) return;
    if (fc == 0x0E && m.data_length_code == 1) {  // heartbeat / boot-up (0x700+n)
      Device *d = dev_acquire(BUS_CAN, PROTO_CANOPEN, node, s.canopenPassive);
      if (!d) return;
      int16_t st = m.data[0] & 0x7F;
      bool changed = d->state != st || !d->present;
      bool wasPresent = d->present;
      d->state = st;
      d->present = true;
      if (!wasPresent && d->rx == 0) d->passive = true;
      d->lastSeenMs = uptime_ms();
      d->rx++;
      d->consecErr = 0;
      static uint32_t lastBump[128];
      if (d->lastSeenMs - lastBump[node] > 1000) changed = true;
      if (changed) lastBump[node] = d->lastSeenMs;
      dev_release(d, changed);
    } else if (fc == 0x01 && m.data_length_code == 8) {  // EMCY (0x080+n)
      Device *d = dev_acquire(BUS_CAN, PROTO_CANOPEN, node, s.canopenPassive);
      if (!d) return;
      d->emcyCode = m.data[0] | (m.data[1] << 8);
      d->emcyReg = m.data[2];
      d->emcyMs = uptime_ms();
      d->emcyCount++;
      d->present = true;
      d->lastSeenMs = d->emcyMs;
      dev_release(d, true);
    } else if (fc == 0x0B || fc == 0x03 || fc == 0x05 || fc == 0x07 || fc == 0x09) {
      // SDO response / TPDO1-4 from an already registered node
      Device *d = dev_acquire(BUS_CAN, PROTO_CANOPEN, node, false);
      if (d) {
        dev_release(d, false);
        dev_seen(BUS_CAN, PROTO_CANOPEN, node, true);
      }
    }
    return;
  }
  // 29-bit: treat as J1939
  uint32_t id = m.identifier;
  uint8_t sa = id & 0xFF;
  uint32_t pgn = j1939_pgn_of(id);
  if (pgn == 0xEE00 && m.data_length_code == 8 && sa < 254) {  // Address Claimed
    Device *d = dev_acquire(BUS_CAN, PROTO_J1939, sa, true);
    if (d) {
      uint64_t name = 0;
      for (int i = 7; i >= 0; i--) name = (name << 8) | m.data[i];
      bool changed = !d->hasName || d->j1939Name != name;
      d->j1939Name = name;
      d->hasName = true;
      d->passive = false;
      dev_release(d, true, changed);
    }
    if (claimCollect && nClaims < 64) {
      bool dup = false;
      for (int i = 0; i < nClaims; i++) dup |= claimSas[i] == sa;
      if (!dup) claimSas[nClaims++] = sa;
    }
  }
  if (pgn == 0xEC00 || pgn == 0xEB00) tp_frame(sa, pgn, m);
  if (sa < 254) dev_j1939_pgn(sa, pgn, m.data, m.data_length_code, s.j1939Passive);
}

static void on_rx(const twai_message_t &m) {
  uint8_t fl = (m.extd ? TF_EXT : 0) | (m.rtr ? TF_RTR : 0);
  uint8_t dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
  trace_add(BUS_CAN, DIR_RX, fl, m.identifier, m.data, m.rtr ? 0 : dlc);
  ids_update(m);
  passive(m);
}

static void housekeeping() {
  uint32_t now = uptime_ms();
  if (now - lastStatusMs >= 100) {
    poll_status();
    lastStatusMs = now;
  }
  if (now - lastRateMs >= 1000) ids_rates();
}

// Wait for a frame matching pred, while processing all traffic normally.
template <typename F>
static bool can_wait(F pred, uint32_t timeoutMs, twai_message_t *out) {
  uint64_t deadline = uptime_us() + (uint64_t)timeoutMs * 1000;
  for (;;) {
    uint64_t now = uptime_us();
    if (now >= deadline || !installed) return false;
    uint32_t waitMs = (uint32_t)((deadline - now) / 1000) + 1;
    if (waitMs > 20) waitMs = 20;
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(waitMs)) == ESP_OK) {
      on_rx(m);
      if (pred(m)) {
        if (out) *out = m;
        return true;
      }
    }
    housekeeping();
  }
}

static void can_idle(uint32_t ms) {
  can_wait([](const twai_message_t &) { return false; }, ms, nullptr);
}

static esp_err_t can_send(uint32_t id, bool ext, bool rtr, const uint8_t *data, uint8_t dlc, bool self = false) {
  if (!installed) return ESP_ERR_INVALID_STATE;
  if (curMode == CAN_MODE_LISTEN) return ESP_ERR_NOT_SUPPORTED;
  twai_message_t m = {};
  m.identifier = id;
  m.extd = ext;
  m.rtr = rtr;
  m.self = self;
  m.data_length_code = dlc;
  if (!rtr && data) memcpy(m.data, data, dlc);
  esp_err_t e = twai_transmit(&m, pdMS_TO_TICKS(100));
  if (e == ESP_OK) trace_add(BUS_CAN, DIR_TX, (ext ? TF_EXT : 0) | (rtr ? TF_RTR : 0), id, m.data, rtr ? 0 : dlc);
  return e;
}

static const char *send_err(esp_err_t e) {
  switch (e) {
    case ESP_ERR_INVALID_STATE: return "CAN is disabled or bus-off";
    case ESP_ERR_NOT_SUPPORTED: return "CAN is listen-only; transmission requires active mode (can normal)";
    case ESP_ERR_TIMEOUT: return "TX queue full (no ACK on bus)";
    case ESP_FAIL: return "TX failed";
  }
  return esp_err_to_name(e);
}

// ------------------------------------------------------------------ CANopen SDO client

enum SdoStatus { SDO_OK = 0, SDO_TIMEOUT = -1, SDO_ABORT = -2, SDO_PROTO = -3, SDO_TXERR = -4 };

static const char *sdo_status_name(int s) {
  switch (s) {
    case SDO_OK: return "ok";
    case SDO_TIMEOUT: return "SDO timeout (no response)";
    case SDO_ABORT: return "SDO abort";
    case SDO_PROTO: return "SDO protocol error";
    case SDO_TXERR: return "transmit failed";
  }
  return "error";
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int sdo_upload(uint8_t node, uint16_t idx, uint8_t sub, uint8_t *out, size_t cap, size_t *len,
                      uint32_t *abortCode, uint16_t tmo) {
  *len = 0;
  *abortCode = 0;
  uint8_t req[8] = {0x40, (uint8_t)idx, (uint8_t)(idx >> 8), sub, 0, 0, 0, 0};
  esp_err_t e = can_send(0x600 + node, false, false, req, 8);
  if (e != ESP_OK) return SDO_TXERR;
  uint32_t rid = 0x580 + node;
  auto isResp = [&](const twai_message_t &m) {
    return !m.extd && m.identifier == rid && m.data_length_code == 8 && m.data[1] == (uint8_t)idx &&
           m.data[2] == (uint8_t)(idx >> 8) && m.data[3] == sub;
  };
  twai_message_t r;
  if (!can_wait(isResp, tmo, &r)) return SDO_TIMEOUT;
  uint8_t cs = r.data[0];
  if (cs == 0x80) {
    *abortCode = le32(r.data + 4);
    return SDO_ABORT;
  }
  if ((cs & 0xE0) != 0x40) return SDO_PROTO;
  if (cs & 0x02) {  // expedited
    size_t n = (cs & 0x01) ? 4 - ((cs >> 2) & 3) : 4;
    if (n > cap) n = cap;
    memcpy(out, r.data + 4, n);
    *len = n;
    return SDO_OK;
  }
  // segmented
  uint8_t toggle = 0;
  auto isSeg = [&](const twai_message_t &m) {
    return !m.extd && m.identifier == rid && m.data_length_code == 8;
  };
  for (int guard = 0; guard < 200; guard++) {
    uint8_t sreq[8] = {(uint8_t)(0x60 | (toggle << 4)), 0, 0, 0, 0, 0, 0, 0};
    if (can_send(0x600 + node, false, false, sreq, 8) != ESP_OK) return SDO_TXERR;
    if (!can_wait(isSeg, tmo, &r)) return SDO_TIMEOUT;
    uint8_t c = r.data[0];
    if (c == 0x80) {
      *abortCode = le32(r.data + 4);
      return SDO_ABORT;
    }
    if ((c & 0xE0) != 0x00 || ((c >> 4) & 1) != toggle) return SDO_PROTO;
    size_t n = 7 - ((c >> 1) & 7);
    for (size_t k = 0; k < n && *len < cap; k++) out[(*len)++] = r.data[1 + k];
    if (c & 0x01) return SDO_OK;
    toggle ^= 1;
  }
  return SDO_PROTO;
}

static int sdo_download(uint8_t node, uint16_t idx, uint8_t sub, const uint8_t *data, size_t n, uint32_t *abortCode,
                        uint16_t tmo) {
  *abortCode = 0;
  if (n < 1 || n > 4) return SDO_PROTO;  // expedited only
  uint8_t req[8] = {(uint8_t)(0x23 | ((4 - n) << 2)), (uint8_t)idx, (uint8_t)(idx >> 8), sub, 0, 0, 0, 0};
  memcpy(req + 4, data, n);
  if (can_send(0x600 + node, false, false, req, 8) != ESP_OK) return SDO_TXERR;
  uint32_t rid = 0x580 + node;
  auto isResp = [&](const twai_message_t &m) {
    return !m.extd && m.identifier == rid && m.data_length_code == 8 && m.data[1] == (uint8_t)idx &&
           m.data[2] == (uint8_t)(idx >> 8) && m.data[3] == sub;
  };
  twai_message_t r;
  if (!can_wait(isResp, tmo, &r)) return SDO_TIMEOUT;
  if (r.data[0] == 0x80) {
    *abortCode = le32(r.data + 4);
    return SDO_ABORT;
  }
  return r.data[0] == 0x60 ? SDO_OK : SDO_PROTO;
}

static void str_from(char *dst, size_t cap, const uint8_t *src, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap && src[i]; i++) dst[o++] = (src[i] >= 32 && src[i] < 127) ? src[i] : '.';
  dst[o] = 0;
}

// Read the standard identity objects into the registry.
static void co_identify(uint8_t node, uint16_t tmo, JsonObject out) {
  uint8_t buf[64];
  size_t n;
  uint32_t ab;
  uint32_t devType = 0, vid = 0, pc = 0, rev = 0, ser = 0;
  char name[40] = "", hw[24] = "", sw[24] = "";
  bool haveId = false;
  if (sdo_upload(node, 0x1000, 0, buf, sizeof(buf), &n, &ab, tmo) == SDO_OK && n >= 4) devType = le32(buf);
  if (sdo_upload(node, 0x1008, 0, buf, sizeof(buf), &n, &ab, tmo) == SDO_OK) str_from(name, sizeof(name), buf, n);
  if (sdo_upload(node, 0x1009, 0, buf, sizeof(buf), &n, &ab, tmo) == SDO_OK) str_from(hw, sizeof(hw), buf, n);
  if (sdo_upload(node, 0x100A, 0, buf, sizeof(buf), &n, &ab, tmo) == SDO_OK) str_from(sw, sizeof(sw), buf, n);
  uint32_t *dst[4] = {&vid, &pc, &rev, &ser};
  for (int s = 1; s <= 4; s++) {
    if (sdo_upload(node, 0x1018, s, buf, sizeof(buf), &n, &ab, tmo) == SDO_OK && n >= 4) {
      *dst[s - 1] = le32(buf);
      haveId = true;
    }
  }
  Device *d = dev_acquire(BUS_CAN, PROTO_CANOPEN, node, true);
  if (d) {
    d->coDeviceType = devType;
    if (haveId) {
      d->coIdentity = true;
      d->coVendorId = vid;
      d->coProductCode = pc;
      d->coRevision = rev;
      d->coSerial = ser;
    }
    if (name[0]) strlcpy(d->name, name, sizeof(d->name));
    if (hw[0] || sw[0]) snprintf(d->revision, sizeof(d->revision), "%s%s%s", hw, (hw[0] && sw[0]) ? " / " : "", sw);
    if (haveId) snprintf(d->serial, sizeof(d->serial), "%lu", (unsigned long)ser);
    dev_release(d, true, true);
  }
  out["deviceType"] = devType;
  if (name[0]) out["name"] = name;
  if (hw[0]) out["hwVersion"] = hw;
  if (sw[0]) out["swVersion"] = sw;
  if (haveId) {
    out["vendorId"] = vid;
    out["productCode"] = pc;
    out["revision"] = rev;
    out["serial"] = ser;
  }
}

static void mark_co_found(uint8_t node) {
  Device *d = dev_acquire(BUS_CAN, PROTO_CANOPEN, node, true);
  if (!d) return;
  d->present = true;
  d->passive = false;
  d->lastSeenMs = uptime_ms();
  d->rx++;
  d->consecErr = 0;
  dev_release(d, true, true);
}

// ------------------------------------------------------------------ jobs

static bool need_active(Job *j) {
  if (!installed) {
    reply_err(j->rt, "CAN disabled");
    return false;
  }
  if (curMode == CAN_MODE_LISTEN) {
    reply_err(j->rt, "CAN is listen-only; command requires active mode (can normal)");
    return false;
  }
  return true;
}

static void scan_event(const char *proto, const char *state, int cur, int from, int to, int found) {
  JsonDocument e;
  e["ev"] = "scan";
  e["bus"] = "can";
  e["proto"] = proto;
  e["state"] = state;
  e["cur"] = cur;
  e["from"] = from;
  e["to"] = to;
  e["found"] = found;
  event_send(e);
}

static void job_scan_canopen(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int from = a["from"] | 1, to = a["to"] | 127;
  if (from < 1) from = 1;
  if (to > 127) to = 127;
  if (from > to) return reply_err(j->rt, "invalid node range");
  uint16_t tmo = a["timeoutMs"] | g_settings.can.scanTimeoutMs;
  bool identify = a["identify"] | true;
  busy = "scan";
  cancelReq = false;
  JsonDocument res;
  JsonArray found = res["found"].to<JsonArray>();
  int nFound = 0;
  uint32_t lastEv = 0;
  for (int node = from; node <= to && !cancelReq; node++) {
    uint32_t now = uptime_ms();
    if (now - lastEv > 150 || node == from) {
      scan_event("canopen", "running", node, from, to, nFound);
      lastEv = now;
    }
    uint8_t buf[8];
    size_t n;
    uint32_t ab;
    int st = sdo_upload(node, 0x1000, 0, buf, sizeof(buf), &n, &ab, tmo);
    if (st == SDO_TXERR) {
      busy = "";
      scan_event("canopen", "cancelled", node, from, to, nFound);
      return reply_err(j->rt, "transmit failed at node %d (bus-off or no ACK)", node);
    }
    if (st == SDO_OK || st == SDO_ABORT || st == SDO_PROTO) {
      mark_co_found(node);
      JsonObject f = found.add<JsonObject>();
      f["node"] = node;
      if (identify) co_identify(node, g_settings.can.sdoTimeoutMs, f);
      nFound++;
      scan_event("canopen", "running", node, from, to, nFound);
    }
  }
  bool cancelled = cancelReq;
  cancelReq = false;
  busy = "";
  scan_event("canopen", cancelled ? "cancelled" : "done", to, from, to, nFound);
  res["cancelled"] = cancelled;
  reply_ok(j->rt, res);
}

static void job_scan_j1939(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  uint8_t sa = g_settings.can.j1939Sa;
  busy = "scan";
  nClaims = 0;
  claimCollect = 1;
  scan_event("j1939", "running", 0, 0, 253, 0);
  // Request for Address Claimed (PGN 60928) to global
  uint8_t req[3] = {0x00, 0xEE, 0x00};
  esp_err_t e = can_send((6u << 26) | (0xEAu << 16) | (0xFFu << 8) | sa, true, false, req, 3);
  if (e != ESP_OK) {
    claimCollect = 0;
    busy = "";
    scan_event("j1939", "cancelled", 0, 0, 253, 0);
    return reply_err(j->rt, "%s", send_err(e));
  }
  can_idle(1250);
  if (a["thorough"] | false) {
    // Ask everyone for Component ID (65259) and Software ID (65242); answers arrive via BAM.
    uint8_t r1[3] = {0xEB, 0xFE, 0x00}, r2[3] = {0xDA, 0xFE, 0x00};
    can_send((6u << 26) | (0xEAu << 16) | (0xFFu << 8) | sa, true, false, r1, 3);
    can_idle(1500);
    can_send((6u << 26) | (0xEAu << 16) | (0xFFu << 8) | sa, true, false, r2, 3);
    can_idle(1500);
  }
  claimCollect = 0;
  busy = "";
  JsonDocument res;
  JsonArray found = res["found"].to<JsonArray>();
  for (int i = 0; i < nClaims; i++) {
    JsonObject f = found.add<JsonObject>();
    f["sa"] = claimSas[i];
    Device *d = dev_acquire(BUS_CAN, PROTO_J1939, claimSas[i], false);
    if (d) {
      char nm[20];
      snprintf(nm, sizeof(nm), "%016llX", (unsigned long long)d->j1939Name);
      f["name"] = nm;
      if (d->vendor[0]) f["vendor"] = d->vendor;
      if (d->product[0]) f["product"] = d->product;
      dev_release(d, false);
    }
  }
  scan_event("j1939", "done", 253, 0, 253, nClaims);
  reply_ok(j->rt, res);
}

static void job_sdo_read(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int node = a["node"] | -1, idx = a["index"] | -1, sub = a["sub"] | 0;
  if (node < 1 || node > 127) return reply_err(j->rt, "node must be 1..127");
  if (idx < 0 || idx > 0xFFFF || sub < 0 || sub > 255) return reply_err(j->rt, "invalid index/sub");
  uint8_t buf[256];
  size_t n;
  uint32_t ab;
  int st = sdo_upload(node, idx, sub, buf, sizeof(buf), &n, &ab, a["timeoutMs"] | g_settings.can.sdoTimeoutMs);
  if (st == SDO_OK || st == SDO_ABORT) dev_seen(BUS_CAN, PROTO_CANOPEN, node, false);
  else if (st == SDO_TIMEOUT) dev_failed(BUS_CAN, PROTO_CANOPEN, node);
  JsonDocument res;
  res["node"] = node;
  res["index"] = idx;
  res["sub"] = sub;
  if (st == SDO_ABORT) {
    res["abort"] = ab;
    return reply_ok(j->rt, res);  // abort is a valid answer; UI explains the code
  }
  if (st != SDO_OK) return reply_err(j->rt, "%s", sdo_status_name(st));
  res["size"] = n;
  res["hex"] = hex_string(buf, n);
  if (n <= 4) {
    uint32_t v = 0;
    for (size_t k = 0; k < n; k++) v |= (uint32_t)buf[k] << (8 * k);
    res["value"] = v;
  } else {
    char s[257];
    str_from(s, sizeof(s), buf, n);
    res["text"] = s;
  }
  reply_ok(j->rt, res);
}

static void job_sdo_write(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int node = a["node"] | -1, idx = a["index"] | -1, sub = a["sub"] | 0;
  if (node < 1 || node > 127) return reply_err(j->rt, "node must be 1..127");
  if (idx < 0 || idx > 0xFFFF || sub < 0 || sub > 255) return reply_err(j->rt, "invalid index/sub");
  uint8_t data[8];
  int n;
  if (a["hex"].is<const char *>()) {
    n = hex_decode(a["hex"], data, sizeof(data));
  } else {
    int size = a["size"] | 4;
    if (size != 1 && size != 2 && size != 4) return reply_err(j->rt, "size must be 1, 2 or 4");
    uint32_t v = (uint32_t)a["value"].as<long long>();
    for (int k = 0; k < size; k++) data[k] = v >> (8 * k);
    n = size;
  }
  if (n < 1 || n > 4) return reply_err(j->rt, "expedited SDO write supports 1..4 bytes");
  uint32_t ab;
  int st = sdo_download(node, idx, sub, data, n, &ab, a["timeoutMs"] | g_settings.can.sdoTimeoutMs);
  JsonDocument res;
  res["node"] = node;
  res["index"] = idx;
  res["sub"] = sub;
  if (st == SDO_ABORT) {
    res["abort"] = ab;
    return reply_ok(j->rt, res);
  }
  if (st != SDO_OK) return reply_err(j->rt, "%s", sdo_status_name(st));
  dev_seen(BUS_CAN, PROTO_CANOPEN, node, false);
  res["written"] = n;
  reply_ok(j->rt, res);
}

static void job_nmt(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int node = a["node"] | -1;
  const char *c = a["cmd"] | "";
  uint8_t cs = !strcmp(c, "start") ? 0x01 : !strcmp(c, "stop") ? 0x02 : !strcmp(c, "preop") ? 0x80
               : !strcmp(c, "reset") ? 0x81 : !strcmp(c, "resetcomm") ? 0x82 : 0;
  if (!cs) return reply_err(j->rt, "cmd must be start|stop|preop|reset|resetcomm");
  if (node < 0 || node > 127) return reply_err(j->rt, "node must be 0..127 (0 = all)");
  uint8_t d[2] = {cs, (uint8_t)node};
  esp_err_t e = can_send(0x000, false, false, d, 2);
  if (e != ESP_OK) return reply_err(j->rt, "%s", send_err(e));
  JsonDocument res;
  res["node"] = node;
  res["cmd"] = c;
  reply_ok(j->rt, res);
}

static void job_co_info(Job *j) {
  int node = j->args["node"] | -1;
  if (node < 1 || node > 127) return reply_err(j->rt, "node must be 1..127");
  uint8_t buf[8];
  size_t n;
  uint32_t ab;
  int st = sdo_upload(node, 0x1000, 0, buf, sizeof(buf), &n, &ab, g_settings.can.sdoTimeoutMs);
  if (st == SDO_TIMEOUT) {
    dev_failed(BUS_CAN, PROTO_CANOPEN, node);
    return reply_err(j->rt, "node %d did not respond", node);
  }
  if (st == SDO_TXERR) return reply_err(j->rt, "transmit failed");
  mark_co_found(node);
  JsonDocument res;
  res["node"] = node;
  co_identify(node, g_settings.can.sdoTimeoutMs, res.to<JsonObject>());
  reply_ok(j->rt, res);
}

static uint32_t j1939_id(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa) {
  uint8_t pf = (pgn >> 8) & 0xFF;
  uint32_t id = ((uint32_t)(prio & 7) << 26) | ((pgn & 0x30000) << 8) | ((uint32_t)pf << 16) | sa;
  id |= (pf < 240 ? (uint32_t)da : (pgn & 0xFF)) << 8;
  return id;
}

static void job_j1939_request(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int da = a["da"] | 255;
  long pgn = a["pgn"] | -1L;
  if (pgn < 0 || pgn > 0x3FFFF) return reply_err(j->rt, "pgn must be 0..262143");
  if (da < 0 || da > 255) return reply_err(j->rt, "da must be 0..255");
  uint16_t tmo = a["timeoutMs"] | 1250;
  uint8_t req[3] = {(uint8_t)pgn, (uint8_t)(pgn >> 8), (uint8_t)(pgn >> 16)};
  uint32_t tpSeq = tpLast.seq;
  esp_err_t e = can_send(j1939_id(6, 0xEA00, da, g_settings.can.j1939Sa), true, false, req, 3);
  if (e != ESP_OK) return reply_err(j->rt, "%s", send_err(e));
  JsonDocument res;
  res["pgn"] = pgn;
  res["da"] = da;
  JsonArray rs = res["responses"].to<JsonArray>();
  uint64_t deadline = uptime_us() + (uint64_t)tmo * 1000;
  while (uptime_us() < deadline) {
    twai_message_t m;
    uint32_t left = (uint32_t)((deadline - uptime_us()) / 1000) + 1;
    bool got = can_wait(
        [&](const twai_message_t &mm) {
          return mm.extd && j1939_pgn_of(mm.identifier) == (uint32_t)pgn &&
                 (da == 255 || (mm.identifier & 0xFF) == (uint32_t)da);
        },
        left, &m);
    if (tpLast.seq != tpSeq) {  // a multi-packet answer completed
      tpSeq = tpLast.seq;
      if (tpLast.pgn == (uint32_t)pgn && (da == 255 || tpLast.sa == da)) {
        JsonObject o = rs.add<JsonObject>();
        o["sa"] = tpLast.sa;
        o["data"] = hex_string(tpLast.data, tpLast.len);
        o["tp"] = true;
        if (da != 255) break;
      }
    }
    if (got) {
      JsonObject o = rs.add<JsonObject>();
      o["sa"] = m.identifier & 0xFF;
      o["data"] = hex_string(m.data, m.data_length_code);
      if (da != 255) break;
    }
    if (!got) break;
  }
  reply_ok(j->rt, res);
}

static void job_j1939_send(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  long pgn = a["pgn"] | -1L;
  int da = a["da"] | 255, prio = a["prio"] | 6;
  if (pgn < 0 || pgn > 0x3FFFF) return reply_err(j->rt, "pgn must be 0..262143");
  uint8_t data[8];
  int n = hex_decode(a["hex"] | "", data, sizeof(data));
  if (n < 0) return reply_err(j->rt, "data: invalid hex or more than 8 bytes (multi-packet send not supported)");
  uint32_t id = j1939_id(prio, pgn, da, g_settings.can.j1939Sa);
  esp_err_t e = can_send(id, true, false, data, n);
  if (e != ESP_OK) return reply_err(j->rt, "%s", send_err(e));
  JsonDocument res;
  res["id"] = id;
  reply_ok(j->rt, res);
}

static void job_send(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  long id = a["id"] | -1L;
  bool ext = a["ext"] | (id > 0x7FF);
  bool rtr = a["rtr"] | false;
  if (id < 0 || id > (ext ? 0x1FFFFFFF : 0x7FF)) return reply_err(j->rt, "id out of range");
  uint8_t data[8];
  int n = 0;
  if (rtr) n = a["dlc"] | 0;
  else n = hex_decode(a["hex"] | "", data, sizeof(data));
  if (n < 0 || n > 8) return reply_err(j->rt, "data must be 0..8 bytes of hex");
  int count = a["count"] | 1, interval = a["intervalMs"] | 0;
  if (count < 1 || count > 100000) return reply_err(j->rt, "count must be 1..100000");
  busy = count > 1 ? "send" : "";
  cancelReq = false;
  int sent = 0;
  for (int i = 0; i < count && !cancelReq; i++) {
    esp_err_t e = can_send(id, ext, rtr, data, n);
    if (e != ESP_OK) {
      busy = "";
      return reply_err(j->rt, "after %d frames: %s", sent, send_err(e));
    }
    sent++;
    if (interval && i + 1 < count) can_idle(interval);
  }
  busy = "";
  JsonDocument res;
  res["sent"] = sent;
  reply_ok(j->rt, res);
}

static void job_autobaud(Job *j) {
  uint16_t window = j->args["windowMs"] | 400;
  static const uint32_t order[] = {500000, 250000, 125000, 1000000, 100000, 50000, 800000, 20000, 10000};
  uint32_t prevRate = g_settings.can.bitrate;
  busy = "autobaud";
  cancelReq = false;
  JsonDocument res;
  JsonArray tried = res["tried"].to<JsonArray>();
  uint32_t best = 0, bestFrames = 0;
  for (uint32_t br : order) {
    if (cancelReq) break;
    twai_timing_config_t t;
    if (!timing_for(br, &t)) continue;
    if (!drv_install(br, CAN_MODE_LISTEN)) continue;
    JsonDocument e;
    e["ev"] = "autobaud";
    e["bitrate"] = br;
    event_send(e);
    uint32_t rx0 = g_counters[BUS_CAN].rx;
    twai_status_info_t s0;
    twai_get_status_info(&s0);
    can_idle(window);
    twai_status_info_t s1;
    twai_get_status_info(&s1);
    uint32_t frames = g_counters[BUS_CAN].rx - rx0;
    uint32_t errs = s1.bus_error_count - s0.bus_error_count;
    JsonObject o = tried.add<JsonObject>();
    o["bitrate"] = br;
    o["frames"] = frames;
    o["errors"] = errs;
    if (frames > 0 && errs == 0 && frames > bestFrames) {
      best = br;
      bestFrames = frames;
      break;  // first error-free rate with traffic
    }
  }
  busy = "";
  if (best) {
    bool changed = best != prevRate;
    g_settings.can.bitrate = best;
    if (changed) g_settings.can.mode = CAN_MODE_LISTEN;  // stay passive until the user opts in
    g_settings.can.enabled = true;
    settings_save();
    res["detected"] = best;
  } else {
    res["detected"] = nullptr;
  }
  apply_settings();
  res["cancelled"] = (bool)cancelReq;
  cancelReq = false;
  reply_ok(j->rt, res);
}

// Loopback self-test: TWAI "no-ack" mode with self-reception. Transmits one
// frame on the bus, so it is only run on explicit request.
static void job_selftest(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  uint32_t id = a["id"] | 0x7FFu;  // outside CANopen's predefined IDs (LSS uses 7E4/7E5)
  if (!drv_install(g_settings.can.bitrate, MODE_SELFTEST)) {
    apply_settings();
    return reply_err(j->rt, "could not start TWAI in self-test mode");
  }
  uint8_t pat[8] = {0xB5, 0x5C, 0x0B, 0xE5, 0x01, 0x02, 0x03, 0x04};
  uint64_t t0 = uptime_us();
  esp_err_t e = can_send(id, false, false, pat, 8, true);
  twai_message_t m;
  bool ok = e == ESP_OK && can_wait(
                               [&](const twai_message_t &mm) {
                                 return !mm.extd && mm.identifier == id && mm.data_length_code == 8 &&
                                        !memcmp(mm.data, pat, 8);
                               },
                               500, &m);
  uint32_t us = (uint32_t)(uptime_us() - t0);
  twai_status_info_t s;
  twai_get_status_info(&s);
  apply_settings();
  JsonDocument res;
  res["pass"] = ok;
  res["roundTripUs"] = us;
  res["txErr"] = s.tx_error_counter;
  res["sendResult"] = esp_err_to_name(e);
  reply_ok(j->rt, res);
}

static void run_job(Job *j) {
  const String &c = j->cmd;
  if (c == "can.apply") {
    apply_settings();
    JsonDocument res;
    can_status_json(res.to<JsonObject>());
    return reply_ok(j->rt, res);
  }
  if (c == "can.autobaud") return job_autobaud(j);
  if (c == "can.selftest") return job_selftest(j);
  if (c == "can.recover") {
    if (!installed) return reply_err(j->rt, "CAN is disabled");
    if (twaiStat.state != TWAI_STATE_BUS_OFF) {
      apply_settings();  // full restart clears error counters
    } else {
      twai_initiate_recovery();
      recovering = true;
    }
    return reply_ok(j->rt);
  }
  if (c == "scan") {
    const char *p = j->args["proto"] | "canopen";
    if (!need_active(j)) return;
    if (!strcmp(p, "j1939")) return job_scan_j1939(j);
    return job_scan_canopen(j);
  }
  if (!need_active(j)) return;
  if (c == "co.sdo.read") job_sdo_read(j);
  else if (c == "co.sdo.write") job_sdo_write(j);
  else if (c == "co.nmt") job_nmt(j);
  else if (c == "co.info") job_co_info(j);
  else if (c == "j1939.request") job_j1939_request(j);
  else if (c == "j1939.send") job_j1939_send(j);
  else if (c == "can.send") job_send(j);
  else reply_err(j->rt, "unknown can command %s", c.c_str());
}

static void do_poll(const PollTask &p) {
  if (p.proto != PROTO_CANOPEN || curMode == CAN_MODE_LISTEN) return;
  bool anyOk = false;
  for (uint8_t i = 0; i < p.nWatch; i++) {
    const WatchItem &w = p.watch[i];
    uint8_t buf[16];
    size_t n;
    uint32_t ab;
    int st = sdo_upload(p.addr, w.addr, w.sub, buf, sizeof(buf), &n, &ab, g_settings.can.sdoTimeoutMs);
    dev_set_watch_value(BUS_CAN, PROTO_CANOPEN, p.addr, i, buf, n, st == SDO_OK ? 0 : (st == SDO_ABORT ? 1 : st), ab);
    if (st == SDO_OK || st == SDO_ABORT) anyOk = true;
    if (uxQueueMessagesWaiting(jobQ)) break;
  }
  if (anyOk) dev_seen(BUS_CAN, PROTO_CANOPEN, p.addr, false);
  else dev_failed(BUS_CAN, PROTO_CANOPEN, p.addr);
}

static void task(void *) {
  apply_settings();
  for (;;) {
    Job *j;
    if (xQueueReceive(jobQ, &j, 0) == pdTRUE) {
      run_job(j);
      delete j;
      continue;
    }
    if (!installed) {
      if (xQueueReceive(jobQ, &j, pdMS_TO_TICKS(100)) == pdTRUE) {
        run_job(j);
        delete j;
      }
      continue;
    }
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(10)) == ESP_OK) on_rx(m);
    housekeeping();
    PollTask p;
    if (curMode != CAN_MODE_LISTEN && dev_next_poll(BUS_CAN, uptime_ms(), p)) {
      busy = "poll";
      do_poll(p);
      busy = "";
    }
  }
}

void can_begin() {
  ids = (IdEntry *)heap_caps_malloc(sizeof(IdEntry) * ID_SLOTS, MALLOC_CAP_SPIRAM);
  tp = (TpSession *)heap_caps_calloc(TP_SLOTS, sizeof(TpSession), MALLOC_CAP_SPIRAM);
  idMtx = xSemaphoreCreateMutex();
  ids_reset();
  jobQ = xQueueCreate(16, sizeof(Job *));
  xTaskCreatePinnedToCore(task, "can", 8192, nullptr, 6, nullptr, 1);
}

bool can_submit(Job *job) {
  if (xQueueSend(jobQ, &job, 0) != pdTRUE) {
    reply_err(job->rt, "CAN queue full - try again");
    delete job;
    return false;
  }
  return true;
}

void can_cancel() { cancelReq = true; }

void can_status_json(JsonObject o) {
  can_settings_to_json(g_settings.can, o);
  o["up"] = installed;
  o["state"] = state_name();
  o["tec"] = twaiStat.tx_error_counter;
  o["rec"] = twaiStat.rx_error_counter;
  o["busErrors"] = twaiStat.bus_error_count;
  o["rxMissed"] = twaiStat.rx_missed_count;
  o["rxOverrun"] = twaiStat.rx_overrun_count;
  o["txFailed"] = twaiStat.tx_failed_count;
  o["arbLost"] = twaiStat.arb_lost_count;
  o["busy"] = (const char *)busy;
  o["queued"] = jobQ ? uxQueueMessagesWaiting(jobQ) : 0;
  o["rx"] = g_counters[BUS_CAN].rx;
  o["tx"] = g_counters[BUS_CAN].tx;
  o["ids"] = idCount;
}
