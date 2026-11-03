#pragma once
#include <Arduino.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "http_reply.h"

constexpr uint32_t MAX_FEED_MS = 30000;
constexpr uint8_t FEED_PINS[] = {1, 2, 21};
constexpr uint16_t FEED_STOP_US = 1500;
constexpr uint16_t FEED_RUN_US = 2000;
SemaphoreHandle_t feedMutex = nullptr;
char feedBootId[24];
uint8_t feedChannel = 0;
uint32_t feedStarted = 0, feedDuration = 0;
uint16_t feedPulses[] = {1500, 1500, 1500};
bool feedReady = false;
String feedRequestId;
const char *feedResult = "idle";
struct FeedRequest { String id; uint8_t channel; uint32_t duration; bool cancelled; };
FeedRequest feedHistory[32];
uint8_t historyIndex = 0;

bool feedWrite(uint8_t index, uint16_t pulse) {
  bool ok = ledcWrite(FEED_PINS[index], (static_cast<uint32_t>(pulse) * 16384UL + 10000UL) / 20000UL);
  if (ok) feedPulses[index] = pulse;
  else {
    ledcDetach(FEED_PINS[index]); pinMode(FEED_PINS[index], OUTPUT); digitalWrite(FEED_PINS[index], LOW);
    feedPulses[index] = 0; feedReady = false;
  }
  return ok;
}
void finishFeed(const char *reason) { // Caller holds feedMutex.
  for (uint8_t i = 0; i < 3; ++i) feedWrite(i, FEED_STOP_US);
  feedChannel = 0;
  feedResult = feedReady ? reason : "fault";
}
void feedingTask(void *) {
  for (;;) {
    xSemaphoreTake(feedMutex, portMAX_DELAY);
    if (feedChannel && static_cast<uint32_t>(millis() - feedStarted) >= feedDuration) finishFeed("completed");
    xSemaphoreGive(feedMutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void initializeFeeding(bool pwmReady) {
  snprintf(feedBootId, sizeof(feedBootId), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  feedMutex = xSemaphoreCreateMutex();
  feedReady = pwmReady && feedMutex;
  if (feedMutex && xTaskCreate(feedingTask, "feed-deadline", 3072, nullptr, 5, nullptr) != pdPASS) feedReady = false;
}
bool validFeedId(const char *id) {
  if (!id || strlen(id) < 16 || strlen(id) > 64) return false;
  for (const char *p = id; *p; ++p) if (!isalnum(static_cast<unsigned char>(*p)) && *p != '-') return false;
  return true;
}
String feedStateJson(bool duplicate = false) { // Caller holds feedMutex.
  const uint32_t elapsed = millis() - feedStarted;
  const uint32_t remaining = feedChannel && elapsed < feedDuration ? feedDuration - elapsed : 0;
  String json = "{\"supported\":true,\"bootId\":\"" + String(feedBootId) + "\",\"ready\":" + (feedReady ? "true" : "false");
  json += ",\"active\":" + String(feedChannel ? "true" : "false") + ",\"channel\":" + String(feedChannel);
  json += ",\"durationMs\":" + String(feedDuration) + ",\"remainingMs\":" + String(remaining);
  json += ",\"requestId\":\"" + feedRequestId + "\",\"result\":\"" + feedResult + "\"";
  json += ",\"maxDurationMs\":30000,\"pins\":[1,2,21],\"pulses\":[" + String(feedPulses[0]) + "," + String(feedPulses[1]) + "," + String(feedPulses[2]) + "]";
  json += ",\"duplicate\":" + String(duplicate ? "true" : "false") + "}";
  return json;
}
esp_err_t feedReply(httpd_req_t *req, const char *status, const String &body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return sendAndClose(req, body.c_str(), body.length());
}
bool allowedFeedRequest(httpd_req_t *req) {
  char marker[8], origin[100], host[80], type[64];
  if (httpd_req_get_hdr_value_str(req, "X-Cat-Home", marker, sizeof(marker)) != ESP_OK || strcmp(marker, "1") != 0) return false;
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK || strncmp(type, "application/json", 16) != 0) return false;
  if (httpd_req_get_hdr_value_len(req, "Origin")) {
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (String(origin) != "http://" + String(host)) return false;
  }
  return true;
}
cJSON *readFeedBody(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 384) return nullptr;
  char body[385]; size_t received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) return nullptr;
    received += n;
  }
  body[received] = 0;
  return cJSON_ParseWithOpts(body, nullptr, true);
}
esp_err_t serveFeedStatus(httpd_req_t *req) {
  if (!feedMutex) return feedReply(req, "503 Service Unavailable", "{\"error\":\"motor_unavailable\"}");
  xSemaphoreTake(feedMutex, portMAX_DELAY); String json = feedStateJson(); xSemaphoreGive(feedMutex);
  return feedReply(req, "200 OK", json);
}
esp_err_t serveFeedStart(httpd_req_t *req) {
  if (!allowedFeedRequest(req)) return feedReply(req, "403 Forbidden", "{\"error\":\"invalid_origin\"}");
  cJSON *body = readFeedBody(req);
  const cJSON *channelValue = cJSON_GetObjectItemCaseSensitive(body, "channel");
  const cJSON *durationValue = cJSON_GetObjectItemCaseSensitive(body, "durationMs");
  const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "requestId"));
  const char *boot = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "bootId"));
  if (!cJSON_IsNumber(channelValue) || !cJSON_IsNumber(durationValue) || !validFeedId(id) || !boot ||
      channelValue->valuedouble != channelValue->valueint || channelValue->valueint < 1 || channelValue->valueint > 3 ||
      durationValue->valuedouble != durationValue->valueint || durationValue->valueint < 1000 || static_cast<uint32_t>(durationValue->valueint) > MAX_FEED_MS) {
    cJSON_Delete(body); return feedReply(req, "400 Bad Request", "{\"error\":\"invalid_parameters\"}");
  }
  if (!feedMutex) { cJSON_Delete(body); return feedReply(req, "503 Service Unavailable", "{\"error\":\"motor_unavailable\"}"); }
  xSemaphoreTake(feedMutex, portMAX_DELAY);
  const char *status = "200 OK"; String reply;
  if (strcmp(boot, feedBootId) != 0) { status = "409 Conflict"; reply = "{\"error\":\"device_restarted\"}"; }
  else {
    FeedRequest *previous = nullptr;
    for (auto &entry : feedHistory) if (entry.id == id) { previous = &entry; break; }
    if (previous) {
      if (!previous->cancelled && (previous->channel != channelValue->valueint || previous->duration != static_cast<uint32_t>(durationValue->valueint))) {
        status = "409 Conflict"; reply = "{\"error\":\"request_id_conflict\"}";
      } else reply = feedStateJson(true);
    } else if (!feedReady) { status = "503 Service Unavailable"; reply = "{\"error\":\"motor_unavailable\"}"; }
    else if (feedChannel) { status = "409 Conflict"; reply = "{\"error\":\"motor_busy\"}"; }
    else {
      feedRequestId = id; feedDuration = durationValue->valueint; feedStarted = millis();
      feedHistory[historyIndex] = {feedRequestId, static_cast<uint8_t>(channelValue->valueint), feedDuration, false};
      historyIndex = (historyIndex + 1) % 32;
      if (feedWrite(channelValue->valueint - 1, FEED_RUN_US)) {
        feedChannel = channelValue->valueint; feedResult = "running"; reply = feedStateJson();
      } else { finishFeed("fault"); status = "503 Service Unavailable"; reply = "{\"error\":\"motor_unavailable\"}"; }
    }
  }
  xSemaphoreGive(feedMutex); cJSON_Delete(body);
  return feedReply(req, status, reply);
}
esp_err_t serveFeedStop(httpd_req_t *req) {
  if (!allowedFeedRequest(req)) return feedReply(req, "403 Forbidden", "{\"error\":\"invalid_origin\"}");
  cJSON *body = readFeedBody(req);
  if (!cJSON_IsObject(body)) { cJSON_Delete(body); return feedReply(req, "400 Bad Request", "{\"error\":\"invalid_parameters\"}"); }
  const cJSON *cancel = cJSON_GetObjectItemCaseSensitive(body, "cancelRequestId");
  const char *id = cJSON_GetStringValue(cancel);
  if (cancel && !validFeedId(id)) { cJSON_Delete(body); return feedReply(req, "400 Bad Request", "{\"error\":\"invalid_parameters\"}"); }
  if (!feedMutex) { cJSON_Delete(body); return feedReply(req, "503 Service Unavailable", "{\"error\":\"motor_unavailable\"}"); }
  xSemaphoreTake(feedMutex, portMAX_DELAY);
  if (id) {
    FeedRequest *previous = nullptr;
    for (auto &entry : feedHistory) if (entry.id == id) { previous = &entry; break; }
    if (previous) previous->cancelled = true;
    else { feedHistory[historyIndex] = {String(id), 0, 0, true}; historyIndex = (historyIndex + 1) % 32; }
  }
  finishFeed("stopped"); String json = feedStateJson();
  xSemaphoreGive(feedMutex); cJSON_Delete(body);
  return feedReply(req, "200 OK", json);
}
