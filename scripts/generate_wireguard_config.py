Import("env")

import base64
import configparser
import ipaddress
from pathlib import Path


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
SECRETS_DIR = PROJECT_DIR / ".local-secrets"
GENERATED_DIR = PROJECT_DIR / ".generated-secrets"
GENERATED_HEADER = GENERATED_DIR / "wireguard_profiles.h"


def require_key(section, key, profile_name):
    value = section.get(key, "").strip()
    if not value:
        raise ValueError(f"{profile_name}: missing {key}")
    return value


def validate_wg_key(value, field, profile_name):
    try:
        decoded = base64.b64decode(value, validate=True)
    except Exception as exc:
        raise ValueError(f"{profile_name}: {field} is not valid base64") from exc
    if len(decoded) != 32:
        raise ValueError(f"{profile_name}: {field} must decode to 32 bytes")
    return value


def parse_endpoint(value, profile_name):
    if value.startswith("["):
        raise ValueError(f"{profile_name}: IPv6 endpoints are not supported")
    host, separator, port_text = value.rpartition(":")
    if not separator or not host:
        raise ValueError(f"{profile_name}: Endpoint must be host:port")
    port = int(port_text)
    if not 1 <= port <= 65535:
        raise ValueError(f"{profile_name}: Endpoint port is out of range")
    return host, port


def cpp_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_profile(filename, profile_name):
    path = SECRETS_DIR / filename
    if not path.is_file():
        raise ValueError(f"missing {path}")

    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str.lower
    try:
        with path.open(encoding="utf-8") as config_file:
            parser.read_file(config_file)
    except configparser.Error as exc:
        raise ValueError(f"{profile_name}: invalid WireGuard INI: {exc}") from exc

    if "Interface" not in parser or "Peer" not in parser:
        raise ValueError(f"{profile_name}: [Interface] and [Peer] are required")
    interface = parser["Interface"]
    peer = parser["Peer"]

    private_key = validate_wg_key(
        require_key(interface, "privatekey", profile_name), "PrivateKey", profile_name
    )
    public_key = validate_wg_key(
        require_key(peer, "publickey", profile_name), "PublicKey", profile_name
    )
    preshared_key = peer.get("presharedkey", "").strip()
    if preshared_key:
        validate_wg_key(preshared_key, "PresharedKey", profile_name)

    addresses = [part.strip() for part in require_key(
        interface, "address", profile_name
    ).split(",")]
    ipv4_interfaces = []
    for address in addresses:
        parsed = ipaddress.ip_interface(address)
        if parsed.version == 4:
            ipv4_interfaces.append(parsed)
    if len(ipv4_interfaces) != 1:
        raise ValueError(f"{profile_name}: exactly one IPv4 Interface Address is required")
    tunnel = ipv4_interfaces[0]

    endpoint, port = parse_endpoint(require_key(peer, "endpoint", profile_name), profile_name)
    keepalive = int(peer.get("persistentkeepalive", "0").strip() or "0")
    if not 0 <= keepalive <= 65535:
        raise ValueError(f"{profile_name}: PersistentKeepalive is out of range")

    mtu = int(interface.get("mtu", "1420").strip() or "1420")
    if not 576 <= mtu <= 1420:
        raise ValueError(f"{profile_name}: MTU must be between 576 and 1420")

    allowed_ipv4 = []
    ignored_ipv6 = []
    for value in require_key(peer, "allowedips", profile_name).split(","):
        network = ipaddress.ip_network(value.strip(), strict=False)
        if network.version == 4:
            allowed_ipv4.append((str(network.network_address), str(network.netmask)))
        else:
            ignored_ipv6.append(str(network))
    if not allowed_ipv4:
        raise ValueError(f"{profile_name}: at least one IPv4 AllowedIPs entry is required")

    if ignored_ipv6:
        print(f"WireGuard {profile_name}: ignoring unsupported IPv6 routes: {', '.join(ignored_ipv6)}")

    return {
        "name": profile_name,
        "private_key": private_key,
        "public_key": public_key,
        "preshared_key": preshared_key,
        "address": str(tunnel.ip),
        "netmask": str(tunnel.netmask),
        "endpoint": endpoint,
        "port": port,
        "keepalive": keepalive,
        "mtu": mtu,
        "allowed_ipv4": allowed_ipv4,
    }


def render_profile(profile):
    routes = ",\n".join(
        "      {%s, %s}" % (cpp_string(address), cpp_string(netmask))
        for address, netmask in profile["allowed_ipv4"]
    )
    psk = cpp_string(profile["preshared_key"]) if profile["preshared_key"] else "nullptr"
    return f"""  {{
    {cpp_string(profile['name'])},
    {cpp_string(profile['private_key'])},
    {cpp_string(profile['public_key'])},
    {psk},
    {cpp_string(profile['address'])},
    {cpp_string(profile['netmask'])},
    {cpp_string(profile['endpoint'])},
    {profile['port']},
    {profile['keepalive']},
    {profile['mtu']},
    {{
{routes}
    }},
    {len(profile['allowed_ipv4'])}
  }}"""


try:
    profiles = [
        load_profile("primary.conf", "server-1"),
        load_profile("secondary.conf", "server-2"),
    ]
except (OSError, ValueError) as exc:
    print(f"WireGuard configuration error: {exc}")
    env.Exit(1)

max_routes = max(len(profile["allowed_ipv4"]) for profile in profiles)
header = f"""// Generated from .local-secrets/*.conf. Do not commit or share this file.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct GeneratedWireGuardRoute {{
  const char* address;
  const char* netmask;
}};

struct GeneratedWireGuardProfile {{
  const char* name;
  const char* privateKey;
  const char* publicKey;
  const char* presharedKey;
  const char* address;
  const char* netmask;
  const char* endpoint;
  uint16_t port;
  uint16_t persistentKeepalive;
  uint16_t mtu;
  GeneratedWireGuardRoute allowedRoutes[{max_routes}];
  size_t allowedRouteCount;
}};

constexpr GeneratedWireGuardProfile kGeneratedWireGuardProfiles[] = {{
{render_profile(profiles[0])},
{render_profile(profiles[1])}
}};
"""

GENERATED_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
if not GENERATED_HEADER.exists() or GENERATED_HEADER.read_text(encoding="utf-8") != header:
    GENERATED_HEADER.write_text(header, encoding="utf-8")
GENERATED_HEADER.chmod(0o600)
env.Append(CPPPATH=[str(GENERATED_DIR)])
print("WireGuard: validated primary.conf and secondary.conf (secret values redacted)")
