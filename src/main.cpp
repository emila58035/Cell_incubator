#include "AHT20Sensor.h"
#include "Config.h"
#include "DisplayManager.h"
#include "FanController.h"
#include "HeaterController.h"
#include "InputManager.h"
#include "SenseAirS8Sensor.h"
#include "SettingsStorage.h"
#include "ValveController.h"
#include "LEDController.h"
#include "SystemState.h"

SystemState systemState;
AHT20Sensor climateSensor;
SenseAirS8Sensor co2Sensor;
InputManager inputManager;
HeaterController heaterController;
DisplayManager displayManager;
ValveController valveController;
LEDController ledController;
FanController fanController;
SettingsStorage settingsStorage;

void setup() {
  Serial.begin(Config::DebugBaudRate);

  heaterController.begin(systemState);
  valveController.begin(systemState);
  ledController.begin(systemState);
  fanController.begin(systemState);

  settingsStorage.begin(systemState);
  displayManager.begin(systemState);
  inputManager.begin();
  climateSensor.begin(systemState);
  co2Sensor.begin(systemState);

  heaterController.update(systemState);
  ledController.update(systemState);
  fanController.update(systemState);
  displayManager.update(systemState);
}

void loop() {
  const uint32_t now = millis();

  inputManager.update(now, systemState);
  if (systemState.settingsSaveRequested) {
    settingsStorage.saveNow(systemState);
  }
  climateSensor.update(now, systemState);
  co2Sensor.update(now, systemState);
  heaterController.update(systemState);
  displayManager.update(systemState);
  valveController.update(now, systemState);
  ledController.update(systemState);
  fanController.update(systemState);
}
