#pragma once

#include <Arduino.h>
#include <s8_uart.h>

#include "SystemState.h"

class SenseAirS8Sensor {
 public:
  SenseAirS8Sensor();

  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  HardwareSerial serial_;
  S8_UART sensor_;
  S8_sensor info_{};

  bool available_ = false;
  uint8_t consecutiveErrors_ = 0;
  uint32_t lastSampleTime_ = 0;
  uint32_t lastConnectAttemptTime_ = 0;

  void startUart();
  bool connect(uint32_t now, SystemState &state);
  void sample(uint32_t now, SystemState &state);
  void clearReceiveBuffer();
  void markUnavailable(uint32_t now, SystemState &state);
};
