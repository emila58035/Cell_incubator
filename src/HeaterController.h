#pragma once

#include <Arduino.h>
#include <PID_v1.h>

#include <array>

#include "Config.h"
#include "SystemState.h"

class HeaterController {
 public:
  HeaterController();

  void begin(SystemState &state);
  void update(SystemState &state);

 private:
  double input_ = 0.0;
  double output_ = 0.0;
  double setpoint_ = 0.0;
  PID pid_;
  bool automatic_ = false;
  bool sensorFault_ = false;
  uint32_t lastDuty_ = 0;

  std::array<double, Config::PredictiveHistorySeconds> pwmHistory_{};
  uint16_t pwmHistoryCount_ = 0;
  uint16_t pwmHistoryHead_ = 0;
  uint32_t lastPredictiveSampleMs_ = 0;
  bool hasPredictiveSample_ = false;
  double predictivePwm_ = 0.0;
  bool hasPredictivePwm_ = false;

  void stop(SystemState &state);
  void writeHeaterDuty(uint32_t duty);
  void publishOutput(SystemState &state, uint32_t duty);
  void resetPrediction();
  void recordPredictionSample(uint32_t duty);
  double predictTemperature(double temperature, double candidatePwm) const;
  void logPrediction(const SystemState &state, double predictedTemperature,
                     double basePwm, double pidPwm);
};
