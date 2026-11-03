/* Cat Home camera test: native USB CDC snapshots, no feeding commands.
 * Pins verified against ESP32-S3CAM schematic, page 1.
 * Compile with ESP32S3, 8MB flash, OPI PSRAM, hardware USB CDC enabled.
 */
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <errno.h>
#include "web_assets.h"
#include <string.h>
#include "esp_system.h"
#include "feeding.h"
#include "lighting.h"
#include "environment.h"

bool cameraReady = false;
bool neutralReady = false;
esp_err_t cameraError = ESP_FAIL;
char command[256];
size_t commandLength = 0;
bool overflow = false;
SemaphoreHandle_t cameraMutex;
httpd_handle_t webServer = nullptr;
Preferences preferences;
SemaphoreHandle_t streamSlots;
#include "wifi_settings.h"
#include "device_discovery.h"

String statusJson() {
  sensor_t *sensor = cameraReady ? esp_camera_sensor_get() : nullptr;
  bool motorsNeutral = false;
  if (feedMutex) {
    xSemaphoreTake(feedMutex, portMAX_DELAY);
    motorsNeutral = feedReady && feedPulses[0] == 1500 && feedPulses[1] == 1500 && feedPulses[2] == 1500;
    xSemaphoreGive(feedMutex);
  }
  String json = "{\"ready\":" + String(cameraReady ? "true" : "false");
  json += ",\"error\":" + String(static_cast<int>(cameraError));
  json += ",\"sensorId\":" + String(sensor ? sensor->id.PID : 0);
  json += ",\"psram\":" + String(ESP.getPsramSize());
  json += ",\"neutral\":" + String(motorsNeutral ? "true" : "false");
  json += ",\"firmware\":\"cat-home-pair-2\",\"wifi\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"wifiStatus\":" + String(static_cast<int>(WiFi.status()));
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += ",\"web\":" + String(webServer ? "true" : "false") + "}";
  json.remove(json.length() - 1);
  size_t clientCount = 10;
  int clients[10];
  if (!webServer || httpd_get_client_list(webServer, &clientCount, clients) != ESP_OK) clientCount = 0;
  json += ",\"stream\":true,\"uptimeMs\":" + String(millis());
  json += ",\"resetReason\":" + String(static_cast<int>(esp_reset_reason()));
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap());
  json += ",\"httpClients\":" + String(clientCount);
  json += ",\"activeStreams\":" + String(streamSlots ? 2 - uxSemaphoreGetCount(streamSlots) : 0);
  json += ",\"lightWrites\":" + String(lightWrites) + ",\"lastLightWriteMs\":" + String(lastLightWriteMs);
  json += ",\"lights\":[" + String(gpio_get_level(LIGHT_PINS[0])) + "," + String(gpio_get_level(LIGHT_PINS[1])) + "]}";
  return json;
}

esp_err_t serveAsset(httpd_req_t *req) {
  const WebAsset *asset = static_cast<const WebAsset *>(req->user_ctx);
  httpd_resp_set_type(req, asset->type);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob:; connect-src 'self' http:; object-src 'none'; base-uri 'self'; frame-ancestors 'none'");
  return sendAndClose(req, reinterpret_cast<const char *>(asset->data), asset->size);
}

esp_err_t serveStatus(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "X-Camera-Transport", "wifi");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  String json = statusJson();
  return sendAndClose(req, json.c_str(), json.length());
}

esp_err_t serveCapture(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Camera-Transport", "wifi");
  httpd_resp_set_hdr(req, "Cross-Origin-Resource-Policy", "same-origin");
  if (!cameraReady || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Camera unavailable\"}";
    return sendAndClose(req, body, strlen(body));
  }
  camera_fb_t *frame = esp_camera_fb_get();
  size_t length = frame ? frame->len : 0;
  uint8_t *copy = length ? static_cast<uint8_t *>(heap_caps_malloc(length, psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT)) : nullptr;
  if (copy) memcpy(copy, frame->buf, length);
  if (frame) esp_camera_fb_return(frame);
  xSemaphoreGive(cameraMutex);
  esp_err_t result;
  if (copy) {
    httpd_resp_set_type(req, "image/jpeg");
    result = sendAndClose(req, reinterpret_cast<const char *>(copy), length);
    free(copy);
  } else {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Capture failed\"}";
    result = sendAndClose(req, body, strlen(body));
  }
  return result;
}

void streamFrames(void *argument) {
  httpd_req_t *req = static_cast<httpd_req_t *>(argument);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Camera-Transport", "wifi");
  httpd_resp_set_hdr(req, "Cross-Origin-Resource-Policy", "same-origin");
  uint8_t *copy = nullptr;
  size_t capacity = 0;
  bool disconnected = false;
  while (WiFi.status() == WL_CONNECTED) {
    // Detect the browser closing its read side before a buffered send times out.
    char pending;
    const int peer = recv(httpd_req_to_sockfd(req), &pending, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peer == 0 || (peer < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      disconnected = true; break;
    }
    const uint32_t started = millis();
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) break;
    camera_fb_t *frame = esp_camera_fb_get();
    size_t length = 0;
    if (frame) {
      if (frame->len > capacity) {
        free(copy);
        capacity = frame->len;
        copy = static_cast<uint8_t *>(heap_caps_malloc(capacity, psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT));
      }
      if (copy) { length = frame->len; memcpy(copy, frame->buf, length); }
      esp_camera_fb_return(frame);
    }
    xSemaphoreGive(cameraMutex);
    if (!length) break;
    // One length-prefixed JPEG at a time; never queue old frames in application memory.
    const uint8_t header[] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(header), sizeof(header)) != ESP_OK ||
        httpd_resp_send_chunk(req, reinterpret_cast<const char *>(copy), length) != ESP_OK) { disconnected = true; break; }
    const uint32_t elapsed = millis() - started;
    vTaskDelay(pdMS_TO_TICKS(elapsed < 40 ? 40 - elapsed : 1));
  }
  free(copy);
  if (!disconnected) httpd_resp_send_chunk(req, nullptr, 0);
  httpd_req_async_handler_complete(req);
  xSemaphoreGive(streamSlots);
  vTaskDelete(nullptr);
}

esp_err_t serveStream(httpd_req_t *req) {
  if (!cameraReady || xSemaphoreTake(streamSlots, 0) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Camera busy; close another preview\"}";
    return sendAndClose(req, body, strlen(body));
  }
  httpd_req_t *asyncRequest = nullptr;
  if (httpd_req_async_handler_begin(req, &asyncRequest) != ESP_OK) {
    xSemaphoreGive(streamSlots);
    return ESP_FAIL;
  }
  if (xTaskCreate(streamFrames, "camera-stream", 6144, asyncRequest, 3, nullptr) != pdPASS) {
    httpd_resp_set_status(asyncRequest, "503 Service Unavailable");
    httpd_resp_sendstr(asyncRequest, "Camera memory unavailable");
    httpd_req_async_handler_complete(asyncRequest);
    xSemaphoreGive(streamSlots);
  }
  return ESP_OK;
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 32;
  config.max_open_sockets = 9;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 3;
  config.send_wait_timeout = 3;
  // Avoid delaying small HTTP headers while the JPEG body is ready to send.
  config.open_fn = [](httpd_handle_t, int socket) -> esp_err_t {
    int enabled = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    return ESP_OK;
  };
  if (httpd_start(&webServer, &config) != ESP_OK) { webServer = nullptr; return; }
  for (const auto &asset : webAssets) {
    httpd_uri_t route = {};
    route.uri = asset.uri; route.method = HTTP_GET; route.handler = serveAsset;
    route.user_ctx = const_cast<WebAsset *>(&asset);
    httpd_register_uri_handler(webServer, &route);
  }
  httpd_uri_t status = {};
  status.uri = "/api/camera/status"; status.method = HTTP_GET; status.handler = serveStatus;
  httpd_register_uri_handler(webServer, &status);
  httpd_uri_t capture = {};
  capture.uri = "/api/camera/capture"; capture.method = HTTP_GET; capture.handler = serveCapture;
  httpd_register_uri_handler(webServer, &capture);
  httpd_uri_t stream = {};
  stream.uri = "/api/camera/stream"; stream.method = HTTP_GET; stream.handler = serveStream;
  httpd_register_uri_handler(webServer, &stream);
  httpd_uri_t feedStatus = {};
  feedStatus.uri = "/api/feed/status"; feedStatus.method = HTTP_GET; feedStatus.handler = serveFeedStatus;
  httpd_register_uri_handler(webServer, &feedStatus);
  httpd_uri_t feedStart = {};
  feedStart.uri = "/api/feed"; feedStart.method = HTTP_POST; feedStart.handler = serveFeedStart;
  httpd_register_uri_handler(webServer, &feedStart);
  httpd_uri_t feedStop = {};
  feedStop.uri = "/api/feed/stop"; feedStop.method = HTTP_POST; feedStop.handler = serveFeedStop;
  httpd_register_uri_handler(webServer, &feedStop);
  httpd_uri_t lightsStatus = {};
  lightsStatus.uri = "/api/lights"; lightsStatus.method = HTTP_GET; lightsStatus.handler = serveLightsStatus;
  httpd_register_uri_handler(webServer, &lightsStatus);
  const char *lightUris[] = {"/api/lights/1", "/api/lights/2"};
  for (uint8_t i = 0; i < 2; ++i) {
    httpd_uri_t light = {};
    light.uri = lightUris[i]; light.method = HTTP_PUT; light.handler = serveLightSet;
    light.user_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(i));
    httpd_register_uri_handler(webServer, &light);
  }
  if (MDNS.begin("cat-home")) MDNS.addService("http", "tcp", 80);
  registerWifiRoutes(webServer);
  registerDiscoveryRoutes(webServer);
  httpd_uri_t environment = {};
  environment.uri = "/api/environment"; environment.method = HTTP_GET; environment.handler = serveEnvironment;
  httpd_register_uri_handler(webServer, &environment);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeHex(const char *text, String &value) {
  value = "";
  if (strcmp(text, "-") == 0) return true;
  const size_t length = strlen(text);
  if (length % 2) return false;
  for (size_t i = 0; i < length; i += 2) {
    int a = hexDigit(text[i]), b = hexDigit(text[i + 1]);
    if (a < 0 || b < 0 || (a == 0 && b == 0)) return false;
    value += static_cast<char>((a << 4) | b);
  }
  return true;
}

void configureWifi(char *args) {
  char *context = nullptr;
  char *ssidHex = strtok_r(args, " ", &context);
  char *passwordHex = strtok_r(nullptr, " ", &context);
  String ssid, password;
  if (!ssidHex || !passwordHex || strtok_r(nullptr, " ", &context) ||
      !decodeHex(ssidHex, ssid) || !decodeHex(passwordHex, password) ||
      ssid.length() == 0 || ssid.length() > 32 || password.length() > 63 ||
      (password.length() > 0 && password.length() < 8)) {
    Serial.println("CATERROR invalid_wifi_config"); return;
  }
  Serial.println(submitSerialWifi(ssid,password) ? "CATSTATUS {\"accepted\":true}" : "CATERROR wifi_busy");
}

void stopFeeders() {
  // Camera uses LEDC timer 0; servo channels 2/3/4 use timers 1/2.
  const uint8_t pins[] = {1, 2, 21};
  const uint32_t duty = (1500UL * 16384UL + 10000UL) / 20000UL;
  neutralReady = true;
  for (uint8_t i = 0; i < 3; ++i) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
    if (!ledcAttachChannel(pins[i], 50, 14, i + 2) || !ledcWrite(pins[i], duty))
      neutralReady = false;
  }
}

void initializeCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 11;
  config.pin_d1 = 9;
  config.pin_d2 = 8;
  config.pin_d3 = 10;
  config.pin_d4 = 12;
  config.pin_d5 = 18;
  config.pin_d6 = 17;
  config.pin_d7 = 16;
  config.pin_xclk = 15;
  config.pin_pclk = 13;
  config.pin_vsync = 6;
  config.pin_href = 7;
  config.pin_sccb_sda = 4;
  config.pin_sccb_scl = 5;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 16; // Mobile fluent preset: 320x240 with moderate JPEG compression.
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  cameraError = esp_camera_init(&config);
  cameraReady = cameraError == ESP_OK;
}

void printStatus() {
  Serial.print("CATSTATUS "); Serial.println(statusJson());
}

void captureFrame() {
  if (!cameraReady) { Serial.printf("CATERROR camera_init_%d\n", static_cast<int>(cameraError)); return; }
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) { Serial.println("CATERROR camera_busy"); return; }
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) { xSemaphoreGive(cameraMutex); Serial.println("CATERROR capture_failed"); return; }
  Serial.printf("CATFRAME %u\n", static_cast<unsigned>(frame->len));
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < frame->len && millis() - started < 5000) {
    size_t n = Serial.write(frame->buf + sent, min(static_cast<size_t>(4096), frame->len - sent));
    sent += n;
    if (!n) delay(1);
  }
  esp_camera_fb_return(frame);
  xSemaphoreGive(cameraMutex);
  Serial.println();
}

void setup() {
  initializeLights();
  stopFeeders();
  initializeFeeding(neutralReady);
  Serial.begin(921600);
  Serial.setDebugOutput(false);
  delay(500);
  cameraMutex = xSemaphoreCreateMutex();
  streamSlots = xSemaphoreCreateCounting(2, 2);
  if (!cameraMutex || !streamSlots) return;
  initializeCamera();
  initializeEnvironment();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("cat-home");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  preferences.begin("cat-wifi", false);
  initializeWifiSettings();
  initializeDiscovery();
}

void loop() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      command[commandLength] = 0;
      if (!overflow && commandLength) {
        if (strcmp(command, "status") == 0) printStatus();
        else if (strcmp(command, "lights") == 0) Serial.println(lightStateJson());
        else if (strcmp(command, "environment") == 0) Serial.println(environmentJson());
        else if (strcmp(command, "capture") == 0) captureFrame();
        else if (strncmp(command, "wifi ", 5) == 0) configureWifi(command + 5);
        else Serial.println("CATERROR unknown_command");
      }
      overflow = false;
      commandLength = 0;
      memset(command, 0, sizeof(command));
    } else if (!overflow) {
      if (c < 32 || c > 126 || commandLength >= sizeof(command) - 1) overflow = true;
      else command[commandLength++] = c;
    }
  }
  serviceWifiSettings();
  serviceDiscovery();
  if (WiFi.status() == WL_CONNECTED) {
    if (!webServer) startWebServer();
  }
  delay(1);
}
