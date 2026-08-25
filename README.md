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

The first recovery-access slice is implemented: public-key-only SSH on port 22,
the terminal dashboard and built-in `help`, WoWLAN for the main PC, and a
restricted SSH bastion that permits only `192.168.1.200:22`. Connect with:

```sh
ssh -i ~/.ssh/id_rsa user@192.168.1.120
```

The ESP32 stores a unique SSH host key in SPIFFS. Only the public half of the
authorized user key is compiled into the firmware; the private key is not part
of this repository.

The recovery architecture, planned dual-WireGuard failover, Wake-on-Wireless,
terminal dashboard, and provisioning security are described in
[docs/recovery-access-architecture.md](docs/recovery-access-architecture.md).
The self-documenting SSH console, complete `help` output, command details, and
recovery examples are specified in
[docs/cli-reference.md](docs/cli-reference.md).

## LED status at a glance

| Color | Meaning |
|---|---|
| 🟡 Solid yellow | First-run Wi-Fi wizard is active |
| 🔵 Blue blinking | Connecting or reconnecting to Wi-Fi |
| 🟢🟢 Two quick green flashes | Wi-Fi and internet are working |
| 🔴 Fast red blinking | Wi-Fi is connected, but internet access is unavailable |
