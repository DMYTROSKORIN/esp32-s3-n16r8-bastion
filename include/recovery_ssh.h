#pragma once

#include <stdint.h>

void startRecoverySshServer();

// Authenticated SSH sessions currently open (console or relay).
uint8_t sshActiveSessions();

