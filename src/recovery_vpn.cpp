#include "recovery_vpn.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <atomic>
#include <esp_wireguard.h>
#include <time.h>

#include "device_config.h"

namespace {
constexpr uint32_t kTaskStack = 16384;
constexpr uint32_t kHealthIntervalMs = 10000;
constexpr uint32_t kConnectRetryMs = 1000;
// esp_wireguard_connect() returns ESP_ERR_RETRY while its DNS lookup is
// still in flight (dns_gethostbyname() == ERR_INPROGRESS). Without a
// deadline, an endpoint whose hostname never resolves (DNS outage, typo,
// expired domain) would retry here forever - no failover, no response to
// `vpn failover`/`vpn retry-primary`, stuck in CONNECTING indefinitely.
constexpr uint32_t kConnectDeadlineMs = 20000;
constexpr uint32_t kInitialGraceMs = 30000;
constexpr uint32_t kMaxHandshakeAgeSeconds = 180;
constexpr uint8_t kFailuresBeforeFailover = 3;
constexpr uint16_t kProbePort = 53;
constexpr uint32_t kProbeTimeoutMs = 1200;

const IPAddress kProbeHosts[] = {
    IPAddress(1, 1, 1, 1),
    IPAddress(8, 8, 8, 8),
};

enum class VpnState : uint8_t {
  kNotConfigured,
  kWaitingForNetwork,
  kConnecting,
  kOnline,
  kDegraded,
};

// A newly issued request always replaces whatever was pending, so the most
// recent console command wins instead of two flags racing to be handled in
// arrival order.
enum class VpnRequest : uint8_t { kNone, kFailover, kPrimary };

wireguard_config_t wireGuardConfig = ESP_WIREGUARD_CONFIG_DEFAULT();
wireguard_ctx_t wireGuardContext = ESP_WIREGUARD_CONTEXT_DEFAULT();
// The VPN task (core 1) writes these; the SSH task (core 0) and the main
// loop read them for the dashboard/status commands. `volatile` only stops
// the compiler from caching a value in a register - it is not a C++
// synchronization primitive and does not make cross-core reads/writes of a
// plain variable well-defined. std::atomic is the correct tool here and
// costs nothing extra on scalars this small.
std::atomic<VpnState> vpnState{VpnState::kNotConfigured};
std::atomic<int8_t> activeProfile{-1};
std::atomic<uint8_t> consecutiveFailures{0};
std::atomic<uint32_t> handshakeAgeSeconds{UINT32_MAX};
std::atomic<VpnRequest> pendingRequest{VpnRequest::kNone};
bool tunnelInitialized = false;
uint32_t profileStartedMs = 0;

void noteFailure() {
  if (consecutiveFailures < UINT8_MAX) {
    ++consecutiveFailures;
  }
}

const char* stateName(VpnState state) {
  switch (state) {
    case VpnState::kNotConfigured:
      return "NOT_CONFIGURED";
    case VpnState::kWaitingForNetwork:
      return "WAITING";
    case VpnState::kConnecting:
      return "CONNECTING";
    case VpnState::kOnline:
      return "ONLINE";
    case VpnState::kDegraded:
      return "DEGRADED";
  }
  return "UNKNOWN";
}

uint8_t profileCount() { return gDeviceConfig.wgProfileCount; }

const WgProfileConfig& profileAt(uint8_t index) { return gDeviceConfig.wg[index]; }

void stopTunnel() {
  if (tunnelInitialized && wireGuardContext.netif != nullptr) {
    esp_wireguard_disconnect(&wireGuardContext);
  }
  tunnelInitialized = false;
  wireGuardContext = ESP_WIREGUARD_CONTEXT_DEFAULT();
}

bool addAllowedRoutes(const WgProfileConfig& profile) {
  for (uint8_t index = 0; index < profile.routeCount; ++index) {
    const esp_err_t result = esp_wireguard_add_allowed_ip(
        &wireGuardContext, profile.routes[index].address,
        profile.routes[index].netmask);
    if (result != ESP_OK) {
      Serial.printf("WireGuard: adding IPv4 route %s/%s failed: %d\n",
                    profile.routes[index].address, profile.routes[index].netmask,
                    result);
      return false;
    }
  }
  return true;
}

bool startProfile(uint8_t index) {
  stopTunnel();
  const WgProfileConfig& profile = profileAt(index);
  const bool profileChanged = activeProfile != static_cast<int8_t>(index);

  wireGuardConfig = ESP_WIREGUARD_CONFIG_DEFAULT();
  wireGuardConfig.private_key = profile.privateKey;
  wireGuardConfig.public_key = profile.publicKey;
  wireGuardConfig.preshared_key =
      profile.presharedKey[0] != '\0' ? profile.presharedKey : nullptr;
  wireGuardConfig.address = profile.address;
  wireGuardConfig.netmask = profile.netmask;
  wireGuardConfig.endpoint = profile.endpoint;
  wireGuardConfig.port = profile.port;
  wireGuardConfig.persistent_keepalive = profile.persistentKeepalive;

  wireGuardContext = ESP_WIREGUARD_CONTEXT_DEFAULT();
  vpnState = VpnState::kConnecting;
  activeProfile = index;
  if (profileChanged) {
    consecutiveFailures = 0;
  }
  handshakeAgeSeconds = UINT32_MAX;
  Serial.printf("WireGuard: starting profile %u at %s:%u\n", index + 1,
                profile.endpoint, profile.port);

  if (esp_wireguard_init(&wireGuardConfig, &wireGuardContext) != ESP_OK) {
    Serial.printf("WireGuard: profile %u initialization failed\n", index + 1);
    return false;
  }
  tunnelInitialized = true;

  esp_err_t result;
  const uint32_t connectStartMs = millis();
  do {
    result = esp_wireguard_connect(&wireGuardContext);
    if (result == ESP_ERR_RETRY) {
      delay(kConnectRetryMs);
    }
  } while (result == ESP_ERR_RETRY && WiFi.status() == WL_CONNECTED &&
           millis() - connectStartMs < kConnectDeadlineMs);

  if (result == ESP_ERR_RETRY) {
    Serial.printf("WireGuard: profile %u connect timed out (DNS never resolved?)\n",
                  index + 1);
    stopTunnel();
    return false;
  }
  if (result != ESP_OK) {
    Serial.printf("WireGuard: profile %u connect failed: %d\n", index + 1,
                  result);
    stopTunnel();
    return false;
  }
  // The library hardcodes WIREGUARDIF_MTU (1420); honor the profile's MTU so
  // TCP MSS matches paths that need a smaller tunnel packet size.
  if (wireGuardContext.netif != nullptr && profile.mtu > 0) {
    wireGuardContext.netif->mtu = profile.mtu;
  }
  if (!addAllowedRoutes(profile)) {
    stopTunnel();
    return false;
  }
  const esp_err_t defaultRouteResult =
      esp_wireguard_set_default(&wireGuardContext);
  if (defaultRouteResult != ESP_OK) {
    Serial.printf("WireGuard: profile %u default route failed: %d\n", index + 1,
                  defaultRouteResult);
    stopTunnel();
    return false;
  }

  profileStartedMs = millis();
  return true;
}

bool controlAddressReachable() {
  for (const IPAddress& host : kProbeHosts) {
    WiFiClient client;
    if (client.connect(host, kProbePort, kProbeTimeoutMs)) {
      client.stop();
      return true;
    }
    client.stop();
  }
  return false;
}

bool tunnelHealthy() {
  if (!tunnelInitialized || wireGuardContext.netif == nullptr ||
      esp_wireguard_peer_is_up(&wireGuardContext) != ESP_OK) {
    handshakeAgeSeconds = UINT32_MAX;
    return false;
  }

  time_t latestHandshake = 0;
  const time_t now = time(nullptr);
  if (esp_wireguard_latest_handshake(&wireGuardContext, &latestHandshake) != ESP_OK ||
      now < latestHandshake) {
    handshakeAgeSeconds = UINT32_MAX;
    return false;
  }
  handshakeAgeSeconds = static_cast<uint32_t>(now - latestHandshake);
  return handshakeAgeSeconds <= kMaxHandshakeAgeSeconds && controlAddressReachable();
}

void waitForClock() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  const uint32_t startedMs = millis();
  while (WiFi.status() == WL_CONNECTED && time(nullptr) < 1700000000 &&
         millis() - startedMs < 30000) {
    delay(250);
  }
}

void vpnTask(void*) {
  uint8_t desiredProfile = 0;
  uint32_t lastHealthCheckMs = 0;

  while (true) {
    if (profileCount() == 0) {
      vpnState = VpnState::kNotConfigured;
      delay(1000);
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (tunnelInitialized) {
        Serial.println("WireGuard: Wi-Fi lost; stopping tunnel");
        stopTunnel();
      }
      vpnState = VpnState::kWaitingForNetwork;
      delay(500);
      continue;
    }

    if (time(nullptr) < 1700000000) {
      waitForClock();
    }

    if (desiredProfile >= profileCount()) {
      desiredProfile = 0;
    }

    // Atomic read-and-clear: a request stored by the console right after
    // this line reads is never dropped, it simply becomes next cycle's
    // pending request instead of being overwritten by the kNone reset below.
    const VpnRequest request = pendingRequest.exchange(VpnRequest::kNone);
    if (request != VpnRequest::kNone) {
      if (request == VpnRequest::kPrimary) {
        desiredProfile = 0;
        stopTunnel();
      } else if (profileCount() > 1) {
        desiredProfile = activeProfile == 0 ? 1 : 0;
        stopTunnel();
      }
    }

    if (!tunnelInitialized) {
      if (!startProfile(desiredProfile)) {
        noteFailure();
        if (profileCount() > 1 && consecutiveFailures >= kFailuresBeforeFailover) {
          desiredProfile = desiredProfile == 0 ? 1 : 0;
          consecutiveFailures = 0;
        }
        delay(kHealthIntervalMs);
        continue;
      }
      lastHealthCheckMs = 0;
    }

    const uint32_t nowMs = millis();
    if (lastHealthCheckMs != 0 && nowMs - lastHealthCheckMs < kHealthIntervalMs) {
      delay(100);
      continue;
    }
    lastHealthCheckMs = nowMs;

    if (tunnelHealthy()) {
      if (vpnState != VpnState::kOnline) {
        Serial.printf("WireGuard: profile %u is online\n", activeProfile + 1);
      }
      vpnState = VpnState::kOnline;
      consecutiveFailures = 0;
      delay(100);
      continue;
    }

    vpnState = VpnState::kDegraded;
    if (nowMs - profileStartedMs < kInitialGraceMs) {
      delay(100);
      continue;
    }

    noteFailure();
    Serial.printf("WireGuard: profile %u health check failed (%u/%u)\n",
                  activeProfile + 1, consecutiveFailures.load(),
                  kFailuresBeforeFailover);
    if (profileCount() > 1 && consecutiveFailures >= kFailuresBeforeFailover) {
      desiredProfile = activeProfile == 0 ? 1 : 0;
      Serial.printf("WireGuard: failing over to profile %u\n", desiredProfile + 1);
      stopTunnel();
    }
    delay(100);
  }
}
}  // namespace

void startRecoveryVpn() {
  if (xTaskCreatePinnedToCore(vpnTask, "recovery-vpn", kTaskStack, nullptr, 3,
                              nullptr, 1) != pdPASS) {
    Serial.println("WireGuard: failed to create VPN task");
  }
}

void recoveryVpnRequestFailover() { pendingRequest.store(VpnRequest::kFailover); }

void recoveryVpnRequestPrimary() { pendingRequest.store(VpnRequest::kPrimary); }

bool recoveryVpnConfigured() { return profileCount() > 0; }

bool recoveryVpnOnline() { return vpnState == VpnState::kOnline; }

const char* recoveryVpnStateName() { return stateName(vpnState); }

const char* recoveryVpnActiveProfileName() {
  static char name[16];
  const int8_t index = activeProfile;
  if (index < 0 || index >= profileCount()) {
    return "none";
  }
  snprintf(name, sizeof(name), "profile-%d", index + 1);
  return name;
}

uint8_t recoveryVpnActiveProfileNumber() {
  const int8_t index = activeProfile;
  return (index >= 0 && index < profileCount())
             ? static_cast<uint8_t>(index + 1)
             : 0;
}

const char* recoveryVpnEndpoint() {
  const int8_t index = activeProfile;
  return (index >= 0 && index < profileCount()) ? profileAt(index).endpoint
                                                 : "unknown";
}

uint16_t recoveryVpnServerSshPort() {
  const int8_t index = activeProfile;
  return (index >= 0 && index < profileCount())
             ? profileAt(index).vpnServerSshPort
             : 0;
}

IPAddress recoveryVpnAddress() {
  const int8_t index = activeProfile;
  IPAddress address;
  if (index >= 0 && index < profileCount()) {
    address.fromString(profileAt(index).address);
  }
  return address;
}

uint32_t recoveryVpnHandshakeAgeSeconds() { return handshakeAgeSeconds; }

uint8_t recoveryVpnConsecutiveFailures() { return consecutiveFailures; }
