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

## 0. Know the hardware

The target is an **ESP32-S3-N16R8** (16 MB flash, 8 MB PSRAM). `platformio.ini`
is pinned to that module: `memory_type = qio_opi`, a 16 MB partition table
with 6.25 MB OTA slots, QIO flash at 80 MHz, 240 MHz CPU. Do not "fix" a
board with less flash/PSRAM by editing those values unless the human asked
for a port; the boot log prints a `WARNING:` line when the chip underneath
does not match, which is the intended signal to stop and report.

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

The first build downloads the ESP-IDF toolchain and **compiles the whole ESP-IDF from source**
(pioarduino HybridCompile with the project's `custom_sdkconfig`), then libssh and the WireGuard
stack: expect 10-20 minutes and a couple of GB under `~/.platformio`. Do not interrupt it; the IDF
libraries are cached afterwards and later builds take seconds. `pio run -e esp32-s3-n16r8-legacy`
builds against the stock prebuilt core in about a minute if you only need a quick sanity build.
A warning about `ssh_message_auth_pubkey`/`ssh_message_auth_publickey_state` being deprecated is
expected (it's a pre-existing LibSSH-ESP32 API deprecation, not something you introduced) — an
actual build failure is not. Treat any `error:` line, especially around `std::atomic` copy
constructors if you touched `recovery_vpn.cpp`, as something to fix before flashing, not to work
around.

## 3. Flash

```sh
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyACM0   # or whatever you found above
```

USB is needed only for a first flash or when the bootloader/partition table changes. A board that
already runs 1.2.0+ can be updated over the air instead, which also gives you automatic rollback
if the new image does not come up: sign the build (`scripts/ota_sign.py sign --key <key> --version
<v> --in .pio/build/esp32-s3-n16r8/firmware.bin --out firmware-signed.bin`, the key is the
maintainer's, not in the repo) and run `ssh <user>@<board> ota < firmware-signed.bin`. You cannot do
that step without the signing key and the owner's SSH access - if you have neither, stop and hand
the signed image or the build back to the human.

This only rewrites program flash; it does not erase the NVS partition, so a previously provisioned
board normally keeps its Wi-Fi/PC/SSH/WireGuard settings across a flash — see the layout-change
exception below. One caveat when upgrading a board from a pre-1.0.0 build: the partition table
changed from `default_8MB.csv` to `default_16MB.csv`. NVS stays at the same offset (settings
survive), but SPIFFS moves, so the SSH host key is regenerated once and the next `ssh` will show a
`REMOTE HOST IDENTIFICATION HAS CHANGED` warning. That is expected exactly once; tell the human.

## 4. Verify boot over serial

> **Keep serial sessions short and deliberate.** On this DevKitC-class board the USB-serial chip
> drives `EN` and `GPIO0` (the `BOOT` button) through the auto-reset circuit. Opening or closing
> the port toggles DTR/RTS and **resets the board**, and can hold `GPIO0` low while the port is open.
> Since 1.0.0 the firmware ignores a `BOOT` level that is already low at startup, so a flash or a
> monitor no longer sends the board into the setup portal or a factory reset by itself - but every
> open/close of the port is still a reboot, which drops any SSH session and the WireGuard tunnel.
> Read the boot log once with the snippet below (it drives DTR/RTS explicitly), close the port, and
> do all further checks over SSH. Never leave a monitor attached while testing network behaviour.

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

- **Normal boot** (existing config still matches the current firmware's layout): you'll see the
  banner `ESP32-S3-N16R8 Bastion v<version> starting (boot #N, reset: ...)`, a `Chip:` line that
  should read `flash 16 MB QIO @ 80 MHz | PSRAM 8192 KB | SDK 5.5.5` (8189 KB and `v4.4.7` on the
  legacy environment), then `Wi-Fi: got IP ...`,
  `Net: ONLINE`, `SSH: listening on ...`, and (if WireGuard profiles are configured)
  `WireGuard: profile N is online`. Nothing further to do — report this back as success. A
  `WARNING:` line about PSRAM or flash size means the board is not an N16R8 — report that.
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
