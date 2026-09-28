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
  resetPrediction();
  publishOutput(state, 0);

  Serial.printf("[HEATER] Ready on GPIO%d; startup state=OFF\n",
                Config::HeaterPwmPin);
}

void HeaterController::update(SystemState &state) {
  // The heating switch is controlled solely by user input.
  if (!state.heatingEnabled) {
    sensorFault_ = false;
    resetPrediction();
    stop(state);
    return;
  }

  // Switch is ON but no usable temperature: pause heating, keep switch ON.
  if (!state.temperatureValid) {
    if (!sensorFault_) {
      sensorFault_ = true;
      Serial.println(
          "[HEATER] Sensor fault: heating paused, switch stays ON");
    }
    resetPrediction();
    stop(state);
    return;
  }

  if (sensorFault_) {
    sensorFault_ = false;
    Serial.println("[HEATER] Sensor recovered: resuming control");
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

  const uint32_t now = millis();
  if (!hasPredictiveSample_ ||
      now - lastPredictiveSampleMs_ >= Config::PredictiveLogIntervalMs) {
    recordPredictionSample(duty);
    const double basePwm = constrain(
        (setpoint_ - Config::PredictiveAmbientCelsius) /
            Config::PredictiveFullPwmRiseCelsius * Config::HeaterPwmMaximum,
        0.0, (double)Config::HeaterPwmMaximum);
    const double predictedTemperature =
        predictTemperature(input_, static_cast<double>(duty));
    logPrediction(state, predictedTemperature, basePwm,
                  static_cast<double>(duty));
    lastPredictiveSampleMs_ = now;
    hasPredictiveSample_ = true;
  }
}

void HeaterController::stop(SystemState &state) {
  if (automatic_) {
    pid_.SetMode(MANUAL);
    automatic_ = false;
  }

  output_ = 0.0;
  resetPrediction();
  writeHeaterDuty(0);
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

void HeaterController::resetPrediction() {
  pwmHistory_.fill(0.0);
  pwmHistoryCount_ = 0;
  pwmHistoryHead_ = 0;
  lastPredictiveSampleMs_ = 0;
  hasPredictiveSample_ = false;
}

void HeaterController::recordPredictionSample(uint32_t duty) {
  pwmHistory_[pwmHistoryHead_] = static_cast<double>(duty);
  pwmHistoryHead_ =
      (pwmHistoryHead_ + 1) % Config::PredictiveHistorySeconds;
  if (pwmHistoryCount_ < Config::PredictiveHistorySeconds) {
    ++pwmHistoryCount_;
  }
}

double HeaterController::predictTemperature(double temperature,
                                             double candidatePwm) const {
  const uint16_t delaySteps = static_cast<uint16_t>(round(
      Config::PredictiveDelaySeconds));
  const uint16_t horizonSteps = Config::PredictiveHorizonSeconds;
  const double dt = 1.0;
  double predicted = temperature;

  for (uint16_t step = 0; step < horizonSteps; ++step) {
    double delayedPwm = candidatePwm;
    if (step < delaySteps && pwmHistoryCount_ > 0) {
      const int32_t relativeIndex =
          static_cast<int32_t>(pwmHistoryCount_) - delaySteps + step;
      if (relativeIndex >= 0) {
        const uint16_t oldestIndex =
            (pwmHistoryHead_ + Config::PredictiveHistorySeconds -
             pwmHistoryCount_) %
            Config::PredictiveHistorySeconds;
        const uint16_t index =
            (oldestIndex + static_cast<uint16_t>(relativeIndex)) %
            Config::PredictiveHistorySeconds;
        delayedPwm = pwmHistory_[index];
      }
    }

    const double equilibriumTemperature =
        Config::PredictiveAmbientCelsius +
        Config::PredictiveFullPwmRiseCelsius *
            delayedPwm / Config::HeaterPwmMaximum;
    predicted +=
        (equilibriumTemperature - predicted) * dt /
        Config::PredictiveTimeConstantSeconds;
  }
  return predicted;
}

void HeaterController::logPrediction(const SystemState &state,
                                     double predictedTemperature,
                                     double basePwm,
                                     double candidatePwm) const {
  Serial.printf(
      "[PREDICT] temp=%.2f set=%.2f pwm=%.0f base=%.1f pred_%us=%.2f\n",
      state.temperature, state.setpoint, candidatePwm, basePwm,
      Config::PredictiveHorizonSeconds, predictedTemperature);
}
