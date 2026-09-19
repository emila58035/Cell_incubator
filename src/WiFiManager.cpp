#include "WiFiManager.h"

#include <WiFi.h>

#include "NetworkConfig.h"

void WiFiManager::begin() {
  if (strlen(NetworkConfig::WifiSsid) == 0) {
    Serial.println(
        "[WIFI] No SSID configured (src/Secrets.h missing or empty); "
        "networking stays off");
    phase_ = Phase::Unconfigured;
    return;
  }

  WiFi.mode(WIFI_STA);
  // This manager owns the retry policy, so keep the library out of it, and do
  // not write credentials to NVS on every boot.
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  startAttempt(millis());
}

void WiFiManager::startAttempt(uint32_t now) {
  phase_ = Phase::Connecting;
  deadlineMs_ = now + NetworkConfig::WifiConnectTimeoutMs;
  WiFi.begin(NetworkConfig::WifiSsid, NetworkConfig::WifiPassword);
  Serial.printf("[WIFI] Connecting to \"%s\"\n", NetworkConfig::WifiSsid);
}

void WiFiManager::scheduleRetry(uint32_t now) {
  backoffMs_ = backoffMs_ == 0
                   ? NetworkConfig::WifiReconnectMinMs
                   : min(backoffMs_ * 2, NetworkConfig::WifiReconnectMaxMs);
  const uint32_t jitter = random(0, backoffMs_ / 4 + 1);
  deadlineMs_ = now + backoffMs_ + jitter;
  phase_ = Phase::BackingOff;
  Serial.printf("[WIFI] Attempt failed; retrying in %lu ms\n",
                (unsigned long)(backoffMs_ + jitter));
}

void WiFiManager::step(uint32_t now) {
  if (phase_ == Phase::Unconfigured) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!linkUp_) {
      linkUp_ = true;
      backoffMs_ = 0;
      Serial.printf("[WIFI] Connected, RSSI %d dBm\n", WiFi.RSSI());
    }
    rssi_ = (int16_t)WiFi.RSSI();
    return;
  }

  if (linkUp_) {
    linkUp_ = false;
    Serial.println("[WIFI] Link lost");
  }

  const wl_status_t status = WiFi.status();
  const bool failedFast =
      status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED;
  const bool deadlinePassed = (int32_t)(now - deadlineMs_) >= 0;

  if (phase_ == Phase::Connecting) {
    if (!failedFast && !deadlinePassed) {
      return;  // still associating
    }
    WiFi.disconnect(false, false);
    scheduleRetry(now);
    return;
  }

  if (deadlinePassed) {
    startAttempt(now);
  }
}