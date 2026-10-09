#pragma once
// A unit of work for a bus task. Requests are queued as jobs so the web
// server / console never block waiting on a bus.
#include "common.h"

struct Job {
  String cmd;
  JsonDocument args;
  ReplyTo rt;
};
