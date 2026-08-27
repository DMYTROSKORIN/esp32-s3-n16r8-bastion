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
// sshd port of the active profile's VPN server itself (portal-provisioned
// per profile), for rendering the jump-host `pc ssh` command. 0 when no
// profile is active.
uint16_t recoveryVpnServerSshPort();
IPAddress recoveryVpnAddress();
uint32_t recoveryVpnHandshakeAgeSeconds();
uint8_t recoveryVpnConsecutiveFailures();
