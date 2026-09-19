#include "SensorSimulator.h"

#include "Config.h"

namespace {

// The simulator writes at the same cadence as the real drivers. This matters:
// every SystemState setter raises displayDirty, so writing on every loop
// iteration would repaint the display thousands of times per second.
constexpr uint32_t SampleIntervalMs = 1000;

constexpr uint32_t PhaseKnownValuesMs = 30000;   // fixed, predictable sample
constexpr uint32_t PhaseWarmUpMs = 70000;
constexpr uint32_t PhaseSteadyMs = 120000;
constexpr uint32_t PhaseCo2OfflineMs = 40000;
constexpr uint32_t PhaseCo2UntrustedMs = 30000;
constexpr uint32_t PhaseClimateFaultMs = 40000;
constexpr uint32_t PhaseRecoveryMs = 40000;
constexpr uint32_t PhaseHumidityFaultMs = 30000;

constexpr uint32_t kPhaseDurations[] = {
    PhaseKnownValuesMs, PhaseWarmUpMs,       PhaseSteadyMs,    PhaseCo2OfflineMs,
    PhaseCo2UntrustedMs, PhaseClimateFaultMs, PhaseRecoveryMs, PhaseHumidityFaultMs,
};
constexpr const char *kPhaseNames[] = {
    "KnownValues", "WarmUp",        "Steady",         "Co2Offline",
    "Co2Untrusted", "ClimateFault", "Recovery",       "HumidityFault",
};
constexpr size_t kPhaseCount = sizeof(kPhaseDurations) / sizeof(kPhaseDurations[0]);
constexpr uint32_t CycleMs = PhaseKnownValuesMs + PhaseWarmUpMs + PhaseSteadyMs +
                             PhaseCo2OfflineMs + PhaseCo2UntrustedMs +
                             PhaseClimateFaultMs + PhaseRecoveryMs +
                             PhaseHumidityFaultMs;

// Thermal model. Full duty raises about 3.3 C per minute and losses grow with
// the gap to ambient, so holding 37 C needs roughly 38% duty - enough movement
// for the PID to modulate and for heater_pwm to carry a real signal.
constexpr float AmbientCelsius = 24.0f;
constexpr float HeatRatePerSecondAtFullDuty = 0.055f;
constexpr float CoolRatePerSecondPerDegree = 0.0016f;

constexpr float HumidityCentrePercent = 55.0f;
constexpr float HumiditySwingPercent = 5.0f;
constexpr uint32_t HumidityPeriodMs = 300000;

// CO2 sweeps across the valve controller's thresholds (25 000 / 40 000 / 48 000
// in SystemState units) so every band is visited during one cycle. The valve
// pulse subtracts from whatever the sweep is doing, which makes the valve field
// and the CO2 curve corroborate each other.
//
// These are SystemState units, not confirmed physical units: see
// docs/REMOTE_ACCESS.md section 4.4.
constexpr float Co2SweepLow = 12000.0f;
constexpr float Co2SweepHigh = 46000.0f;
constexpr uint32_t Co2SweepPeriodMs = 180000;
constexpr float Co2PullPerSecond = 0.06f;
constexpr float Co2ValveDropPerSecond = 9000.0f;

constexpr float KnownTemperature = 37.02f;
constexpr float KnownHumidity = 55.30f;
constexpr float KnownCo2 = 51000.0f;

}  // namespace

void SensorSimulator::begin(SystemState &state) {
  Serial.println(
      "[SIM] WARNING: sensor values are synthetic - never run this build with "
      "heaters or valves connected");
  Serial.printf("[SIM] %u phases, cycle %lu ms, sample interval %lu ms\n",
                (unsigned)kPhaseCount, (unsigned long)CycleMs,
                (unsigned long)SampleIntervalMs);
  startMs_ = millis();
  lastSampleMs_ = startMs_;
  // Publish the first sample immediately. The network task can build a payload
  // before loop() has ever run, and an all-zero snapshot would be a bogus first
  // sample - visible in [env:sim], where payloads are printed even with no link.
  enterPhase(Phase::KnownValues, startMs_, state);
  phase_ = Phase::KnownValues;
}

void SensorSimulator::update(uint32_t now, SystemState &state) {
  const Phase phase = phaseFor(now - startMs_);
  if (phase != phase_) {
    enterPhase(phase, now, state);
    phase_ = phase;
  }

  if (now - lastSampleMs_ < SampleIntervalMs) {
    return;
  }
  // Clamped: a stalled loop must not integrate one huge step.
  const float dt = constrain((now - lastSampleMs_) / 1000.0f, 0.001f, 2.0f);
  lastSampleMs_ = now;

  switch (phase) {
    case Phase::KnownValues:
      state.setClimate(KnownTemperature, KnownHumidity, true);
      state.setCo2((uint32_t)KnownCo2, true, true);
      return;

    case Phase::ClimateFault:
      // Frozen on purpose: the values stay NaN until Recovery, and the PID has
      // already stopped driving the heater.
      return;

    case Phase::Co2Offline:
      stepThermalModel(state, dt);
      stepHumidity(now);
      state.setClimate(temperature_, humidity_, true);
      // Mirrors the real driver: keep the last reading, clear both flags.
      state.setCo2(state.co2, false, false);
      return;

    case Phase::Co2Untrusted:
      stepThermalModel(state, dt);
      stepHumidity(now);
      state.setClimate(temperature_, humidity_, true);
      state.setCo2(state.co2, true, false);
      return;

    case Phase::HumidityFault:
      stepThermalModel(state, dt);
      stepHumidity(now);
      stepCo2(now, state, dt);
      state.setClimate(temperature_, humidity_, false);
      state.setCo2((uint32_t)lroundf(co2_), true, true);
      return;

    case Phase::WarmUp:
    case Phase::Steady:
    case Phase::Recovery:
    default:
      stepThermalModel(state, dt);
      stepHumidity(now);
      stepCo2(now, state, dt);
      state.setClimate(temperature_, humidity_, true);
      state.setCo2((uint32_t)lroundf(co2_), true, true);
      return;
  }
}

SensorSimulator::Phase SensorSimulator::phaseFor(uint32_t elapsedMs) const {
  uint32_t offset = elapsedMs % CycleMs;
  for (size_t i = 0; i < kPhaseCount; ++i) {
    if (offset < kPhaseDurations[i]) {
      return static_cast<Phase>(i);
    }
    offset -= kPhaseDurations[i];
  }
  return static_cast<Phase>(0);
}

void SensorSimulator::enterPhase(Phase phase, uint32_t now, SystemState &state) {
  const size_t index = static_cast<size_t>(phase);
  Serial.printf("[SIM] phase -> %s\n", index < kPhaseCount ? kPhaseNames[index] : "?");
  (void)now;

  switch (phase) {
    case Phase::KnownValues:
      temperature_ = KnownTemperature;
      humidity_ = KnownHumidity;
      co2_ = KnownCo2;
      state.setClimate(temperature_, humidity_, true);
      state.setCo2((uint32_t)lroundf(co2_), true, true);
      break;

    case Phase::WarmUp:
      temperature_ = AmbientCelsius + 0.5f;
      humidity_ = 50.0f;
      co2_ = Co2SweepLow;
      break;

    case Phase::ClimateFault:
      // Both flags go false and both values become NaN, exactly like a failing
      // SHT30. The simulator keeps its own copy so Recovery can restore it.
      state.invalidateClimate();
      break;

    default:
      break;
  }
}

void SensorSimulator::stepThermalModel(const SystemState &state, float dt) {
  const float dutyFraction =
      (float)state.heaterDuty / (float)Config::HeaterPwmMaximum;
  const float heat = dutyFraction * HeatRatePerSecondAtFullDuty;
  const float loss = (temperature_ - AmbientCelsius) * CoolRatePerSecondPerDegree;
  temperature_ = constrain(temperature_ + (heat - loss) * dt + noise(0.02f),
                           -40.0f, 85.0f);
}

void SensorSimulator::stepHumidity(uint32_t now) {
  const float position =
      (float)(now % HumidityPeriodMs) / (float)HumidityPeriodMs;
  humidity_ = constrain(HumidityCentrePercent +
                            HumiditySwingPercent * sinf(2.0f * PI * position) +
                            noise(0.05f),
                        0.0f, 100.0f);
}

void SensorSimulator::stepCo2(uint32_t now, const SystemState &state, float dt) {
  const float position =
      (float)(now % Co2SweepPeriodMs) / (float)Co2SweepPeriodMs;
  const float sweep = 0.5f + 0.5f * sinf(2.0f * PI * position);
  const float target = Co2SweepLow + (Co2SweepHigh - Co2SweepLow) * sweep;

  co2_ += (target - co2_) * Co2PullPerSecond * dt;
  if (state.valveOpen) {
    co2_ -= Co2ValveDropPerSecond * dt;
  }
  co2_ = constrain(co2_, 0.0f, 1000000.0f);
}

// Deterministic on purpose: the same run produces the same numbers, so a
// captured log can be compared against a later one.
float SensorSimulator::noise(float amplitude) {
  rngState_ = rngState_ * 1664525u + 1013904223u;
  const float unit = (float)((rngState_ >> 8) & 0xFFFFu) / 65535.0f;
  return (unit * 2.0f - 1.0f) * amplitude;
}