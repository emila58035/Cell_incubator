#pragma once

#include <Arduino.h>

// Non-blocking WiFi station state machine.
//
// Every method runs on the network task (core 0). The control loop (core 1)
// only ever reads the volatile accessors, because the WiFi API takes internal
// locks and must not be called from the control loop.
class WiFiManager {
 public:
  void begin();
  void step(uint32_t now);

  bool linkUp() const { return linkUp_; }
  int16_t rssi() const { return rssi_; }

 private:
  enum class Phase : uint8_t {
    Unconfigured,
    Connecting,
    BackingOff,
  };

  void startAttempt(uint32_t now);
  void scheduleRetry(uint32_t now);

  Phase phase_ = Phase::Unconfigured;
  uint32_t deadlineMs_ = 0;
  uint32_t backoffMs_ = 0;

  volatile bool linkUp_ = false;
  volatile int16_t rssi_ = 0;
};