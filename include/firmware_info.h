#pragma once

// Set from platformio.ini (-DFIRMWARE_VERSION=\"x.y.z\"); the fallback keeps
// ad-hoc builds from other toolchains compiling.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// Target hardware. This firmware is tuned for exactly this module: 16 MB
// quad-SPI flash (QIO @ 80 MHz) and 8 MB octal PSRAM (OPI). Other ESP32-S3
// variants need a different partition table / memory_type in platformio.ini.
#define FIRMWARE_TARGET_BOARD "ESP32-S3-N16R8"
