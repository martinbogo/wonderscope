// Outbound message queue.
//
// Bus tasks and the async web task all produce JSON; only loop() actually
// writes to the WebSocket / USB serial, so sends never block a bus task.

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>

#include "cli.h"
#include "common.h"
#include "web.h"

struct OutMsg {
  uint32_t client;
  char *s;
  size_t n;
};

static QueueHandle_t q;
static volatile uint32_t dropped;

void out_init() { q = xQueueCreate(96, sizeof(OutMsg)); }

uint32_t out_dropped() { return dropped; }

static char *alloc_str(size_t n) {
  char *s = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s) s = (char *)malloc(n + 1);
  return s;
}

static void enqueue(uint32_t client, char *s, size_t n) {
  OutMsg m{client, s, n};
  if (xQueueSend(q, &m, pdMS_TO_TICKS(client == CLIENT_BROADCAST ? 0 : 50)) != pdTRUE) {
    free(s);
    dropped++;
  }
}

void out_send(uint32_t client, const JsonDocument &doc) {
  size_t n = measureJson(doc);
  char *s = alloc_str(n);
  if (!s) {
    dropped++;
    return;
  }
  serializeJson(doc, s, n + 1);
  enqueue(client, s, n);
}

void out_text(uint32_t client, const String &text, int32_t id) {
  if (client == CLIENT_SERIAL) {
    // The serial console gets plain text with CRLF line endings, and a fresh
    // prompt after each command's final reply.
    String t = text;
    if (t.endsWith("\n")) t.remove(t.length() - 1);
    if (id >= 0) t += (t.length() ? "\n" : "") + String(cli_prompt());
    t.replace("\n", "\r\n");
    char *s = alloc_str(t.length());
    if (!s) return;
    memcpy(s, t.c_str(), t.length() + 1);
    enqueue(client, s, t.length());
    return;
  }
  // Web console: wrap in a JSON event so the page can tell it apart.
  JsonDocument d;
  d["ev"] = "cli";
  if (id >= 0) d["id"] = id;
  d["text"] = text;
  out_send(client, d);
}

void out_pump() {
  OutMsg m;
  int budget = 48;
  while (budget-- > 0 && xQueueReceive(q, &m, 0) == pdTRUE) {
    if (m.client == CLIENT_SERIAL) {
      Serial.write((const uint8_t *)m.s, m.n);
      // console replies end with the prompt; everything else gets a newline
      if (!(m.n >= 2 && m.s[m.n - 2] == '>' && m.s[m.n - 1] == ' ')) Serial.write("\r\n");
    } else if (m.client == CLIENT_BROADCAST) {
      web_text_all(m.s, m.n);
      if (web_serial_events()) {
        Serial.write((const uint8_t *)m.s, m.n);
        Serial.write('\n');
      }
    } else {
      web_text(m.client, m.s, m.n);
    }
    free(m.s);
  }
}

void reply_ok(const ReplyTo &r, JsonDocument &result) {
  if (r.id < 0) return;
  if (r.mode == REPLY_TEXT) {
    out_text(r.client, cli_render(r.cmd, result), r.id);
    return;
  }
  JsonDocument d;
  d["id"] = r.id;
  d["ok"] = true;
  d["result"] = result;
  out_send(r.client, d);
}

void reply_ok(const ReplyTo &r) {
  JsonDocument empty;
  empty.to<JsonObject>();
  reply_ok(r, empty);
}

void reply_err(const ReplyTo &r, const char *fmt, ...) {
  if (r.id < 0) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (r.mode == REPLY_TEXT) {
    out_text(r.client, String("error: ") + buf, r.id);
    return;
  }
  JsonDocument d;
  d["id"] = r.id;
  d["ok"] = false;
  d["error"] = buf;
  out_send(r.client, d);
}

void event_send(JsonDocument &doc) { out_send(CLIENT_BROADCAST, doc); }
