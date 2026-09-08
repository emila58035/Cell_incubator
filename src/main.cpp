#include "AHT20Sensor.h"
#include "Config.h"
#include "DisplayManager.h"
#include "FanController.h"
#include "HeaterController.h"
#include "InputManager.h"
#include "SenseAirS8Sensor.h"
#include "SettingsStorage.h"
#include "UVController.h"
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
UVController uvController;
FanController fanController;
SettingsStorage settingsStorage;

void setup() {
  Serial.begin(Config::DebugBaudRate);

  heaterController.begin(systemState);
  valveController.begin(systemState);
  ledController.begin(systemState);
  uvController.begin(systemState);
  fanController.begin(systemState);

  settingsStorage.begin(systemState);
  displayManager.begin(systemState);
  inputManager.begin();
  climateSensor.begin(systemState);
  co2Sensor.begin(systemState);

  heaterController.update(systemState);
  ledController.update(systemState);
  uvController.update(systemState);
  fanController.update(systemState);
  displayManager.update(systemState);
}

void loop() {
  const uint32_t now = millis();

  inputManager.update(now, systemState);
  settingsStorage.update(now, systemState);
  climateSensor.update(now, systemState);
  co2Sensor.update(now, systemState);
  heaterController.update(systemState);
  displayManager.update(systemState);
  valveController.update(now, systemState);
  ledController.update(systemState);
  uvController.update(systemState);
  fanController.update(systemState);
}
