#include <esp_timer.h>
#include <sys/time.h>

#include "common.h"

static const char *const BUS_NAMES[] = {"rs485", "can"};
static const char *const PROTO_NAMES[] = {"modbus", "canopen", "j1939"};

const char *bus_name(uint8_t bus) { return bus < 2 ? BUS_NAMES[bus] : "?"; }
const char *proto_name(uint8_t proto) { return proto < 3 ? PROTO_NAMES[proto] : "?"; }

int bus_from_name(const char *s) {
  if (!s) return -1;
  for (int i = 0; i < 2; i++)
    if (!strcmp(s, BUS_NAMES[i])) return i;
  return -1;
}

int proto_from_name(const char *s) {
  if (!s) return -1;
  for (int i = 0; i < 3; i++)
    if (!strcmp(s, PROTO_NAMES[i])) return i;
  return -1;
}

size_t hex_encode(const uint8_t *data, size_t len, char *out, size_t outCap) {
  static const char *H = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; i < len && o + 2 < outCap; i++) {
    out[o++] = H[data[i] >> 4];
    out[o++] = H[data[i] & 15];
  }
  if (outCap) out[o] = 0;
  return o;
}

String hex_string(const uint8_t *data, size_t len) {
  String s;
  s.reserve(len * 2 + 1);
  static const char *H = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    s += H[data[i] >> 4];
    s += H[data[i] & 15];
  }
  return s;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hex_decode(const char *s, uint8_t *out, size_t cap) {
  if (!s) return 0;
  size_t n = 0;
  int hi = -1;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
      if (hi >= 0) return -1;
      p++;
      continue;
    }
    if (c == ' ' || c == ',' || c == ':' || c == '-' || c == '\t' || c == '\n' || c == '\r') {
      if (hi >= 0) {  // single nibble token like "A" -> 0x0A
        if (n >= cap) return -1;
        out[n++] = (uint8_t)hi;
        hi = -1;
      }
      continue;
    }
    int v = hexval(c);
    if (v < 0) return -1;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= cap) return -1;
      out[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  if (hi >= 0) {
    if (n >= cap) return -1;
    out[n++] = (uint8_t)hi;
  }
  return (int)n;
}

uint64_t uptime_us() { return (uint64_t)esp_timer_get_time(); }

bool time_valid() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec > 1700000000;  // after Nov 2023
}

int64_t epoch_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
