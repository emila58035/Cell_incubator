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


  attachInterrupt(digitalPinToInterrupt(Config::EncoderAPin), encoderIsr,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(Config::EncoderBPin), encoderIsr,
                  CHANGE);
  attachInterrupt(digitalPinToInterrupt(Config::EncoderPushPin), pushIsr,
                  FALLING);
  attachInterrupt(digitalPinToInterrupt(Config::KOPIN), KOIsr, FALLING);

  Serial.printf("[INPUT] Encoder A=GPIO%d, B=GPIO%d, PUSH=GPIO%d, KO=GPIO%d\n",
                Config::EncoderAPin, Config::EncoderBPin,
                Config::EncoderPushPin, Config::KOPIN);
}

void InputManager::update(uint32_t now, SystemState &state) {
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

  if (steps != 0 && state.page == UiPage::Status) {
    state.setpoint += steps * Config::EncoderTemperatureStep;
    state.setpoint = round(state.setpoint * 10.0) / 10.0;
    state.setpoint = constrain(state.setpoint, Config::MinimumSetpoint,
                               Config::MaximumSetpoint);
    state.markDisplayDirty();
    Serial.printf("[INPUT] Set temperature=%.1f C\n", state.setpoint);
  } else if (steps != 0 && state.settingsItemSelected) {
    if (state.settingsItem == SettingsItem::Light) {
      const int brightness = state.lightBrightness +
                             steps * Config::LightBrightnessStep;
      state.lightBrightness = constrain(brightness, 0, 100);
      state.markDisplayDirty();
      Serial.printf("[INPUT] Light brightness=%u%%\n",
                    state.lightBrightness);
    }
  } else if (steps != 0) {
    const int itemCount = static_cast<int>(SettingsItem::Count);
    const int currentItem = static_cast<int>(state.settingsItem);
    const int nextItem =
        (currentItem + (steps % itemCount) + itemCount) % itemCount;
    state.settingsItem = static_cast<SettingsItem>(nextItem);
    state.markDisplayDirty();
  }

  if (pushPressed && now - lastPushTime_ >= Config::PushDebounceMs) {
    lastPushTime_ = now;
    if (state.page == UiPage::Status) {
      if (!state.temperatureValid && !state.heatingEnabled) {
        Serial.println("[INPUT] Heating rejected: temperature is invalid");
      } else {
        state.heatingEnabled = !state.heatingEnabled;
        state.markDisplayDirty();
        Serial.printf("[INPUT] Heating=%s\n",
                      state.heatingEnabled ? "ON" : "OFF");
      }
    } else {
      state.settingsItemSelected = !state.settingsItemSelected;
      state.markDisplayDirty();
      Serial.printf("[INPUT] Settings item %s\n",
                    state.settingsItemSelected ? "selected" : "released");
    }
  }

  if (KOPressed && now - lastKOTime_ >= Config::PushDebounceMs) {
    lastKOTime_ = now;
    if (state.page == UiPage::Status) {
      state.page = UiPage::Settings;
      state.settingsItemSelected = false;
      state.markDisplayDirty();
      Serial.println("[INPUT] Settings page opened");
    } else if (!state.settingsItemSelected) {
      state.page = UiPage::Status;
      state.markDisplayDirty();
      Serial.println("[INPUT] Status page opened");
    } else {
      switch (state.settingsItem) {
        case SettingsItem::Light:
          state.lightEnabled = !state.lightEnabled;
          Serial.printf("[INPUT] Light=%s\n",
                        state.lightEnabled ? "ON" : "OFF");
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

void IRAM_ATTR InputManager::pushIsr() {
  if (instance_ == nullptr) {
    return;
  }

  portENTER_CRITICAL_ISR(&instance_->mux_);
  instance_->pushPressedEvent_ = true;
  portEXIT_CRITICAL_ISR(&instance_->mux_);
}

void IRAM_ATTR InputManager::KOIsr() {
  if (instance_ == nullptr) {
    return;
  }

  portENTER_CRITICAL_ISR(&instance_->mux_);
  instance_->KOPressedEvent_ = true;
  portEXIT_CRITICAL_ISR(&instance_->mux_);
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
