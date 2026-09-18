#!/usr/bin/env python3
"""Incubator telemetry API - FastAPI + SQLite (read-only dashboard backend).

Scope and constraints (HANDOFF.md 3.3/8, docs/REMOTE_MONITORING.md 5/7):

  * Ingest plus read-only queries. There is no control path in this process.
  * Listens on 127.0.0.1:8000 only. Nginx terminates TLS and enforces Basic
    Auth for the dashboard; this process must never see the public interface.
  * Device auth is `Authorization: Bearer <token>` read from the token file,
    compared with hmac.compare_digest and without early exit on mismatch.
  * The server clock is authoritative (recv_ts). Devices report uptime_s only,
    never a wall-clock timestamp, so no NTP is needed on the device.
  * Values are stored exactly as received. In particular `co2` is NOT
    converted: the firmware sends SystemState.co2 verbatim, which is the
    SenseAir S8 register value multiplied by Config::Co2ScaleFactor. See the
    "CO2 unit" section of README.md before adding any threshold on it.

Environment:
  INCUBATOR_DB       SQLite path   (default /var/lib/incubator/data.db)
  INCUBATOR_DEVICES  token file    (default /etc/incubator/devices.json)
  INCUBATOR_STATIC   static dir    (default <this dir>/static)
  INCUBATOR_OFFLINE_S  silence that counts as offline (default 30s = 3 x 10s)
  INCUBATOR_LOW_HEAP   heap alert floor in bytes (default 40960)
"""

from __future__ import annotations

import hmac
import json
import os
import sqlite3
import time
from pathlib import Path
from typing import Any, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, Response
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

API_VERSION = "1.0.0"
APP_DIR = Path(__file__).resolve().parent

DB_PATH = Path(os.environ.get("INCUBATOR_DB") or "/var/lib/incubator/data.db")
DEVICES_PATH = Path(os.environ.get("INCUBATOR_DEVICES") or "/etc/incubator/devices.json")
STATIC_DIR = Path(os.environ.get("INCUBATOR_STATIC") or (APP_DIR / "static"))

PUBLISH_INTERVAL_S = 10  # must match NetCfg::PublishIntervalMs in the firmware
OFFLINE_AFTER_S = int(os.environ.get("INCUBATOR_OFFLINE_S") or PUBLISH_INTERVAL_S * 3)
LOW_HEAP_BYTES = int(os.environ.get("INCUBATOR_LOW_HEAP") or 40960)

DEVICE_ID_MAX = 64
HEAP_MAX = 8 * 1024 * 1024
HISTORY_POINT_LIMIT = 2000


# --------------------------------------------------------------------------
# Request model
# --------------------------------------------------------------------------
class TelemetryIn(BaseModel):
    """One upload. Ranges reject corrupted or hostile payloads early."""

    # extra="ignore" rather than "forbid": a newer firmware that adds a field
    # must keep working against an older server instead of getting a 400 and
    # silently losing the whole sample. A typo shows up as a NULL on the
    # dashboard, which is visible, rather than as a missing sample, which is not.
    model_config = {"extra": "ignore"}

    device_id: str = Field(min_length=1, max_length=DEVICE_ID_MAX)
    uptime_s: Optional[int] = Field(default=None, ge=0, le=2**32)
    firmware_version: Optional[str] = Field(default=None, max_length=32)

    temperature: Optional[float] = Field(default=None, ge=-40, le=85)
    temperature_ok: Optional[bool] = None
    humidity: Optional[float] = Field(default=None, ge=0, le=100)
    humidity_ok: Optional[bool] = None
    # Device units, verbatim. Do not convert, see module docstring.
    co2: Optional[int] = Field(default=None, ge=0, le=1000000)
    co2_ok: Optional[bool] = None
    co2_available: Optional[bool] = None

    target_temperature: Optional[float] = Field(default=None, ge=0, le=120)
    heater_on: Optional[bool] = None
    heater_pwm: Optional[int] = Field(default=None, ge=0, le=255)
    light: Optional[bool] = None
    light_pct: Optional[int] = Field(default=None, ge=0, le=100)
    fan: Optional[bool] = None
    valve: Optional[bool] = None

    rssi: Optional[int] = Field(default=None, ge=-120, le=0)
    heap_free: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)
    heap_min: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)
    heap_int: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)


# --------------------------------------------------------------------------
# Device tokens (kept out of the database on purpose: the DB is the thing that
# ends up in backups and in dashboard queries)
# --------------------------------------------------------------------------
_token_map: dict[str, dict[str, Any]] = {}
_token_map_mtime: float = -1.0


def token_map() -> dict[str, dict[str, Any]]:
    """device_id -> {"token": str, "label": str|None}, reloaded on file change.

    Accepted shapes (both kept working so hand-written files do not break):
        {"incubator-001": "hex-token"}
        {"incubator-001": {"token": "hex-token", "label": "box 1"}}
    """
    global _token_map, _token_map_mtime
    try:
        mtime = DEVICES_PATH.stat().st_mtime
    except OSError:
        return _token_map  # keep the last good copy instead of failing closed
    if mtime == _token_map_mtime:
        return _token_map
    try:
        raw = json.loads(DEVICES_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"[TOKENS] cannot read {DEVICES_PATH}: {exc}", flush=True)
        return _token_map

    parsed: dict[str, dict[str, Any]] = {}
    for device_id, value in raw.items():
        if isinstance(value, str):
            parsed[device_id] = {"token": value, "label": None}
        elif isinstance(value, dict) and value.get("token"):
            parsed[device_id] = {
                "token": str(value["token"]),
                "label": value.get("label"),
            }
    _token_map = parsed
    _token_map_mtime = mtime
    print(f"[TOKENS] loaded {len(parsed)} device(s) from {DEVICES_PATH}", flush=True)
    return parsed


def authenticate(authorization: Optional[str] = Header(default=None)) -> str:
    """Validate the Bearer token and return the device it belongs to."""
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(
            401, "missing bearer token", headers={"WWW-Authenticate": "Bearer"}
        )
    presented = authorization[7:].strip()
    matched: Optional[str] = None
    # compare_digest is called for every device and the loop never exits early,
    # so response timing does not reveal which token (if any) matched.
    for device_id, meta in token_map().items():
        if hmac.compare_digest(presented, meta["token"]):
            matched = device_id
    if matched is None:
        raise HTTPException(
            401, "invalid token", headers={"WWW-Authenticate": "Bearer"}
        )
    return matched


# --------------------------------------------------------------------------
# Database
# --------------------------------------------------------------------------
def db() -> sqlite3.Connection:
    if not DB_PATH.exists():
        raise HTTPException(503, f"database not initialised ({DB_PATH})")
    conn = sqlite3.connect(f"file:{DB_PATH}?mode=rw", uri=True, timeout=5.0)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout = 5000")
    return conn


INSERT_TELEMETRY = """
INSERT INTO telemetry (
  device_id, recv_ts, uptime_s, fw_version,
  temperature, temperature_ok, humidity, humidity_ok, co2, co2_ok, co2_available,
  target_temperature, heater_on, heater_pwm, light, light_pct, fan, valve,
  rssi, heap_free, heap_min, heap_int
) VALUES (
  :device_id, :recv_ts, :uptime_s, :fw_version,
  :temperature, :temperature_ok, :humidity, :humidity_ok, :co2, :co2_ok, :co2_available,
  :target_temperature, :heater_on, :heater_pwm, :light, :light_pct, :fan, :valve,
  :rssi, :heap_free, :heap_min, :heap_int
)
"""

UPSERT_DEVICE = """
INSERT INTO devices (device_id, label, first_seen, last_seen, fw_version)
VALUES (:device_id, :label, :now, :now, :fw_version)
ON CONFLICT(device_id) DO UPDATE SET
  last_seen  = excluded.last_seen,
  fw_version = COALESCE(excluded.fw_version, devices.fw_version),
  label      = COALESCE(devices.label, excluded.label)
"""


def _flag(value: Optional[bool]) -> Optional[int]:
    return None if value is None else int(value)


def record_alerts(
    conn: sqlite3.Connection,
    device_id: str,
    now: int,
    payload: TelemetryIn,
    prev: Optional[sqlite3.Row],
) -> None:
    """Record state transitions. Only transitions are stored, never samples.

    Offline is detected here, on the server, rather than by the browser: that
    way the gap is recorded even if nobody has the dashboard open.
    """
    # 1) The device is talking again after a silence longer than the threshold.
    if prev is not None:
        gap = now - int(prev["recv_ts"])
        if gap > OFFLINE_AFTER_S:
            conn.execute(
                "INSERT INTO alerts (device_id, raised_ts, cleared_ts, kind, detail)"
                " VALUES (?, ?, ?, 'offline', ?)",
                (device_id, int(prev["recv_ts"]), now, f"silent for {gap}s"),
            )

    # 2) Sensor validity transitions (fault raised on open, cleared on recovery).
    for column, current, name in (
        ("temperature_ok", payload.temperature_ok, "temperature"),
        ("humidity_ok", payload.humidity_ok, "humidity"),
        ("co2_ok", payload.co2_ok, "co2"),
    ):
        if current is None:
            continue
        # SQLite hands back 0/1 for INTEGER columns, never True/False, and
        # `0 is False` is False in Python. Compare truthiness, not identity,
        # or a fault would be raised and then never cleared.
        previous = None if prev is None else prev[column]
        was_ok = None if previous is None else bool(previous)
        if current is False and was_ok is not False:
            conn.execute(
                "INSERT INTO alerts (device_id, raised_ts, kind, detail)"
                " VALUES (?, ?, 'sensor_fault', ?)",
                (device_id, now, name),
            )
        elif current is True and was_ok is False:
            conn.execute(
                "UPDATE alerts SET cleared_ts = ? WHERE device_id = ?"
                " AND kind = 'sensor_fault' AND detail = ? AND cleared_ts IS NULL",
                (now, device_id, name),
            )

    # 3) Heap floor. heap_int (internal RAM only) is preferred when reported,
    # because getFreeHeap() also counts PSRAM on this board and would hide an
    # internal-heap leak.
    heap = payload.heap_int if payload.heap_int is not None else payload.heap_free
    if heap is not None:
        prev_heap = None
        if prev is not None:
            prev_heap = (
                prev["heap_int"] if prev["heap_int"] is not None else prev["heap_free"]
            )
        low_now = heap < LOW_HEAP_BYTES
        low_before = prev_heap is not None and prev_heap < LOW_HEAP_BYTES
        if low_now and not low_before:
            conn.execute(
                "INSERT INTO alerts (device_id, raised_ts, kind, detail)"
                " VALUES (?, ?, 'low_heap', ?)",
                (device_id, now, f"heap {heap} B"),
            )
        elif low_before and not low_now:
            conn.execute(
                "UPDATE alerts SET cleared_ts = ? WHERE device_id = ?"
                " AND kind = 'low_heap' AND cleared_ts IS NULL",
                (now, device_id),
            )


# --------------------------------------------------------------------------
# Application
# --------------------------------------------------------------------------
# Docs endpoints are disabled: the service is reachable from the public
# internet on a bare IP with no domain to hide behind (REMOTE_MONITORING 7).
app = FastAPI(
    title="Incubator telemetry API",
    version=API_VERSION,
    docs_url=None,
    redoc_url=None,
    openapi_url=None,
)


def _json_safe(value: Any) -> Any:
    """Make a value JSON-serialisable without depending on encoder extras.

    Validation errors can carry arbitrary objects under 'input' and 'ctx'
    (for example a ValueError instance), and FastAPI's jsonable_encoder has no
    hook for those, so sanitise explicitly.
    """
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    return str(value)


@app.exception_handler(RequestValidationError)
async def validation_error(_request: Request, exc: RequestValidationError) -> JSONResponse:
    """The spec asks for 400, not FastAPI's default 422."""
    return JSONResponse(status_code=400, content={"detail": _json_safe(exc.errors())[:4]})


@app.post("/api/v1/telemetry", status_code=204, response_class=Response)
def ingest(payload: TelemetryIn, authed_device: str = Depends(authenticate)) -> Response:
    """Store one sample. The device only looks at the status code."""
    if payload.device_id != authed_device:
        raise HTTPException(403, "device_id does not match the presented token")

    now = int(time.time())
    conn = db()
    try:
        with conn:
            prev = conn.execute(
                "SELECT * FROM telemetry WHERE device_id = ?"
                " ORDER BY recv_ts DESC, id DESC LIMIT 1",
                (authed_device,),
            ).fetchone()

            conn.execute(
                INSERT_TELEMETRY,
                {
                    "device_id": authed_device,
                    "recv_ts": now,
                    "uptime_s": payload.uptime_s,
                    "fw_version": payload.firmware_version,
                    "temperature": payload.temperature,
                    "temperature_ok": _flag(payload.temperature_ok),
                    "humidity": payload.humidity,
                    "humidity_ok": _flag(payload.humidity_ok),
                    "co2": payload.co2,
                    "co2_ok": _flag(payload.co2_ok),
                    "co2_available": _flag(payload.co2_available),
                    "target_temperature": payload.target_temperature,
                    "heater_on": _flag(payload.heater_on),
                    "heater_pwm": payload.heater_pwm,
                    "light": _flag(payload.light),
                    "light_pct": payload.light_pct,
                    "fan": _flag(payload.fan),
                    "valve": _flag(payload.valve),
                    "rssi": payload.rssi,
                    "heap_free": payload.heap_free,
                    "heap_min": payload.heap_min,
                    "heap_int": payload.heap_int,
                },
            )
            conn.execute(
                UPSERT_DEVICE,
                {
                    "device_id": authed_device,
                    "label": token_map().get(authed_device, {}).get("label"),
                    "now": now,
                    "fw_version": payload.firmware_version,
                },
            )
            record_alerts(conn, authed_device, now, payload, prev)
    finally:
        conn.close()
    return Response(status_code=204)


def _device_row(conn: sqlite3.Connection, device_id: str) -> Optional[sqlite3.Row]:
    return conn.execute(
        "SELECT * FROM devices WHERE device_id = ?", (device_id,)
    ).fetchone()


def _label(device_id: str) -> Optional[str]:
    meta = token_map().get(device_id)
    return meta.get("label") if meta else None


@app.get("/api/v1/devices")
def list_devices() -> list[dict[str, Any]]:
    now = int(time.time())
    conn = db()
    try:
        rows = conn.execute(
            "SELECT * FROM devices ORDER BY device_id"
        ).fetchall()
        out: list[dict[str, Any]] = []
        for row in rows:
            age = None if row["last_seen"] is None else now - int(row["last_seen"])
            samples = conn.execute(
                "SELECT COUNT(*) AS n FROM telemetry WHERE device_id = ?",
                (row["device_id"],),
            ).fetchone()["n"]
            out.append(
                {
                    "device_id": row["device_id"],
                    "label": row["label"] or _label(row["device_id"]),
                    "first_seen": row["first_seen"],
                    "last_seen": row["last_seen"],
                    "firmware_version": row["fw_version"],
                    "samples": samples,
                    "age_s": age,
                    "online": age is not None and age <= OFFLINE_AFTER_S,
                }
            )
        return out
    finally:
        conn.close()


@app.get("/api/v1/devices/{device_id}/latest")
def latest(device_id: str) -> dict[str, Any]:
    now = int(time.time())
    conn = db()
    try:
        row = conn.execute(
            "SELECT * FROM telemetry WHERE device_id = ?"
            " ORDER BY recv_ts DESC, id DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        if row is None:
            raise HTTPException(404, f"no telemetry stored for {device_id}")
        device = _device_row(conn, device_id)
        alerts = conn.execute(
            "SELECT raised_ts, kind, detail FROM alerts"
            " WHERE device_id = ? AND cleared_ts IS NULL ORDER BY raised_ts DESC LIMIT 10",
            (device_id,),
        ).fetchall()
    finally:
        conn.close()

    out = dict(row)
    # The ingest payload calls it firmware_version and so does the dashboard;
    # only the database column is named fw_version (REMOTE_MONITORING 5.2).
    out["firmware_version"] = out.pop("fw_version", None)
    age = now - int(row["recv_ts"])
    out["age_s"] = age
    out["online"] = age <= OFFLINE_AFTER_S
    out["publish_interval_s"] = PUBLISH_INTERVAL_S
    out["offline_after_s"] = OFFLINE_AFTER_S
    out["label"] = (device["label"] if device else None) or _label(device_id)
    # Convenience only. co2 is deliberately NOT converted.
    out["heater_pct"] = (
        None if row["heater_pwm"] is None else round(row["heater_pwm"] / 255 * 100, 1)
    )
    out["open_alerts"] = [dict(a) for a in alerts]
    return out


POINT_COLUMNS = (
    "recv_ts",
    "temperature",
    "humidity",
    "co2",
    "target_temperature",
    "heater_pwm",
    "light_pct",
    "valve",
)


@app.get("/api/v1/devices/{device_id}/history")
def history(
    device_id: str,
    hours: float = Query(default=1.0, gt=0, le=24 * 30),
    step: Optional[int] = Query(default=None, ge=5, le=3600),
) -> dict[str, Any]:
    """Raw samples for short windows, 1 minute buckets beyond 6 hours.

    Without downsampling a 24h window is 8640 points, which is enough to make
    the browser unusable and wastes bandwidth over a mobile connection.
    """
    now = int(time.time())
    since = now - int(hours * 3600)
    bucket = step if step is not None else (60 if hours > 6 else 0)
    columns = ", ".join(POINT_COLUMNS)

    conn = db()
    try:
        if bucket:
            rows = conn.execute(
                f"""
                SELECT (recv_ts / :bucket) * :bucket AS recv_ts,
                       AVG(temperature)        AS temperature,
                       AVG(humidity)           AS humidity,
                       AVG(co2)                AS co2,
                       AVG(target_temperature) AS target_temperature,
                       AVG(heater_pwm)         AS heater_pwm,
                       AVG(light_pct)          AS light_pct,
                       MAX(valve)              AS valve
                FROM telemetry
                WHERE device_id = :device_id AND recv_ts >= :since
                GROUP BY recv_ts
                ORDER BY recv_ts
                LIMIT :limit
                """,
                {
                    "bucket": bucket,
                    "device_id": device_id,
                    "since": since,
                    "limit": HISTORY_POINT_LIMIT,
                },
            ).fetchall()
        else:
            rows = conn.execute(
                f"SELECT {columns} FROM telemetry"
                " WHERE device_id = :device_id AND recv_ts >= :since"
                " ORDER BY recv_ts DESC LIMIT :limit",
                {"device_id": device_id, "since": since, "limit": HISTORY_POINT_LIMIT},
            ).fetchall()
            rows = list(reversed(rows))
    finally:
        conn.close()

    points: list[dict[str, Any]] = []
    for row in rows:
        point: dict[str, Any] = {}
        for key in POINT_COLUMNS:
            value = row[key]
            if isinstance(value, float):
                value = round(value, 2)
            elif key == "co2" and value is not None:
                value = int(round(value))
            elif key in ("heater_pwm", "light_pct", "valve") and value is not None:
                value = int(round(value))
            point[key] = value
        points.append(point)

    return {
        "device_id": device_id,
        "from_ts": since,
        "to_ts": now,
        "hours": hours,
        "step_s": bucket or None,
        "count": len(points),
        "points": points,
    }


@app.get("/api/v1/devices/{device_id}/alerts")
def alerts(device_id: str, hours: float = Query(default=24.0, gt=0, le=24 * 30)) -> dict[str, Any]:
    since = int(time.time() - hours * 3600)
    conn = db()
    try:
        rows = conn.execute(
            "SELECT id, raised_ts, cleared_ts, kind, detail FROM alerts"
            " WHERE device_id = ? AND raised_ts >= ? ORDER BY raised_ts DESC LIMIT 500",
            (device_id, since),
        ).fetchall()
    finally:
        conn.close()
    return {"device_id": device_id, "hours": hours, "count": len(rows),
            "alerts": [dict(r) for r in rows]}


@app.get("/healthz")
def healthz() -> dict[str, Any]:
    """Unauthenticated on purpose: used by Nginx and by the operator."""
    info: dict[str, Any] = {
        "status": "ok",
        "version": API_VERSION,
        "time": int(time.time()),
        "db": DB_PATH.exists(),
        "devices_configured": len(token_map()),
    }
    if info["db"]:
        try:
            conn = db()
            try:
                info["devices_seen"] = conn.execute(
                    "SELECT COUNT(*) AS n FROM devices"
                ).fetchone()["n"]
                info["samples"] = conn.execute(
                    "SELECT COUNT(*) AS n FROM telemetry"
                ).fetchone()["n"]
            finally:
                conn.close()
        except HTTPException:
            info["status"] = "degraded"
    else:
        info["status"] = "degraded"
    return info


# Mounted last so that every /api/v1 route above wins the match.
if STATIC_DIR.is_dir():
    app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")
else:
    print(f"[STATIC] {STATIC_DIR} not found; dashboard will not be served", flush=True)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        app,
        host=os.environ.get("INCUBATOR_HOST", "127.0.0.1"),
        port=int(os.environ.get("INCUBATOR_PORT") or 8000),
        log_level="info",
    )