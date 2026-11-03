#pragma once
#include "esp_http_server.h"

// Finite responses must release their socket: idle browser keep-alive pools
// otherwise fill the small ESP32 connection table while preview streams run.
inline esp_err_t sendAndClose(httpd_req_t *req, const char *body, size_t length) {
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_send(req, body, length);
  // ESP-IDF closes this session when a synchronous handler returns non-OK.
  // The response has already been sent, including its Content-Length.
  return ESP_FAIL;
}
