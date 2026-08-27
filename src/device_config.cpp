#include "device_config.h"

#include <Arduino.h>
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
  // This is the device's only "wipe my credentials" path, so a silent
  // failure here is worse than usual: the caller reboots right after this
  // returns believing the reset succeeded, while Wi-Fi/SSH credentials or
  // the host key could still be sitting in flash. One retry plus a clear
  // serial warning on persistent failure at least makes that visible.
  bool cleared = false;
  for (uint8_t attempt = 0; attempt < 2 && !cleared; ++attempt) {
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      cleared = prefs.clear();
      prefs.end();
    }
  }
  if (!cleared) {
    Serial.println("Factory reset: failed to clear stored preferences");
  }

  // Earlier firmware revisions stored credentials in the WiFi library's own
  // NVS namespace; wipe those too so the radio never rejoins on its own.
  WiFi.disconnect(true, true);

  bool keyRemoved = false;
  for (uint8_t attempt = 0; attempt < 2 && !keyRemoved; ++attempt) {
    if (SPIFFS.begin(true)) {
      keyRemoved = !SPIFFS.exists(kHostKeyFsPath) || SPIFFS.remove(kHostKeyFsPath);
      SPIFFS.end();
    }
  }
  if (!keyRemoved) {
    Serial.println("Factory reset: failed to remove SSH host key");
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
  if (!prefs.begin(kNamespace, false)) {
    Serial.println("Main PC: failed to open storage for learned MAC");
    return;
  }
  if (prefs.putBytes(kMacKey, mac, 6) != 6) {
    Serial.println("Main PC: failed to persist learned MAC");
  }
  prefs.end();
}

void portalRequestFlagSet() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    Serial.println("Portal: failed to open storage for portal-reopen flag");
    return;
  }
  // A failure here means the BOOT-5s "reopen portal" request silently does
  // nothing on the next boot instead of opening the portal - the device just
  // reboots into its normal connected state, which looks like the button
  // press was ignored rather than like a storage error.
  if (prefs.putUChar(kPortalKey, 1) != 1) {
    Serial.println("Portal: failed to persist portal-reopen flag");
  }
  prefs.end();
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
