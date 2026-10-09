// Periodic pushes to subscribers: live trace (JSON for the dashboard, text
// for consoles), status, device changes and the CAN ID table.

#include <esp_heap_caps.h>

#include "can.h"
#include "cli.h"
#include "devices.h"
#include "rpc.h"
#include "trace.h"
#include "web.h"

static constexpr size_t BATCH = 160;
static constexpr size_t BATCH_HEX_BUDGET = 24 * 1024;
static constexpr int TEXT_LINES_MAX = 80;

static void push_trace(uint32_t &cursor) {
  static TraceFrame *buf;
  if (!buf) buf = (TraceFrame *)heap_caps_malloc(sizeof(TraceFrame) * BATCH, MALLOC_CAP_SPIRAM);
  if (!buf) return;

  SubEntry subs[16];
  int ns = sub_list(subs, 16);
  bool anyJson = false, anyText = false;
  for (int i = 0; i < ns; i++) {
    anyJson |= (subs[i].flags & SUB_TRACE) != 0;
    anyText |= (subs[i].flags & SUB_TTRACE) != 0;
  }
  uint32_t head = trace_head();
  if (!anyJson && !anyText) {
    cursor = head;
    return;
  }
  if (cursor == head) return;

  uint32_t next;
  size_t n = trace_read(cursor, buf, BATCH, &next);
  uint32_t dropped = n ? buf[0].seq - cursor : 0;
  // Respect the hex budget so one batch never gets huge (long RS485 frames).
  size_t used = 0, take = 0;
  for (; take < n; take++) {
    used += buf[take].len * 2 + 48;
    if (used > BATCH_HEX_BUDGET && take > 0) break;
  }
  cursor = take ? buf[take - 1].seq + 1 : next;
  if (!take) return;

  if (anyJson) {
    JsonDocument d;
    d["ev"] = "tr";
    if (dropped) d["drop"] = dropped;
    JsonArray f = d["f"].to<JsonArray>();
    for (size_t i = 0; i < take; i++) {
      const TraceFrame &t = buf[i];
      JsonArray r = f.add<JsonArray>();
      r.add(t.seq);
      r.add((double)t.tsUs / 1000.0);  // ms since boot
      r.add(t.bus);
      r.add(t.dir);
      r.add(t.flags);
      r.add(t.id);
      r.add(hex_string(t.data, t.len));
    }
    for (int i = 0; i < ns; i++) {
      if (!(subs[i].flags & SUB_TRACE)) continue;
      if (subs[i].client != CLIENT_SERIAL && web_client_busy(subs[i].client)) continue;  // slow client: skip batch
      out_send(subs[i].client, d);
    }
  }
  if (anyText) {
    for (int i = 0; i < ns; i++) {
      if (!(subs[i].flags & SUB_TTRACE)) continue;
      if (subs[i].client != CLIENT_SERIAL && web_client_busy(subs[i].client)) continue;
      String text;
      int lines = 0, skipped = 0;
      for (size_t k = 0; k < take; k++) {
        const TraceFrame &t = buf[k];
        if (subs[i].tbus == 1 && t.bus != BUS_RS485) continue;
        if (subs[i].tbus == 2 && t.bus != BUS_CAN) continue;
        if (lines >= TEXT_LINES_MAX) {
          skipped++;
          continue;
        }
        if (lines) text += "\n";
        text += cli_trace_line(t);
        lines++;
      }
      if (dropped) text = String("... ") + dropped + " frames dropped (too fast to print)\n" + text;
      if (skipped) text += String("\n... ") + skipped + " more frames not printed";
      if (lines || skipped) out_text(subs[i].client, text, -1);
    }
  }
}

void push_loop() {
  static uint32_t lastTrace, lastDev, lastSlow, cursor;
  uint32_t now = uptime_ms();
  if (now - lastTrace >= 100) {
    lastTrace = now;
    push_trace(cursor);
  }
  if (!web_client_count()) return;
  if (now - lastDev >= 250) {
    lastDev = now;
    JsonDocument d;
    d["ev"] = "dev";
    JsonArray a = d["d"].to<JsonArray>();
    if (dev_collect_changed(a, 16)) {
      d["now"] = now;
      event_send(d);
    }
  }
  if (now - lastSlow >= 1000) {
    lastSlow = now;
    JsonDocument s;
    s["ev"] = "status";
    status_json(s["s"].to<JsonObject>());
    event_send(s);

    SubEntry subs[16];
    int ns = sub_list(subs, 16);
    bool anyIds = false;
    for (int i = 0; i < ns; i++) anyIds |= (subs[i].flags & SUB_IDS) != 0;
    if (anyIds) {
      JsonDocument d;
      d["ev"] = "ids";
      can_ids_json(d["ids"].to<JsonArray>(), 768);
      for (int i = 0; i < ns; i++)
        if ((subs[i].flags & SUB_IDS) && !web_client_busy(subs[i].client)) out_send(subs[i].client, d);
    }
  }
}
