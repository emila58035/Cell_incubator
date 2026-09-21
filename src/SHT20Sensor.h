#pragma once

#include <Arduino.h>
#include <SHT2x.h>

#include "SystemState.h"

//  Temperature + humidity driver for the Sensirion SHT20 (SHT2x family) over
//  I2C, using the robtillaart/SHT2x library. The chip has no address strap and
//  the library owns its 0x40 address, so no address is passed in here.
class SHT20Sensor {
 public:
  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  SHT20 sensor_;
  bool available_ = false;
  uint8_t consecutiveErrors_ = 0;
  uint32_t lastSampleTime_ = 0;
  uint32_t lastConnectAttemptTime_ = 0;

  bool connect(uint32_t now, SystemState &state);
  void sample(uint32_t now, SystemState &state);
};