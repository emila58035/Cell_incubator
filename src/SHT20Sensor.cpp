#include "SHT20Sensor.h"

#include <Wire.h>

#include "Config.h"

namespace {

//  Translates the SHT2x library error codes into readable diagnostics.
const char *describeError(const int error) {
  switch (error) {
    case SHT2x_OK:
      return "none";
    case SHT2x_ERR_WRITECMD:
      return "I2C command write failed";
    case SHT2x_ERR_READBYTES:
      return "I2C data read failed";
    case SHT2x_ERR_NOT_CONNECT:
      return "no ACK on I2C bus";
    case SHT2x_ERR_CRC_TEMP:
      return "CRC error in temperature frame";
    case SHT2x_ERR_CRC_HUM:
      return "CRC error in humidity frame";
    case SHT2x_ERR_CRC_STATUS:
      return "CRC error in status frame";
    case SHT2x_ERR_RESOLUTION:
      return "invalid resolution requested";
    case SHT2x_ERR_HEATER_ON:
      return "heater could not be enabled";
    case SHT2x_ERR_HEATER_OFF:
      return "heater could not be disabled";
    case SHT2x_ERR_HEATER_COOLDOWN:
      return "heater still cooling down";
    default:
      return "unknown error";
  }
}

//  Only transport level failures mean the sensor has left the bus.
bool isTransportError(const int error) {
  return error == SHT2x_ERR_WRITECMD || error == SHT2x_ERR_READBYTES ||
         error == SHT2x_ERR_NOT_CONNECT;
}

}  // namespace

void SHT20Sensor::begin(SystemState &state) {
  Wire.begin(Config::ShtSdaPin, Config::ShtSclPin);
  connect(millis(), state);
}

void SHT20Sensor::update(uint32_t now, SystemState &state) {
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

bool SHT20Sensor::connect(uint32_t now, SystemState &state) {
  lastConnectAttemptTime_ = now;
  //  begin() probes the bus and then issues a soft reset, so it fails when the
  //  chip is absent or held low on the bus.
  available_ = sensor_.begin();

  if (!available_) {
    state.invalidateClimate();
    Serial.printf("[SHT20] ERROR: sensor not detected (%s)\n",
                  describeError(sensor_.getError()));
    return false;
  }

  Serial.printf("[SHT20] Ready on SDA=GPIO%d, SCL=GPIO%d\n",
                Config::ShtSdaPin, Config::ShtSclPin);
  sample(now, state);
  return true;
}

void SHT20Sensor::sample(uint32_t now, SystemState &state) {
  lastSampleTime_ = now;

  //  read() drives a full conversion of both channels and blocks for up to
  //  ~114 ms at the default 14-bit / 12-bit resolution. It reports I2C level
  //  failures through its return value.
  const bool readOk = sensor_.read();
  //  getError() clears the flag, so it is fetched exactly once.
  //
  //  In SHT2x 0.5.5 this is a forward-compatible guard rather than an active
  //  check: readTemperature() and readHumidity() each overwrite a CRC mismatch
  //  with SHT2x_OK before returning (see the commented-out `return false` in
  //  SHT2x.cpp), so a successful read() always reports SHT2x_OK and checksum
  //  failures are not observable through the synchronous interface. Transport
  //  failures do survive, because they return early past that assignment.
  const int error = sensor_.getError();

  if (!readOk || error != SHT2x_OK) {
    state.invalidateClimate();

    if (isTransportError(error)) {
      available_ = false;
      lastConnectAttemptTime_ = now;
      Serial.printf("[SHT20] ERROR: sensor disconnected (%s)\n",
                    describeError(error));
    } else if (error != SHT2x_OK) {
      Serial.printf("[SHT20] ERROR: rejected reading (%s)\n",
                    describeError(error));
    } else {
      Serial.println("[SHT20] ERROR: measurement failed");
    }
    return;
  }

  const float temperature = sensor_.getTemperature();
  if (!isfinite(temperature) || temperature < -40.0f ||
      temperature > 85.0f) {
    state.invalidateClimate();
    Serial.println("[SHT20] ERROR: invalid temperature");
    return;
  }

  const float humidity = sensor_.getHumidity();
  const bool humidityValid =
      isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;

  state.setClimate(temperature, humidity, humidityValid);
  Serial.printf("[SHT20] Temperature=%.2f C, Humidity=%.2f %%\n",
                temperature, humidity);

  if (!humidityValid) {
    Serial.println("[SHT20] ERROR: invalid humidity");
  }
}