#pragma once
// Text console. The same commands are available over USB serial and in the
// web dashboard's Console tab; each one maps onto an rpc_dispatch() command.
#include "common.h"
#include "trace.h"

void cli_exec(const char *line, uint32_t client, int32_t id = -1);
String cli_render(const char *cmd, JsonDocument &result);
String cli_banner();
String cli_trace_line(const TraceFrame &f);
void cli_serial_loop();  // reads USB serial: console lines, or JSON lines starting with '{'
const char *cli_prompt();
