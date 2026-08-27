# Flashing this board (for an AI coding agent)

This page is written to be handed directly to an AI coding agent (Claude Code, Codex CLI, etc.)
running in a shell with the board connected over USB. If a human pasted you a link to this file,
the intent is: build the firmware, flash it, confirm it boots cleanly, then stop and hand back to
the human. Follow it literally; don't improvise around the parts marked as out of scope below.

## What you can do

Build, flash, and verify boot. That's it. You do not need network access, a GitHub token, or any
of the device owner's credentials to do any of this — the firmware has nothing baked in (see
[recovery-access-architecture.md](recovery-access-architecture.md)).

## What you cannot do

**The setup portal wizard requires a human on Wi-Fi.** After a first flash, a factory reset, or any
firmware change that alters `DeviceConfig`'s layout (see "A config-layout change" below), the board
opens its own access point (`ESP32_SetUp`, `192.168.4.1`) and waits for someone to join it from a
phone or laptop and fill in a web form (Wi-Fi credentials, the PC's address, an SSH public key,
WireGuard `.conf` files). You almost certainly do not have a way to join that Wi-Fi network from
your shell. Once you've confirmed the board is sitting in `State: SETUP` over serial, stop and tell
the human it's waiting for them at `http://192.168.4.1/` on the `ESP32_SetUp` network — do not try
to drive that HTTP API yourself on their behalf; it collects their real Wi-Fi password and SSH key.

## 1. Find PlatformIO and the board

PlatformIO's CLI may not be on `PATH` even if VS Code's PlatformIO extension is installed. Use the
full path if the bare command isn't found:

```sh
command -v pio || echo "not on PATH, use ~/.platformio/penv/bin/pio instead"
```

Find the board's serial device:

```sh
ls /dev/serial/by-id/ 2>/dev/null
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

If nothing shows up, the board isn't plugged in or isn't enumerating — stop and say so rather than
guessing a port.

## 2. Build

From the repo root:

```sh
~/.platformio/penv/bin/pio run
```

A warning about `ssh_message_auth_pubkey`/`ssh_message_auth_publickey_state` being deprecated is
expected (it's a pre-existing LibSSH-ESP32 API deprecation, not something you introduced) — an
actual build failure is not. Treat any `error:` line, especially around `std::atomic` copy
constructors if you touched `recovery_vpn.cpp`, as something to fix before flashing, not to work
around.

## 3. Flash

```sh
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0   # or whatever you found above
```

This only rewrites program flash; it does not erase the NVS partition, so a previously provisioned
board normally keeps its Wi-Fi/PC/SSH/WireGuard settings across a flash — see the layout-change
exception below.

## 4. Verify boot over serial

`pio device monitor` uses an interactive terminal (`termios`) and will fail with
`termios.error: (25, 'Inappropriate ioctl for device')` in a non-interactive/sandboxed shell. Read
the raw serial port instead:

```sh
~/.platformio/penv/bin/python -c "
import serial, time
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)  # hardware reset
end = time.time() + 20
buf = b''
while time.time() < end:
    data = ser.read(256)
    if data:
        buf += data
print(buf.decode(errors='replace'))
"
```

Two outcomes are both "the flash worked, nothing is broken":

- **Normal boot** (existing config still matches the current firmware's layout): you'll see
  `Wi-Fi`, `Internet: available`, `SSH: listening on ...`, and (if WireGuard profiles are
  configured) `WireGuard: profile N is online`. Nothing further to do — report this back as
  success.
- **A config-layout change** (you added/removed/resized a field in `DeviceConfig` or
  `WgProfileConfig` in `include/device_config.h`, or the firmware's `kConfigVersion` in
  `src/device_config.cpp` changed): the board correctly detects its saved NVS blob no longer
  matches, wipes to unprovisioned state, and opens the setup portal instead — you'll see
  `Portal: open network ESP32_SetUp, http://192.168.4.1/` and `State: SETUP`. This is expected
  behavior, not a bug (see `deviceConfigLoad()`'s version+size check). Report this back to the
  human as "flashed, waiting for you to redo the wizard at ESP32_SetUp" rather than treating it as
  a failure.

Anything else on boot — a crash, a reboot loop, a hang before any of the above lines print — is a
real problem. Don't just re-flash and hope; read the log and figure out what regressed.
