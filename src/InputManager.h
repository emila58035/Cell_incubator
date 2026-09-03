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
  bool pushPressedEvent_ = false;
  bool KOPressedEvent_ = false;
  bool pushRawPressed_ = false;
  bool pushStablePressed_ = false;
  bool KORawPressed_ = false;
  bool KOStablePressed_ = false;
  uint32_t pushLastChangeTime_ = 0;
  uint32_t KOLastChangeTime_ = 0;

  static void IRAM_ATTR encoderIsr();
  void IRAM_ATTR captureEncoderTransition();
  void updateButton(uint32_t now, int pin, bool &rawPressed,
                    bool &stablePressed, uint32_t &lastChangeTime,
                    bool &pressedEvent);
};
