#include "ota_update.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <sodium/crypto_sign_ed25519.h>
#include <string.h>

#include "event_log.h"
#include "firmware_info.h"
#include "ota_public_key.h"

namespace {
constexpr uint32_t kSelfTestTimeoutMs = 2 * 60 * 1000;
constexpr uint32_t kSelfTestPollMs = 500;
constexpr uint32_t kHttpTimeoutMs = 20000;
constexpr size_t kHttpChunk = 4096;
constexpr int kMaxRedirects = 5;
constexpr size_t kMinImageBytes = 64 * 1024;  // Anything smaller is not an app.

std::atomic<bool> serviceUp{false};
std::atomic<bool> selfTestPending{false};

// The journal dies with every reboot, and an OTA outcome is exactly the kind
// of event that is followed by a reboot. Keep the last one in NVS so
// `ota status` can still say what happened (e.g. why a rollback occurred).
constexpr char kNvsNamespace[] = "recovery";
constexpr char kNvsLastOtaKey[] = "ota_last";

void rememberOtaEvent(const char* format, ...) __attribute__((format(printf, 1, 2)));
void rememberOtaEvent(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  eventLogf("OTA: %s", text);
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putString(kNvsLastOtaKey, text);
    prefs.end();
  }
}

void lastOtaEvent(char* out, size_t outSize) {
  out[0] = '\0';
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, true)) {
    if (prefs.isKey(kNvsLastOtaKey)) {
      prefs.getString(kNvsLastOtaKey, out, outSize);
    }
    prefs.end();
  }
}

void setResult(OtaResult& result, bool ok, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void setResult(OtaResult& result, bool ok, const char* format, ...) {
  result.ok = ok;
  va_list args;
  va_start(args, format);
  vsnprintf(result.message, sizeof(result.message), format, args);
  va_end(args);
}

const char* stateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "pending-verify";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "invalid";
    case ESP_OTA_IMG_ABORTED:
      return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
      return "undefined";
  }
}
}  // namespace

// The Arduino core marks a PENDING_VERIFY image valid during its own start-up
// unless this weak hook says otherwise. Say otherwise: the decision belongs to
// the self-test below, which waits for Wi-Fi and the SSH server first.
extern "C" bool verifyRollbackLater() { return true; }

// ---------------------------------------------------------------------------
// OtaSink
// ---------------------------------------------------------------------------

struct OtaSink::Impl {
  const esp_partition_t* target = nullptr;
  esp_ota_handle_t handle = 0;
  bool active = false;
  mbedtls_sha256_context sha;
  bool shaActive = false;
  // The last kOtaTrailerSize bytes seen so far: they are the trailer only
  // once the stream ends, so they are held back from the hash/flash until
  // more data proves they were image bytes after all.
  uint8_t tail[kOtaTrailerSize];
  size_t tailLength = 0;
  size_t imageBytes = 0;
  bool imageStartChecked = false;

  bool commitImageBytes(const uint8_t* data, size_t length, OtaResult& result) {
    if (length == 0) {
      return true;
    }
    if (!imageStartChecked) {
      // ESP image magic byte; catches "someone uploaded the .elf/.zip".
      if (data[0] != 0xE9) {
        setResult(result, false, "not an ESP32 app image (bad magic 0x%02x)", data[0]);
        return false;
      }
      imageStartChecked = true;
    }
    mbedtls_sha256_update(&sha, data, length);
    const esp_err_t err = esp_ota_write(handle, data, length);
    if (err != ESP_OK) {
      setResult(result, false, "flash write failed at %u B: %s",
                static_cast<unsigned>(imageBytes), esp_err_to_name(err));
      return false;
    }
    imageBytes += length;
    return true;
  }
};

OtaSink::OtaSink() : impl_(new Impl()) {}

OtaSink::~OtaSink() {
  abort();
  delete impl_;
}

bool OtaSink::begin(OtaResult& result) {
  result = OtaResult{};
  const esp_partition_t* running = esp_ota_get_running_partition();
  impl_->target = esp_ota_get_next_update_partition(nullptr);
  if (impl_->target == nullptr || running == nullptr ||
      impl_->target->address == running->address) {
    setResult(result, false, "no inactive OTA slot in the partition table");
    return false;
  }
  result.targetLabel = impl_->target->label;
  // OTA_WITH_SEQUENTIAL_WRITES erases sectors just ahead of the write cursor
  // instead of the whole 6.25 MB slot up front (which takes ~20 s and would
  // stall the SSH session before the first progress line).
  const esp_err_t err =
      esp_ota_begin(impl_->target, OTA_WITH_SEQUENTIAL_WRITES, &impl_->handle);
  if (err != ESP_OK) {
    setResult(result, false, "esp_ota_begin(%s) failed: %s", impl_->target->label,
              esp_err_to_name(err));
    return false;
  }
  impl_->active = true;
  mbedtls_sha256_init(&impl_->sha);
  mbedtls_sha256_starts(&impl_->sha, 0);
  impl_->shaActive = true;
  impl_->tailLength = 0;
  impl_->imageBytes = 0;
  impl_->imageStartChecked = false;
  received_ = 0;
  return true;
}

bool OtaSink::feed(const uint8_t* data, size_t length, OtaResult& result) {
  if (!impl_->active) {
    setResult(result, false, "sink not started");
    return false;
  }
  received_ += length;
  // Merge the held-back tail with the new data, keep the last
  // kOtaTrailerSize bytes back again, commit everything before them.
  while (length > 0) {
    if (impl_->tailLength < kOtaTrailerSize) {
      const size_t take = min(length, kOtaTrailerSize - impl_->tailLength);
      memcpy(impl_->tail + impl_->tailLength, data, take);
      impl_->tailLength += take;
      data += take;
      length -= take;
      continue;
    }
    // Tail is full: commit as much of the tail as the new data displaces.
    const size_t displace = min(length, kOtaTrailerSize);
    if (!impl_->commitImageBytes(impl_->tail, displace, result)) {
      return false;
    }
    memmove(impl_->tail, impl_->tail + displace, kOtaTrailerSize - displace);
    impl_->tailLength = kOtaTrailerSize - displace;
    // Whatever remains beyond one tail-length can go straight through.
    if (length > kOtaTrailerSize) {
      const size_t direct = length - kOtaTrailerSize;
      if (!impl_->commitImageBytes(data, direct, result)) {
        return false;
      }
      data += direct;
      length -= direct;
    }
  }
  if (result.message[0] == '\0') {
    result.ok = true;
  }
  return true;
}

bool OtaSink::finish(OtaResult& result) {
  if (!impl_->active) {
    setResult(result, false, "sink not started");
    return false;
  }
  if (impl_->tailLength < kOtaTrailerSize || impl_->imageBytes < kMinImageBytes) {
    setResult(result, false, "image too short (%u B) - not a signed firmware file",
              static_cast<unsigned>(received_));
    abort();
    return false;
  }

  // Trailer: version (hashed together with the image) + signature.
  const uint8_t* versionField = impl_->tail;
  const uint8_t* signature = impl_->tail + kOtaVersionFieldSize;
  memcpy(result.version, versionField, kOtaVersionFieldSize);
  result.version[kOtaVersionFieldSize] = '\0';
  for (char* c = result.version; *c != '\0'; ++c) {
    if (*c < 0x20 || *c > 0x7e) {
      *c = '\0';
      break;
    }
  }
  mbedtls_sha256_update(&impl_->sha, versionField, kOtaVersionFieldSize);
  uint8_t digest[32];
  mbedtls_sha256_finish(&impl_->sha, digest);
  mbedtls_sha256_free(&impl_->sha);
  impl_->shaActive = false;

  if (crypto_sign_ed25519_verify_detached(signature, digest, sizeof(digest),
                                          kOtaPublicKey) != 0) {
    setResult(result, false,
              "signature check FAILED - image is not signed with this firmware's "
              "release key (version field: '%s')",
              result.version);
    abort();
    return false;
  }

  // ESP-IDF's own validation: segment layout, checksum, the image's SHA-256.
  esp_err_t err = esp_ota_end(impl_->handle);
  impl_->active = false;
  if (err != ESP_OK) {
    setResult(result, false, "image rejected by esp_ota_end: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_ota_set_boot_partition(impl_->target);
  if (err != ESP_OK) {
    setResult(result, false, "could not set boot partition: %s", esp_err_to_name(err));
    return false;
  }
  result.imageBytes = impl_->imageBytes;
  esp_app_desc_t description = {};
  const bool haveDescription =
      esp_ota_get_partition_description(impl_->target, &description) == ESP_OK;
  setResult(result, true, "verified %s (%u KB, IDF %s) written to %s", result.version,
            static_cast<unsigned>(impl_->imageBytes / 1024U),
            haveDescription ? description.idf_ver : "?", impl_->target->label);
  rememberOtaEvent("installed v%s into %s (replacing v%s), awaiting first boot",
                   result.version, impl_->target->label, FIRMWARE_VERSION);
  return true;
}

void OtaSink::abort() {
  if (impl_->shaActive) {
    mbedtls_sha256_free(&impl_->sha);
    impl_->shaActive = false;
  }
  if (impl_->active) {
    esp_ota_abort(impl_->handle);
    impl_->active = false;
  }
}

// ---------------------------------------------------------------------------
// HTTPS download
// ---------------------------------------------------------------------------

namespace {
void report(OtaReportFn fn, void* userData, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void report(OtaReportFn fn, void* userData, const char* format, ...) {
  if (fn == nullptr) {
    return;
  }
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  fn(line, userData);
}
}  // namespace

bool otaFromUrl(const char* url, OtaReportFn reportFn, void* userData, OtaResult& result) {
  result = OtaResult{};
  if (url == nullptr || strncmp(url, "https://", 8) != 0) {
    setResult(result, false, "only https:// URLs are accepted");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setResult(result, false, "Wi-Fi is not connected");
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kHttpChunk;
  config.buffer_size_tx = 1024;
  config.disable_auto_redirect = true;  // Handled below so each hop is logged.
  config.keep_alive_enable = true;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    setResult(result, false, "http client init failed");
    return false;
  }

  uint8_t* buffer = static_cast<uint8_t*>(malloc(kHttpChunk));
  if (buffer == nullptr) {
    esp_http_client_cleanup(client);
    setResult(result, false, "out of memory");
    return false;
  }

  bool ok = false;
  int64_t contentLength = -1;
  int status = 0;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      setResult(result, false, "connect failed: %s", esp_err_to_name(err));
      goto done;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308) {
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
      report(reportFn, userData, "Redirect %d, following (%d/%d)", status, hop + 1,
             kMaxRedirects);
      if (hop == kMaxRedirects) {
        setResult(result, false, "too many redirects");
        goto done;
      }
      continue;
    }
    break;
  }
  if (status != 200) {
    setResult(result, false, "server answered HTTP %d", status);
    goto done;
  }
  if (contentLength > 0 && contentLength < static_cast<int64_t>(kMinImageBytes)) {
    setResult(result, false, "response too small (%lld B) to be a firmware image",
              static_cast<long long>(contentLength));
    goto done;
  }
  report(reportFn, userData, "Downloading %s", contentLength > 0 ? "" : "(unknown size)");
  if (contentLength > 0) {
    report(reportFn, userData, "Size: %lld KB", static_cast<long long>(contentLength / 1024));
  }

  {
    OtaSink sink;
    if (!sink.begin(result)) {
      goto done;
    }
    report(reportFn, userData, "Writing into %s ...", result.targetLabel);
    size_t lastReported = 0;
    while (true) {
      const int count = esp_http_client_read(client, reinterpret_cast<char*>(buffer),
                                             kHttpChunk);
      if (count < 0) {
        setResult(result, false, "download error after %u B",
                  static_cast<unsigned>(sink.bytesReceived()));
        sink.abort();
        goto done;
      }
      if (count == 0) {
        if (esp_http_client_is_complete_data_received(client) || contentLength <= 0) {
          break;
        }
        setResult(result, false, "connection closed after %u of %lld B",
                  static_cast<unsigned>(sink.bytesReceived()),
                  static_cast<long long>(contentLength));
        sink.abort();
        goto done;
      }
      if (!sink.feed(buffer, static_cast<size_t>(count), result)) {
        sink.abort();
        goto done;
      }
      if (sink.bytesReceived() - lastReported >= 256 * 1024) {
        lastReported = sink.bytesReceived();
        if (contentLength > 0) {
          report(reportFn, userData, "  %u KB (%u%%)",
                 static_cast<unsigned>(lastReported / 1024),
                 static_cast<unsigned>(lastReported * 100 / contentLength));
        } else {
          report(reportFn, userData, "  %u KB", static_cast<unsigned>(lastReported / 1024));
        }
      }
    }
    ok = sink.finish(result);
  }

done:
  free(buffer);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (!ok) {
    eventLogf("OTA: download from URL failed: %s", result.message);
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Slots, rollback, self-test
// ---------------------------------------------------------------------------

void otaDescribeSlots(char* out, size_t outSize) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  size_t used = snprintf(out, outSize, "Running: %s (v%s)   Next boot: %s\r\n",
                         running != nullptr ? running->label : "?", FIRMWARE_VERSION,
                         boot != nullptr ? boot->label : "?");
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr && used < outSize) {
    const esp_partition_t* part = esp_partition_get(it);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(part, &state);
    esp_app_desc_t description = {};
    const bool hasImage = esp_ota_get_partition_description(part, &description) == ESP_OK;
    used += snprintf(out + used, outSize - used,
                     "  %-5s @ 0x%06lx  %5lu KB  %-14s %s%s\r\n", part->label,
                     static_cast<unsigned long>(part->address),
                     static_cast<unsigned long>(part->size / 1024UL), stateName(state),
                     hasImage ? "image present, IDF " : "empty",
                     hasImage ? description.idf_ver : "");
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  char last[160];
  lastOtaEvent(last, sizeof(last));
  if (used < outSize) {
    snprintf(out + used, outSize - used,
             "Self-test: %s\r\nLast OTA event: %s\r\n"
             "Update: ssh <user>@<board> ota < firmware-signed.bin, or "
             "`ota https://.../firmware-signed.bin`\r\n",
             selfTestPending.load() ? "PENDING (this image is not yet confirmed)"
                                    : "confirmed",
             last[0] != '\0' ? last : "none recorded");
  }
}

bool otaRollback(char* message, size_t messageSize) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (running == nullptr || other == nullptr || other->address == running->address) {
    snprintf(message, messageSize, "no other OTA slot");
    return false;
  }
  esp_app_desc_t description = {};
  if (esp_ota_get_partition_description(other, &description) != ESP_OK) {
    snprintf(message, messageSize, "%s holds no valid image", other->label);
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    snprintf(message, messageSize, "could not select %s: %s", other->label,
             esp_err_to_name(err));
    return false;
  }
  rememberOtaEvent("manual rollback from v%s to %s requested", FIRMWARE_VERSION,
                   other->label);
  snprintf(message, messageSize, "Next boot from %s (IDF %s). Rebooting.", other->label,
           description.idf_ver);
  return true;
}

void otaNoteServiceUp() { serviceUp = true; }

bool otaSelfTestPending() { return selfTestPending.load(); }

namespace {
void selfTestTask(void*) {
  const uint32_t startedMs = millis();
  while (true) {
    // Wi-Fi association is only required when the device is in normal
    // operation; in setup-portal mode the AP is the service.
    bool healthy = serviceUp.load() &&
                   (WiFi.status() == WL_CONNECTED || WiFi.getMode() == WIFI_MODE_APSTA);
#ifdef BASTION_TEST_BREAK_SELFTEST
    healthy = false;  // Test hook: simulate a firmware that never comes up.
#endif
    if (healthy) {
      const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      selfTestPending = false;
      rememberOtaEvent("v%s passed self-test after %lu s, image confirmed (%s)",
                       FIRMWARE_VERSION,
                       static_cast<unsigned long>((millis() - startedMs) / 1000UL),
                       esp_err_to_name(err));
      vTaskDelete(nullptr);
      return;
    }
    if (millis() - startedMs >= kSelfTestTimeoutMs) {
      rememberOtaEvent("v%s FAILED self-test (wifi=%d service=%d) - rolled back to the "
                       "previous image",
                       FIRMWARE_VERSION, WiFi.status() == WL_CONNECTED ? 1 : 0,
                       serviceUp.load() ? 1 : 0);
      delay(300);
      esp_ota_mark_app_invalid_rollback_and_reboot();
      // Only reached if rollback is impossible (no valid other image).
      eventLogf("OTA: rollback impossible, keeping this image");
      esp_ota_mark_app_valid_cancel_rollback();
      selfTestPending = false;
      vTaskDelete(nullptr);
      return;
    }
    delay(kSelfTestPollMs);
  }
}
}  // namespace

void otaSelfTestStart() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  selfTestPending = true;
  eventLogf("OTA: first boot of v%s from %s, self-test running (%lu s budget)",
            FIRMWARE_VERSION, running->label,
            static_cast<unsigned long>(kSelfTestTimeoutMs / 1000UL));
  if (xTaskCreatePinnedToCore(selfTestTask, "ota-selftest", 4096, nullptr, 1, nullptr,
                              1) != pdPASS) {
    // Without the task the image would never be confirmed and the next
    // reboot would roll back a working firmware; confirm it right away
    // instead and say so.
    eventLogf("OTA: could not start self-test task, confirming image immediately");
    esp_ota_mark_app_valid_cancel_rollback();
    selfTestPending = false;
  }
}
