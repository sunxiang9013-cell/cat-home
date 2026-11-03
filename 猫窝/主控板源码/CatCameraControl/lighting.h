#pragma once
#include <Arduino.h>
#include "driver/gpio.h"

// D4184 low-side switching modules: HIGH enables the load, LOW disables it.
constexpr gpio_num_t LIGHT_PINS[] = {GPIO_NUM_41, GPIO_NUM_42};
bool lightsReady = false;
uint32_t lightWrites = 0, lastLightWriteMs = 0;

void initializeLights() {
  lightsReady = true;
  for (gpio_num_t pin : LIGHT_PINS) {
    if (gpio_reset_pin(pin) != ESP_OK || gpio_set_level(pin, 0) != ESP_OK ||
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY) != ESP_OK) lightsReady = false;
    gpio_set_level(pin, 0);
  }
}

String lightStateJson() {
  String json = "{\"supported\":true,\"ready\":" + String(lightsReady ? "true" : "false");
  json += ",\"bootId\":\"" + String(feedBootId) + "\",\"lights\":[";
  for (uint8_t i = 0; i < 2; ++i) {
    if (i) json += ",";
    const int level = gpio_get_level(LIGHT_PINS[i]);
    json += "{\"channel\":" + String(i + 1) + ",\"pin\":" + String(static_cast<int>(LIGHT_PINS[i]));
    json += ",\"on\":" + String(level ? "true" : "false") + ",\"level\":" + String(level) + "}";
  }
  return json + "]}";
}

esp_err_t serveLightsStatus(httpd_req_t *req) {
  return feedReply(req, "200 OK", lightStateJson());
}

esp_err_t serveLightSet(httpd_req_t *req) {
  if (!allowedFeedRequest(req)) return feedReply(req, "403 Forbidden", "{\"error\":\"invalid_origin\"}");
  cJSON *body = readFeedBody(req);
  const cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
  const char *boot = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "bootId"));
  if (!cJSON_IsBool(on) || !boot) {
    cJSON_Delete(body); return feedReply(req, "400 Bad Request", "{\"error\":\"invalid_parameters\"}");
  }
  if (strcmp(boot, feedBootId) != 0) {
    cJSON_Delete(body); return feedReply(req, "409 Conflict", "{\"error\":\"device_restarted\"}");
  }
  if (!lightsReady) {
    cJSON_Delete(body); return feedReply(req, "503 Service Unavailable", "{\"error\":\"lights_unavailable\"}");
  }
  const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(req->user_ctx));
  const uint32_t level = cJSON_IsTrue(on) ? 1 : 0;
  cJSON_Delete(body);
  if (index > 1 || gpio_set_level(LIGHT_PINS[index], level) != ESP_OK || gpio_get_level(LIGHT_PINS[index]) != static_cast<int>(level)) {
    return feedReply(req, "503 Service Unavailable", "{\"error\":\"gpio_write_failed\"}");
  }
  lightWrites++;
  lastLightWriteMs = millis();
  return feedReply(req, "200 OK", lightStateJson());
}
