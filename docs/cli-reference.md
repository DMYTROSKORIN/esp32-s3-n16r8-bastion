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

recovery>
```

Line behavior:

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
- `reset:` is the reason for the last reboot (power-on / software / panic
  / interrupt-watchdog / task-watchdog / watchdog / brownout / other).

## Top-level help

The `help` and `?` commands are always available. The actual `help`
output in the current firmware:

```text
ESP32 Recovery Gateway - command reference

STATUS
  status                 Show the complete dashboard
  uptime                 Show device uptime
  version                Show firmware and key fingerprint

MAIN PC
  pc status              Check the configured PC's SSH port
  pc wake                Send WoWLAN Magic Packet
  pc ssh                 Show ready-to-use connect commands

NETWORK
  net status             Show Wi-Fi and internet state

VPN
  vpn status             Show tunnel and handshake state
  vpn failover           Switch to the other profile
  vpn retry-primary      Switch to server-1

HELP
  help                   Show this command list
  help <command>         Show details and examples
  help examples          Show common recovery scenarios
  exit, quit             Close the SSH session

Examples:
  help pc wake
  help pc ssh
  pc status
```

`logout` is also implemented as a synonym for `exit`/`quit`, along with
line editing (Backspace), `Ctrl+C` (clears the current input line), and
`Ctrl+D` (exits).

Commands planned for later stages (not yet implemented): `watch`,
`health`, `pc ping`, `vpn peers`, `vpn reconnect`, `vpn history`,
`wifi status`, `internet check`, `logs`, `logs follow`, `reboot`.

## Per-command help

`help <command>` is required to show:

- purpose;
- full syntax;
- default values;
- what the command changes;
- possible errors;
- one or more ready-to-use examples;
- the related next step.

Example `help pc wake` (the address and MAC are substituted from the
current configuration; `pc wake` fails with a clear message if the MAC
hasn't been learned yet):

```text
pc wake - wake the main PC over Wi-Fi

Usage: pc wake
Target: 192.168.1.200, broadcast on the local subnet
MAC: AA:BB:CC:DD:EE:FF

Sends the Magic Packet three times, 250 ms apart.
Example:
  pc status
  pc wake
  pc status
```

While the MAC hasn't been learned yet, the command instead replies:

```text
The PC's MAC address has not been learned yet.
It must appear on the network at least once (powered on)
before WoWLAN can target it.
```

`help <command>` is currently implemented in detail only for `pc wake`,
`pc ssh`, `status`, and `examples` (see above and below). For any other
command, including `vpn failover`, `vpn status`, `net status`, etc., the
response right now is a generic placeholder:

```text
No detailed help for 'vpn failover'. Type 'help' for all commands.
```

Extending per-command help to the rest of the command registry (see
"Implementation requirement" below) is not implemented yet — planned.

The actual `pc ssh` output (the addresses are filled in dynamically from
the active profile — after a failover the commands print with the
backup server's addresses instead):

```text
pc ssh - connect through this ESP32 bastion

If your device is a VPN client of profile-1 (203.0.113.10):
  ssh -J user@10.66.0.2 user@192.168.1.200

Without a VPN client (jump over the VPN server's sshd):
  ssh -J user@203.0.113.10:8326,user@10.66.0.2 user@192.168.1.200

Only destination 192.168.1.200:22 is permitted.
```

The `8326` jump port is a firmware constant (`kVpnServerSshPort`) for
rendering this example, not something read from the WireGuard profile —
if a VPN server's sshd actually listens elsewhere, adjust the command by
hand.

The ESP32 never stores the Linux account's password or the user's
private SSH key: ProxyJump performs end-to-end authentication from the
local machine, and the board only relays the TCP stream. Full
interactive sessions (including full-screen TUIs like `btop`) work
through the bastion.

## Ready-made scenarios

The `help examples` command shows step sequences for common failures.
The actual output in the current firmware, in this exact order, is:

```text
Common recovery scenarios

PC asleep:
  pc status
  pc wake
  pc status

Main VPN failed:
  status
  pc status
  pc ssh
  Then run the displayed ProxyJump command locally.
```

Below are two more recovery scenarios, useful as a reference but **not
part of** the actual `help examples` output:

### The active VPN server is unreachable

```text
1. vpn status          (over LAN: ssh user@192.168.1.120, if the tunnel is dead)
2. vpn failover
3. vpn status
4. Point your own VPN client at the same server the board is now on
```

### Internet or Wi-Fi is flaky

```text
1. net status
2. status
3. Watch the serial log over USB (115200 baud) for VPN state transitions
```

## Behavior and security

Implemented:

- Only public-key SSH authentication is allowed (a single authorized
  key; the username and the key itself are set through the setup portal
  and stored in NVS); at most 16 auth messages per session.
- The ESP32 never displays the private key, preshared key, or Wi-Fi
  password.
- The SSH bastion only permits the `<PC IP>:<PC port>` destination from
  the current configuration; a request for any other `direct-tcpip`
  destination is rejected.
- The server handles one session at a time. While a relayed tunnel to
  the PC is open, the console is unavailable (and vice versa).
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
- `Ctrl+C` clears the current input line, `Ctrl+D` ends the session.
- `vpn failover` / `vpn retry-primary` acknowledge the request and
  suggest checking the result with `vpn status`.

Planned:

- a ring buffer log of all events, with no secrets in it;
- `reboot` with an explicit text confirmation;
- suggesting similar commands on a typo.

## Implementation requirement (not yet done)

Today, command dispatch (`executeCommand()`) and the help text
(`writeHelp()` / `writeDetailedHelp()`) in `src/recovery_ssh.cpp` are two
separate hardcoded `if`/`else` chains — which is exactly why `help
<command>` above is only implemented for a handful of commands, and why
this document has to be checked against the source by hand instead of
being guaranteed correct by construction.

The intended fix is a single command registry with, per command: name
and aliases; short description; detailed help; syntax; examples; access
level; confirmation flag; handler function — with `help` and
`help <command>` generated directly from it, so the firmware can't drift
from its own built-in help text again.
