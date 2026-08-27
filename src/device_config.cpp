#include "device_config.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <string.h>

namespace {
constexpr char kNamespace[] = "recovery";
constexpr char kConfigKey[] = "cfg";
constexpr char kMacKey[] = "pcmac";
constexpr char kPortalKey[] = "portal";
constexpr uint16_t kConfigVersion = 1;
constexpr char kHostKeyFsPath[] = "/ssh_host_ed25519_key";
}  // namespace

DeviceConfig gDeviceConfig = {};

bool deviceConfigLoad() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {
    return false;
  }
  const size_t stored = prefs.getBytesLength(kConfigKey);
  bool loaded = false;
  if (stored == sizeof(DeviceConfig)) {
    loaded = prefs.getBytes(kConfigKey, &gDeviceConfig, sizeof(DeviceConfig)) ==
                 sizeof(DeviceConfig) &&
             gDeviceConfig.version == kConfigVersion;
  }
  prefs.end();
  if (!loaded) {
    memset(&gDeviceConfig, 0, sizeof(DeviceConfig));
  }
  return loaded;
}

bool deviceConfigSave() {
  gDeviceConfig.version = kConfigVersion;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    return false;
  }
  const bool saved =
      prefs.putBytes(kConfigKey, &gDeviceConfig, sizeof(DeviceConfig)) ==
      sizeof(DeviceConfig);
  prefs.end();
  return saved;
}

bool deviceConfigPresent() {
  return gDeviceConfig.version == kConfigVersion &&
         gDeviceConfig.wifiSsid[0] != '\0';
}

void deviceConfigFactoryReset() {
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  // Earlier firmware revisions stored credentials in the WiFi library's own
  // NVS namespace; wipe those too so the radio never rejoins on its own.
  WiFi.disconnect(true, true);
  if (SPIFFS.begin(true)) {
    SPIFFS.remove(kHostKeyFsPath);
    SPIFFS.end();
  }
  // The persistent state above is what matters and is now cleared; the
  // caller restarts within ~2 s of this call, so gDeviceConfig itself is
  // deliberately left as-is rather than zeroed here. The VPN and SSH tasks
  // keep running against it until reboot, and zeroing it out from under
  // them (they hold raw pointers into gDeviceConfig.wg[]) would risk a
  // WireGuard handshake or SSH auth momentarily operating on blanked-out
  // key material for no benefit, since nothing reads gDeviceConfig again
  // after this function returns.
}

bool deviceConfigMacLoad(uint8_t mac[6]) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {
    return false;
  }
  const bool present = prefs.getBytes(kMacKey, mac, 6) == 6;
  prefs.end();
  return present;
}

void deviceConfigMacStore(const uint8_t mac[6]) {
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putBytes(kMacKey, mac, 6);
    prefs.end();
  }
}

void portalRequestFlagSet() {
  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putUChar(kPortalKey, 1);
    prefs.end();
  }
}

bool portalRequestFlagTake() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    return false;
  }
  const bool requested = prefs.getUChar(kPortalKey, 0) != 0;
  if (requested) {
    prefs.remove(kPortalKey);
  }
  prefs.end();
  return requested;
}
