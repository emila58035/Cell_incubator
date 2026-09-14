#include "SystemState.h"

#include "Config.h"

SystemState::SystemState()
    : temperature(NAN),
      humidity(NAN),
      co2(0),
      temperatureValid(false),
      humidityValid(false),
      co2Available(false),
      co2Valid(false),
      setpoint(Config::DefaultSetpoint),
      heaterOutput(0.0),
      heaterDuty(0),
      heatingEnabled(true),
      lightEnabled(false),
      lightBrightness(0),
      fanEnabled(false),
      valveOpen(false),
      settingsDirty(false),
      settingsSaveRequested(false),
      settingsSaveFailed(false),
      settingsItem(SettingsItem::SetTemperature),
      settingsItemSelected(false),
      displayDirty(true) {}

void SystemState::invalidateClimate() {
  temperature = NAN;
  humidity = NAN;
  temperatureValid = false;
  humidityValid = false;
  displayDirty = true;
}

void SystemState::setClimate(float newTemperature, float newHumidity,
                             bool newHumidityValid) {
  temperature = newTemperature;
  humidity = newHumidity;
  temperatureValid = true;
  humidityValid = newHumidityValid;
  displayDirty = true;
}

void SystemState::setCo2(uint32_t newCo2, bool available, bool valid) {
  co2 = newCo2;
  co2Available = available;
  co2Valid = valid;
  displayDirty = true;
}

void SystemState::markDisplayDirty() {
  displayDirty = true;
}
