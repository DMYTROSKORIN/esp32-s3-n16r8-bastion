#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t kMaxWgProfiles = 2;
constexpr uint8_t kMaxWgRoutes = 4;

struct WgRouteConfig {
  char address[16];
  char netmask[16];
};

struct WgProfileConfig {
  char privateKey[46];
  char publicKey[46];
  char presharedKey[46];  // Empty when the peer has no preshared key.
  char address[16];
  char netmask[16];
  char endpoint[64];
  uint16_t port;
  uint16_t persistentKeepalive;
  uint16_t mtu;
  WgRouteConfig routes[kMaxWgRoutes];
  uint8_t routeCount;
  // sshd port of the VPN server itself (not part of the wg-quick .conf
  // format - set separately in the portal). Only used to render the
  // jump-host `pc ssh` command for reaching this profile's server without a
  // local VPN client.
  uint16_t vpnServerSshPort;
};

// Everything the portal provisions lives in this one NVS blob. sshKeyBase64 is
// sized for RSA-4096; Ed25519 and ECDSA keys are far smaller.
struct DeviceConfig {
  uint16_t version;
  char wifiSsid[33];
  char wifiPassword[65];
  char pcIp[16];
  uint16_t pcPort;
  char sshUser[32];
  char sshKeyType[24];
  char sshKeyBase64[800];
  uint8_t wgProfileCount;
  WgProfileConfig wg[kMaxWgProfiles];
};

extern DeviceConfig gDeviceConfig;

// Loads the blob into gDeviceConfig; false leaves it zeroed (unprovisioned).
bool deviceConfigLoad();
bool deviceConfigSave();
bool deviceConfigPresent();

// Wipes all provisioned settings, the learned PC MAC, legacy WiFi-NVS
// credentials, and the SSH host key in SPIFFS.
void deviceConfigFactoryReset();

bool deviceConfigMacLoad(uint8_t mac[6]);
void deviceConfigMacStore(const uint8_t mac[6]);

// One-shot flag: BOOT held for 5 s sets it, the next boot consumes it and
// starts the setup portal pre-filled with the current settings.
void portalRequestFlagSet();
bool portalRequestFlagTake();
