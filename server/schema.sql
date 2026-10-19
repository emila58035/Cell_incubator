-- Cell Incubator telemetry schema (SQLite).
-- Applied by deploy/setup.sh; every statement is idempotent, so re-running is
-- safe and is also how schema additions get applied later.
--
-- Device API tokens are NOT stored here on purpose: they live in
-- /etc/incubator/devices.json. The database is the artefact that ends up in
-- backups and behind dashboard queries, so it holds no credentials.

PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS devices (
  device_id   TEXT PRIMARY KEY,
  label       TEXT,
  first_seen  INTEGER,
  last_seen   INTEGER,
  fw_version  TEXT
);

CREATE TABLE IF NOT EXISTS telemetry (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id   TEXT NOT NULL,
  recv_ts     INTEGER NOT NULL,   -- server Unix seconds; the authoritative time
  uptime_s    INTEGER,
  fw_version  TEXT,

  temperature REAL,   temperature_ok INTEGER,
  humidity    REAL,   humidity_ok    INTEGER,
  -- Device units, verbatim (SenseAir S8 register x Config::Co2ScaleFactor).
  -- No unit conversion happens on the server; see README "CO2 unit".
  co2         INTEGER, co2_ok        INTEGER, co2_available INTEGER,

  target_temperature REAL,
  heater_on   INTEGER, heater_pwm INTEGER,
  light       INTEGER, light_pct  INTEGER,
  fan         INTEGER,
  fan_pct     INTEGER,
  valve_enabled INTEGER,
  valve       INTEGER,

  -- Diagnostics: rssi for the radio link, heap_* for leak detection.
  -- heap_int is internal RAM only and is the one that predicts an OOM reboot.
  rssi        INTEGER,
  heap_free   INTEGER,
  heap_min    INTEGER,
  heap_int    INTEGER
);

CREATE INDEX IF NOT EXISTS idx_telemetry_dev_ts ON telemetry(device_id, recv_ts DESC);

CREATE TABLE IF NOT EXISTS alerts (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id   TEXT NOT NULL,
  raised_ts   INTEGER NOT NULL,
  cleared_ts  INTEGER,
  kind        TEXT NOT NULL,   -- 'offline' | 'sensor_fault' | 'low_heap'
  detail      TEXT
);

CREATE INDEX IF NOT EXISTS idx_alerts_dev_ts ON alerts(device_id, raised_ts DESC);
CREATE INDEX IF NOT EXISTS idx_alerts_open
  ON alerts(device_id, kind) WHERE cleared_ts IS NULL;

CREATE TABLE IF NOT EXISTS remote_commands (
  command_id  TEXT PRIMARY KEY,
  device_id   TEXT NOT NULL,
  command     TEXT NOT NULL,
  params_json TEXT NOT NULL,
  status      TEXT NOT NULL,
  created_ts  INTEGER NOT NULL,
  expires_ts  INTEGER NOT NULL,
  sent_ts     INTEGER,
  ack_ts      INTEGER,
  ack_reason  TEXT,
  requested_by TEXT
);

CREATE INDEX IF NOT EXISTS idx_remote_commands_device
  ON remote_commands(device_id, created_ts DESC);
