#include "recovery_ssh.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh_esp32.h>
#include <lwip/sockets.h>

#include "device_config.h"
#include "main_pc.h"
#include "recovery_status.h"
#include "recovery_vpn.h"

namespace {
constexpr char kHostKeyFsPath[] = "/ssh_host_ed25519_key";
constexpr char kHostKeyVfsPath[] = "/spiffs/ssh_host_ed25519_key";
constexpr char kBindAddress[] = "0.0.0.0";
constexpr char kBindPort[] = "22";
constexpr uint32_t kSshTaskStack = 32768;

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
// sshd port of the VPN server itself; only used to render copy-paste help
// commands for jumping over it without a local VPN client.
constexpr uint16_t kVpnServerSshPort = 8326;

constexpr char kAnsiReset[] = "\x1b[0m";
constexpr char kAnsiBold[] = "\x1b[1m";
constexpr char kAnsiDim[] = "\x1b[90m";
constexpr char kAnsiGreen[] = "\x1b[32m";
constexpr char kAnsiYellow[] = "\x1b[33m";
constexpr char kAnsiRed[] = "\x1b[31m";
constexpr char kAnsiCyan[] = "\x1b[36m";
constexpr char kPrompt[] = "\x1b[1;32mrecovery>\x1b[0m ";

ssh_key authorizedKey = nullptr;
char authorizedKeyFingerprint[64] = "unknown";

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
    const int written =
        ssh_channel_write(channel, data + offset, length - offset);
    if (written == SSH_ERROR) {
      Serial.printf("SSH: channel write error: %s\n",
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
// centrally) makes every subsequent ssh_channel_is_open() check in the shell
// loop and in writeAllToChannel() itself false immediately, instead of each
// remaining dashboard row/prompt write separately re-discovering the same
// dead channel through its own 30 s stall timeout.
void channelWrite(ssh_channel channel, const char* text) {
  if (channel != nullptr && text != nullptr) {
    if (!writeAllToChannel(channel, reinterpret_cast<const uint8_t*>(text),
                           static_cast<int>(strlen(text))) &&
        ssh_channel_is_open(channel)) {
      ssh_channel_close(channel);
    }
  }
}

void channelPrintf(ssh_channel channel, const char* format, ...) {
  char buffer[768];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  channelWrite(channel, buffer);
}

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

void formatUptime(char* output, size_t outputSize) {
  uint64_t seconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL;
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

void statusRow(ssh_channel channel, const char* label, const char* color,
               const char* state, const char* detail) {
  channelPrintf(channel, "  %-10s %s● %-9s%s %s\r\n", label, color, state,
                kAnsiReset, detail != nullptr ? detail : "");
}

void writeDashboard(ssh_channel channel) {
  char uptime[32];
  formatUptime(uptime, sizeof(uptime));
  const bool pcOnline = mainPcReachable();
  const bool wifiOnline = WiFi.status() == WL_CONNECTED;
  const bool internetOnline = recoveryInternetAvailable();
  const bool vpnOnline = recoveryVpnOnline();
  const uint32_t handshakeAge = recoveryVpnHandshakeAgeSeconds();
  const uint8_t vpnFailures = recoveryVpnConsecutiveFailures();

  char detail[128];
  channelPrintf(channel,
                "\r\n  %sESP32 Recovery Gateway%s\r\n"
                "  ────────────────────────────────────────────────\r\n",
                kAnsiBold, kAnsiReset);

  snprintf(detail, sizeof(detail), "%suptime %s%s", kAnsiDim, uptime, kAnsiReset);
  statusRow(channel, "Device", kAnsiGreen, "ONLINE", detail);

  snprintf(detail, sizeof(detail), "%s  %s%d dBm%s", WiFi.SSID().c_str(),
           kAnsiDim, WiFi.RSSI(), kAnsiReset);
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
                "  %-10s   %sheap %u KB  psram %u KB  reset: %s%s\r\n",
                "Memory", kAnsiDim,
                static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                static_cast<unsigned>(ESP.getFreePsram() / 1024),
                resetReasonName(esp_reset_reason()), kAnsiReset);

  channelPrintf(channel,
                "  ────────────────────────────────────────────────\r\n"
                "  %shelp%s commands   %spc ssh%s how to reach the PC   %spc wake%s wake it up\r\n\r\n",
                kAnsiCyan, kAnsiReset, kAnsiCyan, kAnsiReset, kAnsiCyan,
                kAnsiReset);
}

void writeHelp(ssh_channel channel) {
  channelWrite(channel,
      "\r\nESP32 Recovery Gateway - command reference\r\n\r\n"
      "STATUS\r\n"
      "  status                 Show the complete dashboard\r\n"
      "  uptime                 Show device uptime\r\n"
      "  version                Show firmware and key fingerprint\r\n\r\n"
      "MAIN PC\r\n"
      "  pc status              Check the configured PC's SSH port\r\n"
      "  pc wake                Send WoWLAN Magic Packet\r\n"
      "  pc ssh                 Show ready-to-use connect commands\r\n\r\n"
      "NETWORK\r\n"
      "  net status             Show Wi-Fi and internet state\r\n\r\n"
      "VPN\r\n"
      "  vpn status             Show tunnel and handshake state\r\n"
      "  vpn failover           Switch to the other profile\r\n"
      "  vpn retry-primary      Switch to server-1\r\n\r\n"
      "HELP\r\n"
      "  help                   Show this command list\r\n"
      "  help <command>         Show details and examples\r\n"
      "  help examples          Show common recovery scenarios\r\n"
      "  exit, quit             Close the SSH session\r\n\r\n"
      "Examples:\r\n"
      "  help pc wake\r\n"
      "  help pc ssh\r\n"
      "  pc status\r\n");
}

void writeDetailedHelp(ssh_channel channel, const String& topic) {
  if (topic == "pc wake") {
    char mac[24];
    mainPcMacString(mac, sizeof(mac));
    channelPrintf(channel,
        "\r\npc wake - wake the main PC over Wi-Fi\r\n\r\n"
        "Usage: pc wake\r\n"
        "Target: %s, broadcast on the local subnet\r\n"
        "MAC: %s\r\n\r\n"
        "Sends the Magic Packet three times, 250 ms apart.\r\n"
        "Example:\r\n  pc status\r\n  pc wake\r\n  pc status\r\n",
        gDeviceConfig.pcIp, mac);
  } else if (topic == "pc ssh") {
    const String tunnelIp = recoveryVpnAddress().toString();
    channelPrintf(channel,
        "\r\npc ssh - connect through this ESP32 bastion\r\n\r\n"
        "If your device is a VPN client of %s (%s):\r\n"
        "  %sssh -J %s@%s %s@%s%s\r\n\r\n"
        "Without a VPN client (jump over the VPN server's sshd):\r\n"
        "  %sssh -J %s@%s:%u,%s@%s %s@%s%s\r\n\r\n"
        "Only destination %s:%u is permitted.\r\n",
        recoveryVpnActiveProfileName(), recoveryVpnEndpoint(), kAnsiCyan,
        gDeviceConfig.sshUser, tunnelIp.c_str(), gDeviceConfig.sshUser,
        gDeviceConfig.pcIp, kAnsiReset, kAnsiCyan, gDeviceConfig.sshUser,
        recoveryVpnEndpoint(), kVpnServerSshPort, gDeviceConfig.sshUser,
        tunnelIp.c_str(), gDeviceConfig.sshUser, gDeviceConfig.pcIp, kAnsiReset,
        gDeviceConfig.pcIp, gDeviceConfig.pcPort);
  } else if (topic == "status") {
    channelWrite(channel,
        "\r\nstatus - show the complete recovery dashboard\r\n\r\n"
        "Usage: status\r\n"
        "Checks Wi-Fi, internet state, memory and the main PC's SSH port.\r\n");
  } else if (topic == "examples") {
    channelWrite(channel,
        "\r\nCommon recovery scenarios\r\n\r\n"
        "PC asleep:\r\n  pc status\r\n  pc wake\r\n  pc status\r\n\r\n"
        "Main VPN failed:\r\n  status\r\n  pc status\r\n  pc ssh\r\n"
        "  Then run the displayed ProxyJump command locally.\r\n");
  } else {
    channelPrintf(channel,
                  "\r\nNo detailed help for '%s'. Type 'help' for all commands.\r\n",
                  topic.c_str());
  }
}

void sendWakePacket(ssh_channel channel) {
  if (mainPcReachable()) {
    channelWrite(channel, "\r\nMain PC is already online; SSH port is open.\r\n");
    return;
  }
  if (!mainPcMacKnown()) {
    channelWrite(channel,
                "\r\nThe PC's MAC address has not been learned yet.\r\n"
                "It must appear on the network at least once (powered on) "
                "before WoWLAN can target it.\r\n");
    return;
  }

  const bool sent = mainPcWake();
  char mac[24];
  mainPcMacString(mac, sizeof(mac));
  channelPrintf(channel, "\r\nWoWLAN packet %s to %s.\r\n",
                sent ? "sent" : "failed", mac);
  channelWrite(channel, "Run 'pc status' after the PC has had time to resume.\r\n");
}

bool executeCommand(ssh_channel channel, String command) {
  command.trim();
  command.toLowerCase();

  if (command.isEmpty()) {
    return true;
  }
  if (command == "help" || command == "?") {
    writeHelp(channel);
  } else if (command.startsWith("help ")) {
    writeDetailedHelp(channel, command.substring(5));
  } else if (command == "status") {
    writeDashboard(channel);
  } else if (command == "uptime") {
    char uptime[32];
    formatUptime(uptime, sizeof(uptime));
    channelPrintf(channel, "\r\nUptime: %s\r\n", uptime);
  } else if (command == "version") {
    channelPrintf(channel,
                  "\r\nFirmware: recovery-access bastion\r\nAuthorized key: %s\r\n",
                  authorizedKeyFingerprint);
  } else if (command == "pc status") {
    channelPrintf(channel, "\r\nMain PC %s:%u is %s.\r\n", gDeviceConfig.pcIp,
                  gDeviceConfig.pcPort, mainPcReachable() ? "ONLINE" : "OFFLINE");
  } else if (command == "pc wake") {
    sendWakePacket(channel);
  } else if (command == "pc ssh") {
    writeDetailedHelp(channel, "pc ssh");
  } else if (command == "net status") {
    channelPrintf(channel,
                  "\r\nWi-Fi: %s\r\nSSID: %s\r\nIP: %s\r\nRSSI: %d dBm\r\n"
                  "Internet: %s\r\nState: %s\r\n",
                  WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  recoveryInternetAvailable() ? "ONLINE" : "OFFLINE",
                  recoveryNetworkStateName());
  } else if (command == "vpn status") {
    const uint32_t age = recoveryVpnHandshakeAgeSeconds();
    channelPrintf(channel,
                  "\r\nWireGuard: %s\r\nProfile: %s\r\nTunnel IP: %s\r\n"
                  "Consecutive failures: %u\r\n",
                  recoveryVpnStateName(), recoveryVpnActiveProfileName(),
                  recoveryVpnAddress().toString().c_str(),
                  recoveryVpnConsecutiveFailures());
    if (age == UINT32_MAX) {
      channelWrite(channel, "Latest handshake: not available\r\n");
    } else {
      channelPrintf(channel, "Latest handshake: %lu seconds ago\r\n",
                    static_cast<unsigned long>(age));
    }
  } else if (command == "vpn failover") {
    recoveryVpnRequestFailover();
    channelWrite(channel, "\r\nVPN failover requested. Run 'vpn status' shortly.\r\n");
  } else if (command == "vpn retry-primary") {
    recoveryVpnRequestPrimary();
    channelWrite(channel, "\r\nPrimary VPN retry requested. Run 'vpn status' shortly.\r\n");
  } else if (command == "exit" || command == "quit" || command == "logout") {
    channelWrite(channel, "\r\nBye.\r\n");
    return false;
  } else {
    channelPrintf(channel,
                  "\r\nUnknown command: %s\r\nType 'help' to see commands and examples.\r\n",
                  command.c_str());
  }
  return true;
}

bool ensureHostKey() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SSH: failed to mount SPIFFS");
    return false;
  }
  if (SPIFFS.exists(kHostKeyFsPath)) {
    // A file existing isn't proof it is a loadable key - a prior brownout or
    // full flash could have left a truncated/empty one, which would only
    // surface later as ssh_bind_listen() failing on every boot with no
    // self-heal, disabling the recovery console until someone re-enters the
    // setup portal in person. Confirm it actually imports before trusting it.
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
    Serial.println("SSH: existing host key is unreadable, regenerating");
    SPIFFS.remove(kHostKeyFsPath);
  }

  Serial.println("SSH: generating unique Ed25519 host key");
  ssh_key key = nullptr;
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK || key == nullptr) {
    Serial.println("SSH: host key generation failed");
    return false;
  }
  const int result =
      ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, kHostKeyVfsPath);
  ssh_key_free(key);
  if (result != SSH_OK) {
    Serial.println("SSH: failed to store host key");
    return false;
  }
  return true;
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

void serveShell(ssh_channel channel) {
  writeDashboard(channel);
  channelWrite(channel, kPrompt);

  String line;
  line.reserve(128);
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
    const int count = ssh_channel_read(
        channel, buffer, min(available, static_cast<int>(sizeof(buffer))), 0);
    if (count <= 0) {
      break;
    }
    lastActivityMs = millis();

    for (int index = 0; index < count; ++index) {
      const char input = buffer[index];
      if (input == 3) {  // Ctrl+C
        line = "";
        channelWrite(channel, "^C\r\n");
        channelWrite(channel, kPrompt);
      } else if (input == 4) {  // Ctrl+D
        channelWrite(channel, "\r\nBye.\r\n");
        return;
      } else if (input == '\r' || input == '\n') {
        if (input == '\n' && line.isEmpty()) {
          continue;
        }
        channelWrite(channel, "\r\n");
        const bool keepRunning = executeCommand(channel, line);
        line = "";
        if (!keepRunning) {
          return;
        }
        channelWrite(channel, "\r\n");
        channelWrite(channel, kPrompt);
      } else if (input == 8 || input == 127) {
        if (!line.isEmpty()) {
          line.remove(line.length() - 1);
          channelWrite(channel, "\b \b");
        }
      } else if (input >= 32 && input <= 126 && line.length() < 127) {
        line += input;
        if (!writeAllToChannel(channel, reinterpret_cast<const uint8_t*>(&input),
                               1) &&
            ssh_channel_is_open(channel)) {
          ssh_channel_close(channel);
        }
      }
    }
  }
}

// Mirrors writeAllToChannel() above but for the WiFiClient side of the relay.
bool writeAllToTarget(WiFiClient& target, const uint8_t* data, int length) {
  int offset = 0;
  uint32_t lastProgressMs = millis();
  while (offset < length) {
    if (!target.connected()) {
      return false;
    }
    const size_t written = target.write(data + offset, length - offset);
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

void relayDirectTcpip(ssh_channel channel) {
  WiFiClient target;
  IPAddress pcIp;
  pcIp.fromString(gDeviceConfig.pcIp);
  if (!target.connect(pcIp, gDeviceConfig.pcPort, 2000)) {
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    return;
  }
  target.setNoDelay(true);

  // Blocking mode so ssh_channel_write() below gives real backpressure
  // instead of silently buffering unboundedly (see docs/recovery-access-
  // architecture.md, "Reliability", for why). Reads are unaffected: the
  // ssh_channel_poll/ssh_channel_read_nonblocking calls further down always
  // force their own non-blocking mode regardless of this flag.
  ssh_channel_set_blocking(channel, 1);
  uint8_t buffer[1024];
  uint32_t lastActivityMs = millis();
  uint64_t totalToTarget = 0;
  uint64_t totalToChannel = 0;
  const char* closeReason = "peer closed";
  // A client that closes its write side (SSH EOF) only means "no more input
  // coming from me" - the PC may still be mid-response. Stop forwarding
  // client->PC once that happens, but keep draining PC->client until the PC
  // itself disconnects or goes idle, instead of tearing down both directions
  // immediately and discarding whatever the PC was about to send back.
  bool clientDone = false;
  while (target.connected() && ssh_channel_is_open(channel)) {
    bool transferred = false;
    if (!clientDone && ssh_channel_is_eof(channel)) {
      clientDone = true;
      // Half-close the PC side too (TCP FIN on our write direction) so a
      // program that only replies once it sees EOF on its input isn't left
      // waiting for more input that will never come.
      shutdown(target.fd(), SHUT_WR);
    }
    if (!clientDone) {
      const int sshAvailable = ssh_channel_poll(channel, 0);
      if (sshAvailable < 0) {
        closeReason = "channel poll error";
        break;
      }
      if (sshAvailable > 0) {
        const int count = ssh_channel_read_nonblocking(
            channel, buffer, min(sshAvailable, static_cast<int>(sizeof(buffer))), 0);
        if (count > 0) {
          if (!writeAllToTarget(target, buffer, count)) {
            closeReason = "write to PC stalled/failed";
            break;
          }
          totalToTarget += count;
          transferred = true;
        }
      }
    }

    const int targetAvailable = target.available();
    if (targetAvailable > 0) {
      const int count = target.read(buffer, min(targetAvailable, static_cast<int>(sizeof(buffer))));
      if (count > 0) {
        if (!writeAllToChannel(channel, buffer, count)) {
          closeReason = "write to client stalled/failed";
          break;
        }
        totalToChannel += count;
        transferred = true;
      }
    }

    if (transferred) {
      lastActivityMs = millis();
    } else if (millis() - lastActivityMs > kRelayIdleTimeoutMs) {
      closeReason = "idle timeout";
      break;
    }
    delay(1);
  }

  Serial.printf("Relay: closed (%s), bytes to-PC=%llu to-client=%llu\n",
                closeReason, static_cast<unsigned long long>(totalToTarget),
                static_cast<unsigned long long>(totalToChannel));
  target.stop();
  ssh_channel_send_eof(channel);
  ssh_channel_close(channel);
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
            relayDirectTcpip(channel);
            ssh_channel_free(channel);
          }
          return;
        }
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
  int verbosity = SSH_LOG_WARN;
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &verbosity);
  if (ssh_bind_listen(bind) != SSH_OK) {
    Serial.printf("SSH: listen failed: %s\n", ssh_get_error(bind));
    return false;
  }
  return true;
}

void sshServerTask(void*) {
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  libssh_begin();
  if (!ensureHostKey() || !importAuthorizedKey()) {
    Serial.println("SSH: initialization failed");
    vTaskDelete(nullptr);
    return;
  }

  ssh_bind bind = ssh_bind_new();
  if (bind == nullptr) {
    Serial.println("SSH: ssh_bind_new failed");
    vTaskDelete(nullptr);
    return;
  }
  if (!configureAndListen(bind)) {
    ssh_bind_free(bind);
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("SSH: listening on %s:%s as %s\n", WiFi.localIP().toString().c_str(),
                kBindPort, gDeviceConfig.sshUser);

  // ssh_bind_accept() failing over and over (rather than just once, which is
  // routine for a dropped/reset peer) means the listening socket itself has
  // wedged - e.g. after a Wi-Fi interface reset. Without this, the task would
  // spin accept-fail/disconnect/free forever, permanently and silently
  // disabling the recovery console. Recreating the bind gives it a chance to
  // self-heal instead of requiring a power cycle.
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
      hardenSessionTransport(session);
      if (ssh_handle_key_exchange(session) == SSH_OK &&
          authenticateSession(session)) {
        handleAuthenticatedSession(session);
      }
    } else {
      Serial.printf("SSH: accept failed: %s\n", ssh_get_error(bind));
      if (++consecutiveAcceptFailures >= kMaxConsecutiveAcceptFailures) {
        Serial.println("SSH: too many consecutive accept failures, rebuilding listener");
        ssh_bind_free(bind);
        bind = ssh_bind_new();
        if (bind == nullptr || !configureAndListen(bind)) {
          Serial.println("SSH: failed to rebuild listener");
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
    Serial.println("SSH: failed to create server task");
  }
}
