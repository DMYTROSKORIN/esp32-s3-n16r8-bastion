# ESP32-S3 N16R8

PlatformIO project for an ESP32-S3 development board with 16 MB flash and
8 MB Octal PSRAM.

## Build and upload

Open the directory in VS Code with the PlatformIO extension, then use the
PlatformIO **Upload** action. The serial monitor runs at 115200 baud.

The firmware provides a first-run Wi-Fi wizard. If no network is saved, join
the open `ESP32_SetUp` access point and use its captive portal to select the
target 2.4 GHz network. Credentials are stored in the ESP32's internal NVS and
are never committed to this repository. Hold `BOOT` for five seconds while the
firmware is running to erase the saved network and reopen the wizard.

See [docs/device-behavior.md](docs/device-behavior.md) for the state machine,
LED patterns, internet-check behavior, and recovery procedure.
