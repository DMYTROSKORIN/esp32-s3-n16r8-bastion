#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("ESP32-S3 N16R8 connected");
  Serial.printf("Chip: %s, revision %d, cores: %d\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize() / (1024U * 1024U));
  Serial.printf("PSRAM: %u MB\n", ESP.getPsramSize() / (1024U * 1024U));
}

void loop() {
  Serial.printf("Alive: %lu s\n", millis() / 1000UL);
  delay(1000);
}
