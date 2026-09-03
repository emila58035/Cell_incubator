#pragma once

#include "SystemState.h"

class FanController {
 public:
  void begin(SystemState &state);
  void update(const SystemState &state);
};
