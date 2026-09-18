#pragma once

#include <Arduino.h>
#include <PID_v1.h>

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

  void stop(SystemState &state);
  void writeHeaterDuty(uint32_t duty);
  void publishOutput(SystemState &state, uint32_t duty);
};
