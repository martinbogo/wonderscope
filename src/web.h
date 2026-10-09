#pragma once
#include "common.h"

void web_begin();
void web_loop();
void web_text_all(const char *s, size_t n);
void web_text(uint32_t client, const char *s, size_t n);
bool web_client_busy(uint32_t client);  // WebSocket send queue is full
size_t web_client_count();

// Per-client subscriptions (WebSocket clients and the serial console).
enum SubFlags : uint8_t {
  SUB_TRACE = 0x01,   // JSON frame batches (dashboard Traffic tab)
  SUB_IDS = 0x02,     // CAN ID table snapshots
  SUB_TTRACE = 0x04,  // text frame lines (console 'trace on')
  SUB_EVENTS = 0x08,  // serial only: also receive broadcast JSON events
};
struct SubEntry {
  uint32_t client;
  uint8_t flags;
  uint8_t tbus;  // text trace filter: 0 all, 1 rs485, 2 can, 3 i2c, 4 spi
};
uint8_t sub_get(uint32_t client);
void sub_set(uint32_t client, uint8_t flags);
void sub_set_tbus(uint32_t client, uint8_t tbus);
int sub_list(SubEntry *out, int max);
bool web_serial_events();

void push_loop();  // push.cpp: periodic status / trace / device updates
