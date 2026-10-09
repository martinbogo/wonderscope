// RS485 / Modbus RTU engine.
//
// One task owns UART1. Between jobs it sniffs the bus, splitting frames on
// inter-character silence (Modbus t3.5). Jobs (scan, read, write, raw) run
// synchronously in the same task, so there is never contention on the UART.

#include "rs485.h"

#include <driver/uart.h>

#include "devices.h"
#include "modbus.h"
#include "rpc.h"
#include "settings.h"
#include "trace.h"

static const uart_port_t PORT = UART_NUM_1;
static QueueHandle_t jobQ;
static QueueHandle_t uartQ;
static bool up;
static volatile bool cancelReq;
static const char *volatile busy = "";

struct Link {
  uint32_t baud;
  uint8_t parity, stop;
  bool operator!=(const Link &o) const { return baud != o.baud || parity != o.parity || stop != o.stop; }
};
static Link cur;
static uint32_t gapUs = 4000;

static uint8_t rxBuf[TRACE_MAX_DATA + 4];

// ------------------------------------------------------------------ UART

static uart_parity_t to_parity(uint8_t p) {
  return p == 1 ? UART_PARITY_EVEN : p == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE;
}

static void compute_gap() {
  uint32_t bitsPerChar = 1 + 8 + (cur.parity ? 1 : 0) + cur.stop;
  uint32_t charUs = bitsPerChar * 1000000UL / cur.baud;
  if (!charUs) charUs = 1;
  uint32_t t35 = charUs * 35 / 10;
  if (cur.baud > 19200 && t35 < 1750) t35 = 1750;  // fixed value per Modbus spec
  gapUs = t35;
  uint32_t sym = (t35 + charUs - 1) / charUs;
  if (sym < 2) sym = 2;
  if (sym > 90) sym = 90;
  uart_set_rx_timeout(PORT, (uint8_t)sym);
}

static void link_apply(const Link &l) {
  if (!up) return;
  if (l != cur) {
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(100));
    uart_set_baudrate(PORT, l.baud);
    uart_set_parity(PORT, to_parity(l.parity));
    uart_set_stop_bits(PORT, l.stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1);
    cur = l;
    compute_gap();
  }
}

static Link settings_link() {
  const Rs485Settings &s = g_settings.rs485;
  return Link{s.baud, s.parity, s.stop};
}

static void uart_down() {
  if (!up) return;
  uart_driver_delete(PORT);
  uartQ = nullptr;
  up = false;
}

static bool uart_up() {
  uart_down();
  Link l = settings_link();
  uart_config_t c = {};
  c.baud_rate = (int)l.baud;
  c.data_bits = UART_DATA_8_BITS;
  c.parity = to_parity(l.parity);
  c.stop_bits = l.stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  c.rx_flow_ctrl_thresh = 122;
  c.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(PORT, 2048, 0, 64, &uartQ, 0) != ESP_OK) return false;
  if (uart_param_config(PORT, &c) != ESP_OK ||
      uart_set_pin(PORT, pins::RS485_TX, pins::RS485_RX, pins::RS485_DE, UART_PIN_NO_CHANGE) != ESP_OK ||
      uart_set_mode(PORT, UART_MODE_RS485_HALF_DUPLEX) != ESP_OK) {
    uart_driver_delete(PORT);
    uartQ = nullptr;
    return false;
  }
  up = true;
  cur = l;
  compute_gap();
  return true;
}

static void apply_settings() {
  if (g_settings.rs485.enabled) {
    if (!uart_up()) Serial.println("[rs485] UART init failed");
  } else {
    uart_down();
  }
}

// Read one frame (bytes until t3.5 silence). Waits up to firstMs for the
// first byte. Returns false on timeout with no data.
static bool read_frame(uint8_t *buf, size_t cap, size_t *len, uint32_t firstMs, uint8_t *flags) {
  *len = 0;
  *flags = 0;
  if (!up) {
    vTaskDelay(pdMS_TO_TICKS(firstMs));
    return false;
  }
  uint64_t deadline = uptime_us() + (uint64_t)firstMs * 1000;
  uint32_t gapMs = gapUs / 1000 + 2;
  for (;;) {
    uint32_t waitMs;
    if (*len == 0) {
      uint64_t now = uptime_us();
      if (now >= deadline) return false;
      waitMs = (uint32_t)((deadline - now) / 1000) + 1;
    } else {
      waitMs = gapMs;
    }
    uart_event_t ev;
    if (xQueueReceive(uartQ, &ev, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
      if (*len) return true;  // silence after data: end of frame
      if (uptime_us() >= deadline) return false;
      continue;
    }
    switch (ev.type) {
      case UART_DATA: {
        size_t want = ev.size;
        if (*len + want > cap) {
          *flags |= TF_TRUNC;
          want = cap - *len;
        }
        int n = want ? uart_read_bytes(PORT, buf + *len, want, 0) : 0;
        if (n > 0) *len += n;
        if (*len >= cap) {
          uart_flush_input(PORT);
          return true;
        }
        if (ev.timeout_flag && *len) return true;
        break;
      }
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        *flags |= TF_ERR;
        uart_flush_input(PORT);
        xQueueReset(uartQ);
        if (*len) return true;
        break;
      case UART_PARITY_ERR:
      case UART_FRAME_ERR:
        *flags |= TF_ERR;
        break;
      default:
        break;
    }
  }
}

// ------------------------------------------------------------------ Modbus master

// Send req (CRC already appended). If match, only accept a reply from the
// same address; otherwise return the first frame heard.
static int transact(const uint8_t *req, size_t reqLen, uint8_t *resp, size_t *respLen, uint16_t timeoutMs,
                    bool match = true) {
  *respLen = 0;
  if (!up) return MB_DISABLED;
  uart_flush_input(PORT);
  xQueueReset(uartQ);
  uart_write_bytes(PORT, (const char *)req, reqLen);
  uart_wait_tx_done(PORT, pdMS_TO_TICKS(500));
  trace_add(BUS_RS485, DIR_TX, mb_crc_ok(req, reqLen) ? TF_CRC_OK : 0, req[0], req, reqLen);
  if (match && req[0] == 0) {  // broadcast: no reply by definition
    vTaskDelay(pdMS_TO_TICKS(gapUs / 1000 + 5));
    return MB_OK;
  }
  uint64_t deadline = uptime_us() + (uint64_t)timeoutMs * 1000;
  for (;;) {
    uint64_t now = uptime_us();
    if (now >= deadline) return MB_TIMEOUT;
    size_t n;
    uint8_t fl;
    if (!read_frame(resp, TRACE_MAX_DATA, &n, (uint32_t)((deadline - now) / 1000) + 1, &fl)) return MB_TIMEOUT;
    bool ok = mb_crc_ok(resp, n);
    trace_add(BUS_RS485, DIR_RX, (ok ? TF_CRC_OK : 0) | fl, n ? resp[0] : 0, resp, n);
    *respLen = n;
    if (!match) return ok ? MB_OK : MB_CRC;
    if (!ok) {
      if (n && resp[0] == req[0]) return MB_CRC;
      continue;  // noise or another device
    }
    if (resp[0] != req[0]) continue;
    if (resp[1] == (req[1] | 0x80)) return n >= 5 ? resp[2] : MB_BADRESP;
    if (resp[1] != req[1]) return MB_BADRESP;
    return MB_OK;
  }
}

// Read coils/discretes (fn 1/2) or registers (fn 3/4). Raw data bytes (as on
// the wire, after the byte count) go to out.
static int mb_read(uint8_t addr, uint8_t fn, uint16_t start, uint16_t count, uint8_t *out, size_t *outLen,
                   uint16_t timeoutMs) {
  *outLen = 0;
  if (fn < 1 || fn > 4 || !count) return MB_BADARG;
  if ((fn <= 2 && count > 2000) || (fn >= 3 && count > 125)) return MB_BADARG;
  uint8_t req[8] = {addr, fn, (uint8_t)(start >> 8), (uint8_t)start, (uint8_t)(count >> 8), (uint8_t)count};
  size_t rl = mb_append_crc(req, 6);
  uint8_t resp[TRACE_MAX_DATA];
  size_t n;
  int st = transact(req, rl, resp, &n, timeoutMs);
  if (st != MB_OK) return st;
  size_t expect = fn <= 2 ? (count + 7) / 8 : count * 2;
  if (n < 5 || resp[2] != expect || n != 5 + expect) return MB_BADRESP;
  memcpy(out, resp + 3, expect);
  *outLen = expect;
  return MB_OK;
}

static int mb_write(uint8_t addr, uint8_t fn, uint16_t start, const uint16_t *vals, uint16_t n, uint16_t timeoutMs) {
  uint8_t req[TRACE_MAX_DATA];
  size_t len = 0;
  req[len++] = addr;
  req[len++] = fn;
  req[len++] = start >> 8;
  req[len++] = start & 0xFF;
  if (fn == 5) {
    if (n != 1) return MB_BADARG;
    req[len++] = vals[0] ? 0xFF : 0x00;
    req[len++] = 0x00;
  } else if (fn == 6) {
    if (n != 1) return MB_BADARG;
    req[len++] = vals[0] >> 8;
    req[len++] = vals[0] & 0xFF;
  } else if (fn == 15) {
    if (n < 1 || n > 1968) return MB_BADARG;
    uint8_t bc = (n + 7) / 8;
    req[len++] = n >> 8;
    req[len++] = n & 0xFF;
    req[len++] = bc;
    memset(req + len, 0, bc);
    for (uint16_t i = 0; i < n; i++)
      if (vals[i]) req[len + i / 8] |= 1 << (i % 8);
    len += bc;
  } else if (fn == 16) {
    if (n < 1 || n > 123) return MB_BADARG;
    req[len++] = n >> 8;
    req[len++] = n & 0xFF;
    req[len++] = n * 2;
    for (uint16_t i = 0; i < n; i++) {
      req[len++] = vals[i] >> 8;
      req[len++] = vals[i] & 0xFF;
    }
  } else {
    return MB_BADARG;
  }
  len = mb_append_crc(req, len);
  uint8_t resp[TRACE_MAX_DATA];
  size_t rn;
  int st = transact(req, len, resp, &rn, timeoutMs);
  if (st != MB_OK) return st;
  if (addr == 0) return MB_OK;
  if (rn != 8 || resp[2] != req[2] || resp[3] != req[3]) return MB_BADRESP;
  return MB_OK;
}

static void copy_printable(char *dst, size_t cap, const uint8_t *src, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) dst[o++] = (src[i] >= 32 && src[i] < 127) ? src[i] : '.';
  dst[o] = 0;
}

// Try "Read Device Identification" (0x2B/0x0E) and "Report Server ID" (0x11).
// Results go straight into the registry.
static void mb_identify(uint8_t addr, uint16_t timeoutMs, JsonObject out) {
  uint8_t req[8] = {addr, 0x2B, 0x0E, 0x01, 0x00};
  size_t rl = mb_append_crc(req, 5);
  uint8_t resp[TRACE_MAX_DATA];
  size_t n;
  char vendor[40] = "", product[40] = "", rev[24] = "", name[40] = "";
  int st = transact(req, rl, resp, &n, timeoutMs);
  if (st == MB_OK && n >= 10) {
    uint8_t nObj = resp[7];
    size_t p = 8;
    for (uint8_t k = 0; k < nObj && p + 2 <= n - 2; k++) {
      uint8_t id = resp[p], ol = resp[p + 1];
      p += 2;
      if (p + ol > n - 2) break;
      if (id == 0) copy_printable(vendor, sizeof(vendor), resp + p, ol);
      else if (id == 1) copy_printable(product, sizeof(product), resp + p, ol);
      else if (id == 2) copy_printable(rev, sizeof(rev), resp + p, ol);
      p += ol;
    }
    out["devId"] = true;
  } else {
    out["devId"] = mb_status_name(st);
  }
  uint8_t req2[4] = {addr, 0x11};
  rl = mb_append_crc(req2, 2);
  st = transact(req2, rl, resp, &n, timeoutMs);
  if (st == MB_OK && n >= 5 && resp[2] + 5 == n) {
    out["serverId"] = hex_string(resp + 3, resp[2]);
    // Server ID payload is vendor specific; keep a printable copy if it looks like text.
    size_t printable = 0;
    for (size_t i = 0; i < resp[2]; i++)
      if (resp[3 + i] >= 32 && resp[3 + i] < 127) printable++;
    if (resp[2] > 2 && printable * 10 >= resp[2] * 7) copy_printable(name, sizeof(name), resp + 3, resp[2]);
  }
  Device *d = dev_acquire(BUS_RS485, PROTO_MODBUS, addr, true);
  if (d) {
    if (vendor[0]) strlcpy(d->vendor, vendor, sizeof(d->vendor));
    if (product[0]) strlcpy(d->product, product, sizeof(d->product));
    if (rev[0]) strlcpy(d->revision, rev, sizeof(d->revision));
    if (name[0]) strlcpy(d->name, name, sizeof(d->name));
    dev_release(d, true, vendor[0] || product[0] || name[0]);
  }
  if (vendor[0]) out["vendor"] = vendor;
  if (product[0]) out["product"] = product;
  if (rev[0]) out["revision"] = rev;
  if (name[0]) out["name"] = name;
}

static void mark_found(uint8_t addr) {
  Device *d = dev_acquire(BUS_RS485, PROTO_MODBUS, addr, true);
  if (!d) return;
  d->present = true;
  d->passive = false;
  d->lastSeenMs = uptime_ms();
  d->rx++;
  d->consecErr = 0;
  d->baud = cur.baud;
  d->parity = cur.parity;
  d->stop = cur.stop;
  dev_release(d, true, true);
}

// Link parameters for talking to a device: explicit args > stored device > global.
static Link link_for(JsonObjectConst a, uint8_t addr) {
  Link l = settings_link();
  Device *d = dev_acquire(BUS_RS485, PROTO_MODBUS, addr, false);
  if (d) {
    if (d->baud) l = Link{d->baud, d->parity, (uint8_t)(d->stop ? d->stop : 1)};
    dev_release(d, false);
  }
  if (!a["baud"].isNull()) l.baud = a["baud"];
  if (a["parity"].is<const char *>()) {
    char c = toupper(a["parity"].as<const char *>()[0]);
    l.parity = c == 'E' ? 1 : c == 'O' ? 2 : 0;
  }
  if (!a["stop"].isNull()) l.stop = a["stop"].as<int>() == 2 ? 2 : 1;
  return l;
}

// ------------------------------------------------------------------ jobs

static void send_scan_event(const char *state, int cur_, int from, int to, int found, int pass, int passes) {
  JsonDocument e;
  e["ev"] = "scan";
  e["bus"] = "rs485";
  e["proto"] = "modbus";
  e["state"] = state;
  e["cur"] = cur_;
  e["from"] = from;
  e["to"] = to;
  e["found"] = found;
  e["baud"] = cur.baud;
  e["parity"] = cur.parity == 1 ? "E" : cur.parity == 2 ? "O" : "N";
  e["pass"] = pass;
  e["passes"] = passes;
  event_send(e);
}

static void job_scan(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int from = a["from"] | 1, to = a["to"] | 247;
  if (from < 1) from = 1;
  if (to > 247) to = 247;
  if (from > to) return reply_err(j->rt, "invalid address range");
  bool thorough = a["thorough"] | false;
  uint16_t tmo = a["timeoutMs"] | g_settings.rs485.scanTimeoutMs;

  // Link configurations to try (baud sweep), default: current settings.
  Link links[16];
  int nl = 0;
  JsonArrayConst ls = a["links"];
  if (!ls.isNull()) {
    for (JsonVariantConst v : ls) {
      if (nl >= 16) break;
      Link l = settings_link();
      if (v.is<uint32_t>()) l.baud = v.as<uint32_t>();
      else {
        l.baud = v["baud"] | l.baud;
        const char *p = v["parity"] | "N";
        l.parity = toupper(p[0]) == 'E' ? 1 : toupper(p[0]) == 'O' ? 2 : 0;
        l.stop = (v["stop"] | 1) == 2 ? 2 : 1;
      }
      if (l.baud >= 300 && l.baud <= 1000000) links[nl++] = l;
    }
  }
  if (!nl) links[nl++] = settings_link();

  busy = "scan";
  cancelReq = false;
  JsonDocument res;
  JsonArray found = res["found"].to<JsonArray>();
  int nFound = 0, scanned = 0;
  uint32_t lastEv = 0;
  for (int li = 0; li < nl && !cancelReq; li++) {
    link_apply(links[li]);
    for (int addr = from; addr <= to && !cancelReq; addr++) {
      uint32_t now = uptime_ms();
      if (now - lastEv > 150 || addr == from) {
        send_scan_event("running", addr, from, to, nFound, li + 1, nl);
        lastEv = now;
      }
      scanned++;
      uint8_t buf[16];
      size_t bl;
      int st = mb_read(addr, 3, 0, 1, buf, &bl, tmo);
      if (st == MB_TIMEOUT && thorough) {
        st = mb_read(addr, 4, 0, 1, buf, &bl, tmo);
        if (st == MB_TIMEOUT) st = mb_read(addr, 1, 0, 1, buf, &bl, tmo);
      }
      if (st == MB_OK || st > 0 || st == MB_BADRESP) {
        mark_found(addr);
        JsonObject f = found.add<JsonObject>();
        f["addr"] = addr;
        f["baud"] = cur.baud;
        f["parity"] = cur.parity == 1 ? "E" : cur.parity == 2 ? "O" : "N";
        f["probe"] = mb_status_name(st);
        mb_identify(addr, tmo + 50, f);
        nFound++;
        send_scan_event("running", addr, from, to, nFound, li + 1, nl);
      } else if (st == MB_CRC) {
        // Reply with bad CRC: typically a baud/parity mismatch.
        JsonObject f = res["garbled"].add<JsonObject>();
        f["addr"] = addr;
        f["baud"] = cur.baud;
      }
    }
  }
  bool cancelled = cancelReq;
  cancelReq = false;
  link_apply(settings_link());
  busy = "";
  send_scan_event(cancelled ? "cancelled" : "done", to, from, to, nFound, nl, nl);
  res["scanned"] = scanned;
  res["cancelled"] = cancelled;
  reply_ok(j->rt, res);
}

static void job_read(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int addr = a["addr"] | -1, fn = a["fn"] | 3, start = a["start"] | 0, count = a["count"] | 1;
  if (addr < 1 || addr > 247) return reply_err(j->rt, "addr must be 1..247");
  if (start < 0 || start > 65535) return reply_err(j->rt, "start must be 0..65535");
  link_apply(link_for(a, addr));
  uint8_t data[256];
  size_t n;
  uint16_t tmo = a["timeoutMs"] | g_settings.rs485.timeoutMs;
  int st = mb_read(addr, fn, start, count, data, &n, tmo);
  link_apply(settings_link());
  if (st == MB_OK) dev_seen(BUS_RS485, PROTO_MODBUS, addr, false);
  else if (st == MB_TIMEOUT) dev_failed(BUS_RS485, PROTO_MODBUS, addr);
  if (st != MB_OK) return reply_err(j->rt, "%s", mb_status_name(st));
  JsonDocument res;
  res["addr"] = addr;
  res["fn"] = fn;
  res["start"] = start;
  res["count"] = count;
  res["raw"] = hex_string(data, n);
  JsonArray v = res["values"].to<JsonArray>();
  if (fn <= 2) {
    for (int i = 0; i < count; i++) v.add((data[i / 8] >> (i % 8)) & 1);
  } else {
    for (int i = 0; i < count; i++) v.add((uint16_t)((data[2 * i] << 8) | data[2 * i + 1]));
  }
  reply_ok(j->rt, res);
}

static void job_write(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int addr = a["addr"] | -1, fn = a["fn"] | 6, start = a["start"] | -1;
  if (addr < 0 || addr > 247) return reply_err(j->rt, "addr must be 0..247 (0 = broadcast)");
  if (start < 0 || start > 65535) return reply_err(j->rt, "start must be 0..65535");
  static uint16_t vals[130];
  uint16_t n = 0;
  JsonArrayConst va = a["values"];
  if (va.isNull()) {
    if (a["value"].isNull()) return reply_err(j->rt, "missing values");
    vals[n++] = (uint16_t)a["value"].as<long>();
  } else {
    for (JsonVariantConst v : va) {
      if (n >= 123 && fn == 16) return reply_err(j->rt, "too many values (max 123)");
      if (n >= 130) break;
      vals[n++] = v.is<bool>() ? (v.as<bool>() ? 1 : 0) : (uint16_t)v.as<long>();
    }
  }
  if (fn == 6 && n > 1) fn = 16;
  if (fn == 5 && n > 1) fn = 15;
  link_apply(link_for(a, addr));
  int st = mb_write(addr, fn, start, vals, n, a["timeoutMs"] | g_settings.rs485.timeoutMs);
  link_apply(settings_link());
  if (st == MB_OK && addr) dev_seen(BUS_RS485, PROTO_MODBUS, addr, false);
  else if (st == MB_TIMEOUT) dev_failed(BUS_RS485, PROTO_MODBUS, addr);
  if (st != MB_OK) return reply_err(j->rt, "%s", mb_status_name(st));
  JsonDocument res;
  res["addr"] = addr;
  res["fn"] = fn;
  res["start"] = start;
  res["written"] = n;
  reply_ok(j->rt, res);
}

static void job_raw(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  uint8_t req[TRACE_MAX_DATA];
  int n = hex_decode(a["hex"] | "", req, sizeof(req) - 2);
  if (n <= 0) return reply_err(j->rt, "hex: no data or invalid hex");
  bool crc = a["crc"] | true;
  bool expect = a["expect"] | true;
  if (crc) n = mb_append_crc(req, n);
  JsonDocument res;
  res["tx"] = hex_string(req, n);
  if (!up) return reply_err(j->rt, "RS485 is disabled");
  if (!expect) {
    uart_write_bytes(PORT, (const char *)req, n);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(500));
    trace_add(BUS_RS485, DIR_TX, mb_crc_ok(req, n) ? TF_CRC_OK : 0, req[0], req, n);
    return reply_ok(j->rt, res);
  }
  uint8_t resp[TRACE_MAX_DATA];
  size_t rn;
  int st = transact(req, n, resp, &rn, a["timeoutMs"] | g_settings.rs485.timeoutMs, false);
  if (rn) {
    res["rx"] = hex_string(resp, rn);
    res["crcOk"] = mb_crc_ok(resp, rn);
  }
  res["status"] = mb_status_name(st);
  reply_ok(j->rt, res);
}

static void job_ident(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int addr = a["addr"] | -1;
  if (addr < 1 || addr > 247) return reply_err(j->rt, "addr must be 1..247");
  link_apply(link_for(a, addr));
  uint16_t tmo = a["timeoutMs"] | g_settings.rs485.timeoutMs;
  uint8_t buf[16];
  size_t bl;
  int st = mb_read(addr, 3, 0, 1, buf, &bl, tmo);
  JsonDocument res;
  res["addr"] = addr;
  res["probe"] = mb_status_name(st);
  bool present = st == MB_OK || st > 0 || st == MB_BADRESP;
  res["present"] = present;
  if (present) {
    mark_found(addr);
    mb_identify(addr, tmo, res.as<JsonObject>());
  } else if (st == MB_TIMEOUT) {
    dev_failed(BUS_RS485, PROTO_MODBUS, addr);
  }
  link_apply(settings_link());
  reply_ok(j->rt, res);
}

static void run_job(Job *j) {
  const String &c = j->cmd;
  if (c == "rs485.apply") {
    apply_settings();
    JsonDocument res;
    rs485_status_json(res.to<JsonObject>());
    reply_ok(j->rt, res);
    return state_changed();
  }
  if (!up) return reply_err(j->rt, "RS485 disabled");
  if (c == "scan") job_scan(j);
  else if (c == "mb.read") job_read(j);
  else if (c == "mb.write") job_write(j);
  else if (c == "mb.raw") job_raw(j);
  else if (c == "mb.ident") job_ident(j);
  else reply_err(j->rt, "unknown rs485 command %s", c.c_str());
}

// ------------------------------------------------------------------ polling & sniffing

static void do_poll(const PollTask &p) {
  link_apply(p.baud ? Link{p.baud, p.parity, (uint8_t)(p.stop ? p.stop : 1)} : settings_link());
  bool anyOk = false;
  for (uint8_t i = 0; i < p.nWatch; i++) {
    const WatchItem &w = p.watch[i];
    uint8_t data[256];
    size_t n;
    int st = mb_read(p.addr, w.fn, w.addr, w.count, data, &n, g_settings.rs485.timeoutMs);
    dev_set_watch_value(BUS_RS485, PROTO_MODBUS, p.addr, i, data, n > 16 ? 16 : n, st == MB_OK ? 0 : st, 0);
    if (st == MB_OK || st > 0) anyOk = true;
    if (uxQueueMessagesWaiting(jobQ)) break;  // user action takes priority
  }
  link_apply(settings_link());
  if (anyOk) dev_seen(BUS_RS485, PROTO_MODBUS, p.addr, false);
  else dev_failed(BUS_RS485, PROTO_MODBUS, p.addr);
}

// Passive discovery: a valid request followed by a valid reply from the same
// address identifies a live device without active polling.
static void on_sniffed(const uint8_t *f, size_t n, uint8_t flags) {
  static uint8_t lastAddr, lastFn;
  static uint32_t lastMs;
  static bool lastValid;
  bool ok = mb_crc_ok(f, n);
  trace_add(BUS_RS485, DIR_RX, (ok ? TF_CRC_OK : 0) | flags, n ? f[0] : 0, f, n);
  if (!ok) return;
  uint8_t addr = f[0], fn = f[1];
  uint32_t now = uptime_ms();
  if (lastValid && addr && addr <= 247 && addr == lastAddr && (fn == lastFn || fn == (lastFn | 0x80)) &&
      now - lastMs < 1000) {
    Device *d = dev_acquire(BUS_RS485, PROTO_MODBUS, addr, true);
    if (d) {
      bool wasPresent = d->present;
      d->present = true;
      if (d->rx == 0) d->passive = true;
      d->rx++;
      d->lastSeenMs = now;
      if (!d->baud) {
        d->baud = cur.baud;
        d->parity = cur.parity;
        d->stop = cur.stop;
      }
      dev_release(d, true, !wasPresent);
    }
    lastValid = false;
  } else {
    lastAddr = addr;
    lastFn = fn;
    lastMs = now;
    lastValid = true;
  }
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
    if (!up) {
      if (xQueueReceive(jobQ, &j, pdMS_TO_TICKS(100)) == pdTRUE) {
        run_job(j);
        delete j;
      }
      continue;
    }
    size_t n;
    uint8_t fl;
    if (read_frame(rxBuf, TRACE_MAX_DATA, &n, 10, &fl)) {
      on_sniffed(rxBuf, n, fl);
      continue;
    }
    PollTask p;
    if (dev_next_poll(BUS_RS485, uptime_ms(), p)) {
      busy = "poll";
      do_poll(p);
      busy = "";
    }
  }
}

void rs485_begin() {
  jobQ = xQueueCreate(16, sizeof(Job *));
  xTaskCreatePinnedToCore(task, "rs485", 8192, nullptr, 5, nullptr, 1);
}

bool rs485_submit(Job *job) {
  if (xQueueSend(jobQ, &job, 0) != pdTRUE) {
    reply_err(job->rt, "RS485 queue full - try again");
    delete job;
    return false;
  }
  return true;
}

void rs485_cancel() { cancelReq = true; }

void rs485_status_json(JsonObject o) {
  rs485_settings_to_json(g_settings.rs485, o);
  o["up"] = up;
  o["busy"] = (const char *)busy;
  o["queued"] = jobQ ? uxQueueMessagesWaiting(jobQ) : 0;
  o["rx"] = g_counters[BUS_RS485].rx;
  o["tx"] = g_counters[BUS_RS485].tx;
  o["err"] = g_counters[BUS_RS485].err;
}
