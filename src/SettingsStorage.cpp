#include "SettingsStorage.h"

#include <Preferences.h>
#include <stddef.h>

#include "Config.h"

namespace {

constexpr char StorageNamespace[] = "incubator";
constexpr char StorageKey[] = "settings";
constexpr uint32_t StorageMagic = 0x43494E43;
constexpr uint16_t StorageVersion = 2;
constexpr uint16_t LegacyStorageVersion = 1;

constexpr uint8_t LightEnabledFlag = 1 << 0;
constexpr uint8_t FanEnabledFlag = 1 << 1;
constexpr uint8_t LegacyFanEnabledFlag = 1 << 2;

struct StoredSettings {
  uint32_t magic;
  uint16_t version;
  int16_t setpointTenths;
  uint8_t lightBrightness;
  uint8_t flags;
  uint16_t reserved;
  uint32_t checksum;
};

uint32_t calculateChecksum(const StoredSettings &settings) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&settings);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(StoredSettings, checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

}  // namespace

void SettingsStorage::begin(SystemState &state) {
  state.heatingEnabled = false;
  const bool loaded = load(state);
  if (!loaded) {
    Serial.println("[SETTINGS] No valid saved settings; using defaults");
  }

  state.settingsDirty = !loaded;
  state.settingsSaveRequested = false;
  state.settingsSaveFailed = false;
  logSettings("Active", capture(state));
  Serial.println("[SETTINGS] Heating=OFF (not persisted)");
  state.markDisplayDirty();
}

bool SettingsStorage::saveNow(SystemState &state) {
  const bool saved = save(capture(state));
  state.settingsSaveRequested = false;
  state.settingsSaveFailed = !saved;
  if (saved) {
    state.settingsDirty = false;
  }
  state.markDisplayDirty();
  return saved;
}

SettingsStorage::Snapshot SettingsStorage::capture(const SystemState &state) {
  Snapshot snapshot{};
  snapshot.setpointTenths = static_cast<int16_t>(round(state.setpoint * 10.0));
  snapshot.lightBrightness = min<uint8_t>(state.lightBrightness, 100);
  snapshot.lightEnabled = state.lightEnabled && snapshot.lightBrightness > 0;
  snapshot.fanEnabled = state.fanEnabled;
  return snapshot;
}

bool SettingsStorage::load(SystemState &state) {
  Preferences preferences;
  if (!preferences.begin(StorageNamespace, false)) {
    Serial.println("[SETTINGS] ERROR: failed to open NVS");
    return false;
  }

  StoredSettings stored{};
  const size_t storedSize = preferences.getBytesLength(StorageKey);
  const size_t bytesRead =
      storedSize == sizeof(stored)
          ? preferences.getBytes(StorageKey, &stored, sizeof(stored))
          : 0;
  preferences.end();

  const int16_t minimumSetpoint =
      static_cast<int16_t>(round(Config::MinimumSetpoint * 10.0));
  const int16_t maximumSetpoint =
      static_cast<int16_t>(round(Config::MaximumSetpoint * 10.0));
  const bool valid =
      bytesRead == sizeof(stored) && stored.magic == StorageMagic &&
      (stored.version == StorageVersion ||
       stored.version == LegacyStorageVersion) &&
      stored.checksum == calculateChecksum(stored) &&
      stored.setpointTenths >= minimumSetpoint &&
      stored.setpointTenths <= maximumSetpoint &&
      stored.lightBrightness <= 100;
  if (!valid) {
    return false;
  }

  state.setpoint = stored.setpointTenths / 10.0;
  state.lightBrightness = stored.lightBrightness;
  state.lightEnabled = (stored.flags & LightEnabledFlag) != 0 &&
                       state.lightBrightness > 0;
  const uint8_t fanFlag = stored.version == LegacyStorageVersion
                              ? LegacyFanEnabledFlag
                              : FanEnabledFlag;
  state.fanEnabled = (stored.flags & fanFlag) != 0;
  Serial.println("[SETTINGS] Loaded from NVS");
  return true;
}

bool SettingsStorage::save(const Snapshot &settings) {
  StoredSettings stored{};
  stored.magic = StorageMagic;
  stored.version = StorageVersion;
  stored.setpointTenths = settings.setpointTenths;
  stored.lightBrightness = settings.lightBrightness;
  stored.flags = (settings.lightEnabled ? LightEnabledFlag : 0) |
                 (settings.fanEnabled ? FanEnabledFlag : 0);
  stored.checksum = calculateChecksum(stored);

  Preferences preferences;
  if (!preferences.begin(StorageNamespace, false)) {
    Serial.println("[SETTINGS] ERROR: failed to open NVS for writing");
    return false;
  }

  const size_t bytesWritten =
      preferences.putBytes(StorageKey, &stored, sizeof(stored));
  preferences.end();
  if (bytesWritten != sizeof(stored)) {
    Serial.println("[SETTINGS] ERROR: failed to save settings");
    return false;
  }

  logSettings("Saved", settings);
  return true;
}

void SettingsStorage::logSettings(const char *event,
                                  const Snapshot &settings) {
  Serial.printf(
      "[SETTINGS] %s: setpoint=%.1f C, light=%s/%u%%, fan=%s\n",
      event, settings.setpointTenths / 10.0,
      settings.lightEnabled ? "ON" : "OFF", settings.lightBrightness,
      settings.fanEnabled ? "ON" : "OFF");
}
