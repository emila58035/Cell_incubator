#pragma once

#include <Arduino.h>

#include "SystemState.h"

class ValveController {
public:

   void begin(SystemState &state);
   void update(uint32_t now, SystemState &state);

private:
   void pulseValve(uint32_t now , uint32_t pulsetime);
   uint32_t lastUpdateTime=0;
   uint32_t lastOpenTime=0;
   bool valveOpen = false;
};
