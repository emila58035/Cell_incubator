#pragma once

#include <Arduino.h>

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
  bool LEDState;

  bool displayDirty;

  void invalidateClimate();
  void setClimate(float newTemperature, float newHumidity,
                  bool newHumidityValid);
  void setCo2(uint32_t newCo2, bool available, bool valid);
  void markDisplayDirty();
};
