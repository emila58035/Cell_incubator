#include "DisplayManager.h"

#include "Config.h"

namespace {

const char *onOff(bool enabled) { return enabled ? "ON" : "OFF"; }

uint16_t selectionColor(const SystemState &state) {
  return state.settingsItemSelected ? TFT_YELLOW : TFT_CYAN;
}

void formatTemperature(char *buffer, size_t size, const SystemState &state) {
  if (state.temperatureValid) {
    snprintf(buffer, size, "%.1f C", state.temperature);
  } else {
    snprintf(buffer, size, "--");
  }
}

void formatHumidity(char *buffer, size_t size, const SystemState &state) {
  if (state.humidityValid) {
    snprintf(buffer, size, "%.1f %%", state.humidity);
  } else {
    snprintf(buffer, size, "--");
  }
}

void formatCo2(char *buffer, size_t size, const SystemState &state) {
  if (state.co2Valid) {
    snprintf(buffer, size, "%lu ppm", static_cast<unsigned long>(state.co2));
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
  snprintf(buffer, size, "%.1f C  %s", state.setpoint,
           onOff(state.heatingEnabled));
}

void formatPower(char *buffer, size_t size, const SystemState &state) {
  const double power = state.heatingEnabled
                           ? state.heaterOutput / Config::HeaterPwmMaximum * 100.0
                           : 0.0;
  snprintf(buffer, size, "HEATER POWER %.0f%%", power);
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
  constexpr int LabelX = 12;
  constexpr int ValueX = Config::DynamicAreaWidth - 12;
  constexpr int SensorRow1Y = 13;
  constexpr int SensorRow2Y = 35;
  constexpr int SensorRow3Y = 57;
  constexpr int SeparatorY = 70;
  constexpr int RowX = 7;
  constexpr int RowWidth = Config::DynamicAreaWidth - RowX * 2;
  constexpr int RowHeight = 26;
  constexpr int SetTemperatureY = 78;
  constexpr int LightY = 107;
  constexpr int UvY = 136;
  constexpr int FanY = 165;
  constexpr int FooterY = 207;

  sprite_.fillSprite(TFT_BLACK);
  sprite_.setTextSize(1);
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString("TEMP", LabelX, SensorRow1Y, 2);
  sprite_.drawString("HUMIDITY", LabelX, SensorRow2Y, 2);
  sprite_.drawString("CO2", LabelX, SensorRow3Y, 2);

  char valueText[24];
  sprite_.setTextDatum(MR_DATUM);
  formatTemperature(valueText, sizeof(valueText), state);
  sprite_.drawString(valueText, ValueX, SensorRow1Y, 2);
  formatHumidity(valueText, sizeof(valueText), state);
  sprite_.drawString(valueText, ValueX, SensorRow2Y, 2);
  formatCo2(valueText, sizeof(valueText), state);
  sprite_.drawString(valueText, ValueX, SensorRow3Y, 2);
  sprite_.drawFastHLine(0, SeparatorY, Config::DynamicAreaWidth, TFT_DARKGREY);

  const auto drawRow = [&](SettingsItem item, int y, const char *label,
                           const char *value) {
    if (state.settingsItem == item) {
      sprite_.drawRect(RowX, y, RowWidth, RowHeight, selectionColor(state));
    }
    sprite_.setTextDatum(ML_DATUM);
    sprite_.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite_.drawString(label, RowX + 10, y + RowHeight / 2, 2);
    sprite_.setTextDatum(MR_DATUM);
    sprite_.drawString(value, RowX + RowWidth - 10, y + RowHeight / 2, 2);
  };

  formatSetTemperature(valueText, sizeof(valueText), state);
  drawRow(SettingsItem::SetTemperature, SetTemperatureY, "SET TEMP", valueText);
  formatLight(valueText, sizeof(valueText), state);
  drawRow(SettingsItem::Light, LightY, "LIGHT", valueText);
  drawRow(SettingsItem::Uv, UvY, "UV", onOff(state.uvEnabled));
  drawRow(SettingsItem::Fan, FanY, "FAN", onOff(state.fanEnabled));

  formatPower(valueText, sizeof(valueText), state);
  sprite_.setTextDatum(MC_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString(valueText, Config::DynamicAreaWidth / 2, FooterY, 2);
  sprite_.pushSprite(Config::DynamicAreaX, Config::DynamicAreaY);
}

void DisplayManager::renderDirect(const SystemState &state) {
  constexpr int LabelX = Config::DynamicAreaX + 12;
  constexpr int ValueX = Config::DynamicAreaX + Config::DynamicAreaWidth - 12;
  constexpr int SensorRow1Y = Config::DynamicAreaY + 13;
  constexpr int SensorRow2Y = Config::DynamicAreaY + 35;
  constexpr int SensorRow3Y = Config::DynamicAreaY + 57;
  constexpr int SeparatorY = Config::DynamicAreaY + 70;
  constexpr int RowX = Config::DynamicAreaX + 7;
  constexpr int RowWidth = Config::DynamicAreaWidth - 14;
  constexpr int RowHeight = 26;
  constexpr int SetTemperatureY = Config::DynamicAreaY + 78;
  constexpr int LightY = Config::DynamicAreaY + 107;
  constexpr int UvY = Config::DynamicAreaY + 136;
  constexpr int FanY = Config::DynamicAreaY + 165;
  constexpr int FooterY = Config::DynamicAreaY + 207;

  tft_.fillRect(Config::DynamicAreaX, Config::DynamicAreaY,
                Config::DynamicAreaWidth, Config::DynamicAreaHeight,
                TFT_BLACK);
  tft_.setTextSize(1);
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.drawString("TEMP", LabelX, SensorRow1Y, 2);
  tft_.drawString("HUMIDITY", LabelX, SensorRow2Y, 2);
  tft_.drawString("CO2", LabelX, SensorRow3Y, 2);

  char valueText[24];
  tft_.setTextDatum(MR_DATUM);
  formatTemperature(valueText, sizeof(valueText), state);
  tft_.drawString(valueText, ValueX, SensorRow1Y, 2);
  formatHumidity(valueText, sizeof(valueText), state);
  tft_.drawString(valueText, ValueX, SensorRow2Y, 2);
  formatCo2(valueText, sizeof(valueText), state);
  tft_.drawString(valueText, ValueX, SensorRow3Y, 2);
  tft_.drawFastHLine(Config::DynamicAreaX, SeparatorY,
                     Config::DynamicAreaWidth, TFT_DARKGREY);

  const auto drawRow = [&](SettingsItem item, int y, const char *label,
                           const char *value) {
    if (state.settingsItem == item) {
      tft_.drawRect(RowX, y, RowWidth, RowHeight, selectionColor(state));
    }
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TFT_WHITE, TFT_BLACK);
    tft_.drawString(label, RowX + 10, y + RowHeight / 2, 2);
    tft_.setTextDatum(MR_DATUM);
    tft_.drawString(value, RowX + RowWidth - 10, y + RowHeight / 2, 2);
  };

  formatSetTemperature(valueText, sizeof(valueText), state);
  drawRow(SettingsItem::SetTemperature, SetTemperatureY, "SET TEMP", valueText);
  formatLight(valueText, sizeof(valueText), state);
  drawRow(SettingsItem::Light, LightY, "LIGHT", valueText);
  drawRow(SettingsItem::Uv, UvY, "UV", onOff(state.uvEnabled));
  drawRow(SettingsItem::Fan, FanY, "FAN", onOff(state.fanEnabled));

  formatPower(valueText, sizeof(valueText), state);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.drawString(valueText, Config::DynamicAreaX + Config::DynamicAreaWidth / 2,
                  FooterY, 2);
}
