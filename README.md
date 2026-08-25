# ESP32-S3 N16R8

PlatformIO project for an ESP32-S3 development board with 16 MB flash and
8 MB Octal PSRAM.

## Build and upload

Open the directory in VS Code with the PlatformIO extension, then use the
PlatformIO **Upload** action. The serial monitor runs at 115200 baud.

The initial firmware prints the detected chip, flash and PSRAM sizes once per
boot, followed by a one-second heartbeat.
