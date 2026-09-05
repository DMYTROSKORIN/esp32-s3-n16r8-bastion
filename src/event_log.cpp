#include "event_log.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr size_t kLineSize = 112;
constexpr size_t kCapacity = 256;  // 256 x 112 B = 28 KB, lives in PSRAM.

char (*lines)[kLineSize] = nullptr;
size_t head = 0;   // Next slot to write.
size_t count = 0;  // Retained entries.
SemaphoreHandle_t mutex = nullptr;

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
  xSemaphoreGive(mutex);
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
