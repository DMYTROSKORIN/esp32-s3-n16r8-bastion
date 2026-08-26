"""Compresses portal/index.html into a PROGMEM header inside the build dir."""

Import("env")

import gzip
from pathlib import Path

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
SOURCE = PROJECT_DIR / "portal" / "index.html"
GENERATED_DIR = Path(env.subst("$BUILD_DIR")) / "generated"
HEADER = GENERATED_DIR / "portal_html.h"

data = gzip.compress(SOURCE.read_bytes(), 9, mtime=0)
lines = ",\n  ".join(
    ", ".join(f"0x{byte:02x}" for byte in data[offset : offset + 12])
    for offset in range(0, len(data), 12)
)
header = f"""// Generated from portal/index.html. Do not edit.
#pragma once

#include <pgmspace.h>
#include <stddef.h>

constexpr size_t kPortalHtmlGzLength = {len(data)};
const uint8_t kPortalHtmlGz[] PROGMEM = {{
  {lines}
}};
"""

GENERATED_DIR.mkdir(parents=True, exist_ok=True)
if not HEADER.exists() or HEADER.read_text(encoding="utf-8") != header:
    HEADER.write_text(header, encoding="utf-8")
    print(f"Portal page: embedded {SOURCE.stat().st_size} B -> {len(data)} B gzip")
env.Append(CPPPATH=[str(GENERATED_DIR)])
