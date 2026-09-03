#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

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
  void renderStatusSprite(const SystemState &state);
  void renderSettingsSprite(const SystemState &state);
  void renderStatusDirect(const SystemState &state);
  void renderSettingsDirect(const SystemState &state);
};
