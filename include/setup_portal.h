#pragma once

// Starts the open ESP32_SetUp access point with a captive setup portal.
void setupPortalStart();
// Drives DNS + HTTP and performs the post-apply restart; call from loop().
void setupPortalLoop();
