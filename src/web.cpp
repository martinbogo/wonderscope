// HTTP + WebSocket server.
//
//   GET  /                 dashboard (gzipped assets compiled into firmware)
//   WS   /ws               JSON commands/replies + live events
//   GET  /api/devices.json export device list (labels, watch lists, identities)
//   POST /api/devices      import device list
//   POST /api/update       firmware update (OTA)

#include "web.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>

#include "devices.h"
#include "generated/web_assets.h"
#include "rpc.h"
#include "settings.h"

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ------------------------------------------------------------------ subscriptions

static SubEntry subs[16];
static portMUX_TYPE subMux = portMUX_INITIALIZER_UNLOCKED;

static int sub_find(uint32_t client) {
  for (int i = 0; i < 16; i++)
    if (subs[i].client == client) return i;
  return -1;
}

uint8_t sub_get(uint32_t client) {
  portENTER_CRITICAL(&subMux);
  int i = sub_find(client);
  uint8_t f = i >= 0 ? subs[i].flags : 0;
  portEXIT_CRITICAL(&subMux);
  return f;
}

void sub_set(uint32_t client, uint8_t flags) {
  portENTER_CRITICAL(&subMux);
  int i = sub_find(client);
  if (i < 0) i = sub_find(0);
  if (i >= 0) {
    subs[i].client = client;
    subs[i].flags = flags;
  }
  portEXIT_CRITICAL(&subMux);
}

void sub_set_tbus(uint32_t client, uint8_t tbus) {
  portENTER_CRITICAL(&subMux);
  int i = sub_find(client);
  if (i >= 0) subs[i].tbus = tbus;
  portEXIT_CRITICAL(&subMux);
}

static void sub_remove(uint32_t client) {
  portENTER_CRITICAL(&subMux);
  int i = sub_find(client);
  if (i >= 0) subs[i] = SubEntry{0, 0, 0};
  portEXIT_CRITICAL(&subMux);
}

int sub_list(SubEntry *out, int max) {
  int n = 0;
  portENTER_CRITICAL(&subMux);
  for (int i = 0; i < 16 && n < max; i++)
    if (subs[i].client && subs[i].flags) out[n++] = subs[i];
  portEXIT_CRITICAL(&subMux);
  return n;
}

bool web_serial_events() { return sub_get(CLIENT_SERIAL) & SUB_EVENTS; }

// ------------------------------------------------------------------ helpers

void web_text_all(const char *s, size_t n) {
  if (ws.count()) ws.textAll(s, n);
}

void web_text(uint32_t client, const char *s, size_t n) { ws.text(client, s, n); }

bool web_client_busy(uint32_t client) {
  AsyncWebSocketClient *c = ws.client(client);
  return !c || c->queueIsFull();
}

size_t web_client_count() { return ws.count(); }

static bool authed(AsyncWebServerRequest *r) {
  if (!g_settings.auth.pass[0]) return true;
  return r->authenticate(g_settings.auth.user, g_settings.auth.pass);
}

#define GUARD(req)                                        \
  do {                                                    \
    if (!authed(req)) return req->requestAuthentication(); \
  } while (0)

// ------------------------------------------------------------------ WebSocket

static void handle_ws_message(uint32_t client, const char *data, size_t len) {
  JsonDocument req;
  if (deserializeJson(req, data, len)) {
    JsonDocument e;
    e["ok"] = false;
    e["error"] = "bad JSON";
    out_send(client, e);
    return;
  }
  ReplyTo rt;
  rt.client = client;
  rt.id = req["id"] | -1;
  rt.mode = REPLY_JSON;
  rpc_dispatch(req, rt);
}

static void on_ws(AsyncWebSocket *, AsyncWebSocketClient *c, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  static String frag;
  static uint32_t fragClient;
  switch (type) {
    case WS_EVT_CONNECT: {
      sub_set(c->id(), 0);
      JsonDocument d;
      d["ev"] = "hello";
      hello_json(d["d"].to<JsonObject>());
      out_send(c->id(), d);
      break;
    }
    case WS_EVT_DISCONNECT:
      sub_remove(c->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len) {
        if (info->opcode == WS_TEXT) handle_ws_message(c->id(), (const char *)data, len);
      } else {
        if (info->index == 0) {
          frag = "";
          fragClient = c->id();
        }
        if (fragClient == c->id() && frag.length() + len < 65536) frag.concat((const char *)data, len);
        if (info->final && info->index + len == info->len && fragClient == c->id()) {
          handle_ws_message(c->id(), frag.c_str(), frag.length());
          frag = "";
        }
      }
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------------ HTTP

static String importBuf;

void web_begin() {
  ws.onEvent(on_ws);
  ws.handleHandshake([](AsyncWebServerRequest *r) { return authed(r); });
  server.addHandler(&ws);

  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset *a = &WEB_ASSETS[i];
    auto handler = [a](AsyncWebServerRequest *r) {
      GUARD(r);
      if (r->hasHeader("If-None-Match") && r->header("If-None-Match") == WEB_ASSETS_ETAG) {
        r->send(304);
        return;
      }
      AsyncWebServerResponse *resp = r->beginResponse(200, a->mime, a->data, a->len);
      resp->addHeader("Content-Encoding", "gzip");
      resp->addHeader("ETag", WEB_ASSETS_ETAG);
      resp->addHeader("Cache-Control", "no-cache");
      r->send(resp);
    };
    server.on(a->path, HTTP_GET, handler);
    if (!strcmp(a->path, "/index.html")) server.on("/", HTTP_GET, handler);
  }

  server.on("/api/devices.json", HTTP_GET, [](AsyncWebServerRequest *r) {
    GUARD(r);
    AsyncWebServerResponse *resp = r->beginResponse(200, "application/json", dev_export_json());
    resp->addHeader("Content-Disposition", "attachment; filename=\"wonderscope-devices.json\"");
    r->send(resp);
  });

  server.on(
      "/api/devices", HTTP_POST,
      [](AsyncWebServerRequest *r) {
        GUARD(r);
        const char *err = dev_import_json(importBuf.c_str(), importBuf.length());
        importBuf = "";
        if (err) return r->send(400, "text/plain", err);
        JsonDocument e;
        e["ev"] = "devreload";
        event_send(e);
        r->send(200, "text/plain", "OK");
      },
      nullptr,
      [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!authed(r)) return;
        if (index == 0) {
          importBuf = "";
          importBuf.reserve(total);
        }
        if (total <= 256 * 1024) importBuf.concat((const char *)data, len);
      });

  server.on(
      "/api/update", HTTP_POST,
      [](AsyncWebServerRequest *r) {
        GUARD(r);
        bool ok = !Update.hasError();
        AsyncWebServerResponse *resp =
            r->beginResponse(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : Update.errorString());
        resp->addHeader("Connection", "close");
        r->send(resp);
        if (ok) {
          JsonDocument q;
          q["cmd"] = "sys.reboot";
          ReplyTo rt;
          rpc_dispatch(q, rt);
        }
      },
      [](AsyncWebServerRequest *r, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!authed(r)) return;
        if (index == 0) {
          Serial.printf("[ota] receiving %s\n", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
        }
        if (Update.isRunning() && Update.write(data, len) != len) Update.printError(Serial);
        if (final) {
          if (Update.end(true)) Serial.printf("[ota] done, %u bytes\n", (unsigned)(index + len));
          else Update.printError(Serial);
        }
      });

  // Captive portal: anything unknown on another host goes to the dashboard.
  server.onNotFound([](AsyncWebServerRequest *r) {
    String host = r->host();
    bool ours = host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString() ||
                host.startsWith(g_settings.wifi.hostname);
    if (r->method() == HTTP_GET && !ours) {
      r->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
      return;
    }
    r->send(404, "text/plain", "Not found");
  });

  server.begin();
}

void web_loop() {
  static uint32_t last;
  uint32_t now = uptime_ms();
  if (now - last > 1000) {
    last = now;
    ws.cleanupClients();
  }
}
