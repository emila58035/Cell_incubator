#pragma once

#include <Arduino.h>

// Credentials arrive as -D flags from secrets.ini, which is gitignored (see
// secrets.example.ini). platformio.ini carries empty placeholders, so a clone
// without secrets still compiles. Empty values are treated as "not
// configured": the networking modules then stay quiet instead of retrying
// forever against nothing.
#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID ""
#endif
#ifndef SECRET_WIFI_PASS
#define SECRET_WIFI_PASS ""
#endif
#ifndef SECRET_SERVER_URL
#define SECRET_SERVER_URL ""
#endif
#ifndef SECRET_DEVICE_ID
#define SECRET_DEVICE_ID ""
#endif
#ifndef SECRET_DEVICE_TOKEN
#define SECRET_DEVICE_TOKEN ""
#endif

// The CA public certificate is a multi-line PEM, so it does not fit into a -D
// flag. It lives in src/ServerCaCert.h (gitignored, template next to it).
// A clone without that header still compiles and simply reports nothing.

namespace NetworkConfig {

// The simulated build reports a suffixed version so that simulated samples are
// distinguishable from real ones in the database and on the dashboard.
#if SENSOR_SIMULATION
constexpr char FirmwareVersion[] = "0.1.0-sim";
#else
constexpr char FirmwareVersion[] = "0.1.0";
#endif

constexpr char WifiSsid[] = SECRET_WIFI_SSID;
constexpr char WifiPassword[] = SECRET_WIFI_PASS;
constexpr char ServerUrl[] = SECRET_SERVER_URL;
constexpr char DeviceId[] = SECRET_DEVICE_ID;
constexpr char DeviceToken[] = SECRET_DEVICE_TOKEN;

// The CA public certificate is a multi-line PEM and therefore cannot travel in
// a -D flag. It lives in src/ServerCaCert.h, which is gitignored (template in
// src/ServerCaCert.h.example). This include sits inside the namespace on
// purpose: the header defines NetworkConfig::ServerCaCertPem, exactly as its
// own first comment says. Without the header the value stays empty, which the
// client treats as "not configured".
#if __has_include("ServerCaCert.h")
#include "ServerCaCert.h"
#else
constexpr char ServerCaCertPem[] = "";
#endif

constexpr uint32_t ReportIntervalMs = 10000;

// 15 s is the time the access point gets to answer a single association
// attempt. WL_NO_SSID_AVAIL and WL_CONNECT_FAILED shorten this to a fail-fast.
constexpr uint32_t WifiConnectTimeoutMs = 15000;
constexpr uint32_t WifiReconnectMinMs = 5000;
constexpr uint32_t WifiReconnectMaxMs = 60000;

// Blocking budget of one report attempt. All three are set explicitly:
// HTTPClient::setTimeout() does NOT cover the TLS handshake. WiFiClientSecure
// keeps its own field, defaulting to 120000 ms, and setHandshakeTimeout()
// takes SECONDS (it multiplies by 1000 internally), so 8 means 8 s.
constexpr int32_t ConnectTimeoutMs = 3000;
constexpr uint32_t HandshakeTimeoutSeconds = 8;
constexpr uint16_t ResponseTimeoutMs = 3000;

// The network task is pinned to core 0 because core 1 must never host a
// blocking task: the control loop does not yield, and the same-priority time
// slicing would steal milliseconds from it.
constexpr uint32_t NetTaskStackBytes = 12288;
constexpr UBaseType_t NetTaskPriority = 1;
constexpr BaseType_t NetTaskCore = 0;
constexpr uint32_t NetTaskPollMs = 20;

// One TLS session needs about 2 x 16 KB of internal heap: the pinned
// precompiled libraries have CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN = 16384 with
// asymmetric content length disabled. Below this watermark a report is skipped
// rather than risking an out-of-memory restart.
constexpr size_t MinInternalHeapBytes = 40960;

// Measured body length is about 430 bytes; the margin covers wider numbers.
constexpr size_t MaxBodyBytes = 640;

}  // namespace NetworkConfig