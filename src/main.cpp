#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <math.h>

#include "device_config.h"
#include "main_pc.h"
#include "recovery_ssh.h"
#include "recovery_status.h"
#include "recovery_vpn.h"
#include "setup_portal.h"

namespace {
constexpr uint8_t kBootButtonPin = 0;

// Two hold tiers on the same button: 5 s reopens the setup portal pre-filled
// with the current settings, 10 s wipes everything back to factory state.
constexpr uint32_t kEditModeHoldMs = 5000;
constexpr uint32_t kFactoryResetHoldMs = 10000;
constexpr uint32_t kFactoryResetFlashMs = 1500;
constexpr uint32_t kHoldFeedbackStartMs = 3000;

constexpr uint32_t kReconnectIntervalMs = 10000;
constexpr uint32_t kInternetCheckIntervalMs = 10000;
constexpr uint32_t kInternetConnectTimeoutMs = 1000;

const IPAddress kInternetCheckHosts[] = {
    IPAddress(8, 8, 8, 8),
    IPAddress(8, 8, 4, 4),
};
constexpr uint16_t kDnsPort = 53;

enum class DeviceState {
  kSetup,
  kConnecting,
  kOnline,
  kNoInternet,
};

DeviceState deviceState = DeviceState::kConnecting;
uint32_t stateStartedMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastInternetCheckMs = 0;
uint32_t bootPressedMs = 0;
uint32_t bootHoldElapsedMs = 0;

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

const char* stateName(DeviceState state) {
  switch (state) {
    case DeviceState::kSetup:
      return "SETUP";
    case DeviceState::kConnecting:
      return "CONNECTING";
    case DeviceState::kOnline:
      return "ONLINE";
    case DeviceState::kNoInternet:
      return "NO_INTERNET";
  }
  return "UNKNOWN";
}

void setState(DeviceState nextState) {
  if (deviceState == nextState) {
    return;
  }

  deviceState = nextState;
  stateStartedMs = millis();
  Serial.printf("State: %s\n", stateName(deviceState));
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
// `startMs` into the cycle, `flashMs` on and `gapMs` off between them. Used
// to count out a small number (Wi-Fi+internet OK, or which VPN profile is
// active) as a number of blinks rather than a single on/off state.
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

  const uint32_t elapsed = millis() - stateStartedMs;

  switch (deviceState) {
    case DeviceState::kSetup:
      setLed(40, 20, 0);  // Solid yellow.
      break;

    case DeviceState::kConnecting: {
      // Smooth blue "breathing" pulse (sine envelope) instead of a hard
      // blink: this is a wait state, not an alert, so it reads calmer.
      constexpr uint32_t kBreathMs = 1800;
      const float phase =
          static_cast<float>(elapsed % kBreathMs) / static_cast<float>(kBreathMs);
      const float level = (sinf(phase * 2.0f * PI - PI / 2.0f) + 1.0f) * 0.5f;
      setLed(0, 0, static_cast<uint8_t>(level * 38.0f));
      break;
    }

    case DeviceState::kOnline: {
      // Heartbeat: two soft-edged green flashes confirm Wi-Fi + internet.
      // Violet flashes count out which VPN profile is active — one flash
      // for the primary, two for the secondary/failover profile — so a
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

    case DeviceState::kNoInternet:
      setLed((elapsed % 200U) < 100U ? 50 : 0, 0, 0);  // Fast red blink.
      break;
  }
}

bool hasInternetAccess() {
  for (const IPAddress& host : kInternetCheckHosts) {
    WiFiClient client;
    if (client.connect(host, kDnsPort, kInternetConnectTimeoutMs)) {
      client.stop();
      return true;
    }
    client.stop();
  }
  return false;
}

// Flashes the reset confirmation, wipes all provisioned settings, and
// restarts into the (now unprovisioned) setup portal. Never returns.
void doFactoryReset() {
  Serial.println("BOOT held 10s: factory reset.");
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
  const bool pressed = digitalRead(kBootButtonPin) == LOW;

  if (!pressed) {
    if (bootPressedMs != 0 && millis() - bootPressedMs >= kEditModeHoldMs) {
      Serial.println("BOOT held 5s: reopening setup portal.");
      portalRequestFlagSet();
      delay(200);
      ESP.restart();
    }
    bootPressedMs = 0;
    bootHoldElapsedMs = 0;
    return;
  }

  if (bootPressedMs == 0) {
    bootPressedMs = millis();
  }
  bootHoldElapsedMs = millis() - bootPressedMs;

  if (bootHoldElapsedMs >= kFactoryResetHoldMs) {
    doFactoryReset();  // Does not return.
  }
}

void handleNetworkState() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    setState(DeviceState::kConnecting);
    if (now - lastReconnectMs >= kReconnectIntervalMs) {
      Serial.println("Wi-Fi disconnected; retrying saved network.");
      WiFi.reconnect();
      lastReconnectMs = now;
    }
    return;
  }

  if (lastInternetCheckMs != 0 &&
      now - lastInternetCheckMs < kInternetCheckIntervalMs) {
    return;
  }

  lastInternetCheckMs = now;
  const bool online = hasInternetAccess();
  setState(online ? DeviceState::kOnline : DeviceState::kNoInternet);
  Serial.printf("Internet: %s | IP: %s | RSSI: %d dBm\n",
                online ? "available" : "unavailable",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(kBootButtonPin, INPUT_PULLUP);

  Serial.println();
  Serial.println("ESP32-S3 N16R8 starting");
  Serial.printf("Chip: %s rev %d | Flash: %u MB | PSRAM: %u MB\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getFlashChipSize() / (1024U * 1024U),
                ESP.getPsramSize() / (1024U * 1024U));

  deviceConfigLoad();
  const bool editRequested = portalRequestFlagTake();

  if (!deviceConfigPresent() || editRequested) {
    setupPortalStart();
    setState(DeviceState::kSetup);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(gDeviceConfig.wifiSsid,
             gDeviceConfig.wifiPassword[0] != '\0' ? gDeviceConfig.wifiPassword
                                                    : nullptr);
  Serial.printf("Connecting to Wi-Fi: %s\n", gDeviceConfig.wifiSsid);
  lastReconnectMs = millis();
  setState(DeviceState::kConnecting);

  startRecoverySshServer();
  startRecoveryVpn();
}

void loop() {
  handleBootButton();

  if (deviceState == DeviceState::kSetup) {
    setupPortalLoop();
  } else {
    handleNetworkState();
    mainPcMaintain();
  }

  renderStatusLed();
  delay(5);
}

const char* recoveryNetworkStateName() {
  return stateName(deviceState);
}

bool recoveryInternetAvailable() {
  return deviceState == DeviceState::kOnline;
}
