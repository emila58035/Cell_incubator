#include "UVController.h"

#include "Config.h"

void UVController::begin(SystemState &state) {
  state.uvEnabled = false;
  pinMode(Config::UvOutputPin, OUTPUT);
  digitalWrite(Config::UvOutputPin,
               Config::UvOutputActiveLow ? HIGH : LOW);
  Serial.printf("[UV] Ready on GPIO%d, startup=OFF\n", Config::UvOutputPin);
}

void UVController::update(const SystemState &state) {
  const bool outputHigh = state.uvEnabled ? !Config::UvOutputActiveLow
                                          : Config::UvOutputActiveLow;
  digitalWrite(Config::UvOutputPin, outputHigh ? HIGH : LOW);
}
