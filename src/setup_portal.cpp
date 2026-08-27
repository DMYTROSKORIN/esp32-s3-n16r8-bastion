#include "setup_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <libssh/libssh.h>
#include <mbedtls/base64.h>
#include <string.h>

#include "device_config.h"
#include "portal_html.h"
#include "wg_conf.h"

namespace {
constexpr char kApName[] = "ESP32_SetUp";
constexpr uint16_t kDnsPort = 53;
constexpr uint32_t kRebootDelayMs = 1500;

const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApMask(255, 255, 255, 0);

const char* const kSshKeyTypes[] = {
    "ssh-ed25519",           "ssh-rsa",
    "ecdsa-sha2-nistp256",   "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
};

DNSServer dnsServer;
WebServer httpServer(80);
uint32_t rebootAtMs = 0;
bool scanRequested = false;

// Accumulates {"errors":{"field":"message",...}}. Field names and messages are
// fixed ASCII strings, so no JSON escaping is needed here.
struct ErrorList {
  char json[640];
  size_t length = 0;
  bool any = false;

  void add(const char* field, const char* message) {
    const int written = snprintf(
        json + length, sizeof(json) - length, "%s\"%s\":\"%s\"",
        any ? "," : "{\"errors\":{", field, message);
    if (written > 0 && length + written < sizeof(json)) {
      length += written;
    }
    any = true;
  }

  const char* finish() {
    snprintf(json + length, sizeof(json) - length, "}}");
    return json;
  }
};

size_t jsonEscape(const char* text, char* out, size_t outSize) {
  size_t used = 0;
  for (const char* c = text; *c != '\0' && used + 7 < outSize; ++c) {
    const unsigned char ch = static_cast<unsigned char>(*c);
    if (ch == '"' || ch == '\\') {
      out[used++] = '\\';
      out[used++] = ch;
    } else if (ch < 0x20) {
      used += snprintf(out + used, outSize - used, "\\u%04x", ch);
    } else {
      out[used++] = ch;
    }
  }
  out[used] = '\0';
  return used;
}

void redirectToPortal() {
  httpServer.sendHeader("Location", "http://192.168.4.1/", true);
  httpServer.send(302, "text/plain", "");
}

void handleRoot() {
  httpServer.sendHeader("Content-Encoding", "gzip");
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send_P(200, "text/html",
                    reinterpret_cast<const char*>(kPortalHtmlGz),
                    kPortalHtmlGzLength);
}

void handleNotFound() {
  if (httpServer.uri() == "/favicon.ico") {
    httpServer.send(404, "text/plain", "");
    return;
  }
  redirectToPortal();
}

void handleConfig() {
  static char json[640];
  char ssid[100];
  char wg1[140];
  char wg2[140];
  jsonEscape(gDeviceConfig.wifiSsid, ssid, sizeof(ssid));
  jsonEscape(gDeviceConfig.wg[0].endpoint, wg1, sizeof(wg1));
  jsonEscape(gDeviceConfig.wg[1].endpoint, wg2, sizeof(wg2));

  snprintf(json, sizeof(json),
           "{\"provisioned\":%s,\"ssid\":\"%s\",\"passSet\":%s,"
           "\"pcIp\":\"%s\",\"pcPort\":%u,\"sshUser\":\"%s\","
           "\"sshKeySet\":%s,\"sshKeyType\":\"%s\","
           "\"wg1Set\":%s,\"wg1Endpoint\":\"%s\",\"wg1SshPort\":%u,"
           "\"wg2Set\":%s,\"wg2Endpoint\":\"%s\",\"wg2SshPort\":%u}",
           deviceConfigPresent() ? "true" : "false", ssid,
           gDeviceConfig.wifiPassword[0] != '\0' ? "true" : "false",
           gDeviceConfig.pcIp, gDeviceConfig.pcPort, gDeviceConfig.sshUser,
           gDeviceConfig.sshKeyBase64[0] != '\0' ? "true" : "false",
           gDeviceConfig.sshKeyType,
           gDeviceConfig.wgProfileCount >= 1 ? "true" : "false", wg1,
           gDeviceConfig.wg[0].vpnServerSshPort,
           gDeviceConfig.wgProfileCount >= 2 ? "true" : "false", wg2,
           gDeviceConfig.wg[1].vpnServerSshPort);
  httpServer.send(200, "application/json", json);
}

void handleScan() {
  if (httpServer.hasArg("refresh") || !scanRequested) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    scanRequested = true;
    httpServer.send(200, "application/json", "{\"status\":\"pending\"}");
    return;
  }

  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    httpServer.send(200, "application/json", "{\"status\":\"pending\"}");
    return;
  }
  if (found < 0) {
    scanRequested = false;
    httpServer.send(200, "application/json", "{\"networks\":[]}");
    return;
  }

  // Emit networks strongest-first without re-sorting the driver's list.
  static char json[4096];
  size_t used = snprintf(json, sizeof(json), "{\"networks\":[");
  bool first = true;
  bool emitted[64] = {};
  const int16_t count = min<int16_t>(found, 64);
  for (int16_t rank = 0; rank < count; ++rank) {
    int16_t best = -1;
    for (int16_t index = 0; index < count; ++index) {
      if (!emitted[index] &&
          (best < 0 || WiFi.RSSI(index) > WiFi.RSSI(best))) {
        best = index;
      }
    }
    emitted[best] = true;
    if (WiFi.SSID(best).isEmpty()) {
      continue;
    }
    char ssid[100];
    jsonEscape(WiFi.SSID(best).c_str(), ssid, sizeof(ssid));
    const int written = snprintf(
        json + used, sizeof(json) - used,
        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", first ? "" : ",", ssid,
        WiFi.RSSI(best),
        WiFi.encryptionType(best) == WIFI_AUTH_OPEN ? "true" : "false");
    if (written <= 0 || used + written >= sizeof(json) - 3) {
      break;
    }
    used += written;
    first = false;
  }
  snprintf(json + used, sizeof(json) - used, "]}");
  httpServer.send(200, "application/json", json);
}

bool copyArg(const char* name, char* out, size_t outSize) {
  const String& value = httpServer.arg(name);
  if (value.length() >= outSize) {
    return false;
  }
  strcpy(out, value.c_str());
  return true;
}

bool validIpv4String(const char* text, char* canonical, size_t canonicalSize) {
  IPAddress ip;
  if (!ip.fromString(text)) {
    return false;
  }
  // Reject addresses that can never be a real host on the LAN: 0.0.0.0/8,
  // multicast (224-239) and the reserved/broadcast range (240-255).
  if (ip[0] == 0 || ip[0] >= 224) {
    return false;
  }
  snprintf(canonical, canonicalSize, "%s", ip.toString().c_str());
  return true;
}

bool validSshUser(const char* user) {
  const size_t length = strlen(user);
  if (length == 0 || length >= sizeof(DeviceConfig::sshUser)) {
    return false;
  }
  for (const char* c = user; *c != '\0'; ++c) {
    if (!isalnum(static_cast<unsigned char>(*c)) && *c != '.' && *c != '_' &&
        *c != '-') {
      return false;
    }
  }
  return true;
}

// An OpenSSH public key blob is a sequence of SSH wire-format fields, each a
// 4-byte big-endian length prefix followed by that many bytes. Verifying the
// whole blob decodes as such - consuming every byte with no overrun and no
// leftover - catches truncated/corrupted keys that happen to start with a
// valid algorithm-name field, which the length-prefix-only check below does
// not: such a key would otherwise save successfully, then fail
// ssh_pki_import_pubkey_base64() after reboot with no way back in except
// physically re-entering the setup portal.
bool isWellFormedSshWireFields(const unsigned char* data, size_t length) {
  size_t offset = 0;
  bool sawField = false;
  while (offset < length) {
    if (length - offset < 4) {
      return false;
    }
    const uint32_t fieldLength = (static_cast<uint32_t>(data[offset]) << 24) |
                                 (data[offset + 1] << 16) |
                                 (data[offset + 2] << 8) | data[offset + 3];
    offset += 4;
    if (fieldLength > length - offset) {
      return false;
    }
    offset += fieldLength;
    sawField = true;
  }
  return sawField;
}

// Accepts "type base64 [comment]" and verifies the base64 blob's embedded
// algorithm name matches the declared type.
bool parseSshPublicKey(const char* text, char* typeOut, size_t typeSize,
                       char* keyOut, size_t keySize) {
  while (*text == ' ' || *text == '\t') {
    ++text;
  }
  const char* typeEnd = strpbrk(text, " \t");
  if (typeEnd == nullptr) {
    return false;
  }
  const size_t typeLength = static_cast<size_t>(typeEnd - text);
  if (typeLength == 0 || typeLength >= typeSize) {
    return false;
  }
  memcpy(typeOut, text, typeLength);
  typeOut[typeLength] = '\0';

  bool known = false;
  for (const char* const candidate : kSshKeyTypes) {
    known = known || strcmp(candidate, typeOut) == 0;
  }
  if (!known) {
    return false;
  }

  const char* base64 = typeEnd;
  while (*base64 == ' ' || *base64 == '\t') {
    ++base64;
  }
  const char* base64End = strpbrk(base64, " \t\r\n");
  const size_t base64Length =
      base64End != nullptr ? static_cast<size_t>(base64End - base64)
                           : strlen(base64);
  if (base64Length == 0 || base64Length >= keySize) {
    return false;
  }

  static unsigned char decoded[640];
  size_t decodedLength = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded), &decodedLength,
                            reinterpret_cast<const unsigned char*>(base64),
                            base64Length) != 0 ||
      decodedLength < 4 + typeLength) {
    return false;
  }
  const uint32_t embeddedLength = (static_cast<uint32_t>(decoded[0]) << 24) |
                                  (decoded[1] << 16) | (decoded[2] << 8) |
                                  decoded[3];
  if (embeddedLength != typeLength ||
      memcmp(decoded + 4, typeOut, typeLength) != 0 ||
      !isWellFormedSshWireFields(decoded, decodedLength)) {
    return false;
  }

  memcpy(keyOut, base64, base64Length);
  keyOut[base64Length] = '\0';

  // The checks above catch truncation and gross corruption but don't know
  // each algorithm's field semantics (e.g. an ed25519 key's second field
  // must be exactly 32 bytes, RSA's e/n must be well-formed mpints) - a key
  // could still pass them yet fail to import. Settle it for real with the
  // exact function the SSH server calls after every reboot, so a bad key is
  // rejected here instead of silently disabling the recovery console until
  // someone re-enters this portal in person.
  ssh_key trial = nullptr;
  const bool importable =
      ssh_pki_import_pubkey_base64(keyOut, ssh_key_type_from_name(typeOut),
                                   &trial) == SSH_OK;
  if (trial != nullptr) {
    ssh_key_free(trial);
  }
  return importable;
}

// Attempts a real STA connection with the given credentials while the setup
// AP keeps running, so a wrong Wi-Fi password is caught before it is ever
// saved. Returns nullptr on success, or the field to blame with a reason.
const char* testWifiCredentials(const char* ssid, const char* password,
                                char* reasonOut, size_t reasonSize) {
  constexpr uint32_t kTimeoutMs = 15000;
  constexpr uint32_t kPollIntervalMs = 200;

  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssid, password[0] != '\0' ? password : nullptr);

  const uint32_t startMs = millis();
  wl_status_t status;
  do {
    delay(kPollIntervalMs);
    status = WiFi.status();
  } while (status != WL_CONNECTED && status != WL_CONNECT_FAILED &&
           status != WL_NO_SSID_AVAIL && millis() - startMs < kTimeoutMs);
  WiFi.disconnect(false);

  if (status == WL_CONNECTED) {
    return nullptr;
  }
  if (status == WL_NO_SSID_AVAIL) {
    snprintf(reasonOut, reasonSize,
             "network not found (out of range, or not 2.4 GHz)");
    return "ssid";
  }
  if (status == WL_CONNECT_FAILED) {
    snprintf(reasonOut, reasonSize, "wrong password or authentication failed");
    return "pass";
  }
  snprintf(reasonOut, reasonSize, "could not connect (timed out)");
  return "ssid";
}

void handleApply() {
  static DeviceConfig draft;
  draft = gDeviceConfig;
  ErrorList errors;

  if (httpServer.hasArg("ssid") &&
      !copyArg("ssid", draft.wifiSsid, sizeof(draft.wifiSsid))) {
    errors.add("ssid", "network name is too long (32 max)");
  }
  if (httpServer.hasArg("pass") &&
      !copyArg("pass", draft.wifiPassword, sizeof(draft.wifiPassword))) {
    errors.add("pass", "password is too long (64 max)");
  }

  if (httpServer.hasArg("pc_ip")) {
    char buffer[40];
    if (!copyArg("pc_ip", buffer, sizeof(buffer)) ||
        !validIpv4String(buffer, draft.pcIp, sizeof(draft.pcIp))) {
      errors.add("pc_ip", "enter a valid IPv4 address");
    }
  }
  if (httpServer.hasArg("pc_port")) {
    const long port = httpServer.arg("pc_port").toInt();
    if (port < 1 || port > 65535) {
      errors.add("pc_port", "port must be 1-65535");
    } else {
      draft.pcPort = static_cast<uint16_t>(port);
    }
  }

  if (httpServer.hasArg("ssh_user")) {
    char buffer[48];
    if (!copyArg("ssh_user", buffer, sizeof(buffer)) || !validSshUser(buffer)) {
      errors.add("ssh_user", "letters, digits, . _ - only (31 max)");
    } else {
      strcpy(draft.sshUser, buffer);
    }
  }
  if (httpServer.hasArg("ssh_key")) {
    const String& key = httpServer.arg("ssh_key");
    if (key.isEmpty()) {
      errors.add("ssh_key", "a public key is required");
    } else if (!parseSshPublicKey(key.c_str(), draft.sshKeyType,
                                  sizeof(draft.sshKeyType), draft.sshKeyBase64,
                                  sizeof(draft.sshKeyBase64))) {
      errors.add("ssh_key",
                 "not a valid OpenSSH public key (ed25519, rsa or ecdsa)");
    }
  }

  const char* const wgFields[kMaxWgProfiles] = {"wg1", "wg2"};
  const char* const wgPortFields[kMaxWgProfiles] = {"wg1_ssh_port",
                                                    "wg2_ssh_port"};
  for (uint8_t slot = 0; slot < kMaxWgProfiles; ++slot) {
    // Re-uploading the .conf resets the whole profile struct (including
    // vpnServerSshPort, which isn't part of the .conf format); remember the
    // previously saved port so it survives a conf-only edit that doesn't
    // also resend the port field.
    const uint16_t previousSshPort = draft.wg[slot].vpnServerSshPort;
    if (httpServer.hasArg(wgFields[slot])) {
      const String& conf = httpServer.arg(wgFields[slot]);
      if (conf.isEmpty()) {
        memset(&draft.wg[slot], 0, sizeof(WgProfileConfig));
      } else {
        char reason[96];
        if (!parseWireGuardConf(conf.c_str(), draft.wg[slot], reason,
                                sizeof(reason))) {
          errors.add(wgFields[slot], reason);
        } else if (!httpServer.hasArg(wgPortFields[slot])) {
          draft.wg[slot].vpnServerSshPort = previousSshPort;
        }
      }
    }

    if (httpServer.hasArg(wgPortFields[slot])) {
      const long port = httpServer.arg(wgPortFields[slot]).toInt();
      if (port < 1 || port > 65535) {
        errors.add(wgPortFields[slot], "port must be 1-65535");
      } else {
        draft.wg[slot].vpnServerSshPort = static_cast<uint16_t>(port);
      }
    }
  }

  // Cross-field checks on the merged result.
  if (draft.wifiSsid[0] == '\0') {
    errors.add("ssid", "select a Wi-Fi network");
  }
  if (draft.pcIp[0] == '\0') {
    errors.add("pc_ip", "enter the PC's IP address");
  }
  if (draft.pcPort == 0) {
    draft.pcPort = 22;
  }
  if (draft.sshUser[0] == '\0') {
    errors.add("ssh_user", "enter a username");
  }
  if (draft.sshKeyBase64[0] == '\0') {
    errors.add("ssh_key", "a public key is required");
  }
  if (draft.wg[1].endpoint[0] != '\0' && draft.wg[0].endpoint[0] == '\0') {
    errors.add("wg1", "add a primary profile before a secondary one");
  }
  for (uint8_t slot = 0; slot < kMaxWgProfiles; ++slot) {
    if (draft.wg[slot].endpoint[0] != '\0' &&
        draft.wg[slot].vpnServerSshPort == 0) {
      draft.wg[slot].vpnServerSshPort = 22;
    }
  }

  // A real connection attempt catches a wrong password before it is saved;
  // skip it if cheaper checks above already failed, or if Wi-Fi was not
  // touched (nothing new to verify).
  const bool wifiTouched = httpServer.hasArg("ssid") || httpServer.hasArg("pass");
  if (!errors.any && wifiTouched) {
    char reason[80];
    const char* field =
        testWifiCredentials(draft.wifiSsid, draft.wifiPassword, reason,
                            sizeof(reason));
    if (field != nullptr) {
      errors.add(field, reason);
    }
  }

  if (errors.any) {
    httpServer.send(400, "application/json", errors.finish());
    return;
  }

  draft.wgProfileCount =
      draft.wg[0].endpoint[0] != '\0' ? (draft.wg[1].endpoint[0] != '\0' ? 2 : 1)
                                      : 0;
  gDeviceConfig = draft;
  if (!deviceConfigSave()) {
    httpServer.send(500, "application/json",
                    "{\"errors\":{\"_\":\"saving to flash failed\"}}");
    return;
  }
  Serial.println("Portal: settings saved; restarting.");
  httpServer.send(200, "application/json", "{\"ok\":true}");
  rebootAtMs = millis() + kRebootDelayMs;
}
}  // namespace

void setupPortalStart() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);  // STA side stays idle but enables network scans.

  // This access point is the device's only provisioning path - if it never
  // comes up, an unprovisioned unit is permanently unreachable without a
  // physical reflash. A transient radio/allocation failure is worth a few
  // retries before giving up and rebooting to try the whole sequence again,
  // rather than silently proceeding to start an HTTP server nobody can
  // reach and logging a misleadingly confident "portal open" message.
  bool apReady = false;
  for (uint8_t attempt = 0; attempt < 3 && !apReady; ++attempt) {
    apReady = WiFi.softAPConfig(kApIp, kApIp, kApMask) && WiFi.softAP(kApName);
    if (!apReady) {
      Serial.println("Portal: failed to start access point, retrying");
      WiFi.softAPdisconnect(true);
      delay(500);
    }
  }
  if (!apReady) {
    Serial.println("Portal: could not start access point after retries; rebooting");
    delay(500);
    ESP.restart();
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  if (!dnsServer.start(kDnsPort, "*", kApIp)) {
    // Non-fatal: captive-portal auto-popup won't fire, but the HTTP server
    // below is still reachable by navigating to the AP's address directly.
    Serial.println("Portal: captive DNS failed to start; open http://192.168.4.1/ manually");
  }

  httpServer.on("/", handleRoot);
  httpServer.on("/api/config", HTTP_GET, handleConfig);
  httpServer.on("/api/scan", HTTP_GET, handleScan);
  httpServer.on("/api/apply", HTTP_POST, handleApply);
  // Captive-portal probes: a redirect makes phones pop the setup page open.
  for (const char* probe :
       {"/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
        "/success.txt", "/canonical.html", "/redirect"}) {
    httpServer.on(probe, redirectToPortal);
  }
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();

  Serial.printf("Portal: open network %s, http://%s/\n", kApName,
                kApIp.toString().c_str());
}

void setupPortalLoop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
  if (rebootAtMs != 0 && static_cast<int32_t>(millis() - rebootAtMs) >= 0) {
    ESP.restart();
  }
}
