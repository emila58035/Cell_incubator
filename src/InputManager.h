#pragma once

#include <Arduino.h>

#include "SystemState.h"

class InputManager {
 public:
  void begin();
  void update(uint32_t now, SystemState &state);

 private:
  static InputManager *instance_;

  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  volatile int32_t encoderSteps_ = 0;
  volatile int8_t encoderAccumulator_ = 0;
  volatile uint8_t encoderState_ = 0;
  volatile bool pushPressedEvent_ = false;
  volatile bool KOPressedEvent_ = false;
  uint32_t lastPushTime_ = 0;
  uint32_t lastKOTime_ = 0;

  static void IRAM_ATTR encoderIsr();
  static void IRAM_ATTR pushIsr();
  static void IRAM_ATTR KOIsr();
  void IRAM_ATTR captureEncoderTransition();
};
