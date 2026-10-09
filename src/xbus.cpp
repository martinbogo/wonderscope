// Expansion buses (I2C and SPI).
//
// The ESP32-S3 has two I2C controllers; the on-board RTC uses Wire (I2C0).
// Both external I2C buses share Wire1, which is re-bound to the selected
// bus's pins on demand. All bus access happens in this task.

#include "xbus.h"

#include <SPI.h>
#include <Wire.h>

#include "devices.h"
#include "i2c_drivers.h"
#include "rpc.h"
#include "settings.h"
#include "trace.h"

static QueueHandle_t jobQ;
static volatile bool cancelReq;
static const char *volatile busy = "";
static int curI2c = -1;  // bus bound to Wire1, -1 = none
static uint32_t curHz;
static SPIClass spi(FSPI);
static bool spiUp;
static uint32_t lastAutoScan;

static const I2cBusSettings &i2c_cfg(uint8_t bus) { return bus == BUS_QWIIC ? g_settings.qwiic : g_settings.i2c; }

// ------------------------------------------------------------------ I2C

static bool i2c_select(uint8_t bus) {
  const I2cBusSettings &c = i2c_cfg(bus);
  if (!c.enabled) return false;
  if (curI2c == bus && curHz == c.hz) return true;
  if (curI2c >= 0) Wire1.end();
  int sda = bus == BUS_QWIIC ? pins::QWIIC_SDA : pins::HDR_SDA;
  int scl = bus == BUS_QWIIC ? pins::QWIIC_SCL : pins::HDR_SCL;
  if (!Wire1.begin(sda, scl, c.hz)) {
    curI2c = -1;
    return false;
  }
  Wire1.setTimeOut(25);
  curI2c = bus;
  curHz = c.hz;
  return true;
}

static void i2c_release() {
  if (curI2c >= 0) Wire1.end();
  curI2c = -1;
}

bool xi2c_write(const I2cDev &d, const uint8_t *data, size_t n) {
  if (!i2c_select(d.bus)) return false;
  Wire1.beginTransmission(d.addr);
  if (n) Wire1.write(data, n);
  uint8_t e = Wire1.endTransmission(true);
  trace_add(d.bus, DIR_TX, e ? (TF_NACK | TF_ERR) : 0, d.addr, data, n);
  return e == 0;
}

bool xi2c_read(const I2cDev &d, uint8_t *data, size_t n) {
  if (!i2c_select(d.bus)) return false;
  size_t got = Wire1.requestFrom((uint16_t)d.addr, n, true);
  for (size_t i = 0; i < got && i < n; i++) data[i] = Wire1.read();
  trace_add(d.bus, DIR_RX, got == n ? 0 : (TF_NACK | TF_ERR), d.addr, data, got);
  return got == n;
}

bool xi2c_write_read(const I2cDev &d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn) {
  if (!i2c_select(d.bus)) return false;
  Wire1.beginTransmission(d.addr);
  Wire1.write(w, wn);
  uint8_t e = Wire1.endTransmission(false);  // repeated start
  trace_add(d.bus, DIR_TX, e ? (TF_NACK | TF_ERR) : 0, d.addr, w, wn);
  if (e) return false;
  size_t got = Wire1.requestFrom((uint16_t)d.addr, rn, true);
  for (size_t i = 0; i < got && i < rn; i++) r[i] = Wire1.read();
  trace_add(d.bus, DIR_RX, got == rn ? 0 : (TF_NACK | TF_ERR), d.addr, r, got);
  return got == rn;
}

bool xi2c_reg_read(const I2cDev &d, uint8_t reg, uint8_t *r, size_t rn) { return xi2c_write_read(d, &reg, 1, r, rn); }

bool xi2c_reg_write(const I2cDev &d, uint8_t reg, uint8_t value) {
  uint8_t b[2] = {reg, value};
  return xi2c_write(d, b, 2);
}

// Address probe without tracing. Returns 0 ACK, 1 NACK, 2 bus error.
static int i2c_probe(uint8_t bus, uint8_t addr) {
  if (!i2c_select(bus)) return 2;
  Wire1.beginTransmission(addr);
  uint8_t e = Wire1.endTransmission(true);
  if (e == 0) return 0;
  if (e == 2 || e == 3) return 1;
  return 2;
}

static void register_i2c(uint8_t bus, uint8_t addr, JsonObject out) {
  I2cDev d{bus, addr};
  I2cIdent id;
  i2c_identify(d, id);
  Device *dv = dev_acquire(bus, PROTO_I2C, addr, true);
  if (dv) {
    bool fresh = !dv->product[0] || id.confirmed;
    if (fresh && id.product[0]) strlcpy(dv->product, id.product, sizeof(dv->product));
    if (fresh && id.vendor[0]) strlcpy(dv->vendor, id.vendor, sizeof(dv->vendor));
    if (!dv->driver[0] && id.driver[0] && id.confirmed) {
      strlcpy(dv->driver, id.driver, sizeof(dv->driver));
      if (!dv->pollMs) dv->pollMs = 1000;
    }
    dv->present = true;
    dv->passive = false;
    dv->lastSeenMs = uptime_ms();
    dv->rx++;
    dv->consecErr = 0;
    dev_release(dv, true, true);
  }
  if (!out.isNull()) {
    out["addr"] = addr;
    if (id.product[0]) out["product"] = id.product;
    if (id.vendor[0]) out["vendor"] = id.vendor;
    if (id.driver[0]) out["driver"] = id.driver;
    out["confirmed"] = id.confirmed;
  }
}

static void scan_event(uint8_t bus, const char *state, int cur, int from, int to, int found) {
  JsonDocument e;
  e["ev"] = "scan";
  e["bus"] = bus_name(bus);
  e["proto"] = bus == BUS_SPI ? "spi" : "i2c";
  e["state"] = state;
  e["cur"] = cur;
  e["from"] = from;
  e["to"] = to;
  e["found"] = found;
  event_send(e);
}

static void job_i2c_scan(Job *j, uint8_t bus) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  int from = a["from"] | 0x08, to = a["to"] | 0x77;
  if (from < 0x03) from = 0x03;
  if (to > 0x77) to = 0x77;
  if (!i2c_select(bus)) return reply_err(j->rt, "%s is disabled", bus == BUS_QWIIC ? "Qwiic I2C" : "header I2C");
  busy = "scan";
  cancelReq = false;
  JsonDocument res;
  JsonArray found = res["found"].to<JsonArray>();
  int nFound = 0, busErr = 0;
  uint32_t lastEv = 0;
  for (int addr = from; addr <= to && !cancelReq; addr++) {
    if (uptime_ms() - lastEv > 150 || addr == from) {
      scan_event(bus, "running", addr, from, to, nFound);
      lastEv = uptime_ms();
    }
    int r = i2c_probe(bus, addr);
    if (r == 2 && ++busErr >= 3) {
      busy = "";
      scan_event(bus, "cancelled", addr, from, to, nFound);
      return reply_err(j->rt, "bus error at 0x%02X: SDA/SCL held low or missing pull-ups", addr);
    }
    if (r == 0) {
      register_i2c(bus, addr, found.add<JsonObject>());
      nFound++;
      scan_event(bus, "running", addr, from, to, nFound);
    }
  }
  bool cancelled = cancelReq;
  cancelReq = false;
  busy = "";
  scan_event(bus, cancelled ? "cancelled" : "done", to, from, to, nFound);
  res["cancelled"] = cancelled;
  reply_ok(j->rt, res);
}

// Periodic silent probe so devices plugged into Qwiic appear (and unplugged
// ones go offline) without a manual scan.
static void auto_scan(uint8_t bus) {
  if (!i2c_select(bus)) return;
  int errs = 0;
  for (int addr = 0x08; addr <= 0x77; addr++) {
    if (uxQueueMessagesWaiting(jobQ)) return;
    int r = i2c_probe(bus, addr);
    if (r == 2) {
      if (++errs >= 3) return;  // bus fault: try again next round
      continue;
    }
    Device *d = dev_acquire(bus, PROTO_I2C, addr, false);
    bool known = d != nullptr, present = d && d->present && d->consecErr < 3;
    if (d) dev_release(d, false);
    if (r == 0 && !present) {
      JsonObject none;
      register_i2c(bus, addr, none);
    } else if (r == 0 && known) {
      dev_seen(bus, PROTO_I2C, addr, false);
    } else if (r == 1 && present) {
      dev_failed(bus, PROTO_I2C, addr);
      dev_failed(bus, PROTO_I2C, addr);
      dev_failed(bus, PROTO_I2C, addr);
    }
  }
}

static uint8_t bus_arg(Job *j) {
  const char *b = j->args["bus"] | "qwiic";
  return (!strcmp(b, "i2c") || !strcmp(b, "header")) ? BUS_I2C : BUS_QWIIC;
}

static void job_i2c_rw(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  uint8_t bus = bus_arg(j);
  int addr = a["addr"] | -1;
  if (addr < 0x03 || addr > 0x77) return reply_err(j->rt, "address must be 0x03..0x77");
  if (!i2c_select(bus)) return reply_err(j->rt, "%s is disabled", bus == BUS_QWIIC ? "Qwiic I2C" : "header I2C");
  I2cDev d{bus, (uint8_t)addr};
  uint8_t w[34], r[64];
  int wn = 0, rn = 0;
  bool hasReg = !a["reg"].isNull();
  int regBytes = a["regBytes"] | 1;
  if (hasReg) {
    int reg = a["reg"].as<int>();
    if (regBytes == 2) w[wn++] = reg >> 8;
    w[wn++] = reg & 0xFF;
  }
  const String &c = j->cmd;
  if (c == "i2c.write" || c == "i2c.xfer") {
    int n = hex_decode(a["hex"] | "", w + wn, sizeof(w) - wn);
    if (n < 0) return reply_err(j->rt, "invalid hex (max 32 bytes)");
    wn += n;
  }
  if (c == "i2c.read" || c == "i2c.xfer") rn = a["count"] | (c == "i2c.read" ? 1 : 0);
  if (rn < 0 || rn > 64) return reply_err(j->rt, "count must be 0..64");
  bool ok;
  if (wn && rn) ok = xi2c_write_read(d, w, wn, r, rn);
  else if (rn) ok = xi2c_read(d, r, rn);
  else ok = xi2c_write(d, w, wn);
  if (!ok) {
    dev_failed(bus, PROTO_I2C, addr);
    return reply_err(j->rt, "no ACK from 0x%02X", addr);
  }
  dev_seen(bus, PROTO_I2C, addr, false);
  JsonDocument res;
  res["bus"] = bus_name(bus);
  res["addr"] = addr;
  if (hasReg) res["reg"] = a["reg"];
  if (wn) res["tx"] = hex_string(w, wn);
  if (rn) res["rx"] = hex_string(r, rn);
  reply_ok(j->rt, res);
}

static void job_i2c_ident(Job *j) {
  uint8_t bus = bus_arg(j);
  int addr = j->args["addr"] | -1;
  if (addr < 0x03 || addr > 0x77) return reply_err(j->rt, "address must be 0x03..0x77");
  int r = i2c_probe(bus, addr);
  if (r == 2) return reply_err(j->rt, "%s is disabled or the bus is faulty", bus == BUS_QWIIC ? "Qwiic I2C" : "header I2C");
  JsonDocument res;
  res["bus"] = bus_name(bus);
  res["present"] = r == 0;
  if (r == 0) register_i2c(bus, addr, res.as<JsonObject>());
  else dev_failed(bus, PROTO_I2C, addr);
  res["addr"] = addr;
  reply_ok(j->rt, res);
}

// ------------------------------------------------------------------ SPI

static bool spi_select() {
  const SpiSettings &s = g_settings.spi;
  if (!s.enabled) return false;
  if (!spiUp) {
    for (int i = 0; i < s.nCs; i++) {
      pinMode(s.cs[i], OUTPUT);
      digitalWrite(s.cs[i], HIGH);
    }
    spi.begin(pins::SPI_SCK, pins::SPI_MISO, pins::SPI_MOSI, -1);
    spiUp = true;
  }
  return true;
}

static void spi_release() {
  if (!spiUp) return;
  spi.end();
  spiUp = false;
}

static bool spi_cs_configured(int cs) {
  for (int i = 0; i < g_settings.spi.nCs; i++)
    if (g_settings.spi.cs[i] == cs) return true;
  return false;
}

static void spi_xfer(uint8_t cs, const uint8_t *tx, uint8_t *rx, size_t n) {
  const SpiSettings &s = g_settings.spi;
  static const uint8_t MODES[] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
  spi.beginTransaction(SPISettings(s.hz, MSBFIRST, MODES[s.mode & 3]));
  digitalWrite(cs, LOW);
  spi.transferBytes(tx, rx, n);
  digitalWrite(cs, HIGH);
  spi.endTransaction();
  trace_add(BUS_SPI, DIR_TX, 0, cs, tx, n);
  trace_add(BUS_SPI, DIR_RX, 0, cs, rx, n);
}

static bool all_same(const uint8_t *b, size_t n, uint8_t v) {
  for (size_t i = 0; i < n; i++)
    if (b[i] != v) return false;
  return true;
}

static const char *jedec_vendor(uint8_t m) {
  switch (m) {
    case 0xEF: return "Winbond";
    case 0xC2: return "Macronix";
    case 0x20: return "Micron / ST";
    case 0x1F: return "Adesto / Atmel";
    case 0xBF: return "SST / Microchip";
    case 0x9D: return "ISSI";
    case 0xC8: return "GigaDevice";
    case 0x01: return "Infineon / Cypress";
    case 0x68: return "Boya";
    case 0x85: return "Puya";
    case 0x0B: return "XTX";
    case 0x5E: return "Zbit";
  }
  return "";
}

// Identify whatever answers on one CS line.
static void spi_identify(uint8_t cs, JsonObject out) {
  uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4];
  spi_xfer(cs, tx, rx, 4);
  char product[40] = "", vendor[24] = "";
  bool present = false;
  if (!all_same(rx + 1, 3, 0x00) && !all_same(rx + 1, 3, 0xFF)) {
    present = true;
    strlcpy(vendor, jedec_vendor(rx[1]), sizeof(vendor));
    uint8_t cap = rx[3];
    if (cap >= 0x10 && cap <= 0x22)
      snprintf(product, sizeof(product), "SPI flash %lu Mbit (JEDEC %02X %02X %02X)", (1UL << cap) * 8 / 1048576UL, rx[1],
               rx[2], rx[3]);
    else
      snprintf(product, sizeof(product), "JEDEC ID %02X %02X %02X", rx[1], rx[2], rx[3]);
  } else {
    struct Probe { uint8_t reg; uint8_t id; const char *name; const char *vendor; } probes[] = {
        {0xD0, 0x60, "BME280", "Bosch"},       {0xD0, 0x58, "BMP280", "Bosch"},   {0xD0, 0x61, "BME680", "Bosch"},
        {0x75, 0x70, "MPU-6500", "InvenSense"}, {0x75, 0x71, "MPU-9250", "InvenSense"}, {0x75, 0x68, "MPU-6000", "InvenSense"},
        {0x0F, 0x33, "LIS3DH", "ST"},          {0x0F, 0x6A, "LSM6DS3 / LSM6DSO", "ST"}, {0x0F, 0x69, "LSM6DS3", "ST"},
        {0x00, 0xE5, "ADXL345", "Analog Devices"},
    };
    uint8_t lastReg = 0xFF, val = 0;
    for (auto &p : probes) {
      if (p.reg != lastReg) {
        uint8_t t[2] = {(uint8_t)(p.reg | 0x80), 0}, r[2];
        spi_xfer(cs, t, r, 2);
        val = r[1];
        lastReg = p.reg;
        if (val != 0x00 && val != 0xFF) present = true;
      }
      if (val == p.id) {
        strlcpy(product, p.name, sizeof(product));
        strlcpy(vendor, p.vendor, sizeof(vendor));
        break;
      }
    }
    if (present && !product[0]) strlcpy(product, "SPI device (MISO active, not identified)", sizeof(product));
  }
  out["cs"] = cs;
  out["present"] = present;
  if (product[0]) out["product"] = product;
  if (vendor[0]) out["vendor"] = vendor;
  if (!present) return;
  Device *d = dev_acquire(BUS_SPI, PROTO_SPI, cs, true);
  if (d) {
    strlcpy(d->product, product, sizeof(d->product));
    strlcpy(d->vendor, vendor, sizeof(d->vendor));
    d->present = true;
    d->passive = false;
    d->lastSeenMs = uptime_ms();
    d->rx++;
    d->consecErr = 0;
    dev_release(d, true, true);
  }
}

static void job_spi_scan(Job *j) {
  if (!spi_select()) return reply_err(j->rt, "SPI is disabled");
  busy = "scan";
  JsonDocument res;
  JsonArray found = res["found"].to<JsonArray>();
  JsonArray probed = res["probed"].to<JsonArray>();
  const SpiSettings &s = g_settings.spi;
  int n = 0;
  for (int i = 0; i < s.nCs; i++) {
    scan_event(BUS_SPI, "running", s.cs[i], s.cs[0], s.cs[s.nCs - 1], n);
    JsonDocument one;
    spi_identify(s.cs[i], one.to<JsonObject>());
    probed.add(s.cs[i]);
    if (one["present"]) {
      found.add(one.as<JsonObject>());
      n++;
    }
  }
  busy = "";
  scan_event(BUS_SPI, "done", 0, 0, 0, n);
  reply_ok(j->rt, res);
}

static void job_spi(Job *j) {
  JsonObjectConst a = j->args.as<JsonObjectConst>();
  if (!spi_select()) return reply_err(j->rt, "SPI is disabled");
  int cs = a["cs"] | (int)g_settings.spi.cs[0];
  if (!spi_cs_configured(cs)) return reply_err(j->rt, "CS IO%d is not configured (spi cs ...)", cs);
  const String &c = j->cmd;
  uint8_t tx[64] = {0}, rx[64];
  int n = 0;
  JsonDocument res;
  res["cs"] = cs;
  if (c == "spi.ident") {
    spi_identify(cs, res.to<JsonObject>());
    return reply_ok(j->rt, res);
  }
  if (c == "spi.xfer") {
    n = hex_decode(a["hex"] | "", tx, sizeof(tx));
    if (n <= 0) return reply_err(j->rt, "hex: 1..64 bytes required");
  } else {
    int reg = a["reg"] | -1;
    if (reg < 0 || reg > 0xFF) return reply_err(j->rt, "reg must be 0x00..0xFF");
    bool rb = a["readBit"] | g_settings.spi.readBit;
    if (c == "spi.read") {
      int cnt = a["count"] | 1;
      if (cnt < 1 || cnt > 63) return reply_err(j->rt, "count must be 1..63");
      tx[0] = rb ? (reg | 0x80) : reg;
      n = cnt + 1;
    } else {  // spi.write
      tx[0] = rb ? (reg & 0x7F) : reg;
      int m = hex_decode(a["hex"] | "", tx + 1, sizeof(tx) - 1);
      if (m <= 0) return reply_err(j->rt, "hex: 1..63 data bytes required");
      n = m + 1;
    }
    res["reg"] = reg;
  }
  spi_xfer(cs, tx, rx, n);
  res["tx"] = hex_string(tx, n);
  res["rx"] = hex_string(rx, n);
  if (c == "spi.read") res["data"] = hex_string(rx + 1, n - 1);
  // Raw transfers refresh a registered device but never create one (SPI has no ACK).
  Device *d = dev_acquire(BUS_SPI, PROTO_SPI, cs, false);
  if (d) {
    dev_release(d, false);
    dev_seen(BUS_SPI, PROTO_SPI, cs, false);
  }
  reply_ok(j->rt, res);
}

// ------------------------------------------------------------------ polling

static void do_poll(const PollTask &p) {
  bool ok = false;
  if (bus_is_i2c(p.bus)) {
    I2cDev d{p.bus, p.addr};
    if (p.driver[0]) {
      DevValue v[MAX_VALUES];
      int n = driver_read(p.driver, d, v, MAX_VALUES);
      if (n > 0) dev_set_values(p.bus, PROTO_I2C, p.addr, v, n, 0);
      else if (n < 0) {
        dev_set_values(p.bus, PROTO_I2C, p.addr, nullptr, 0, n);
        driver_reset(d);
      }
      ok |= n >= 0;
    }
    for (uint8_t i = 0; i < p.nWatch; i++) {
      const WatchItem &w = p.watch[i];
      uint8_t reg[2], r[16];
      int rl = 0;
      if (w.sub == 2) reg[rl++] = w.addr >> 8;
      reg[rl++] = w.addr & 0xFF;
      bool good = xi2c_write_read(d, reg, rl, r, w.count);
      dev_set_watch_value(p.bus, PROTO_I2C, p.addr, i, r, w.count, good ? 0 : -1, 0);
      ok |= good;
      if (uxQueueMessagesWaiting(jobQ)) break;
    }
    if (ok) dev_seen(p.bus, PROTO_I2C, p.addr, false);
    else dev_failed(p.bus, PROTO_I2C, p.addr);
  } else if (p.bus == BUS_SPI && spi_select() && spi_cs_configured(p.addr)) {
    for (uint8_t i = 0; i < p.nWatch; i++) {
      const WatchItem &w = p.watch[i];
      uint8_t tx[17] = {0}, rx[17];
      tx[0] = g_settings.spi.readBit ? (w.addr | 0x80) : w.addr;
      spi_xfer(p.addr, tx, rx, w.count + 1);
      dev_set_watch_value(BUS_SPI, PROTO_SPI, p.addr, i, rx + 1, w.count, 0, 0);
    }
    dev_seen(BUS_SPI, PROTO_SPI, p.addr, false);
  }
}

// ------------------------------------------------------------------ jobs

static void run_job(Job *j) {
  const String &c = j->cmd;
  if (c == "xbus.apply") {
    // Re-bind lazily with the new settings.
    i2c_release();
    spi_release();
    const char *b = j->args["bus"] | "qwiic";
    int bus = bus_from_name(b);
    JsonDocument res;
    xbus_status_json(bus < 0 ? BUS_QWIIC : bus, res.to<JsonObject>());
    reply_ok(j->rt, res);
    return state_changed();
  }
  if (c == "scan") {
    int bus = bus_from_name(j->args["bus"] | "");
    if (bus == BUS_SPI) return job_spi_scan(j);
    return job_i2c_scan(j, bus == BUS_I2C ? BUS_I2C : BUS_QWIIC);
  }
  if (c == "i2c.ident") return job_i2c_ident(j);
  if (c.startsWith("i2c.")) return job_i2c_rw(j);
  if (c.startsWith("spi.")) return job_spi(j);
  reply_err(j->rt, "unknown expansion bus command %s", c.c_str());
}

static void task(void *) {
  for (;;) {
    Job *j;
    if (xQueueReceive(jobQ, &j, pdMS_TO_TICKS(20)) == pdTRUE) {
      run_job(j);
      delete j;
      continue;
    }
    uint32_t now = uptime_ms();
    PollTask p;
    const uint8_t buses[] = {BUS_QWIIC, BUS_I2C, BUS_SPI};
    for (uint8_t b : buses) {
      bool en = b == BUS_SPI ? g_settings.spi.enabled : i2c_cfg(b).enabled;
      if (en && dev_next_poll(b, now, p)) {
        busy = "poll";
        do_poll(p);
        busy = "";
      }
    }
    if (now - lastAutoScan > 5000) {
      lastAutoScan = now;
      if (g_settings.qwiic.enabled && g_settings.qwiic.autoScan) auto_scan(BUS_QWIIC);
      if (g_settings.i2c.enabled && g_settings.i2c.autoScan) auto_scan(BUS_I2C);
    }
  }
}

void xbus_begin() {
  jobQ = xQueueCreate(16, sizeof(Job *));
  xTaskCreatePinnedToCore(task, "xbus", 8192, nullptr, 4, nullptr, 1);
}

bool xbus_submit(Job *job) {
  if (xQueueSend(jobQ, &job, 0) != pdTRUE) {
    reply_err(job->rt, "expansion bus queue full - try again");
    delete job;
    return false;
  }
  return true;
}

void xbus_cancel() { cancelReq = true; }

void xbus_status_json(uint8_t bus, JsonObject o) {
  if (bus == BUS_SPI) {
    spi_settings_to_json(g_settings.spi, o);
    o["up"] = spiUp || g_settings.spi.enabled;
    o["sck"] = pins::SPI_SCK;
    o["mosi"] = pins::SPI_MOSI;
    o["miso"] = pins::SPI_MISO;
  } else {
    i2c_settings_to_json(i2c_cfg(bus), o);
    o["up"] = i2c_cfg(bus).enabled;
    o["sda"] = bus == BUS_QWIIC ? pins::QWIIC_SDA : pins::HDR_SDA;
    o["scl"] = bus == BUS_QWIIC ? pins::QWIIC_SCL : pins::HDR_SCL;
  }
  o["busy"] = (const char *)busy;
  o["rx"] = g_counters[bus].rx;
  o["tx"] = g_counters[bus].tx;
  o["err"] = g_counters[bus].err;
}
