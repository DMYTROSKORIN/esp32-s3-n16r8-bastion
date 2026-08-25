# ESP32-S3 N16R8

PlatformIO project for an ESP32-S3 development board with 16 MB flash and
8 MB Octal PSRAM.

## Build and upload

Open the directory in VS Code with the PlatformIO extension, then use the
PlatformIO **Upload** action. The serial monitor runs at 115200 baud.

The firmware prints the detected chip, flash and PSRAM sizes and then connects
to Wi-Fi. On first boot, join the temporary `ESP32-S3-Setup` access point and
use its captive portal to select the target network. Credentials are stored in
the ESP32's internal NVS storage and are never committed to this repository.

The onboard RGB LED is yellow during Wi-Fi setup, briefly green after a
successful connection, blue blinking during normal operation, and red blinking
if the Wi-Fi connection is lost.
