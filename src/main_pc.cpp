#include "main_pc.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <ping/ping_sock.h>
#include <string.h>

#include "device_config.h"
#include "event_log.h"

namespace {
constexpr uint32_t kMaintainIntervalMs = 60000;
constexpr uint32_t kMaintainProbeTimeoutMs = 400;
constexpr uint16_t kWolPort = 9;

// pcMac/pcMacValid/pcMacLoaded are written from the net-monitor task
// (learning a freshly-seen MAC, core 1) and read from the SSH task
// (dashboard/pc wake/pc status, core 0). A spinlock plus copying a snapshot before use
// keeps every read and write of these three fields consistent with each
// other, instead of a formatter or a wake packet potentially mixing bytes
// from an old and a newly-learned MAC.
portMUX_TYPE pcMacMux = portMUX_INITIALIZER_UNLOCKED;
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
  // Scan every entry instead of using etharp_find_addr(netif_default, ...):
  // on this Arduino-ESP32/esp_netif build, lwIP's netif_default does not
  // reliably match the netif the Wi-Fi STA's ARP entries are recorded
  // under, so filtering by it drops entries that are genuinely present.
  for (uint8_t slot = 0; slot < ARP_TABLE_SIZE; ++slot) {
    ip4_addr_t* entryIp = nullptr;
    struct netif* entryNetif = nullptr;
    struct eth_addr* entryEth = nullptr;
    if (etharp_get_entry(slot, &entryIp, &entryNetif, &entryEth) &&
        entryIp != nullptr && ip4_addr_cmp(entryIp, &query->ip)) {
      memcpy(query->mac, entryEth->addr, 6);
      query->found = true;
      break;
    }
  }
  xSemaphoreGive(query->done);
}

bool arpLookup(const IPAddress& ip, uint8_t mac[6]) {
  static SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr) {
    return false;
  }
  // A callback from a previous call that timed out here can still fire
  // later and give the semaphore; drain any such leftover token first so
  // this call's own wait below can't be satisfied by stale data.
  while (xSemaphoreTake(done, 0) == pdTRUE) {
  }
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
  if (pcMacLoaded) {
    return;
  }
  // NVS access happens outside the critical section (it can be slow); the
  // section only publishes the result, and the pcMacLoaded check inside it
  // makes a race between two first-callers harmless (the loser's read is
  // simply discarded instead of double-applied).
  uint8_t loaded[6];
  const bool valid = deviceConfigMacLoad(loaded);
  portENTER_CRITICAL(&pcMacMux);
  if (!pcMacLoaded) {
    pcMacLoaded = true;
    if (valid) {
      memcpy(pcMac, loaded, 6);
      pcMacValid = true;
    }
  }
  portEXIT_CRITICAL(&pcMacMux);
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
  // Only start the cooldown once Wi-Fi is actually up, so a premature call
  // during the connecting phase doesn't burn the first real attempt.
  if (WiFi.status() != WL_CONNECTED || gDeviceConfig.pcIp[0] == '\0') {
    return;
  }
  const uint32_t now = millis();
  if (lastMaintainMs != 0 && now - lastMaintainMs < kMaintainIntervalMs) {
    return;
  }
  lastMaintainMs = now;

  ensureMacLoaded();
  if (!mainPcReachable(kMaintainProbeTimeoutMs)) {
    return;
  }
  // The TCP probe just refreshed the ARP entry for the PC.
  uint8_t learned[6];
  if (!arpLookup(configuredPcIp(), learned)) {
    return;
  }

  bool changed = false;
  portENTER_CRITICAL(&pcMacMux);
  if (!pcMacValid || memcmp(learned, pcMac, 6) != 0) {
    memcpy(pcMac, learned, 6);
    pcMacValid = true;
    changed = true;
  }
  portEXIT_CRITICAL(&pcMacMux);

  if (changed) {
    deviceConfigMacStore(learned);
    eventLogf("Main PC: learned MAC %02x:%02x:%02x:%02x:%02x:%02x", learned[0],
              learned[1], learned[2], learned[3], learned[4], learned[5]);
  }
}

bool mainPcMacKnown() {
  ensureMacLoaded();
  portENTER_CRITICAL(&pcMacMux);
  const bool valid = pcMacValid;
  portEXIT_CRITICAL(&pcMacMux);
  return valid;
}

void mainPcMacString(char* out, size_t outSize) {
  ensureMacLoaded();
  uint8_t snapshot[6];
  portENTER_CRITICAL(&pcMacMux);
  const bool valid = pcMacValid;
  memcpy(snapshot, pcMac, 6);
  portEXIT_CRITICAL(&pcMacMux);
  if (valid) {
    snprintf(out, outSize, "%02x:%02x:%02x:%02x:%02x:%02x", snapshot[0],
             snapshot[1], snapshot[2], snapshot[3], snapshot[4], snapshot[5]);
  } else {
    snprintf(out, outSize, "not learned yet");
  }
}

bool mainPcWake() {
  ensureMacLoaded();
  uint8_t snapshot[6];
  portENTER_CRITICAL(&pcMacMux);
  const bool valid = pcMacValid;
  memcpy(snapshot, pcMac, 6);
  portEXIT_CRITICAL(&pcMacMux);
  if (!valid || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  uint8_t packet[102];
  memset(packet, 0xff, 6);
  for (size_t repeat = 0; repeat < 16; ++repeat) {
    memcpy(packet + 6 + repeat * 6, snapshot, 6);
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

namespace {
struct PingContext {
  PingStats stats;
  uint32_t sumMs;
  SemaphoreHandle_t done;
};

void onPingSuccess(esp_ping_handle_t handle, void* argument) {
  PingContext* context = static_cast<PingContext*>(argument);
  uint32_t elapsedMs = 0;
  esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsedMs, sizeof(elapsedMs));
  ++context->stats.received;
  context->sumMs += elapsedMs;
  if (context->stats.received == 1 || elapsedMs < context->stats.minMs) {
    context->stats.minMs = elapsedMs;
  }
  if (elapsedMs > context->stats.maxMs) {
    context->stats.maxMs = elapsedMs;
  }
}

void onPingTimeout(esp_ping_handle_t, void*) {}

void onPingEnd(esp_ping_handle_t handle, void* argument) {
  PingContext* context = static_cast<PingContext*>(argument);
  uint32_t sent = 0;
  esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
  context->stats.sent = sent;
  if (context->stats.received > 0) {
    context->stats.avgMs = context->sumMs / context->stats.received;
  }
  xSemaphoreGive(context->done);
}
}  // namespace

bool mainPcPing(uint8_t count, PingStats& stats) {
  stats = PingStats{};
  if (gDeviceConfig.pcIp[0] == '\0' || WiFi.status() != WL_CONNECTED || count == 0) {
    return false;
  }
  // One ping burst at a time: the SSH console is single-session, so a static
  // context is enough and avoids a heap allocation per command.
  static PingContext context;
  static SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr) {
    return false;
  }
  while (xSemaphoreTake(done, 0) == pdTRUE) {
  }
  context = PingContext{};
  context.done = done;

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.count = count;
  config.interval_ms = 500;
  config.timeout_ms = 1000;
  config.target_addr.type = IPADDR_TYPE_V4;
  config.target_addr.u_addr.ip4.addr = static_cast<uint32_t>(configuredPcIp());

  esp_ping_callbacks_t callbacks = {};
  callbacks.cb_args = &context;
  callbacks.on_ping_success = onPingSuccess;
  callbacks.on_ping_timeout = onPingTimeout;
  callbacks.on_ping_end = onPingEnd;

  esp_ping_handle_t handle = nullptr;
  if (esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) {
    return false;
  }
  esp_ping_start(handle);
  // Worst case: count x (interval + timeout), plus slack.
  const TickType_t budget =
      pdMS_TO_TICKS(static_cast<uint32_t>(count) * 1500UL + 2000UL);
  const bool finished = xSemaphoreTake(done, budget) == pdTRUE;
  esp_ping_stop(handle);
  esp_ping_delete_session(handle);
  stats = context.stats;
  return finished;
}
