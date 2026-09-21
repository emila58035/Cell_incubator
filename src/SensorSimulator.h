#pragma once

#include <Arduino.h>

#include "SystemState.h"

// Synthetic sensor source for development on a board with no sensors attached.
//
// It stands in for SHT20Sensor and SenseAirS8Sensor and writes into SystemState
// through the very same setters they use, so every layer above stays real: the
// PID, the valve logic, the display and the telemetry payload are all produced
// by the production code. Only the sensor readings are fabricated.
//
// Two warnings, both deliberate:
//
//   * This build must never run with heaters attached. The synthetic
//     temperature makes the PID drive real PWM outputs and the valve driver
//     pulse a real valve.
//   * These numbers are not a plant model. They exist so that the telemetry
//     fields stay mutually consistent and the server-side alert paths all get
//     exercised. They say nothing about PID tuning, which still needs a real
//     SHT20 (docs/REMOTE_ACCESS.md section 7.3).
//
// The 8 phase scenario repeats every SIM_CYCLE_MS and injects every failure
// mode the payload can express, in the same domain as the real drivers:
// co2Available / co2Valid separately, and an invalidated climate block.
class SensorSimulator {
 public:
  void begin(SystemState &state);
  void update(uint32_t now, SystemState &state);

 private:
  enum class Phase : uint8_t {
    KnownValues,    // 30 s   fixed, predictable sample for a first end-to-end check
    WarmUp,         // 70 s   thermal model climbs from ambient to the setpoint
    Steady,         // 120 s  PID modulates, CO2 sweeps across the valve bands
    Co2Offline,     // 40 s   co2Available = false (sensor gone)
    Co2Untrusted,   // 30 s   co2Available = true but co2Valid = false
    ClimateFault,   // 40 s   invalidateClimate(): both flags false, values NaN
    Recovery,       // 40 s   climate valid again, so the alert should clear
    HumidityFault,  // 30 s   humidityValid = false only
    Count,
  };

  Phase phaseFor(uint32_t elapsedMs) const;
  void enterPhase(Phase phase, uint32_t now, SystemState &state);
  void stepThermalModel(const SystemState &state, float dtSeconds);
  void stepHumidity(uint32_t now);
  void stepCo2(uint32_t now, const SystemState &state, float dtSeconds);
  float noise(float amplitude);

  uint32_t startMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t rngState_ = 0x13579BDFu;
  Phase phase_ = Phase::Count;

  // The simulator owns its own copy of the readings, so it can keep modelling
  // while SystemState holds NaN during a climate fault.
  float temperature_ = 0.0f;
  float humidity_ = 0.0f;
  float co2_ = 0.0f;
};