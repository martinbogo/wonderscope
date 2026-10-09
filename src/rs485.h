#pragma once
#include "common.h"
#include "jobs.h"

void rs485_begin();
bool rs485_submit(Job *job);  // takes ownership
void rs485_cancel();          // abort a running scan
void rs485_status_json(JsonObject o);
