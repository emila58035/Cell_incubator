#pragma once

#include <Arduino.h>

#include "SystemState.h"

class SettingsStorage {
 public:
  void begin(SystemState &state);
  bool saveNow(SystemState &state);

 private:
  struct Snapshot {
    int16_t setpointTenths;
    uint8_t lightBrightness;
    bool lightEnabled;
    bool fanEnabled;
    bool valveEnabled;
  };

  static Snapshot capture(const SystemState &state);
  bool load(SystemState &state);
  bool save(const Snapshot &settings);
  static void logSettings(const char *event, const Snapshot &settings);
};
