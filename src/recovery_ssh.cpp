#include "recovery_ssh.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh_esp32.h>
#include <lwip/sockets.h>

#include "authorized_key.h"
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
constexpr uint32_t kRelayWriteStallTimeoutMs = 10000;
constexpr uint8_t kMaxAuthMessages = 16;

const IPAddress kMainPcIp(10, 10, 10, 200);
const IPAddress kLanBroadcast(10, 10, 10, 255);
constexpr uint16_t kMainPcSshPort = 22;
constexpr uint8_t kMainPcMac[] = {0x50, 0x2e, 0x91, 0x8d, 0x24, 0x5a};
// sshd port of both VPN servers; only used to render copy-paste help commands.
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

void channelWrite(ssh_channel channel, const char* text) {
  if (channel != nullptr && text != nullptr) {
    ssh_channel_write(channel, text, strlen(text));
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

bool mainPcSshReachable(uint32_t timeoutMs = 700) {
  WiFiClient client;
  const bool connected = client.connect(kMainPcIp, kMainPcSshPort, timeoutMs);
  client.stop();
  return connected;
}

void statusRow(ssh_channel channel, const char* label, const char* color,
               const char* state, const char* detail) {
  channelPrintf(channel, "  %-10s %s● %-9s%s %s\r\n", label, color, state,
                kAnsiReset, detail != nullptr ? detail : "");
}

void writeDashboard(ssh_channel channel) {
  char uptime[32];
  formatUptime(uptime, sizeof(uptime));
  const bool pcOnline = mainPcSshReachable();
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

  snprintf(detail, sizeof(detail), "192.168.1.200  %sssh :22 %s%s", kAnsiDim,
           pcOnline ? "open" : "closed", kAnsiReset);
  statusRow(channel, "Main PC", pcOnline ? kAnsiGreen : kAnsiRed,
            pcOnline ? "ONLINE" : "OFFLINE", detail);

  snprintf(detail, sizeof(detail), "%sAA:BB:CC:DD:EE:FF%s", kAnsiDim, kAnsiReset);
  statusRow(channel, "WoWLAN", pcOnline ? kAnsiDim : kAnsiGreen,
            pcOnline ? "STANDBY" : "READY", detail);

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
      "  pc status              Check 192.168.1.200:22\r\n"
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
    channelWrite(channel,
        "\r\npc wake - wake the main Linux PC over Wi-Fi\r\n\r\n"
        "Usage: pc wake\r\n"
        "Target: 192.168.1.200, broadcast 192.168.1.255\r\n"
        "MAC: AA:BB:CC:DD:EE:FF\r\n\r\n"
        "Sends the Magic Packet three times, 250 ms apart.\r\n"
        "Example:\r\n  pc status\r\n  pc wake\r\n  pc status\r\n");
  } else if (topic == "pc ssh") {
    const String tunnelIp = recoveryVpnAddress().toString();
    channelPrintf(channel,
        "\r\npc ssh - connect through this ESP32 bastion\r\n\r\n"
        "If your device is a VPN client of %s (%s):\r\n"
        "  %sssh -J user@%s user@192.168.1.200%s\r\n\r\n"
        "Without a VPN client (jump over the VPN server's sshd):\r\n"
        "  %sssh -J user@%s:%u,user@%s user@192.168.1.200%s\r\n\r\n"
        "Only destination 192.168.1.200:22 is permitted.\r\n",
        recoveryVpnActiveProfileName(), recoveryVpnEndpoint(), kAnsiCyan,
        tunnelIp.c_str(), kAnsiReset, kAnsiCyan, recoveryVpnEndpoint(),
        kVpnServerSshPort, tunnelIp.c_str(), kAnsiReset);
  } else if (topic == "status") {
    channelWrite(channel,
        "\r\nstatus - show the complete recovery dashboard\r\n\r\n"
        "Usage: status\r\n"
        "Checks Wi-Fi, internet state, memory and 192.168.1.200:22.\r\n");
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
  if (mainPcSshReachable()) {
    channelWrite(channel, "\r\nMain PC is already online; SSH port 22 is open.\r\n");
    return;
  }

  uint8_t packet[102];
  memset(packet, 0xff, 6);
  for (size_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * sizeof(kMainPcMac), kMainPcMac,
           sizeof(kMainPcMac));
  }

  WiFiUDP udp;
  udp.begin(9);
  bool sent = true;
  for (int attempt = 0; attempt < 3; ++attempt) {
    sent = udp.beginPacket(kLanBroadcast, 9) == 1 && sent;
    udp.write(packet, sizeof(packet));
    sent = udp.endPacket() == 1 && sent;
    delay(250);
  }
  udp.stop();

  channelPrintf(channel,
                "\r\nWoWLAN packet %s to AA:BB:CC:DD:EE:FF via 192.168.1.255.\r\n",
                sent ? "sent" : "failed");
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
                  "\r\nFirmware: recovery prototype 0.1\r\nAuthorized key: %s\r\n",
                  kRecoverySshKeyFingerprint);
  } else if (command == "pc status") {
    channelPrintf(channel, "\r\nMain PC 192.168.1.200:22 is %s.\r\n",
                  mainPcSshReachable() ? "ONLINE" : "OFFLINE");
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
    return true;
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
        strcmp(ssh_message_auth_user(message), kRecoverySshUser) == 0) {
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
        ssh_channel_write(channel, &input, 1);
      }
    }
  }
}

// Both write sides can accept fewer bytes than offered (a full SSH window or
// TCP send buffer); dropping the remainder would corrupt the relayed stream,
// so every chunk is written out completely or the relay is torn down.
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
  if (!target.connect(kMainPcIp, kMainPcSshPort, 2000)) {
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    return;
  }
  target.setNoDelay(true);

  ssh_channel_set_blocking(channel, 0);
  uint8_t buffer[1024];
  uint32_t lastActivityMs = millis();
  while (target.connected() && ssh_channel_is_open(channel) &&
         !ssh_channel_is_eof(channel)) {
    bool transferred = false;
    const int sshAvailable = ssh_channel_poll(channel, 0);
    if (sshAvailable > 0) {
      const int count = ssh_channel_read_nonblocking(
          channel, buffer, min(sshAvailable, static_cast<int>(sizeof(buffer))), 0);
      if (count > 0) {
        if (!writeAllToTarget(target, buffer, count)) {
          break;
        }
        transferred = true;
      }
    }

    const int targetAvailable = target.available();
    if (targetAvailable > 0) {
      const int count = target.read(buffer, min(targetAvailable, static_cast<int>(sizeof(buffer))));
      if (count > 0) {
        if (!writeAllToChannel(channel, buffer, count)) {
          break;
        }
        transferred = true;
      }
    }

    if (transferred) {
      lastActivityMs = millis();
    } else if (millis() - lastActivityMs > kRelayIdleTimeoutMs) {
      break;
    }
    delay(1);
  }

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
        if (destination != nullptr && strcmp(destination, "192.168.1.200") == 0 &&
            destinationPort == kMainPcSshPort) {
          ssh_channel channel = ssh_message_channel_request_open_reply_accept(message);
          ssh_message_free(message);
          relayDirectTcpip(channel);
          ssh_channel_free(channel);
          return;
        }
      }
    }

    ssh_message_reply_default(message);
    ssh_message_free(message);
  }
}

void sshServerTask(void*) {
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  libssh_begin();
  if (!ensureHostKey() ||
      ssh_pki_import_pubkey_base64(kRecoverySshPublicKeyBase64, SSH_KEYTYPE_RSA,
                                   &authorizedKey) != SSH_OK) {
    Serial.println("SSH: initialization failed");
    vTaskDelete(nullptr);
    return;
  }

  ssh_bind bind = ssh_bind_new();
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, kBindAddress);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT_STR, kBindPort);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY, kHostKeyVfsPath);
  int verbosity = SSH_LOG_WARN;
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &verbosity);

  if (ssh_bind_listen(bind) != SSH_OK) {
    Serial.printf("SSH: listen failed: %s\n", ssh_get_error(bind));
    ssh_bind_free(bind);
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("SSH: listening on %s:%s as %s\n", WiFi.localIP().toString().c_str(),
                kBindPort, kRecoverySshUser);

  while (true) {
    ssh_session session = ssh_new();
    if (session == nullptr) {
      delay(1000);
      continue;
    }
    if (ssh_bind_accept(bind, session) == SSH_OK) {
      hardenSessionTransport(session);
      if (ssh_handle_key_exchange(session) == SSH_OK &&
          authenticateSession(session)) {
        handleAuthenticatedSession(session);
      }
    }
    ssh_disconnect(session);
    ssh_free(session);
    delay(20);
  }
}
}  // namespace

void startRecoverySshServer() {
  xTaskCreatePinnedToCore(sshServerTask, "recovery-ssh", kSshTaskStack, nullptr,
                          2, nullptr, 0);
}
