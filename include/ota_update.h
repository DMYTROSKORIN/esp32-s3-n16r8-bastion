#pragma once

#include <stddef.h>
#include <stdint.h>

// A/B firmware update.
//
// Signed image format (produced by scripts/ota_sign.py, what CI attaches to
// every release as firmware-signed.bin):
//
//   [ ESP-IDF app image ][ 32-byte version, NUL-padded ][ 64-byte Ed25519 sig ]
//
// The signature covers SHA-256(image || version) and is verified with the
// release public key compiled into the firmware (include/ota_public_key.h).
// The image is streamed straight into the inactive OTA slot while the hash
// is computed, so no staging buffer is needed; nothing is committed until the
// signature and ESP-IDF's own image check both pass.
//
// After a reboot into the new slot the bootloader marks it PENDING_VERIFY.
// otaSelfTestStart() confirms the image (Wi-Fi associated + SSH server
// listening within kSelfTestTimeoutMs) or lets the bootloader roll back.

constexpr size_t kOtaVersionFieldSize = 32;
constexpr size_t kOtaSignatureSize = 64;
constexpr size_t kOtaTrailerSize = kOtaVersionFieldSize + kOtaSignatureSize;

// Progress/status text sink: `line` is a complete, NUL-terminated line
// without newline. Used to echo progress to the SSH channel.
typedef void (*OtaReportFn)(const char* line, void* userData);

struct OtaResult {
  bool ok;
  char message[160];   // Human-readable outcome (success or the exact failure).
  char version[kOtaVersionFieldSize + 1];  // Version from the trailer when known.
  const char* targetLabel;  // "app0"/"app1" the image went to, or nullptr.
  size_t imageBytes;
};

// Streaming receiver. Create, call begin(), feed() repeatedly, then finish().
class OtaSink {
 public:
  OtaSink();
  ~OtaSink();
  bool begin(OtaResult& result);
  bool feed(const uint8_t* data, size_t length, OtaResult& result);
  // Verifies the trailer signature and the image, activates the slot.
  // The caller reboots afterwards.
  bool finish(OtaResult& result);
  // Releases the OTA handle without committing (error paths).
  void abort();
  size_t bytesReceived() const { return received_; }

 private:
  struct Impl;
  Impl* impl_;
  size_t received_ = 0;
};

// Downloads `url` (https:// only, public CA bundle) and applies it.
// `report` receives progress lines. Returns result.ok; reboot is left to the
// caller so it can print the outcome first.
bool otaFromUrl(const char* url, OtaReportFn report, void* userData, OtaResult& result);

// Fills a multi-line human-readable slot summary ("ota status").
void otaDescribeSlots(char* out, size_t outSize);

// Switches the boot partition to the other slot if it holds a valid image.
// Returns false with a reason in `message` otherwise; caller reboots on true.
bool otaRollback(char* message, size_t messageSize);

// Call once early in setup(): if the running image is PENDING_VERIFY, starts
// the self-test that confirms or rolls back. Harmless otherwise.
void otaSelfTestStart();

// True while the running image is still PENDING_VERIFY.
bool otaSelfTestPending();

// Set once the device is serving its purpose: the SSH server is accepting
// connections, or the setup portal is up (a firmware whose config layout
// changed legitimately lands there). One of the self-test criteria.
void otaNoteServiceUp();
