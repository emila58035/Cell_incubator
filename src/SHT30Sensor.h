#pragma once

#include <Arduino.h>
#include <SHT85.h>

#include "Config.h"
#include "SystemState.h"

//  Temperature + humidity driver for the Sensirion SHT30 over I2C, using the
//  robtillaart/SHT85 library (SHT3x/SHT85 family driver).
class SHT30Sensor {
 public:
  //  The SHT30 driver needs its I2C address at construction time, so it is
  //  bound to Config::ShtAddress here instead of in begin().
  SHT30Sensor();

  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  SHT30 sensor_;
  bool available_ = false;
  uint32_t lastSampleTime_ = 0;
  uint32_t lastConnectAttemptTime_ = 0;

  bool connect(uint32_t now, SystemState &state);
  void sample(uint32_t now, SystemState &state);
};
