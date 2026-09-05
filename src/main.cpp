#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <math.h>

#include "device_config.h"
#include "event_log.h"
#include "firmware_info.h"
#include "net_monitor.h"
#include "recovery_ssh.h"
#include "recovery_vpn.h"
#include "setup_portal.h"

namespace {
constexpr uint8_t kBootButtonPin = 0;
constexpr char kHostname[] = "esp32-bastion";

// Two hold tiers on the same button: 5 s reopens the setup portal pre-filled
// with the current settings, 10 s wipes everything back to factory state.
constexpr uint32_t kEditModeHoldMs = 5000;
constexpr uint32_t kFactoryResetHoldMs = 10000;
constexpr uint32_t kFactoryResetFlashMs = 1500;
constexpr uint32_t kHoldFeedbackStartMs = 3000;
// Mechanical contact bounce on a press or release can re-trigger a CHANGE
// interrupt several times within a couple of milliseconds; a 30 ms deadband
// between accepted edges is comfortably longer than any bounce train this
// class of button produces and comfortably shorter than a human's fastest
// deliberate press or release, so it can't be mistaken for one.
constexpr uint32_t kBootDebounceMs = 30;

// Task watchdog. The Arduino loop task and the net-monitor task feed it; both
// are designed so that no single iteration ever legitimately exceeds a few
// seconds (the longest is the portal's 15 s Wi-Fi trial, which feeds inside
// its wait loop). 60 s therefore only fires for a genuine hang - a wedged
// lwIP call, a deadlock, an infinite loop - and turns it into a clean reboot
// with `reset: task-watchdog` in the next dashboard instead of a dead board.
constexpr uint32_t kWatchdogTimeoutSeconds = 60;

enum class DeviceMode { kSetup, kRun };
DeviceMode deviceMode = DeviceMode::kRun;
NetState lastLedState = NetState::kConnecting;
uint32_t stateStartedMs = 0;
uint32_t bootHoldElapsedMs = 0;

// loop() must time a human's BOOT-button hold accurately enough to tell a
// 5 s press from a 10 s one; an interrupt latches the true edge timestamps
// the instant they happen, independent of how late loop() reads them.
portMUX_TYPE bootButtonMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool bootButtonPressed = false;
volatile uint32_t bootPressStartMs = 0;
volatile uint32_t bootReleaseMs = 0;
volatile bool bootReleasePending = false;
volatile uint32_t lastBootEdgeMs = 0;

void IRAM_ATTR bootButtonIsr() {
  const uint32_t now = millis();
  portENTER_CRITICAL_ISR(&bootButtonMux);
  // Debounce: an edge within kBootDebounceMs of the last *accepted* one is
  // bounce, not a real press/release - drop it instead of letting it
  // overwrite bootPressStartMs/bootReleaseMs with a bogus timestamp.
  if (now - lastBootEdgeMs >= kBootDebounceMs) {
    lastBootEdgeMs = now;
    if (digitalRead(kBootButtonPin) == LOW) {
      bootButtonPressed = true;
      bootPressStartMs = now;
    } else if (bootButtonPressed) {
      // Only a release that follows a press *observed by this firmware*
      // counts. GPIO0 is also driven by the USB-serial auto-reset circuit
      // (esptool, serial monitors), which routinely holds it low across a
      // reset; a bare release edge with no recorded press is that circuit
      // letting go, not a human finishing a 5 s hold.
      bootButtonPressed = false;
      bootReleaseMs = now;
      bootReleasePending = true;
    }
  }
  portEXIT_CRITICAL_ISR(&bootButtonMux);
}

void setLed(uint8_t red, uint8_t green, uint8_t blue) {
  static uint8_t previousRed = 255;
  static uint8_t previousGreen = 255;
  static uint8_t previousBlue = 255;

  if (red == previousRed && green == previousGreen && blue == previousBlue) {
    return;
  }

  previousRed = red;
  previousGreen = green;
  previousBlue = blue;
  neopixelWrite(RGB_BUILTIN, red, green, blue);
}

// Brightness of a short flash at `elapsedMs` into a window of `durationMs`,
// eased in and out over `edgeMs` at each end instead of snapping on/off.
uint8_t easedFlashLevel(uint32_t elapsedMs, uint32_t durationMs, uint8_t peak,
                        uint32_t edgeMs) {
  if (elapsedMs >= durationMs) {
    return 0;
  }
  if (elapsedMs < edgeMs) {
    return static_cast<uint8_t>(static_cast<uint32_t>(peak) * elapsedMs / edgeMs);
  }
  const uint32_t remaining = durationMs - elapsedMs;
  if (remaining < edgeMs) {
    return static_cast<uint8_t>(static_cast<uint32_t>(peak) * remaining / edgeMs);
  }
  return peak;
}

// Brightness for a burst of `count` identical soft-edged flashes starting at
// `startMs` into the cycle, `flashMs` on and `gapMs` off between them.
uint8_t burstLevel(uint32_t phase, uint32_t startMs, uint8_t count,
                   uint32_t flashMs, uint32_t gapMs, uint8_t peak,
                   uint32_t edgeMs) {
  if (count == 0 || phase < startMs) {
    return 0;
  }
  const uint32_t t = phase - startMs;
  const uint32_t stepMs = flashMs + gapMs;
  const uint32_t index = t / stepMs;
  if (index >= count) {
    return 0;
  }
  return easedFlashLevel(t - index * stepMs, flashMs, peak, edgeMs);
}

void renderStatusLed() {
  if (bootHoldElapsedMs >= kHoldFeedbackStartMs) {
    // Amber blink while BOOT is held, speeding up past the 5 s edit-mode
    // threshold to preview that a longer hold now means factory reset. Kept
    // as a hard blink on purpose: this is an alert/countdown, not a status.
    const uint32_t period = bootHoldElapsedMs >= kEditModeHoldMs ? 250 : 500;
    const bool on = (millis() % period) < period / 2;
    setLed(on ? 45 : 0, on ? 22 : 0, 0);
    return;
  }

  if (deviceMode == DeviceMode::kSetup) {
    setLed(40, 20, 0);  // Solid yellow.
    return;
  }

  const NetState state = netMonitorState();
  if (state != lastLedState) {
    lastLedState = state;
    stateStartedMs = millis();
  }
  const uint32_t elapsed = millis() - stateStartedMs;

  switch (state) {
    case NetState::kConnecting: {
      // Smooth blue "breathing" pulse (sine envelope) instead of a hard
      // blink: this is a wait state, not an alert, so it reads calmer.
      constexpr uint32_t kBreathMs = 1800;
      const float phase =
          static_cast<float>(elapsed % kBreathMs) / static_cast<float>(kBreathMs);
      const float level = (sinf(phase * 2.0f * PI - PI / 2.0f) + 1.0f) * 0.5f;
      setLed(0, 0, static_cast<uint8_t>(level * 38.0f));
      break;
    }

    case NetState::kOnline: {
      // Heartbeat: two soft-edged green flashes confirm Wi-Fi + internet.
      // Violet flashes count out which VPN profile is active - one flash
      // for the primary, two for the secondary/failover profile - so a
      // glance at the LED shows both "is the VPN up" and "which server".
      // No violet at all means the VPN is not up (or not configured).
      constexpr uint32_t kCycleMs = 3600;
      constexpr uint32_t kFlashMs = 100;
      constexpr uint32_t kEdgeMs = 20;
      constexpr uint32_t kVioletStartMs = 2600;
      constexpr uint32_t kVioletMs = 150;
      constexpr uint32_t kVioletGapMs = 150;
      const uint32_t phase = elapsed % kCycleMs;

      const uint8_t green = burstLevel(phase, 0, 2, kFlashMs, kFlashMs, 40, kEdgeMs);

      const uint8_t vpnProfile =
          recoveryVpnOnline() ? recoveryVpnActiveProfileNumber() : 0;
      const uint8_t violetRed = burstLevel(phase, kVioletStartMs, vpnProfile,
                                           kVioletMs, kVioletGapMs, 50, kEdgeMs);
      const uint8_t violetBlue = burstLevel(phase, kVioletStartMs, vpnProfile,
                                            kVioletMs, kVioletGapMs, 70, kEdgeMs);

      setLed(violetRed, green, violetBlue);
      break;
    }

    case NetState::kNoInternet:
      setLed((elapsed % 200U) < 100U ? 50 : 0, 0, 0);  // Fast red blink.
      break;
  }
}

// Flashes the reset confirmation, wipes all provisioned settings, and
// restarts into the (now unprovisioned) setup portal. Never returns.
void doFactoryReset() {
  eventLogf("BOOT held 10s: factory reset.");
  const uint32_t flashStartMs = millis();
  while (millis() - flashStartMs < kFactoryResetFlashMs) {
    setLed((millis() % 200U) < 100U ? 60 : 0, 0, 0);
    delay(10);
  }
  deviceConfigFactoryReset();
  delay(200);
  ESP.restart();
}

void handleBootButton() {
  // Snapshot the ISR-latched state; this is what makes the hold-duration
  // classification below correct regardless of how late this particular
  // loop() iteration is running.
  portENTER_CRITICAL(&bootButtonMux);
  const bool pressed = bootButtonPressed;
  const uint32_t pressStartMs = bootPressStartMs;
  const bool releasePending = bootReleasePending;
  const uint32_t releaseMs = bootReleaseMs;
  bootReleasePending = false;
  portEXIT_CRITICAL(&bootButtonMux);

  if (releasePending && releaseMs - pressStartMs >= kEditModeHoldMs) {
    eventLogf("BOOT held 5s: reopening setup portal.");
    portalRequestFlagSet();
    delay(200);
    ESP.restart();
  }

  if (!pressed) {
    bootHoldElapsedMs = 0;
    return;
  }

  bootHoldElapsedMs = millis() - pressStartMs;
  if (bootHoldElapsedMs >= kFactoryResetHoldMs) {
    doFactoryReset();  // Does not return.
  }
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
      return "task-watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep";
    default:
      return "other";
  }
}

const char* wifiDisconnectReasonName(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
      return "auth expired";
    case WIFI_REASON_AUTH_LEAVE:
      return "auth leave";
    case WIFI_REASON_ASSOC_EXPIRE:
      return "assoc expired";
    case WIFI_REASON_ASSOC_LEAVE:
      return "assoc leave";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND:
      return "AP not found";
    case WIFI_REASON_AUTH_FAIL:
      return "auth failed";
    case WIFI_REASON_ASSOC_FAIL:
      return "assoc failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL:
      return "connection failed";
    default:
      return "other";
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      eventLogf("Wi-Fi: got IP %s (gw %s, ch %d, %d dBm)",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      eventLogf("Wi-Fi: disconnected, reason %u (%s)",
                info.wifi_sta_disconnected.reason,
                wifiDisconnectReasonName(info.wifi_sta_disconnected.reason));
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      eventLogf("Wi-Fi: lost IP");
      break;
    default:
      break;
  }
}

const char* flashModeName(FlashMode_t mode) {
  switch (mode) {
    case FM_QIO:
      return "QIO";
    case FM_QOUT:
      return "QOUT";
    case FM_DIO:
      return "DIO";
    case FM_DOUT:
      return "DOUT";
    case FM_FAST_READ:
      return "FAST_READ";
    case FM_SLOW_READ:
      return "SLOW_READ";
    default:
      return "unknown";
  }
}

void logBootBanner(uint32_t bootCount) {
  eventLogf("ESP32-S3-N16R8 Bastion v%s starting (boot #%lu, reset: %s)",
            FIRMWARE_VERSION, static_cast<unsigned long>(bootCount),
            resetReasonName(esp_reset_reason()));
  eventLogf("Chip: %s rev %d @ %u MHz | flash %u MB %s @ %u MHz | PSRAM %u KB | SDK %s",
            ESP.getChipModel(), ESP.getChipRevision(),
            static_cast<unsigned>(ESP.getCpuFreqMHz()),
            static_cast<unsigned>(ESP.getFlashChipSize() / (1024U * 1024U)),
            flashModeName(ESP.getFlashChipMode()),
            static_cast<unsigned>(ESP.getFlashChipSpeed() / 1000000U),
            static_cast<unsigned>(ESP.getPsramSize() / 1024U), ESP.getSdkVersion());
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running != nullptr) {
    eventLogf("Partition: %s @ 0x%lx, %lu KB (app uses %lu KB)", running->label,
              static_cast<unsigned long>(running->address),
              static_cast<unsigned long>(running->size / 1024UL),
              static_cast<unsigned long>(ESP.getSketchSize() / 1024UL));
  }
  // This firmware is tuned for the N16R8 module. Warn loudly - but keep
  // running - when the hardware underneath is something else, because the
  // most likely symptom (libssh allocations failing without PSRAM) would
  // otherwise look like a random network bug.
  if (ESP.getPsramSize() < 4U * 1024U * 1024U) {
    eventLogf("WARNING: expected 8 MB PSRAM (N16R8), found %u KB - check "
              "board_build.arduino.memory_type",
              static_cast<unsigned>(ESP.getPsramSize() / 1024U));
  }
  if (ESP.getFlashChipSize() < 16U * 1024U * 1024U) {
    eventLogf("WARNING: expected 16 MB flash (N16R8), found %u MB",
              static_cast<unsigned>(ESP.getFlashChipSize() / (1024U * 1024U)));
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  eventLogInit();

  // The Arduino core already routes large mallocs to PSRAM (~8 MB, vs.
  // ~320 KB of internal RAM); lower the threshold further so libssh's much
  // smaller per-packet buffers land there too. Under sustained high-volume
  // traffic (a full-screen TUI redrawing constantly) those small internal
  // allocations fragment the tiny internal heap until one fails outright
  // with ENOMEM, killing the relay - confirmed live via
  // "ssh_socket_write: Out of memory" while htop (much lighter output)
  // never triggers it.
  heap_caps_malloc_extmem_enable(512);

  pinMode(kBootButtonPin, INPUT_PULLUP);
  // A LOW level already present at boot is deliberately *not* treated as a
  // press: on DevKitC-class boards GPIO0 is shared with the USB-serial
  // auto-reset circuit, and esptool or an attached serial monitor leaves it
  // low for seconds after a reset. Counting that as a hold used to reboot a
  // freshly flashed board into the setup portal (5 s) or, worse, factory
  // reset it (10 s). Holding BOOT through power-up is documented as
  // unsupported anyway (the ROM treats it as download mode). The hold timers
  // therefore start only from a falling edge seen after this point.
  bootButtonPressed = false;
  bootPressStartMs = 0;
  attachInterrupt(digitalPinToInterrupt(kBootButtonPin), bootButtonIsr, CHANGE);

  Serial.println();
  const uint32_t bootCount = deviceConfigBumpBootCount();
  logBootBanner(bootCount);

  // Watchdog on the loop task; the net-monitor task subscribes itself.
  esp_task_wdt_init(kWatchdogTimeoutSeconds, true);
  esp_task_wdt_add(nullptr);

  deviceConfigLoad();
  const bool editRequested = portalRequestFlagTake();

  WiFi.onEvent(onWifiEvent);
  WiFi.setHostname(kHostname);

  if (!deviceConfigPresent() || editRequested) {
    deviceMode = DeviceMode::kSetup;
    setupPortalStart();
    eventLogf("State: SETUP");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Modem power-save (the Arduino default) lets the radio doze between
  // beacons, which costs 50-300 ms of latency on the first packet after any
  // pause and visibly throttles sustained throughput. This board is
  // USB-powered and exists to be reachable instantly, so the radio stays
  // awake. Build with -DBASTION_WIFI_POWER_SAVE=1 to keep modem-sleep (e.g.
  // on a marginal power supply that browns out with the radio always on).
#if defined(BASTION_WIFI_POWER_SAVE) && BASTION_WIFI_POWER_SAVE
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  eventLogf("Wi-Fi: modem power-save enabled (build option)");
#else
  WiFi.setSleep(WIFI_PS_NONE);
#endif
  WiFi.begin(gDeviceConfig.wifiSsid,
             gDeviceConfig.wifiPassword[0] != '\0' ? gDeviceConfig.wifiPassword
                                                    : nullptr);
  eventLogf("Wi-Fi: connecting to %s", gDeviceConfig.wifiSsid);
  eventLogf("State: CONNECTING");

  netMonitorStart();
  startRecoverySshServer();
  startRecoveryVpn();
}

void loop() {
  esp_task_wdt_reset();
  handleBootButton();
  if (deviceMode == DeviceMode::kSetup) {
    setupPortalLoop();
  }
  renderStatusLed();
  delay(5);
}
