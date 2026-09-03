#include "FanController.h"

#include "Config.h"

void FanController::begin(SystemState &state) {
  state.fanEnabled = false;
  pinMode(Config::FanOutputPin, OUTPUT);
  digitalWrite(Config::FanOutputPin,
               Config::FanOutputActiveLow ? HIGH : LOW);
  Serial.printf("[FAN] Ready on GPIO%d, startup=OFF\n",
                Config::FanOutputPin);
}

void FanController::update(const SystemState &state) {
  const bool outputHigh = state.fanEnabled ? !Config::FanOutputActiveLow
                                           : Config::FanOutputActiveLow;
  digitalWrite(Config::FanOutputPin, outputHigh ? HIGH : LOW);
}
