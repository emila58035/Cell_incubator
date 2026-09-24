#include "HardwareSerial.h"
#include "ValveController.h"

#include "Config.h"

void ValveController::begin(SystemState &state) {
  pinMode(Config::ValvePin, OUTPUT);
  setValveOpen(state, false);
}

void ValveController::update(uint32_t now, SystemState &state) {
  // Manual switch off, or no trustworthy CO2 reading: the valve stays closed.
  if (!state.valveEnabled || !state.co2Available || !state.co2Valid) {
    setValveOpen(state, false);
    lastUpdateTime = now;
    return;
  }

  if (state.co2 < Config::ValveCO2threshold1) {
    if (now - lastUpdateTime < Config::ValveIntervalMs1) {
      return;
    }
    pulseValve(now, Config::ValveOpenDurationMs_long, state);
  } else if (state.co2 < Config::ValveCO2threshold2) {
    if (now - lastUpdateTime < Config::ValveIntervalMs2) {
      return;
    }
    pulseValve(now, Config::ValveOpenDurationMs_short, state);
  } else if (state.co2 < Config::ValveCO2threshold3) {
    if (now - lastUpdateTime < Config::ValveIntervalMs3) {
      return;
    }
    pulseValve(now, Config::ValveOpenDurationMs_short, state);
  } else {
    setValveOpen(state, false);
    lastUpdateTime = now;
  }
}

void ValveController::setValveOpen(SystemState &state, bool open) {
    digitalWrite(Config::ValvePin, open ? HIGH : LOW);
    if (valveOpen == open) {
        return;
    }

    valveOpen = open;
    state.valveOpen = open;
    state.markDisplayDirty();
}

void ValveController::pulseValve(uint32_t now, uint32_t pulseTime,
                                 SystemState &state) {
    if (!valveOpen) {
        setValveOpen(state, true);
        lastOpenTime = millis();
        Serial.println("Valve Open");
    } else if (millis() - lastOpenTime >= pulseTime) {
        setValveOpen(state, false);
        lastUpdateTime = now;
        Serial.println("Valve Close");
    }
}

