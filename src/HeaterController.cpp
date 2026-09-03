#include "HeaterController.h"

#include "Config.h"

HeaterController::HeaterController()
    : pid_(&input_, &output_, &setpoint_, Config::ConservativeKp,
           Config::ConservativeKi, Config::ConservativeKd, DIRECT) {}

void HeaterController::begin(SystemState &state) {
  pinMode(Config::HeaterPwmPin, OUTPUT);
  digitalWrite(Config::HeaterPwmPin,
               Config::HeaterPwmActiveLow ? HIGH : LOW);
  ledcAttach(Config::HeaterPwmPin, Config::HeaterPwmFrequency,
             Config::HeaterPwmResolution);
  writeHeaterDuty(0);

  setpoint_ = state.setpoint;
  pid_.SetOutputLimits(0, Config::HeaterPwmMaximum);
  pid_.SetSampleTime(Config::AhtSampleIntervalMs);
  pid_.SetMode(MANUAL);
  publishOutput(state, 0);

  Serial.printf("[HEATER] Ready on GPIO%d; startup state=OFF\n",
                Config::HeaterPwmPin);
}

void HeaterController::update(SystemState &state) {
  if (!state.temperatureValid) {
    stop(state, true);
    return;
  }

  if (!state.heatingEnabled) {
    stop(state, false);
    return;
  }

  input_ = state.temperature;
  setpoint_ = state.setpoint;

  if (!automatic_) {
    output_ = 0.0;
    pid_.SetMode(AUTOMATIC);
    automatic_ = true;
  }

  const double gap = abs(setpoint_ - input_);
  if (gap < Config::ConservativeBandCelsius) {
    pid_.SetTunings(Config::ConservativeKp, Config::ConservativeKi,
                    Config::ConservativeKd);
  } else {
    pid_.SetTunings(Config::AggressiveKp, Config::AggressiveKi,
                    Config::AggressiveKd);
  }

  pid_.Compute();
  const uint32_t duty =
      (uint32_t)constrain(output_, 0.0, (double)Config::HeaterPwmMaximum);
  writeHeaterDuty(duty);
  publishOutput(state, duty);
}

void HeaterController::stop(SystemState &state, bool disableRequest) {
  if (automatic_) {
    pid_.SetMode(MANUAL);
    automatic_ = false;
  }

  output_ = 0.0;
  writeHeaterDuty(0);

  if (disableRequest && state.heatingEnabled) {
    state.heatingEnabled = false;
    Serial.println("[HEATER] Disabled: temperature is invalid");
  }

  publishOutput(state, 0);
}

void HeaterController::writeHeaterDuty(uint32_t duty) {
  const uint32_t limitedDuty = min(duty, Config::HeaterPwmMaximum);
  const uint32_t physicalDuty = Config::HeaterPwmActiveLow
                                    ? Config::HeaterPwmMaximum - limitedDuty
                                    : limitedDuty;
  ledcWrite(Config::HeaterPwmPin, physicalDuty);
}

void HeaterController::publishOutput(SystemState &state, uint32_t duty) {
  if (state.heaterDuty != duty || state.heaterOutput != output_ ||
      lastDuty_ != duty) {
    state.heaterDuty = duty;
    state.heaterOutput = output_;
    state.markDisplayDirty();
  }
  lastDuty_ = duty;
}
