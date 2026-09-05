# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [1.2.0] - 2026-09-06

### Added

- **A/B over-the-air updates** with signature verification and automatic
  rollback (`src/ota_update.cpp`). Two ways in:
  - `ssh <user>@<board> ota < firmware-signed.bin` streams the image over the
    SSH connection (works over the LAN, through WireGuard and via `-J`);
  - `ota https://.../firmware-signed.bin` in the console makes the board
    download it itself (HTTPS only, public CA bundle, redirects followed, so
    GitHub Releases URLs work directly).
  The image is written straight into the inactive slot while SHA-256 is
  computed; nothing is activated until the Ed25519 release signature (public
  key in `include/ota_public_key.h`) and ESP-IDF's own image validation pass.
  After the reboot the new image must bring up Wi-Fi and the SSH server (or
  the setup portal) within 2 minutes to be confirmed; otherwise the bootloader
  rolls back to the previous slot. Settings, WireGuard profiles, the learned
  MAC and the SSH host key are untouched.
- `ota status` (slots, states, self-test, last OTA event kept in NVS across
  the reboot) and `ota rollback yes` (boot the other slot's image).
- `scripts/ota_sign.py`: `keygen` / `pubkey --header` / `sign` / `verify`.
  Signed image = ESP app image + 32-byte version field + 64-byte Ed25519
  signature over SHA-256(image || version). CI signs every build with the
  `OTA_SIGNING_KEY` repository secret and attaches `firmware-signed.bin` to
  the GitHub Release on tag pushes.
- **Non-interactive commands**: `ssh <user>@<board> <command>` runs one
  console command and exits with status 0 (`ssh user@board logs 50`,
  `ssh user@board pc wake`), which makes the console scriptable.
- Dashboard redesign: rules span the terminal width (from the SSH `pty-req`),
  the version is highlighted on its own, addresses/SSID/profile are bright
  instead of grey, RSSI and free heap are colour-graded, the reset reason is
  red when it was a panic/watchdog/brownout, and a `Firmware` row shows the
  running slot and self-test state.

### Fixed

- The Arduino core confirmed a freshly booted OTA image unconditionally at
  start-up; `verifyRollbackLater()` is now overridden so the self-test decides.

### Tested on the bench

- Tampered image (one byte flipped): rejected, `signature check FAILED`,
  nothing written. Random data: rejected on the first chunk (bad magic).
- Valid image over SSH: 1.7 MB in 16-19 s including verification, reboot into
  the other slot, self-test passed after 2 s, state `valid`.
- Deliberately broken image (self-test sabotaged): booted, failed the 120 s
  self-test, bootloader rolled back to the previous image; `ota status`
  reports the failure from NVS.
- Download from a GitHub Releases URL from the console: 302 redirect
  followed, 1.7 MB fetched over TLS and written in ~20 s, verified, rebooted,
  self-test passed after 2 s.

## [1.1.0] - 2026-09-05

### Changed

- **Custom-built core.** The default environment now uses the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform
  (release 55.03.311: Arduino core 3.3.11 on ESP-IDF 5.5.5) in its
  "HybridCompile" mode: `custom_sdkconfig` in `platformio.ini` rebuilds the
  IDF libraries from source with this project's settings. The first build
  compiles the whole IDF (~6 min on 16 cores, longer on CI); later builds
  reuse the cached libraries. The board is the exact `esp32-s3-devkitc1-n16r8`
  definition shipped by pioarduino.
- **lwIP TCP window 5760 → 32768 bytes** (`CONFIG_LWIP_TCP_WND_DEFAULT`,
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`), receive mailbox 6 → 32 segments, SACK
  on, `tcpip_thread` stack 2560 → 6144 bytes, Wi-Fi driver buffers sized to
  match (16 static / 64 dynamic RX, 64 dynamic TX, BA window 32), Wi-Fi and
  lwIP hot paths in IRAM, IDF libraries at `-O2`. This removes the throughput
  ceiling documented in 1.0.0.
- Watchdog set-up uses the ESP-IDF 5 API (`esp_task_wdt_reconfigure`) when
  built on IDF 5; the IDF 4 path is kept for the legacy environment.
- `esp32-s3-n16r8-legacy` environment keeps the 1.0.0 build (official
  PlatformIO `espressif32 @ 7.0.1`, prebuilt core, 5760-byte window) as an
  escape hatch and for A/B comparisons.
- LibSSH-ESP32's reference X25519 and libsodium's (via WireGuard) both define
  `crypto_scalarmult`; the IDF 5 link tolerates the duplicate
  (`-Wl,--allow-multiple-definition`, both are equivalent implementations).

### Measured (same LAN bench as 1.0.0, same board, RSSI -73 dBm)

| Metric | 1.0.0 (prebuilt core, 5760 B window) | 1.1.0 (custom core, 32 KB window) |
|---|---|---|
| Relay PC → client, 2 MB via `ssh -J` | 129-165 KB/s | **205-250 KB/s** |
| Relay client → PC, 2 MB via `ssh -J` | 83-89 KB/s | **126-270 KB/s** |
| btop soak via `ssh -J`, 200x50, **100 ms** refresh | - | 243 s / 57 MB / 235 KB/s and 183 s / 43 MB / 237 KB/s sustained, clean exits |
| ICMP RTT idle | 6.7 ms | 6.0 ms |
| Free internal heap, idle, VPN up | 189 KB | 121 KB (min 94 KB during the soak) |

Both directions are now limited by the Wi-Fi link and CPU rather than by the
TCP window; through WireGuard (30-60 ms RTT) the gain is proportionally larger,
because 32 KB in flight covers a whole btop redraw in one round-trip. The
lower internal heap is the ESP-IDF 5 baseline plus the larger static Wi-Fi
buffers; the dynamic Wi-Fi/lwIP pools live in PSRAM
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, off in pioarduino's defaults). The
spread in the client → PC figures is Wi-Fi run-to-run variance at -73 dBm.

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
