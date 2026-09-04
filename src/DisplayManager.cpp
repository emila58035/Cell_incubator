#include "DisplayManager.h"

#include "Config.h"

namespace {

const char *onOff(bool enabled) { return enabled ? "ON" : "OFF"; }

uint16_t selectionColor(const SystemState &state) {
  return state.settingsItemSelected ? TFT_YELLOW : TFT_CYAN;
}

constexpr uint16_t ScreenColor = 0x10A2;
constexpr uint16_t CardColor = 0x2104;
constexpr uint16_t MutedColor = 0x8410;

void formatTemperature(char *buffer, size_t size, const SystemState &state) {
  if (state.temperatureValid) {
    snprintf(buffer, size, "%.1f", state.temperature);
  } else {
    snprintf(buffer, size, "--");
  }
}

void formatHumidity(char *buffer, size_t size, const SystemState &state) {
  if (state.humidityValid) {
    snprintf(buffer, size, "%.1f", state.humidity);
  } else {
    snprintf(buffer, size, "--");
  }
}

void formatCo2(char *buffer, size_t size, const SystemState &state) {
  if (state.co2Valid) {
    snprintf(buffer, size, "%lu", static_cast<unsigned long>(state.co2));
  } else {
    snprintf(buffer, size, "--");
  }
}

void formatLight(char *buffer, size_t size, const SystemState &state) {
  if (state.lightEnabled) {
    snprintf(buffer, size, "%u%%", state.lightBrightness);
  } else {
    snprintf(buffer, size, "OFF");
  }
}

void formatSetTemperature(char *buffer, size_t size, const SystemState &state) {
  snprintf(buffer, size, "%.1f C", state.setpoint);
}

uint8_t heatingPowerPercent(const SystemState &state) {
  if (!state.heatingEnabled) {
    return 0;
  }

  const double power = state.heaterOutput / Config::HeaterPwmMaximum * 100.0;
  return static_cast<uint8_t>(constrain(round(power), 0.0, 100.0));
}

// The same layout is used for the sprite and the fallback direct renderer.
template <typename Display>
void renderLayout(Display &display, const SystemState &state, int originX,
                  int originY) {
  constexpr int TempX = 8;
  constexpr int HumidityX = 107;
  constexpr int Co2X = 206;
  constexpr int SensorY = 5;
  constexpr int SensorWidth = 98;
  constexpr int SensorHeight = 99;
  constexpr int SeparatorY = 112;
  constexpr int ControlLabelY = 124;
  constexpr int ControlRow1Y = 130;
  constexpr int ControlRow2Y = 165;
  constexpr int ControlWidth = 88;
  constexpr int ControlHeight = 30;
  constexpr int FooterY = 211;

  const auto x = [originX](int value) { return originX + value; };
  const auto y = [originY](int value) { return originY + value; };

  display.fillRect(originX, originY, Config::DynamicAreaWidth,
                   Config::DynamicAreaHeight, ScreenColor);

  const auto drawMetric = [&](int cardX, const char *label, const char *value,
                              const char *unit, uint16_t borderColor,
                              uint16_t statusColor, const char *statusText,
                              int valueFont) {
    display.fillRoundRect(x(cardX), y(SensorY), SensorWidth, SensorHeight, 4,
                          CardColor);
    display.drawRoundRect(x(cardX), y(SensorY), SensorWidth, SensorHeight, 4,
                          borderColor);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_LIGHTGREY, CardColor);
    display.drawString(label, x(cardX + 7), y(SensorY + 13), 2);

    // Keep the value left aligned and the unit at a stable right edge.
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_WHITE, CardColor);
    display.drawString(value, x(cardX + 7), y(SensorY + 59), valueFont);
    display.setTextDatum(MR_DATUM);
    display.setTextColor(TFT_LIGHTGREY, CardColor);
    display.drawString(unit, x(cardX + SensorWidth - 7), y(SensorY + 59), 2);

    display.setTextDatum(ML_DATUM);
    display.setTextColor(statusColor, CardColor);
    display.drawString(statusText, x(cardX + 7), y(SensorY + 87), 1);
    display.fillCircle(x(cardX + SensorWidth - 9),
                       y(SensorY + SensorHeight - 9), 3, statusColor);
  };

  char valueText[24];
  formatTemperature(valueText, sizeof(valueText), state);
  drawMetric(TempX, "TEMP", valueText, "C", TFT_ORANGE,
             state.temperatureValid ? TFT_GREEN : TFT_ORANGE,
             state.temperatureValid ? "OK" : "ERROR", 4);
  formatHumidity(valueText, sizeof(valueText), state);
  drawMetric(HumidityX, "HUMIDITY", valueText, "%", TFT_CYAN,
             state.humidityValid ? TFT_GREEN : TFT_ORANGE,
             state.humidityValid ? "OK" : "ERROR", 4);
  formatCo2(valueText, sizeof(valueText), state);
  drawMetric(Co2X, "CO2", valueText, "ppm", TFT_YELLOW,
             state.co2Valid ? TFT_GREEN : TFT_ORANGE,
             state.co2Valid ? "OK" : "ERROR", 2);

  display.drawFastHLine(x(8), y(SeparatorY), Config::DynamicAreaWidth - 16,
                        MutedColor);
  display.setTextDatum(ML_DATUM);
  display.setTextColor(MutedColor, ScreenColor);
  display.drawString("CONTROL", x(8), y(ControlLabelY), 1);

  const auto drawControl = [&](SettingsItem item, int cardX, int cardY,
                               const char *label, const char *value,
                               uint16_t valueColor, bool active) {
    const bool current = state.settingsItem == item;
    const uint16_t borderColor = current ? selectionColor(state) : MutedColor;
    display.fillRoundRect(x(cardX), y(cardY), ControlWidth, ControlHeight, 3,
                          CardColor);
    display.drawRoundRect(x(cardX), y(cardY), ControlWidth, ControlHeight, 3,
                          borderColor);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_LIGHTGREY, CardColor);
    display.drawString(label, x(cardX + 7), y(cardY + 8), 1);
    display.setTextColor(valueColor, CardColor);
    display.drawString(value, x(cardX + 7), y(cardY + 21), 2);
    display.fillCircle(x(cardX + ControlWidth - 9), y(cardY + 15), 4,
                       active ? valueColor : MutedColor);
  };

  formatSetTemperature(valueText, sizeof(valueText), state);
  drawControl(SettingsItem::SetTemperature, 8, ControlRow1Y, "SET TEMP",
               valueText, TFT_WHITE, state.heatingEnabled);
  formatLight(valueText, sizeof(valueText), state);
  drawControl(SettingsItem::Light, 108, ControlRow1Y, "LIGHT", valueText,
              state.lightEnabled ? TFT_YELLOW : TFT_WHITE, state.lightEnabled);
  const uint8_t lightBrightness = state.lightEnabled ? state.lightBrightness : 0;
  display.fillRoundRect(x(154), y(ControlRow1Y + 22), 27, 4, 2, MutedColor);
  display.fillRoundRect(x(154), y(ControlRow1Y + 22),
                        lightBrightness * 27 / 100, 4, 2, TFT_YELLOW);
  drawControl(SettingsItem::Uv, 208, ControlRow1Y, "UV",
              onOff(state.uvEnabled), state.uvEnabled ? TFT_YELLOW : TFT_WHITE,
              state.uvEnabled);
  drawControl(SettingsItem::Fan, 8, ControlRow2Y, "FAN", onOff(state.fanEnabled),
              state.fanEnabled ? TFT_GREEN : TFT_WHITE, state.fanEnabled);

  const uint8_t power = heatingPowerPercent(state);
  char powerText[8];
  snprintf(powerText, sizeof(powerText), "%u%%", power);
  drawControl(SettingsItem::Count, 108, ControlRow2Y, "HEAT POWER", powerText,
              state.heatingEnabled ? TFT_ORANGE : TFT_WHITE,
              state.heatingEnabled);
  display.fillRoundRect(x(154), y(ControlRow2Y + 22), 27, 4, 2, MutedColor);
  display.fillRoundRect(x(154), y(ControlRow2Y + 22), power * 27 / 100, 4, 2,
                        TFT_ORANGE);

  drawControl(SettingsItem::Count, 208, ControlRow2Y, "VALVE",
              onOff(state.valveOpen), state.valveOpen ? TFT_GREEN : TFT_WHITE,
              state.valveOpen);

  display.setTextDatum(ML_DATUM);
  display.setTextColor(MutedColor, ScreenColor);
  display.drawString("KO: SELECT", x(8), y(FooterY), 1);
  display.setTextDatum(MR_DATUM);
  display.drawString("PUSH: TOGGLE", x(Config::DynamicAreaWidth - 8),
                     y(FooterY), 1);
}

}  // namespace

DisplayManager::DisplayManager() : sprite_(&tft_) {}

void DisplayManager::begin(SystemState &state) {
  pinMode(Config::TftBacklightPin, OUTPUT);
  digitalWrite(Config::TftBacklightPin, HIGH);
  pinMode(Config::TftRESPin, OUTPUT);
  digitalWrite(Config::TftRESPin, HIGH);
  delay(100);

  tft_.init();
  tft_.invertDisplay(false);
  tft_.setRotation(1);
  tft_.fillScreen(TFT_BLACK);

  sprite_.setColorDepth(16);
  spriteReady_ = sprite_.createSprite(Config::DynamicAreaWidth,
                                      Config::DynamicAreaHeight) != nullptr;
  Serial.printf("[DISPLAY] Size=%d x %d, sprite=%s\n", tft_.width(),
                tft_.height(), spriteReady_ ? "READY" : "FAILED");

  state.markDisplayDirty();
  update(state);
}

void DisplayManager::update(SystemState &state) {
  if (!state.displayDirty) {
    return;
  }

  if (spriteReady_) {
    renderSprite(state);
  } else {
    renderDirect(state);
  }
  state.displayDirty = false;
}

void DisplayManager::renderSprite(const SystemState &state) {
  sprite_.fillSprite(ScreenColor);
  renderLayout(sprite_, state, 0, 0);
  sprite_.pushSprite(Config::DynamicAreaX, Config::DynamicAreaY);
}

void DisplayManager::renderDirect(const SystemState &state) {
  renderLayout(tft_, state, Config::DynamicAreaX, Config::DynamicAreaY);
}
