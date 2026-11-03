#pragma once
#include <WiFiUdp.h>

// This product's paired boards. Never learn an arbitrary neighbour as the camera.
static constexpr const char *CAT_MAIN_ID = "1c:db:d4:75:10:ec";
static constexpr const char *CAT_CAMERA_ID = "88:f1:55:67:97:d0";
static constexpr uint16_t CAT_DISCOVERY_PORT = 42137;
#ifdef CAT_REMOTE_CAMERA
static constexpr const char *CAT_ROLE = "camera";
static constexpr const char *CAT_PEER_ROLE = "main";
static constexpr const char *CAT_PEER_ID = CAT_MAIN_ID;
#else
static constexpr const char *CAT_ROLE = "main";
static constexpr const char *CAT_PEER_ROLE = "camera";
static constexpr const char *CAT_PEER_ID = CAT_CAMERA_ID;
#endif
WiFiUDP discoveryUdp;
SemaphoreHandle_t discoveryMutex = nullptr;
IPAddress discoveryLocal, discoveryPeer;
String discoveryBssid;
uint32_t discoverySeen = 0, discoverySent = 0;
bool discoveryListening = false, discoveryHasPeer = false;

void initializeDiscovery() { discoveryMutex = xSemaphoreCreateMutex(); }
void serviceDiscovery() {
  if (!discoveryMutex) return;
  const bool online = WiFi.status() == WL_CONNECTED;
  const IPAddress local = online ? WiFi.localIP() : IPAddress();
  const String bssid = online ? WiFi.BSSIDstr() : String();
  xSemaphoreTake(discoveryMutex, portMAX_DELAY);
  if (local != discoveryLocal || bssid != discoveryBssid || !online) {
    discoveryUdp.stop(); discoveryListening = false;
    discoveryHasPeer = false; discoveryPeer = IPAddress();
    discoveryLocal = local; discoveryBssid = bssid; discoverySent = 0;
  }
  if (!online || !uint32_t(local)) { xSemaphoreGive(discoveryMutex); return; }
  if (!discoveryListening) discoveryListening = discoveryUdp.begin(CAT_DISCOVERY_PORT);
  for (int count = 0; discoveryListening && count < 4; ++count) {
    const int length = discoveryUdp.parsePacket();
    if (!length) break;
    const IPAddress sender = discoveryUdp.remoteIP();
    const uint16_t senderPort = discoveryUdp.remotePort();
    char packet[192];
    if (length >= static_cast<int>(sizeof(packet))) { while (discoveryUdp.available()) discoveryUdp.read(); continue; }
    const int read = discoveryUdp.read(reinterpret_cast<uint8_t *>(packet), sizeof(packet)-1);
    if (read <= 0) continue;
    packet[read] = 0;
    char role[12], mac[24];
    const bool subnet = (uint32_t(sender) & uint32_t(WiFi.subnetMask())) == (uint32_t(local) & uint32_t(WiFi.subnetMask()));
    if (senderPort == CAT_DISCOVERY_PORT && subnet && sender != local &&
        sscanf(packet, "CAT-HOME/1 %11s %23s", role, mac) == 2 &&
        !strcmp(role, CAT_PEER_ROLE) && !strcasecmp(mac, CAT_PEER_ID)) {
      discoveryPeer = sender; discoverySeen = millis(); discoveryHasPeer = true;
    }
  }
  if (discoveryHasPeer && millis()-discoverySeen > 15000) discoveryHasPeer = false;
  if (discoveryListening && (!discoverySent || millis()-discoverySent >= 3000)) {
    String mac = WiFi.macAddress(); mac.toLowerCase();
    const String packet = String("CAT-HOME/1 ") + CAT_ROLE + " " + mac;
    IPAddress broadcast(uint32_t(local) | ~uint32_t(WiFi.subnetMask()));
    if (discoveryUdp.beginPacket(broadcast, CAT_DISCOVERY_PORT)) {
      discoveryUdp.write(reinterpret_cast<const uint8_t *>(packet.c_str()), packet.length()); discoveryUdp.endPacket();
    }
    // Unicast heartbeats also work when the AP suppresses subsequent broadcasts.
    if (discoveryHasPeer && discoveryUdp.beginPacket(discoveryPeer, CAT_DISCOVERY_PORT)) {
      discoveryUdp.write(reinterpret_cast<const uint8_t *>(packet.c_str()), packet.length()); discoveryUdp.endPacket();
    }
    discoverySent = millis();
  }
  xSemaphoreGive(discoveryMutex);
}

String deviceIdentityJson() {
  String mac = WiFi.macAddress(); mac.toLowerCase();
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "product", "cat-home");
  cJSON_AddStringToObject(json, "role", CAT_ROLE);
  cJSON_AddStringToObject(json, "id", mac.c_str());
  cJSON_AddNumberToObject(json, "discoveryVersion", 1);
  cJSON_AddStringToObject(json, "ip", WiFi.localIP().toString().c_str());
  cJSON_AddStringToObject(json, "ssid", WiFi.SSID().c_str());
  cJSON *peer = cJSON_AddObjectToObject(json, "peer");
  cJSON_AddStringToObject(peer, "id", CAT_PEER_ID);
  cJSON_AddStringToObject(peer, "role", CAT_PEER_ROLE);
  if (discoveryMutex) {
    xSemaphoreTake(discoveryMutex, portMAX_DELAY);
    const bool fresh = discoveryHasPeer && WiFi.status()==WL_CONNECTED &&
      discoveryLocal==WiFi.localIP() && discoveryBssid==WiFi.BSSIDstr() && millis()-discoverySeen<=15000;
    cJSON_AddBoolToObject(peer, "online", fresh);
    cJSON_AddStringToObject(peer, "ip", fresh ? discoveryPeer.toString().c_str() : "");
    xSemaphoreGive(discoveryMutex);
  } else { cJSON_AddBoolToObject(peer, "online", false); cJSON_AddStringToObject(peer, "ip", ""); }
  char *encoded = cJSON_PrintUnformatted(json);
  String result = encoded ? encoded : "{}";
  cJSON_free(encoded); cJSON_Delete(json); return result;
}
esp_err_t serveDeviceIdentity(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "application/json");
  String json = deviceIdentityJson();
  return httpd_resp_send(req, json.c_str(), json.length());
}
void registerDiscoveryRoutes(httpd_handle_t server) {
  for (const char *uri : {"/api/device", "/api/devices"}) {
    httpd_uri_t route={}; route.uri=uri; route.method=HTTP_GET; route.handler=serveDeviceIdentity;
    httpd_register_uri_handler(server, &route);
  }
}
