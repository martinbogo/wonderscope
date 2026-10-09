#pragma once
// Traffic trace: a ring buffer (in PSRAM) of every frame seen or sent on
// either bus. The web UI and the console both read from it by sequence number.
#include "common.h"

enum TraceDir : uint8_t { DIR_RX = 0, DIR_TX = 1 };

enum TraceFlags : uint8_t {
  TF_EXT = 0x01,     // CAN 29-bit identifier
  TF_RTR = 0x02,     // CAN remote frame
  TF_CRC_OK = 0x04,  // RS485: Modbus RTU CRC valid
  TF_ERR = 0x08,     // RS485: parity/framing/overflow seen in this frame
  TF_TRUNC = 0x10,   // frame longer than buffer, truncated
  TF_NACK = 0x20,    // I2C: address or data not acknowledged
};
// I2C frames: DIR_TX = write, DIR_RX = read; id = 7-bit address.
// SPI frames: DIR_TX = MOSI, DIR_RX = MISO; id = CS GPIO.

constexpr size_t TRACE_MAX_DATA = 256;

struct TraceFrame {
  uint32_t seq;
  uint64_t tsUs;  // uptime_us() at end of frame
  uint32_t id;    // CAN identifier; RS485: first byte (Modbus address) or 0
  uint8_t bus, dir, flags;
  uint16_t len;
  uint8_t data[TRACE_MAX_DATA];
};

void trace_init();
void trace_add(uint8_t bus, uint8_t dir, uint8_t flags, uint32_t id, const uint8_t *data, size_t len);
uint32_t trace_head();  // next seq to be written
// Copy frames with seq >= from (oldest available if from is too old). Returns count copied.
size_t trace_read(uint32_t from, TraceFrame *out, size_t max, uint32_t *next);
void trace_clear();

struct BusCounters {
  uint32_t rx, tx, err;
};
extern BusCounters g_counters[BUS_COUNT];
