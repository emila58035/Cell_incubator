#include "FanController.h"

#include "Config.h"

void FanController::begin(SystemState &state) {
  state.fanEnabled = false;
  state.fanPower = 0;
  pinMode(Config::FanOutputPin, OUTPUT);
  digitalWrite(Config::FanOutputPin,
               Config::FanOutputActiveLow ? HIGH : LOW);
  ledcAttach(Config::FanOutputPin, Config::FanPwmFrequency,
             Config::FanPwmResolution);
  update(state);
  Serial.printf("[FAN] Ready on GPIO%d, startup=OFF\n",
                Config::FanOutputPin);
}

void FanController::update(const SystemState &state) {
  const uint32_t duty = map(state.fanEnabled ? state.fanPower : 0,
                            0, 100, 0, Config::FanPwmMaximum);
  ledcWrite(Config::FanOutputPin, Config::FanOutputActiveLow
                                    ? Config::FanPwmMaximum - duty : duty);
}
