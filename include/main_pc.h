#pragma once

#include <stddef.h>
#include <stdint.h>

// Call periodically (net-monitor task) while Wi-Fi is connected: probes the
// configured PC and, when it answers, learns its MAC from the ARP table so
// `pc wake` works later with the PC powered off.
void mainPcMaintain();

// Live TCP probe of the configured PC's SSH port.
bool mainPcReachable(uint32_t timeoutMs = 700);

bool mainPcMacKnown();
// Writes "aa:bb:cc:dd:ee:ff" or "not learned yet".
void mainPcMacString(char* out, size_t outSize);

// Sends the Magic Packet burst to the Wi-Fi subnet broadcast address.
// Returns false when the MAC has not been learned or sending failed.
bool mainPcWake();

struct PingStats {
  uint32_t sent;
  uint32_t received;
  uint32_t minMs;
  uint32_t avgMs;
  uint32_t maxMs;
};

// ICMP echo burst (`count` requests, 500 ms apart, 1 s timeout each) to the
// configured PC. Blocks until the burst completes. Returns false when the ping
// session could not even be started (no Wi-Fi, allocation failure).
bool mainPcPing(uint8_t count, PingStats& stats);
