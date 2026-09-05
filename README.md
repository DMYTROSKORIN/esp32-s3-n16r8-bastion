<p align="center">
  <h1 align="center">ESP32-S3-N16R8 Bastion</h1>
  <p align="center"><strong>A self-provisioning ESP32-S3 emergency-access bastion for the night your main WireGuard tunnel goes dark.</strong></p>
</p>

<p align="center">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg"></a>
  <a href="CHANGELOG.md"><img alt="Version" src="https://img.shields.io/badge/firmware-v1.1.0-2ea44f.svg"></a>
  <a href=".github/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/DMYTROSKORIN/esp32-s3-n16r8-bastion/build.yml?branch=main&label=build"></a>
  <img alt="Platform" src="https://img.shields.io/badge/board-ESP32--S3--N16R8%20only-e07020.svg">
  <img alt="Framework" src="https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-00979d.svg">
  <img alt="WireGuard" src="https://img.shields.io/badge/VPN-WireGuard%20dual--profile-88171a.svg">
</p>

---

> [!IMPORTANT]
> This board is a deliberately narrow emergency door, not a general-purpose router. It permits
> `direct-tcpip` forwarding only to the one provisioned PC address and port, accepts SSH from a
> single authorized public key, and serves one console session at a time. The setup AP
> (`ESP32_SetUp`) is open/unauthenticated by design — see
> [Provisioning](#provisioning-wi-fi-pc-ssh-key-wireguard) for the reasoning and the trade-off.
> Review [docs/recovery-access-architecture.md](docs/recovery-access-architecture.md)
> before exposing this to a network you don't fully trust.

## What is this?

You normally reach your main PC through its own WireGuard client. That's one extra moving part —
one thing that can quietly stop working while you're away, with nothing left to SSH into and fix it.

This project turns a $10 ESP32-S3 dev board into an independent, always-on side door: it joins your
Wi-Fi on its own, holds up to two WireGuard profiles with automatic active/passive failover, and
exposes a hardened single-session SSH console. From there you can check on the PC, jump straight
into it over a `direct-tcpip` bastion channel, or send it a Wake-on-WLAN Magic Packet if it's asleep.
Nothing about the board depends on the PC's own tunnel being healthy — that's the point.

Everything is provisioned once, at runtime, through an on-device captive portal. No Wi-Fi password,
key, or `.conf` file is ever baked into the firmware or committed to a repository.

## Hardware: ESP32-S3-N16R8

This firmware targets exactly one module, the **ESP32-S3-N16R8** — an ESP32-S3 with **16 MB
quad-SPI flash** and **8 MB octal PSRAM**, on a DevKitC-1 class board (WS2812 RGB LED on GPIO 48,
`BOOT` button on GPIO 0). The build is tuned to that part rather than to the generic board profile:

| | N16R8 setting | Why it matters |
|---|---|---|
| Flash | 16 MB, QIO @ 80 MHz, `default_16MB.csv` | two 6.25 MB OTA slots + 3.4 MB SPIFFS; the board default (`default_8MB.csv`) would leave half the chip unused |
| PSRAM | 8 MB OPI (`memory_type = qio_opi`), allocations ≥ 512 B go there | libssh's per-packet buffers stop fragmenting the ~320 KB internal heap under btop-class traffic; the event journal lives there too |
| CPU | 240 MHz, both cores in use | Wi-Fi/lwIP/SSH on core 0; LED, network monitor and WireGuard on core 1 |
| Crypto | hardware AES, SHA, big-number unit | only AES ciphers are offered over SSH, all hardware-accelerated; chacha20 is not part of this libssh/mbedTLS build |
| Core | pioarduino 55.03.311 (Arduino 3.3.11 / ESP-IDF 5.5.5), IDF libraries rebuilt from source with `custom_sdkconfig` | lwIP TCP window 32 KB instead of the prebuilt core's 5760 B, SACK, 6 KB `tcpip_thread` stack, Wi-Fi/lwIP hot paths in IRAM, everything at `-O2` |

The boot log prints what it found (`flash 16 MB QIO @ 80 MHz | PSRAM 8189 KB`) and warns if the
chip is not an N16R8. Other ESP32-S3 variants (N8R2, N16R2, N8…) are **not supported** by this
configuration — see [docs/recovery-access-architecture.md](docs/recovery-access-architecture.md#target-hardware-esp32-s3-n16r8)
before attempting a port.

## Quick look

The SSH console opens straight into a live status dashboard — no separate monitoring needed:

```text
  ESP32 Recovery Gateway  ESP32-S3-N16R8 v1.1.0 · power-on
  ────────────────────────────────────────────────
  Device     ● ONLINE    uptime 0d 00:07:44
  Wi-Fi      ● ONLINE    MyHomeWiFi  -51 dBm  up 0d 00:07:39
  Internet   ● ONLINE
  WireGuard  ● ONLINE    profile-1  10.66.0.2  handshake 10s ago
  Main PC    ● ONLINE    192.168.1.200  ssh :22 open
  WoWLAN     ● STANDBY   AA:BB:CC:DD:EE:FF
  Memory       heap 196 KB (min 171 KB)  psram 8034/8189 KB  sessions 3
  ────────────────────────────────────────────────
  help commands   pc ssh how to reach the PC   pc wake wake it up
```

Beyond the dashboard the console offers `watch` (auto-refresh), `logs` (a secrets-free event
journal: Wi-Fi disconnect reasons, VPN transitions, every SSH login and relay close), `pc ping`,
`pc wake`, `vpn failover` / `vpn retry-primary`, and `reboot` — all with `help <command>`
generated from the same table that dispatches them. Arrow keys recall history.

A single onboard RGB LED mirrors the same state without a computer in reach at all — see
[LED status at a glance](#led-status-at-a-glance) below.

## Build and flash

Open the directory in VS Code with the PlatformIO extension, then use the PlatformIO **Upload**
action (or `pio run -t upload` / `pio run -t upload -t monitor`). The serial monitor runs at
115200 baud; PlatformIO auto-detects the upload port.

The default environment builds the ESP-IDF libraries from source with this project's `sdkconfig`
overrides (pioarduino "HybridCompile"), which is what lifts lwIP's TCP window from 5760 bytes to
32 KB. The **first build downloads the IDF toolchain and compiles the IDF once — expect 10-20
minutes**; later builds take seconds until `custom_sdkconfig` changes. `pio run -e
esp32-s3-n16r8-legacy` builds the same firmware on the stock prebuilt core in under a minute, with
the old 5760-byte window, if you need a quick fallback.

Handing this off to an AI coding agent (Claude Code, Codex CLI, etc.) instead? Point it at
[docs/agent-flashing.md](docs/agent-flashing.md) — it covers finding the port, building, flashing,
and reading back the boot log without an interactive terminal, and is explicit about the one thing
it can't do for you: the setup portal wizard needs a human on Wi-Fi.

The firmware is fully provisioned at runtime — nothing is baked into the build. On first boot (or
after a factory reset) it opens an open access point, `ESP32_SetUp`, with a lightweight captive
setup page covering Wi-Fi, the main PC's address, SSH access, and up to two WireGuard profiles.
Applying the form saves everything to NVS and reboots into normal operation. See
[docs/device-behavior.md](docs/device-behavior.md) for the full flow, the LED state machine, and the
`BOOT`-button hold tiers (5 s reopens the portal pre-filled with the current settings, 10 s wipes the
device back to factory state).

Connect from the LAN using the provisioned username and the ESP32's own address, assigned by your
router's DHCP (shown in the boot log, or in your router's lease list under the hostname
`esp32-bastion`) — not the main PC's address:

```sh
ssh user@192.168.1.120
```

Connect remotely as a VPN client of the primary server, or without any VPN client by jumping over
the VPN server's own sshd (the `pc ssh` console command always prints these with the currently
active addresses and username):

```sh
ssh user@10.66.0.2                                              # console
ssh -J user@10.66.0.2 user@192.168.1.200                       # bastion
ssh -J user@203.0.113.10:8326,user@10.66.0.2 user@192.168.1.200
```

The ESP32 stores a unique SSH host key in SPIFFS, generated on first boot and wiped only by a
factory reset. The single-session SSH server is hardened against stalled or vanished clients
(key-exchange/auth timeouts, TCP keepalive, idle timeouts), and the bastion relay handles partial
writes and correctly propagates client-side EOF while draining the PC's remaining response, so full
interactive sessions — including TUI apps like `btop` — work through the jump chain.

### Throughput and stability of the SSH path

The relay is built for sustained TUI traffic, not just keystrokes:

- **AES only, in hardware.** The server pins `aes128/256-gcm@openssh.com` and `aes128/256-ctr`
  with SHA-2 MACs and curve25519/ECDH key exchange. All of them run on the S3's hardware AES block;
  chacha20-poly1305 is not compiled into this libssh/mbedTLS build at all, so OpenSSH clients land on
  AES-GCM without any configuration.
- **`select()`-driven relay** on the SSH socket and a raw lwIP socket to the PC: no millisecond
  polling, 8 KB internal-RAM buffers per direction, `TCP_NODELAY` and keepalive on both sockets.
- **Back-pressure instead of buffering.** Channel writes are blocking so the client's SSH window
  throttles the PC through the relay; a write that makes no progress for 30 s ends the relay with a
  journaled reason instead of exhausting memory (the original `btop` failure mode).
- **Radio always on.** Wi-Fi modem power-save is disabled: measured idle RTT to the board drops from
  ~70 ms (9-140 ms jitter) to ~6 ms, which is the difference between a laggy and a crisp console
  (`-DBASTION_WIFI_POWER_SAVE=1` restores modem-sleep).

- **32 KB TCP window.** The prebuilt Arduino core fixes lwIP's window at 5760 bytes, which capped a
  connection at `5760 B / RTT` (~160 KB/s on the LAN, under 200 KB/s over WireGuard). Since 1.1.0 the
  IDF libraries are rebuilt with a 32 KB window, SACK and matching Wi-Fi buffers: on the same bench
  the relay went from 129-165 to 234-246 KB/s downstream and from 83-89 to 187-270 KB/s upstream.

Details, the measurement method and the remaining limits are in
[docs/recovery-access-architecture.md](docs/recovery-access-architecture.md#ssh-throughput-on-the-esp32-s3).

## Provisioning (Wi-Fi, PC, SSH key, WireGuard)

Everything is entered once through the `ESP32_SetUp` portal — no files are placed in this repository
or on the build machine:

- **Wi-Fi**: pick a network from the on-device scan (or enter one manually) and its password.
- **Main PC**: IP address and SSH port. Used for the bastion's allowed destination, the `pc status`
  check, and as the Wake-on-WLAN target — its MAC address is learned automatically from ARP the
  first time it is seen online, no manual entry.
- **SSH access**: username and public key (ed25519/RSA/ECDSA), loaded from a file.
- **WireGuard**: a primary profile (required for VPN) and an optional secondary one for failover,
  each a standard wg-quick client `.conf` loaded from a file. Either profile can be removed with its
  own button — removing the primary also clears the secondary, since a secondary without a primary
  is invalid. This firmware requires `AllowedIPs` to include `0.0.0.0/0` (full tunnel).

The browser performs a lightweight format check as soon as a file is picked (key type, `.conf`
section headers). On **Apply**, the firmware performs the authoritative validation before saving
anything — a real trial import through libssh for the key, and full field-by-field parsing for the
WireGuard profile (endpoint, `AllowedIPs`, `MTU` 576–1420).

Public keys and WireGuard profiles are file-upload only, with no text field to paste into. This is
deliberate: on iOS the portal opens inside Apple's Captive Network Assistant, a restricted
mini-browser that loses all form state the moment you leave it to copy text from elsewhere — the
native file picker, unlike an app switch, stays inside that mini-browser and survives.

Applying the Wi-Fi fields first makes a real connection attempt (up to 15 s, with `ESP32_SetUp`
staying up throughout) before anything is saved — a wrong password or an out-of-range/5 GHz-only
network is reported inline instead of being discovered only after a reboot.

The firmware syncs the clock over NTP, starts the primary profile, verifies a recent handshake and
TCP reachability through the tunnel every 10 seconds, and changes profile only after three
consecutive failed checks. The SSH console provides `vpn status`, `vpn failover`, and
`vpn retry-primary`. Every call into the WireGuard library is executed on lwIP's own thread, which
is what lwIP's threading model requires and what keeps the failover path free of data races.

The board also supervises itself: a 60 s task watchdog turns any hang into a clean reboot (reported
as `task-watchdog` on the next dashboard), and ten minutes without Wi-Fi association triggers a
restart to recover a wedged radio. See [docs/device-behavior.md](docs/device-behavior.md#self-supervision-watchdog-and-automatic-restart).

The full recovery architecture, the failover algorithm, Wake-on-Wireless, SSH-server hardening, the
reliability fixes applied so far, and a troubleshooting checklist are described in
[docs/recovery-access-architecture.md](docs/recovery-access-architecture.md). The SSH console, its
real `help` output, command details, and recovery examples are documented in
[docs/cli-reference.md](docs/cli-reference.md).

## LED status at a glance

| Color | Meaning |
|---|---|
| 🟡 Solid yellow | Setup portal is active (first run, or `BOOT` held 5 s) |
| 🟠 Amber blink | `BOOT` is being held (speeds up past the 5 s threshold) |
| 🔴 Fast red blink, 1.5 s | Factory reset confirmed at the 10 s `BOOT` hold |
| 🔵 Blue breathing (smooth) | Connecting or reconnecting to Wi-Fi |
| 🟢🟢 Two green flashes, no violet | Wi-Fi + internet OK, but the VPN tunnel is not up |
| 🟢🟢 + 🟣 one violet flash | Wi-Fi + internet OK, VPN up on the **primary** profile |
| 🟢🟢 + 🟣🟣 two violet flashes | Wi-Fi + internet OK, VPN up on the **secondary/failover** profile |
| 🔴 Fast red blinking (steady) | Wi-Fi is connected, but internet access is unavailable |

The number of violet flashes after the green pair doubles as the WireGuard profile number, so a
glance at the LED shows whether failover has already happened without opening the SSH console.
Flash edges are eased in software (~20 ms) for a cleaner look; the alert patterns (red, amber, the
reset flash) stay hard-edged on purpose. See [docs/device-behavior.md](docs/device-behavior.md) for
exact timings.

## Why

A VPN client running on the machine you're trying to reach is a single point of failure by
construction: the day it breaks is the day you needed it most. The board provides an independent
recovery path whose Wi-Fi, WireGuard, and SSH state does not depend on the PC at all. Its narrow
command set and forwarding policy deliberately limit both the failure surface and the attack
surface. This project is that device: small enough to leave plugged into a spare USB port
indefinitely, cheap enough to be an easy insurance policy against exactly that day.

## Project

| | |
|---|---|
| Board | **ESP32-S3-N16R8** (16 MB QIO flash, 8 MB OPI PSRAM), DevKitC-1 class — the only supported variant |
| Firmware | v1.1.0 — see [CHANGELOG.md](CHANGELOG.md) |
| Framework | Arduino core 3.3.11 / ESP-IDF 5.5.5 via [pioarduino](https://github.com/pioarduino/platform-espressif32) 55.03.311, IDF rebuilt with `custom_sdkconfig`, `-O2`; legacy env on `espressif32 @ 7.0.1` (Arduino 2.0.17) |
| CI | [GitHub Actions](.github/workflows/build.yml) builds every push and uploads the binaries |
| SSH server | [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) (Arduino port of libssh) |
| WireGuard client | [esphome-libs/wireguard](https://github.com/esphome-libs/wireguard) (`esp_wireguard`/`wireguardif`) |
| Docs | [Recovery architecture](docs/recovery-access-architecture.md) · [Device behavior & LED](docs/device-behavior.md) · [CLI reference](docs/cli-reference.md) · [Flashing for AI agents](docs/agent-flashing.md) |

## Contributing

This is a personal-infrastructure project first, but issues and pull requests are welcome, in
particular around:

- A/B OTA updates with automatic rollback (the 16 MB partition table already has both slots)
- Hardening for production deployment (Secure Boot V2, Flash Encryption, per-device signing)

Please keep the runtime-provisioning model intact — no secrets should ever need to be baked into
the firmware or committed to this repository.

## Acknowledgements

This project builds on:

- [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) by ewpa, a port of [libssh](https://www.libssh.org/) — mostly LGPL-2.1-or-later, with some BSD-2-Clause parts
- [esphome-libs/wireguard](https://github.com/esphome-libs/wireguard) — BSD-3-Clause
- [esphome-libs/libsodium](https://github.com/esphome-libs/libsodium), pulled in as a WireGuard dependency — MIT (the vendored upstream [libsodium](https://github.com/jedisct1/libsodium) itself is ISC)

These are fetched by PlatformIO at build time (`lib_deps` in `platformio.ini`) and are not vendored
into this repository; each keeps its own upstream license.

## License

This project's own code is licensed under the [MIT License](LICENSE).

---

<sub>Contact: <a href="https://github.com/DMYTROSKORIN">@DMYTROSKORIN</a></sub>
