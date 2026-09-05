# Recovery access architecture

## Purpose

The ESP32-S3 acts as an independent emergency bastion for restoring access to
the main Linux PC when the PC's own WireGuard tunnel stops working.

Target scenario:

```text
Remote laptop/phone
          │
          │ WireGuard through one of two VPN servers
          ▼
      ESP32-S3
          │
          │ local network 192.168.1.0/24
          ▼
Linux PC 192.168.1.200:22
          │
          └── diagnose and restart the main WireGuard tunnel
```

## Target hardware: ESP32-S3-N16R8

The firmware is built for, tested on and only supported on the
**ESP32-S3-N16R8** module - an ESP32-S3 with 16 MB quad-SPI flash and 8 MB
octal-SPI PSRAM (DevKitC-1 class boards with the onboard WS2812 RGB LED on
GPIO 48 and the `BOOT` button on GPIO 0). Everything below that touches
memory or flash layout assumes exactly that part:

| Resource | How it is used |
|---|---|
| 16 MB flash, QIO @ 80 MHz | `default_16MB.csv`: two 6.25 MB OTA app slots (`app0`/`app1`), 3.4 MB SPIFFS (SSH host key), 64 KB coredump, NVS at 0x9000 (provisioned settings, learned MAC, boot counter). The stock `default_8MB.csv` of the DevKitC-1 board definition would waste half the chip. |
| 8 MB PSRAM, OPI @ 80 MHz | `board_build.arduino.memory_type = qio_opi`; `heap_caps_malloc_extmem_enable(512)` sends every allocation of 512 B or more to PSRAM, which is what keeps libssh's per-packet buffers from fragmenting the ~320 KB of internal RAM under sustained traffic. The event journal (28 KB) lives there too. Internal RAM is reserved for what must be fast: task stacks, lwIP/Wi-Fi buffers, the two 8 KB relay buffers. |
| CPU 240 MHz, dual core | Core 0: Wi-Fi driver, lwIP `tcpip_thread`, SSH server task. Core 1: Arduino loop (LED, button, watchdog feed), `net-monitor`, `recovery-vpn`. |
| Custom-built core | Arduino 3.3.11 / ESP-IDF 5.5.5 (pioarduino 55.03.311) with the IDF libraries rebuilt from source: lwIP TCP window and send buffer 32 KB, receive mailbox 32, SACK, `tcpip_thread` stack 6 KB, 16/64 static/dynamic Wi-Fi RX buffers, 16 static TX, BA window 32, dynamic Wi-Fi/lwIP pools in PSRAM, Wi-Fi/lwIP hot paths in IRAM, `-O2`. See `custom_sdkconfig` in `platformio.ini`; the legacy environment documents what the stock core gives instead. |
| Hardware AES / SHA / MPI | mbedTLS uses the S3's accelerators for AES (all SSH ciphers offered), SHA-256/512 (MACs, KEX hashes) and big-number math (ECDH). chacha20-poly1305 is not part of this libssh/mbedTLS build (the Arduino core omits mbedTLS's CHACHAPOLY module) and is not offered - see "SSH throughput" below. |

The boot banner prints what it actually found (`flash 16 MB QIO @ 80 MHz |
PSRAM 8192 KB | SDK 5.5.5`) and logs a `WARNING` if the PSRAM or flash size does not
match the N16R8, because the most likely symptom of a mismatch (libssh
allocations failing) would otherwise look like a random network bug.

## Status indication (RGB LED)

Besides the SSH console, the device reports its state through a single
built-in RGB LED — the only diagnostic available without a computer. The full
description with exact timings is in
[device-behavior.md](device-behavior.md); here is a quick-reference summary:

| Indication | State |
|---|---|
| 🟡 Solid yellow | Setup portal is open (`ESP32_SetUp`) |
| 🟠 Amber blink | `BOOT` button is being held |
| 🔴 Fast red, 1.5 s | Full factory reset confirmed |
| 🔵 Smooth blue breathing | Connecting to Wi-Fi |
| 🟢🟢 two green flashes, no violet flash | Wi-Fi and internet are up, VPN tunnel is not |
| 🟢🟢 + 🟣 one flash | VPN up on the **primary** profile (`profile-1`) |
| 🟢🟢 + 🟣🟣 two flashes | VPN up on the **secondary** profile (`profile-2`) — a failover happened |
| 🔴 Fast red (steady) | Wi-Fi is up, internet is not |

The number of violet flashes after the green pair equals the number of the
active WireGuard profile — so a single glance at the LED shows whether the
VPN is up and whether a failover has already happened, without opening the
SSH console.

## VPN topology (example of this device's current provisioning)

Both profiles are loaded through the setup portal (see "Provisioning through
the setup portal" below) — the firmware is not tied to specific servers;
slot 1 and slot 2 can point at any WireGuard server the user owns. Below is
an example of how this particular device happens to be provisioned right
now (values are illustrative):

| Role | Endpoint | Board profile | Board tunnel IP |
|---|---|---|---|
| Primary | `203.0.113.10:51820` | `profile-1` | `10.66.0.2` |
| Secondary | `198.51.100.10:51820` | `profile-2` | `10.66.0.3` |

The VPN server's own sshd port used in the jump-without-a-VPN-client example
below (`8326`) is a per-profile field set in the setup portal alongside each
profile's `.conf` upload (`vpnServerSshPort` in `WgProfileConfig`), since
wg-quick's `.conf` format has no such field. It defaults to `22` if left
blank, and the two profiles can use different ports.

Key topology properties, true for any pair of WireGuard servers with no
routing between them:

- **There is no routing between the VPN servers.** The board is only
  reachable on whichever server it's currently connected to. The client
  (laptop/phone) must connect to the same server, or the board's tunnel IP
  is unreachable — this looks like "the VPN came up but there's no
  connection" even though both tunnels are healthy.
- **The board's tunnel IP changes on failover** (in the example above,
  `10.66.0.2` on the primary, `10.66.0.3` on the secondary; actual values
  depend on the loaded `.conf` files). The console's `pc ssh` command always
  prints ready-to-use commands with the current address.
- Profile 1 (primary) should be whichever server the user's client devices
  connect to by default.
- Both servers need `ip_forward=1`, an nftables forward chain with
  `iifname wg0 accept` plus MSS clamping, and masquerade for traffic from
  `wg0` — otherwise the board's outbound internet traffic through the tunnel
  won't work.

### Connection commands

From a device connected as a VPN client to the primary server:

```bash
ssh user@10.66.0.2                          # board console
ssh -J user@10.66.0.2 user@192.168.1.200   # straight to the main PC
```

Without a VPN client (jumping through the VPN server's sshd):

```bash
ssh -J user@203.0.113.10:8326,user@10.66.0.2 user@192.168.1.200
```

After a failover to the secondary server, the server, tunnel IP, and jump
port (if the two profiles were given different ones) all change in all
commands: `198.51.100.10`, `10.66.0.3`, and the secondary profile's own
configured SSH port.

Notes on the SSH client:

- On first connection the client asks about the host key twice: first the
  VPN server's key (the jump host), then the board's key. The board's key is
  unique, generated on first boot, and stored in SPIFFS — it survives
  reflashing (with one exception: upgrading from a pre-1.0.0 build moves the
  SPIFFS partition, so the key is regenerated exactly once).
- OpenSSH 10+ prints a warning about the missing post-quantum key exchange:
  LibSSH-ESP32 doesn't support it yet. The risk is minimal — the SSH traffic
  already runs inside WireGuard with a `PresharedKey`, which gives the
  tunnel itself post-quantum protection. The warning can be silenced in
  `~/.ssh/config`:

  ```
  Host 10.66.0.2 10.66.0.3 192.168.1.120
      WarnWeakCrypto no-pq-kex
  ```

The ESP32 must not grant arbitrary access to the whole local network. It only
allows a fixed set of emergency actions and a TCP tunnel to the PC address
configured through the portal (currently `192.168.1.200:22`).

## Verified environment

| Parameter | Value |
|---|---|
| Main PC | Linux |
| Main PC IP | `192.168.1.200/24` (example; set through the portal) |
| Emergency service | SSH, TCP `22` |
| PC connectivity | Wi-Fi (WoWLAN needs an adapter and driver that support Magic Packet) |
| MAC for wake-up | learned automatically via ARP, never entered manually |
| Local network broadcast | `192.168.1.255` (example; taken from the current Wi-Fi subnet) |
| Primary VPN | WireGuard |
| VPN servers | Two, active/passive failover |

The ESP32's IP is assigned by DHCP. A stable address is pinned on the router.

## SSH bastion and terminal interface

The SSH server starts once Wi-Fi is connected and listens on `0.0.0.0:22` —
it is reachable both from the local network (`192.168.1.120`) and through the
WireGuard tunnel at the board's tunnel IP. Only public-key authentication for
the user configured through the portal is allowed (currently `user`);
password authentication is disabled.

Right after login the user sees a summary screen (ANSI colours: cyan
labels, a green/yellow/red ● and state word per line, the facts you act on
in bright white, rules as wide as the client's terminal):

```text
  ESP32 Recovery Gateway   v1.2.0   ESP32-S3-N16R8  • reset: power-on
  ──────────────────────────────────────────────────────────────────────────────
  Device     ● ONLINE     up 0d 00:07:44
  Wi-Fi      ● ONLINE     MyHomeWiFi  -51 dBm  ch 6  ip 192.168.1.120  up 0d 00:07:39
  Internet   ● ONLINE
  WireGuard  ● ONLINE     profile-1  10.66.0.2  handshake 10 s ago
  Main PC    ● ONLINE     192.168.1.200:22  ssh open
  WoWLAN     ● STANDBY    aa:bb:cc:dd:ee:ff
  Memory     ● OK         heap 121 KB (min 114)  psram 8117/8192 KB
  Firmware   ● CONFIRMED  slot app0  sessions 3
  ──────────────────────────────────────────────────────────────────────────────
  help commands   pc ssh reach the PC   pc wake wake it up   ota update
```

The header carries the firmware version (highlighted), the board and the
reason for the last reset (red when it was a panic, watchdog or brownout).
The `VPN errors` line only appears after consecutive health-check failures.
`WoWLAN` shows `READY` when the main PC is offline and can be woken, and
`STANDBY` once the PC is already online. `Memory` grades the free internal
heap by its low-water mark since boot. `Firmware` shows the running OTA slot
and whether a freshly installed image is still in its self-test. The exact
line semantics are in [cli-reference.md](cli-reference.md).

### SSH server robustness

The server handles one session at a time, so every blocking operation is
bounded — otherwise a single abandoned client (stuck on a host-key prompt, a
phone that dropped off the network, a port scanner) would permanently block
the emergency console until the board was power-cycled. That exact failure
was found and fixed on 2026-08-25.

Implemented limits (`src/recovery_ssh.cpp`):

| Mechanism | Value | Protects against |
|---|---|---|
| `SSH_OPTIONS_TIMEOUT` per session | 30 s | client opened the TCP connection but stays silent during key exchange / auth |
| TCP keepalive on the socket | 30 s idle, 5 s interval, 4 probes | client vanished without a FIN (mobile network, dropped tunnel) |
| Deadline from accept to shell/relay start | 60 s | data trickling in, stretching out the pre-session phase |
| Auth message limit | 16 | endless authentication brute-forcing |
| Interactive console idle timeout | 10 min | a session left open and forgotten |
| PC TCP tunnel idle timeout | 10 min with no traffic | a stuck forwarded channel |
| Relay write stall | 30 s without progress in either direction | a client or PC that stopped reading but keeps the TCP connection up |
| Non-blocking connect to the PC | 2 s | the PC's sshd is down: the channel is refused promptly instead of hanging the session |

After any error or timeout the server is guaranteed to go back to `accept`
and take the next session.

### SSH throughput on the ESP32-S3

Where the bytes of a `btop` session actually go, and what bounds them:

```text
PC ──TCP──▶ ESP32 lwIP rx (32 KB window) ──▶ relay buffer (8 KB) ──▶ libssh encrypt (AES, hw)
        ──▶ lwIP tx (32 KB send buffer) ──▶ Wi-Fi ──▶ [WireGuard: chacha20 in software] ──▶ client
```

(5760 B for both buffers on the legacy prebuilt-core environment.)

What the firmware does to keep that path fast and, above all, stable:

- **Cipher selection.** The server pins `aes128-gcm@openssh.com`,
  `aes256-gcm@openssh.com`, `aes128-ctr`, `aes256-ctr` with SHA-2 MACs, all of
  which run on the S3's hardware AES block. `chacha20-poly1305@openssh.com`,
  the first choice of every stock OpenSSH client, is not available in this
  libssh/mbedTLS build (the Arduino core does not compile mbedTLS's CHACHAPOLY
  module; a client forcing it gets `no matching cipher found`), so clients
  land on AES-GCM with no configuration. The explicit list keeps that true
  across library upgrades and rules out CBC/3DES. KEX is limited to
  curve25519 / ECDH-P256 (DH group-exchange is a multi-second modexp on first
  connect).
- **`select()`-driven relay.** `relayDirectTcpip()` blocks in `select()` on
  the SSH socket and a raw lwIP socket to the PC instead of polling both ends
  every millisecond. A burst is moved in up to 8 KB slices from buffers allocated
  once in internal RAM. Both sockets have `TCP_NODELAY` (interactive
  keystrokes and small redraws never wait for Nagle) and TCP keepalive.
- **Blocking channel writes.** `ssh_channel_write()` runs in blocking mode so
  the client's SSH window and TCP flow control back-pressure the PC through
  the relay, instead of libssh buffering output unboundedly (the original
  `ssh_socket_write: Out of memory` failure under btop). A write that makes
  no progress for 30 s ends the relay with a journaled reason.
- **PSRAM for libssh.** Allocations of 512 B and up go to PSRAM; the tiny
  internal heap no longer fragments under a high packet rate.
- **Radio awake.** Wi-Fi modem power-save is off (`WIFI_PS_NONE`). Measured
  on the bench (2026-09-05): idle ICMP RTT to the board 5.6 ms average with
  the radio awake versus 70 ms average (9-140 ms) with modem-sleep, because
  every packet after a pause waits for the next beacon. Under sustained load
  both modes converge (~7-8 ms), so this is about console responsiveness, not
  bulk throughput. `-DBASTION_WIFI_POWER_SAVE=1` restores modem-sleep for
  marginal power supplies.
- **`-O2`.** libssh, the WireGuard stack and the relay are compiled from
  source; flash is not a constraint on this module.

- **32 KB TCP window (1.1.0).** lwIP in the *prebuilt* Arduino core has
  `TCP_WND = TCP_SND_BUF = 5760` bytes, fixed at compile time and not
  adjustable per socket (`ESP_PER_SOC_TCP_WND = 0`), so a connection could
  never have more than 5760 bytes in flight and was bounded by `5760 B / RTT`.
  Since 1.1.0 the default environment rebuilds the IDF libraries
  (pioarduino HybridCompile, `custom_sdkconfig` in `platformio.ini`) with a
  32 KB window and send buffer, a 32-segment receive mailbox, SACK and
  Wi-Fi driver buffers sized to match. The table shows what the window alone
  allows; the link and CPU are the next limits.

| Path | Typical RTT | Bound with 5760 B (1.0.0 / legacy env) | Bound with 32 KB (1.1.0) |
|---|---|---|---|
| Same LAN | 2-7 ms | 0.8-2.9 MB/s | CPU/link-bound |
| Through WireGuard, nearby server | 20-30 ms | 190-290 KB/s | 1.1-1.6 MB/s |
| Through WireGuard, far server / mobile | 60-100 ms | 58-96 KB/s | 330-550 KB/s |

Measured on the LAN bench (2026-09-05, RSSI -73 dBm, loaded RTT ~7 ms, 2 MB
through `ssh -J` including key exchange, same board and client throughout):

| Firmware | PC → client | client → PC | Idle RTT |
|---|---|---|---|
| e8a9388 (pre-1.0.0) | 105-123 KB/s | 43-44 KB/s | 74 ms (modem-sleep) |
| 1.0.0, prebuilt core, 5760 B window | 129-165 KB/s | 83-89 KB/s | 6.7 ms |
| 1.1.0, custom core, 32 KB window | **205-250 KB/s** | **126-270 KB/s** | 6.0 ms |

AES-GCM and AES-CTR measure identically, i.e. the link and the window, not
the crypto, set the pace. On the LAN the remaining limit is the 2.4 GHz link
at -73 dBm and per-packet CPU work; through WireGuard the larger window
matters proportionally more, because a whole btop redraw now fits in one
round-trip. A 341 s btop soak at 500 ms refresh (1.0.0) relayed 14.6 MB with
a flat heap and a clean exit; on 1.1.0, btop at its fastest 100 ms refresh
ran 243 s and 183 s relaying 57 MB and 43 MB at a sustained 235 KB/s, with
free internal heap never below 85 KB.
Bulk file transfer through the bastion is still not a design goal.

Commands:

| Command | Purpose | Status |
|---|---|---|
| `status` | One-shot state summary | implemented |
| `uptime` | Device uptime | implemented |
| `version` | Firmware version and authorized key fingerprint | implemented |
| `pc status` | Check whether `192.168.1.200:22` is reachable | implemented |
| `pc ssh` | Print ready-to-use ProxyJump commands with current addresses | implemented |
| `pc wake` | Send a Magic Packet to the main PC | implemented |
| `net status` | Wi-Fi, SSID, IP, RSSI, internet | implemented |
| `vpn status` | Active profile, tunnel IP, handshake, and errors | implemented |
| `vpn failover` | Manually switch to the other VPN profile | implemented |
| `vpn retry-primary` | Force a return to `profile-1` | implemented |
| `help`, `help <command>` | Help generated from the command registry | implemented |
| `exit`, `quit`, `logout` | Close the session | implemented |
| `watch` | Auto-refreshing terminal dashboard (any key stops) | implemented |
| `pc ping [count]` | ICMP check of the PC's LAN address via `esp_ping` | implemented |
| `logs [n]` | Recent events from the in-memory journal | implemented |
| `reboot` | Reboot the ESP32 after `reboot yes` confirmation | implemented |
| `ota status` / `ota <https-url>` / `ota rollback yes` | A/B firmware update, see "Over-the-air updates" | implemented |
| `ssh user@board <command>` | any console command non-interactively; `ota` reads an image from stdin | implemented |

The TCP tunnel to the PC is implemented as a standard SSH `direct-tcpip`
channel: the board acts as a jump host (`ssh -J`), and only a single
destination is allowed, `192.168.1.200:22`. A request for any other
destination is rejected.

`help` is part of the interface, not just documentation. The full structure,
detailed help text, and concrete command examples are documented in
[cli-reference.md](cli-reference.md). Since 1.0.0 the implementation really
does build both `help` and `help <command>` from the single `kCommands[]`
registry that dispatches the handlers, so the text cannot drift from what the
firmware actually does.

A standard SSH `direct-tcpip` channel is the preferred way to reach the Linux
PC over plain SSH. It lets the ESP32 act as a jump host without standing up a
general SOCKS proxy or routing the whole subnet.

## Over-the-air updates

Since 1.2.0 the board updates itself in place, using the two 6.25 MB app
slots of the 16 MB partition table (`app0`/`app1`) in the classic A/B
pattern (`src/ota_update.cpp`).

### Image format

CI (and `scripts/ota_sign.py sign` locally) turns the build's `firmware.bin`
into `firmware-signed.bin`:

```text
[ ESP-IDF app image ][ 32-byte version, NUL-padded ][ 64-byte Ed25519 signature ]
                     └────── signed: SHA-256(image || version field) ──────┘
```

The public key is compiled into the firmware (`include/ota_public_key.h`);
the private key exists only in the GitHub repository secret
`OTA_SIGNING_KEY` and in the maintainer's `~/.config/esp32-bastion/`. A
board therefore accepts only images produced by this project's CI (or by
someone holding that key); anyone building their own fork regenerates the
pair with `ota_sign.py keygen` and the header with `pubkey --header`.

### Delivery paths

| Path | Command | Needs |
|---|---|---|
| Push over SSH | `ssh user@board ota < firmware-signed.bin` | the console reachable (LAN, WireGuard, or `-J`); no internet on the board |
| Pull over HTTPS | `ota https://…/firmware-signed.bin` at the prompt | internet on the board; `https://` only, validated against the ESP-IDF CA bundle; up to 5 redirects (GitHub Releases redirect to `objects.githubusercontent.com`) |

Both feed the same streaming sink: bytes go to `esp_ota_write()` into the
inactive slot as they arrive (sequential erase, so the first progress line
appears immediately), SHA-256 is computed on the fly, and the last 96 bytes
are held back as the trailer. Nothing is decided until the stream ends:
then the signature is checked against the digest, `esp_ota_end()` runs
ESP-IDF's own image validation (segments, checksum, embedded SHA-256), and
only then `esp_ota_set_boot_partition()` points the bootloader at the new
slot. A rejected image leaves the running firmware and the other slot
untouched; the SSH exec exits with status 1 and the reason. Uploaded
non-images are rejected on the first chunk (ESP image magic byte).

### Self-test and rollback

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` the bootloader marks a newly
booted slot `pending-verify`. The Arduino core would confirm it
unconditionally during start-up; `verifyRollbackLater()` is overridden so
that the firmware's own self-test decides:

- **pass**: Wi-Fi associated and the SSH server listening (or the setup
  portal up, which is where a firmware with a changed config layout
  legitimately lands) within **120 s** → `esp_ota_mark_app_valid_cancel_rollback()`;
- **fail**: the budget expires, or the image panics / trips the watchdog
  before confirming → `esp_ota_mark_app_invalid_rollback_and_reboot()`, and
  the bootloader boots the previous slot again.

Verified on the bench (2026-09-06): an image built with the self-test
deliberately sabotaged booted, failed the 120 s test and the board came
back on the previous image; a valid image passed after 2 s. The outcome of
every OTA step is journaled and the last one is also kept in NVS
(`recovery/ota_last`), because the journal itself dies with the reboot
that follows an update - `ota status` shows it.

What the update never touches: NVS (all provisioned settings, the learned
MAC, boot counter), SPIFFS (SSH host key), the other app slot. A new
firmware whose `DeviceConfig` layout changed will open the setup portal
exactly as after a USB flash; that state passes the self-test.

### Non-goals and limits

- No downgrade protection: any correctly signed image installs, including
  an older one - by design, so that a bad release can be undone by
  installing the previous release the same way (or `ota rollback yes`).
- The console is single-session; while an image is being received (10-20 s
  on the LAN, up to a minute or two through WireGuard for 1.7 MB) nothing
  else can log in.
- The bootloader itself and the partition table are not updated over the
  air. Both are stable; changing either still requires USB.

## Wake-on-Wireless LAN

The ESP32 can send a Magic Packet onto the local network by itself; no
separate sending server is needed. Waking a PC over Wi-Fi, however, depends
on the PC's adapter, driver, BIOS/UEFI, and power state.

WoWLAN depends on Magic Packet support in the specific Wi-Fi adapter and on
PCI/USB wakeup being enabled in the BIOS/UEFI and in the network profile —
this needs to be set up on the PC itself once, before the first `pc wake`.
The MAC address is never requested in the portal — the ESP32 determines it
itself from the lwIP ARP table the first time it TCP-probes the PC after it
appears on the network (`src/main_pc.cpp`), and caches it in NVS; the packet
is sent to the broadcast address of the current Wi-Fi subnet. Until the MAC
is learned, `pc wake` replies with `NO MAC` / a clear message instead of
trying to wake an unknown address.

A reference manual check of the same Magic Packet from another device on the
same local network (to compare against what `pc wake` actually sends):

```bash
wakeonlan -i 192.168.1.255 AA:BB:CC:DD:EE:FF
```

Put the main PC to sleep first (e.g. `systemctl suspend`). ARP-based MAC
learning has been verified on a live device; the ESP32 actually sending the
Magic Packet and the PC actually waking from `s2idle` have not yet been
verified end to end. The supported target sleep state is `s2idle`; after a
full shutdown (not sleep) WoWLAN usually doesn't work — that's a limitation
of the adapter/OS, not the firmware.

If the PC doesn't wake after `pc wake`, check in this order: the Magic
Packet actually reaches the adapter (`tcpdump -i <iface> udp port 9` on the
PC itself, from another session), WoWLAN is enabled in the adapter settings
and in the BIOS/UEFI, the network profile in NetworkManager/systemd-networkd
allows wakeup, and the PC is actually in `s2idle` rather than fully powered
off.

If a particular Wi-Fi adapter or BIOS doesn't support the needed power mode,
a fallback is wiring the ESP32 through an optocoupler/relay in parallel with
the PC's power button.

## Two WireGuard profiles

The ESP32 supports up to two independent profiles loaded through the setup
portal: `profile-1` (primary, required for the VPN to work) and `profile-2`
(secondary, optional). Neither server is tied to the firmware — they're just
two slots the user loads their own `.conf` files into (see the example of
this device's actual provisioning in "VPN topology" above).

Implemented active/passive algorithm (`src/recovery_vpn.cpp`):

1. The VPN task waits for Wi-Fi, then syncs the clock over NTP
   (`pool.ntp.org`, `time.cloudflare.com`) — a WireGuard handshake cannot be
   verified without correct time.
2. Once loaded, the board brings up the tunnel via `profile-1` and makes it
   the default route (`esp_wireguard_set_default`).
3. The health check runs every **10 s** and requires all of:
   - a confirmed peer (`esp_wireguard_peer_is_up`);
   - the last handshake age **≤ 180 s**;
   - a successful TCP connect through the tunnel to `1.1.1.1:53` or
     `8.8.8.8:53` (1.2 s timeout).
4. A **30 s** grace period applies right after a profile starts: failed
   checks don't count yet (the handshake is still being established).
5. A single missed check doesn't trigger a switch; a profile is considered
   unavailable only after **3 consecutive** failures.
6. The board tears down the current interface and brings up the other
   profile. It stays there — it does not switch back on its own — as long as
   that profile keeps working.
7. `vpn failover` and `vpn retry-primary` switch the profile manually.
8. On Wi-Fi loss the tunnel is torn down and the state returns to `WAITING`;
   the cycle restarts once the network is back.

Task states: `WAITING → CONNECTING → ONLINE ⇄ DEGRADED`.

Only one WireGuard profile is ever active at a time. Profiles are allowed to
differ in endpoint, tunnel IP, private key, peer public key, and preshared
key — the current profiles genuinely do differ in all of these, including
the tunnel IP.

## Provisioning through the setup portal

Profiles are imported at runtime, not at build time. The single-page
`ESP32_SetUp` portal (`portal/index.html`, gzip-embedded into the firmware by
`scripts/embed_portal.py`) collects Wi-Fi, the PC's IP/port, the username and
public SSH key, and one or two WireGuard profiles.

The public SSH key and both WireGuard profiles are accepted **only as file
uploads** — there is no paste-in text field at all. Reason: on iOS the
portal opens inside Apple's Captive Network Assistant (CNA), a restricted
system mini-browser that loses all form state the moment you leave it for
another app. Pasting text from elsewhere requires exactly that kind of app
switch, while the native file picker does not — it opens as a modal sheet on
top of CNA and doesn't count as "leaving." Every file is validated on the
spot right after it's picked (key type/length, presence of
`[Interface]`/`[Peer]` and an endpoint in a WireGuard config), without
waiting for form submission. The secondary profile has a "Remove" button;
so does the primary one (added because there was otherwise no way to fully
replace both sides when switching VPN servers), and clicking it cascades to
clear the secondary too, since a config with a secondary but no primary
profile is invalid.

The Wi-Fi password field and the other text fields (PC IP, port, username)
remain ordinary keyboard input — typing text doesn't leave the CNA app and
so doesn't break anything.

The `src/wg_conf.cpp` parser parses and validates a standard wg-quick `.conf`
right on the device (ported over from a former Python script,
`generate_wireguard_config.py`, which no longer exists). The supported
fields match what the old build-time validator accepted:

```ini
[Interface]
PrivateKey = ...        # base64, exactly 32 bytes — checked
Address = ...           # exactly one IPv4 address (IPv6 is ignored)
MTU = 1300              # optional; 576-1420, defaults to 1420
DNS = ...               # accepted, but unused by the board

[Peer]
PublicKey = ...         # base64, 32 bytes — checked
PresharedKey = ...      # optional; base64, 32 bytes
Endpoint = host:port    # IPv4/hostname only, host limited to alnum/./-
AllowedIPs = ...        # must include 0.0.0.0/0; IPv6 is ignored
PersistentKeepalive = ...
```

A note on MTU: the `esp_wireguard` library hard-codes an MTU of 1420, so the
firmware applies the value from the `.conf` directly to `netif->mtu` right
after the tunnel comes up.

**`AllowedIPs` must include `0.0.0.0/0`** (a full tunnel) — this isn't a
WireGuard format restriction, it's a requirement of this specific firmware:
the health check (see "Two WireGuard profiles" above) verifies the tunnel by
making a TCP connection to `1.1.1.1:53`/`8.8.8.8:53` **through the WireGuard
interface**. A narrower split-tunnel profile that doesn't cover those
addresses will technically come up and even complete the handshake, but the
health check will consider it permanently unreachable and the device will
flap between profiles forever. That's why `parseWireGuardConf()` rejects any
profile without an explicit `0.0.0.0/0` at the point it's saved through the
portal, with a clear error, instead of silently accepting a config that
won't work. The tradeoff: an equivalent full-tunnel expressed as the pair
`0.0.0.0/1, 128.0.0.0/1`, or a split-tunnel that explicitly includes both
health-check addresses, are also rejected — a deliberate simplification in
favor of one clear rule.

The entire configuration — Wi-Fi, PC IP/port, username, SSH key, both
WireGuard profiles — is stored as a single `DeviceConfig` block in the
`recovery` NVS namespace (`include/device_config.h`,
`src/device_config.cpp`). `POST /api/apply` validates every submitted field,
returns `400` with per-field messages on any error, and saves nothing in
that case. If the Wi-Fi SSID/password changed and every other field passed
its cheap validation, a real STA connection attempt is made before saving
(`WiFi.begin`, up to 15 s) — the `ESP32_SetUp` access point keeps running
throughout, since the ESP32 holds AP and STA mode simultaneously. A wrong
password (`WL_CONNECT_FAILED`) or a network that can't be found
(`WL_NO_SSID_AVAIL`, including a 5 GHz-only network) is returned as a
field-specific error without saving — so a bad password can never land in
NVS and only be discovered after a reboot into a dead network. On success,
the whole block is written atomically (`Preferences::putBytes`) and a reboot
is scheduled 1.5 s later. The PC's MAC address is never requested in the
form — `src/main_pc.cpp` determines and caches it via the lwIP ARP table the
first time the PC is seen on the network.

### Wizard security: a deliberate tradeoff

The portal runs over plain HTTP on the open, passwordless `ESP32_SetUp`
Wi-Fi network. That means the WireGuard private key, the SSH public key, the
home Wi-Fi password, and the main PC's IP are transmitted **in the clear**
and are theoretically visible to any device within range of the access
point during setup.

An earlier design considered here was a two-stage provisioning flow (Wi-Fi
over the open AP, secrets only over USB or after a physical confirmation via
the `BOOT` button) specifically to avoid this risk. It was deliberately
dropped in favor of a single-pass web portal: the convenience of setting up
from a phone (requirement #10 — the common case) outweighed it. Mitigating
factors:

- the risk window is limited to the setup itself, typically tens of
  seconds — the open network isn't needed any longer than that;
- the attack requires physical presence within Wi-Fi range exactly while
  the form is being filled in;
- after `Apply & restart` the device shuts the access point down, and the
  private keys from the form are never transmitted over the air again.

If this risk is unacceptable for a given deployment, a safer alternative is
to set a password on the AP (`WiFi.softAP(kApName, password)` in
`src/setup_portal.cpp`) or to provision the device physically in a space
where stray Wi-Fi clients are excluded. Neither has been done as of now.

### Secrets at rest: no NVS/Flash encryption, deliberately

The Wi-Fi password, both WireGuard private/preshared keys, and the SSH host
key are stored unencrypted — the Wi-Fi password and WireGuard keys in the
NVS blob (`src/device_config.cpp`), the SSH host key as a plain file on
SPIFFS (`src/recovery_ssh.cpp`). Anyone with the board in hand and a
`esptool.py read_flash` can pull all of it straight out of SPI flash.

ESP32-S3 has real answers to this — NVS Encryption and Flash Encryption V2,
both backed by keys burned into eFuse — and they were deliberately not
enabled here:

- Flash Encryption is **one-way**: once the relevant eFuses are burned, the
  chip can never be un-encrypted, and re-flashing afterward requires either
  the original encryption key or accepting that a wrong image bricks the
  device. That's a permanent, high-consequence trade against this project's
  "just reflash it" workflow (including [handing a flash off to an AI
  agent](agent-flashing.md), which has no way to handle an encryption key
  even if one existed).
- The threat this defends against is physical possession of the board by
  someone who wants its keys — not remote compromise. This device is meant
  to live in one specific, physically secure location (the same place the
  main PC already sits), where an attacker walking off with it is excluded
  from the threat model already assumed for that location. Encrypting
  secrets against a threat that isn't in scope buys nothing but a
  permanent, irreversible complication to the build/flash/reprovision
  workflow.

If a future deployment puts the board somewhere physical access by an
untrusted party is actually plausible, that changes the calculus and this
section should be revisited — Flash Encryption V2 plus NVS Encryption would
be the right tool then, accepted one-way cost and all.

## Reliability

Implemented:

- independent FreeRTOS tasks: Arduino loop (LED, BOOT button, watchdog feed;
  core 1), `net-monitor` (Wi-Fi reconnects, internet probe, PC MAC learning;
  core 1), `recovery-vpn` (WireGuard; core 1) and `recovery-ssh` (core 0).
  Nothing that can block on the network runs on the loop task any more, so
  the LED animation and the 5 s / 10 s button classification never stall;
- a 60 s task watchdog subscribed by the loop and net-monitor tasks. Every
  iteration of both is bounded to a few seconds by construction (the longest
  legitimate wait, the portal's 15 s Wi-Fi trial, feeds the watchdog inside
  its loop), so a timeout means a genuine hang and becomes a clean reboot
  with `reset: task-watchdog` in the next dashboard;
- automatic restart after 10 minutes without Wi-Fi association: a radio that
  stays unassociated that long is almost never coming back by itself (driver
  wedged after a brownout, DHCP state stuck), and a reboot re-runs every
  init path from scratch in ~3 s. Loss of *internet* with Wi-Fi still up does
  not trigger it - that is the ISP's problem, not the board's;
- an in-memory event journal (`logs`) with uptime stamps: Wi-Fi events with
  the driver's disconnect reason, `Net:` transitions, VPN transitions, SSH
  logins with peer address and negotiated algorithms, rejected forwarding
  requests, relay close reasons with byte counts, and every restart decision;
- **all `esp_wireguard` calls run on lwIP's `tcpip_thread`** (marshalled with
  `tcpip_callback()` + a semaphore, see `onLwipThread()` in
  `recovery_vpn.cpp`). The library calls `netif_add/remove/set_default`, the
  raw UDP API, `dns_gethostbyname()` and `sys_timeout()` directly; lwIP
  requires all of those to run on its own thread (or under its core lock,
  which the legacy Arduino 2.x build does not enable; the IDF 5 build has
  `CONFIG_LWIP_TCPIP_CORE_LOCKING=y`, and the marshalling is correct under
  both), and the previous code called them from the VPN task on core 1 while
  `tcpip_thread` on core 0 was servicing Wi-Fi traffic.
  All of the wrapped calls are non-blocking (`connect()` reports an
  in-flight DNS lookup as `ESP_ERR_RETRY`), so the hop costs one context
  switch and never stalls packet processing;
- controlled failover between the two VPN profiles (3 failed health checks,
  a 30 s grace period after a profile starts);
- `PersistentKeepalive = 15` to survive NAT;
- tunnel recovery after a Wi-Fi drop and an external IP change;
- the SSH server is protected against hanging on abandoned sessions (the
  timeout table above) — one dead client no longer blocks the emergency
  console;
- the bastion's TCP relay correctly handles a partial write when the SSH
  window or the TCP buffer is full;
- the channel in `relayDirectTcpip` runs in **blocking** mode specifically
  for writes (`ssh_channel_set_blocking(channel, 1)`) — the earlier
  non-blocking version let this libssh fork silently buffer outgoing data
  into an ever-growing `session->out_buffer` instead of respecting the
  network's real throughput; under a heavy stream (`btop`, frequent
  full-screen ANSI/truecolor redraws) this would eventually crash the
  session with `ssh_socket_write: Out of memory` on some `realloc`, whereas
  `htop` never generated enough data to trigger it. Found and fixed on
  2026-08-27 (see the `SSH_OPTIONS_TIMEOUT` = 30 s entry above — it now also
  bounds the worst case for a blocking write). Reads
  (`ssh_channel_poll`/`ssh_channel_read_nonblocking`) are unaffected — both
  functions force their own non-blocking mode regardless of this flag;
- on EOF from the SSH client, the relay doesn't tear down both sides at
  once: it does `shutdown(fd, SHUT_WR)` on the socket to the PC (a proper
  TCP half-close) and keeps pulling the PC's response until the PC closes
  its side or the connection times out on idle — previously a response sent
  after the request's EOF was lost;
- `ssh_channel_write()` in the interactive console (dashboard, prompt,
  character echo) goes through the same reliable, retrying
  `writeAllToChannel()` as the relay, rather than a bare call with no
  result check;
- WireGuard's `esp_wireguard_connect()` in `startProfile()` is bounded by a
  20 s deadline — previously an endpoint with a hostname that never
  resolved (a DNS outage, a typo) could keep the VPN task looping on
  `ESP_ERR_RETRY` forever, with no failover and no response to
  `vpn failover`/`vpn retry-primary`;
- `deviceConfigFactoryReset()` no longer zeroes `gDeviceConfig` in memory —
  the persistent state (NVS/SPIFFS) is already correctly wiped by that
  point, and zeroing the struct while the VPN/SSH tasks on other cores still
  hold raw pointers into it was a race with no upside (the board reboots
  ~2 s later regardless);
- the portal checks the SSH key's structural integrity
  (`isWellFormedSshWireFields`) and performs a real trial import via
  `ssh_pki_import_pubkey_base64()` — the same function the SSH server calls
  after reboot — before saving it; previously a structurally similar but
  corrupted/truncated key could pass the portal and permanently disable SSH
  after reboot with no self-recovery;
- `ensureHostKey()` actually attempts to load the existing host key
  (`ssh_pki_import_privkey_file`) instead of trusting the mere fact that the
  file exists — an empty/corrupted file used to make `ssh_bind_listen()`
  fail with no regeneration;
- `ssh_bind_new()`, the relay buffer allocations and every
  `xTaskCreatePinnedToCore` call (SSH, VPN, net-monitor) are checked for
  failure and logged;
- a `BOOT` level that is already low when the firmware starts is not treated
  as a press. GPIO0 doubles as the USB-serial auto-reset line on DevKitC
  boards, and esptool or an attached monitor holds it low for seconds after
  a reset; before 1.0.0 that could reboot a freshly flashed board into the
  setup portal (5 s) or factory-reset it (10 s);
- `vpn failover`/`vpn retry-primary` requests go through a single
  `std::atomic<VpnRequest>` with `exchange()` instead of two independent
  `volatile bool`s: this is a single-slot mailbox where the last command
  wins — `exchange()` removes the race between reading and clearing the
  request, so an intervening command can't be dropped silently, though this
  is not a guarantee that every individually issued command runs;
- `consecutiveFailures` (the count of consecutive VPN health-check
  failures) saturates at its maximum instead of wrapping to 0 after ~42
  minutes of continuous failure — previously the dashboard could show a
  false "recovered";
- the profile's MTU is applied to the tunnel.

### Known accepted risks (not fixed, rationale)

- ~~`esp_wireguard`/`wireguardif` calls raw lwIP APIs from a user task~~ —
  fixed in 1.0.0: every library call is now executed on `tcpip_thread` (see
  "Reliability" above). What remains is the library's *internal* behaviour,
  which was always correct: its receive path and timers already ran on
  `tcpip_thread`;
- `tcpip_thread` has a 2560-byte stack in the *legacy* (prebuilt) build
  (`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE`); the default 1.1.0 build gives it
  6144 bytes. `esp_wireguard_connect()` runs there and performs the X25519
  key derivation for the new interface - the same amount of stack the
  library's own handshake-response handler already used on that thread, so
  no new worst case, but on the legacy build the headroom is not generous.
  `-DBASTION_STACK_DIAG` logs the high-water mark for verification;
- a narrow race in the ARP lookup (`main_pc.cpp`): after draining a stale
  semaphore token, there remains a theoretical chance that a callback from
  an already-timed-out previous call is still sitting in the
  `tcpip_callback` queue and fires while the next request is being prepared
  — strictly speaking, it could also land in the gap between clearing
  `found` and writing the new IP into `ArpQuery`. There's no guarantee that
  a "late" callback reads an already-current IP; the practical safety
  margin comes from somewhere else: `ArpQuery` is a single reused static
  object, not a per-call one, and in normal operation it repeatedly queries
  the same, unchanging configured PC IP. So the worst practical effect is
  one extra redundant ARP scan, not a result swapped for someone else's MAC.
  A complete fix would need per-call lifetime management (heap allocation
  plus a generation id, or a `shared_ptr`), which was judged a
  disproportionate amount of complexity for this narrow, non-corrupting
  case.

Planned:

- a persisted copy of the last N journal lines across a watchdog reboot
  (the last OTA event already survives in NVS);
- keeping the last known-good configuration until a new one is verified;
- a stable, independent power supply (brownout resets are already detected
  by the chip and show up as `reset: brownout` in the dashboard and journal);
- for production: Secure Boot V2 and Flash Encryption (signed OTA updates
  exist since 1.2.0).

## Implementation stages

1. ~~SSH server on the local network, public-key authentication, and a
   dashboard~~ — done.
2. ~~`192.168.1.200:22` checks, the `pc ssh` command and an in-memory event
   journal (`logs`)~~ — done.
3. Exercising the already-configured WoWLAN from `s2idle`; the `pc wake`
   command is implemented, the actual wake-up test hasn't been run yet.
4. ~~A single WireGuard profile and remote SSH access to the ESP32 only~~ —
   done (SSH is also reachable from the LAN; keeping it that way is a
   decision — see "Open decisions").
5. ~~A TCP bastion to `192.168.1.200:22` over WireGuard~~ — done, tested
   through the full chain and under load.
6. ~~A second profile, automatic failover, and manual control commands~~ —
   done.
7. ~~Provisioning Wi-Fi, the PC, the SSH key, and WireGuard profiles through
   a web portal on the running device~~ — done (see "Provisioning through
   the setup portal" above). Transactional rollback (restoring the previous
   working set of profiles if the new one fails verification) — planned.
8. ~~A/B OTA with signature verification and automatic rollback~~ — done
   in 1.2.0 (see "Over-the-air updates"); extended soak testing continues.
9. Production hardening: Secure Boot, Flash Encryption, unique keys, a
   password on `ESP32_SetUp` or some other protection for provisioning
   against eavesdropping (see "Wizard security" above). Signed OTA images
   (1.2.0) cover the update path; Secure Boot would extend the same
   guarantee to the bootloader and to USB flashing.

## Diagnosing common failures

Lessons from debugging on 2026-08-25/26 — check in this order:

1. **"The VPN came up, but the board isn't reachable."** Make sure the
   client is connected to the same VPN server the board is currently on
   (`vpn status` in the console over LAN, or `wg show` on the server: the
   board's peer should have a recent handshake). Remember that the board's
   tunnel IP differs between servers.
2. **TCP:22 opens, but the SSH banner never arrives.** Before the fix this
   meant the single-threaded server had wedged (fixable only by rebooting
   the board); after the fix the server reclaims dead sessions on its own
   within 30-60 s. If it recurs, capture the serial log.
3. **A session gets dropped under load (e.g. `btop`, but not `htop`).** Two
   independent fixes: (a) bytes lost on a partial write in the relay —
   fixed, verify with a checksum:
   `ssh -J … user@192.168.1.200 'seq 1 200000' | md5sum`; (b) the session
   itself would crash with `ssh_socket_write: Out of memory` because of a
   non-blocking channel write — fixed on 2026-08-27 (see "Reliability"
   above). If it recurs, `logs` (and the serial log) will show the relay's
   close reason, the exact libssh error text and the bytes transferred in
   each direction.
4. **No handshake at all on the server.** Check that the board's public key
   matches its peer entry on the server (`echo <privkey> | wg pubkey`) and
   the PSK.
5. **VPN stays in `CONNECTING` for a long time, `vpn failover`/
   `vpn retry-primary` execute with a delay.** Before the fix on
   2026-08-27, an endpoint with a hostname that never resolved could keep
   `esp_wireguard_connect()` looping on `ESP_ERR_RETRY` with no time limit
   at all. Each individual connection attempt is now bounded to 20 s, but
   after a failed attempt the task waits 10 s and tries again — so the
   overall `CONNECTING` state can last longer than a single attempt. A
   manual command is stored atomically and processed between attempts,
   meaning it takes effect within roughly 30 s rather than instantly. If the
   serial log shows a recurring `connect timed out`, check the DNS for the
   profile's `Endpoint` hostname.
6. **The board is unreachable right after flashing, or while a serial
   monitor is attached.** Every open/close of the USB-serial port resets the
   board through the auto-reset circuit (DTR/RTS), dropping Wi-Fi, the
   tunnel and any SSH session for ~3 s; a monitor left open with default
   DTR/RTS levels can also hold GPIO0 (`BOOT`) low. Since 1.0.0 a low level
   present at startup is ignored, so the board no longer wanders into the
   setup portal because of it, but the resets themselves remain: close the
   serial port before testing anything over the network (see
   [agent-flashing.md](agent-flashing.md)).
7. **SSH is unreachable after portal setup, even though the portal reported
   success.** Before the fix, a structurally similar but
   corrupted/truncated SSH key could pass the portal's validation and
   permanently break the SSH server after reboot, with no self-recovery.
   The portal now performs a real trial import using the same function the
   server uses, so such a key should now be rejected right in the form. If
   it recurs, the serial log will show `SSH: initialization failed`;
   recovery is to hold `BOOT` for 5 s and upload a correct key again.

## Open decisions

- how to protect `ESP32_SetUp` against eavesdropping during provisioning
  (currently an open network with no password — an accepted tradeoff, see
  "Wizard security" above);
- transactional rollback: restoring the previous working set of WireGuard
  profiles if a newly applied one fails verification;
- whether to keep ESP32 SSH reachable from the LAN or restrict it to
  WireGuard only;
- a hardware way to power the PC on if WoWLAN doesn't work after a full
  shutdown;
- an end-to-end test of the ESP32 actually waking the PC via Magic Packet
  (ARP-based MAC learning has already been verified on a live device — see
  "Wake-on-Wireless LAN" above);
- whether the legacy (prebuilt-core) environment should be kept beyond 1.1.x
  now that the custom-built core is the default, given that every change has
  to be verified twice while it exists.
