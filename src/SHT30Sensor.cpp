#include "SHT30Sensor.h"

#include <Wire.h>

#include "Config.h"

namespace {

//  Translates the SHT85 library error codes into readable diagnostics.
const char *describeError(const int error) {
  switch (error) {
    case SHT_OK:
      return "none";
    case SHT_ERR_WRITECMD:
      return "I2C command write failed";
    case SHT_ERR_READBYTES:
      return "I2C data read failed";
    case SHT_ERR_NOT_CONNECT:
      return "no ACK on I2C bus";
    case SHT_ERR_CRC_TEMP:
      return "CRC error in temperature frame";
    case SHT_ERR_CRC_HUM:
      return "CRC error in humidity frame";
    case SHT_ERR_CRC_STATUS:
      return "CRC error in status frame";
    case SHT_ERR_SERIAL_NUMBER_CRC:
      return "CRC error in serial number";
    default:
      return "unknown error";
  }
}

//  Only transport level failures mean the sensor has left the bus; a CRC
//  mismatch is a corrupt frame from a sensor that is still present.
bool isTransportError(const int error) {
  return error == SHT_ERR_WRITECMD || error == SHT_ERR_READBYTES ||
         error == SHT_ERR_NOT_CONNECT;
}

}  // namespace

SHT30Sensor::SHT30Sensor() : sensor_(Config::ShtAddress, &Wire) {}

void SHT30Sensor::begin(SystemState &state) {
  Wire.begin(Config::ShtSdaPin, Config::ShtSclPin);
  connect(millis(), state);
}

void SHT30Sensor::update(uint32_t now, SystemState &state) {
  if (!available_) {
    if (now - lastConnectAttemptTime_ >= Config::ShtReconnectIntervalMs) {
      connect(now, state);
    }
    return;
  }

  if (now - lastSampleTime_ >= Config::ShtSampleIntervalMs) {
    sample(now, state);
  }
}

bool SHT30Sensor::connect(uint32_t now, SystemState &state) {
  lastConnectAttemptTime_ = now;
  //  begin() verifies the address and issues a soft reset, so it fails when
  //  the chip is absent or held low on the bus.
  available_ = sensor_.begin();

  if (!available_) {
    state.invalidateClimate();
    Serial.printf("[SHT30] ERROR: sensor not detected at 0x%02X (%s)\n",
                  Config::ShtAddress, describeError(sensor_.getError()));
    return false;
  }

  Serial.printf("[SHT30] Ready: SHT%u at 0x%02X, SDA=GPIO%d, SCL=GPIO%d\n",
                sensor_.getType(), sensor_.getAddress(), Config::ShtSdaPin,
                Config::ShtSclPin);
  sample(now, state);
  return true;
}

void SHT30Sensor::sample(uint32_t now, SystemState &state) {
  lastSampleTime_ = now;

  //  fast == false selects the "no clock stretching" measurement and enables
  //  the library's CRC-8 validation of both frames, which the incubator's
  //  control loop depends on. Blocks ~15 ms, well under the sample interval.
  if (!sensor_.read(false)) {
    const int error = sensor_.getError();
    state.invalidateClimate();

    if (isTransportError(error)) {
      available_ = false;
      lastConnectAttemptTime_ = now;
      Serial.printf("[SHT30] ERROR: sensor disconnected (%s)\n",
                    describeError(error));
    } else {
      Serial.printf("[SHT30] ERROR: measurement failed (%s)\n",
                    describeError(error));
    }
    return;
  }

  const float temperature = sensor_.getTemperature();
  if (!isfinite(temperature) || temperature < -40.0f ||
      temperature > 85.0f) {
    state.invalidateClimate();
    Serial.println("[SHT30] ERROR: invalid temperature");
    return;
  }

  const float humidity = sensor_.getHumidity();
  const bool humidityValid =
      isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;

  state.setClimate(temperature, humidity, humidityValid);
  Serial.printf("[SHT30] Temperature=%.2f C, Humidity=%.2f %%\n",
                temperature, humidity);

  if (!humidityValid) {
    Serial.println("[SHT30] ERROR: invalid humidity");
  }
}
