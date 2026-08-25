#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

namespace {
constexpr char kSetupApName[] = "ESP32-S3-Setup";
constexpr uint32_t kBlinkIntervalMs = 500;

uint32_t lastBlinkMs = 0;
bool ledOn = false;

void setLed(uint8_t red, uint8_t green, uint8_t blue) {
  neopixelWrite(RGB_BUILTIN, red, green, blue);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("ESP32-S3 N16R8 connected");
  Serial.printf("Chip: %s, revision %d, cores: %d\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize() / (1024U * 1024U));
  Serial.printf("PSRAM: %u MB\n", ESP.getPsramSize() / (1024U * 1024U));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  setLed(40, 20, 0);  // Yellow: connecting or configuration portal active.
  Serial.printf("Connecting to saved Wi-Fi. If needed, join '%s'.\n", kSetupApName);

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);

  if (!wifiManager.autoConnect(kSetupApName)) {
    Serial.println("Wi-Fi setup timed out; restarting.");
    setLed(50, 0, 0);
    delay(3000);
    ESP.restart();
  }

  setLed(0, 50, 0);
  Serial.println("Wi-Fi connected");
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
  delay(1000);
  setLed(0, 0, 0);
}

void loop() {
  const uint32_t now = millis();
  if (now - lastBlinkMs < kBlinkIntervalMs) {
    return;
  }

  lastBlinkMs = now;
  ledOn = !ledOn;

  if (WiFi.status() == WL_CONNECTED) {
    setLed(0, 0, ledOn ? 35 : 0);  // Blue heartbeat.
  } else {
    setLed(ledOn ? 50 : 0, 0, 0);  // Red blinking: connection lost.
  }

  if (ledOn) {
    Serial.printf("Alive: %lu s | Wi-Fi: %s | IP: %s | RSSI: %d dBm\n",
                  now / 1000UL,
                  WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  }
}
