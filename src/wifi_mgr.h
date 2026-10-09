#pragma once
#include "common.h"

void wifi_begin();
void wifi_apply();  // re-read settings and restart AP/STA
void wifi_loop();   // captive-portal DNS
void wifi_status_json(JsonObject o);
String wifi_mac();
bool wifi_ntp_synced();
