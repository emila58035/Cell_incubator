#include <stdint.h>
#pragma once

#include <Arduino.h>

namespace Config {
constexpr uint32_t DebugBaudRate = 115200;

constexpr int AhtSdaPin = 1;
constexpr int AhtSclPin = 2;
constexpr uint8_t AhtAddress = 0x38;
constexpr uint32_t AhtSampleIntervalMs = 1000;
constexpr uint32_t AhtReconnectIntervalMs = 5000;

constexpr int HeaterPwmPin = 3;
constexpr uint32_t HeaterPwmFrequency = 1000;
constexpr uint8_t HeaterPwmResolution = 8;
constexpr uint32_t HeaterPwmMaximum = 255;
constexpr bool HeaterPwmActiveLow = true;

constexpr int LightPwmPin = 38;
constexpr uint32_t LightPwmFrequency = 1000;
constexpr uint8_t LightPwmResolution = 8;
constexpr uint32_t LightPwmMaximum = 255;
constexpr bool LightPwmActiveLow = true;
constexpr uint8_t LightBrightnessStep = 5;

constexpr int UvOutputPin = 39;
constexpr bool UvOutputActiveLow = true;

constexpr int FanOutputPin = 47;
constexpr bool FanOutputActiveLow = true;

constexpr int S8RxPin = 5;
constexpr int S8TxPin = 6;
constexpr uint8_t S8UartPort = 1;
constexpr uint32_t Co2SampleIntervalMs = 5000;
constexpr uint32_t Co2ReconnectIntervalMs = 10000;
constexpr uint8_t Co2MaximumConsecutiveErrors = 3;
// Preserves the scaling used by the source project. Set to 1 for direct S8 ppm.
constexpr uint8_t Co2ScaleFactor = 10;

constexpr int TftRESPin = 18;
constexpr int TftBacklightPin = 15;
constexpr int EncoderAPin = 41;
constexpr int EncoderBPin = 42;
constexpr int EncoderPushPin = 45;
constexpr int KOPIN  = 46;
constexpr uint32_t PushDebounceMs = 200;

constexpr double DefaultSetpoint = 37.0;
constexpr double MinimumSetpoint = 10.0;
constexpr double MaximumSetpoint = 100.0;
constexpr double EncoderTemperatureStep = 0.1;

constexpr double ConservativeKp = 10.0;
constexpr double ConservativeKi = 0.1;
constexpr double ConservativeKd = 0.0;
constexpr double AggressiveKp = 20.0;
constexpr double AggressiveKi = 0.0;
constexpr double AggressiveKd = 0.0;
constexpr double ConservativeBandCelsius = 3.0;

constexpr int DisplayWidth = 320;
constexpr int DisplayHeight = 240;
constexpr int DynamicAreaX = 8;
constexpr int DynamicAreaY = 10;
constexpr int DynamicAreaWidth = 304;
constexpr int DynamicAreaHeight = 220;

constexpr int ValvePin = 48;
constexpr uint32_t ValveIntervalMs1 = 10000;
constexpr uint32_t ValveIntervalMs2 = 20000;
constexpr uint32_t ValveIntervalMs3 = 20000;
constexpr uint32_t ValveCO2threshold1 = 25000;
constexpr uint32_t ValveCO2threshold2 = 40000;
constexpr uint32_t ValveCO2threshold3 = 48000;
constexpr uint32_t ValveOpenDurationMs_long = 1000;
constexpr uint32_t ValveOpenDurationMs_short = 500;

}
