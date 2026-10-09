#include "trace.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

constexpr uint32_t TRACE_SLOTS = 2048;  // ~560 KB of PSRAM

static TraceFrame *ring;
static uint32_t head;  // seq of next frame
static SemaphoreHandle_t mtx;

BusCounters g_counters[BUS_COUNT];

void trace_init() {
  ring = (TraceFrame *)heap_caps_calloc(TRACE_SLOTS, sizeof(TraceFrame), MALLOC_CAP_SPIRAM);
  mtx = xSemaphoreCreateMutex();
}

void trace_add(uint8_t bus, uint8_t dir, uint8_t flags, uint32_t id, const uint8_t *data, size_t len) {
  if (bus < BUS_COUNT) {
    if (dir == DIR_TX) g_counters[bus].tx++;
    else g_counters[bus].rx++;
    if (flags & TF_ERR) g_counters[bus].err++;
  }
  if (!ring) return;
  if (len > TRACE_MAX_DATA) {
    len = TRACE_MAX_DATA;
    flags |= TF_TRUNC;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  TraceFrame &f = ring[head % TRACE_SLOTS];
  f.seq = head;
  f.tsUs = uptime_us();
  f.id = id;
  f.bus = bus;
  f.dir = dir;
  f.flags = flags;
  f.len = (uint16_t)len;
  memcpy(f.data, data, len);
  head++;
  xSemaphoreGive(mtx);
}

uint32_t trace_head() { return head; }

size_t trace_read(uint32_t from, TraceFrame *out, size_t max, uint32_t *next) {
  if (!ring) {
    *next = from;
    return 0;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint32_t oldest = head > TRACE_SLOTS ? head - TRACE_SLOTS : 0;
  if (from < oldest || from > head) from = oldest;
  size_t n = 0;
  while (from < head && n < max) {
    out[n++] = ring[from % TRACE_SLOTS];
    from++;
  }
  *next = from;
  xSemaphoreGive(mtx);
  return n;
}

void trace_clear() {
  // Frames stay in the ring (cheap) but clients skip ahead; counters reset.
  memset(g_counters, 0, sizeof(g_counters));
}
