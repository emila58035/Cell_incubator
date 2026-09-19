#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// The panel setup lives in src/User_Setup.h and is force-included through the
// build_flags in platformio.ini. Without that include TFT_eSPI silently falls
// back to its stock ILI9341 setup, which still compiles but drives the wrong
// controller. Fail loudly instead.
#if !defined(ST7789_DRIVER) || (TFT_WIDTH != 240) || (TFT_HEIGHT != 320)
#error "Panel setup missing: src/User_Setup.h was not loaded via build_flags"
#endif

#include "SystemState.h"

class DisplayManager {
 public:
  DisplayManager();

  void begin(SystemState &state);
  void update(SystemState &state);

 private:
  TFT_eSPI tft_;
  TFT_eSprite sprite_;
  bool spriteReady_ = false;

  void renderSprite(const SystemState &state);
  void renderDirect(const SystemState &state);
};
