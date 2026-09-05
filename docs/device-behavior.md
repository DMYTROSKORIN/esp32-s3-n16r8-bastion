# LED indication and device behavior

This document describes the **ESP32-S3-N16R8** firmware (16 MB flash, 8 MB
PSRAM, DevKitC-1 class board with a WS2812 RGB LED on GPIO 48 and the `BOOT`
button on GPIO 0), built on Arduino core 3.3 / ESP-IDF 5.5 with a custom
sdkconfig since 1.1.0. Behaviour is identical on the legacy prebuilt-core
build except for network throughput (see the architecture document).

A single onboard RGB LED shows everything needed to bring the ESP32-S3 up
and diagnose it without a computer or a serial monitor. Color reports the
current state, and the flash pattern distinguishes normal operation from a
problem.

## Quick color reference

| Indication | State | What it means | What to do |
|---|---|---|---|
| 🟡 **Solid yellow** | `SETUP` | Device is unprovisioned (or `BOOT` was held 5 s) and has opened the setup portal | Connect to the open `ESP32_SetUp` network and fill in the form at `192.168.4.1` |
| 🟠 **Amber blink** | `BOOT` held | `BOOT` button is held; blinking speeds up once the hold reaches 5 seconds | Release at 5 s to reopen setup, hold to 10 s for a full factory reset |
| 🔴 **Fast red, 1.5 s, then reboot** | reset confirmed | `BOOT` held 10 s — full factory reset accepted | Nothing to do; the device reboots on its own |
| 🔵 **Smooth blue breathing** | `CONNECTING` | ESP32 is connecting or reconnecting to the saved Wi-Fi network | Usually just wait; if blue persists for a long time, check the router or reprovision the network |
| 🟢🟢 **Two green flashes** | `ONLINE`, VPN not up | Wi-Fi is connected and internet access is confirmed, but the WireGuard tunnel is currently not up | If a VPN is configured and expected to be up, check `vpn status` over the SSH console |
| 🟢🟢 + 🟣 **Two green + one violet** | `ONLINE`, VPN on the primary profile | Everything is fine: internet is up, the **first (primary)** WireGuard profile is active | No action needed |
| 🟢🟢 + 🟣🟣 **Two green + two violet** | `ONLINE`, VPN on the failover profile | Everything is fine, but the device is running on the **second (failover)** profile — a failover occurred | Worth checking the primary VPN server |
| 🔴 **Continuous fast red blinking** | `NO_INTERNET` | Wi-Fi is connected, but the internet-reachability check is failing | Check the router's internet connection, DNS, network restrictions, or firewall |

### What normal operation looks like

```text
🟢 100 ms → pause 100 ms → 🟢 100 ms → pause ~2.3 s → 🟣 150 ms [→ pause 150 ms → 🟣 150 ms] → pause → repeat (~3.6 s cycle)
```

The double green flash signals "Wi-Fi and internet are healthy." The number
of violet flashes after the pause encodes VPN state: none — the tunnel is
not up, one — the primary profile is active, two — the failover profile is
active (a failover occurred). This way a single glance at the LED shows both
that the device is online and which VPN server it is currently using,
without opening the SSH console.

The edges of every flash (green and violet) are softened slightly in
software (~20 ms rise/fall) — this isn't a hardware feature of the LED, it's
an intermediate-brightness calculation on every iteration of the main loop
(every ~5 ms; since 1.0.0 nothing that can block on the network runs on the
loop task, so the animation no longer stutters during reachability probes).
The blink pattern itself stays crisp and easy to
read; the smoothing only affects the short flash edges and the full smooth
breathing in the `CONNECTING` state. Alert signals (fast red blinking, amber
while `BOOT` is held, the reset flash) deliberately stay hard-edged — the
sharpness is part of the meaning there.

## First boot

If the device hasn't been provisioned yet (no saved configuration in NVS),
it automatically enters setup mode:

1. The LED lights up **solid yellow**.
2. The ESP32 creates an open Wi-Fi network, `ESP32_SetUp`, with no password.
3. The user connects to it from a phone or a computer — the portal is
   designed primarily for setup from a phone.
4. The setup portal usually opens automatically (captive portal). If it
   doesn't, open `http://192.168.4.1` in a browser.
5. A single page covers everything: the Wi-Fi network and password, the main
   PC's IP and SSH port, the username, and the public SSH key plus one or two
   WireGuard profiles — **file upload only** (a "Choose file…" button with
   inline validation), with no manual paste/typing. This is a deliberate
   choice: on iOS the portal opens inside a stripped-down mini-browser
   (Captive Network Assistant) that loses all form state on any switch to
   another app — and pasting from the clipboard requires exactly that kind
   of switch. File upload works through the system picker, which never
   leaves the mini-browser.
6. On **Apply & restart**, the ESP32 first makes a real connection attempt
   to the given Wi-Fi network (while the `ESP32_SetUp` access point keeps
   running), and only on success does it validate the remaining fields,
   atomically save the configuration to NVS, and reboot. A wrong password or
   an unreachable network is reported as an inline error on the
   password/network field rather than a surprise after reboot.
7. After rebooting, the device starts **breathing blue smoothly** while it
   connects to the chosen network.
8. Once connected and the internet check succeeds, a sequence of **two green
   flashes** begins.

The portal has no timeout and stays reachable until setup is completed. The
ESP32-S3 only supports 2.4 GHz Wi-Fi. WireGuard is optional — without it the
device runs in a reduced mode (LAN SSH works, VPN and the bastion are
unavailable).

## Reprovisioning or a full reset

The `BOOT` button has two hold thresholds:

1. Power on the device and wait for the main firmware to start.
2. Press and hold `BOOT`. After ~3 seconds the LED starts **blinking amber**
   — a signal that the button press was recognized.
   - **Release between 5 and 10 seconds:** the ESP32 reboots and reopens the
     setup portal, already **pre-filled with the current values** (except
     the password and secrets — their fields show that a value is saved and
     only accept a new one if you type one in).
   - **Keep holding to 10 seconds:** blinking speeds up, and at the 10 s
     mark the LED switches to **fast red** for about 1.5 seconds — with no
     need to release the button — and a full factory reset happens: Wi-Fi,
     PC address, SSH key and username, both WireGuard profiles, and the
     unique SSH host key in SPIFFS are permanently deleted.
3. After the reset, the device reboots and opens the empty setup portal, as
   on first boot.

Do not hold `BOOT` while powering on or during a hardware reset — the ESP32
may instead enter the system firmware-download mode. For the same reason a
`BOOT` level that is already low when the firmware starts is ignored: the
hold timers only start from a press that begins after boot. This also stops
the USB-serial auto-reset circuit (esptool, an attached serial monitor), which
holds GPIO0 low for a while after a reset, from being mistaken for a 5 s or
10 s hold.

## State machine

```text
No saved configuration
          │
          ▼
   🟡 SETUP / ESP32_SetUp ──► Apply & restart
          │                        │
          │                        ▼
          │                     reboot
          │                        │
          ▼                        ▼
   🔵 CONNECTING ◄─────────────── Wi-Fi lost ─────────────────┐
          │                            ▲                       │
          ├── Wi-Fi + internet ──► 🟢 ONLINE                   │
          │                            │                       │
          └── Wi-Fi, no internet ► 🔴 NO_INTERNET ─────────────┘
                                       │         (Wi-Fi lost from here too)
                                       └── internet restored ──► 🟢 ONLINE

BOOT held 3-5 s → 🟠 blinking   │ released at 5-10 s → reboot → 🟡 SETUP (pre-filled)
BOOT held 10 s   → 🔴 1.5 s     → factory reset → reboot → 🟡 SETUP (empty)
```

## How internet access is detected

Being connected to the router doesn't yet guarantee internet access, so
Wi-Fi and internet are shown as separate states.

Every 10 seconds the firmware tries to open a TCP connection to Google
Public DNS on port 53:

- primary server: `8.8.8.8`;
- fallback server: `8.8.4.4`.

A successful TCP connection to at least one server is enough for `ONLINE`
(no DNS query is sent or answered — this is purely a reachability probe). If
both servers are unreachable, the ESP32 switches to `NO_INTERNET`. Once
access is restored, the device automatically returns to `ONLINE`.

## Self-supervision: watchdog and automatic restart

The device is meant to sit unattended for months, so it watches itself:

- **Task watchdog, 60 s.** The main loop and the network-monitor task feed a
  hardware-backed task watchdog. Both are written so that no single
  iteration legitimately takes more than a few seconds (the longest, the
  15 s Wi-Fi trial in the setup portal, feeds the watchdog from inside its
  wait loop). If either stops feeding it, the chip reboots and the next
  dashboard shows `task-watchdog` as the reset reason; `logs` then shows
  what preceded it.
- **10 minutes without Wi-Fi → restart.** The LED keeps breathing blue
  while the firmware retries the saved network every 10 s; if the radio has
  not associated for 10 minutes straight, the device restarts to re-run
  every initialisation path from scratch. Wi-Fi being up but the *internet*
  being down (fast red blinking) never triggers a restart.
- **Boot counter and reset reason.** Every boot increments a counter in NVS
  and logs the reason for the previous reset (`power-on`, `software`,
  `panic`, `task-watchdog`, `brownout`, ...). A rising count of `brownout`
  resets points at the power supply, not the firmware.
- **Wi-Fi radio stays awake.** Modem power-save is disabled for the lowest
  latency and steady throughput (the board is USB-powered). Builds that must
  run from a marginal supply can restore modem-sleep with
  `-DBASTION_WIFI_POWER_SAVE=1`; the boot log then says so.

## Recovery VPN and SSH console

Three more subsystems run alongside the LED indication (details in
[recovery-access-architecture.md](recovery-access-architecture.md)):

- The **network monitor** task owns Wi-Fi reconnects, the internet
  reachability probe and learning the PC's MAC address, so the main loop
  only animates the LED and times the `BOOT` button.
- The **SSH server** starts once connected to Wi-Fi and listens on port 22
  on all interfaces — it is reachable from the local network and through the
  WireGuard tunnel. Login is by authorized public key only.
- The **WireGuard task** brings up a tunnel to the primary VPN server after
  NTP time sync and holds it as the default route. Every 10 seconds it runs
  a health check: a recent handshake (≤ 180 s) plus a TCP check against
  `1.1.1.1:53` / `8.8.8.8:53` **through the tunnel**. After three
  consecutive failures the board automatically switches to the failover
  server.
- The serial log (115200 baud) and the in-memory journal (`logs` over SSH)
  record the same events with uptime stamps: Wi-Fi disconnect reasons,
  `Net:` state changes, VPN transitions (`WireGuard: profile 1 is online`,
  `health check failed (n/3)`, `failing over to …`), SSH logins and relay
  statistics — the first place to look when diagnosing anything.

Note: the "internet for LED indication" check (`8.8.8.8:53`, `8.8.4.4:53`)
is a separate mechanism from the VPN health check; once the tunnel is up,
both checks run through WireGuard.

## Settings storage and IP address

- The entire configuration (Wi-Fi, PC IP/port, username and public SSH key,
  up to two WireGuard profiles) is stored as one block in the ESP32's
  `recovery` NVS namespace — see `include/device_config.h`.
- None of it lives in the source code, PlatformIO settings, or Git.
- The main PC's MAC address isn't entered manually — it's learned
  automatically via ARP the first time the PC appears on the network, and is
  also cached in NVS.
- The same namespace holds a boot counter and the one-shot "reopen the
  portal" flag set by a 5 s `BOOT` hold; a factory reset clears all of it.
- The firmware gets its IP address via DHCP.
- A stable local address, if needed, is pinned on the router via a DHCP
  reservation rather than hardcoded into the firmware.

## Quick diagnostics

- **Yellow won't turn off:** setup isn't finished yet. Open `ESP32_SetUp`
  and the `192.168.4.1` portal.
- **Blue blinks for a long time:** the saved network is unreachable or its
  password changed. Check the router, or hold `BOOT` for 5 seconds to
  reprovision.
- **Continuous fast red blinking:** the ESP32 sees the router but can't
  reach the internet. Check the internet uplink and network rules for
  `8.8.8.8:53` and `8.8.4.4:53`.
- **Double green:** the device is operating normally.
- **Device rebooted on its own:** open the SSH console; the dashboard header
  shows the reset reason and `logs` the last events before it. `brownout`
  means the USB supply dipped; `task-watchdog` means a task hung and the
  watchdog recovered the board.
- **Console feels laggy / btop stutters over VPN:** expected to a degree —
  the TCP window of the ESP32's network stack caps a single connection at
  roughly 5760 bytes per round-trip (see "SSH throughput" in the
  architecture document). Over the LAN the session should be snappy; if it
  is not, check RSSI with `net status`.
- **WoWLAN doesn't wake the PC:** the console shows `NO MAC` on the `WoWLAN`
  line as long as the PC hasn't been seen on the network yet since
  setup/reset — power it on manually once, and the MAC will be remembered
  from then on.
