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

// Total number of lines written since boot; each line's sequence number is
// its position in that count. Lets a reader ask for "everything after X".
uint32_t eventLogSequence();

// Calls `emit(line, userData)` for every retained line, oldest first. `emit`
// receives a NUL-terminated copy that stays valid only for the callback.
void eventLogForEach(void (*emit)(const char* line, void* userData),
                     void* userData);

// Same, but only lines with a sequence number >= `fromSequence` (lines that
// have already been evicted from the ring are skipped). Returns the sequence
// number to pass next time to continue where this call stopped.
uint32_t eventLogForEachSince(uint32_t fromSequence,
                              void (*emit)(const char* line, void* userData),
                              void* userData);

// Persistence across reboots. The ring lives in RAM and dies with the reboot
// that usually follows the interesting event, so the last lines are written
// to a file on SPIFFS: on every planned restart (reboot command, OTA, Wi-Fi
// loss restart, rollback) and periodically from the network monitor, which
// bounds the loss after a panic or watchdog reset to the snapshot interval.
bool eventLogPersist(const char* reason);
void eventLogPersistPeriodically();  // Call from a task loop; snapshots every 10 min.
// Emits the saved journal from before the last reboot; false if none exists.
bool eventLogPreviousForEach(void (*emit)(const char* line, void* userData),
                             void* userData);
