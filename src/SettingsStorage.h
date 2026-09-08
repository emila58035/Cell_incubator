#pragma once

#include <Arduino.h>

#include "SystemState.h"

class SettingsStorage {
 public:
  void begin(SystemState &state);
  void update(uint32_t now, const SystemState &state);

 private:
  struct Snapshot {
    int16_t setpointTenths;
    uint8_t lightBrightness;
    bool lightEnabled;
    bool uvEnabled;
    bool fanEnabled;
  };

  Snapshot observed_{};
  bool pendingSave_ = false;
  uint32_t lastChangeTime_ = 0;

  static Snapshot capture(const SystemState &state);
  static bool equal(const Snapshot &left, const Snapshot &right);
  bool load(SystemState &state);
  bool save(const Snapshot &settings);
  static void logSettings(const char *event, const Snapshot &settings);
};
