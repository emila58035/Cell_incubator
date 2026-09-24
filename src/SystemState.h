#pragma once

#include <Arduino.h>

enum class SettingsItem : uint8_t {
  SetTemperature,
  Light,
  Fan,
  Save,
  Valve,
  Count,
};

struct SystemState {
  SystemState();

  float temperature;
  float humidity;
  uint32_t co2;

  bool temperatureValid;
  bool humidityValid;
  bool co2Available;
  bool co2Valid;

  double setpoint;
  double heaterOutput;
  uint32_t heaterDuty;
  bool heatingEnabled;
  bool lightEnabled;
  uint8_t lightBrightness;
  bool fanEnabled;
  bool valveEnabled;
  bool valveOpen;

  bool settingsDirty;
  bool settingsSaveRequested;
  bool settingsSaveFailed;

  SettingsItem settingsItem;
  bool settingsItemSelected;

  bool displayDirty;

  void invalidateClimate();
  void setClimate(float newTemperature, float newHumidity,
                  bool newHumidityValid);
  void setCo2(uint32_t newCo2, bool available, bool valid);
  void markDisplayDirty();
};
