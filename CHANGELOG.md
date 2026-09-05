# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-09-05

First versioned release. The firmware is now explicitly tuned for, and only
supported on, the **ESP32-S3-N16R8** module (16 MB QIO flash, 8 MB OPI PSRAM).

### Added

- Firmware version (`FIRMWARE_VERSION`, set in `platformio.ini`) shown in the
  boot banner, the dashboard header, `version` and the portal's `/api/config`.
- In-memory event journal (256 lines, PSRAM, secrets-free) and the `logs [n]`
  console command. Wi-Fi events now log the driver's disconnect reason.
- Console commands `watch` (auto-refreshing dashboard), `pc ping [count]`
  (ICMP via `esp_ping`), `reboot` (with `reboot yes` confirmation).
- Command registry: `help` and `help <command>` are generated from the same
  table that dispatches commands. Per-command help now exists for every command.
- Line editor: command history (↑/↓, 8 entries), `Ctrl+U`, `Ctrl+L`, and
  proper parsing of terminal escape sequences (arrow keys used to leak `[A`
  into the command line).
- Task watchdog (60 s) on the loop and net-monitor tasks; a hang now ends in a
  clean reboot reported as `task-watchdog` instead of a dead board.
- Automatic restart after 10 minutes without Wi-Fi association (radio recovery).
- Boot counter in NVS; boot banner reports flash mode/speed, PSRAM size, the
  running OTA partition and warns when the hardware is not an N16R8.
- SSH journal entries for every login (peer address, negotiated cipher and
  KEX), authentication failure, rejected `direct-tcpip` destination and relay
  close (duration, bytes, average rate).
- Dashboard: Wi-Fi association uptime, heap low-water mark, PSRAM total,
  session counter. `net status` adds BSSID, channel and gateway.
- `pc ssh` prints the LAN jump command as well.
- GitHub Actions workflow that builds the firmware on every push/PR.
- This changelog.

### Changed

- **Partition table** `default_16MB.csv` (two 6.25 MB OTA slots, 3.4 MB SPIFFS,
  coredump) instead of the board default `default_8MB.csv`, which left half of
  the N16R8's flash unused. NVS stays at the same offset, so provisioning
  survives the upgrade; SPIFFS moves, so the SSH host key is regenerated once
  (expect a one-time `known_hosts` warning).
- PlatformIO platform pinned to `espressif32 @ 7.0.1` (Arduino core 2.0.17 /
  ESP-IDF 4.4.7); flash and CPU clocks set explicitly (QIO 80 MHz, 240 MHz).
- Compiled with `-O2` instead of `-Os`: libssh, WireGuard and the relay are
  built from source, and flash is not a constraint on this module.
- **SSH algorithms** pinned explicitly: AES-GCM/AES-CTR ciphers (all on the
  ESP32-S3's hardware AES block), SHA-2 MACs, curve25519/ECDH-P256 KEX. The
  libssh build already lacked chacha20-poly1305 (mbedTLS CHACHAPOLY is not
  compiled into the Arduino core), so nothing changes for clients; the pin
  keeps CBC/3DES and DH group-exchange from ever being offered.
- **Bastion relay** rewritten around `select()` on the SSH socket and a raw
  lwIP socket to the PC: no more 1 ms polling loop, 8 KB internal-RAM
  buffers per direction (was 1 KB via `WiFiClient`), `TCP_NODELAY` and
  keepalive on both sockets, non-blocking connect with a 2 s deadline.
- `TCP_NODELAY` on the client-facing SSH socket (interactive latency).
- SSH task stack reduced from 32 KB to 20 KB (measured peak use ~6.5 KB).
- Wi-Fi modem power-save disabled by default (`WiFi.setSleep(WIFI_PS_NONE)`);
  `-DBASTION_WIFI_POWER_SAVE=1` restores the previous behaviour.
- Wi-Fi/internet supervision and MAC learning moved from `loop()` into a
  dedicated `net-monitor` task; the LED animation and BOOT-button timing no
  longer stall for up to ~2.4 s during TCP probes.
- **WireGuard library calls are marshalled onto lwIP's `tcpip_thread`** via
  `tcpip_callback()`. `esp_wireguard` manipulates the netif list, raw UDP
  PCBs, DNS and `sys_timeout()` directly; calling those from the VPN task on
  core 1 was a genuine (if rare) data race with lwIP on core 0. This closes
  the first item under "Known accepted risks" in the architecture document.
- Portal: error JSON buffer grown to 1 KB and made truncation-safe; the 15 s
  Wi-Fi trial feeds the watchdog; `/api/config` reports `fw` and `board`.
- All diagnostics go through the journal (`eventLogf`) instead of bare
  `Serial.print*`, with an uptime stamp.
- Device hostname set to `esp32-bastion` (shows up in the router's DHCP list).

### Fixed

- Noisy `nvs_get_blob len fail: cfg NOT_FOUND` error on every unprovisioned
  boot (`Preferences::isKey()` is checked first).
- `version` reported PSRAM in whole megabytes, rounding 8 MB down to "7 MB";
  sizes are now printed in KB.
- Arrow keys and other escape sequences corrupting the console input line.
- A `BOOT` level already low at startup was counted as a press. The USB-serial
  auto-reset circuit holds GPIO0 low for seconds after esptool or a serial
  monitor resets the board, which rebooted a freshly flashed device into the
  setup portal (5 s) or could factory-reset it (10 s). Hold timers now start
  only from a press observed after boot.

### Measured (LAN bench, 2026-09-05, RSSI -73 dBm, same board, same client)

"Before" is the last pre-1.0.0 commit (e8a9388) flashed onto the same board.
Throughput is wall-clock for a 2 MB transfer through `ssh -J`, including the
~1.5 s of key exchange, averaged over two runs.

| Metric | Before (e8a9388) | 1.0.0 |
|---|---|---|
| ICMP RTT to the board, idle | 74 ms avg (7-205 ms), modem-sleep | **6.7 ms avg (3-18 ms)**, radio awake |
| Relay PC → client | 105-123 KB/s | **129-165 KB/s** |
| Relay client → PC | 43-44 KB/s | **83-89 KB/s** |
| Free internal heap, idle, VPN up | 202 KB | 189 KB (journal, net-monitor task, 2 x 8 KB relay buffers) |
| Console login + 7 commands | - | 2.9 s wall clock |
| btop soak through `ssh -J`, 200x50, 500 ms refresh | - | 341 s, 14.6 MB relayed, clean exit, heap flat |
| SSH task stack peak | - | ~6.5 KB (of 20 KB) |

AES-GCM and AES-CTR measured within noise of each other. The relay figures
are bounded by lwIP's fixed 5760-byte TCP window on a 2.4 GHz link with
~7 ms loaded RTT, not by CPU or cipher (see below).

### Known limits

- lwIP's TCP receive/send window in the prebuilt Arduino core is fixed at
  5760 bytes and cannot be raised from the sketch. Over a high-latency path
  (through WireGuard to a remote client) a single TCP connection is bounded to
  roughly `5760 B / RTT` — about 190 KB/s at 30 ms, 95 KB/s at 60 ms. This is
  the ceiling for the SSH session itself; it is unaffected by cipher choice.
  Raising it requires a custom framework build (see the architecture document).
