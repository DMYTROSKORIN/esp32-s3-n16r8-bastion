# Recovery-access terminal console

## Purpose

The ESP32's SSH console should be usable without any outside instructions.
On login it immediately shows a dashboard, and the `help` command explains
the available actions with ready-to-use examples. The user shouldn't have to
remember any syntax.

This is not a Unix shell. Arbitrary system commands, running programs, and
filesystem access are not supported. The console exposes only a safe set of
operations for diagnostics and connectivity recovery.

Without SSH access, the same high-level state (Wi-Fi/internet/VPN profile)
is partly visible on the built-in RGB LED — see the color table in
[recovery-access-architecture.md](recovery-access-architecture.md#status-indication-rgb-led) or the
full description in [device-behavior.md](device-behavior.md).

## Landing screen

The dashboard uses ANSI colors: a ● indicator on every line — green means
that specific line is healthy/`ONLINE`, red means Wi-Fi/internet/the main
PC is unreachable or the VPN has consecutive health-check failures, and
yellow covers every other WireGuard state (`WAITING`/`CONNECTING`/
`DEGRADED`/`NOT_CONFIGURED`) as well as `WoWLAN: NO MAC` — plus gray
secondary details and cyan command hints. The `recovery>` prompt is bold
green.

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

recovery>
```

Line behavior:

- The header shows the target board, the firmware version and the reason
  for the last reboot (power-on / software / panic / interrupt-watchdog /
  task-watchdog / watchdog / brownout / other). A `task-watchdog` there
  means the 60 s software watchdog fired — see `logs` for what preceded it.
- `Wi-Fi` shows the SSID, RSSI and for how long the current association
  has lasted (resets to zero on every disconnect).
- `WireGuard` shows the VPN task's actual state
  (`NOT_CONFIGURED / WAITING / CONNECTING / ONLINE / DEGRADED`) and the
  active profile name. The tunnel IP and handshake age are appended only
  once a handshake actually exists; before that, only the profile name is
  shown.
- A `VPN errors` line appears once `consecutiveFailures > 0` for the
  active profile — incremented by any failed connection/health-check
  attempt, not only by health-check failures on an otherwise-established
  tunnel.
- `WoWLAN` shows `READY` with the MAC address when the PC is offline and
  can be woken, `STANDBY` when the PC is already online, and `NO MAC` if
  the MAC hasn't been learned yet — which requires the PC to have
  answered a TCP probe on its configured SSH port at least once since
  provisioning/reset, so the ARP lookup that follows the probe can
  succeed.
- `Memory` shows the free internal heap and its low-water mark since boot
  (a steadily sinking minimum is the early sign of a leak), free/total
  PSRAM, and how many SSH sessions have authenticated since boot.

## Top-level help

The `help` and `?` commands are always available. Both the command list
and the per-command help are generated from the single command registry
(`kCommands[]` in `src/recovery_ssh.cpp`) that also dispatches the
handlers, so this output cannot drift from what the firmware does. The
actual `help` output in the current firmware:

```text
ESP32 Recovery Gateway v1.1.0 - command reference

STATUS
  status               Show the complete dashboard
  watch                Auto-refresh the dashboard every 2 s (any key stops)
  uptime               Show device uptime
  version              Show firmware, board and key fingerprint
  logs                 Show recent events (logs [n], default 40)

MAIN PC
  pc status            Check the configured PC's SSH port
  pc ping              ICMP ping the PC (pc ping [count], default 4)
  pc wake              Send WoWLAN Magic Packet
  pc ssh               Show ready-to-use connect commands

NETWORK
  net status           Show Wi-Fi and internet state

VPN
  vpn status           Show tunnel and handshake state
  vpn failover         Switch to the other profile
  vpn retry-primary    Switch back to the primary profile

DEVICE
  reboot               Restart the ESP32 (asks for confirmation)

HELP
  help                 Show this command list; `help <command>` for details
  exit                 Close the SSH session (also: quit, logout)

Examples:
  help pc wake
  help examples
  logs 100
```

`?` is a synonym for `help`; `quit` and `logout` are synonyms for `exit`.
Commands are case-insensitive and tolerate repeated spaces.

### Line editing

The console is a small line editor, not a raw byte sink:

| Key | Effect |
|---|---|
| `Backspace` | delete the last character |
| `↑` / `↓` | recall the previous / next command (8 entries, kept across sessions until reboot) |
| `Ctrl+C` | discard the current input line |
| `Ctrl+U` | clear the current input line |
| `Ctrl+L` | clear the screen and redraw the prompt |
| `Ctrl+D` | end the session |

Terminal escape sequences (arrow keys, Home/End, function keys) are parsed
and either acted upon or swallowed — they no longer leak `[A`-style
fragments into the command line.

## Per-command help

`help <command>` prints the command's one-line summary followed by its
detailed text from the registry: purpose, syntax, defaults, what it
changes, and examples. Commands whose detail depends on live state
(`pc wake`, `pc ssh`) fill in the current addresses.

Example `help pc wake` (the address and MAC are substituted from the
current configuration; `pc wake` fails with a clear message if the MAC
hasn't been learned yet):

```text
pc wake - Send WoWLAN Magic Packet

Target: 192.168.1.200, broadcast on the local subnet
MAC: AA:BB:CC:DD:EE:FF
Sends the Magic Packet three times, 250 ms apart.
```

While the MAC hasn't been learned yet, the command itself replies:

```text
The PC's MAC address has not been learned yet.
It must appear on the network at least once (powered on)
before WoWLAN can target it.
```

The actual `pc ssh` output (the addresses are filled in dynamically from
the active profile — after a failover the commands print with the
backup server's addresses instead):

```text
pc ssh - connect through this ESP32 bastion

On the LAN:
  ssh -J user@192.168.1.120 user@192.168.1.200

If your device is a VPN client of profile-1 (203.0.113.10):
  ssh -J user@10.66.0.2 user@192.168.1.200

Without a VPN client (jump over the VPN server's sshd):
  ssh -J user@203.0.113.10:8326,user@10.66.0.2 user@192.168.1.200

Only destination 192.168.1.200:22 is permitted.
```

The `8326` jump port comes from the "VPN server's SSH port" field set for
this profile in the setup portal (defaults to `22` if left blank) — it is
not part of the WireGuard `.conf` itself, since wg-quick has no such field.

The ESP32 never stores the Linux account's password or the user's
private SSH key: ProxyJump performs end-to-end authentication from the
local machine, and the board only relays the TCP stream. Full
interactive sessions (including full-screen TUIs like `btop`) work
through the bastion.

### `watch`

Clears the screen and redraws the dashboard every 2 seconds until any key
is pressed. Use it while a PC is waking up or a tunnel is failing over.

### `logs [n]`

Prints the last `n` lines (default 40, up to 256) of the in-memory event
journal. Every line is prefixed with the device uptime in seconds at the
moment it was written:

```text
Event journal (40 of 87 lines, uptime seconds)
  12.404 Wi-Fi: got IP 192.168.1.120 (gw 192.168.1.1, ch 6, -52 dBm)
  12.910 Net: ONLINE
  13.201 SSH: listening on 192.168.1.120:22 as user (key SHA256:...)
  41.007 WireGuard: profile 1 is online
  388.120 SSH: 192.168.1.50:51234 authenticated (aes128-ctr curve25519-sha256)
  388.130 SSH: bastion relay from 192.168.1.50:51234 to 192.168.1.200:22
  1450.800 Relay: closed (peer closed) after 1062s, to-PC 18211 B, to-client 6120453 B (5 KB/s)
```

The journal records state changes, Wi-Fi events with the driver's
disconnect reason, VPN transitions, SSH logins with the peer address and
negotiated algorithms, rejected forwarding requests, relay statistics, and
watchdog/restart decisions. It never contains keys, passwords or profile
contents. It lives in PSRAM and is lost on reboot; the same lines are
echoed to the serial console (115200 baud) as they happen.

### `pc ping [count]`

Sends `count` ICMP echo requests (default 4, max 20) to the PC's LAN
address, 500 ms apart, and reports the reply count and min/avg/max RTT.
Where `pc status` only answers "is sshd reachable", `pc ping` separates
"the PC is off" from "the PC is up but sshd is down".

### `reboot`

```text
recovery> reboot
Reboot the ESP32 now? Type `reboot yes` to confirm.
recovery> reboot yes
Rebooting. Reconnect in ~10 seconds.
```

Provisioned settings, the learned MAC and the host key are kept. The
request is written to the journal (which the reboot then discards) and to
the serial console.

## Ready-made scenarios

The `help examples` command shows step sequences for common failures.
The actual output in the current firmware, in this exact order, is:

```text
Common recovery scenarios

PC asleep:
  pc ping
  pc wake
  watch

Main VPN failed:
  status
  pc status
  pc ssh
  Then run the displayed ProxyJump command locally.

Tunnel flapping:
  logs
  vpn status
  vpn failover
```

Two more recovery scenarios, useful as a reference but **not part of** the
actual `help examples` output:

### The active VPN server is unreachable

```text
1. vpn status          (over LAN: ssh user@192.168.1.120, if the tunnel is dead)
2. vpn failover
3. vpn status
4. Point your own VPN client at the same server the board is now on
```

### Internet or Wi-Fi is flaky

```text
1. net status          (RSSI, channel, how long the association has held)
2. logs                (Wi-Fi disconnect reasons, Net: state transitions)
3. status
```

## Behavior and security

Implemented:

- Only public-key SSH authentication is allowed (a single authorized
  key; the username and the key itself are set through the setup portal
  and stored in NVS); at most 16 auth messages per session. Every failed
  authentication is journaled with the peer address.
- Only AES ciphers (`aes128/256-gcm@openssh.com`, `aes128/256-ctr`),
  SHA-2 MACs and curve25519/ECDH-P256 key exchange are offered — see
  "SSH throughput" in
  [recovery-access-architecture.md](recovery-access-architecture.md#ssh-throughput-on-the-esp32-s3)
  for the reasoning behind the pinned list.
- The ESP32 never displays the private key, preshared key, or Wi-Fi
  password.
- The SSH bastion only permits the `<PC IP>:<PC port>` destination from
  the current configuration; a request for any other `direct-tcpip`
  destination is rejected and journaled.
- The server handles one session at a time. While a relayed tunnel to
  the PC is open, the console is unavailable (and vice versa). One
  channel per session: `ssh -J` opens exactly one `direct-tcpip` channel,
  which is the supported pattern; multiplexed sessions (`ControlMaster`,
  several `-L` forwards on one connection) are not.
- Protection against stalled clients: a 30 s libssh I/O timeout on the
  session (covers a stall during key exchange or authentication) plus
  TCP keepalive as a backstop for a client that vanishes without a FIN;
  a 60 s budget *after* authentication succeeds for the client to open a
  channel and either request a shell or open the PC tunnel; and a
  10-minute idle timeout for both the console and the tunnel. The
  interactive shell prints `Idle timeout. Bye.` before closing; an idle
  tunnel just closes silently (it's a raw relay, not a text channel).
  Either way the server goes back to accepting connections right after.
- An unknown command isn't executed and points the user to `help`.
- `reboot` requires an explicit `reboot yes`.

Planned:

- suggesting similar commands on a typo;
- `logs follow` (stream new journal lines until a key is pressed).
