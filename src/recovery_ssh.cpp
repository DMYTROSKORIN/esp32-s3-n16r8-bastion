#include "recovery_ssh.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh_esp32.h>
#include <lwip/sockets.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "device_config.h"
#include "event_log.h"
#include "firmware_info.h"
#include "main_pc.h"
#include "net_monitor.h"
#include "recovery_status.h"
#include "recovery_vpn.h"

namespace {
constexpr char kHostKeyFsPath[] = "/ssh_host_ed25519_key";
constexpr char kHostKeyVfsPath[] = "/spiffs/ssh_host_ed25519_key";
constexpr char kBindAddress[] = "0.0.0.0";
constexpr char kBindPort[] = "22";
// libssh's key exchange (curve25519 + Ed25519 signature) and the per-packet
// crypto run on this task's stack. Measured peak use is ~6.5 KB (32 KB stack
// left 25.6 KB untouched after KEX + auth + a 4 MB relay on 2026-09-05, see
// BASTION_STACK_DIAG); 20 KB keeps a 3x margin for RSA client keys and
// future libssh versions while returning 12 KB of internal RAM to the heap.
constexpr uint32_t kSshTaskStack = 20480;

// The server handles one session at a time, so every blocking libssh call must
// be bounded: a stalled or vanished client would otherwise wedge the recovery
// console until the board is power-cycled.
constexpr long kSessionIoTimeoutSeconds = 30;
constexpr uint32_t kPreShellDeadlineMs = 60000;
constexpr uint32_t kShellIdleTimeoutMs = 10 * 60 * 1000;
constexpr uint32_t kRelayIdleTimeoutMs = 10 * 60 * 1000;
// Heavy, frequently-redrawing TUI apps (btop) can push far more data than a
// mobile-tethered client's link can drain in a hiccup; 10 s was tearing down
// sessions that were merely congested, not dead. 30 s gives real congestion
// room to clear while still catching a genuinely gone client reasonably fast.
constexpr uint32_t kRelayWriteStallTimeoutMs = 30000;
constexpr uint8_t kMaxAuthMessages = 16;
constexpr uint32_t kWatchRefreshMs = 2000;
constexpr uint8_t kHistoryDepth = 8;
constexpr size_t kMaxLineLength = 127;

// One relay buffer per direction, allocated once for the task's lifetime in
// internal RAM (a copy target in slower PSRAM would cost throughput for no
// memory benefit: the buffers are permanent anyway). 8 KB is larger than
// lwIP's 5760-byte TCP receive window, so a single recv() always drains the
// PC socket fully, and matches what one ssh_channel_read_nonblocking() call
// can deliver in practice; anything bigger only ties up internal RAM.
constexpr size_t kRelayBufferSize = 8192;

// Algorithms offered to clients, pinned explicitly rather than inherited from
// libssh's build-dependent defaults. In this LibSSH-ESP32/mbedTLS build the
// default list is already AES-only (chacha20-poly1305 needs mbedTLS's
// CHACHAPOLY module, which the Arduino core does not compile in - verified
// 2026-09-05: a client forcing chacha20 gets "no matching cipher found"), so
// every AES cipher here runs on the ESP32-S3's hardware AES block. Pinning the
// list keeps that true across library upgrades and drops the CBC/3DES legacy
// entries a future default might re-add. DH group-exchange is excluded from
// KEX (multi-second modexp on first connect); every OpenSSH since 2014
// prefers curve25519 anyway.
#ifndef BASTION_BENCH_ALL_CIPHERS
constexpr char kCiphers[] =
    "aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes256-ctr";
constexpr char kMacs[] =
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-256,hmac-sha2-512-etm@openssh.com,"
    "hmac-sha2-512";
constexpr char kKex[] =
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256";
#endif

constexpr char kAnsiReset[] = "\x1b[0m";
constexpr char kAnsiBold[] = "\x1b[1m";
constexpr char kAnsiDim[] = "\x1b[90m";
constexpr char kAnsiGreen[] = "\x1b[32m";
constexpr char kAnsiYellow[] = "\x1b[33m";
constexpr char kAnsiRed[] = "\x1b[31m";
constexpr char kAnsiCyan[] = "\x1b[36m";
constexpr char kPrompt[] = "\x1b[1;32mrecovery>\x1b[0m ";
constexpr char kClearScreen[] = "\x1b[2J\x1b[H";

ssh_key authorizedKey = nullptr;
char authorizedKeyFingerprint[64] = "unknown";
uint8_t* relayToPc = nullptr;
uint8_t* relayToClient = nullptr;
std::atomic<uint32_t> sessionsServed{0};
std::atomic<uint32_t> authFailures{0};
char peerAddress[48] = "-";

// ---------------------------------------------------------------------------
// Channel output helpers
// ---------------------------------------------------------------------------

// A single ssh_channel_write() call can accept fewer bytes than offered (a
// full SSH window or TCP send buffer); dropping the remainder would corrupt
// or truncate the output, so every call retries until the whole chunk is
// sent, the channel is gone, or nothing progresses for too long.
bool writeAllToChannel(ssh_channel channel, const uint8_t* data, int length) {
  int offset = 0;
  uint32_t lastProgressMs = millis();
  while (offset < length) {
    if (!ssh_channel_is_open(channel)) {
      return false;
    }
    const int written = ssh_channel_write(channel, data + offset, length - offset);
    if (written == SSH_ERROR) {
      eventLogf("SSH: channel write error: %s",
                ssh_get_error(ssh_channel_get_session(channel)));
      return false;
    }
    if (written > 0) {
      offset += written;
      lastProgressMs = millis();
    } else if (millis() - lastProgressMs > kRelayWriteStallTimeoutMs) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

// A failed write means the channel is stalled or gone; closing it here (once,
// centrally) makes every subsequent ssh_channel_is_open() check false
// immediately, instead of each remaining dashboard row separately
// re-discovering the same dead channel through its own stall timeout.
void channelWrite(ssh_channel channel, const char* text) {
  if (channel == nullptr || text == nullptr) {
    return;
  }
  if (!writeAllToChannel(channel, reinterpret_cast<const uint8_t*>(text),
                         static_cast<int>(strlen(text))) &&
      ssh_channel_is_open(channel)) {
    ssh_channel_close(channel);
  }
}

void channelPrintf(ssh_channel channel, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void channelPrintf(ssh_channel channel, const char* format, ...) {
  char buffer[768];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  channelWrite(channel, buffer);
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
      return "task-watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_BROWNOUT:
      return "brownout";
    default:
      return "other";
  }
}

void formatDuration(uint64_t seconds, char* output, size_t outputSize) {
  const uint32_t days = seconds / 86400ULL;
  seconds %= 86400ULL;
  const uint32_t hours = seconds / 3600ULL;
  seconds %= 3600ULL;
  const uint32_t minutes = seconds / 60ULL;
  const uint32_t secs = seconds % 60ULL;
  snprintf(output, outputSize, "%lud %02lu:%02lu:%02lu",
           static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes), static_cast<unsigned long>(secs));
}

void formatUptime(char* output, size_t outputSize) {
  formatDuration(static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL, output,
                 outputSize);
}

void statusRow(ssh_channel channel, const char* label, const char* color,
               const char* state, const char* detail) {
  channelPrintf(channel, "  %-10s %s● %-9s%s %s\r\n", label, color, state,
                kAnsiReset, detail != nullptr ? detail : "");
}

void writeDashboard(ssh_channel channel) {
  char uptime[32];
  char wifiUptime[32];
  formatUptime(uptime, sizeof(uptime));
  formatDuration(netMonitorWifiUptimeSeconds(), wifiUptime, sizeof(wifiUptime));
  const bool pcOnline = mainPcReachable();
  const bool wifiOnline = WiFi.status() == WL_CONNECTED;
  const bool internetOnline = recoveryInternetAvailable();
  const bool vpnOnline = recoveryVpnOnline();
  const uint32_t handshakeAge = recoveryVpnHandshakeAgeSeconds();
  const uint8_t vpnFailures = recoveryVpnConsecutiveFailures();

  char detail[160];
  channelPrintf(channel,
                "\r\n  %sESP32 Recovery Gateway%s  %s%s v%s · %s%s\r\n"
                "  ────────────────────────────────────────────────\r\n",
                kAnsiBold, kAnsiReset, kAnsiDim, FIRMWARE_TARGET_BOARD,
                FIRMWARE_VERSION, resetReasonName(esp_reset_reason()), kAnsiReset);

  snprintf(detail, sizeof(detail), "%suptime %s%s", kAnsiDim, uptime, kAnsiReset);
  statusRow(channel, "Device", kAnsiGreen, "ONLINE", detail);

  snprintf(detail, sizeof(detail), "%s  %s%d dBm  up %s%s", WiFi.SSID().c_str(),
           kAnsiDim, WiFi.RSSI(), wifiUptime, kAnsiReset);
  statusRow(channel, "Wi-Fi", wifiOnline ? kAnsiGreen : kAnsiRed,
            wifiOnline ? "ONLINE" : "OFFLINE", wifiOnline ? detail : nullptr);

  statusRow(channel, "Internet", internetOnline ? kAnsiGreen : kAnsiRed,
            internetOnline ? "ONLINE" : "OFFLINE", nullptr);

  if (handshakeAge != UINT32_MAX) {
    snprintf(detail, sizeof(detail), "%s  %s%s  %shandshake %lus ago%s",
             recoveryVpnActiveProfileName(),
             recoveryVpnAddress().toString().c_str(), kAnsiReset, kAnsiDim,
             static_cast<unsigned long>(handshakeAge), kAnsiReset);
  } else {
    snprintf(detail, sizeof(detail), "%s", recoveryVpnActiveProfileName());
  }
  statusRow(channel, "WireGuard", vpnOnline ? kAnsiGreen : kAnsiYellow,
            recoveryVpnStateName(), detail);
  if (vpnFailures > 0) {
    snprintf(detail, sizeof(detail), "%s%u consecutive%s", kAnsiRed, vpnFailures,
             kAnsiReset);
    statusRow(channel, "VPN errors", kAnsiRed, "FAILING", detail);
  }

  snprintf(detail, sizeof(detail), "%s  %sssh :%u %s%s", gDeviceConfig.pcIp,
           kAnsiDim, gDeviceConfig.pcPort, pcOnline ? "open" : "closed",
           kAnsiReset);
  statusRow(channel, "Main PC", pcOnline ? kAnsiGreen : kAnsiRed,
            pcOnline ? "ONLINE" : "OFFLINE", detail);

  char mac[24];
  mainPcMacString(mac, sizeof(mac));
  snprintf(detail, sizeof(detail), "%s%s%s", kAnsiDim, mac, kAnsiReset);
  statusRow(channel, "WoWLAN",
            pcOnline ? kAnsiDim : (mainPcMacKnown() ? kAnsiGreen : kAnsiYellow),
            pcOnline ? "STANDBY" : (mainPcMacKnown() ? "READY" : "NO MAC"),
            detail);

  channelPrintf(channel,
                "  %-10s   %sheap %u KB (min %u KB)  psram %u/%u KB  sessions %lu%s\r\n",
                "Memory", kAnsiDim,
                static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                static_cast<unsigned>(ESP.getMinFreeHeap() / 1024),
                static_cast<unsigned>(ESP.getFreePsram() / 1024),
                static_cast<unsigned>(ESP.getPsramSize() / 1024),
                static_cast<unsigned long>(sessionsServed.load()), kAnsiReset);

  channelPrintf(channel,
                "  ────────────────────────────────────────────────\r\n"
                "  %shelp%s commands   %spc ssh%s how to reach the PC   %spc wake%s wake it up\r\n\r\n",
                kAnsiCyan, kAnsiReset, kAnsiCyan, kAnsiReset, kAnsiCyan,
                kAnsiReset);
}

// ---------------------------------------------------------------------------
// Command registry: dispatch and help are generated from one table so the
// built-in help can never drift from what the firmware actually does.
// ---------------------------------------------------------------------------

struct Command {
  const char* name;      // Full command, e.g. "pc wake".
  const char* group;     // Section header in `help`.
  const char* summary;   // One line for `help`.
  const char* detail;    // Multi-line text for `help <command>` (may be null).
  // Returns false to end the session. `args` is the trimmed remainder.
  bool (*handler)(ssh_channel channel, const char* args);
};

bool cmdHelp(ssh_channel channel, const char* args);
bool cmdStatus(ssh_channel channel, const char*);
bool cmdWatch(ssh_channel channel, const char*);
bool cmdUptime(ssh_channel channel, const char*);
bool cmdVersion(ssh_channel channel, const char*);
bool cmdLogs(ssh_channel channel, const char* args);
bool cmdPcStatus(ssh_channel channel, const char*);
bool cmdPcWake(ssh_channel channel, const char*);
bool cmdPcSsh(ssh_channel channel, const char*);
bool cmdPcPing(ssh_channel channel, const char* args);
bool cmdNetStatus(ssh_channel channel, const char*);
bool cmdVpnStatus(ssh_channel channel, const char*);
bool cmdVpnFailover(ssh_channel channel, const char*);
bool cmdVpnRetryPrimary(ssh_channel channel, const char*);
bool cmdReboot(ssh_channel channel, const char* args);
bool cmdExit(ssh_channel channel, const char*);

// Longest names first within a group so "pc status" is matched before "pc".
const Command kCommands[] = {
    {"status", "STATUS", "Show the complete dashboard",
     "Checks Wi-Fi, internet, WireGuard, memory and the main PC's SSH port.\r\n",
     cmdStatus},
    {"watch", "STATUS", "Auto-refresh the dashboard every 2 s (any key stops)",
     "Redraws the dashboard every 2 seconds until you press a key.\r\n"
     "Handy while waiting for a PC to wake or a tunnel to fail over.\r\n",
     cmdWatch},
    {"uptime", "STATUS", "Show device uptime", nullptr, cmdUptime},
    {"version", "STATUS", "Show firmware, board and key fingerprint", nullptr,
     cmdVersion},
    {"logs", "STATUS", "Show recent events (logs [n], default 40)",
     "Prints the last n lines of the in-memory event journal (up to 256).\r\n"
     "The journal is secrets-free: state changes, VPN transitions, SSH\r\n"
     "logins and relay statistics. It is lost on reboot.\r\n"
     "Example:\r\n  logs 100\r\n",
     cmdLogs},
    {"pc status", "MAIN PC", "Check the configured PC's SSH port", nullptr,
     cmdPcStatus},
    {"pc ping", "MAIN PC", "ICMP ping the PC (pc ping [count], default 4)",
     "Sends ICMP echo requests to the PC's LAN address, 500 ms apart.\r\n"
     "Use it to tell \"PC is off\" from \"PC is up but sshd is down\".\r\n",
     cmdPcPing},
    {"pc wake", "MAIN PC", "Send WoWLAN Magic Packet", nullptr, cmdPcWake},
    {"pc ssh", "MAIN PC", "Show ready-to-use connect commands", nullptr, cmdPcSsh},
    {"net status", "NETWORK", "Show Wi-Fi and internet state", nullptr,
     cmdNetStatus},
    {"vpn status", "VPN", "Show tunnel and handshake state", nullptr, cmdVpnStatus},
    {"vpn failover", "VPN", "Switch to the other profile",
     "Stops the active tunnel and starts the other WireGuard profile.\r\n"
     "Ignored when only one profile is provisioned.\r\n",
     cmdVpnFailover},
    {"vpn retry-primary", "VPN", "Switch back to the primary profile", nullptr,
     cmdVpnRetryPrimary},
    {"reboot", "DEVICE", "Restart the ESP32 (asks for confirmation)",
     "Type `reboot yes` to skip the confirmation prompt.\r\n"
     "Provisioned settings are kept; only the current session ends.\r\n",
     cmdReboot},
    {"help", "HELP", "Show this command list; `help <command>` for details",
     nullptr, cmdHelp},
    {"exit", "HELP", "Close the SSH session (also: quit, logout)", nullptr, cmdExit},
};

const Command* findCommand(const char* line, const char** argsOut) {
  const Command* best = nullptr;
  size_t bestLength = 0;
  for (const Command& command : kCommands) {
    const size_t length = strlen(command.name);
    if (strncmp(line, command.name, length) == 0 &&
        (line[length] == '\0' || line[length] == ' ') && length > bestLength) {
      best = &command;
      bestLength = length;
    }
  }
  if (best == nullptr) {
    // Aliases.
    if (strcmp(line, "?") == 0) {
      return findCommand("help", argsOut);
    }
    if (strcmp(line, "quit") == 0 || strcmp(line, "logout") == 0) {
      return findCommand("exit", argsOut);
    }
    return nullptr;
  }
  const char* args = line + bestLength;
  while (*args == ' ') {
    ++args;
  }
  *argsOut = args;
  return best;
}

bool cmdHelp(ssh_channel channel, const char* args) {
  if (*args != '\0') {
    if (strcmp(args, "examples") == 0) {
      channelWrite(channel,
                   "\r\nCommon recovery scenarios\r\n\r\n"
                   "PC asleep:\r\n  pc ping\r\n  pc wake\r\n  watch\r\n\r\n"
                   "Main VPN failed:\r\n  status\r\n  pc status\r\n  pc ssh\r\n"
                   "  Then run the displayed ProxyJump command locally.\r\n\r\n"
                   "Tunnel flapping:\r\n  logs\r\n  vpn status\r\n  vpn failover\r\n");
      return true;
    }
    const char* rest = nullptr;
    const Command* command = findCommand(args, &rest);
    if (command == nullptr) {
      channelPrintf(channel,
                    "\r\nNo such command '%s'. Type 'help' for all commands.\r\n",
                    args);
      return true;
    }
    channelPrintf(channel, "\r\n%s - %s\r\n\r\n", command->name, command->summary);
    if (command->detail != nullptr) {
      channelWrite(channel, command->detail);
    }
    // Commands whose detail depends on live state.
    if (strcmp(command->name, "pc wake") == 0) {
      char mac[24];
      mainPcMacString(mac, sizeof(mac));
      channelPrintf(channel,
                    "Target: %s, broadcast on the local subnet\r\nMAC: %s\r\n"
                    "Sends the Magic Packet three times, 250 ms apart.\r\n",
                    gDeviceConfig.pcIp, mac);
    } else if (strcmp(command->name, "pc ssh") == 0) {
      cmdPcSsh(channel, "");
    }
    return true;
  }

  channelPrintf(channel, "\r\nESP32 Recovery Gateway v%s - command reference\r\n",
                FIRMWARE_VERSION);
  const char* group = nullptr;
  for (const Command& command : kCommands) {
    if (group == nullptr || strcmp(group, command.group) != 0) {
      group = command.group;
      channelPrintf(channel, "\r\n%s\r\n", group);
    }
    channelPrintf(channel, "  %-20s %s\r\n", command.name, command.summary);
  }
  channelWrite(channel,
               "\r\nExamples:\r\n  help pc wake\r\n  help examples\r\n  logs 100\r\n");
  return true;
}

bool cmdStatus(ssh_channel channel, const char*) {
  writeDashboard(channel);
  return true;
}

bool cmdWatch(ssh_channel channel, const char*) {
  while (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
    channelWrite(channel, kClearScreen);
    writeDashboard(channel);
    channelPrintf(channel, "  %sPress any key to stop watching.%s\r\n", kAnsiDim,
                  kAnsiReset);
    const int available = ssh_channel_poll_timeout(channel, kWatchRefreshMs, 0);
    if (available < 0) {
      return false;
    }
    if (available > 0) {
      char scratch[64];
      ssh_channel_read_nonblocking(channel, scratch, sizeof(scratch), 0);
      break;
    }
  }
  return true;
}

bool cmdUptime(ssh_channel channel, const char*) {
  char uptime[32];
  formatUptime(uptime, sizeof(uptime));
  channelPrintf(channel, "\r\nUptime: %s\r\n", uptime);
  return true;
}

bool cmdVersion(ssh_channel channel, const char*) {
  channelPrintf(channel,
                "\r\nFirmware: esp32-s3-n16r8-bastion v%s (built %s %s)\r\n"
                "Board: %s  %s rev %d  flash %u MB  psram %u KB  cpu %u MHz\r\n"
                "SDK: %s\r\nlibssh: %s\r\nAuthorized key: %s\r\n",
                FIRMWARE_VERSION, __DATE__, __TIME__, FIRMWARE_TARGET_BOARD,
                ESP.getChipModel(), ESP.getChipRevision(),
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024U * 1024U)),
                static_cast<unsigned>(ESP.getPsramSize() / 1024U),
                static_cast<unsigned>(ESP.getCpuFreqMHz()), ESP.getSdkVersion(),
                ssh_version(0), authorizedKeyFingerprint);
  return true;
}

struct LogEmitState {
  ssh_channel channel;
  size_t skip;
  size_t index;
};

void emitLogLine(const char* line, void* userData) {
  LogEmitState* state = static_cast<LogEmitState*>(userData);
  if (state->index++ < state->skip) {
    return;
  }
  channelPrintf(state->channel, "  %s\r\n", line);
}

bool cmdLogs(ssh_channel channel, const char* args) {
  long wanted = *args != '\0' ? strtol(args, nullptr, 10) : 40;
  if (wanted < 1) {
    wanted = 40;
  }
  const size_t total = eventLogCount();
  LogEmitState state{channel, total > static_cast<size_t>(wanted)
                                  ? total - static_cast<size_t>(wanted)
                                  : 0,
                     0};
  channelPrintf(channel, "\r\n%sEvent journal (%u of %u lines, uptime seconds)%s\r\n",
                kAnsiDim, static_cast<unsigned>(total - state.skip),
                static_cast<unsigned>(total), kAnsiReset);
  eventLogForEach(emitLogLine, &state);
  return true;
}

bool cmdPcStatus(ssh_channel channel, const char*) {
  channelPrintf(channel, "\r\nMain PC %s:%u is %s.\r\n", gDeviceConfig.pcIp,
                gDeviceConfig.pcPort, mainPcReachable() ? "ONLINE" : "OFFLINE");
  return true;
}

bool cmdPcPing(ssh_channel channel, const char* args) {
  long count = *args != '\0' ? strtol(args, nullptr, 10) : 4;
  if (count < 1 || count > 20) {
    count = 4;
  }
  channelPrintf(channel, "\r\nPinging %s x%ld ...\r\n", gDeviceConfig.pcIp, count);
  PingStats stats;
  if (!mainPcPing(static_cast<uint8_t>(count), stats)) {
    channelWrite(channel, "Ping could not be started (no Wi-Fi?).\r\n");
    return true;
  }
  if (stats.received == 0) {
    channelPrintf(channel, "%lu sent, no replies - host is down or drops ICMP.\r\n",
                  static_cast<unsigned long>(stats.sent));
  } else {
    channelPrintf(channel, "%lu sent, %lu received  rtt min/avg/max %lu/%lu/%lu ms\r\n",
                  static_cast<unsigned long>(stats.sent),
                  static_cast<unsigned long>(stats.received),
                  static_cast<unsigned long>(stats.minMs),
                  static_cast<unsigned long>(stats.avgMs),
                  static_cast<unsigned long>(stats.maxMs));
  }
  return true;
}

bool cmdPcWake(ssh_channel channel, const char*) {
  if (mainPcReachable()) {
    channelWrite(channel, "\r\nMain PC is already online; SSH port is open.\r\n");
    return true;
  }
  if (!mainPcMacKnown()) {
    channelWrite(channel,
                 "\r\nThe PC's MAC address has not been learned yet.\r\n"
                 "It must appear on the network at least once (powered on) "
                 "before WoWLAN can target it.\r\n");
    return true;
  }
  const bool sent = mainPcWake();
  char mac[24];
  mainPcMacString(mac, sizeof(mac));
  eventLogf("SSH: pc wake -> %s (%s)", mac, sent ? "sent" : "failed");
  channelPrintf(channel, "\r\nWoWLAN packet %s to %s.\r\n", sent ? "sent" : "failed",
                mac);
  channelWrite(channel, "Run 'watch' or 'pc ping' while the PC resumes.\r\n");
  return true;
}

bool cmdPcSsh(ssh_channel channel, const char*) {
  const String tunnelIp = recoveryVpnAddress().toString();
  channelPrintf(
      channel,
      "\r\npc ssh - connect through this ESP32 bastion\r\n\r\n"
      "On the LAN:\r\n  %sssh -J %s@%s %s@%s%s\r\n\r\n"
      "If your device is a VPN client of %s (%s):\r\n"
      "  %sssh -J %s@%s %s@%s%s\r\n\r\n"
      "Without a VPN client (jump over the VPN server's sshd):\r\n"
      "  %sssh -J %s@%s:%u,%s@%s %s@%s%s\r\n\r\n"
      "Only destination %s:%u is permitted.\r\n",
      kAnsiCyan, gDeviceConfig.sshUser, WiFi.localIP().toString().c_str(),
      gDeviceConfig.sshUser, gDeviceConfig.pcIp, kAnsiReset,
      recoveryVpnActiveProfileName(), recoveryVpnEndpoint(), kAnsiCyan,
      gDeviceConfig.sshUser, tunnelIp.c_str(), gDeviceConfig.sshUser,
      gDeviceConfig.pcIp, kAnsiReset, kAnsiCyan, gDeviceConfig.sshUser,
      recoveryVpnEndpoint(), recoveryVpnServerSshPort(), gDeviceConfig.sshUser,
      tunnelIp.c_str(), gDeviceConfig.sshUser, gDeviceConfig.pcIp, kAnsiReset,
      gDeviceConfig.pcIp, gDeviceConfig.pcPort);
  return true;
}

bool cmdNetStatus(ssh_channel channel, const char*) {
  char wifiUptime[32];
  formatDuration(netMonitorWifiUptimeSeconds(), wifiUptime, sizeof(wifiUptime));
  channelPrintf(channel,
                "\r\nWi-Fi: %s\r\nSSID: %s\r\nBSSID: %s\r\nChannel: %d\r\n"
                "IP: %s\r\nGateway: %s\r\nRSSI: %d dBm\r\nAssociated for: %s\r\n"
                "Internet: %s\r\nState: %s\r\n",
                WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE",
                WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel(),
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(), WiFi.RSSI(), wifiUptime,
                recoveryInternetAvailable() ? "ONLINE" : "OFFLINE",
                recoveryNetworkStateName());
  return true;
}

bool cmdVpnStatus(ssh_channel channel, const char*) {
  const uint32_t age = recoveryVpnHandshakeAgeSeconds();
  channelPrintf(channel,
                "\r\nWireGuard: %s\r\nProfile: %s\r\nEndpoint: %s\r\nTunnel IP: %s\r\n"
                "Consecutive failures: %u\r\n",
                recoveryVpnStateName(), recoveryVpnActiveProfileName(),
                recoveryVpnEndpoint(), recoveryVpnAddress().toString().c_str(),
                recoveryVpnConsecutiveFailures());
  if (age == UINT32_MAX) {
    channelWrite(channel, "Latest handshake: not available\r\n");
  } else {
    channelPrintf(channel, "Latest handshake: %lu seconds ago\r\n",
                  static_cast<unsigned long>(age));
  }
  return true;
}

bool cmdVpnFailover(ssh_channel channel, const char*) {
  recoveryVpnRequestFailover();
  eventLogf("SSH: vpn failover requested by %s", peerAddress);
  channelWrite(channel, "\r\nVPN failover requested. Run 'vpn status' shortly.\r\n");
  return true;
}

bool cmdVpnRetryPrimary(ssh_channel channel, const char*) {
  recoveryVpnRequestPrimary();
  eventLogf("SSH: vpn retry-primary requested by %s", peerAddress);
  channelWrite(channel, "\r\nPrimary VPN retry requested. Run 'vpn status' shortly.\r\n");
  return true;
}

bool cmdReboot(ssh_channel channel, const char* args) {
  if (strcmp(args, "yes") != 0) {
    channelWrite(channel, "\r\nReboot the ESP32 now? Type `reboot yes` to confirm.\r\n");
    return true;
  }
  eventLogf("SSH: reboot requested by %s", peerAddress);
  channelWrite(channel, "\r\nRebooting. Reconnect in ~10 seconds.\r\n");
  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
  delay(300);
  ESP.restart();
  return false;
}

bool cmdExit(ssh_channel channel, const char*) {
  channelWrite(channel, "\r\nBye.\r\n");
  return false;
}

bool executeCommand(ssh_channel channel, String command) {
  command.trim();
  if (command.isEmpty()) {
    return true;
  }
  // Collapse repeated spaces so "pc   status" still matches.
  while (command.indexOf("  ") >= 0) {
    command.replace("  ", " ");
  }
  String lowered = command;
  lowered.toLowerCase();

  const char* args = nullptr;
  const Command* found = findCommand(lowered.c_str(), &args);
  if (found == nullptr) {
    channelPrintf(channel,
                  "\r\nUnknown command: %s\r\nType 'help' to see commands and examples.\r\n",
                  command.c_str());
    return true;
  }
  return found->handler(channel, args);
}

// ---------------------------------------------------------------------------
// Interactive shell: a tiny line editor with history and terminal escape
// sequence filtering (arrow keys used to leak "[A" into the command line).
// ---------------------------------------------------------------------------

class LineEditor {
 public:
  void reset() {
    line = "";
    escapeState = 0;
  }

  // Feeds one input byte. Returns true when a complete line is ready in
  // `line` (the caller executes it and then calls acceptLine()/reset()).
  // Sets `interrupt` for Ctrl+C and `eof` for Ctrl+D.
  bool feed(ssh_channel channel, char input, bool* interrupt, bool* eof) {
    *interrupt = false;
    *eof = false;

    if (escapeState == 1) {
      escapeState = (input == '[' || input == 'O') ? 2 : 0;
      return false;
    }
    if (escapeState == 2) {
      // CSI parameters are 0x30-0x3F, intermediates 0x20-0x2F; the final
      // byte is 0x40-0x7E.
      if (input >= 0x40 && input <= 0x7e) {
        escapeState = 0;
        if (input == 'A') {
          recall(channel, -1);
        } else if (input == 'B') {
          recall(channel, +1);
        }
      }
      return false;
    }

    switch (input) {
      case 0x1b:
        escapeState = 1;
        return false;
      case 3:  // Ctrl+C
        *interrupt = true;
        line = "";
        historyCursor = historyCount;
        return false;
      case 4:  // Ctrl+D
        *eof = true;
        return false;
      case 0x0c:  // Ctrl+L: redraw.
        channelWrite(channel, kClearScreen);
        channelWrite(channel, kPrompt);
        channelWrite(channel, line.c_str());
        return false;
      case 0x15:  // Ctrl+U: clear line.
        replaceLine(channel, "");
        return false;
      case '\r':
      case '\n':
        if (input == '\n' && line.isEmpty()) {
          return false;  // CRLF from the client: the CR already fired.
        }
        return true;
      case 8:
      case 127:
        if (!line.isEmpty()) {
          line.remove(line.length() - 1);
          channelWrite(channel, "\b \b");
        }
        return false;
      default:
        break;
    }
    if (input >= 32 && input <= 126 && line.length() < kMaxLineLength) {
      line += input;
      if (!writeAllToChannel(channel, reinterpret_cast<const uint8_t*>(&input),
                             1) &&
          ssh_channel_is_open(channel)) {
        ssh_channel_close(channel);
      }
    }
    return false;
  }

  void acceptLine() {
    if (!line.isEmpty() &&
        (historyCount == 0 || history[(historyCount - 1) % kHistoryDepth] != line)) {
      history[historyCount % kHistoryDepth] = line;
      ++historyCount;
    }
    historyCursor = historyCount;
    line = "";
  }

  String line;

 private:
  void replaceLine(ssh_channel channel, const String& text) {
    line = text;
    channelWrite(channel, "\r\x1b[K");
    channelWrite(channel, kPrompt);
    channelWrite(channel, line.c_str());
  }

  void recall(ssh_channel channel, int direction) {
    const uint32_t oldest =
        historyCount > kHistoryDepth ? historyCount - kHistoryDepth : 0;
    if (direction < 0) {
      if (historyCursor <= oldest) {
        return;
      }
      --historyCursor;
    } else {
      if (historyCursor >= historyCount) {
        return;
      }
      ++historyCursor;
    }
    replaceLine(channel, historyCursor == historyCount
                             ? String()
                             : history[historyCursor % kHistoryDepth]);
  }

  String history[kHistoryDepth];
  uint32_t historyCount = 0;
  uint32_t historyCursor = 0;
  uint8_t escapeState = 0;
};

void serveShell(ssh_channel channel) {
  writeDashboard(channel);
  channelWrite(channel, kPrompt);

  static LineEditor editor;  // Persists history across sessions.
  editor.reset();
  uint32_t lastActivityMs = millis();
  while (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
    const int available = ssh_channel_poll_timeout(channel, 1000, 0);
    if (available == 0) {
      if (millis() - lastActivityMs > kShellIdleTimeoutMs) {
        channelWrite(channel, "\r\nIdle timeout. Bye.\r\n");
        return;
      }
      continue;
    }
    if (available < 0) {
      return;
    }

    char buffer[64];
    const int count = ssh_channel_read_nonblocking(
        channel, buffer, min(available, static_cast<int>(sizeof(buffer))), 0);
    if (count <= 0) {
      break;
    }
    lastActivityMs = millis();

    for (int index = 0; index < count; ++index) {
      bool interrupt = false;
      bool eof = false;
      const bool ready = editor.feed(channel, buffer[index], &interrupt, &eof);
      if (eof) {
        channelWrite(channel, "\r\nBye.\r\n");
        return;
      }
      if (interrupt) {
        channelWrite(channel, "^C\r\n");
        channelWrite(channel, kPrompt);
        continue;
      }
      if (!ready) {
        continue;
      }
      channelWrite(channel, "\r\n");
      const bool keepRunning = executeCommand(channel, editor.line);
      editor.acceptLine();
      if (!keepRunning) {
        return;
      }
      channelWrite(channel, "\r\n");
      channelWrite(channel, kPrompt);
    }
  }
}

// ---------------------------------------------------------------------------
// direct-tcpip relay (the `ssh -J` bastion path)
// ---------------------------------------------------------------------------

// Sends the whole buffer to the PC socket, waiting (bounded) for the socket
// to become writable whenever the TCP send buffer is full.
bool sendAllToPc(int fd, const uint8_t* data, int length) {
  int offset = 0;
  uint32_t lastProgressMs = millis();
  while (offset < length) {
    const int written = send(fd, data + offset, length - offset, MSG_DONTWAIT);
    if (written > 0) {
      offset += written;
      lastProgressMs = millis();
      continue;
    }
    if (written < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
      return false;
    }
    if (millis() - lastProgressMs > kRelayWriteStallTimeoutMs) {
      return false;
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval wait = {1, 0};
    select(fd + 1, nullptr, &writable, nullptr, &wait);
  }
  return true;
}

int connectToPc() {
  IPAddress pcIp;
  if (!pcIp.fromString(gDeviceConfig.pcIp)) {
    return -1;
  }
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(gDeviceConfig.pcPort);
  address.sin_addr.s_addr = static_cast<uint32_t>(pcIp);

  // Non-blocking connect with a 2 s deadline, then back to blocking with
  // MSG_DONTWAIT used explicitly where needed.
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result < 0 && errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval wait = {2, 0};
    result = select(fd + 1, nullptr, &writable, nullptr, &wait);
    int error = 0;
    socklen_t errorLength = sizeof(error);
    if (result <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) != 0 ||
        error != 0) {
      result = -1;
    } else {
      result = 0;
    }
  }
  if (result != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, flags);

  int enable = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
  int idleSeconds = 30;
  int intervalSeconds = 5;
  int probeCount = 4;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSeconds, sizeof(idleSeconds));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSeconds, sizeof(intervalSeconds));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probeCount, sizeof(probeCount));
  return fd;
}

void relayDirectTcpip(ssh_channel channel, ssh_session session) {
  const int pcFd = connectToPc();
  if (pcFd < 0) {
    eventLogf("Relay: %s:%u refused/unreachable", gDeviceConfig.pcIp,
              gDeviceConfig.pcPort);
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    return;
  }
  const int sshFd = ssh_get_fd(session);

  // Blocking mode so ssh_channel_write() gives real backpressure instead of
  // silently buffering unboundedly in session->out_buffer (which ended in
  // "ssh_socket_write: Out of memory" under btop). Reads are unaffected:
  // ssh_channel_poll()/ssh_channel_read_nonblocking() always force their own
  // non-blocking mode regardless of this flag.
  ssh_channel_set_blocking(channel, 1);

  uint32_t lastActivityMs = millis();
  const uint32_t startedMs = lastActivityMs;
  uint64_t totalToPc = 0;
  uint64_t totalToClient = 0;
  const char* closeReason = "peer closed";
  // A client that closes its write side (SSH EOF) only means "no more input
  // coming from me" - the PC may still be mid-response. Stop forwarding
  // client->PC once that happens, but keep draining PC->client until the PC
  // itself disconnects or goes idle.
  bool clientDone = false;

  // Instead of polling both ends every millisecond, block in select() on the
  // two sockets until one of them has something to say. That removes the
  // ~1 ms wake-ups (CPU and latency) and lets a burst be forwarded in 16 KB
  // slices as fast as the two TCP stacks allow.
  while (ssh_channel_is_open(channel)) {
    // libssh may already hold decrypted-but-unread channel data from a packet
    // that arrived together with the previous one; consume it before
    // sleeping in select() or it would sit there until the next packet.
    int pendingFromClient = clientDone ? 0 : ssh_channel_poll(channel, 0);
    if (pendingFromClient == SSH_EOF) {
      pendingFromClient = 0;
    } else if (pendingFromClient < 0) {
      closeReason = "channel poll error";
      break;
    }

    // The SSH socket stays in the set even after the client's EOF: window
    // adjustments, keepalives and the eventual channel close still arrive
    // on it, and only processing them lets ssh_channel_is_open() notice
    // that the client has gone instead of waiting out the idle timeout.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(pcFd, &readable);
    FD_SET(sshFd, &readable);
    const int maxFd = max(pcFd, sshFd);
    timeval wait = {0, 0};
    if (pendingFromClient == 0) {
      wait.tv_sec = 1;
    }
    const int ready = select(maxFd + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0 && errno != EINTR) {
      // EBADF here is the normal end of a session: the client tore down the
      // TCP connection and libssh closed its socket underneath us.
      closeReason = errno == EBADF ? "client disconnected" : "select error";
      break;
    }

    bool transferred = false;

    if (clientDone && ready > 0 && FD_ISSET(sshFd, &readable)) {
      // Nothing to forward any more; just let libssh digest whatever the
      // client sent (most likely a channel close).
      if (ssh_channel_poll(channel, 0) == SSH_ERROR) {
        closeReason = "client disconnected";
        break;
      }
    }

    // Client -> PC.
    if (!clientDone && (pendingFromClient > 0 || (ready > 0 && FD_ISSET(sshFd, &readable)))) {
      const int count = ssh_channel_read_nonblocking(channel, relayToPc,
                                                     kRelayBufferSize, 0);
      if (count == SSH_ERROR) {
        // A read error on an otherwise healthy relay means the SSH session
        // itself went away (client closed the connection mid-stream).
        closeReason = ssh_channel_is_open(channel) ? "channel read error"
                                                   : "client disconnected";
        break;
      }
      if (count > 0) {
        if (!sendAllToPc(pcFd, relayToPc, count)) {
          closeReason = "write to PC stalled/failed";
          break;
        }
        totalToPc += count;
        transferred = true;
      }
    }
    if (!clientDone && ssh_channel_is_eof(channel)) {
      clientDone = true;
      // Half-close the PC side too (TCP FIN on our write direction) so a
      // program that only replies once it sees EOF on its input isn't left
      // waiting for more input that will never come.
      shutdown(pcFd, SHUT_WR);
    }

    // PC -> client.
    if (ready > 0 && FD_ISSET(pcFd, &readable)) {
      const int count = recv(pcFd, relayToClient, kRelayBufferSize, MSG_DONTWAIT);
      if (count == 0) {
        closeReason = "PC closed";
        break;
      }
      if (count < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        closeReason = "PC socket error";
        break;
      }
      if (count > 0) {
        if (!writeAllToChannel(channel, relayToClient, count)) {
          closeReason = "write to client stalled/failed";
          break;
        }
        totalToClient += count;
        transferred = true;
      }
    }

    if (transferred) {
      lastActivityMs = millis();
    } else if (millis() - lastActivityMs > kRelayIdleTimeoutMs) {
      closeReason = "idle timeout";
      break;
    }
  }

  const uint32_t elapsedMs = millis() - startedMs;
  eventLogf("Relay: closed (%s) after %lus, to-PC %llu B, to-client %llu B (%lu KB/s)",
            closeReason, static_cast<unsigned long>(elapsedMs / 1000UL),
            static_cast<unsigned long long>(totalToPc),
            static_cast<unsigned long long>(totalToClient),
            static_cast<unsigned long>(
                elapsedMs > 0 ? ((totalToPc + totalToClient) / elapsedMs) : 0));
  close(pcFd);
  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
}

// ---------------------------------------------------------------------------
// Session handling
// ---------------------------------------------------------------------------

bool ensureHostKey() {
  if (!SPIFFS.begin(true)) {
    eventLogf("SSH: failed to mount SPIFFS");
    return false;
  }
  if (SPIFFS.exists(kHostKeyFsPath)) {
    // A file existing isn't proof it is a loadable key - a prior brownout or
    // full flash could have left a truncated/empty one, which would only
    // surface later as ssh_bind_listen() failing on every boot with no
    // self-heal. Confirm it actually imports before trusting it.
    ssh_key existing = nullptr;
    const bool loads = ssh_pki_import_privkey_file(kHostKeyVfsPath, nullptr,
                                                   nullptr, nullptr,
                                                   &existing) == SSH_OK;
    if (existing != nullptr) {
      ssh_key_free(existing);
    }
    if (loads) {
      return true;
    }
    eventLogf("SSH: existing host key is unreadable, regenerating");
    SPIFFS.remove(kHostKeyFsPath);
  }

  eventLogf("SSH: generating unique Ed25519 host key");
  ssh_key key = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK || key == nullptr) {
    eventLogf("SSH: host key generation failed");
    return false;
  }
  const int result =
      ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, kHostKeyVfsPath);
  ssh_key_free(key);
  if (result != SSH_OK) {
    eventLogf("SSH: failed to store host key");
    return false;
  }
  return true;
}

void describePeer(ssh_session session) {
  snprintf(peerAddress, sizeof(peerAddress), "unknown");
  const socket_t fd = ssh_get_fd(session);
  if (fd == SSH_INVALID_SOCKET) {
    return;
  }
  sockaddr_in address = {};
  socklen_t length = sizeof(address);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    snprintf(peerAddress, sizeof(peerAddress), "%s:%u",
             inet_ntoa(address.sin_addr), ntohs(address.sin_port));
  }
}

void hardenSessionTransport(ssh_session session) {
  long timeoutSeconds = kSessionIoTimeoutSeconds;
  ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds);

  const socket_t fd = ssh_get_fd(session);
  if (fd != SSH_INVALID_SOCKET) {
    int enable = 1;
    int idleSeconds = 30;
    int intervalSeconds = 5;
    int probeCount = 4;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSeconds, sizeof(idleSeconds));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSeconds,
               sizeof(intervalSeconds));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probeCount, sizeof(probeCount));
    // Interactive keystrokes and small TUI updates must not wait for Nagle's
    // 200 ms coalescing timer; libssh already batches a full SSH packet per
    // write, so there is nothing left for Nagle to gain here.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
  }
}

bool authenticateSession(ssh_session session) {
  for (uint8_t attempt = 0; attempt < kMaxAuthMessages; ++attempt) {
    ssh_message message = ssh_message_get(session);
    if (message == nullptr) {
      return false;
    }

    bool accepted = false;
    bool replied = false;
    if (ssh_message_type(message) == SSH_REQUEST_AUTH &&
        ssh_message_subtype(message) == SSH_AUTH_METHOD_PUBLICKEY &&
        strcmp(ssh_message_auth_user(message), gDeviceConfig.sshUser) == 0) {
      ssh_key offeredKey = ssh_message_auth_pubkey(message);
      if (offeredKey != nullptr &&
          ssh_key_cmp(offeredKey, authorizedKey, SSH_KEY_CMP_PUBLIC) == 0) {
        const ssh_publickey_state_e state = ssh_message_auth_publickey_state(message);
        if (state == SSH_PUBLICKEY_STATE_NONE) {
          ssh_message_auth_reply_pk_ok_simple(message);
          replied = true;
        } else if (state == SSH_PUBLICKEY_STATE_VALID) {
          ssh_message_auth_reply_success(message, 0);
          accepted = true;
          replied = true;
        }
      }
    }

    if (!replied) {
      ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PUBLICKEY);
      ssh_message_reply_default(message);
    }
    ssh_message_free(message);
    if (accepted) {
      return true;
    }
  }
  return false;
}

void handleAuthenticatedSession(ssh_session session) {
  const uint32_t startedMs = millis();
  while (millis() - startedMs < kPreShellDeadlineMs) {
    ssh_message message = ssh_message_get(session);
    if (message == nullptr) {
      return;
    }

    if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN) {
      const int subtype = ssh_message_subtype(message);
      if (subtype == SSH_CHANNEL_SESSION) {
        ssh_channel channel = ssh_message_channel_request_open_reply_accept(message);
        ssh_message_free(message);
        if (channel == nullptr) {
          return;
        }

        bool shellRequested = false;
        while (!shellRequested && millis() - startedMs < kPreShellDeadlineMs) {
          ssh_message request = ssh_message_get(session);
          if (request == nullptr) {
            ssh_channel_free(channel);
            return;
          }
          if (ssh_message_type(request) == SSH_REQUEST_CHANNEL) {
            const int requestType = ssh_message_subtype(request);
            if (requestType == SSH_CHANNEL_REQUEST_PTY ||
                requestType == SSH_CHANNEL_REQUEST_ENV ||
                requestType == SSH_CHANNEL_REQUEST_WINDOW_CHANGE) {
              ssh_message_channel_request_reply_success(request);
            } else if (requestType == SSH_CHANNEL_REQUEST_SHELL) {
              ssh_message_channel_request_reply_success(request);
              shellRequested = true;
            } else {
              ssh_message_reply_default(request);
            }
          } else {
            ssh_message_reply_default(request);
          }
          ssh_message_free(request);
        }
        if (shellRequested) {
          eventLogf("SSH: console session from %s", peerAddress);
          serveShell(channel);
        }
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return;
      }

      if (subtype == SSH_CHANNEL_DIRECT_TCPIP) {
        const char* destination = ssh_message_channel_request_open_destination(message);
        const int destinationPort =
            ssh_message_channel_request_open_destination_port(message);
        if (destination != nullptr && strcmp(destination, gDeviceConfig.pcIp) == 0 &&
            destinationPort == gDeviceConfig.pcPort) {
          ssh_channel channel = ssh_message_channel_request_open_reply_accept(message);
          ssh_message_free(message);
          if (channel != nullptr) {
            eventLogf("SSH: bastion relay from %s to %s:%d", peerAddress,
                      destination, destinationPort);
            relayDirectTcpip(channel, session);
            ssh_channel_free(channel);
          }
          return;
        }
        eventLogf("SSH: rejected direct-tcpip to %s:%d from %s",
                  destination != nullptr ? destination : "?", destinationPort,
                  peerAddress);
      }
    }

    ssh_message_reply_default(message);
    ssh_message_free(message);
  }
}

bool importAuthorizedKey() {
  const enum ssh_keytypes_e keyType =
      ssh_key_type_from_name(gDeviceConfig.sshKeyType);
  if (keyType == SSH_KEYTYPE_UNKNOWN ||
      ssh_pki_import_pubkey_base64(gDeviceConfig.sshKeyBase64, keyType,
                                   &authorizedKey) != SSH_OK) {
    return false;
  }

  unsigned char* hash = nullptr;
  size_t hashLength = 0;
  if (ssh_get_publickey_hash(authorizedKey, SSH_PUBLICKEY_HASH_SHA256, &hash,
                             &hashLength) == SSH_OK) {
    char* fingerprint =
        ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
    ssh_clean_pubkey_hash(&hash);
    if (fingerprint != nullptr) {
      snprintf(authorizedKeyFingerprint, sizeof(authorizedKeyFingerprint), "%s",
               fingerprint);
      ssh_string_free_char(fingerprint);
    }
  }
  return true;
}

// Applies the fixed bind options and starts listening. Used both for the
// initial bind and to rebuild one that has started failing every accept.
bool configureAndListen(ssh_bind bind) {
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, kBindAddress);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT_STR, kBindPort);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY, kHostKeyVfsPath);
#ifndef BASTION_BENCH_ALL_CIPHERS
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_CIPHERS_C_S, kCiphers);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_CIPHERS_S_C, kCiphers);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HMAC_C_S, kMacs);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HMAC_S_C, kMacs);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_KEY_EXCHANGE, kKex);
#endif
  int verbosity = SSH_LOG_WARN;
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &verbosity);
  if (ssh_bind_listen(bind) != SSH_OK) {
    eventLogf("SSH: listen failed: %s", ssh_get_error(bind));
    return false;
  }
  return true;
}

uint8_t* allocateRelayBuffer() {
  uint8_t* buffer = static_cast<uint8_t*>(
      heap_caps_malloc(kRelayBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<uint8_t*>(heap_caps_malloc(kRelayBufferSize, MALLOC_CAP_8BIT));
  }
  return buffer;
}

void sshServerTask(void*) {
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  libssh_begin();
  relayToPc = allocateRelayBuffer();
  relayToClient = allocateRelayBuffer();
  if (relayToPc == nullptr || relayToClient == nullptr || !ensureHostKey() ||
      !importAuthorizedKey()) {
    eventLogf("SSH: initialization failed");
    vTaskDelete(nullptr);
    return;
  }

  ssh_bind bind = ssh_bind_new();
  if (bind == nullptr) {
    eventLogf("SSH: ssh_bind_new failed");
    vTaskDelete(nullptr);
    return;
  }
  if (!configureAndListen(bind)) {
    ssh_bind_free(bind);
    vTaskDelete(nullptr);
    return;
  }
  eventLogf("SSH: listening on %s:%s as %s (key %s)",
            WiFi.localIP().toString().c_str(), kBindPort, gDeviceConfig.sshUser,
            authorizedKeyFingerprint);

  // ssh_bind_accept() failing over and over (rather than just once, which is
  // routine for a dropped/reset peer) means the listening socket itself has
  // wedged - e.g. after a Wi-Fi interface reset. Recreating the bind gives it
  // a chance to self-heal instead of requiring a power cycle.
  constexpr uint8_t kMaxConsecutiveAcceptFailures = 10;
  uint8_t consecutiveAcceptFailures = 0;

  while (true) {
    ssh_session session = ssh_new();
    if (session == nullptr) {
      delay(1000);
      continue;
    }
    if (ssh_bind_accept(bind, session) == SSH_OK) {
      consecutiveAcceptFailures = 0;
      describePeer(session);
      hardenSessionTransport(session);
      if (ssh_handle_key_exchange(session) != SSH_OK) {
        eventLogf("SSH: key exchange with %s failed: %s", peerAddress,
                  ssh_get_error(session));
      } else if (!authenticateSession(session)) {
        authFailures.fetch_add(1);
        eventLogf("SSH: authentication failed from %s (%lu total)", peerAddress,
                  static_cast<unsigned long>(authFailures.load()));
      } else {
        sessionsServed.fetch_add(1);
        eventLogf("SSH: %s authenticated (%s %s)", peerAddress,
                  ssh_get_cipher_in(session) != nullptr ? ssh_get_cipher_in(session) : "?",
                  ssh_get_kex_algo(session) != nullptr ? ssh_get_kex_algo(session) : "?");
        handleAuthenticatedSession(session);
#ifdef BASTION_STACK_DIAG
        eventLogf("SSH: task stack headroom %u B",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
      }
    } else {
      eventLogf("SSH: accept failed: %s", ssh_get_error(bind));
      if (++consecutiveAcceptFailures >= kMaxConsecutiveAcceptFailures) {
        eventLogf("SSH: too many consecutive accept failures, rebuilding listener");
        ssh_bind_free(bind);
        bind = ssh_bind_new();
        if (bind == nullptr || !configureAndListen(bind)) {
          eventLogf("SSH: failed to rebuild listener");
          if (bind != nullptr) {
            ssh_bind_free(bind);
          }
          ssh_disconnect(session);
          ssh_free(session);
          vTaskDelete(nullptr);
          return;
        }
        consecutiveAcceptFailures = 0;
      }
    }
    ssh_disconnect(session);
    ssh_free(session);
    delay(20);
  }
}
}  // namespace

void startRecoverySshServer() {
  if (xTaskCreatePinnedToCore(sshServerTask, "recovery-ssh", kSshTaskStack,
                              nullptr, 2, nullptr, 0) != pdPASS) {
    eventLogf("SSH: failed to create server task");
  }
}
