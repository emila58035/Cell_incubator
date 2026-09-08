#include "SenseAirS8Sensor.h"

#include "Config.h"

namespace {

constexpr uint8_t ModbusAddress = 0xFE;
constexpr uint8_t ReadInputRegisters = 0x04;
constexpr uint16_t FirmwareRegister = 0x001C;
constexpr uint16_t Co2Register = 0x0003;
constexpr size_t RequestLength = 8;
constexpr size_t MaximumBytesPerUpdate = 32;

}  // namespace

SenseAirS8Sensor::SenseAirS8Sensor() : serial_(Config::S8UartPort) {}

void SenseAirS8Sensor::begin(SystemState &state) {
  Serial.println("[CO2] Initializing SenseAir S8...");
  Serial.printf("[CO2] UART%d, baud=%lu, RX=GPIO%d, TX=GPIO%d\n",
                Config::S8UartPort,
                static_cast<unsigned long>(Config::S8BaudRate),
                Config::S8RxPin, Config::S8TxPin);
  startUart();
  startConnection(millis(), state);
}

void SenseAirS8Sensor::update(uint32_t now, SystemState &state) {
  if (requestType_ != RequestType::None) {
    receiveResponse(now, state);
    return;
  }

  if (!available_) {
    if (now - lastConnectAttemptTime_ >= Config::Co2ReconnectIntervalMs) {
      Serial.println("[CO2] Attempting automatic reconnect...");
      startUart();
      startConnection(now, state);
    }
    return;
  }

  if (now - lastSampleTime_ >= Config::Co2SampleIntervalMs) {
    requestMeasurement(now);
  }
}

void SenseAirS8Sensor::startUart() {
  serial_.end();
  serial_.begin(Config::S8BaudRate, SERIAL_8N1, Config::S8RxPin,
                Config::S8TxPin);
  clearReceiveBuffer();
}

void SenseAirS8Sensor::startConnection(uint32_t now, SystemState &state) {
  lastConnectAttemptTime_ = now;
  available_ = false;
  consecutiveErrors_ = 0;
  state.setCo2(state.co2, false, false);
  sendReadRequest(FirmwareRegister, RequestType::Firmware, now);
  Serial.println("[CO2] Firmware request sent");
}

void SenseAirS8Sensor::requestMeasurement(uint32_t now) {
  lastSampleTime_ = now;
  sendReadRequest(Co2Register, RequestType::Measurement, now);
  Serial.printf("[CO2] Measurement request sent at %lu ms\n",
                static_cast<unsigned long>(now));
}

void SenseAirS8Sensor::sendReadRequest(uint16_t reg, RequestType type,
                                       uint32_t now) {
  clearReceiveBuffer();

  uint8_t request[RequestLength] = {
      ModbusAddress,
      ReadInputRegisters,
      static_cast<uint8_t>(reg >> 8),
      static_cast<uint8_t>(reg & 0xFF),
      0x00,
      0x01,
      0x00,
      0x00,
  };
  const uint16_t crc = calculateModbusCrc(request, RequestLength - 2);
  request[6] = static_cast<uint8_t>(crc & 0xFF);
  request[7] = static_cast<uint8_t>(crc >> 8);

  responseLength_ = 0;
  requestType_ = type;
  requestStartTime_ = now;
  serial_.write(request, sizeof(request));
}

void SenseAirS8Sensor::receiveResponse(uint32_t now, SystemState &state) {
  size_t processed = 0;
  while (serial_.available() > 0 && processed < MaximumBytesPerUpdate) {
    const int incoming = serial_.read();
    processed++;
    if (incoming < 0) {
      break;
    }

    const uint8_t byte = static_cast<uint8_t>(incoming);
    if (responseLength_ == 0 && byte != ModbusAddress) {
      continue;
    }

    response_[responseLength_++] = byte;
    if (responseLength_ == ResponseLength) {
      handleResponse(now, state);
      return;
    }
  }

  if (now - requestStartTime_ >= Config::Co2ResponseTimeoutMs) {
    handleRequestFailure(now, state, "response timeout");
  }
}

void SenseAirS8Sensor::handleResponse(uint32_t now, SystemState &state) {
  if (!responseIsValid()) {
    handleRequestFailure(now, state, "invalid Modbus response");
    return;
  }

  const uint16_t value =
      (static_cast<uint16_t>(response_[3]) << 8) | response_[4];
  const uint32_t elapsed = now - requestStartTime_;
  const RequestType completedRequest = requestType_;
  requestType_ = RequestType::None;
  responseLength_ = 0;

  if (completedRequest == RequestType::Firmware) {
    available_ = true;
    consecutiveErrors_ = 0;
    state.setCo2(state.co2, true, false);
    lastSampleTime_ = now - Config::Co2SampleIntervalMs;
    Serial.printf("[CO2] Sensor ready, firmware=%u.%u, elapsed=%lu ms\n",
                  response_[3], response_[4],
                  static_cast<unsigned long>(elapsed));
    return;
  }

  if (completedRequest == RequestType::Measurement && value > 0) {
    const uint32_t scaledPpm = value * Config::Co2ScaleFactor;
    available_ = true;
    consecutiveErrors_ = 0;
    state.setCo2(scaledPpm, true, true);
    Serial.printf("[CO2] Reading OK: %lu ppm, elapsed=%lu ms\n",
                  static_cast<unsigned long>(scaledPpm),
                  static_cast<unsigned long>(elapsed));
    return;
  }

  handleRequestFailure(now, state, "invalid measurement");
}

void SenseAirS8Sensor::handleRequestFailure(uint32_t now, SystemState &state,
                                            const char *reason) {
  const RequestType failedRequest = requestType_;
  const uint32_t elapsed = now - requestStartTime_;
  requestType_ = RequestType::None;
  responseLength_ = 0;
  clearReceiveBuffer();

  if (failedRequest == RequestType::Firmware) {
    Serial.printf("[CO2] ERROR: connection %s, elapsed=%lu ms\n", reason,
                  static_cast<unsigned long>(elapsed));
    markUnavailable(now, state);
    return;
  }

  consecutiveErrors_++;
  state.setCo2(state.co2, true, false);
  Serial.printf("[CO2] ERROR: %s, elapsed=%lu ms, count=%u\n", reason,
                static_cast<unsigned long>(elapsed), consecutiveErrors_);
  if (consecutiveErrors_ >= Config::Co2MaximumConsecutiveErrors) {
    markUnavailable(now, state);
  }
}

bool SenseAirS8Sensor::responseIsValid() const {
  if (responseLength_ != ResponseLength || response_[0] != ModbusAddress ||
      response_[1] != ReadInputRegisters || response_[2] != 0x02) {
    return false;
  }

  const uint16_t expectedCrc =
      calculateModbusCrc(response_, ResponseLength - 2);
  const uint16_t receivedCrc =
      static_cast<uint16_t>(response_[5]) |
      (static_cast<uint16_t>(response_[6]) << 8);
  return receivedCrc == expectedCrc;
}

void SenseAirS8Sensor::clearReceiveBuffer() {
  while (serial_.available() > 0) {
    serial_.read();
  }
}

void SenseAirS8Sensor::markUnavailable(uint32_t now, SystemState &state) {
  Serial.println("[CO2] Sensor unavailable; UART will be reconnected later");
  available_ = false;
  consecutiveErrors_ = 0;
  lastConnectAttemptTime_ = now;
  requestType_ = RequestType::None;
  responseLength_ = 0;
  state.setCo2(state.co2, false, false);
  serial_.end();
}

uint16_t SenseAirS8Sensor::calculateModbusCrc(const uint8_t *data,
                                              size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x0001) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
  }
  return crc;
}
