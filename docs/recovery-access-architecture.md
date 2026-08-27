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
  reflashing.
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

Right after login the user sees a summary screen (implemented with ANSI
colors: a green/yellow/red ● indicator per line, gray secondary details,
cyan command hints):

```text
  ESP32 Recovery Gateway
  ────────────────────────────────────────────────
  Device     ● ONLINE    uptime 0d 00:07:44
  Wi-Fi      ● ONLINE    MyHomeWiFi  -51 dBm
  Internet   ● ONLINE
  WireGuard  ● ONLINE    profile-1  10.66.0.2  handshake 10s ago
  Main PC    ● ONLINE    192.168.1.200  ssh :22 open
  WoWLAN     ● STANDBY   AA:BB:CC:DD:EE:FF
  Memory       heap 199 KB  psram 8166 KB  reset: power-on
  ────────────────────────────────────────────────
  help commands   pc ssh how to reach the PC   pc wake wake it up
```

The `VPN errors` line only appears after consecutive health-check failures.
`WoWLAN` shows `READY` when the main PC is offline and can be woken, and
`STANDBY` once the PC is already online.

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

After any error or timeout the server is guaranteed to go back to `accept`
and take the next session.

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
| `watch` | Auto-refreshing terminal dashboard | planned |
| `pc ping` | ICMP check of `192.168.1.200` | planned |
| `logs` | Recent events from the ring buffer log | planned |
| `reboot` | Reboot the ESP32 after confirmation | planned |

The TCP tunnel to the PC is implemented as a standard SSH `direct-tcpip`
channel: the board acts as a jump host (`ssh -J`), and only a single
destination is allowed, `192.168.1.200:22`. A request for any other
destination is rejected.

`help` is part of the interface, not just documentation. The full structure,
detailed help text, and concrete command examples are documented in
[cli-reference.md](cli-reference.md). The implementation builds the help text
from the same command registry that dispatches the handlers, so the text
cannot drift from what the firmware actually does.

A standard SSH `direct-tcpip` channel is the preferred way to reach the Linux
PC over plain SSH. It lets the ESP32 act as a jump host without standing up a
general SOCKS proxy or routing the whole subnet.

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

## Reliability

Implemented:

- independent FreeRTOS tasks for Wi-Fi/LED, internet health, WireGuard, and
  SSH;
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
- `ssh_bind_new()` and both `xTaskCreatePinnedToCore` calls (SSH and VPN)
  are checked for failure and logged;
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

- `esp_wireguard`/`wireguardif` (a third-party library) calls raw lwIP APIs
  (`netif_add/remove/set_default`, the raw UDP API, direct edits to
  `netif->mtu`) from a user FreeRTOS task on core 1, rather than from
  `tcpip_thread` and not under the core lock (`CONFIG_LWIP_TCPIP_CORE_LOCKING`
  is disabled in the SDK in use) — this formally violates lwIP's threading
  model. Across extensive active testing (dozens of
  reconnects/failovers/reboots) it has never manifested, but rare races on
  the `netif` list/UDP PCBs are theoretically possible right at the moment
  of a failover or Wi-Fi loss under concurrent SSH traffic. A proper fix
  would mean wrapping every library call through `tcpip_callback` — an
  invasive rewrite of third-party code that hasn't been undertaken;
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

- a hardware watchdog and a logged reason for every reboot;
- a ring-buffer log with no secrets in it;
- A/B OTA, new-version verification, and automatic rollback;
- keeping the last known-good configuration until a new one is verified;
- brownout detection and a stable, independent power supply;
- for production: Secure Boot V2, Flash Encryption, and signed updates.

## Implementation stages

1. ~~SSH server on the local network, public-key authentication, and a
   dashboard~~ — done.
2. `192.168.1.200:22` checks and the `pc ssh` command — done; a ring-buffer
   log — planned.
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
8. A/B OTA, watchdog fault injection, and extended soak testing.
9. Production hardening: Secure Boot, Flash Encryption, unique keys, a
   password on `ESP32_SetUp` or some other protection for provisioning
   against eavesdropping (see "Wizard security" above).

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
   above). If it recurs, the serial log will show the exact libssh error
   text and the bytes transferred in each direction.
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
6. **SSH is unreachable after portal setup, even though the portal reported
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
  "Wake-on-Wireless LAN" above).
