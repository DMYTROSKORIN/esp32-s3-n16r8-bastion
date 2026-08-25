#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

namespace {
constexpr char kSetupApName[] = "ESP32_SetUp";
constexpr uint8_t kBootButtonPin = 0;

constexpr uint32_t kBootHoldMs = 5000;
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

WiFiManager wifiManager;
DeviceState deviceState = DeviceState::kConnecting;
uint32_t stateStartedMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastInternetCheckMs = 0;
uint32_t bootPressedMs = 0;

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

void renderStatusLed() {
  const uint32_t elapsed = millis() - stateStartedMs;

  switch (deviceState) {
    case DeviceState::kSetup:
      setLed(40, 20, 0);  // Solid yellow.
      break;

    case DeviceState::kConnecting:
      setLed(0, 0, (elapsed % 1000U) < 500U ? 35 : 0);  // Blue blink.
      break;

    case DeviceState::kOnline: {
      // Two 100 ms green flashes followed by a one-second pause.
      const uint32_t phase = elapsed % 1300U;
      const bool on = phase < 100U || (phase >= 200U && phase < 300U);
      setLed(0, on ? 40 : 0, 0);
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

void startSetupPortal() {
  Serial.printf("Starting open setup network: %s\n", kSetupApName);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.startConfigPortal(kSetupApName);
  setState(DeviceState::kSetup);
}

void startSavedWiFiConnection() {
  Serial.printf("Connecting to saved Wi-Fi: %s\n",
                wifiManager.getWiFiSSID(true).c_str());
  WiFi.begin();
  lastReconnectMs = millis();
  setState(DeviceState::kConnecting);
}

void handleBootButton() {
  const bool pressed = digitalRead(kBootButtonPin) == LOW;

  if (!pressed) {
    bootPressedMs = 0;
    return;
  }

  if (bootPressedMs == 0) {
    bootPressedMs = millis();
    return;
  }

  if (millis() - bootPressedMs < kBootHoldMs) {
    return;
  }

  Serial.println("BOOT held for 5 seconds: clearing Wi-Fi settings.");
  setLed(50, 20, 0);
  wifiManager.resetSettings();
  delay(500);
  ESP.restart();
}

void handleSetup() {
  wifiManager.process();

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  wifiManager.stopConfigPortal();
  Serial.printf("Wi-Fi connected | SSID: %s | IP: %s | RSSI: %d dBm\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  lastInternetCheckMs = 0;
  setState(DeviceState::kConnecting);
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
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  Serial.println();
  Serial.println("ESP32-S3 N16R8 starting");
  Serial.printf("Chip: %s rev %d | Flash: %u MB | PSRAM: %u MB\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getFlashChipSize() / (1024U * 1024U),
                ESP.getPsramSize() / (1024U * 1024U));

  wifiManager.setDebugOutput(false);

  if (wifiManager.getWiFiIsSaved()) {
    startSavedWiFiConnection();
  } else {
    startSetupPortal();
  }
}

void loop() {
  handleBootButton();

  if (deviceState == DeviceState::kSetup) {
    handleSetup();
  } else {
    handleNetworkState();
  }

  renderStatusLed();
  delay(5);
}
