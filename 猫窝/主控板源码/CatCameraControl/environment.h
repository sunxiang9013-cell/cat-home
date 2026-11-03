#pragma once
#include <Wire.h>
#include <math.h>

// Camera SCCB owns the port selected by the installed ESP32 camera library.
#if defined(CONFIG_SCCB_HARDWARE_I2C_PORT1)
TwoWire environmentWire(0);
#else
TwoWire environmentWire(1);
#endif
constexpr uint8_t AHT_ADDRESS = 0x38;
SemaphoreHandle_t environmentMutex = nullptr;
struct EnvironmentSample {
  float temperature = 0, humidity = 0;
  uint32_t sampledAt = 0, sequence = 0;
  bool valid = false;
  const char *error = "starting";
} environmentSample;

void environmentError(const char *error) {
  if (!environmentMutex) return;
  xSemaphoreTake(environmentMutex, portMAX_DELAY);
  environmentSample.valid = false;
  environmentSample.error = error;
  xSemaphoreGive(environmentMutex);
}
bool ahtWrite(const uint8_t *data, size_t length) {
  environmentWire.beginTransmission(AHT_ADDRESS);
  environmentWire.write(data, length);
  return environmentWire.endTransmission() == 0;
}
bool ahtRead(uint8_t *data, size_t length) {
  if (environmentWire.requestFrom(AHT_ADDRESS, length) != length) return false;
  for (size_t i = 0; i < length; ++i) data[i] = environmentWire.read();
  return true;
}
void environmentTask(void *) {
  bool initialized = false;
  for (;;) {
    if (!initialized) {
      const uint8_t statusCommand = 0x71;
      uint8_t status;
      if (!ahtWrite(&statusCommand, 1) || !ahtRead(&status, 1)) {
        environmentError("not_detected"); vTaskDelay(pdMS_TO_TICKS(2000)); continue;
      }
      if (!(status & 0x08)) {
        const uint8_t initialize[] = {0xBE, 0x08, 0x00};
        if (!ahtWrite(initialize, sizeof(initialize))) {
          environmentError("init_failed"); vTaskDelay(pdMS_TO_TICKS(2000)); continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      initialized = true;
    }
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    uint8_t data[7];
    const char *error = nullptr;
    if (!ahtWrite(trigger, sizeof(trigger))) error = "not_detected";
    else {
      vTaskDelay(pdMS_TO_TICKS(85));
      const uint32_t started = millis();
      do {
        if (!ahtRead(data, sizeof(data))) { error = "read_failed"; break; }
        if (!(data[0] & 0x80)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
      } while (millis() - started < 200);
      if (!error && (data[0] & 0x80)) error = "busy_timeout";
      if (!error && !(data[0] & 0x08)) error = "not_calibrated";
      uint8_t crc = 0xFF;
      for (uint8_t i = 0; i < 6 && !error; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
      }
      if (!error && crc != data[6]) error = "crc_failed";
    }
    if (error) { environmentError(error); initialized = false; }
    else {
      const uint32_t rh = (uint32_t(data[1]) << 12) | (uint32_t(data[2]) << 4) | (data[3] >> 4);
      const uint32_t temp = (uint32_t(data[3] & 0x0F) << 16) | (uint32_t(data[4]) << 8) | data[5];
      xSemaphoreTake(environmentMutex, portMAX_DELAY);
      environmentSample.temperature = temp * (200.0f / 1048576.0f) - 50.0f;
      environmentSample.humidity = rh * (100.0f / 1048576.0f);
      environmentSample.sampledAt = millis();
      environmentSample.sequence++;
      environmentSample.valid = true;
      environmentSample.error = "";
      xSemaphoreGive(environmentMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
void initializeEnvironment() {
  environmentMutex = xSemaphoreCreateMutex();
  if (!environmentMutex) return;
  environmentWire.setTimeOut(100);
  if (!environmentWire.begin(14, 47, 100000)) { environmentError("bus_failed"); return; }
  if (xTaskCreate(environmentTask, "aht20", 4096, nullptr, 1, nullptr) != pdPASS) environmentError("task_failed");
}
String environmentJson() {
  EnvironmentSample sample;
  if (environmentMutex) {
    xSemaphoreTake(environmentMutex, portMAX_DELAY);
    sample = environmentSample;
    xSemaphoreGive(environmentMutex);
  } else sample.error = "unavailable";
  const uint32_t age = millis() - sample.sampledAt;
  const bool ready = sample.valid && sample.sequence && age <= 6000;
  String json = "{\"supported\":true,\"sensor\":\"AHT20\",\"sda\":14,\"scl\":47,\"address\":56,\"ready\":";
  json += ready ? "true" : "false";
  json += ",\"temperatureC\":" + (ready ? String(sample.temperature, 1) : String("null"));
  json += ",\"humidityPct\":" + (ready ? String(sample.humidity, 1) : String("null"));
  json += ",\"sampleAgeMs\":" + (sample.sequence ? String(age) : String("null"));
  json += ",\"sequence\":" + String(sample.sequence);
  json += ",\"error\":\"" + String(sample.valid && !ready ? "stale" : sample.error) + "\"}";
  return json;
}
esp_err_t serveEnvironment(httpd_req_t *req) {
  return feedReply(req, "200 OK", environmentJson());
}
