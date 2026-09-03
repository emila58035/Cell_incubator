#pragma once

#include <AHT20.h>
#include <Arduino.h>

#include "SystemState.h"

class AHT20Sensor {
 public:
  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  AHT20 sensor_;
  bool available_ = false;
  uint32_t lastSampleTime_ = 0;
  uint32_t lastConnectAttemptTime_ = 0;

  bool connect(uint32_t now, SystemState &state);
  void sample(uint32_t now, SystemState &state);
};
