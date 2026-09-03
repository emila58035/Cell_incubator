#include "LEDController.h"

#include "Config.h"

void LEDController::begin(SystemState &state) {
  state.lightEnabled = false;
  pinMode(Config::LightPwmPin, OUTPUT);
  digitalWrite(Config::LightPwmPin,
               Config::LightPwmActiveLow ? HIGH : LOW);
  ledcAttach(Config::LightPwmPin, Config::LightPwmFrequency,
             Config::LightPwmResolution);
  writeBrightness(0);
  Serial.printf("[LIGHT] Ready on GPIO%d, startup=OFF\n",
                Config::LightPwmPin);
}

void LEDController::update(SystemState &state) {
  writeBrightness(state.lightEnabled ? state.lightBrightness : 0);
}

void LEDController::writeBrightness(uint8_t brightness) {
  const uint32_t logicalDuty = map(brightness, 0, 100, 0,
                                   Config::LightPwmMaximum);
  const uint32_t physicalDuty = Config::LightPwmActiveLow
                                    ? Config::LightPwmMaximum - logicalDuty
                                    : logicalDuty;
  ledcWrite(Config::LightPwmPin, physicalDuty);
}
