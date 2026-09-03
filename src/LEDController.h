#pragma once

#include <Arduino.h>

#include "SystemState.h"

class LEDController {
 public:
  void begin(SystemState &state);
  void update(SystemState &state);

 private:
  void writeBrightness(uint8_t brightness);
};
