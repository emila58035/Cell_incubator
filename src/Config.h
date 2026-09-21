#include <stdint.h>
#pragma once

#include <Arduino.h>

namespace Config {
constexpr uint32_t DebugBaudRate = 115200;

constexpr int ShtSdaPin = 1;
constexpr int ShtSclPin = 2;
// The SHT2x family has no address strap: the SHT20 answers at I2C 0x40, and the
// robtillaart/SHT2x driver hardcodes that address internally.
constexpr uint32_t ShtSampleIntervalMs = 1000;
constexpr uint32_t ShtReconnectIntervalMs = 5000;
// A run of failed samples this long is treated as a real disconnection; a
// shorter run only invalidates the reading for that cycle.
constexpr uint8_t ShtMaximumConsecutiveErrors = 3;
// The library's read() gates readiness on the datasheet maxima (85 ms for a
// 14-bit temperature, 29 ms for a 12-bit humidity) with no margin. On this
// hardware that is too early: the read then dies on the I2C driver's ~50 ms
// timeout, and it only succeeds on the following cycle because the abandoned
// measurement has completed by then - which shows up as a strict alternation
// between success and failure. These waits are deliberately above the maxima.
constexpr uint32_t ShtTemperatureConversionMs = 150;
constexpr uint32_t ShtHumidityConversionMs = 80;
// The SHT2x needs up to 15 ms after a soft reset or power-up before it accepts
// the next command, and SHT2x::begin() returns immediately after issuing that
// reset. Sampling without this gap makes the first measurement NACK with
// SHT2x_ERR_WRITECMD, and because every reconnect calls begin() again the
// failure repeats forever. 50 ms is the datasheet minimum plus margin.
constexpr uint32_t ShtResetRecoveryMs = 50;

constexpr int HeaterPwmPin = 39;
constexpr uint32_t HeaterPwmFrequency = 1000;
constexpr uint8_t HeaterPwmResolution = 8;
constexpr uint32_t HeaterPwmMaximum = 255;
constexpr bool HeaterPwmActiveLow = false;

constexpr int LightPwmPin = 38;
constexpr uint32_t LightPwmFrequency = 1000;
constexpr uint8_t LightPwmResolution = 8;
constexpr uint32_t LightPwmMaximum = 255;
constexpr bool LightPwmActiveLow = false;
constexpr uint8_t LightBrightnessStep = 5;

constexpr int FanOutputPin = 47;
constexpr bool FanOutputActiveLow = false;

constexpr int S8RxPin = 5;
constexpr int S8TxPin = 4;
constexpr uint8_t S8UartPort = 1;
constexpr uint32_t S8BaudRate = 9600;
constexpr uint32_t Co2ResponseTimeoutMs = 500;
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
constexpr uint32_t PushDebounceMs = 100;

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
