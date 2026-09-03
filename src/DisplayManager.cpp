#include "DisplayManager.h"

#include "Config.h"

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
  constexpr int LabelX = 12;
  constexpr int ValueX = 236;
  constexpr int UnitX = 248;
  constexpr int Row1Y = 27;
  constexpr int Row2Y = 82;
  constexpr int Row3Y = 137;
  constexpr int FooterY = 193;

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

  char setpointText[20];
  snprintf(setpointText, sizeof(setpointText), "Set %.1f C", state.setpoint);
  sprite_.setTextDatum(ML_DATUM);
  sprite_.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  sprite_.drawString(setpointText, LabelX, FooterY, 2);

  sprite_.setTextDatum(MC_DATUM);
  sprite_.drawString(state.LEDState ? "Light ON" : "Light OFF",
                     Config::DynamicAreaWidth / 2, FooterY, 2);

  char powerText[20];
  if (state.heatingEnabled) {
    snprintf(powerText, sizeof(powerText), "Power %.0f%%",
             state.heaterOutput / Config::HeaterPwmMaximum * 100.0);
  } else {
    snprintf(powerText, sizeof(powerText), "Power OFF");
  }
  sprite_.setTextDatum(MR_DATUM);
  sprite_.drawString(powerText, Config::DynamicAreaWidth - LabelX, FooterY, 2);

  sprite_.pushSprite(Config::DynamicAreaX, Config::DynamicAreaY);
}

void DisplayManager::renderDirect(const SystemState &state) {
  constexpr int LabelX = Config::DynamicAreaX + 12;
  constexpr int ValueX = Config::DynamicAreaX + 236;
  constexpr int UnitX = Config::DynamicAreaX + 248;
  constexpr int Row1Y = Config::DynamicAreaY + 27;
  constexpr int Row2Y = Config::DynamicAreaY + 82;
  constexpr int Row3Y = Config::DynamicAreaY + 137;
  constexpr int FooterY = Config::DynamicAreaY + 193;

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

  char setpointText[20];
  snprintf(setpointText, sizeof(setpointText), "Set %.1f C", state.setpoint);
  tft_.drawString(setpointText, LabelX, FooterY, 2);

  tft_.setTextDatum(MC_DATUM);
  tft_.drawString(state.LEDState ? "Light ON" : "Light OFF",
                  Config::DisplayWidth / 2, FooterY, 2);

  char powerText[20];
  if (state.heatingEnabled) {
    snprintf(powerText, sizeof(powerText), "Power %.0f%%",
             state.heaterOutput / Config::HeaterPwmMaximum * 100.0);
  } else {
    snprintf(powerText, sizeof(powerText), "Power OFF");
  }
  tft_.setTextDatum(MR_DATUM);
  tft_.drawString(powerText, Config::DynamicAreaX +
                                 Config::DynamicAreaWidth - 12,
                  FooterY, 2);
}
