#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_websocket_client.h"

#include "NetworkConfig.h"
#include "SystemState.h"
#include "WiFiManager.h"

// Control state published by the control loop (core 1) and copied by the
// network task (core 0).
//
// Every member is at most 4 bytes and 4-byte aligned, so a single store cannot
// tear while the other core reads it. Both sides still enter the same portMUX
// spinlock: a spinlock only excludes across cores when everyone takes it, and
// a consistent multi-field snapshot needs it anyway.
//
// `double` members of SystemState are narrowed here on purpose: an 8-byte value
// is two stores on this target and would tear. float carries far more
// resolution than a temperature or a duty cycle needs.
struct TelemetrySample {
  float temperature;
  float humidity;
  float setpoint;
  uint32_t co2;
  uint32_t heaterDuty;
  uint32_t uptimeS;
  uint8_t lightBrightness;
  uint8_t flags;
  uint16_t reserved;
};

enum TelemetryFlag : uint8_t {
  TelemetryTemperatureValid = 1u << 0,
  TelemetryHumidityValid = 1u << 1,
  TelemetryCo2Available = 1u << 2,
  TelemetryCo2Valid = 1u << 3,
  TelemetryHeatingEnabled = 1u << 4,
  TelemetryLightEnabled = 1u << 5,
  TelemetryFanEnabled = 1u << 6,
  TelemetryValveEnabled = 1u << 7,
};

// Diagnostics are read by the network task at the moment of the report, so
// they describe the reporting attempt rather than the control snapshot.
// heapInt is internal RAM only: ESP.getFreeHeap() also counts PSRAM and would
// hide an internal leak, and internal RAM is what decides whether another TLS
// session fits.
struct TelemetryDiagnostics {
  uint32_t heapFree;
  uint32_t heapMin;
  uint32_t heapInt;
  int16_t rssi;
};

class TelemetryClient {
 public:
  // Creates the network task on core 0. Call once from setup().
  void begin();

  // Called from the control loop (core 1) at the end of every iteration.
  // Publishes the control snapshot and returns within microseconds.
  void update(uint32_t now, SystemState &state);

  // Safe to read from core 1 at any time; plain volatile reads.
  bool linkUp() const { return linkUp_; }
  bool cloudOk() const { return cloudOk_; }
  int16_t rssi() const { return rssi_; }
  int16_t lastHttpCode() const { return httpCode_; }
  uint32_t lastSuccessMs() const { return lastSuccessMs_; }

 private:
  static void taskEntry(void *arg);
  static void websocketEvent(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data);
  void taskLoop();
  void publish(uint32_t now, SystemState &state);
  TelemetrySample takeSnapshot();
  TelemetryDiagnostics gatherDiagnostics() const;
  void reportOnce(uint32_t now);
  void sendTelemetry(uint32_t now);
  void sendHeartbeat(uint32_t now);
  void handleWebsocketEvent(int32_t eventId, esp_websocket_event_data_t *event);
  void applyPendingCommand(SystemState &state);
  void sendPendingAck();
#if SENSOR_SIMULATION
  // Development build only: build and print the payload without posting, so the
  // contract can be checked with no access point and no credentials.
  void printBodyOnly();
#endif
  bool buildBody(const TelemetrySample &sample,
                 const TelemetryDiagnostics &diagnostics, char *out, size_t size,
                 int &length);
  int postBody(const char *body, int length);

  WiFiManager wifi_;
  WiFiClientSecure tls_;
  HTTPClient http_;
  esp_websocket_client_handle_t websocket_ = nullptr;

  char body_[NetworkConfig::MaxBodyBytes];
  char authorization_[96];
  TelemetrySample sample_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  volatile bool linkUp_ = false;
  volatile bool cloudOk_ = false;
  volatile int16_t rssi_ = 0;
  volatile int16_t httpCode_ = 0;
  volatile uint32_t lastSuccessMs_ = 0;
  bool reportedMissingCa_ = false;

  enum RemoteCommandType : uint8_t {
    CommandNone,
    CommandSetTemperature,
    CommandSetHeater,
    CommandSetLight,
    CommandSetFan,
    CommandSetValve,
  };
  struct PendingCommand {
    RemoteCommandType type = CommandNone;
    float temperature = 0.0f;
    bool enabled = false;
    uint8_t brightness = 0;
    char id[40] = {};
  };
  PendingCommand pendingCommand_;
  bool commandPending_ = false;
  bool ackPending_ = false;
  bool ackApplied_ = false;
  char ackId_[40] = {};
  char ackReason_[48] = {};
  char lastCommandId_[40] = {};
  portMUX_TYPE commandMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t lastHeartbeatMs_ = 0;

  // Static allocation keeps the stack in .bss, which is internal RAM. A
  // heap-allocated task stack could land in PSRAM (the precompiled libraries
  // set SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY), where it would be unreachable
  // while an SPI flash write disables the cache - for example while the
  // settings module commits to NVS.
  StaticTask_t taskTcb_;
  StackType_t taskStack_[NetworkConfig::NetTaskStackBytes / sizeof(StackType_t)];
};
