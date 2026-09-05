#pragma once

#include <stdint.h>

// Wi-Fi/internet supervision, moved off the Arduino loop() into its own
// low-priority task so the blocking TCP probes (up to ~2.4 s) never stall the
// LED animation, the BOOT-button handling or the watchdog feed.
enum class NetState : uint8_t {
  kConnecting,  // Wi-Fi association pending or lost.
  kOnline,      // Wi-Fi up and a TCP probe to a public DNS server succeeded.
  kNoInternet,  // Wi-Fi up, but nothing beyond the router answers.
};

// Starts the supervision task. Call once from setup() after WiFi.begin().
void netMonitorStart();

NetState netMonitorState();
const char* netStateName(NetState state);

// Seconds Wi-Fi has been continuously associated (0 while disconnected).
uint32_t netMonitorWifiUptimeSeconds();
