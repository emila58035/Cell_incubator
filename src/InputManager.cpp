#include "InputManager.h"

#include "Config.h"

InputManager *InputManager::instance_ = nullptr;

void InputManager::begin() {
  instance_ = this;

  pinMode(Config::EncoderAPin, INPUT_PULLUP);
  pinMode(Config::EncoderBPin, INPUT_PULLUP);
  pinMode(Config::EncoderPushPin, INPUT_PULLUP);
  pinMode(Config::KOPIN, INPUT_PULLUP);

  encoderState_ = (digitalRead(Config::EncoderAPin) << 1) |
                  digitalRead(Config::EncoderBPin);
  pushRawPressed_ = digitalRead(Config::EncoderPushPin) == LOW;
  pushStablePressed_ = pushRawPressed_;
  KORawPressed_ = digitalRead(Config::KOPIN) == LOW;
  KOStablePressed_ = KORawPressed_;
  pushLastChangeTime_ = millis();
  KOLastChangeTime_ = pushLastChangeTime_;

  attachInterrupt(digitalPinToInterrupt(Config::EncoderAPin), encoderIsr,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(Config::EncoderBPin), encoderIsr,
                  CHANGE);

  Serial.printf("[INPUT] Encoder A=GPIO%d, B=GPIO%d, PUSH=GPIO%d, KO=GPIO%d\n",
                Config::EncoderAPin, Config::EncoderBPin,
                Config::EncoderPushPin, Config::KOPIN);
}

void InputManager::update(uint32_t now, SystemState &state) {
  updateButton(now, Config::EncoderPushPin, pushRawPressed_,
               pushStablePressed_, pushLastChangeTime_, pushPressedEvent_);
  updateButton(now, Config::KOPIN, KORawPressed_, KOStablePressed_,
               KOLastChangeTime_, KOPressedEvent_);

  int32_t steps;
  bool pushPressed;
  bool KOPressed;

  portENTER_CRITICAL(&mux_);
  steps = encoderSteps_;
  encoderSteps_ = 0;
  pushPressed = pushPressedEvent_;
  pushPressedEvent_ = false;
  KOPressed = KOPressedEvent_;
  KOPressedEvent_ = false;
  portEXIT_CRITICAL(&mux_);

  if (steps != 0 && state.settingsItemSelected) {
    switch (state.settingsItem) {
      case SettingsItem::SetTemperature:
        state.setpoint += steps * Config::EncoderTemperatureStep;
        state.setpoint = round(state.setpoint * 10.0) / 10.0;
        state.setpoint = constrain(state.setpoint, Config::MinimumSetpoint,
                                   Config::MaximumSetpoint);
        state.markDisplayDirty();
        Serial.printf("[INPUT] Set temperature=%.1f C\n", state.setpoint);
        break;
      case SettingsItem::Light: {
        const int startingBrightness = state.lightEnabled
                                           ? state.lightBrightness
                                           : 0;
        const int brightness = constrain(
            startingBrightness + steps * Config::LightBrightnessStep, 0, 100);
        state.lightBrightness = brightness;
        state.lightEnabled = brightness > 0;
        state.markDisplayDirty();
        Serial.printf("[INPUT] Light brightness=%u%%\n",
                      state.lightBrightness);
        break;
      }
      case SettingsItem::Uv:
      case SettingsItem::Fan:
      case SettingsItem::Count:
        break;
    }
  } else if (steps != 0) {
    const int itemCount = static_cast<int>(SettingsItem::Count);
    const int currentItem = static_cast<int>(state.settingsItem);
    const int nextItem =
        (currentItem + (steps % itemCount) + itemCount) % itemCount;
    state.settingsItem = static_cast<SettingsItem>(nextItem);
    state.markDisplayDirty();
  }

  if (KOPressed) {
    state.settingsItemSelected = !state.settingsItemSelected;
    state.markDisplayDirty();
    Serial.printf("[INPUT] Item %s\n",
                  state.settingsItemSelected ? "selected" : "released");
  }

  if (pushPressed) {
    if (state.settingsItemSelected) {
      switch (state.settingsItem) {
        case SettingsItem::SetTemperature:
          if (!state.temperatureValid && !state.heatingEnabled) {
            Serial.println("[INPUT] Heating rejected: temperature is invalid");
          } else {
            state.heatingEnabled = !state.heatingEnabled;
            Serial.printf("[INPUT] Heating=%s\n",
                          state.heatingEnabled ? "ON" : "OFF");
          }
          break;
        case SettingsItem::Light:
          if (state.lightEnabled) {
            state.lightEnabled = false;
            state.lightBrightness = 0;
          } else {
            state.lightEnabled = true;
            state.lightBrightness = 100;
          }
          Serial.printf("[INPUT] Light=%s, brightness=%u%%\n",
                        state.lightEnabled ? "ON" : "OFF",
                        state.lightBrightness);
          break;
        case SettingsItem::Uv:
          state.uvEnabled = !state.uvEnabled;
          Serial.printf("[INPUT] UV=%s\n", state.uvEnabled ? "ON" : "OFF");
          break;
        case SettingsItem::Fan:
          state.fanEnabled = !state.fanEnabled;
          Serial.printf("[INPUT] Fan=%s\n",
                        state.fanEnabled ? "ON" : "OFF");
          break;
        case SettingsItem::Count:
          break;
      }
      state.markDisplayDirty();
    }
  }
}

void IRAM_ATTR InputManager::encoderIsr() {
  if (instance_ != nullptr) {
    instance_->captureEncoderTransition();
  }
}

void IRAM_ATTR InputManager::captureEncoderTransition() {
  static const int8_t transitionTable[16] = {
      0, -1, 1, 0, 1, 0, 0, -1,
      -1, 0, 0, 1, 0, 1, -1, 0};

  const uint8_t currentState =
      (digitalRead(Config::EncoderAPin) << 1) |
      digitalRead(Config::EncoderBPin);
  const uint8_t transition =
      ((encoderState_ << 2) | currentState) & 0x0F;
  encoderState_ = currentState;

  const int8_t movement = transitionTable[transition];
  if (movement == 0) {
    return;
  }

  portENTER_CRITICAL_ISR(&mux_);
  encoderAccumulator_ += movement;
  if (encoderAccumulator_ >= 4) {
    encoderSteps_++;
    encoderAccumulator_ = 0;
  } else if (encoderAccumulator_ <= -4) {
    encoderSteps_--;
    encoderAccumulator_ = 0;
  }
  portEXIT_CRITICAL_ISR(&mux_);
}

void InputManager::updateButton(uint32_t now, int pin, bool &rawPressed,
                                bool &stablePressed,
                                uint32_t &lastChangeTime,
                                bool &pressedEvent) {
  const bool sampledPressed = digitalRead(pin) == LOW;
  if (sampledPressed != rawPressed) {
    rawPressed = sampledPressed;
    lastChangeTime = now;
  }

  if (rawPressed == stablePressed ||
      now - lastChangeTime < Config::PushDebounceMs) {
    return;
  }

  stablePressed = rawPressed;
  if (stablePressed) {
    pressedEvent = true;
  }
}
