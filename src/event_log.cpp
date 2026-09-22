#include "event_log.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr size_t kLineSize = 160;
constexpr size_t kCapacity = 256;  // 256 x 160 B = 40 KB, lives in PSRAM.

constexpr char kPreviousPath[] = "/journal.prev";
constexpr size_t kPersistLines = 120;
constexpr uint32_t kPersistIntervalMs = 10 * 60 * 1000;

char (*lines)[kLineSize] = nullptr;
size_t head = 0;   // Next slot to write.
size_t count = 0;  // Retained entries.
uint32_t sequence = 0;  // Lines written since boot.
uint32_t lastPersistMs = 0;
SemaphoreHandle_t mutex = nullptr;
// Serialises writers of the snapshot file: the periodic snapshot (net-monitor
// task) and a planned-restart snapshot (an SSH session task, the update
// checker) can coincide, and two writers on one temp file produce an
// interleaved journal that the rename then promotes as the "previous" one.
SemaphoreHandle_t persistMutex = nullptr;

void stampUptime(char* out, size_t outSize) {
  const uint64_t ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  const uint32_t totalSeconds = static_cast<uint32_t>(ms / 1000ULL);
  snprintf(out, outSize, "%lu.%03lu", static_cast<unsigned long>(totalSeconds),
           static_cast<unsigned long>(ms % 1000ULL));
}
}  // namespace

void eventLogInit() {
  if (lines != nullptr) {
    return;
  }
  // Prefer PSRAM: this buffer is written rarely and read only when someone
  // types `logs`, so the slower external RAM costs nothing noticeable and
  // keeps the scarce internal heap free for Wi-Fi/lwIP/libssh buffers.
  lines = static_cast<char (*)[kLineSize]>(
      heap_caps_calloc(kCapacity, kLineSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (lines == nullptr) {
    lines = static_cast<char (*)[kLineSize]>(calloc(kCapacity, kLineSize));
  }
  mutex = xSemaphoreCreateMutex();
  persistMutex = xSemaphoreCreateMutex();
}

void eventLogf(const char* format, ...) {
  char stamp[20];
  stampUptime(stamp, sizeof(stamp));

  char body[kLineSize];
  va_list args;
  va_start(args, format);
  vsnprintf(body, sizeof(body), format, args);
  va_end(args);

  // Serial first: even if the ring is unavailable (very early boot, OOM), the
  // message still reaches the serial console.
  Serial.printf("[%s] %s\n", stamp, body);

  if (lines == nullptr || mutex == nullptr) {
    return;
  }
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;  // Never block a network task on a diagnostic log line.
  }
  snprintf(lines[head], kLineSize, "%s %s", stamp, body);
  head = (head + 1) % kCapacity;
  if (count < kCapacity) {
    ++count;
  }
  ++sequence;
  xSemaphoreGive(mutex);
}

uint32_t eventLogSequence() {
  if (mutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  const uint32_t current = sequence;
  xSemaphoreGive(mutex);
  return current;
}

uint32_t eventLogForEachSince(uint32_t fromSequence,
                              void (*emit)(const char* line, void* userData),
                              void* userData) {
  if (lines == nullptr || mutex == nullptr) {
    return fromSequence;
  }
  for (;;) {
    char line[kLineSize];
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint32_t oldest = sequence - static_cast<uint32_t>(count);
    if (fromSequence < oldest) {
      fromSequence = oldest;  // Already evicted from the ring.
    }
    if (fromSequence >= sequence) {
      xSemaphoreGive(mutex);
      return fromSequence;
    }
    const size_t slot = (head + kCapacity - count + (fromSequence - oldest)) % kCapacity;
    memcpy(line, lines[slot], kLineSize);
    xSemaphoreGive(mutex);
    line[kLineSize - 1] = '\0';
    emit(line, userData);
    ++fromSequence;
  }
}

namespace {
struct PersistState {
  File file;
  size_t written;
};

void persistLine(const char* line, void* userData) {
  PersistState* state = static_cast<PersistState*>(userData);
  state->file.println(line);
  ++state->written;
}
}  // namespace

namespace {
bool persistLocked(const char* reason) {
  if (!SPIFFS.begin(true)) {
    return false;
  }
  // Write to a temporary name first so a reset in the middle of the write
  // leaves the previous snapshot intact rather than a truncated one.
  File file = SPIFFS.open("/journal.tmp", FILE_WRITE);
  if (!file) {
    return false;
  }
  char stamp[20];
  stampUptime(stamp, sizeof(stamp));
  file.printf("# journal saved: %s, uptime %s s, %lu lines written this boot\n", reason,
              stamp, static_cast<unsigned long>(eventLogSequence()));
  const size_t total = eventLogCount();
  const uint32_t from = eventLogSequence() - static_cast<uint32_t>(
                                                 total > kPersistLines ? kPersistLines : total);
  PersistState state{file, 0};
  eventLogForEachSince(from, persistLine, &state);
  state.file.close();
  SPIFFS.remove(kPreviousPath);
  const bool ok = SPIFFS.rename("/journal.tmp", kPreviousPath);
  lastPersistMs = millis();
  return ok;
}
}  // namespace

bool eventLogPersist(const char* reason) {
  // A snapshot takes well under a second; a caller that cannot get the lock
  // within 5 s is racing a stuck SPIFFS and had better not pile on.
  if (persistMutex == nullptr ||
      xSemaphoreTake(persistMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return false;
  }
  const bool ok = persistLocked(reason);
  xSemaphoreGive(persistMutex);
  return ok;
}

void eventLogPersistPeriodically() {
  if (millis() - lastPersistMs >= kPersistIntervalMs) {
    eventLogPersist("periodic snapshot");
  }
}

bool eventLogPreviousForEach(void (*emit)(const char* line, void* userData),
                             void* userData) {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(kPreviousPath)) {
    return false;
  }
  File file = SPIFFS.open(kPreviousPath, FILE_READ);
  if (!file) {
    return false;
  }
  char line[kLineSize];
  while (file.available()) {
    const size_t length = file.readBytesUntil('\n', line, sizeof(line) - 1);
    line[length] = '\0';
    if (length > 0 && line[length - 1] == '\r') {
      line[length - 1] = '\0';
    }
    emit(line, userData);
  }
  file.close();
  return true;
}

size_t eventLogCount() {
  if (mutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  const size_t retained = count;
  xSemaphoreGive(mutex);
  return retained;
}

void eventLogForEach(void (*emit)(const char* line, void* userData),
                     void* userData) {
  if (lines == nullptr || mutex == nullptr) {
    return;
  }
  // Copy each line out under the lock and emit it outside: `emit` writes to
  // an SSH channel and may block on the network for a long time, and the
  // logger must never be held hostage by a slow client.
  for (size_t index = 0;; ++index) {
    char line[kLineSize];
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (index >= count) {
      xSemaphoreGive(mutex);
      return;
    }
    const size_t slot = (head + kCapacity - count + index) % kCapacity;
    memcpy(line, lines[slot], kLineSize);
    xSemaphoreGive(mutex);
    line[kLineSize - 1] = '\0';
    emit(line, userData);
  }
}
