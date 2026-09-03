#pragma once

#include "SystemState.h"

class UVController {
 public:
  void begin(SystemState &state);
  void update(const SystemState &state);
};
