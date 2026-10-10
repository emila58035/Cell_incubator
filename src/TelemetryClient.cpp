#include "TelemetryClient.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "Config.h"

namespace {
const char *jsonBool(bool value) { return value ? "true" : "false"; }

// JSON has no NaN literal. Printing "nan" - what printf produces for a NaN -
// makes the whole body unparseable, so the server would reject the entire
// sample. It would reject exactly the sample that reports a sensor fault, which
// is the case the sensor_fault alert exists for. Send null instead and let the
// matching *_ok flag carry the meaning. SystemState holds NaN whenever
// invalidateClimate() runs.
void jsonFloat(char *out, size_t size, float value, int decimals) {
  if (isnan(value)) {
    snprintf(out, size, "null");
    return;
  }
  snprintf(out, size, "%.*f", decimals, (double)value);
}

bool jsonString(const char *json, const char *key, char *out, size_t size) {
  char marker[40];
  snprintf(marker, sizeof(marker), "\"%s\":\"", key);
  const char *start = strstr(json, marker);
  if (start == nullptr) return false;
  start += strlen(marker);
  const char *end = strchr(start, '"');
  if (end == nullptr || end == start || (size_t)(end - start) >= size) return false;
  memcpy(out, start, end - start);
  out[end - start] = '\0';
  return true;
}

bool jsonBoolValue(const char *json, const char *key, bool &value) {
  char marker[40];
  snprintf(marker, sizeof(marker), "\"%s\":", key);
  const char *start = strstr(json, marker);
  if (start == nullptr) return false;
  start += strlen(marker);
  if (strncmp(start, "true", 4) == 0) { value = true; return true; }
  if (strncmp(start, "false", 5) == 0) { value = false; return true; }
  return false;
}

bool jsonNumber(const char *json, const char *key, float &value) {
  char marker[40];
  snprintf(marker, sizeof(marker), "\"%s\":", key);
  const char *start = strstr(json, marker);
  if (start == nullptr) return false;
  start += strlen(marker);
  char *end = nullptr;
  value = strtof(start, &end);
  return end != start;
}
}  // namespace

void TelemetryClient::begin() {
  wifi_.begin();
  xTaskCreateStaticPinnedToCore(taskEntry, "netTask",
                                NetworkConfig::NetTaskStackBytes, this,
                                NetworkConfig::NetTaskPriority, taskStack_,
                                &taskTcb_, NetworkConfig::NetTaskCore);
}

void TelemetryClient::taskEntry(void *arg) {
  static_cast<TelemetryClient *>(arg)->taskLoop();
}

void TelemetryClient::websocketEvent(void *handler_args, esp_event_base_t,
                                     int32_t event_id, void *event_data) {
  auto *client = static_cast<TelemetryClient *>(handler_args);
  client->handleWebsocketEvent(event_id,
      static_cast<esp_websocket_event_data_t *>(event_data));
}

void TelemetryClient::taskLoop() {
  Serial.printf("[NET] netTask on core %d, stack %u B at %p (internal=%d)\n",
                (int)xPortGetCoreID(), (unsigned)NetworkConfig::NetTaskStackBytes,
                (void *)taskStack_, (int)esp_ptr_internal(taskStack_));

  uint32_t nextReportMs = 0;
  for (;;) {
    // Yielding every iteration is mandatory: the task watchdog only polls the
    // core 0 idle task and is configured to panic after 5 s. A task that spins
    // on core 0 would starve idle0 and reset the board.
    vTaskDelay(pdMS_TO_TICKS(NetworkConfig::NetTaskPollMs));

    const uint32_t now = millis();
    wifi_.step(now);
    linkUp_ = wifi_.linkUp();
    rssi_ = wifi_.rssi();

    if (!linkUp_) {
#if SENSOR_SIMULATION
      // Development aid: still build and print the payload, so the contract can
      // be checked with no access point and no credentials. Throttled to the
      // report interval on purpose - printing on every 20 ms iteration floods
      // the log and keeps the task inside the UART for ~37 ms per line, which
      // distorts exactly the timing this task is supposed to keep quiet.
      if ((int32_t)(now - nextReportMs) >= 0) {
        nextReportMs = now + NetworkConfig::ReportIntervalMs;
        printBodyOnly();
      }
#endif
      continue;
    }
    if (strlen(NetworkConfig::WebSocketUrl) == 0) {
      if ((int32_t)(now - nextReportMs) >= 0) {
        nextReportMs = now + NetworkConfig::ReportIntervalMs;
        reportOnce(now);
      }
      continue;
    }
    if (websocket_ == nullptr && strlen(NetworkConfig::WebSocketUrl) > 0) {
      snprintf(authorization_, sizeof(authorization_),
               "Authorization: Bearer %s\r\n", NetworkConfig::DeviceToken);
      esp_websocket_client_config_t config = {};
      config.uri = NetworkConfig::WebSocketUrl;
      config.cert_pem = NetworkConfig::ServerCaCertPem;
      config.headers = authorization_;
      config.user_context = this;
      config.task_stack = 8192;
      config.buffer_size = 2048;
      config.ping_interval_sec = 15;
      config.disable_auto_reconnect = false;
      websocket_ = esp_websocket_client_init(&config);
      if (websocket_ != nullptr) {
        esp_websocket_register_events(websocket_, WEBSOCKET_EVENT_ANY,
                                      websocketEvent, this);
        esp_websocket_client_start(websocket_);
      }
    }
    if (websocket_ == nullptr || !esp_websocket_client_is_connected(websocket_)) {
      continue;
    }
    if ((int32_t)(now - nextReportMs) >= 0) {
      nextReportMs = now + NetworkConfig::ReportIntervalMs;
      sendTelemetry(now);
    }
    if (now - lastHeartbeatMs_ >= 15000) {
      lastHeartbeatMs_ = now;
      sendHeartbeat(now);
    }
    sendPendingAck();
  }
}

void TelemetryClient::update(uint32_t now, SystemState &state) {
  applyPendingCommand(state);
  publish(now, state);
}

void TelemetryClient::publish(uint32_t now, SystemState &state) {
  portENTER_CRITICAL(&mux_);
  sample_.temperature = state.temperature;
  sample_.humidity = state.humidity;
  sample_.setpoint = (float)state.setpoint;
  sample_.co2 = state.co2;
  sample_.heaterDuty = state.heaterDuty;
  sample_.uptimeS = now / 1000u;
  sample_.lightBrightness = state.lightBrightness;

  uint8_t flags = 0;
  if (state.temperatureValid) {
    flags |= TelemetryTemperatureValid;
  }
  if (state.humidityValid) {
    flags |= TelemetryHumidityValid;
  }
  if (state.co2Available) {
    flags |= TelemetryCo2Available;
  }
  if (state.co2Valid) {
    flags |= TelemetryCo2Valid;
  }
  if (state.heatingEnabled) {
    flags |= TelemetryHeatingEnabled;
  }
  if (state.lightEnabled) {
    flags |= TelemetryLightEnabled;
  }
  if (state.fanEnabled) {
    flags |= TelemetryFanEnabled;
  }
  if (state.valveEnabled) {
    flags |= TelemetryValveEnabled;
  }
  sample_.flags = flags;
  portEXIT_CRITICAL(&mux_);
}

TelemetrySample TelemetryClient::takeSnapshot() {
  TelemetrySample copy;
  portENTER_CRITICAL(&mux_);
  copy = sample_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

TelemetryDiagnostics TelemetryClient::gatherDiagnostics() const {
  TelemetryDiagnostics diagnostics;
  diagnostics.heapFree = (uint32_t)ESP.getFreeHeap();
  diagnostics.heapMin = (uint32_t)ESP.getMinFreeHeap();
  diagnostics.heapInt =
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  diagnostics.rssi = rssi_;
  return diagnostics;
}

void TelemetryClient::reportOnce(uint32_t now) {
  if (strlen(NetworkConfig::ServerCaCertPem) == 0) {
    if (!reportedMissingCa_) {
      reportedMissingCa_ = true;
      Serial.println(
          "[NET] No CA certificate configured (src/ServerCaCert.h missing?); "
          "reports are suppressed");
    }
    return;
  }
  if (strlen(NetworkConfig::DeviceId) == 0 ||
      strlen(NetworkConfig::DeviceToken) == 0) {
    if (!reportedMissingCa_) {
      reportedMissingCa_ = true;
      Serial.println("[NET] Device id or token missing; reports are suppressed");
    }
    return;
  }

  const size_t heapInt =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (heapInt < NetworkConfig::MinInternalHeapBytes) {
    Serial.printf("[NET] Skipping report: internal heap %u B < %u B\n",
                  (unsigned)heapInt, (unsigned)NetworkConfig::MinInternalHeapBytes);
    return;
  }

  const TelemetrySample sample = takeSnapshot();

  TelemetryDiagnostics diagnostics = gatherDiagnostics();

  int length = 0;
  if (!buildBody(sample, diagnostics, body_, sizeof(body_), length)) {
    Serial.println("[NET] Payload buffer too small; report dropped");
    return;
  }
#if SENSOR_SIMULATION
  Serial.printf("[SIM] body: %s\n", body_);
#endif

  const int code = postBody(body_, length);
  httpCode_ = (int16_t)code;
#if SENSOR_SIMULATION
  Serial.printf("[SIM] report -> HTTP %d\n", code);
#endif
  if (code == 204) {
    cloudOk_ = true;
    lastSuccessMs_ = now;
  } else {
    cloudOk_ = false;
    Serial.printf("[NET] Report failed with %d\n", code);
  }
}

#if SENSOR_SIMULATION
void TelemetryClient::printBodyOnly() {
  const TelemetrySample sample = takeSnapshot();
  const TelemetryDiagnostics diagnostics = gatherDiagnostics();
  int length = 0;
  if (!buildBody(sample, diagnostics, body_, sizeof(body_), length)) {
    Serial.println("[SIM] Payload buffer too small");
    return;
  }
  Serial.printf("[SIM] body (no link): %s\n", body_);
}
#endif

bool TelemetryClient::buildBody(const TelemetrySample &sample,
                                const TelemetryDiagnostics &diagnostics,
                                char *out, size_t size, int &length) {
  char temperatureText[16];
  char humidityText[16];
  char setpointText[16];
  jsonFloat(temperatureText, sizeof(temperatureText), sample.temperature, 2);
  jsonFloat(humidityText, sizeof(humidityText), sample.humidity, 2);
  jsonFloat(setpointText, sizeof(setpointText), sample.setpoint, 2);

  const int written = snprintf(
      out, size,
      "{\"device_id\":\"%s\",\"uptime_s\":%lu,\"firmware_version\":\"%s\","
      "\"temperature\":%s,\"temperature_ok\":%s,"
      "\"humidity\":%s,\"humidity_ok\":%s,"
      "\"co2\":%lu,\"co2_ok\":%s,\"co2_available\":%s,"
      "\"target_temperature\":%s,"
      "\"heater_on\":%s,\"heater_pwm\":%lu,"
      "\"light\":%s,\"light_pct\":%u,"
      "\"fan\":%s,\"valve_enabled\":%s,\"valve\":%s,"
      "\"rssi\":%d,"
      "\"heap_free\":%lu,\"heap_min\":%lu,\"heap_int\":%lu}",
      NetworkConfig::DeviceId, (unsigned long)sample.uptimeS,
      NetworkConfig::FirmwareVersion, temperatureText,
      jsonBool(sample.flags & TelemetryTemperatureValid),
      humidityText, jsonBool(sample.flags & TelemetryHumidityValid),
      (unsigned long)sample.co2, jsonBool(sample.flags & TelemetryCo2Valid),
      jsonBool(sample.flags & TelemetryCo2Available),
      setpointText,
      jsonBool(sample.flags & TelemetryHeatingEnabled),
      (unsigned long)sample.heaterDuty,
      jsonBool(sample.flags & TelemetryLightEnabled),
      (unsigned)sample.lightBrightness,
      jsonBool(sample.flags & TelemetryFanEnabled),
      jsonBool(sample.flags & TelemetryValveEnabled),
      jsonBool(sample.flags & TelemetryValveEnabled), (int)diagnostics.rssi,
      (unsigned long)diagnostics.heapFree, (unsigned long)diagnostics.heapMin,
      (unsigned long)diagnostics.heapInt);

  if (written < 0 || (size_t)written >= size) {
    return false;
  }
  length = written;
  return true;
}

int TelemetryClient::postBody(const char *body, int length) {
  if (http_.connected()) {
    http_.end();
  }

  tls_.setCACert(NetworkConfig::ServerCaCertPem);
  // HTTPClient::setTimeout() does not reach the handshake, and this setter is
  // in seconds.
  tls_.setHandshakeTimeout(NetworkConfig::HandshakeTimeoutSeconds);
  http_.setConnectTimeout(NetworkConfig::ConnectTimeoutMs);
  http_.setTimeout(NetworkConfig::ResponseTimeoutMs);
  http_.setReuse(false);

  if (!http_.begin(tls_, NetworkConfig::ServerUrl)) {
    Serial.printf("[NET] Cannot parse server url \"%s\"\n",
                  NetworkConfig::ServerUrl);
    return -1;
  }

  snprintf(authorization_, sizeof(authorization_), "Bearer %s",
           NetworkConfig::DeviceToken);
  http_.addHeader("Content-Type", "application/json");
  http_.addHeader("Authorization", authorization_);

  const int code = http_.POST((uint8_t *)body, (size_t)length);
  // Releases the TLS session and its buffers instead of holding ~32 KB of
  // internal heap for the whole reporting interval.
  http_.end();
  return code;
}

void TelemetryClient::sendTelemetry(uint32_t now) {
  const TelemetrySample sample = takeSnapshot();
  const TelemetryDiagnostics diagnostics = gatherDiagnostics();
  int length = 0;
  if (!buildBody(sample, diagnostics, body_, sizeof(body_), length)) return;
  // The existing formatter already handles NaN correctly; use it as the source
  // of the JSON payload and only add the WebSocket message type.
  const char *bodyStart = body_;
  char wrapped[NetworkConfig::MaxBodyBytes + 16];
  snprintf(wrapped, sizeof(wrapped), "{\"type\":\"telemetry\",%s", bodyStart + 1);
  esp_websocket_client_send_text(websocket_, wrapped, strlen(wrapped),
                                 pdMS_TO_TICKS(1000));
  (void)now;
}

void TelemetryClient::sendHeartbeat(uint32_t now) {
  char message[96];
  const int length = snprintf(message, sizeof(message),
                              "{\"type\":\"heartbeat\",\"uptime_s\":%lu}",
                              (unsigned long)(now / 1000u));
  esp_websocket_client_send_text(websocket_, message, length,
                                 pdMS_TO_TICKS(1000));
}

void TelemetryClient::handleWebsocketEvent(
    int32_t eventId, esp_websocket_event_data_t *event) {
  if (eventId == WEBSOCKET_EVENT_CONNECTED) {
    linkUp_ = true;
    cloudOk_ = true;
    char hello[160];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"device_id\":\"%s\",\"firmware_version\":\"%s\","
             "\"uptime_s\":%lu}", NetworkConfig::DeviceId,
             NetworkConfig::FirmwareVersion, (unsigned long)(millis() / 1000u));
    esp_websocket_client_send_text(websocket_, hello, strlen(hello),
                                   pdMS_TO_TICKS(1000));
    return;
  }
  if (eventId == WEBSOCKET_EVENT_DISCONNECTED ||
      eventId == WEBSOCKET_EVENT_ERROR) {
    linkUp_ = false;
    cloudOk_ = false;
    return;
  }
  if (eventId != WEBSOCKET_EVENT_DATA || event == nullptr ||
      event->data_ptr == nullptr || event->payload_offset != 0) return;
  char json[1024];
  const int length = min(event->data_len, (int)sizeof(json) - 1);
  memcpy(json, event->data_ptr, length);
  json[length] = '\0';
  if (strstr(json, "\"type\":\"command\"") == nullptr) return;
  PendingCommand command;
  if (!jsonString(json, "command_id", command.id, sizeof(command.id))) return;
  portENTER_CRITICAL(&commandMux_);
  const bool duplicate = strcmp(command.id, lastCommandId_) == 0;
  portEXIT_CRITICAL(&commandMux_);
  if (duplicate) return;
  char commandName[32];
  if (!jsonString(json, "command", commandName, sizeof(commandName))) return;
  if (strcmp(commandName, "set_temperature") == 0) {
    float value;
    if (!jsonNumber(json, "value", value) || value < 10.0f || value > 100.0f) return;
    command.type = CommandSetTemperature;
    command.temperature = value;
  } else if (strcmp(commandName, "set_heater") == 0) {
    command.type = CommandSetHeater;
    if (!jsonBoolValue(json, "enabled", command.enabled)) return;
  } else if (strcmp(commandName, "set_light") == 0) {
    command.type = CommandSetLight;
    if (!jsonBoolValue(json, "enabled", command.enabled)) return;
    float brightness;
    command.brightness = command.enabled ? 100 : 0;
    if (jsonNumber(json, "brightness", brightness)) {
      if (brightness < 0 || brightness > 100) return;
      command.brightness = (uint8_t)brightness;
    }
  } else if (strcmp(commandName, "set_fan") == 0) {
    command.type = CommandSetFan;
    if (!jsonBoolValue(json, "enabled", command.enabled)) return;
  } else if (strcmp(commandName, "set_valve_enabled") == 0) {
    command.type = CommandSetValve;
    if (!jsonBoolValue(json, "enabled", command.enabled)) return;
  } else {
    return;
  }
  portENTER_CRITICAL(&commandMux_);
  if (!commandPending_) {
    pendingCommand_ = command;
    commandPending_ = true;
  } else {
    strncpy(ackId_, command.id, sizeof(ackId_) - 1);
    ackId_[sizeof(ackId_) - 1] = '\0';
    ackApplied_ = false;
    snprintf(ackReason_, sizeof(ackReason_), "busy");
    ackPending_ = true;
  }
  portEXIT_CRITICAL(&commandMux_);
}

void TelemetryClient::applyPendingCommand(SystemState &state) {
  PendingCommand command;
  portENTER_CRITICAL(&commandMux_);
  if (!commandPending_) {
    portEXIT_CRITICAL(&commandMux_);
    return;
  }
  command = pendingCommand_;
  commandPending_ = false;
  portEXIT_CRITICAL(&commandMux_);
  bool applied = true;
  switch (command.type) {
    case CommandSetTemperature:
      state.setpoint = constrain((double)command.temperature,
                                 Config::MinimumSetpoint, Config::MaximumSetpoint);
      break;
    case CommandSetHeater:
      state.heatingEnabled = command.enabled;
      break;
    case CommandSetLight:
      state.lightEnabled = command.enabled && command.brightness > 0;
      state.lightBrightness = command.brightness;
      break;
    case CommandSetFan:
      state.fanEnabled = command.enabled;
      break;
    case CommandSetValve:
      state.valveEnabled = command.enabled;
      break;
    default:
      applied = false;
      break;
  }
  if (applied) {
    state.settingsDirty = true;
    state.settingsSaveRequested = true;
    state.settingsSaveFailed = false;
    state.markDisplayDirty();
  }
  portENTER_CRITICAL(&commandMux_);
  strncpy(ackId_, command.id, sizeof(ackId_) - 1);
  ackId_[sizeof(ackId_) - 1] = '\0';
  strncpy(lastCommandId_, command.id, sizeof(lastCommandId_) - 1);
  lastCommandId_[sizeof(lastCommandId_) - 1] = '\0';
  ackApplied_ = applied;
  snprintf(ackReason_, sizeof(ackReason_), "%s", applied ? "" : "unsupported_command");
  ackPending_ = true;
  portEXIT_CRITICAL(&commandMux_);
}

void TelemetryClient::sendPendingAck() {
  char id[40];
  char reason[48];
  bool applied;
  portENTER_CRITICAL(&commandMux_);
  if (!ackPending_) {
    portEXIT_CRITICAL(&commandMux_);
    return;
  }
  strncpy(id, ackId_, sizeof(id));
  strncpy(reason, ackReason_, sizeof(reason));
  applied = ackApplied_;
  ackPending_ = false;
  portEXIT_CRITICAL(&commandMux_);
  char message[180];
  int length;
  if (applied) {
    length = snprintf(message, sizeof(message),
                       "{\"type\":\"command_ack\",\"command_id\":\"%s\",\"status\":\"applied\"}", id);
  } else {
    length = snprintf(message, sizeof(message),
                       "{\"type\":\"command_ack\",\"command_id\":\"%s\",\"status\":\"rejected\",\"reason\":\"%s\"}",
                       id, reason);
  }
  esp_websocket_client_send_text(websocket_, message, length,
                                 pdMS_TO_TICKS(1000));
}
