#include "TelemetryClient.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

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
    if ((int32_t)(now - nextReportMs) < 0) {
      continue;
    }
    // Deadline rather than accumulator: a skipped report must not shift the
    // following ones.
    nextReportMs = now + NetworkConfig::ReportIntervalMs;
    reportOnce(now);
  }
}

void TelemetryClient::update(uint32_t now, SystemState &state) {
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
      "\"fan\":%s,\"valve\":%s,"
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