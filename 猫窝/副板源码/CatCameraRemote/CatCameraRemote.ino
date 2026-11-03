/* Classic ESP32-CAM: independent second camera. Pin map from supplied specification p3. */
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <atomic>
std::atomic<bool> capturePending{false};
std::atomic<uint32_t> frameCount{0},frameFailures{0},lastFrameMs{0},sendFailures{0};
std::atomic<bool> cameraReady{false};
std::atomic<esp_err_t> cameraError{ESP_FAIL};
SemaphoreHandle_t cameraMutex=nullptr,streamSlots=nullptr;
httpd_handle_t webServer=nullptr;
Preferences preferences;
char command[256];size_t commandLength=0;bool overflow=false;
#define CAT_REMOTE_CAMERA
#include "wifi_settings.h"
#include "device_discovery.h"
String networkDiagnostics() {
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);
  String state=wifiStateJson();
  xSemaphoreGive(wifiSettingsMutex);
  state.remove(state.length()-1);
  state+=",\"bssid\":\""+WiFi.BSSIDstr()+"\",\"gateway\":\""+WiFi.gatewayIP().toString();
  state+="\",\"subnet\":\""+WiFi.subnetMask().toString()+"\",\"channel\":"+String(WiFi.channel());
  state+=",\"web\":"+String(webServer?"true":"false")+"}";
  return state;
}
void cameraHeaders(httpd_req_t *req) {
  httpd_resp_set_hdr(req,"Cache-Control","no-store");
  httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
  httpd_resp_set_hdr(req,"Access-Control-Expose-Headers","X-Camera-Transport");
  httpd_resp_set_hdr(req,"Cross-Origin-Resource-Policy","cross-origin");
}
esp_err_t sendCameraBody(httpd_req_t *req,const char *body,size_t length) {
  const esp_err_t result=httpd_resp_send(req,body,length);
  if(result!=ESP_OK)sendFailures.fetch_add(1);
  return result;
}
String statusJson() {
  String json="{\"firmware\":\"cat-camera-remote-pair-2\",\"ready\":"+String(cameraReady?"true":"false");
  json+=",\"error\":"+String(static_cast<int>(cameraError));
  json+=",\"stream\":true,\"wifi\":"+String(WiFi.status()==WL_CONNECTED?"true":"false");
  json+=",\"ip\":\""+WiFi.localIP().toString()+"\",\"rssi\":"+String(WiFi.RSSI());
  json+=",\"psram\":"+String(ESP.getPsramSize())+",\"freeHeap\":"+String(ESP.getFreeHeap());
  json+=",\"activeStreams\":"+String(streamSlots?1-uxSemaphoreGetCount(streamSlots):0);
  size_t clients=10;int sockets[10];
  if(!webServer||httpd_get_client_list(webServer,&clients,sockets)!=ESP_OK)clients=0;
  json+=",\"httpClients\":"+String(clients)+",\"capturePending\":"+String(capturePending.load()?"true":"false");
  json+=",\"frames\":"+String(frameCount.load())+",\"frameFailures\":"+String(frameFailures.load());
  json+=",\"lastFrameAgeMs\":"+String(lastFrameMs.load()?millis()-lastFrameMs.load():0);
  json+=",\"sendFailures\":"+String(sendFailures.load());
  sensor_t *sensor=cameraReady.load()?esp_camera_sensor_get():nullptr;
  json+=",\"sensorId\":"+String(sensor?sensor->id.PID:0);
  return json+",\"uptimeMs\":"+String(millis())+"}";
}
esp_err_t serveStatus(httpd_req_t *req) {
  cameraHeaders(req);httpd_resp_set_type(req,"application/json");
  httpd_resp_set_hdr(req,"X-Camera-Transport","wifi");
  String json=statusJson();return sendCameraBody(req,json.c_str(),json.length());
}
esp_err_t captureReply(httpd_req_t *req) {
  cameraHeaders(req);
  httpd_resp_set_hdr(req, "X-Camera-Transport", "wifi");

  if (!cameraReady || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Camera unavailable\"}";
    return sendCameraBody(req, body, strlen(body));
  }
  camera_fb_t *frame = esp_camera_fb_get();
  size_t length = frame ? frame->len : 0;
  if(length){frameCount.fetch_add(1);lastFrameMs.store(millis());}else frameFailures.fetch_add(1);
  uint8_t *copy = length ? static_cast<uint8_t *>(heap_caps_malloc(length, psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT)) : nullptr;
  if (copy) memcpy(copy, frame->buf, length);
  if (frame) esp_camera_fb_return(frame);
  xSemaphoreGive(cameraMutex);
  esp_err_t result;
  if (copy) {
    httpd_resp_set_type(req, "image/jpeg");
    result = sendCameraBody(req, reinterpret_cast<const char *>(copy), length);
    free(copy);
  } else {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Capture failed\"}";
    result = sendCameraBody(req, body, strlen(body));
  }
  return result;
}

void finishCameraRequest(httpd_req_t *req,bool failed) {
  const int socket=httpd_req_to_sockfd(req);
  httpd_req_async_handler_complete(req);
  // Successful finite responses remain reusable; the server owns idle/LRU cleanup.
  // Do not explicitly tear down a socket immediately after queueing its JPEG body.
  if(failed)httpd_sess_trigger_close(webServer,socket);
}
void captureWorker(void *argument) {
  httpd_req_t *req=static_cast<httpd_req_t *>(argument);
  const esp_err_t result=captureReply(req);
  capturePending.store(false);finishCameraRequest(req,result!=ESP_OK);vTaskDelete(nullptr);
}
esp_err_t serveCapture(httpd_req_t *req) {
  if(!cameraReady||capturePending.exchange(true)) {
    cameraHeaders(req);httpd_resp_set_status(req,"503 Service Unavailable");
    return sendCameraBody(req,"Camera unavailable",18);
  }
  httpd_req_t *asyncRequest=nullptr;
  if(httpd_req_async_handler_begin(req,&asyncRequest)!=ESP_OK){capturePending.store(false);return ESP_FAIL;}
  if(xTaskCreate(captureWorker,"camera-capture",6144,asyncRequest,2,nullptr)!=pdPASS){
    cameraHeaders(asyncRequest);httpd_resp_set_status(asyncRequest,"503 Service Unavailable");
    const esp_err_t result=httpd_resp_sendstr(asyncRequest,"Camera memory unavailable");
    capturePending.store(false);finishCameraRequest(asyncRequest,result!=ESP_OK);
  }
  return ESP_OK;
}

void streamFrames(void *argument) {
  httpd_req_t *req = static_cast<httpd_req_t *>(argument);
  httpd_resp_set_type(req, "application/octet-stream");
  cameraHeaders(req);
  httpd_resp_set_hdr(req, "X-Camera-Transport", "wifi");

  uint8_t *copy = nullptr;
  size_t capacity = 0;
  bool disconnected = false;
  while (WiFi.status() == WL_CONNECTED) {
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
    if (!length){frameFailures.fetch_add(1);break;}
    frameCount.fetch_add(1);lastFrameMs.store(millis());
    // One length-prefixed JPEG at a time; never queue old frames in application memory.
    const uint8_t header[] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(header), sizeof(header)) != ESP_OK ||
        httpd_resp_send_chunk(req, reinterpret_cast<const char *>(copy), length) != ESP_OK) { sendFailures.fetch_add(1); disconnected = true; break; }
    const uint32_t elapsed = millis() - started;
    vTaskDelay(pdMS_TO_TICKS(elapsed < 40 ? 40 - elapsed : 1));
  }
  free(copy);
  if (!disconnected && httpd_resp_send_chunk(req, nullptr, 0)!=ESP_OK)disconnected=true;
  finishCameraRequest(req,disconnected);
  xSemaphoreGive(streamSlots);
  vTaskDelete(nullptr);
}

esp_err_t serveStream(httpd_req_t *req) {
  if (!cameraReady || xSemaphoreTake(streamSlots, 0) != pdTRUE) {
    cameraHeaders(req);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"Camera busy; close another preview\"}";
    return sendCameraBody(req, body, strlen(body));
  }
  httpd_req_t *asyncRequest = nullptr;
  if (httpd_req_async_handler_begin(req, &asyncRequest) != ESP_OK) {
    xSemaphoreGive(streamSlots);
    return ESP_FAIL;
  }
  if (xTaskCreate(streamFrames, "camera-stream", 6144, asyncRequest, 3, nullptr) != pdPASS) {
    cameraHeaders(asyncRequest);
    httpd_resp_set_status(asyncRequest, "503 Service Unavailable");
    const esp_err_t result=httpd_resp_sendstr(asyncRequest, "Camera memory unavailable");
    finishCameraRequest(asyncRequest,result!=ESP_OK);
    xSemaphoreGive(streamSlots);
  }
  return ESP_OK;
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

void initializeCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
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

void startWebServer() {
  httpd_config_t config=HTTPD_DEFAULT_CONFIG();
  config.stack_size=12288;config.max_uri_handlers=12;config.max_open_sockets=7;
  config.lru_purge_enable=true;config.recv_wait_timeout=3;config.send_wait_timeout=3;
  config.open_fn=[](httpd_handle_t,int socket)->esp_err_t {int enabled=1;setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled));return ESP_OK;};
  if(httpd_start(&webServer,&config)!=ESP_OK){webServer=nullptr;return;}
  const char *uris[]={"/","/api/camera/status","/api/camera/capture","/api/camera/stream"};
  esp_err_t (*handlers[])(httpd_req_t*)={serveStatus,serveStatus,serveCapture,serveStream};
  for(uint8_t i=0;i<4;i++){httpd_uri_t route={};route.uri=uris[i];route.method=HTTP_GET;route.handler=handlers[i];httpd_register_uri_handler(webServer,&route);}
  if(MDNS.begin("cat-camera-2"))MDNS.addService("http","tcp",80);
  registerWifiRoutes(webServer);
  registerDiscoveryRoutes(webServer);
}
void setup() {
  pinMode(4,OUTPUT);digitalWrite(4,LOW);
  Serial.begin(115200);Serial.setDebugOutput(false);delay(500);
  cameraMutex=xSemaphoreCreateMutex();streamSlots=xSemaphoreCreateCounting(1,1);
  if(!cameraMutex||!streamSlots)return;
  initializeCamera();
  WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setHostname("cat-camera-2");WiFi.setSleep(false);WiFi.setAutoReconnect(true);
  preferences.begin("cat-wifi",false);
  initializeWifiSettings();
  initializeDiscovery();
  Serial.print("CATSTATUS ");Serial.println(statusJson());
}
void loop() {
  while(Serial.available()) {
    char c=static_cast<char>(Serial.read());
    if(c=='\n'||c=='\r') {
      command[commandLength]=0;
      if(!overflow&&commandLength){
        if(strcmp(command,"status")==0){Serial.print("CATSTATUS ");Serial.println(statusJson());}
        else if(strcmp(command,"network")==0){Serial.print("CATNETWORK ");Serial.println(networkDiagnostics());}
        else if(strncmp(command,"wifi ",5)==0)configureWifi(command+5);
        else Serial.println("CATERROR unknown_command");
      }
      overflow=false;commandLength=0;memset(command,0,sizeof(command));
    }else if(!overflow){if(c<32||c>126||commandLength>=sizeof(command)-1)overflow=true;else command[commandLength++]=c;}
  }
  serviceWifiSettings();
  serviceDiscovery();
  if(WiFi.status()==WL_CONNECTED){
    if(!webServer){startWebServer();Serial.print("CATSTATUS ");Serial.println(statusJson());}
  }
  delay(1);
}
