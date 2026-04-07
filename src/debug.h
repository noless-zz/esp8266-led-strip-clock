#pragma once

#include "config.h"

// ============================================================================
// Remote Debug Logging
// ============================================================================

void debugLogf(const char* level, const char* tag, const char* fmt, ...);

#define DLOGI(tag, ...) debugLogf("INF", tag, __VA_ARGS__)
#define DLOGE(tag, ...) debugLogf("ERR", tag, __VA_ARGS__)
#define DLOGW(tag, ...) debugLogf("WRN", tag, __VA_ARGS__)

void captureBootInfo();
