#include "cli.h"

#include <time.h>

#include "can.h"
#include "rpc.h"
#include "settings.h"
#include "web.h"

// ======================================================================
// Tokenizer / argument helpers
// ======================================================================

struct Args {
  static constexpr int MAX = 40;
  char buf[512];
  char *v[MAX];
  int n = 0;
  const char *operator[](int i) const { return i < n ? v[i] : ""; }
  bool is(int i, const char *s) const { return i < n && !strcasecmp(v[i], s); }
};

static void tokenize(const char *line, Args &a) {
  strlcpy(a.buf, line, sizeof(a.buf));
  a.n = 0;
  char *p = a.buf;
  while (*p && a.n < Args::MAX) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '"') {
      p++;
      a.v[a.n++] = p;
      while (*p && *p != '"') p++;
    } else {
      a.v[a.n++] = p;
      while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (*p) *p++ = 0;
  }
}

// Integer in decimal or 0x-hex.
static bool num(const char *s, long &out) {
  if (!s || !*s) return false;
  char *end;
  out = strtol(s, &end, 0);
  return *end == 0;
}

// Integer that is hex by convention (CAN IDs, CANopen indices); 0x optional.
static bool hexnum(const char *s, long &out) {
  if (!s || !*s) return false;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  char *end;
  out = strtol(s, &end, 16);
  return *end == 0;
}

// "1-32" or "17"
static bool range(const char *s, long &from, long &to) {
  const char *dash = strchr(s, '-');
  if (!dash) {
    if (!num(s, from)) return false;
    to = from;
    return true;
  }
  char a[16], b[16];
  size_t la = dash - s;
  if (la == 0 || la >= sizeof(a)) return false;
  memcpy(a, s, la);
  a[la] = 0;
  strlcpy(b, dash + 1, sizeof(b));
  return num(a, from) && num(b, to) && from <= to;
}

// Device key: full "rs485:modbus:17", or short "mb17", "mb:17", "co5", "co:5", "j1939:0", "j0".
static bool dev_key_arg(const char *s, String &key) {
  if (!s || !*s) return false;
  int colons = 0;
  for (const char *p = s; *p; p++) colons += *p == ':';
  if (colons == 2) {
    key = s;
    return true;
  }
  String t(s);
  t.toLowerCase();
  String proto, addr;
  int c = t.indexOf(':');
  if (c >= 0) {
    proto = t.substring(0, c);
    addr = t.substring(c + 1);
  } else {
    int i = 0;
    while (i < (int)t.length() && isalpha((unsigned char)t[i])) i++;
    proto = t.substring(0, i);
    addr = t.substring(i);
  }
  long a;
  if (!num(addr.c_str(), a)) return false;
  if (proto == "mb" || proto == "modbus" || proto == "m") key = "rs485:modbus:";
  else if (proto == "co" || proto == "canopen" || proto == "c") key = "can:canopen:";
  else if (proto == "j" || proto == "j1939") key = "can:j1939:";
  else if (proto == "qw" || proto == "qwiic" || proto == "q") key = "qwiic:i2c:";
  else if (proto == "i2c" || proto == "hdr" || proto == "header") key = "i2c:i2c:";
  else if (proto == "spi" || proto == "cs") key = "spi:spi:";
  else return false;
  key += a;
  return true;
}

// ======================================================================
// Command table
// ======================================================================

struct Ctx {
  uint32_t client;
  int32_t id;
  JsonDocument req;
  char render[24] = "";
  String out;  // immediate text output (local commands)
};

typedef bool (*CmdFn)(Args &a, Ctx &c);  // return true to dispatch c.req

struct CliCmd {
  const char *name;
  const char *usage;
  const char *summary;
  const char *help;
  CmdFn fn;
};

static bool usage_err(Ctx &c, const char *msg);
extern const CliCmd CMDS[];
extern const size_t NCMDS;

#define REQ(c, name)          \
  do {                        \
    (c).req["cmd"] = (name);  \
  } while (0)

static void render_as(Ctx &c, const char *r) { strlcpy(c.render, r, sizeof(c.render)); }

// ---------------------------------------------------------------- help

static String pad(const String &s, size_t w) {
  String r = s;
  while (r.length() < w) r += ' ';
  return r;
}

static bool cmd_help(Args &a, Ctx &c) {
  if (a.n > 1) {
    for (size_t i = 0; i < NCMDS; i++) {
      if (!strcasecmp(CMDS[i].name, a[1])) {
        c.out = String("Usage: ") + CMDS[i].usage + "\n\n" + CMDS[i].summary + "\n";
        if (CMDS[i].help && *CMDS[i].help) c.out += String("\n") + CMDS[i].help;
        return false;
      }
    }
    c.out = String("No such command: ") + a[1] + "  (type 'help' for the list)";
    return false;
  }
  c.out =
      "WonderScope console. All dashboard functions are available as commands.\n"
      "'help <command>' shows usage and examples.\n\n";
  const char *groups[][2] = {
      {"General", "help status info clear"},
      {"Bus control", "rs485 can"},
      {"Discovery", "scan devices dev ids"},
      {"Modbus RTU (RS485)", "mb"},
      {"CANopen", "sdo nmt co"},
      {"J1939", "j1939"},
      {"I2C / SPI expansion", "i2c spi pins"},
      {"Raw frames", "cansend rs485send trace"},
      {"System", "wifi auth time reboot factory-reset"},
  };
  for (auto &g : groups) {
    c.out += String(g[0]) + ":\n";
    String names(g[1]);
    int start = 0;
    while (start < (int)names.length()) {
      int sp = names.indexOf(' ', start);
      if (sp < 0) sp = names.length();
      String nm = names.substring(start, sp);
      start = sp + 1;
      for (size_t i = 0; i < NCMDS; i++)
        if (nm == CMDS[i].name) c.out += "  " + pad(CMDS[i].name, 15) + CMDS[i].summary + "\n";
    }
    c.out += "\n";
  }
  c.out +=
      "Device keys: mb17 (Modbus addr 17), co5 (CANopen node 5), j0 (J1939 SA 0),\n"
      "             qwiic:0x76, i2c:0x44 (header I2C), spi:10 (CS GPIO),\n"
      "             or the full form rs485:modbus:17.\n"
      "Numbers: decimal or 0x-hex. CAN IDs and CANopen indices are always hex.";
  return false;
}

static bool cmd_clear(Args &, Ctx &c) {
  c.out = "\x1b[2J\x1b[H";
  return false;
}

static bool cmd_status(Args &, Ctx &c) {
  REQ(c, "status");
  return true;
}

static bool cmd_info(Args &, Ctx &c) {
  REQ(c, "hello");
  render_as(c, "info");
  return true;
}

// ---------------------------------------------------------------- bus control

static bool cmd_rs485(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.rs485");
    return true;
  }
  REQ(c, "rs485.config");
  for (int i = 1; i < a.n; i++) {
    long v;
    if (a.is(i, "on")) c.req["enabled"] = true;
    else if (a.is(i, "off")) c.req["enabled"] = false;
    else if (a.is(i, "baud") && num(a[i + 1], v)) c.req["baud"] = v, i++;
    else if (a.is(i, "parity") && i + 1 < a.n) c.req["parity"] = a[++i];
    else if (a.is(i, "stop") && num(a[i + 1], v)) c.req["stop"] = v, i++;
    else if (a.is(i, "timeout") && num(a[i + 1], v)) c.req["timeoutMs"] = v, i++;
    else if (a.is(i, "scantimeout") && num(a[i + 1], v)) c.req["scanTimeoutMs"] = v, i++;
    else if (num(a[i], v) && v >= 300) c.req["baud"] = v;  // "rs485 19200"
    else return usage_err(c, a[i]);
  }
  render_as(c, "rs485.config");
  return true;
}

static bool parse_bitrate(const char *s, long &bps) {
  String t(s);
  t.toLowerCase();
  float mult = 1;
  if (t.endsWith("k")) mult = 1000, t.remove(t.length() - 1);
  else if (t.endsWith("m")) mult = 1000000, t.remove(t.length() - 1);
  char *end;
  float f = strtof(t.c_str(), &end);
  if (*end) return false;
  bps = (long)(f * mult);
  if (bps <= 1000) bps *= 1000;  // "250" means 250k
  return true;
}

static bool cmd_can(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.can");
    return true;
  }
  if (a.is(1, "autobaud")) {
    REQ(c, "can.autobaud");
    c.out = "Detecting bitrate (listen-only)...";
    return true;
  }
  if (a.is(1, "selftest")) {
    REQ(c, "can.selftest");
    return true;
  }
  if (a.is(1, "recover")) {
    REQ(c, "can.recover");
    render_as(c, "ok");
    return true;
  }
  REQ(c, "can.config");
  for (int i = 1; i < a.n; i++) {
    long v;
    if (a.is(i, "on")) c.req["enabled"] = true;
    else if (a.is(i, "off")) c.req["enabled"] = false;
    else if (a.is(i, "listen")) c.req["mode"] = "listen";
    else if (a.is(i, "normal") || a.is(i, "active")) c.req["mode"] = "normal";
    else if (a.is(i, "mode") && i + 1 < a.n) {
      i++;
      c.req["mode"] = a.is(i, "active") ? "normal" : a[i];
    } else if (a.is(i, "bitrate") && parse_bitrate(a[i + 1], v)) c.req["bitrate"] = v, i++;
    else if (a.is(i, "sa") && num(a[i + 1], v)) c.req["j1939Sa"] = v, i++;
    else if (a.is(i, "sdotimeout") && num(a[i + 1], v)) c.req["sdoTimeoutMs"] = v, i++;
    else if (parse_bitrate(a[i], v)) c.req["bitrate"] = v;  // "can 500k"
    else return usage_err(c, a[i]);
  }
  render_as(c, "can.config");
  return true;
}

// ---------------------------------------------------------------- discovery

static bool cmd_scan(Args &a, Ctx &c) {
  if (a.n < 2) return usage_err(c, nullptr);
  if (a.is(1, "stop") || a.is(1, "cancel")) {
    REQ(c, "scan.cancel");
    render_as(c, "ok");
    return true;
  }
  REQ(c, "scan");
  long from, to, v;
  if (a.is(1, "rs485") || a.is(1, "modbus")) {
    c.req["bus"] = "rs485";
    JsonArray links;
    const char *parity = nullptr;
    for (int i = 2; i < a.n; i++) {
      if (a.is(i, "thorough")) c.req["thorough"] = true;
      else if (a.is(i, "parity") && i + 1 < a.n) parity = a[++i];
      else if (a.is(i, "timeout") && num(a[i + 1], v)) c.req["timeoutMs"] = v, i++;
      else if (a.is(i, "baud") && i + 1 < a.n) {
        links = c.req["links"].to<JsonArray>();
        String list(a[++i]);
        int s = 0;
        while (s < (int)list.length()) {
          int e = list.indexOf(',', s);
          if (e < 0) e = list.length();
          long b;
          if (!num(list.substring(s, e).c_str(), b)) return usage_err(c, a[i]);
          links.add<JsonObject>()["baud"] = b;
          s = e + 1;
        }
      } else if (range(a[i], from, to)) c.req["from"] = from, c.req["to"] = to;
      else return usage_err(c, a[i]);
    }
    if (parity) {
      if (links.isNull()) {
        links = c.req["links"].to<JsonArray>();
        links.add<JsonObject>()["baud"] = g_settings.rs485.baud;
      }
      for (JsonObject l : links) l["parity"] = parity;
    }
    long f = c.req["from"] | 1, t = c.req["to"] | 247;
    size_t passes = links.isNull() ? 1 : links.size();
    uint32_t tmo = c.req["timeoutMs"] | g_settings.rs485.scanTimeoutMs;
    c.out = String("Scanning Modbus addresses ") + f + ".." + t + " (" + passes + " link setting" +
            (passes > 1 ? "s" : "") + ", up to ~" + (unsigned)((t - f + 1) * passes * (tmo + 10) / 1000 + 1) +
            " s). 'scan stop' to abort.";
  } else if (a.is(1, "canopen") || a.is(1, "co")) {
    c.req["bus"] = "can";
    c.req["proto"] = "canopen";
    for (int i = 2; i < a.n; i++) {
      if (a.is(i, "quick")) c.req["identify"] = false;
      else if (a.is(i, "timeout") && num(a[i + 1], v)) c.req["timeoutMs"] = v, i++;
      else if (range(a[i], from, to)) c.req["from"] = from, c.req["to"] = to;
      else return usage_err(c, a[i]);
    }
    c.out = "Scanning CANopen nodes via SDO 0x1000... 'scan stop' to abort.";
  } else if (a.is(1, "j1939")) {
    c.req["bus"] = "can";
    c.req["proto"] = "j1939";
    if (a.is(2, "thorough")) c.req["thorough"] = true;
    c.out = "Requesting J1939 address claims...";
  } else {
    return usage_err(c, a[1]);
  }
  render_as(c, "scan");
  return true;
}

static bool cmd_devices(Args &a, Ctx &c) {
  REQ(c, "dev.list");
  if (a.n > 1) c.req["bus"] = a[1];
  render_as(c, "dev.list");
  return true;
}

static bool cmd_dev(Args &a, Ctx &c) {
  if (a.n < 2 || a.is(1, "list") || a.is(1, "ls")) {
    REQ(c, "dev.list");
    if (a.n > 2) c.req["bus"] = a[2];
    render_as(c, "dev.list");
    return true;
  }
  if (a.is(1, "clear")) {
    REQ(c, "dev.clear");
    if (a.n > 2) c.req["bus"] = a[2];
    render_as(c, "dev.clear");
    return true;
  }
  String key;
  // allow "dev mb17" as shorthand for "dev show mb17"
  int ki = 2;
  const char *sub = a[1];
  if (dev_key_arg(a[1], key)) {
    sub = "show";
    ki = 1;
  } else if (!dev_key_arg(a[2], key)) {
    return usage_err(c, a[2]);
  }
  c.req["key"] = key;
  if (!strcasecmp(sub, "show")) {
    REQ(c, "dev.get");
    render_as(c, "dev.get");
  } else if (!strcasecmp(sub, "rm") || !strcasecmp(sub, "remove") || !strcasecmp(sub, "del")) {
    REQ(c, "dev.remove");
    render_as(c, "ok");
  } else if (!strcasecmp(sub, "label")) {
    String label;
    for (int i = ki + 1; i < a.n; i++) label += (label.length() ? " " : "") + String(a[i]);
    REQ(c, "dev.update");
    c.req["label"] = label;
    render_as(c, "dev.get");
  } else if (!strcasecmp(sub, "poll")) {
    long ms = 0;
    if (!a.is(ki + 1, "off") && !num(a[ki + 1], ms)) return usage_err(c, a[ki + 1]);
    REQ(c, "dev.update");
    c.req["pollMs"] = ms;
    render_as(c, "dev.get");
  } else if (!strcasecmp(sub, "add")) {
    REQ(c, "dev.add");
    render_as(c, "dev.get");
  } else if (!strcasecmp(sub, "driver")) {
    if (a.n <= ki + 1) return usage_err(c, nullptr);
    REQ(c, "dev.update");
    c.req["driver"] = a.is(ki + 1, "none") ? "" : a[ki + 1];
    render_as(c, "dev.get");
  } else {
    return usage_err(c, sub);
  }
  return true;
}

// ---------------------------------------------------------------- I2C / SPI

// "qwiic" (default) or "header"/"i2c"
static const char *i2c_bus_word(const char *s, bool &matched) {
  matched = true;
  if (!strcasecmp(s, "qwiic") || !strcasecmp(s, "qw")) return "qwiic";
  if (!strcasecmp(s, "header") || !strcasecmp(s, "hdr") || !strcasecmp(s, "i2c")) return "i2c";
  matched = false;
  return "qwiic";
}

static bool cmd_i2c(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.i2c");
    return true;
  }
  bool m;
  if (a.is(1, "scan")) {
    REQ(c, "scan");
    const char *b = a.n > 2 ? i2c_bus_word(a[2], m) : "qwiic";
    c.req["bus"] = b;
    c.out = String("Scanning ") + (strcmp(b, "qwiic") ? "header I2C (IO8/IO9)" : "Qwiic I2C (IO2/IO1)") + " 0x08-0x77...";
    render_as(c, "i2c.scan");
    return true;
  }
  const char *bus = i2c_bus_word(a[1], m);
  if (m && a.n > 2 && (a.is(2, "on") || a.is(2, "off") || a.is(2, "autoscan") || isdigit((unsigned char)a[2][0]))) {
    REQ(c, strcmp(bus, "qwiic") ? "i2c.config" : "qwiic.config");
    for (int i = 2; i < a.n; i++) {
      if (a.is(i, "on")) c.req["enabled"] = true;
      else if (a.is(i, "off")) c.req["enabled"] = false;
      else if (a.is(i, "autoscan") && i + 1 < a.n) c.req["autoScan"] = a.is(++i, "on");
      else c.req["hz"] = a[i];
    }
    render_as(c, "show.i2cbus");
    return true;
  }
  // i2c read|write|xfer|ident <bus> <addr> ...
  const char *op = a[1];
  int bi = 2;
  bus = i2c_bus_word(a[bi], m);
  if (!m) return usage_err(c, a[bi]);
  long addr, reg, n;
  if (!num(a[bi + 1], addr)) return usage_err(c, a[bi + 1]);
  c.req["bus"] = bus;
  c.req["addr"] = addr;
  if (!strcasecmp(op, "ident") || !strcasecmp(op, "probe")) {
    REQ(c, "i2c.ident");
    render_as(c, "i2c.ident");
  } else if (!strcasecmp(op, "read") || !strcasecmp(op, "r")) {
    REQ(c, "i2c.read");
    if (a.n > bi + 2) {
      if (!num(a[bi + 2], reg)) return usage_err(c, a[bi + 2]);
      c.req["reg"] = reg;
      if (reg > 0xFF) c.req["regBytes"] = 2;
    }
    if (a.n > bi + 3) {
      if (!num(a[bi + 3], n)) return usage_err(c, a[bi + 3]);
      c.req["count"] = n;
    }
    render_as(c, "i2c.rw");
  } else if (!strcasecmp(op, "write") || !strcasecmp(op, "w")) {
    if (a.n < bi + 4 || !num(a[bi + 2], reg)) return usage_err(c, nullptr);
    REQ(c, "i2c.write");
    c.req["reg"] = reg;
    if (reg > 0xFF) c.req["regBytes"] = 2;
    String hex;
    for (int i = bi + 3; i < a.n; i++) hex += String(a[i]) + " ";
    c.req["hex"] = hex;
    render_as(c, "i2c.rw");
  } else if (!strcasecmp(op, "xfer")) {
    REQ(c, "i2c.xfer");
    String hex;
    for (int i = bi + 2; i < a.n; i++) {
      if (a.is(i, "r") && num(a[i + 1], n)) {
        c.req["count"] = n;
        i++;
      } else hex += String(a[i]) + " ";
    }
    c.req["hex"] = hex;
    render_as(c, "i2c.rw");
  } else {
    return usage_err(c, op);
  }
  return true;
}

static bool cmd_spi(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.spi");
    return true;
  }
  long cs, reg, n;
  if (a.is(1, "scan")) {
    REQ(c, "scan");
    c.req["bus"] = "spi";
    render_as(c, "spi.scan");
    return true;
  }
  if (a.is(1, "xfer") || a.is(1, "read") || a.is(1, "write") || a.is(1, "ident")) {
    if (!num(a[2], cs)) return usage_err(c, a[2]);
    c.req["cs"] = cs;
    if (a.is(1, "ident")) {
      REQ(c, "spi.ident");
      render_as(c, "spi.ident");
      return true;
    }
    if (a.is(1, "xfer")) {
      REQ(c, "spi.xfer");
      String hex;
      for (int i = 3; i < a.n; i++) hex += String(a[i]) + " ";
      c.req["hex"] = hex;
    } else {
      if (!num(a[3], reg)) return usage_err(c, a[3]);
      c.req["reg"] = reg;
      if (a.is(1, "read")) {
        REQ(c, "spi.read");
        if (a.n > 4) {
          if (!num(a[4], n)) return usage_err(c, a[4]);
          c.req["count"] = n;
        }
      } else {
        REQ(c, "spi.write");
        String hex;
        for (int i = 4; i < a.n; i++) hex += String(a[i]) + " ";
        if (!hex.length()) return usage_err(c, nullptr);
        c.req["hex"] = hex;
      }
    }
    render_as(c, "spi.rw");
    return true;
  }
  REQ(c, "spi.config");
  for (int i = 1; i < a.n; i++) {
    long v;
    if (a.is(i, "on")) c.req["enabled"] = true;
    else if (a.is(i, "off")) c.req["enabled"] = false;
    else if (a.is(i, "mode") && num(a[i + 1], v)) c.req["mode"] = v, i++;
    else if (a.is(i, "readbit") && i + 1 < a.n) c.req["readBit"] = a.is(++i, "on");
    else if (a.is(i, "cs") && i + 1 < a.n) {
      JsonArray arr = c.req["cs"].to<JsonArray>();
      String list(a[++i]);
      int s = 0;
      while (s < (int)list.length()) {
        int e = list.indexOf(',', s);
        if (e < 0) e = list.length();
        if (!num(list.substring(s, e).c_str(), v)) return usage_err(c, a[i]);
        arr.add(v);
        s = e + 1;
      }
    } else if (isdigit((unsigned char)a[i][0])) c.req["hz"] = a[i];
    else return usage_err(c, a[i]);
  }
  render_as(c, "show.spibus");
  return true;
}

static const char PIN_DIAGRAM[] =
    "Qwiic connector (SH1.0, beside USB-C)\n"
    "  GND   3V3   SDA = IO2   SCL = IO1\n"
    "\n"
    "Pin header, 2x10, 2.0 mm pitch (inside the case; power terminal end at top)\n"
    "\n"
    "                    3V3   o o   5V\n"
    "                    GND   o o   GND\n"
    "        UART0 TX   IO43   o o   IO20   USB D+  (do not use)\n"
    "        UART0 RX   IO44   o o   IO19   USB D-  (do not use)\n"
    "        SPI CS*     IO3   o o   IO14   SPI CS*\n"
    "        SPI CS*     IO4   o o   IO13   SPI MISO\n"
    "        SPI CS*     IO5   o o   IO12   SPI SCK\n"
    "        SPI CS*     IO6   o o   IO11   SPI MOSI\n"
    "        SPI CS*     IO7   o o   IO10   SPI CS (default)\n"
    "        I2C SDA     IO8   o o   IO9    I2C SCL\n"
    "\n"
    "  * optional additional chip selects ('spi cs 10,5')\n"
    "  Logic level 3.3 V, not 5 V tolerant. These pins are not isolated.\n"
    "  The 5V pin is a supply output. Fit 2.2-4.7 kOhm I2C pull-ups for long or fast buses.";

static bool cmd_pins(Args &, Ctx &c) {
  c.out = PIN_DIAGRAM;
  return false;
}

static bool cmd_ids(Args &a, Ctx &c) {
  if (a.is(1, "clear")) {
    REQ(c, "can.ids.clear");
    render_as(c, "ok");
    return true;
  }
  REQ(c, "can.ids");
  render_as(c, "can.ids");
  return true;
}

// ---------------------------------------------------------------- Modbus

static int mb_table(const char *s, bool forWrite) {
  String t(s);
  t.toLowerCase();
  if (t == "hr" || t == "holding" || t == "h" || t == "4x") return forWrite ? 6 : 3;
  if (t == "ir" || t == "input" || t == "i" || t == "3x") return forWrite ? -1 : 4;
  if (t == "co" || t == "coil" || t == "coils" || t == "c" || t == "0x") return forWrite ? 5 : 1;
  if (t == "di" || t == "discrete" || t == "d" || t == "1x") return forWrite ? -1 : 2;
  long fn;
  if (num(s, fn) && fn >= 1 && fn <= 16) return fn;
  return -1;
}

static bool cmd_mb(Args &a, Ctx &c) {
  long addr, start, v;
  if (a.is(1, "read") || a.is(1, "r")) {
    int fn = mb_table(a[3], false);
    if (!num(a[2], addr) || fn < 1 || fn > 4 || !num(a[4], start)) return usage_err(c, nullptr);
    long count = 1;
    if (a.n > 5 && !num(a[5], count)) return usage_err(c, a[5]);
    REQ(c, "mb.read");
    c.req["addr"] = addr;
    c.req["fn"] = fn;
    c.req["start"] = start;
    c.req["count"] = count;
    render_as(c, "mb.read");
    return true;
  }
  if (a.is(1, "write") || a.is(1, "w")) {
    int fn = mb_table(a[3], true);
    if (!num(a[2], addr) || fn < 0 || !num(a[4], start) || a.n < 6) return usage_err(c, nullptr);
    REQ(c, "mb.write");
    c.req["addr"] = addr;
    c.req["fn"] = fn;
    c.req["start"] = start;
    JsonArray vals = c.req["values"].to<JsonArray>();
    for (int i = 5; i < a.n; i++) {
      if (a.is(i, "on") || a.is(i, "true")) v = 1;
      else if (a.is(i, "off") || a.is(i, "false")) v = 0;
      else if (!num(a[i], v)) return usage_err(c, a[i]);
      vals.add(v);
    }
    render_as(c, "mb.write");
    return true;
  }
  if (a.is(1, "ident") || a.is(1, "id") || a.is(1, "probe")) {
    if (!num(a[2], addr)) return usage_err(c, nullptr);
    REQ(c, "mb.ident");
    c.req["addr"] = addr;
    render_as(c, "mb.ident");
    return true;
  }
  if (a.is(1, "raw")) {
    String hex;
    REQ(c, "mb.raw");
    for (int i = 2; i < a.n; i++) {
      if (a.is(i, "nocrc")) c.req["crc"] = false;
      else if (a.is(i, "noreply")) c.req["expect"] = false;
      else hex += String(a[i]) + " ";
    }
    if (!hex.length()) return usage_err(c, nullptr);
    c.req["hex"] = hex;
    render_as(c, "mb.raw");
    return true;
  }
  return usage_err(c, a[1]);
}

static bool cmd_rs485send(Args &a, Ctx &c) {
  String hex;
  REQ(c, "mb.raw");
  c.req["crc"] = false;
  for (int i = 1; i < a.n; i++) {
    if (a.is(i, "crc")) c.req["crc"] = true;
    else if (a.is(i, "noreply")) c.req["expect"] = false;
    else hex += String(a[i]) + " ";
  }
  if (!hex.length()) return usage_err(c, nullptr);
  c.req["hex"] = hex;
  render_as(c, "mb.raw");
  return true;
}

// ---------------------------------------------------------------- CANopen

// "1018:1", "1018sub1", "0x1018:0x01", "1018"
static bool od_addr(const char *s, long &idx, long &sub) {
  char tmp[24];
  strlcpy(tmp, s, sizeof(tmp));
  char *colon = strchr(tmp, ':');
  if (!colon) colon = strchr(tmp, '.');
  sub = 0;
  if (colon) {
    *colon = 0;
    if (!num(colon + 1, sub)) return false;
  }
  return hexnum(tmp, idx) && idx >= 0 && idx <= 0xFFFF && sub >= 0 && sub <= 255;
}

static bool cmd_sdo(Args &a, Ctx &c) {
  long node, idx, sub, v;
  if (!num(a[2], node) || !od_addr(a[3], idx, sub)) return usage_err(c, nullptr);
  if (a.is(1, "read") || a.is(1, "r")) {
    REQ(c, "co.sdo.read");
  } else if (a.is(1, "write") || a.is(1, "w")) {
    if (a.n < 5) return usage_err(c, nullptr);
    REQ(c, "co.sdo.write");
    const char *val = a[4];
    String type = a.n > 5 ? String(a[5]) : String("u32");
    type.toLowerCase();
    if (!strncasecmp(val, "hex:", 4)) c.req["hex"] = val + 4;
    else {
      if (!num(val, v)) return usage_err(c, val);
      c.req["value"] = v;
      c.req["size"] = (type == "u8" || type == "i8" || type == "s8")    ? 1
                      : (type == "u16" || type == "i16" || type == "s16") ? 2
                                                                          : 4;
    }
  } else {
    return usage_err(c, a[1]);
  }
  c.req["node"] = node;
  c.req["index"] = idx;
  c.req["sub"] = sub;
  render_as(c, c.req["cmd"].as<const char *>());
  return true;
}

static bool cmd_nmt(Args &a, Ctx &c) {
  long node;
  if (a.is(1, "all")) node = 0;
  else if (!num(a[1], node)) return usage_err(c, nullptr);
  const char *cmd = a[2];
  const char *map[][2] = {{"start", "start"},      {"op", "start"},    {"operational", "start"}, {"stop", "stop"},
                          {"preop", "preop"},      {"pre-op", "preop"}, {"reset", "reset"},      {"resetnode", "reset"},
                          {"resetcomm", "resetcomm"}, {"reset-comm", "resetcomm"}};
  const char *m = nullptr;
  for (auto &e : map)
    if (!strcasecmp(cmd, e[0])) m = e[1];
  if (!m) return usage_err(c, cmd);
  REQ(c, "co.nmt");
  c.req["node"] = node;
  c.req["cmd"] = m;
  render_as(c, "co.nmt");
  return true;
}

static bool cmd_co(Args &a, Ctx &c) {
  long node;
  if ((a.is(1, "info") || a.is(1, "identify")) && num(a[2], node)) {
    REQ(c, "co.info");
    c.req["node"] = node;
    render_as(c, "co.info");
    return true;
  }
  return usage_err(c, a[1]);
}

// ---------------------------------------------------------------- J1939

static bool cmd_j1939(Args &a, Ctx &c) {
  long pgn, da = 255, prio = 6;
  if (a.is(1, "req") || a.is(1, "request")) {
    if (!num(a[2], pgn)) return usage_err(c, nullptr);
    if (a.n > 3 && !num(a[3], da)) return usage_err(c, a[3]);
    REQ(c, "j1939.request");
    c.req["pgn"] = pgn;
    c.req["da"] = da;
    render_as(c, "j1939.request");
    return true;
  }
  if (a.is(1, "send")) {
    if (!num(a[2], pgn) || a.n < 4) return usage_err(c, nullptr);
    if (a.n > 4 && !num(a[4], da)) return usage_err(c, a[4]);
    if (a.n > 5 && !num(a[5], prio)) return usage_err(c, a[5]);
    REQ(c, "j1939.send");
    c.req["pgn"] = pgn;
    c.req["hex"] = a[3];
    c.req["da"] = da;
    c.req["prio"] = prio;
    render_as(c, "ok");
    return true;
  }
  return usage_err(c, a[1]);
}

// ---------------------------------------------------------------- raw CAN

// can-utils syntax: 123#DEADBEEF, 12345678#00, 123#R, 123#R4
static bool cmd_cansend(Args &a, Ctx &c) {
  const char *f = a[1];
  const char *hash = strchr(f, '#');
  if (!hash) return usage_err(c, nullptr);
  char idstr[12];
  size_t il = hash - f;
  if (il == 0 || il > 8) return usage_err(c, f);
  memcpy(idstr, f, il);
  idstr[il] = 0;
  long id;
  if (!hexnum(idstr, id)) return usage_err(c, f);
  REQ(c, "can.send");
  c.req["id"] = id;
  c.req["ext"] = il > 3;
  const char *d = hash + 1;
  if (*d == 'R' || *d == 'r') {
    c.req["rtr"] = true;
    c.req["dlc"] = d[1] ? atoi(d + 1) : 0;
  } else {
    c.req["hex"] = d;
  }
  long v;
  if (a.n > 2) {
    if (!num(a[2], v)) return usage_err(c, a[2]);
    c.req["count"] = v;
  }
  if (a.n > 3) {
    if (!num(a[3], v)) return usage_err(c, a[3]);
    c.req["intervalMs"] = v;
  }
  render_as(c, "can.send");
  return true;
}

static bool cmd_trace(Args &a, Ctx &c) {
  REQ(c, "sub");
  JsonObject t = c.req["topics"].to<JsonObject>();
  bool on = true;
  int busSel = 0;
  for (int i = 1; i < a.n; i++) {
    if (a.is(i, "off") || a.is(i, "stop")) on = false;
    else if (a.is(i, "on")) on = true;
    else if (a.is(i, "rs485")) busSel = 1;
    else if (a.is(i, "can")) busSel = 2;
    else if (a.is(i, "i2c") || a.is(i, "qwiic")) busSel = 3;
    else if (a.is(i, "spi")) busSel = 4;
    else if (a.is(i, "all")) busSel = 0;
    else return usage_err(c, a[i]);
  }
  t["textTrace"] = on;
  t["textBus"] = busSel;
  static const char *const SEL[] = {" (all buses)", " (RS485)", " (CAN)", " (I2C)", " (SPI)"};
  c.out = on ? String("Live trace ON") + SEL[busSel] +
                   "; 'trace off' to stop."
             : "Live trace off.";
  render_as(c, "silent");
  return true;
}

// ---------------------------------------------------------------- system

static bool cmd_wifi(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.wifi");
    return true;
  }
  REQ(c, "wifi.config");
  if (a.is(1, "ap") && a.n >= 3) {
    c.req["apSsid"] = a[2];
    c.req["apPass"] = a.n > 3 ? a[3] : "";
  } else if (a.is(1, "sta") && a.is(2, "off")) {
    c.req["staSsid"] = "";
    c.req["staPass"] = "";
  } else if (a.is(1, "sta") && a.n >= 3) {
    c.req["staSsid"] = a[2];
    c.req["staPass"] = a.n > 3 ? a[3] : "";
  } else if (a.is(1, "hostname") && a.n == 3) {
    c.req["hostname"] = a[2];
  } else {
    return usage_err(c, a[1]);
  }
  render_as(c, "note");
  return true;
}

static bool cmd_auth(Args &a, Ctx &c) {
  REQ(c, "auth.config");
  if (a.is(1, "off")) c.req["pass"] = "";
  else if (a.n == 3) {
    c.req["user"] = a[1];
    c.req["pass"] = a[2];
  } else return usage_err(c, nullptr);
  render_as(c, "auth");
  return true;
}

static bool cmd_time(Args &a, Ctx &c) {
  if (a.n == 1) {
    REQ(c, "status");
    render_as(c, "show.time");
    return true;
  }
  long s;
  if (!num(a[1], s)) return usage_err(c, a[1]);
  REQ(c, "time.set");
  c.req["epoch"] = (int64_t)s * 1000;
  c.req["force"] = true;
  render_as(c, "show.timeset");
  return true;
}

static bool cmd_reboot(Args &, Ctx &c) {
  REQ(c, "sys.reboot");
  render_as(c, "note");
  return true;
}

static bool cmd_factory(Args &a, Ctx &c) {
  if (!a.is(1, "confirm")) {
    c.out = "Erases all settings (Wi-Fi, bus configuration, login) and the device list.\n"
            "Confirm with: factory-reset confirm";
    return false;
  }
  REQ(c, "sys.factory");
  c.req["confirm"] = true;
  render_as(c, "note");
  return true;
}

// ---------------------------------------------------------------- table

const CliCmd CMDS[] = {
    {"help", "help [command]", "List commands, or show details for one", "", cmd_help},
    {"clear", "clear", "Clear the screen", "", cmd_clear},
    {"status", "status", "Bus, Wi-Fi and system status", "", cmd_status},
    {"info", "info", "Firmware and board information", "", cmd_info},
    {"rs485", "rs485 [on|off] [baud <n>] [parity n|e|o] [stop 1|2] [timeout <ms>]",
     "Show or configure the RS485 port",
     "Without arguments shows the current settings and counters. Changes are saved.\n"
     "  timeout      Modbus response timeout in ms (default 200)\n"
     "  scantimeout  per-address timeout during scans (default 80)\n"
     "Examples:\n"
     "  rs485 on baud 19200 parity e\n"
     "  rs485 9600\n"
     "  rs485 off",
     cmd_rs485},
    {"can", "can [on|off] [bitrate <n>] [listen|normal] | can autobaud | can recover | can selftest",
     "Show or configure the CAN port",
     "Bitrates: 10k 20k 50k 100k 125k 250k 500k 800k 1M.\n"
     "Modes:\n"
     "  listen  listen-only (default): no transmission, no ACK\n"
     "  normal  active: required for scans, SDO, NMT, J1939 requests and cansend\n"
     "Subcommands:\n"
     "  autobaud  try each bitrate in listen-only mode and keep the one with clean traffic\n"
     "  recover   restart the controller (clears bus-off / error counters)\n"
     "  selftest  internal loopback test; TRANSMITS one frame (ID 7FF) on the bus\n"
     "Other settings: sa <0..253> (our J1939 source address), sdotimeout <ms>\n"
     "Examples:\n"
     "  can 500k\n"
     "  can bitrate 250k normal\n"
     "  can autobaud",
     cmd_can},
    {"scan", "scan rs485|canopen|j1939 [<from>-<to>] [options] | scan stop", "Discover devices on a bus",
     "scan rs485 [<from>-<to>] [thorough] [baud <b1,b2,..>] [parity n|e|o] [timeout <ms>]\n"
     "  Probes each Modbus address (FC03 reg 0; 'thorough' also tries FC04/FC01).\n"
     "  Any reply, including an exception, registers a device. Identification:\n"
     "  FC2B/0E (vendor, product, revision) and FC11.\n"
     "  'baud 9600,19200,38400' sweeps several baud rates.\n"
     "scan canopen [<from>-<to>] [quick]\n"
     "  Reads object 0x1000 from each node ID via SDO, then identity objects\n"
     "  (0x1008, 0x1009, 0x100A, 0x1018) unless 'quick'. Needs CAN normal mode.\n"
     "scan j1939 [thorough]\n"
     "  Sends Request for Address Claimed to all; 'thorough' also requests\n"
     "  Component ID (65259) and Software ID (65242). Needs CAN normal mode.\n"
     "Passive discovery runs continuously (Modbus request/response pairs, CANopen\n"
     "heartbeat, J1939 traffic) without transmitting.\n"
     "Examples:\n"
     "  scan rs485 1-32\n"
     "  scan rs485 1-247 baud 9600,19200 parity e\n"
     "  scan canopen\n"
     "  scan stop",
     cmd_scan},
    {"devices", "devices [rs485|can]", "List discovered devices", "Alias: dev list", cmd_devices},
    {"dev", "dev show|label|poll|driver|rm|add <key> [...] | dev clear [bus]", "Inspect or edit one device",
     "dev show <key>             details, identity, decoded values and watch values\n"
     "dev label <key> <text>     give the device a name\n"
     "dev poll <key> <ms|off>    poll its driver / watch list\n"
     "dev driver <key> <name|none>  set the I2C decoding driver (see 'help i2c')\n"
     "dev add <key>              add a device manually (e.g. not answering scans)\n"
     "dev rm <key>               forget a device\n"
     "dev clear [bus]            forget all devices (rs485|can|qwiic|i2c|spi)\n"
     "Keys: mb17, co5, j0, qwiic:0x76, i2c:0x44, spi:10 or rs485:modbus:17\n"
     "Examples:\n"
     "  dev show mb17\n"
     "  dev label co5 \"Left drive\"\n"
     "  dev driver qwiic:0x76 bme280",
     cmd_dev},
    {"i2c", "i2c | i2c scan [qwiic|header] | i2c <bus> on|off [100k|400k|1m] | i2c read|write|xfer|ident <bus> <addr> ...",
     "I2C on the Qwiic connector and the pin header",
     "Buses: qwiic  Qwiic / SH1.0 connector, SDA IO2, SCL IO1 (enabled by default)\n"
     "       header pin header, SDA IO8, SCL IO9 (wire internally, then 'i2c header on')\n"
     "i2c scan [qwiic|header]           probe 0x08-0x77 and identify devices\n"
     "i2c <bus> on|off [100k|400k|1m] [autoscan on|off]\n"
     "i2c ident <bus> <addr>            probe and identify one address\n"
     "i2c read <bus> <addr> [reg] [n]   read n bytes (from register reg)\n"
     "i2c write <bus> <addr> <reg> <byte...>\n"
     "i2c xfer <bus> <addr> <hex...> [r <n>]  write then read with repeated start\n"
     "Registers above 0xFF are sent as 16-bit addresses.\n"
     "Decoding drivers: bme280 bmp280 sht3x sht4x aht20 bh1750 tmp102 mcp9808 ina219\n"
     "                  mpu6050 mpu6500 scd4x. Assigned automatically when identified.\n"
     "Qwiic is probed every 5 s, so devices appear when plugged in.\n"
     "Examples:\n"
     "  i2c scan\n"
     "  i2c read qwiic 0x76 0xD0 1\n"
     "  i2c header on 400k",
     cmd_i2c},
    {"spi", "spi | spi on|off [hz] [mode 0-3] [cs 10,5] | spi scan | spi xfer|read|write|ident <cs> ...",
     "SPI on the pin header",
     "Pins: SCK IO12, MOSI IO11, MISO IO13, CS IO10 (default). Extra CS: IO3-IO8, IO14.\n"
     "spi on [1m] [mode 0] [cs 10,5]    enable and configure\n"
     "spi scan                          probe each CS for JEDEC flash and sensor IDs\n"
     "spi xfer <cs> <hex...>            full-duplex transfer, prints MISO\n"
     "spi read <cs> <reg> [n]           register read (bit 7 set unless 'readbit off')\n"
     "spi write <cs> <reg> <byte...>\n"
     "Examples:\n"
     "  spi on 4m cs 10\n"
     "  spi xfer 10 9F 00 00 00\n"
     "  spi read 10 0xD0",
     cmd_spi},
    {"pins", "pins", "Expansion pin diagram (Qwiic, pin header)", "", cmd_pins},
    {"ids", "ids [clear]", "Table of every CAN identifier seen (count, rate, last data)", "", cmd_ids},
    {"mb", "mb read|write|ident|raw ...", "Modbus RTU master requests",
     "mb read <addr> <table> <start> [count]\n"
     "    table: hr (holding, FC03), ir (input, FC04), co (coils, FC01), di (discrete, FC02)\n"
     "mb write <addr> <hr|co> <start> <value> [value...]\n"
     "    one value -> FC06/FC05, several -> FC16/FC15. addr 0 = broadcast\n"
     "mb ident <addr>     probe and read device identification\n"
     "mb raw <hex...> [nocrc] [noreply]\n"
     "    send arbitrary bytes; CRC is appended unless 'nocrc'\n"
     "Addresses/registers are 0-based protocol addresses (40001 = hr 0).\n"
     "Examples:\n"
     "  mb read 1 hr 0 10\n"
     "  mb write 1 hr 100 1500\n"
     "  mb write 1 co 3 on\n"
     "  mb raw 01 03 00 00 00 02",
     cmd_mb},
    {"rs485send", "rs485send <hex...> [crc] [noreply]", "Send raw bytes on RS485 (no CRC unless 'crc')",
     "Prints whatever comes back within the response timeout.\n"
     "Example: rs485send 01 03 00 00 00 01 84 0A",
     cmd_rs485send},
    {"sdo", "sdo read|write <node> <index>[:sub] [value] [u8|u16|u32]", "CANopen SDO read/write",
     "Index is hex, sub-index decimal or 0x-hex. Writes use expedited SDO (1..4 bytes);\n"
     "give the value as a number with a size, or raw bytes as hex:<bytes>.\n"
     "Examples:\n"
     "  sdo read 5 1018:1          vendor ID\n"
     "  sdo read 5 1008            device name (segmented transfer)\n"
     "  sdo write 5 1017 1000 u16  heartbeat every 1000 ms\n"
     "  sdo write 5 1010:1 hex:73617665   store parameters (\"save\")",
     cmd_sdo},
    {"nmt", "nmt <node|all> start|stop|preop|reset|resetcomm", "Send a CANopen NMT command",
     "Example: nmt all start    (puts every node in Operational)", cmd_nmt},
    {"co", "co info <node>", "Read a CANopen node's identity objects", "", cmd_co},
    {"j1939", "j1939 req <pgn> [da] | j1939 send <pgn> <hex> [da] [prio]", "J1939 requests and messages",
     "PGN in decimal or 0x-hex; da 255 (default) = global.\n"
     "Our source address is set with 'can sa <n>' (default 249).\n"
     "Examples:\n"
     "  j1939 req 65259 0     component ID from the engine ECU\n"
     "  j1939 req 65226       DM1 active faults from everyone\n"
     "  j1939 send 0xEF00 0102030405060708 0x21",
     cmd_j1939},
    {"cansend", "cansend <id>#<data> [count] [interval_ms]", "Send a raw CAN frame (can-utils syntax)",
     "IDs with more than 3 hex digits are sent as 29-bit extended frames.\n"
     "Use R for a remote frame, optionally with a DLC: 123#R or 123#R8.\n"
     "Needs CAN normal mode.\n"
     "Examples:\n"
     "  cansend 123#DEADBEEF\n"
     "  cansend 18FEF100#FFFFFF0A\n"
     "  cansend 000#0100           NMT start all\n"
     "  cansend 123#01 10 100      ten frames, 100 ms apart",
     cmd_cansend},
    {"trace", "trace [on|off] [rs485|can|all]", "Stream live bus frames to this console",
     "Prints each RX and TX frame. The dashboard Traffic tab adds protocol decoding.",
     cmd_trace},
    {"wifi", "wifi | wifi ap <ssid> [pass] | wifi sta <ssid> [pass] | wifi sta off | wifi hostname <name>",
     "Show or change Wi-Fi",
     "The access point is always on (default password 'wonderscope').\n"
     "Joining a network adds http://<hostname>.local\n"
     "Examples:\n"
     "  wifi sta MyShopWiFi s3cretpass\n"
     "  wifi ap WonderScope-Line3 newpassword",
     cmd_wifi},
    {"auth", "auth <user> <password> | auth off", "Require a login for the web dashboard",
     "Does not apply to the serial console.", cmd_auth},
    {"time", "time [<unix-seconds>]", "Show or set the clock",
     "Set by NTP (station mode) or the browser; retained by the RTC when a backup\n"
     "battery is fitted.",
     cmd_time},
    {"reboot", "reboot", "Restart the board", "", cmd_reboot},
    {"factory-reset", "factory-reset confirm", "Erase all settings and the device list", "", cmd_factory},
};
const size_t NCMDS = sizeof(CMDS) / sizeof(CMDS[0]);

static const CliCmd *current;

static bool usage_err(Ctx &c, const char *bad) {
  c.out = "";
  if (bad && *bad) c.out = String("Invalid argument '") + bad + "'.\n";
  c.out += String("Usage: ") + (current ? current->usage : "?") + "\n(see 'help " +
           (current ? current->name : "") + "')";
  return false;
}

// ======================================================================
// Execution
// ======================================================================

static int32_t nextSerialId = 1;

const char *cli_prompt() { return "wonderscope> "; }

void cli_exec(const char *line, uint32_t client, int32_t id) {
  Args a;
  tokenize(line, a);
  if (client == CLIENT_SERIAL && id < 0) id = nextSerialId++;
  if (a.n == 0) {
    if (client == CLIENT_SERIAL) out_text(client, "", id);
    else out_text(client, "", id);
    return;
  }
  // aliases
  const char *name = a[0];
  if (!strcasecmp(name, "?") || !strcasecmp(name, "h")) name = "help";
  else if (!strcasecmp(name, "ls")) name = "devices";
  else if (!strcasecmp(name, "factory")) name = "factory-reset";
  current = nullptr;
  for (size_t i = 0; i < NCMDS; i++)
    if (!strcasecmp(CMDS[i].name, name)) current = &CMDS[i];
  if (!current) {
    out_text(client, String("Unknown command '") + a[0] + "'. Type 'help' for the list.", id);
    return;
  }
  Ctx c;
  c.client = client;
  c.id = id;
  bool dispatch = current->fn(a, c);
  if (!dispatch) {
    out_text(client, c.out, id);
    return;
  }
  if (c.out.length()) out_text(client, c.out, -1);  // progress note; final reply follows
  ReplyTo rt;
  rt.client = client;
  rt.id = id;
  rt.mode = REPLY_TEXT;
  strlcpy(rt.cmd, c.render[0] ? c.render : (c.req["cmd"] | ""), sizeof(rt.cmd));
  rpc_dispatch(c.req, rt);
}

// ======================================================================
// Rendering results as text
// ======================================================================

static String fmt_age(uint32_t nowMs, uint32_t thenMs) {
  if (!thenMs) return "never";
  uint32_t s = (nowMs - thenMs) / 1000;
  if (s < 60) return String(s) + "s ago";
  if (s < 3600) return String(s / 60) + "m ago";
  return String(s / 3600) + "h ago";
}

static const char *nmt_state(int s) {
  switch (s) {
    case 0: return "boot-up";
    case 4: return "stopped";
    case 5: return "operational";
    case 127: return "pre-operational";
  }
  return "?";
}

static const char *sdo_abort_text(uint32_t code) {
  switch (code) {
    case 0x05030000: return "toggle bit not alternated";
    case 0x05040000: return "SDO protocol timed out";
    case 0x05040001: return "invalid command specifier";
    case 0x05040005: return "out of memory";
    case 0x06010000: return "unsupported access to object";
    case 0x06010001: return "attempt to read a write-only object";
    case 0x06010002: return "attempt to write a read-only object";
    case 0x06020000: return "object does not exist";
    case 0x06040041: return "object cannot be mapped to PDO";
    case 0x06040042: return "PDO length exceeded";
    case 0x06040043: return "general parameter incompatibility";
    case 0x06040047: return "general internal incompatibility";
    case 0x06060000: return "access failed due to hardware error";
    case 0x06070010: return "data type does not match (length)";
    case 0x06070012: return "data type does not match (too long)";
    case 0x06070013: return "data type does not match (too short)";
    case 0x06090011: return "sub-index does not exist";
    case 0x06090030: return "value range exceeded";
    case 0x06090031: return "value too high";
    case 0x06090032: return "value too low";
    case 0x08000000: return "general error";
    case 0x08000020: return "data cannot be transferred or stored";
    case 0x08000021: return "data cannot be stored (local control)";
    case 0x08000022: return "data cannot be stored (device state)";
    case 0x08000024: return "no data available";
  }
  return "unknown abort code";
}

static String status_rs485(JsonObjectConst r) {
  char b[256];
  snprintf(b, sizeof(b), "RS485  %s  %lu 8%s%d  timeout %dms  rx %lu  tx %lu  err %lu%s%s",
           r["enabled"] ? (r["up"] ? "ON " : "ERR") : "OFF", (unsigned long)(r["baud"] | 0),
           (const char *)(r["parity"] | "N"), (int)(r["stop"] | 1), (int)(r["timeoutMs"] | 0),
           (unsigned long)(r["rx"] | 0), (unsigned long)(r["tx"] | 0), (unsigned long)(r["err"] | 0),
           strlen(r["busy"] | "") ? "  busy:" : "", (const char *)(r["busy"] | ""));
  return b;
}

static String status_can(JsonObjectConst c) {
  char b[300];
  snprintf(b, sizeof(b), "CAN    %s  %lu kbit/s  %s  state %s  TEC %d REC %d  rx %lu  tx %lu  busErr %lu  ids %d%s%s",
           c["enabled"] ? (c["up"] ? "ON " : "ERR") : "OFF", (unsigned long)((c["bitrate"] | 0) / 1000),
           strcmp(c["mode"] | "", "listen") ? "ACTIVE" : "listen-only", (const char *)(c["state"] | "?"),
           (int)(c["tec"] | 0), (int)(c["rec"] | 0), (unsigned long)(c["rx"] | 0), (unsigned long)(c["tx"] | 0),
           (unsigned long)(c["busErrors"] | 0), (int)(c["ids"] | 0), strlen(c["busy"] | "") ? "  busy:" : "",
           (const char *)(c["busy"] | ""));
  return b;
}

static String hz_text(uint32_t hz) {
  if (hz >= 1000000 && hz % 1000000 == 0) return String(hz / 1000000) + "M";
  if (hz >= 1000) return String(hz / 1000) + "k";
  return String(hz);
}

static String status_i2c(const char *label, JsonObjectConst q) {
  char b[200];
  snprintf(b, sizeof(b), "%-6s %s  %s  SDA IO%d SCL IO%d  autoscan %s  rx %lu  tx %lu  nack %lu", label,
           q["enabled"] ? "ON " : "OFF", hz_text(q["hz"] | 0).c_str(), (int)(q["sda"] | 0), (int)(q["scl"] | 0),
           (q["autoScan"] | false) ? "on" : "off", (unsigned long)(q["rx"] | 0), (unsigned long)(q["tx"] | 0),
           (unsigned long)(q["err"] | 0));
  return b;
}

static String status_spi(JsonObjectConst s) {
  String cs;
  for (JsonVariantConst v : s["cs"].as<JsonArrayConst>()) cs += (cs.length() ? "," : "") + String("IO") + (int)v;
  char b[200];
  snprintf(b, sizeof(b), "SPI    %s  %s mode %d  SCK IO12 MOSI IO11 MISO IO13  CS %s  readbit %s", s["enabled"] ? "ON " : "OFF",
           hz_text(s["hz"] | 0).c_str(), (int)(s["mode"] | 0), cs.c_str(), (s["readBit"] | true) ? "on" : "off");
  return b;
}

static String status_wifi(JsonObjectConst w) {
  String s = String("Wi-Fi  AP \"") + (const char *)(w["ap"]["ssid"] | "") + "\" " + (const char *)(w["ap"]["ip"] | "") +
             " (" + (int)(w["ap"]["clients"] | 0) + " clients)";
  const char *sta = w["sta"]["ssid"] | "";
  if (*sta) {
    if (w["sta"]["connected"])
      s += String("\n       STA \"") + sta + "\" " + (const char *)(w["sta"]["ip"] | "") + "  RSSI " +
           (int)(w["sta"]["rssi"] | 0) + " dBm  http://" + (const char *)(w["hostname"] | "") + ".local/";
    else
      s += String("\n       STA \"") + sta + "\" not connected";
  }
  return s;
}

static String fmt_time(int64_t ms) {
  if (!ms) return "not set";
  time_t t = ms / 1000;
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[40];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", &tm);
  return b;
}

static String render_device(JsonObjectConst d, uint32_t now) {
  String s;
  char b[200];
  snprintf(b, sizeof(b), "%s  %s\n", (const char *)(d["key"] | ""), (const char *)(d["label"] | ""));
  s += b;
  bool present = d["present"] | false;
  int consec = d["consecErr"] | 0;
  snprintf(b, sizeof(b), "  status     %s%s, last seen %s, rx %lu, errors %lu\n",
           consec >= 3 ? "NOT RESPONDING" : present ? "online" : "not seen since boot",
           (d["passive"] | false) ? " (passive)" : "", fmt_age(now, d["lastSeen"] | 0).c_str(),
           (unsigned long)(d["rx"] | 0), (unsigned long)(d["errs"] | 0));
  s += b;
  if (!d["state"].isNull()) s += String("  NMT state  ") + nmt_state(d["state"]) + "\n";
  if (!d["baud"].isNull())
    s += String("  link       ") + (long)d["baud"] + " 8" + (const char *)(d["parity"] | "N") + (int)(d["stop"] | 1) + "\n";
  const char *fields[][2] = {{"vendor", "vendor"}, {"product", "product"}, {"revision", "revision"},
                             {"name", "name"},     {"serial", "serial"},   {"j1939Name", "J1939 NAME"}};
  for (auto &f : fields)
    if (d[f[0]].is<const char *>()) s += "  " + pad(f[1], 11) + (const char *)d[f[0]] + "\n";
  if (d["co"].is<JsonObjectConst>()) {
    JsonObjectConst co = d["co"];
    snprintf(b, sizeof(b), "  identity   vendor 0x%08lX product 0x%08lX rev 0x%08lX serial %lu\n",
             (unsigned long)(co["vendorId"] | 0), (unsigned long)(co["productCode"] | 0),
             (unsigned long)(co["revision"] | 0), (unsigned long)(co["serial"] | 0));
    s += b;
  }
  if (d["emcy"].is<JsonObjectConst>()) {
    snprintf(b, sizeof(b), "  EMCY       code 0x%04X reg 0x%02X (%lu total, %s)\n", (unsigned)(d["emcy"]["code"] | 0),
             (unsigned)(d["emcy"]["reg"] | 0), (unsigned long)(d["emcy"]["count"] | 0),
             fmt_age(now, d["emcy"]["ts"] | 0).c_str());
    s += b;
  }
  if (d["notes"].is<const char *>() && strlen(d["notes"])) s += String("  notes      ") + (const char *)d["notes"] + "\n";
  if (d["driver"].is<const char *>()) {
    s += String("  driver     ") + (const char *)d["driver"] + "  (poll " +
         ((int)(d["pollMs"] | 0) ? String((int)d["pollMs"]) + " ms" : String("off")) + ")" +
         ((int)(d["valsErr"] | 0) ? "  read error" : "") + "\n";
    for (JsonObjectConst v : d["values"].as<JsonArrayConst>()) {
      snprintf(b, sizeof(b), "    %-16s %10.3f %s\n", (const char *)(v["n"] | ""), (float)(v["v"] | 0.0f), (const char *)(v["u"] | ""));
      s += b;
    }
  }
  JsonArrayConst w = d["watch"];
  if (w.size()) {
    s += String("  watch list (poll ") + ((int)(d["pollMs"] | 0) ? String((int)d["pollMs"]) + " ms" : String("off")) + ")\n";
    for (JsonObjectConst it : w) {
      snprintf(b, sizeof(b), "    %-20s %s  raw %s%s\n", (const char *)(it["name"] | ""),
               (int)(it["fn"] | 0) ? (String("fn") + (int)it["fn"] + " @" + (int)it["addr"]).c_str()
                                   : (String("0x") + String((int)it["addr"], HEX) + ":" + (int)it["sub"]).c_str(),
               (const char *)(it["raw"] | "-"), (int)(it["err"] | 0) ? "  (error)" : "");
      s += b;
    }
  }
  JsonArrayConst p = d["pgns"];
  if (p.size()) {
    s += "  PGNs seen\n";
    for (JsonObjectConst e : p) {
      snprintf(b, sizeof(b), "    %6lu (0x%05lX)  x%-7lu %s\n", (unsigned long)(e["pgn"] | 0), (unsigned long)(e["pgn"] | 0),
               (unsigned long)(e["count"] | 0), (const char *)(e["data"] | ""));
      s += b;
    }
  }
  return s;
}

String cli_render(const char *cmd, JsonDocument &doc) {
  String r = cmd;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  char b[256];
  if (r == "silent") return "";
  if (r == "ok") return "OK";
  if (r == "note") return o["note"] | "OK";
  if (r == "status") {
    return String(FW_NAME " up ") + (uint32_t)((o["up"] | 0) / 1000) + "s  heap " + (uint32_t)(o["heap"] | 0) / 1024 +
           "k  psram " + (uint32_t)(o["psram"] | 0) / 1024 + "k  web clients " + (int)(o["clients"] | 0) +
           "  devices " + (int)(o["devices"] | 0) + "\n" + "Time   " + fmt_time(o["epoch"] | (int64_t)0) + "\n" +
           status_wifi(o["wifi"]) + "\n" + status_rs485(o["rs485"]) + "\n" + status_can(o["can"]) + "\n" +
           status_i2c("Qwiic", o["qwiic"]) + "\n" + status_i2c("I2C", o["i2c"]) + "\n" + status_spi(o["spi"]);
  }
  if (r == "show.rs485" || r == "rs485.config") {
    JsonObjectConst rs = r == "show.rs485" ? o["rs485"].as<JsonObjectConst>() : o;
    return status_rs485(rs);
  }
  if (r == "show.can" || r == "can.config") {
    JsonObjectConst c = r == "show.can" ? o["can"].as<JsonObjectConst>() : o;
    String s = status_can(c);
    if (!strcmp(c["mode"] | "", "listen"))
      s += "\n       (listen-only; 'can normal' enables transmission)";
    return s;
  }
  if (r == "show.wifi") return status_wifi(o["wifi"]);
  if (r == "show.i2c") return status_i2c("Qwiic", o["qwiic"]) + "\n" + status_i2c("I2C", o["i2c"]);
  if (r == "show.i2cbus") return status_i2c((int)(o["sda"] | 0) == pins::QWIIC_SDA ? "Qwiic" : "I2C", o);
  if (r == "show.spi") return status_spi(o["spi"]);
  if (r == "show.spibus") return status_spi(o);
  if (r == "i2c.scan" || r == "spi.scan") {
    JsonArrayConst f = o["found"];
    String s = String(o["cancelled"] | false ? "Scan cancelled. " : "Scan complete. ") + f.size() + " device" +
               (f.size() == 1 ? "" : "s") + " found.\n";
    for (JsonObjectConst d : f) {
      if (!d["addr"].isNull())
        snprintf(b, sizeof(b), "  0x%02X  %-36s %-18s %s\n", (int)d["addr"], (const char *)(d["product"] | "unknown"),
                 (const char *)(d["vendor"] | ""), d["driver"].is<const char *>() ? (String("driver ") + (const char *)d["driver"]).c_str() : "");
      else
        snprintf(b, sizeof(b), "  CS IO%-3d %s %s\n", (int)(d["cs"] | 0), (const char *)(d["product"] | ""), (const char *)(d["vendor"] | ""));
      s += b;
    }
    if (r == "spi.scan" && !f.size()) s += "  No response on any CS (MISO idle). Check wiring and mode.\n";
    return s;
  }
  if (r == "i2c.ident" || r == "spi.ident") {
    if (!(o["present"] | false)) return r == "i2c.ident" ? String("No ACK.") : String("No response (MISO idle).");
    String s = String(o["product"] | "present, not identified");
    if (o["vendor"].is<const char *>()) s += String("  (") + (const char *)o["vendor"] + ")";
    if (o["driver"].is<const char *>()) s += String("\ndriver ") + (const char *)o["driver"] + " assigned; values in 'dev show'";
    return s;
  }
  if (r == "i2c.rw" || r == "spi.rw") {
    String s;
    if (o["tx"].is<const char *>()) s += String(r == "spi.rw" ? "MOSI " : "W ") + (const char *)o["tx"] + "\n";
    if (o["rx"].is<const char *>()) s += String(r == "spi.rw" ? "MISO " : "R ") + (const char *)o["rx"] + "\n";
    if (!s.length()) s = "OK";
    return s;
  }
  if (r == "show.time" || r == "show.timeset")
    return String("Time   ") + fmt_time(r == "show.time" ? (o["epoch"] | (int64_t)0) : (o["epoch"] | (int64_t)0));
  if (r == "info") {
    return String(o["fw"] | "") + " " + (const char *)(o["version"] | "") + " (built " + (const char *)(o["build"] | "") +
           ")\n" + "Board  " + (const char *)(o["board"] | "") + "\nChip   " + (const char *)(o["chip"] | "") +
           ", flash " + (uint32_t)(o["flash"] | 0) / 1048576 + " MB, PSRAM " +
           (uint32_t)(o["psramSize"] | 0) / 1048576 + " MB\nMAC    " + (const char *)(o["mac"] | "");
  }
  if (r == "auth") return (o["enabled"] | false) ? String("Dashboard login enabled for user '") + (const char *)(o["user"] | "") + "'"
                                                 : String("Dashboard login disabled");
  if (r == "dev.clear") return String("Removed ") + (int)(o["removed"] | 0) + " devices";
  if (r == "scan") {
    JsonArrayConst f = o["found"];
    String s = String(o["cancelled"] | false ? "Scan cancelled. " : "Scan complete. ") + f.size() + " device" +
               (f.size() == 1 ? "" : "s") + " found.\n";
    for (JsonObjectConst d : f) {
      if (!d["addr"].isNull()) {
        snprintf(b, sizeof(b), "  mb%-4d %6lu 8%s1  %s %s %s %s\n", (int)d["addr"], (unsigned long)(d["baud"] | 0),
                 (const char *)(d["parity"] | "N"), (const char *)(d["vendor"] | ""), (const char *)(d["product"] | ""),
                 (const char *)(d["revision"] | ""), (const char *)(d["name"] | ""));
      } else if (!d["node"].isNull()) {
        snprintf(b, sizeof(b), "  co%-4d %s  vendor 0x%08lX product 0x%08lX  %s\n", (int)d["node"],
                 (const char *)(d["name"] | "(no name)"), (unsigned long)(d["vendorId"] | 0),
                 (unsigned long)(d["productCode"] | 0), (const char *)(d["swVersion"] | ""));
      } else {
        snprintf(b, sizeof(b), "  j%-5d NAME %s  %s %s\n", (int)(d["sa"] | 0), (const char *)(d["name"] | ""),
                 (const char *)(d["vendor"] | ""), (const char *)(d["product"] | ""));
      }
      s += b;
    }
    JsonArrayConst g = o["garbled"];
    if (g.size()) s += String("  ") + g.size() + " address(es) answered with CRC errors - check baud/parity/termination.\n";
    return s;
  }
  if (r == "dev.list") {
    JsonArrayConst ds = o["devices"];
    uint32_t now = o["now"] | 0;
    if (!ds.size()) return "No devices. Discover with: scan rs485 | scan canopen | scan j1939";
    String s = "KEY                LABEL                STATUS            LAST SEEN  IDENTITY\n";
    for (JsonObjectConst d : ds) {
      int consec = d["consecErr"] | 0;
      const char *st = consec >= 3 ? "not responding" : (d["present"] | false) ? ((d["passive"] | false) ? "online (passive)" : "online")
                                                                               : "offline";
      String ident = String((const char *)(d["vendor"] | "")) + " " + (const char *)(d["product"] | "") + " " +
                     (const char *)(d["name"] | "");
      ident.trim();
      snprintf(b, sizeof(b), "%-18s %-20.20s %-17s %-10s %s\n", (const char *)(d["key"] | ""),
               (const char *)(d["label"] | ""), st, fmt_age(now, d["lastSeen"] | 0).c_str(), ident.c_str());
      s += b;
    }
    return s;
  }
  if (r == "dev.get") return render_device(o, o["now"] | uptime_ms());
  if (r == "mb.read") {
    int fn = o["fn"] | 3, start = o["start"] | 0;
    JsonArrayConst v = o["values"];
    String s;
    if (fn <= 2) {
      s = fn == 1 ? "coil    value\n" : "input   value\n";
      int i = 0;
      for (JsonVariantConst x : v) {
        snprintf(b, sizeof(b), "%-7d %d\n", start + i++, (int)x);
        s += b;
      }
    } else {
      s = "reg     hex      unsigned  signed\n";
      int i = 0;
      for (JsonVariantConst x : v) {
        uint16_t u = x.as<uint16_t>();
        snprintf(b, sizeof(b), "%-7d 0x%04X   %-9u %d\n", start + i++, u, u, (int16_t)u);
        s += b;
      }
    }
    return s;
  }
  if (r == "mb.write") return String("OK: wrote ") + (int)(o["written"] | 0) + " value(s) with FC" + (int)(o["fn"] | 0);
  if (r == "mb.raw") {
    String s = String("TX ") + (const char *)(o["tx"] | "");
    if (o["rx"].is<const char *>())
      s += String("\nRX ") + (const char *)o["rx"] + ((o["crcOk"] | false) ? "  (CRC ok)" : "  (CRC bad / not Modbus)");
    else if (o["status"].is<const char *>())
      s += String("\n   ") + (const char *)o["status"];
    return s;
  }
  if (r == "mb.ident") {
    String s = String("Address ") + (int)(o["addr"] | 0) + ": " + ((o["present"] | false) ? "present" : "no response") +
               " (" + (const char *)(o["probe"] | "") + ")\n";
    const char *keys[] = {"vendor", "product", "revision", "name", "serverId"};
    for (const char *k : keys)
      if (o[k].is<const char *>()) s += "  " + pad(k, 10) + (const char *)o[k] + "\n";
    return s;
  }
  if (r == "co.sdo.read" || r == "co.sdo.write") {
    snprintf(b, sizeof(b), "node %d  0x%04X:%d  ", (int)(o["node"] | 0), (unsigned)(o["index"] | 0), (int)(o["sub"] | 0));
    String s(b);
    if (!o["abort"].isNull()) {
      uint32_t ab = o["abort"];
      snprintf(b, sizeof(b), "ABORT 0x%08lX: %s", (unsigned long)ab, sdo_abort_text(ab));
      return s + b;
    }
    if (r == "co.sdo.write") return s + "written OK";
    if (!o["value"].isNull()) {
      uint32_t v = o["value"];
      snprintf(b, sizeof(b), "= 0x%0*lX  (%lu)  [%d bytes]", 2 * (int)(o["size"] | 4), (unsigned long)v, (unsigned long)v,
               (int)(o["size"] | 0));
      return s + b;
    }
    return s + "= \"" + (const char *)(o["text"] | "") + "\"  [" + (int)(o["size"] | 0) + " bytes] " +
           (const char *)(o["hex"] | "");
  }
  if (r == "co.nmt") return String("NMT ") + (const char *)(o["cmd"] | "") + " sent to " +
                            ((int)(o["node"] | 0) ? String("node ") + (int)o["node"] : String("all nodes"));
  if (r == "co.info") {
    String s = String("Node ") + (int)(o["node"] | 0) + "\n";
    snprintf(b, sizeof(b), "  device type 0x%08lX\n", (unsigned long)(o["deviceType"] | 0));
    s += b;
    const char *keys[] = {"name", "hwVersion", "swVersion"};
    for (const char *k : keys)
      if (o[k].is<const char *>()) s += "  " + pad(k, 12) + (const char *)o[k] + "\n";
    if (!o["vendorId"].isNull()) {
      snprintf(b, sizeof(b), "  vendor 0x%08lX  product 0x%08lX  revision 0x%08lX  serial %lu\n",
               (unsigned long)o["vendorId"], (unsigned long)(o["productCode"] | 0), (unsigned long)(o["revision"] | 0),
               (unsigned long)(o["serial"] | 0));
      s += b;
    }
    return s;
  }
  if (r == "j1939.request") {
    JsonArrayConst rs = o["responses"];
    String s = String("PGN ") + (long)(o["pgn"] | 0) + ": " + rs.size() + " response(s)\n";
    for (JsonObjectConst x : rs) s += String("  SA ") + (int)x["sa"] + "  " + (const char *)(x["data"] | "") + "\n";
    return s;
  }
  if (r == "can.send") return String("Sent ") + (int)(o["sent"] | 0) + " frame(s)";
  if (r == "can.ids") {
    JsonArrayConst ids = o["ids"];
    if (!ids.size()) return "No CAN traffic received.";
    String s = "ID          DLC  COUNT     RATE/s  AGE      DATA\n";
    for (JsonArrayConst e : ids) {
      bool ext = (int)e[1];
      char idb[12];
      snprintf(idb, sizeof(idb), ext ? "%08lX" : "%03lX", (unsigned long)(uint32_t)e[0]);
      snprintf(b, sizeof(b), "%-11s %-4d %-9lu %-7.1f %-8s %s%s\n", idb, (int)e[2], (unsigned long)(uint32_t)e[3],
               (float)e[4], (String((uint32_t)e[5] / 1000) + "s").c_str(), (const char *)e[6], (int)e[7] ? " RTR" : "");
      s += b;
    }
    return s;
  }
  if (r == "can.autobaud") {
    String s;
    for (JsonObjectConst t : o["tried"].as<JsonArrayConst>()) {
      snprintf(b, sizeof(b), "  %7lu bit/s: %lu frames, %lu errors\n", (unsigned long)t["bitrate"],
               (unsigned long)(t["frames"] | 0), (unsigned long)(t["errors"] | 0));
      s += b;
    }
    if (o["detected"].isNull()) s += "No valid traffic at any bitrate. Check H/L/GND wiring and termination.";
    else s += String("Bitrate detected: ") + (unsigned long)o["detected"] / 1000 + " kbit/s (listen-only).";
    return s;
  }
  if (r == "can.selftest")
    return (o["pass"] | false) ? String("PASS - controller and transceiver loopback OK (") + (uint32_t)(o["roundTripUs"] | 0) + " us)"
                               : String("FAIL - frame not received back (") + (const char *)(o["sendResult"] | "") + ")";
  // default: pretty JSON
  String s;
  serializeJsonPretty(doc, s);
  return s;
}

String cli_banner() {
  return String("\n" FW_NAME " " FW_VERSION " - Waveshare ESP32-S3-RS485-CAN bus monitor\n") +
         "Dashboard: connect to Wi-Fi \"" + g_settings.wifi.apSsid + "\" and open http://192.168.4.1/\n" +
         "Type 'help' for console commands.\n";
}

String cli_trace_line(const TraceFrame &f) {
  char b[TRACE_MAX_DATA * 3 + 96];
  int n;
  int64_t ep = epoch_ms();
  if (ep) {
    int64_t frameMs = ep - (int64_t)((uptime_us() - f.tsUs) / 1000);
    time_t t = frameMs / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    n = snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(frameMs % 1000));
  } else {
    n = snprintf(b, sizeof(b), "%10.4f ", f.tsUs / 1e6);
  }
  if (f.bus == BUS_CAN) {
    n += snprintf(b + n, sizeof(b) - n, "CAN   %s  ", f.dir == DIR_TX ? "TX" : "RX");
    n += snprintf(b + n, sizeof(b) - n, (f.flags & TF_EXT) ? "%08lX" : "     %03lX", (unsigned long)f.id);
    if (f.flags & TF_RTR) n += snprintf(b + n, sizeof(b) - n, "  [RTR]");
    else n += snprintf(b + n, sizeof(b) - n, "  [%d] ", f.len);
  } else if (bus_is_i2c(f.bus)) {
    n += snprintf(b + n, sizeof(b) - n, "%-5s %s 0x%02lX%s", f.bus == BUS_QWIIC ? "QWIIC" : "I2C", f.dir == DIR_TX ? "W" : "R",
                  (unsigned long)f.id, (f.flags & TF_NACK) ? " NACK" : "");
  } else if (f.bus == BUS_SPI) {
    n += snprintf(b + n, sizeof(b) - n, "SPI   CS%-2lu %s", (unsigned long)f.id, f.dir == DIR_TX ? "MOSI" : "MISO");
  } else {
    n += snprintf(b + n, sizeof(b) - n, "RS485 %s  ", f.dir == DIR_TX ? "TX" : "RX");
  }
  for (int i = 0; i < f.len && n < (int)sizeof(b) - 40; i++) n += snprintf(b + n, sizeof(b) - n, " %02X", f.data[i]);
  if (f.bus == BUS_RS485) {
    n += snprintf(b + n, sizeof(b) - n, "%s%s", (f.flags & TF_CRC_OK) ? "   crc ok" : "   (no valid CRC)",
                  (f.flags & TF_ERR) ? " [uart error]" : "");
  }
  return b;
}

// ======================================================================
// USB serial input
// ======================================================================

void cli_serial_loop() {
  static char line[512];
  static size_t len;
  static bool jsonLine;
  static bool greeted;
  if (!greeted && Serial && uptime_ms() > 1500) {
    greeted = true;
    Serial.print(cli_banner());
    Serial.print(cli_prompt());
  }
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r' || ch == '\n') {
      if (ch == '\n' && len == 0 && !jsonLine) continue;  // CRLF
      line[len] = 0;
      if (jsonLine) {
        JsonDocument req;
        if (deserializeJson(req, line, len) == DeserializationError::Ok) {
          ReplyTo rt;
          rt.client = CLIENT_SERIAL;
          rt.id = req["id"] | 0;
          rt.mode = REPLY_JSON;
          rpc_dispatch(req, rt);
        } else {
          Serial.println("{\"ok\":false,\"error\":\"bad JSON\"}");
        }
      } else {
        Serial.print("\r\n");
        if (len) cli_exec(line, CLIENT_SERIAL);
        else Serial.print(cli_prompt());
      }
      len = 0;
      jsonLine = false;
      continue;
    }
    if (len == 0 && ch == '{') jsonLine = true;  // machine interface: no echo
    if (ch == 8 || ch == 127) {                 // backspace
      if (len) {
        len--;
        if (!jsonLine) Serial.print("\b \b");
      }
      continue;
    }
    if (ch == 3) {  // Ctrl-C: stop trace, clear line
      len = 0;
      JsonDocument req;
      req["cmd"] = "sub";
      req["topics"]["textTrace"] = false;
      ReplyTo rt;
      rt.client = CLIENT_SERIAL;
      rpc_dispatch(req, rt);
      Serial.print("^C\r\n");
      Serial.print(cli_prompt());
      continue;
    }
    if (ch < 32 && ch != '\t') continue;
    if (len < sizeof(line) - 1) {
      line[len++] = (char)ch;
      if (!jsonLine) Serial.write((uint8_t)ch);
    }
  }
}
