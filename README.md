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

The recovery-access stack is implemented and verified end to end:
public-key-only SSH on port 22, the colored terminal dashboard with built-in
`help`, WoWLAN for the main PC, dual-WireGuard failover, and a restricted SSH
bastion that permits only `192.168.1.200:22`.

Connect from the LAN:

```sh
ssh user@192.168.1.120
```

Connect remotely as a VPN client of the primary server, or without any VPN
client by jumping over the VPN server's sshd (the `pc ssh` console command
always prints these with the currently active addresses):

```sh
ssh user@10.66.0.6                                              # console
ssh -J user@10.66.0.6 user@192.168.1.200                       # bastion
ssh -J user@203.0.113.10:8326,user@10.66.0.6 user@192.168.1.200
```

The ESP32 stores a unique SSH host key in SPIFFS. Only the public half of the
authorized user key is compiled into the firmware; the private key is not part
of this repository. The single-session SSH server is hardened against stalled
or vanished clients (key-exchange/auth timeouts, TCP keepalive, idle
timeouts), and the bastion relay handles partial writes, so full interactive
sessions — including TUI apps like `btop` — work through the jump chain.

## WireGuard profiles

Place the two client profiles in `.local-secrets/primary.conf` (primary VPN
server) and `.local-secrets/secondary.conf` (standby), then build normally. A
pre-build validator parses them and creates a private generated header outside
the tracked source tree. The `.conf` files, generated header, private keys,
and preshared keys are ignored by Git. Keep the profile files mode `0600`.
An optional `MTU` value in `[Interface]` (576–1420) is applied to the tunnel
interface; the library default is 1420.

The firmware syncs the clock over NTP, starts `server-1`, verifies a recent
handshake and TCP reachability through the tunnel every 10 seconds, and
changes profile only after three consecutive failed checks. It stays on
`server-2` while that tunnel remains healthy. The SSH console provides
`vpn status`, `vpn failover`, and `vpn retry-primary`.

Note the addressing trap: the board is visible only on the server it is
currently connected to, and its tunnel IP differs per server (`10.66.0.6` on
the primary, `10.66.0.9` on the standby). Your client must be connected to
the same VPN server as the board.

The recovery architecture, actual VPN topology, failover algorithm,
Wake-on-Wireless, SSH-server hardening, and troubleshooting checklist are
described in
[docs/recovery-access-architecture.md](docs/recovery-access-architecture.md).
The SSH console, its real `help` output, command details, and recovery
examples are documented in [docs/cli-reference.md](docs/cli-reference.md).

## LED status at a glance

| Color | Meaning |
|---|---|
| 🟡 Solid yellow | First-run Wi-Fi wizard is active |
| 🔵 Blue blinking | Connecting or reconnecting to Wi-Fi |
| 🟢🟢 Two quick green flashes | Wi-Fi and internet are working |
| 🔴 Fast red blinking | Wi-Fi is connected, but internet access is unavailable |
