#include "net_monitor.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <atomic>
#include <esp_task_wdt.h>

#include "event_log.h"
#include "main_pc.h"
#include "recovery_status.h"

namespace {
constexpr uint32_t kTaskStack = 6144;
constexpr uint32_t kTickMs = 250;
constexpr uint32_t kReconnectIntervalMs = 10000;
constexpr uint32_t kInternetCheckIntervalMs = 10000;
constexpr uint32_t kInternetConnectTimeoutMs = 1000;
// A radio that stays unassociated this long is almost never going to come
// back by itself (driver wedged after a brownout, DHCP state stuck, router
// rebooted onto a different channel while we were mid-scan). A full restart
// is cheap, takes ~3 s and re-runs every init path from scratch; without it a
// headless device could sit unreachable for days.
constexpr uint32_t kWifiLostRebootMs = 10 * 60 * 1000;

const IPAddress kInternetCheckHosts[] = {
    IPAddress(8, 8, 8, 8),
    IPAddress(8, 8, 4, 4),
};
constexpr uint16_t kDnsPort = 53;

std::atomic<NetState> netState{NetState::kConnecting};
std::atomic<uint32_t> wifiAssociatedSinceMs{0};

bool hasInternetAccess() {
  for (const IPAddress& host : kInternetCheckHosts) {
    WiFiClient client;
    const bool ok = client.connect(host, kDnsPort, kInternetConnectTimeoutMs);
    client.stop();
    if (ok) {
      return true;
    }
  }
  return false;
}

void setState(NetState next) {
  if (netState.exchange(next) != next) {
    eventLogf("Net: %s", netStateName(next));
  }
}

void netMonitorTask(void*) {
  // Subscribed to the task watchdog: every blocking call in here is bounded
  // (probe timeouts + PC probe well under 5 s), so a missed feed means lwIP or
  // the Wi-Fi driver has genuinely hung underneath us.
  esp_task_wdt_add(nullptr);

  uint32_t lastReconnectMs = 0;
  uint32_t lastInternetCheckMs = 0;
  uint32_t wifiLostSinceMs = 0;

  while (true) {
    esp_task_wdt_reset();
    const uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      wifiAssociatedSinceMs = 0;
      if (wifiLostSinceMs == 0) {
        wifiLostSinceMs = now;
      }
      setState(NetState::kConnecting);
      lastInternetCheckMs = 0;
      if (now - lastReconnectMs >= kReconnectIntervalMs) {
        eventLogf("Wi-Fi: not associated, retrying saved network");
        WiFi.reconnect();
        lastReconnectMs = now;
      }
      if (now - wifiLostSinceMs >= kWifiLostRebootMs) {
        eventLogf("Wi-Fi: unassociated for %lu min, restarting to recover the radio",
                  static_cast<unsigned long>(kWifiLostRebootMs / 60000UL));
        delay(200);
        ESP.restart();
      }
      delay(kTickMs);
      continue;
    }

    if (wifiAssociatedSinceMs == 0) {
      wifiAssociatedSinceMs = now != 0 ? now : 1;
    }
    wifiLostSinceMs = 0;

    if (lastInternetCheckMs == 0 ||
        now - lastInternetCheckMs >= kInternetCheckIntervalMs) {
      lastInternetCheckMs = now;
      const bool online = hasInternetAccess();
      setState(online ? NetState::kOnline : NetState::kNoInternet);
    }

    // Learns/refreshes the PC's MAC once a minute (its own cooldown).
    mainPcMaintain();
    delay(kTickMs);
  }
}
}  // namespace

void netMonitorStart() {
  if (xTaskCreatePinnedToCore(netMonitorTask, "net-monitor", kTaskStack, nullptr,
                              1, nullptr, 1) != pdPASS) {
    eventLogf("Net: failed to create monitor task");
  }
}

NetState netMonitorState() { return netState.load(); }

const char* netStateName(NetState state) {
  switch (state) {
    case NetState::kConnecting:
      return "CONNECTING";
    case NetState::kOnline:
      return "ONLINE";
    case NetState::kNoInternet:
      return "NO_INTERNET";
  }
  return "UNKNOWN";
}

uint32_t netMonitorWifiUptimeSeconds() {
  const uint32_t since = wifiAssociatedSinceMs.load();
  return since == 0 ? 0 : (millis() - since) / 1000UL;
}

// recovery_status.h
const char* recoveryNetworkStateName() { return netStateName(netState.load()); }

bool recoveryInternetAvailable() { return netState.load() == NetState::kOnline; }
