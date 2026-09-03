#include "HardwareSerial.h"
#include "LEDController.h"

#include "Config.h"

void LEDController::begin(SystemState &state) {
    state.LEDState = false;
    pinMode(Config::LEDOutputPin, OUTPUT);
    digitalWrite(Config::LEDOutputPin,
                 Config::LEDOutputActiveLow ? HIGH : LOW);
    Serial.printf("[LED] Ready on GPIO%d, startup=OFF\n",
                Config::LEDOutputPin);
}

void LEDController::update(SystemState &state) {
    digitalWrite(Config::LEDOutputPin,
               state.LEDState
                   ? (Config::LEDOutputActiveLow ? LOW : HIGH)
                   : (Config::LEDOutputActiveLow ? HIGH : LOW));
}
