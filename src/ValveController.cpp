#include "HardwareSerial.h"
#include "ValveController.h"

#include "Config.h"

void ValveController::begin(SystemState &state) {
  pinMode(Config::ValvePin, OUTPUT);
  digitalWrite(Config::ValvePin, LOW);
}

void ValveController::update(uint32_t now, SystemState &state) {
  if(!state.co2Available || !state.co2Valid){
    digitalWrite(Config::ValvePin, LOW);
    valveOpen = false;
    lastUpdateTime = now;
  }
  if (state.co2Available && state.co2Valid) {
    if(state.co2 < Config::ValveCO2threshold1) {
        if(now - lastUpdateTime < Config::ValveIntervalMs1) {
          return;
        }
        else pulseValve(now, Config::ValveOpenDurationMs_long);
    } else if(state.co2 < Config::ValveCO2threshold2){
        if(now - lastUpdateTime < Config::ValveIntervalMs2) {
          return;
        }
        else pulseValve(now, Config::ValveOpenDurationMs_short);
    } else if(state.co2 < Config::ValveCO2threshold3){
        if(now - lastUpdateTime < Config::ValveIntervalMs3) {
          return;
        }
        else pulseValve(now, Config::ValveOpenDurationMs_short);
    } else {
        digitalWrite(Config::ValvePin, LOW);
        valveOpen = false;
        lastUpdateTime = now;
    }
  }
}

void ValveController::pulseValve(uint32_t now, uint32_t pulsetime) {
    if (!valveOpen) {
        digitalWrite(Config::ValvePin, HIGH);
        valveOpen = true;
        lastOpenTime = millis();
        Serial.println("Valve Open");
    } else if (millis() - lastOpenTime >= pulsetime) {
        digitalWrite(Config::ValvePin, LOW);
        valveOpen = false;
        lastUpdateTime = now;
        Serial.println("Valve Close");
    }
}

