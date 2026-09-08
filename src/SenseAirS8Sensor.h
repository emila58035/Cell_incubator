#pragma once

#include <Arduino.h>

#include "SystemState.h"

class SenseAirS8Sensor {
 public:
  SenseAirS8Sensor();

  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  enum class RequestType : uint8_t {
    None,
    Firmware,
    Measurement,
  };

  static constexpr size_t ResponseLength = 7;

  HardwareSerial serial_;
  bool available_ = false;
  uint8_t consecutiveErrors_ = 0;
  uint32_t lastSampleTime_ = 0;
  uint32_t lastConnectAttemptTime_ = 0;
  uint32_t requestStartTime_ = 0;
  RequestType requestType_ = RequestType::None;
  uint8_t response_[ResponseLength]{};
  size_t responseLength_ = 0;

  void startUart();
  void startConnection(uint32_t now, SystemState &state);
  void requestMeasurement(uint32_t now);
  void sendReadRequest(uint16_t reg, RequestType type, uint32_t now);
  void receiveResponse(uint32_t now, SystemState &state);
  void handleResponse(uint32_t now, SystemState &state);
  void handleRequestFailure(uint32_t now, SystemState &state,
                            const char *reason);
  bool responseIsValid() const;
  void clearReceiveBuffer();
  void markUnavailable(uint32_t now, SystemState &state);
  static uint16_t calculateModbusCrc(const uint8_t *data, size_t length);
};
