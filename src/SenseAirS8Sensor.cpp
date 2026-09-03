#include "SenseAirS8Sensor.h"

#include <cstring>

#include "Config.h"

SenseAirS8Sensor::SenseAirS8Sensor()
    : serial_(Config::S8UartPort), sensor_(serial_) {}

void SenseAirS8Sensor::begin(SystemState &state) {
  Serial.println("[CO2] Initializing SenseAir S8...");
  Serial.printf("[CO2] UART%d, baud=%d, RX=GPIO%d, TX=GPIO%d\n",
                Config::S8UartPort, S8_BAUDRATE, Config::S8RxPin,
                Config::S8TxPin);
  startUart();
  connect(millis(), state);
}

void SenseAirS8Sensor::update(uint32_t now, SystemState &state) {
  if (!available_) {
    if (now - lastConnectAttemptTime_ >= Config::Co2ReconnectIntervalMs) {
      Serial.println("[CO2] Attempting automatic reconnect...");
      serial_.end();
      delay(20);
      startUart();
      connect(now, state);
    }
    return;
  }

  if (now - lastSampleTime_ >= Config::Co2SampleIntervalMs) {
    sample(now, state);
  }
}

void SenseAirS8Sensor::startUart() {
  serial_.begin(S8_BAUDRATE, SERIAL_8N1, Config::S8RxPin, Config::S8TxPin);
  clearReceiveBuffer();
}

bool SenseAirS8Sensor::connect(uint32_t now, SystemState &state) {
  lastConnectAttemptTime_ = now;
  clearReceiveBuffer();
  memset(info_.firm_version, 0, sizeof(info_.firm_version));
  sensor_.get_firmware_version(info_.firm_version);

  available_ = strlen(info_.firm_version) > 0;
  consecutiveErrors_ = 0;
  state.setCo2(state.co2, available_, false);

  if (!available_) {
    Serial.println("[CO2] ERROR: SenseAir S8 not detected");
    return false;
  }

  Serial.printf("[CO2] Sensor ready, firmware=%s\n", info_.firm_version);
  Serial.printf("[CO2] Sampling interval=%lu ms\n",
                (unsigned long)Config::Co2SampleIntervalMs);
  lastSampleTime_ = now - Config::Co2SampleIntervalMs;
  return true;
}

void SenseAirS8Sensor::sample(uint32_t now, SystemState &state) {
  lastSampleTime_ = now;
  clearReceiveBuffer();

  Serial.printf("[CO2] Requesting measurement at %lu ms...\n",
                (unsigned long)now);
  const uint32_t startedAt = millis();
  const int16_t rawPpm = sensor_.get_co2();
  const uint32_t elapsed = millis() - startedAt;

  if (rawPpm > 0) {
    const uint32_t scaledPpm =
        (uint32_t)rawPpm * Config::Co2ScaleFactor;
    consecutiveErrors_ = 0;
    state.setCo2(scaledPpm, true, true);
    Serial.printf("[CO2] Reading OK: %lu ppm, elapsed=%lu ms\n",
                  (unsigned long)scaledPpm, (unsigned long)elapsed);
    return;
  }

  consecutiveErrors_++;
  state.setCo2(state.co2, true, false);
  Serial.printf("[CO2] ERROR: read failed, elapsed=%lu ms, count=%u\n",
                (unsigned long)elapsed, consecutiveErrors_);

  if (consecutiveErrors_ >= Config::Co2MaximumConsecutiveErrors) {
    markUnavailable(now, state);
  }
}

void SenseAirS8Sensor::clearReceiveBuffer() {
  while (serial_.available() > 0) {
    serial_.read();
  }
}

void SenseAirS8Sensor::markUnavailable(uint32_t now, SystemState &state) {
  Serial.println("[CO2] Too many errors; UART will be reconnected");
  available_ = false;
  consecutiveErrors_ = 0;
  lastConnectAttemptTime_ = now;
  state.setCo2(state.co2, false, false);
  serial_.end();
}
