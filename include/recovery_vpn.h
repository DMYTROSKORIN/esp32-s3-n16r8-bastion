#pragma once

#include <Arduino.h>

void startRecoveryVpn();
void recoveryVpnRequestFailover();
void recoveryVpnRequestPrimary();

bool recoveryVpnOnline();
const char* recoveryVpnStateName();
const char* recoveryVpnActiveProfileName();
const char* recoveryVpnEndpoint();
IPAddress recoveryVpnAddress();
uint32_t recoveryVpnHandshakeAgeSeconds();
uint8_t recoveryVpnConsecutiveFailures();
