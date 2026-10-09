#pragma once
// Modbus RTU framing helpers.
#include <stddef.h>
#include <stdint.h>

inline uint16_t mb_crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
  }
  return c;
}

inline bool mb_crc_ok(const uint8_t *d, size_t n) {
  if (n < 4) return false;
  uint16_t c = mb_crc16(d, n - 2);
  return d[n - 2] == (c & 0xFF) && d[n - 1] == (c >> 8);
}

inline size_t mb_append_crc(uint8_t *d, size_t n) {
  uint16_t c = mb_crc16(d, n);
  d[n] = c & 0xFF;
  d[n + 1] = c >> 8;
  return n + 2;
}

// Transaction status: >0 is a Modbus exception code from the device.
enum MbStatus : int {
  MB_OK = 0,
  MB_TIMEOUT = -1,
  MB_CRC = -2,
  MB_BADRESP = -3,
  MB_DISABLED = -4,
  MB_BADARG = -5,
};

inline const char *mb_status_name(int st) {
  switch (st) {
    case MB_OK: return "ok";
    case MB_TIMEOUT: return "timeout (no response)";
    case MB_CRC: return "CRC error in response";
    case MB_BADRESP: return "malformed response";
    case MB_DISABLED: return "RS485 is disabled";
    case MB_BADARG: return "invalid argument";
    case 1: return "exception 01: illegal function";
    case 2: return "exception 02: illegal data address";
    case 3: return "exception 03: illegal data value";
    case 4: return "exception 04: server device failure";
    case 5: return "exception 05: acknowledge";
    case 6: return "exception 06: server device busy";
    case 8: return "exception 08: memory parity error";
    case 10: return "exception 0A: gateway path unavailable";
    case 11: return "exception 0B: gateway target failed to respond";
  }
  return st > 0 ? "exception (vendor specific)" : "error";
}
