# ESP32-S3-N16R8 Bastion

PlatformIO project for an ESP32-S3 development board (16 MB flash, 8 MB
Octal PSRAM) acting as an emergency-access SSH/WireGuard bastion for a
Linux PC on the same LAN.

## Build and upload

Open the directory in VS Code with the PlatformIO extension, then use the
PlatformIO **Upload** action. The serial monitor runs at 115200 baud.

The firmware is fully provisioned at runtime — nothing is baked into the
build. On first boot (or after a factory reset) it opens an open access point,
`ESP32_SetUp`, with a lightweight captive setup page covering Wi-Fi, the main
PC's address, SSH access, and up to two WireGuard profiles. Applying the form
saves everything to NVS and reboots into normal operation. See
[docs/device-behavior.md](docs/device-behavior.md) for the full flow, the LED
state machine, and the `BOOT`-button hold tiers (5 s reopens the portal
pre-filled with the current settings, 10 s wipes the device back to factory
state).

The recovery-access stack is implemented and verified end to end:
public-key-only SSH on port 22, the colored terminal dashboard with built-in
`help`, WoWLAN for the main PC (MAC learned automatically, no manual entry),
dual-WireGuard failover, and a restricted SSH bastion that permits only the
configured PC's address.

Connect from the LAN with the username and PC address you provisioned, e.g.:

```sh
ssh user@192.168.1.120
```

Connect remotely as a VPN client of the primary server, or without any VPN
client by jumping over the VPN server's sshd (the `pc ssh` console command
always prints these with the currently active addresses and username):

```sh
ssh user@10.66.0.2                                              # console
ssh -J user@10.66.0.2 user@192.168.1.200                       # bastion
ssh -J user@203.0.113.10:2222,user@10.66.0.2 user@192.168.1.200
```

The ESP32 stores a unique SSH host key in SPIFFS, generated on first boot and
wiped only by a factory reset. The single-session SSH server is hardened
against stalled or vanished clients (key-exchange/auth timeouts, TCP
keepalive, idle timeouts), and the bastion relay handles partial writes, so
full interactive sessions — including TUI apps like `btop` — work through the
jump chain.

## Provisioning (Wi-Fi, PC, SSH key, WireGuard)

Everything is entered once through the `ESP32_SetUp` portal — no files are
placed in this repository or on the build machine:

- **Wi-Fi**: pick a network from the on-device scan (or enter one manually)
  and its password.
- **Main PC**: IP address and SSH port. Used for the bastion's allowed
  destination, the `pc status` check, and as the Wake-on-WLAN target — its MAC
  address is learned automatically from ARP the first time it is seen online,
  no manual entry.
- **SSH access**: username and public key (ed25519/RSA/ECDSA), loaded from a
  file, with an inline check (key type and length) as soon as it is picked.
- **WireGuard**: a primary profile (required for VPN) and an optional
  secondary one for failover, each a standard wg-quick client `.conf` loaded
  from a file, with an inline check (`[Interface]`/`[Peer]`, endpoint) on
  pick. Either profile can be removed with its own button — removing the
  primary also clears the secondary, since a secondary without a primary is
  invalid. The device parses and validates every field on-device (key format,
  endpoint, `AllowedIPs`, `MTU` 576–1420) before accepting it.

Public keys and WireGuard profiles are file-upload only, with no text field to
paste into. This is deliberate: on iOS the portal opens inside Apple's Captive
Network Assistant, a restricted mini-browser that loses all form state the
moment you leave it to copy text from elsewhere — the native file picker,
unlike an app switch, stays inside that mini-browser and survives.

Applying the Wi-Fi fields first makes a real connection attempt (up to 15 s,
with `ESP32_SetUp` staying up throughout) before anything is saved — a wrong
password or an out-of-range/5 GHz-only network is reported inline instead of
being discovered only after a reboot.

The firmware syncs the clock over NTP, starts the primary profile, verifies a
recent handshake and TCP reachability through the tunnel every 10 seconds, and
changes profile only after three consecutive failed checks. The SSH console
provides `vpn status`, `vpn failover`, and `vpn retry-primary`.

Note the addressing trap: the board is visible only on the server it is
currently connected to, and its tunnel IP differs per server. Your client must
be connected to the same VPN server as the board — `pc ssh` always prints the
currently correct addresses.

The recovery architecture, this device's actual VPN topology, the failover
algorithm, Wake-on-Wireless, SSH-server hardening, and a troubleshooting
checklist are described in
[docs/recovery-access-architecture.md](docs/recovery-access-architecture.md).
The SSH console, its real `help` output, command details, and recovery
examples are documented in [docs/cli-reference.md](docs/cli-reference.md).

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

The number of violet flashes after the green pair doubles as the WireGuard
profile number, so a glance at the LED shows whether failover has already
happened without opening the SSH console. Flash edges are eased in software
(~20 ms) for a cleaner look; the alert patterns (red, amber, the reset flash)
stay hard-edged on purpose. See
[docs/device-behavior.md](docs/device-behavior.md) for exact timings.
