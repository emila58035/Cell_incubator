#include "AHT20Sensor.h"

#include <Wire.h>

#include "Config.h"

void AHT20Sensor::begin(SystemState &state) {
  Wire.begin(Config::AhtSdaPin, Config::AhtSclPin);
  connect(millis(), state);
}

void AHT20Sensor::update(uint32_t now, SystemState &state) {
  if (!available_) {
    if (now - lastConnectAttemptTime_ >= Config::AhtReconnectIntervalMs) {
      connect(now, state);
    }
    return;
  }

  if (now - lastSampleTime_ >= Config::AhtSampleIntervalMs) {
    sample(now, state);
  }
}

bool AHT20Sensor::connect(uint32_t now, SystemState &state) {
  lastConnectAttemptTime_ = now;
  available_ = sensor_.begin();

  if (!available_) {
    state.invalidateClimate();
    Serial.println("[AHT20] ERROR: sensor not detected");
    return false;
  }

  Serial.printf("[AHT20] Ready on SDA=GPIO%d, SCL=GPIO%d\n",
                Config::AhtSdaPin, Config::AhtSclPin);
  sample(now, state);
  return true;
}

void AHT20Sensor::sample(uint32_t now, SystemState &state) {
  lastSampleTime_ = now;

  Wire.beginTransmission(Config::AhtAddress);
  if (Wire.endTransmission() != 0) {
    available_ = false;
    lastConnectAttemptTime_ = now;
    state.invalidateClimate();
    Serial.println("[AHT20] ERROR: sensor disconnected");
    return;
  }

  const float temperature = sensor_.getTemperature();
  if (!isfinite(temperature) || temperature < -40.0f ||
      temperature > 85.0f) {
    state.invalidateClimate();
    Serial.println("[AHT20] ERROR: invalid temperature");
    return;
  }

  const float humidity = sensor_.getHumidity();
  const bool humidityValid =
      isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;

  state.setClimate(temperature, humidity, humidityValid);
  Serial.printf("[AHT20] Temperature=%.2f C, Humidity=%.2f %%\n",
                temperature, humidity);

  if (!humidityValid) {
    Serial.println("[AHT20] ERROR: invalid humidity");
  }
}
