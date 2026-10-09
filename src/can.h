#pragma once
#include "common.h"
#include "jobs.h"

void can_begin();
bool can_submit(Job *job);  // takes ownership
void can_cancel();
void can_status_json(JsonObject o);
// Snapshot of the passive CAN ID table (sorted by identifier).
size_t can_ids_json(JsonArray arr, size_t max);
void can_ids_clear();
size_t can_ids_count();

// J1939 PGN helpers shared with the console.
uint32_t j1939_pgn_of(uint32_t id);
