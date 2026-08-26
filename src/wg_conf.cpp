#include "wg_conf.h"

#include <ctype.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
enum class Section : uint8_t { kNone, kInterface, kPeer, kOther };

struct Parser {
  char* errorOut;
  size_t errorSize;

  bool fail(const char* message) {
    snprintf(errorOut, errorSize, "%s", message);
    return false;
  }
};

// Copies [begin, end) into out with surrounding whitespace removed.
// Returns false when the trimmed value does not fit.
bool copyTrimmed(const char* begin, const char* end, char* out, size_t outSize) {
  while (begin < end && isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (end > begin && isspace(static_cast<unsigned char>(end[-1]))) {
    --end;
  }
  const size_t length = static_cast<size_t>(end - begin);
  if (length >= outSize) {
    return false;
  }
  memcpy(out, begin, length);
  out[length] = '\0';
  return true;
}

bool isWgKey(const char* value) {
  unsigned char decoded[34];
  size_t decodedLength = 0;
  return mbedtls_base64_decode(decoded, sizeof(decoded), &decodedLength,
                               reinterpret_cast<const unsigned char*>(value),
                               strlen(value)) == 0 &&
         decodedLength == 32;
}

// Parses a dotted-quad IPv4 address; `end` receives the first unconsumed
// character. Returns false on malformed input.
bool parseIpv4(const char* text, uint32_t* address, const char** end) {
  uint32_t value = 0;
  for (int octet = 0; octet < 4; ++octet) {
    if (!isdigit(static_cast<unsigned char>(*text))) {
      return false;
    }
    uint32_t part = 0;
    int digits = 0;
    while (isdigit(static_cast<unsigned char>(*text)) && digits < 3) {
      part = part * 10 + static_cast<uint32_t>(*text - '0');
      ++text;
      ++digits;
    }
    if (part > 255) {
      return false;
    }
    value = (value << 8) | part;
    if (octet < 3) {
      if (*text != '.') {
        return false;
      }
      ++text;
    }
  }
  *address = value;
  *end = text;
  return true;
}

void formatIpv4(uint32_t address, char* out, size_t outSize) {
  snprintf(out, outSize, "%u.%u.%u.%u", (address >> 24) & 0xff,
           (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff);
}

uint32_t prefixToMask(int prefix) {
  return prefix == 0 ? 0 : 0xffffffffUL << (32 - prefix);
}

// Parses "a.b.c.d" or "a.b.c.d/nn" ensuring nothing trails the prefix.
bool parseIpv4WithPrefix(const char* text, uint32_t* address, int* prefix) {
  const char* rest = nullptr;
  if (!parseIpv4(text, address, &rest)) {
    return false;
  }
  if (*rest == '\0') {
    *prefix = 32;
    return true;
  }
  if (*rest != '/') {
    return false;
  }
  char* prefixEnd = nullptr;
  const long value = strtol(rest + 1, &prefixEnd, 10);
  if (prefixEnd == rest + 1 || *prefixEnd != '\0' || value < 0 || value > 32) {
    return false;
  }
  *prefix = static_cast<int>(value);
  return true;
}

bool parseUint16(const char* text, long minimum, long maximum, uint16_t* out) {
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < minimum || value > maximum) {
    return false;
  }
  *out = static_cast<uint16_t>(value);
  return true;
}

bool handleAddress(Parser& parser, const char* value, WgProfileConfig& profile,
                   bool* addressSeen) {
  // Comma-separated list; exactly one IPv4 interface address is required.
  char entry[64];
  const char* cursor = value;
  while (*cursor != '\0') {
    const char* comma = strchr(cursor, ',');
    const char* entryEnd = comma != nullptr ? comma : cursor + strlen(cursor);
    if (!copyTrimmed(cursor, entryEnd, entry, sizeof(entry))) {
      return parser.fail("Interface Address entry is too long");
    }
    cursor = comma != nullptr ? comma + 1 : entryEnd;
    if (entry[0] == '\0') {
      continue;
    }
    if (strchr(entry, ':') != nullptr) {
      continue;  // IPv6 addresses are ignored, matching the old validator.
    }
    uint32_t address = 0;
    int prefix = 0;
    if (!parseIpv4WithPrefix(entry, &address, &prefix)) {
      return parser.fail("Interface Address is not a valid IPv4 address");
    }
    if (*addressSeen) {
      return parser.fail("exactly one IPv4 Interface Address is required");
    }
    *addressSeen = true;
    formatIpv4(address, profile.address, sizeof(profile.address));
    formatIpv4(prefixToMask(prefix), profile.netmask, sizeof(profile.netmask));
  }
  return true;
}

bool handleAllowedIps(Parser& parser, const char* value,
                      WgProfileConfig& profile) {
  char entry[64];
  const char* cursor = value;
  while (*cursor != '\0') {
    const char* comma = strchr(cursor, ',');
    const char* entryEnd = comma != nullptr ? comma : cursor + strlen(cursor);
    if (!copyTrimmed(cursor, entryEnd, entry, sizeof(entry))) {
      return parser.fail("AllowedIPs entry is too long");
    }
    cursor = comma != nullptr ? comma + 1 : entryEnd;
    if (entry[0] == '\0') {
      continue;
    }
    if (strchr(entry, ':') != nullptr) {
      continue;  // Unsupported IPv6 routes are ignored.
    }
    uint32_t address = 0;
    int prefix = 0;
    if (!parseIpv4WithPrefix(entry, &address, &prefix)) {
      return parser.fail("AllowedIPs entry is not a valid IPv4 network");
    }
    if (profile.routeCount >= kMaxWgRoutes) {
      return parser.fail("too many IPv4 AllowedIPs entries (4 max)");
    }
    WgRouteConfig& route = profile.routes[profile.routeCount++];
    const uint32_t mask = prefixToMask(prefix);
    formatIpv4(address & mask, route.address, sizeof(route.address));
    formatIpv4(mask, route.netmask, sizeof(route.netmask));
  }
  return true;
}

bool handleEndpoint(Parser& parser, const char* value,
                    WgProfileConfig& profile) {
  if (value[0] == '[') {
    return parser.fail("IPv6 endpoints are not supported");
  }
  const char* separator = strrchr(value, ':');
  if (separator == nullptr || separator == value) {
    return parser.fail("Endpoint must be host:port");
  }
  if (!copyTrimmed(value, separator, profile.endpoint,
                   sizeof(profile.endpoint)) ||
      profile.endpoint[0] == '\0') {
    return parser.fail("Endpoint host is empty or too long");
  }
  if (!parseUint16(separator + 1, 1, 65535, &profile.port)) {
    return parser.fail("Endpoint port is out of range");
  }
  return true;
}
}  // namespace

bool parseWireGuardConf(const char* text, WgProfileConfig& profile,
                        char* errorOut, size_t errorSize) {
  Parser parser{errorOut, errorSize};
  memset(&profile, 0, sizeof(profile));
  profile.mtu = 1420;

  Section section = Section::kNone;
  bool interfaceSeen = false;
  bool peerSeen = false;
  bool addressSeen = false;
  char key[32];
  char value[128];

  const char* cursor = text;
  while (*cursor != '\0') {
    const char* newline = strchr(cursor, '\n');
    const char* lineEnd = newline != nullptr ? newline : cursor + strlen(cursor);
    const char* line = cursor;
    cursor = newline != nullptr ? newline + 1 : lineEnd;

    while (line < lineEnd && isspace(static_cast<unsigned char>(*line))) {
      ++line;
    }
    while (lineEnd > line && isspace(static_cast<unsigned char>(lineEnd[-1]))) {
      --lineEnd;
    }
    if (line == lineEnd || *line == '#' || *line == ';') {
      continue;
    }

    if (*line == '[') {
      if (lineEnd[-1] != ']') {
        return parser.fail("malformed section header");
      }
      char name[24];
      if (!copyTrimmed(line + 1, lineEnd - 1, name, sizeof(name))) {
        return parser.fail("malformed section header");
      }
      if (strcasecmp(name, "Interface") == 0) {
        if (interfaceSeen) {
          return parser.fail("multiple [Interface] sections");
        }
        interfaceSeen = true;
        section = Section::kInterface;
      } else if (strcasecmp(name, "Peer") == 0) {
        if (peerSeen) {
          return parser.fail("multiple [Peer] sections are not supported");
        }
        peerSeen = true;
        section = Section::kPeer;
      } else {
        section = Section::kOther;
      }
      continue;
    }

    const char* equals = static_cast<const char*>(memchr(line, '=', lineEnd - line));
    if (equals == nullptr) {
      return parser.fail("expected key = value");
    }
    if (!copyTrimmed(line, equals, key, sizeof(key)) || key[0] == '\0') {
      return parser.fail("expected key = value");
    }
    // Keys and preshared/private keys are base64 and never exceed the value
    // buffer; only AllowedIPs may, so it gets the raw slice instead.
    const bool valueFits =
        copyTrimmed(equals + 1, lineEnd, value, sizeof(value));

    if (section == Section::kInterface) {
      if (strcasecmp(key, "PrivateKey") == 0) {
        if (!valueFits || !isWgKey(value)) {
          return parser.fail("PrivateKey is not a valid WireGuard key");
        }
        strcpy(profile.privateKey, value);
      } else if (strcasecmp(key, "Address") == 0) {
        if (!valueFits) {
          return parser.fail("Interface Address list is too long");
        }
        if (!handleAddress(parser, value, profile, &addressSeen)) {
          return false;
        }
      } else if (strcasecmp(key, "MTU") == 0) {
        if (!valueFits || !parseUint16(value, 576, 1420, &profile.mtu)) {
          return parser.fail("MTU must be between 576 and 1420");
        }
      }
    } else if (section == Section::kPeer) {
      if (strcasecmp(key, "PublicKey") == 0) {
        if (!valueFits || !isWgKey(value)) {
          return parser.fail("PublicKey is not a valid WireGuard key");
        }
        strcpy(profile.publicKey, value);
      } else if (strcasecmp(key, "PresharedKey") == 0) {
        if (!valueFits || !isWgKey(value)) {
          return parser.fail("PresharedKey is not a valid WireGuard key");
        }
        strcpy(profile.presharedKey, value);
      } else if (strcasecmp(key, "Endpoint") == 0) {
        if (!valueFits) {
          return parser.fail("Endpoint is too long");
        }
        if (!handleEndpoint(parser, value, profile)) {
          return false;
        }
      } else if (strcasecmp(key, "PersistentKeepalive") == 0) {
        if (!valueFits ||
            !parseUint16(value, 0, 65535, &profile.persistentKeepalive)) {
          return parser.fail("PersistentKeepalive is out of range");
        }
      } else if (strcasecmp(key, "AllowedIPs") == 0) {
        char allowed[256];
        if (!copyTrimmed(equals + 1, lineEnd, allowed, sizeof(allowed))) {
          return parser.fail("AllowedIPs list is too long");
        }
        if (!handleAllowedIps(parser, allowed, profile)) {
          return false;
        }
      }
    }
  }

  if (!interfaceSeen || !peerSeen) {
    return parser.fail("[Interface] and [Peer] are required");
  }
  if (profile.privateKey[0] == '\0') {
    return parser.fail("missing PrivateKey");
  }
  if (profile.publicKey[0] == '\0') {
    return parser.fail("missing PublicKey");
  }
  if (!addressSeen) {
    return parser.fail("exactly one IPv4 Interface Address is required");
  }
  if (profile.endpoint[0] == '\0') {
    return parser.fail("missing Endpoint");
  }
  if (profile.routeCount == 0) {
    return parser.fail("at least one IPv4 AllowedIPs entry is required");
  }
  return true;
}
