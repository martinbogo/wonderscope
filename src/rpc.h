#pragma once
// Command dispatcher shared by the web dashboard (WebSocket JSON), the web
// console and the USB serial console. Every feature is one command here.
#include "common.h"

// req: {"cmd": "...", ...args}. Replies via reply_ok/reply_err to rt.
void rpc_dispatch(JsonDocument &req, const ReplyTo &rt);
void status_json(JsonObject o);
// Broadcast status + settings to every client after a configuration change,
// so all views of the same state update together.
void state_changed();
void hello_json(JsonObject o);

// Deferred system actions (run from loop()).
void rpc_loop();
