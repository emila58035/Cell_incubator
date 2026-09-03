#include "DisplayManager.h"

#include "Config.h"

DisplayManager::DisplayManager() : sprite_(&tft_) {}

namespace {

const char *onOff(bool enabled) { return enabled ? "ON" : "OFF"; }

uint16_t settingColor(const SystemState &state, SettingsItem item) {
  if (state.settingsItem != item) {
    return TFT_DARKGREY;
  }
  return state.settingsItemSelected ? TFT_YELLOW : TFT_CYAN;
}

}  // namespace

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
  spriteReady_ =
      sprite_.createSprite(Config::DynamicAreaWidth,
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
  if (state.page == UiPage::Settings) {
    renderSettingsSprite(state);
    return;
  }
  renderStatusSprite(state);
}

void DisplayManager::renderStatusSprite(const SystemState &state) {
  constexpr int LabelX = 12;
  constexpr int ValueX = 236;
  constexpr int UnitX = 248;
  constexpr int Row1Y = 27;
  constexpr int Row2Y = 82;
  constexpr int Row3Y = 137;
  constexpr int Footer1Y = 171;
  constexpr int Footer2Y = 193;
  constexpr int Footer3Y = 212;

  sprite_.fillSprite(TFT_BLACK);
  sprite_.setTextSize(1);

  sprite_.drawFastHLine(0, 55, Config::DynamicAreaWidth, TFT_DARKGREY);
  sprite_.drawFastHLine(0, 110, Config::DynamicAreaWidth, TFT_DARKGREY);
  sprite_.drawFastHLine(0, 165, Config::DynamicAreaWidth, TFT_DARKGREY);

  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString("TEMP", LabelX, Row1Y, 4);
  sprite_.drawString("HUMIDITY", LabelX, Row2Y, 4);
  sprite_.drawString("CO2", LabelX, Row3Y, 4);

  char valueText[16];
  sprite_.setTextDatum(MR_DATUM);
  if (state.temperatureValid) {
    sprite_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%.1f", state.temperature);
  } else {
    sprite_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  sprite_.drawString(valueText, ValueX, Row1Y, 4);

  if (state.humidityValid) {
    sprite_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%.1f", state.humidity);
  } else {
    sprite_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  sprite_.drawString(valueText, ValueX, Row2Y, 4);

  if (state.co2Valid) {
    sprite_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%lu", (unsigned long)state.co2);
  } else {
    sprite_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  sprite_.drawString(valueText, ValueX, Row3Y, 4);

  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString("C", UnitX, Row1Y, 2);
  sprite_.drawString("%", UnitX, Row2Y, 2);
  sprite_.drawString("ppm", UnitX, Row3Y, 2);

  char statusText[40];
  snprintf(statusText, sizeof(statusText), "Set %.1fC  Heat %s", state.setpoint,
           onOff(state.heatingEnabled));
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString(statusText, LabelX, Footer1Y, 2);

  snprintf(statusText, sizeof(statusText), "Light %s %u%%  UV %s",
           onOff(state.lightEnabled), state.lightBrightness,
           onOff(state.uvEnabled));
  sprite_.drawString(statusText, LabelX, Footer2Y, 2);

  snprintf(statusText, sizeof(statusText), "Fan %s  Power %.0f%%",
           onOff(state.fanEnabled),
           state.heatingEnabled
               ? state.heaterOutput / Config::HeaterPwmMaximum * 100.0
               : 0.0);
  sprite_.drawString(statusText, LabelX, Footer3Y, 2);

  sprite_.pushSprite(Config::DynamicAreaX, Config::DynamicAreaY);
}

void DisplayManager::renderDirect(const SystemState &state) {
  if (state.page == UiPage::Settings) {
    renderSettingsDirect(state);
    return;
  }
  renderStatusDirect(state);
}

void DisplayManager::renderStatusDirect(const SystemState &state) {
  constexpr int LabelX = Config::DynamicAreaX + 12;
  constexpr int ValueX = Config::DynamicAreaX + 236;
  constexpr int UnitX = Config::DynamicAreaX + 248;
  constexpr int Row1Y = Config::DynamicAreaY + 27;
  constexpr int Row2Y = Config::DynamicAreaY + 82;
  constexpr int Row3Y = Config::DynamicAreaY + 137;
  constexpr int Footer1Y = Config::DynamicAreaY + 171;
  constexpr int Footer2Y = Config::DynamicAreaY + 193;
  constexpr int Footer3Y = Config::DynamicAreaY + 212;

  tft_.fillRect(Config::DynamicAreaX, Config::DynamicAreaY,
                Config::DynamicAreaWidth, Config::DynamicAreaHeight,
                TFT_BLACK);
  tft_.drawFastHLine(Config::DynamicAreaX, Config::DynamicAreaY + 55,
                     Config::DynamicAreaWidth, TFT_DARKGREY);
  tft_.drawFastHLine(Config::DynamicAreaX, Config::DynamicAreaY + 110,
                     Config::DynamicAreaWidth, TFT_DARKGREY);
  tft_.drawFastHLine(Config::DynamicAreaX, Config::DynamicAreaY + 165,
                     Config::DynamicAreaWidth, TFT_DARKGREY);
  tft_.setTextSize(1);

  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.drawString("TEMP", LabelX, Row1Y, 4);
  tft_.drawString("HUMIDITY", LabelX, Row2Y, 4);
  tft_.drawString("CO2", LabelX, Row3Y, 4);

  char valueText[16];
  tft_.setTextDatum(MR_DATUM);
  if (state.temperatureValid) {
    tft_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%.1f", state.temperature);
  } else {
    tft_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  tft_.drawString(valueText, ValueX, Row1Y, 4);

  if (state.humidityValid) {
    tft_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%.1f", state.humidity);
  } else {
    tft_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  tft_.drawString(valueText, ValueX, Row2Y, 4);

  if (state.co2Valid) {
    tft_.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "%lu", (unsigned long)state.co2);
  } else {
    tft_.setTextColor(TFT_RED, TFT_BLACK);
    snprintf(valueText, sizeof(valueText), "--");
  }
  tft_.drawString(valueText, ValueX, Row3Y, 4);

  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.drawString("C", UnitX, Row1Y, 2);
  tft_.drawString("%", UnitX, Row2Y, 2);
  tft_.drawString("ppm", UnitX, Row3Y, 2);

  char statusText[40];
  snprintf(statusText, sizeof(statusText), "Set %.1fC  Heat %s", state.setpoint,
           onOff(state.heatingEnabled));
  tft_.drawString(statusText, LabelX, Footer1Y, 2);

  snprintf(statusText, sizeof(statusText), "Light %s %u%%  UV %s",
           onOff(state.lightEnabled), state.lightBrightness,
           onOff(state.uvEnabled));
  tft_.drawString(statusText, LabelX, Footer2Y, 2);

  snprintf(statusText, sizeof(statusText), "Fan %s  Power %.0f%%",
           onOff(state.fanEnabled),
           state.heatingEnabled
               ? state.heaterOutput / Config::HeaterPwmMaximum * 100.0
               : 0.0);
  tft_.drawString(statusText, LabelX, Footer3Y, 2);
}

void DisplayManager::renderSettingsSprite(const SystemState &state) {
  constexpr int X = 8;
  constexpr int Width = Config::DynamicAreaWidth - X * 2;
  constexpr int RowHeight = 44;
  constexpr int LightY = 45;
  constexpr int UvY = 98;
  constexpr int FanY = 151;

  sprite_.fillSprite(TFT_BLACK);
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString("SETTINGS", X, 17, 2);
  sprite_.drawFastHLine(0, 33, Config::DynamicAreaWidth, TFT_DARKGREY);

  sprite_.drawRect(X, LightY, Width, RowHeight,
                  settingColor(state, SettingsItem::Light));
  sprite_.drawRect(X, UvY, Width, RowHeight,
                  settingColor(state, SettingsItem::Uv));
  sprite_.drawRect(X, FanY, Width, RowHeight,
                  settingColor(state, SettingsItem::Fan));

  sprite_.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite_.drawString("LIGHT", X + 12, LightY + RowHeight / 2, 2);
  sprite_.drawString("UV", X + 12, UvY + RowHeight / 2, 2);
  sprite_.drawString("FAN", X + 12, FanY + RowHeight / 2, 2);

  char valueText[20];
  sprite_.setTextDatum(MR_DATUM);
  snprintf(valueText, sizeof(valueText), "%s  %u%%", onOff(state.lightEnabled),
           state.lightBrightness);
  sprite_.drawString(valueText, X + Width - 12, LightY + RowHeight / 2, 2);
  sprite_.drawString(onOff(state.uvEnabled), X + Width - 12,
                     UvY + RowHeight / 2, 2);
  sprite_.drawString(onOff(state.fanEnabled), X + Width - 12,
                     FanY + RowHeight / 2, 2);

  sprite_.pushSprite(Config::DynamicAreaX, Config::DynamicAreaY);
}

void DisplayManager::renderSettingsDirect(const SystemState &state) {
  constexpr int X = Config::DynamicAreaX + 8;
  constexpr int Width = Config::DynamicAreaWidth - 16;
  constexpr int RowHeight = 44;
  constexpr int LightY = Config::DynamicAreaY + 45;
  constexpr int UvY = Config::DynamicAreaY + 98;
  constexpr int FanY = Config::DynamicAreaY + 151;

  tft_.fillRect(Config::DynamicAreaX, Config::DynamicAreaY,
                Config::DynamicAreaWidth, Config::DynamicAreaHeight,
                TFT_BLACK);
  tft_.setTextDatum(ML_DATUM);
  tft_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft_.drawString("SETTINGS", X, Config::DynamicAreaY + 17, 2);
  tft_.drawFastHLine(Config::DynamicAreaX, Config::DynamicAreaY + 33,
                     Config::DynamicAreaWidth, TFT_DARKGREY);

  tft_.drawRect(X, LightY, Width, RowHeight,
                settingColor(state, SettingsItem::Light));
  tft_.drawRect(X, UvY, Width, RowHeight,
                settingColor(state, SettingsItem::Uv));
  tft_.drawRect(X, FanY, Width, RowHeight,
                settingColor(state, SettingsItem::Fan));

  tft_.setTextColor(TFT_WHITE, TFT_BLACK);
  tft_.drawString("LIGHT", X + 12, LightY + RowHeight / 2, 2);
  tft_.drawString("UV", X + 12, UvY + RowHeight / 2, 2);
  tft_.drawString("FAN", X + 12, FanY + RowHeight / 2, 2);

  char valueText[20];
  tft_.setTextDatum(MR_DATUM);
  snprintf(valueText, sizeof(valueText), "%s  %u%%", onOff(state.lightEnabled),
           state.lightBrightness);
  tft_.drawString(valueText, X + Width - 12, LightY + RowHeight / 2, 2);
  tft_.drawString(onOff(state.uvEnabled), X + Width - 12,
                  UvY + RowHeight / 2, 2);
  tft_.drawString(onOff(state.fanEnabled), X + Width - 12,
                  FanY + RowHeight / 2, 2);
}
