#pragma once

#include <stddef.h>

#include "device_config.h"

// Parses and validates a wg-quick client .conf uploaded through the setup
// portal. On success fills `profile`; on failure writes a short human-readable
// reason into `errorOut` and leaves `profile` unspecified.
bool parseWireGuardConf(const char* text, WgProfileConfig& profile,
                        char* errorOut, size_t errorSize);
