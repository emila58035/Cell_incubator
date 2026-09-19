#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

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
  TelemetryValveOpen = 1u << 7,
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
  void taskLoop();
  void publish(uint32_t now, SystemState &state);
  TelemetrySample takeSnapshot();
  void reportOnce(uint32_t now);
  bool buildBody(const TelemetrySample &sample,
                 const TelemetryDiagnostics &diagnostics, char *out, size_t size,
                 int &length);
  int postBody(const char *body, int length);

  WiFiManager wifi_;
  WiFiClientSecure tls_;
  HTTPClient http_;

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

  // Static allocation keeps the stack in .bss, which is internal RAM. A
  // heap-allocated task stack could land in PSRAM (the precompiled libraries
  // set SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY), where it would be unreachable
  // while an SPI flash write disables the cache - for example while the
  // settings module commits to NVS.
  StaticTask_t taskTcb_;
  StackType_t taskStack_[NetworkConfig::NetTaskStackBytes / sizeof(StackType_t)];
};