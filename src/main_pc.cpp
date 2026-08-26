#include "main_pc.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <string.h>

#include "device_config.h"

namespace {
constexpr uint32_t kMaintainIntervalMs = 60000;
constexpr uint32_t kMaintainProbeTimeoutMs = 400;
constexpr uint16_t kWolPort = 9;

uint8_t pcMac[6] = {};
bool pcMacValid = false;
bool pcMacLoaded = false;
uint32_t lastMaintainMs = 0;

struct ArpQuery {
  ip4_addr_t ip;
  uint8_t mac[6];
  volatile bool found;
  SemaphoreHandle_t done;
};

// Runs inside the lwIP thread: the ARP table must not be read concurrently
// with its owner, and this Arduino core builds without TCPIP core locking.
void arpLookupInTcpipThread(void* argument) {
  ArpQuery* query = static_cast<ArpQuery*>(argument);
  struct eth_addr* ethRet = nullptr;
  const ip4_addr_t* ipRet = nullptr;
  if (netif_default != nullptr &&
      etharp_find_addr(netif_default, &query->ip, &ethRet, &ipRet) >= 0 &&
      ethRet != nullptr) {
    memcpy(query->mac, ethRet->addr, 6);
    query->found = true;
  }
  xSemaphoreGive(query->done);
}

bool arpLookup(const IPAddress& ip, uint8_t mac[6]) {
  static SemaphoreHandle_t done = xSemaphoreCreateBinary();
  static ArpQuery query;
  query.found = false;
  query.done = done;
  ip4_addr_set_u32(&query.ip, static_cast<uint32_t>(ip));
  if (tcpip_callback(arpLookupInTcpipThread, &query) != ERR_OK ||
      xSemaphoreTake(done, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }
  if (query.found) {
    memcpy(mac, query.mac, 6);
  }
  return query.found;
}

void ensureMacLoaded() {
  if (!pcMacLoaded) {
    pcMacLoaded = true;
    pcMacValid = deviceConfigMacLoad(pcMac);
  }
}

IPAddress configuredPcIp() {
  IPAddress ip;
  ip.fromString(gDeviceConfig.pcIp);
  return ip;
}
}  // namespace

bool mainPcReachable(uint32_t timeoutMs) {
  if (gDeviceConfig.pcIp[0] == '\0') {
    return false;
  }
  WiFiClient client;
  const bool connected =
      client.connect(configuredPcIp(), gDeviceConfig.pcPort, timeoutMs);
  client.stop();
  return connected;
}

void mainPcMaintain() {
  const uint32_t now = millis();
  if (now - lastMaintainMs < kMaintainIntervalMs && lastMaintainMs != 0) {
    return;
  }
  lastMaintainMs = now;
  if (WiFi.status() != WL_CONNECTED || gDeviceConfig.pcIp[0] == '\0') {
    return;
  }
  ensureMacLoaded();
  if (!mainPcReachable(kMaintainProbeTimeoutMs)) {
    return;
  }
  // The TCP probe just refreshed the ARP entry for the PC.
  uint8_t learned[6];
  if (arpLookup(configuredPcIp(), learned) &&
      (!pcMacValid || memcmp(learned, pcMac, 6) != 0)) {
    memcpy(pcMac, learned, 6);
    pcMacValid = true;
    deviceConfigMacStore(pcMac);
    Serial.printf("Main PC: learned MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                  pcMac[0], pcMac[1], pcMac[2], pcMac[3], pcMac[4], pcMac[5]);
  }
}

bool mainPcMacKnown() {
  ensureMacLoaded();
  return pcMacValid;
}

void mainPcMacString(char* out, size_t outSize) {
  ensureMacLoaded();
  if (pcMacValid) {
    snprintf(out, outSize, "%02x:%02x:%02x:%02x:%02x:%02x", pcMac[0], pcMac[1],
             pcMac[2], pcMac[3], pcMac[4], pcMac[5]);
  } else {
    snprintf(out, outSize, "not learned yet");
  }
}

bool mainPcWake() {
  ensureMacLoaded();
  if (!pcMacValid || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  uint8_t packet[102];
  memset(packet, 0xff, 6);
  for (size_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * 6, pcMac, 6);
  }

  const uint32_t ip = static_cast<uint32_t>(WiFi.localIP());
  const uint32_t mask = static_cast<uint32_t>(WiFi.subnetMask());
  const IPAddress broadcast(ip | ~mask);

  WiFiUDP udp;
  udp.begin(kWolPort);
  bool sent = true;
  for (int attempt = 0; attempt < 3; ++attempt) {
    sent = udp.beginPacket(broadcast, kWolPort) == 1 && sent;
    udp.write(packet, sizeof(packet));
    sent = udp.endPacket() == 1 && sent;
    delay(250);
  }
  udp.stop();
  return sent;
}
