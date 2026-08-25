# Device behavior

## First-run wizard

When no Wi-Fi credentials are stored, the device starts an open access point
named `ESP32_SetUp`. The captive portal lets the user select a 2.4 GHz Wi-Fi
network and enter its password. The portal remains available until setup is
completed; it has no timeout.

Wi-Fi credentials are stored in the ESP32's NVS flash. They are not present in
the source tree, firmware configuration, or Git history.

The firmware uses DHCP. A stable address, if required, must be assigned with a
DHCP reservation on the router.

## State machine

| State | Meaning | RGB LED pattern |
|---|---|---|
| `SETUP` | The first-run portal is active | Solid yellow |
| `CONNECTING` | Connecting or reconnecting to saved Wi-Fi | Blue, 500 ms on / 500 ms off |
| `ONLINE` | Wi-Fi and internet access are available | Two 100 ms green flashes, then a 1 s pause |
| `NO_INTERNET` | Wi-Fi is connected but internet checks fail | Fast red, 100 ms on / 100 ms off |

State transitions:

```text
no saved Wi-Fi ──> SETUP ──credentials saved──> CONNECTING
saved Wi-Fi ──────────────────────────────────> CONNECTING
CONNECTING ──Wi-Fi + internet─────────────────> ONLINE
CONNECTING ──Wi-Fi, no internet───────────────> NO_INTERNET
ONLINE/NO_INTERNET ──Wi-Fi lost───────────────> CONNECTING
any normal state ──BOOT held 5 s──> erase Wi-Fi, restart ──> SETUP
```

## Internet check

While Wi-Fi is connected, the firmware attempts a TCP connection to Google
Public DNS on port 53 every 10 seconds. It checks `8.8.8.8`, followed by
`8.8.4.4`; either successful connection counts as internet access.

## Changing the Wi-Fi network

With the firmware running normally, hold the `BOOT` button for five seconds.
The device erases only its saved Wi-Fi configuration and restarts in `SETUP`
mode. Do not hold `BOOT` while resetting or powering on, because that selects
the ESP32 ROM download mode instead.
