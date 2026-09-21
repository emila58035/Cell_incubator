// The development build ([env:sim] in platformio.ini) swaps the two sensor
// drivers for SensorSimulator, which fabricates readings so that the telemetry
// path can be exercised on a board with no sensors attached.
#ifndef SENSOR_SIMULATION
#define SENSOR_SIMULATION 0
#endif

#include "Config.h"
#include "DisplayManager.h"
#include "FanController.h"
#include "HeaterController.h"
#include "InputManager.h"
#include "LEDController.h"
#include "SettingsStorage.h"
#include "SystemState.h"
#include "TelemetryClient.h"
#include "ValveController.h"
#if SENSOR_SIMULATION
#include "SensorSimulator.h"
#else
#include "SenseAirS8Sensor.h"
#include "SHT20Sensor.h"
#endif

SystemState systemState;
#if SENSOR_SIMULATION
SensorSimulator sensorSimulator;
#else
SHT20Sensor climateSensor;
SenseAirS8Sensor co2Sensor;
#endif
InputManager inputManager;
HeaterController heaterController;
DisplayManager displayManager;
ValveController valveController;
LEDController ledController;
FanController fanController;
SettingsStorage settingsStorage;
TelemetryClient telemetryClient;

void setup() {
  Serial.begin(Config::DebugBaudRate);

  // Boot diagnostics. These also settle two questions that the build alone
  // cannot: how much PSRAM the module really provides, and which flash size
  // the image header reports. No delay is added on purpose - the heater pin is
  // still unconfigured at this point, so it must not stay that way longer than
  // the existing code already does.
  Serial.printf("\n[BOOT] %s rev%d, %u core(s), flash %u B, psram %u B, heap %u B\n",
                ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores(),
                (unsigned)ESP.getFlashChipSize(),
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

  heaterController.begin(systemState);
  valveController.begin(systemState);
  ledController.begin(systemState);
  fanController.begin(systemState);

  settingsStorage.begin(systemState);
  displayManager.begin(systemState);
  inputManager.begin();
#if SENSOR_SIMULATION
  sensorSimulator.begin(systemState);
#else
  climateSensor.begin(systemState);
  co2Sensor.begin(systemState);
#endif

  heaterController.update(systemState);
  ledController.update(systemState);
  fanController.update(systemState);
  displayManager.update(systemState);

  telemetryClient.begin();
}

void loop() {
  const uint32_t now = millis();

  inputManager.update(now, systemState);
  if (systemState.settingsSaveRequested) {
    settingsStorage.saveNow(systemState);
  }
#if SENSOR_SIMULATION
  sensorSimulator.update(now, systemState);
#else
  climateSensor.update(now, systemState);
  co2Sensor.update(now, systemState);
#endif
  heaterController.update(systemState);
  displayManager.update(systemState);
  valveController.update(now, systemState);
  ledController.update(systemState);
  fanController.update(systemState);
  // Appended last on purpose: the networking modules must not shift the
  // relative timing of the control modules above.
  telemetryClient.update(now, systemState);
}
