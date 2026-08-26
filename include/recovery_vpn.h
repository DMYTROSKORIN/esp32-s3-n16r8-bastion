#pragma once

#include <Arduino.h>

void startRecoveryVpn();
void recoveryVpnRequestFailover();
void recoveryVpnRequestPrimary();

bool recoveryVpnConfigured();
bool recoveryVpnOnline();
const char* recoveryVpnStateName();
const char* recoveryVpnActiveProfileName();
// 1-based index of the currently active profile (1 = primary, 2 = secondary),
// or 0 when no profile is active.
uint8_t recoveryVpnActiveProfileNumber();
const char* recoveryVpnEndpoint();
IPAddress recoveryVpnAddress();
uint32_t recoveryVpnHandshakeAgeSeconds();
uint8_t recoveryVpnConsecutiveFailures();
