#pragma once

#include <stddef.h>
#include <stdint.h>

// Call periodically from the main loop while Wi-Fi is connected: probes the
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
