// Wi-Fi: the board always runs its own access point (192.168.4.1, with a
// captive-portal DNS so phones pop the dashboard up), and optionally joins an
// existing network as a station, advertised as http://<hostname>.local.

#include "wifi_mgr.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_sntp.h>

#include "rtc.h"
#include "settings.h"

static DNSServer dns;
static bool dnsUp, mdnsUp;
static volatile bool ntpSynced;

static void on_ntp(struct timeval *) {
  ntpSynced = true;
  rtc_write_now();
}

bool wifi_ntp_synced() { return ntpSynced; }

String wifi_mac() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static void on_event(WiFiEvent_t ev, WiFiEventInfo_t) {
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[wifi] joined %s, IP %s  ->  http://%s.local/\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(), g_settings.wifi.hostname);
    sntp_set_time_sync_notification_cb(on_ntp);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  }
}

void wifi_apply() {
  const WifiSettings &w = g_settings.wifi;
  if (dnsUp) {
    dns.stop();
    dnsUp = false;
  }
  if (mdnsUp) {
    MDNS.end();
    mdnsUp = false;
  }
  if (WiFi.getMode() & WIFI_STA) WiFi.disconnect(true);
  if (WiFi.getMode() & WIFI_AP) WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.persistent(false);
  WiFi.setHostname(w.hostname);
  WiFi.mode(w.staSsid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(w.apSsid, w.apPass[0] ? w.apPass : nullptr);
  WiFi.setSleep(false);  // lower latency for the live dashboard
  if (w.staSsid[0]) WiFi.begin(w.staSsid, w.staPass[0] ? w.staPass : nullptr);
  dnsUp = dns.start(53, "*", WiFi.softAPIP());
  if (MDNS.begin(w.hostname)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
  }
  Serial.printf("[wifi] AP \"%s\" at http://%s/  (%s)\n", w.apSsid, WiFi.softAPIP().toString().c_str(),
                w.apPass[0] ? "WPA2" : "open");
}

void wifi_begin() {
  WiFi.onEvent(on_event);
  wifi_apply();
}

void wifi_loop() {
  if (dnsUp) dns.processNextRequest();
}

void wifi_status_json(JsonObject o) {
  JsonObject ap = o["ap"].to<JsonObject>();
  ap["ssid"] = g_settings.wifi.apSsid;
  ap["ip"] = WiFi.softAPIP().toString();
  ap["clients"] = WiFi.softAPgetStationNum();
  JsonObject sta = o["sta"].to<JsonObject>();
  sta["ssid"] = g_settings.wifi.staSsid;
  bool conn = WiFi.status() == WL_CONNECTED;
  sta["connected"] = conn;
  if (conn) {
    sta["ip"] = WiFi.localIP().toString();
    sta["rssi"] = WiFi.RSSI();
  }
  o["hostname"] = g_settings.wifi.hostname;
  o["ntp"] = (bool)ntpSynced;
}
