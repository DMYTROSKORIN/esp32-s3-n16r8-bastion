#pragma once

#include <stddef.h>
#include <stdint.h>

// Lightweight, secrets-free event journal: a fixed-size ring buffer of short
// text lines that lives in PSRAM (falls back to internal RAM when PSRAM is
// missing). Every entry is also echoed to the serial console with an uptime
// stamp, so the serial log and `logs` over SSH show the same history.
//
// Safe to call from any task; the ring is protected by a FreeRTOS mutex and
// the formatting happens on the caller's stack (keep lines short).
void eventLogInit();
void eventLogf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Number of lines currently retained (0..kEventLogCapacity).
size_t eventLogCount();

// Calls `emit(line, userData)` for every retained line, oldest first. `emit`
// receives a NUL-terminated copy that stays valid only for the callback.
void eventLogForEach(void (*emit)(const char* line, void* userData),
                     void* userData);
