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

import csv
import hmac
import io
import json
import os
import sqlite3
import time
import uuid
from pathlib import Path
from typing import Any, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from camera_api import CameraAPI

API_VERSION = "1.2.0"
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
COMMAND_TTL_S = 60
COMMAND_TYPES = {"set_temperature", "set_heater", "set_light", "set_fan", "set_valve_enabled"}
WS_HEARTBEAT_TIMEOUT_S = 45
ws_clients: dict[str, WebSocket] = {}
ws_seen: dict[str, int] = {}


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
    valve_enabled: Optional[bool] = None
    valve: Optional[bool] = None

    rssi: Optional[int] = Field(default=None, ge=-120, le=0)
    heap_free: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)
    heap_min: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)
    heap_int: Optional[int] = Field(default=None, ge=0, le=HEAP_MAX)


class CommandIn(BaseModel):
    command: str = Field(min_length=1, max_length=32)
    params: dict[str, Any] = Field(default_factory=dict)
    expires_in: int = Field(default=COMMAND_TTL_S, ge=5, le=3600)


def _command_valid(command: str, params: dict[str, Any]) -> Optional[str]:
    if command not in COMMAND_TYPES:
        return "unknown_command"
    if command == "set_temperature":
        value = params.get("value")
        if not isinstance(value, (int, float)) or isinstance(value, bool) or not 10 <= float(value) <= 100:
            return "temperature_out_of_range"
    elif command == "set_light":
        if not isinstance(params.get("enabled"), bool):
            return "enabled_must_be_boolean"
        brightness = params.get("brightness", 100 if params["enabled"] else 0)
        if not isinstance(brightness, int) or isinstance(brightness, bool) or not 0 <= brightness <= 100:
            return "brightness_out_of_range"
    elif command in {"set_heater", "set_fan", "set_valve_enabled"}:
        if not isinstance(params.get("enabled"), bool):
            return "enabled_must_be_boolean"
    return None


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
    columns = {row[1] for row in conn.execute("PRAGMA table_info(telemetry)")}
    if "valve_enabled" not in columns:
        with conn:
            conn.execute("ALTER TABLE telemetry ADD COLUMN valve_enabled INTEGER")
    return conn


INSERT_TELEMETRY = """
INSERT INTO telemetry (
  device_id, recv_ts, uptime_s, fw_version,
  temperature, temperature_ok, humidity, humidity_ok, co2, co2_ok, co2_available,
  target_temperature, heater_on, heater_pwm, light, light_pct, fan, valve_enabled, valve,
  rssi, heap_free, heap_min, heap_int
) VALUES (
  :device_id, :recv_ts, :uptime_s, :fw_version,
  :temperature, :temperature_ok, :humidity, :humidity_ok, :co2, :co2_ok, :co2_available,
  :target_temperature, :heater_on, :heater_pwm, :light, :light_pct, :fan, :valve_enabled, :valve,
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

    for column, current, name in (
        ("temperature_ok", payload.temperature_ok, "temperature"),
        ("humidity_ok", payload.humidity_ok, "humidity"),
        ("co2_ok", payload.co2_ok, "co2"),
    ):
        if current is None:
            continue
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

    heap = payload.heap_int if payload.heap_int is not None else payload.heap_free
    if heap is not None:
        prev_heap = None
        if prev is not None:
            prev_heap = prev["heap_int"] if prev["heap_int"] is not None else prev["heap_free"]
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


def _store_telemetry(payload: TelemetryIn, authed_device: str) -> None:
    if payload.device_id != authed_device:
        raise HTTPException(403, "device_id does not match the presented token")
    now = int(time.time())
    conn = db()
    try:
        with conn:
            prev = conn.execute(
                "SELECT * FROM telemetry WHERE device_id = ? ORDER BY recv_ts DESC, id DESC LIMIT 1",
                (authed_device,),
            ).fetchone()
            conn.execute(
                INSERT_TELEMETRY,
                {
                    "device_id": authed_device, "recv_ts": now,
                    "uptime_s": payload.uptime_s, "fw_version": payload.firmware_version,
                    "temperature": payload.temperature, "temperature_ok": _flag(payload.temperature_ok),
                    "humidity": payload.humidity, "humidity_ok": _flag(payload.humidity_ok),
                    "co2": payload.co2, "co2_ok": _flag(payload.co2_ok),
                    "co2_available": _flag(payload.co2_available),
                    "target_temperature": payload.target_temperature,
                    "heater_on": _flag(payload.heater_on), "heater_pwm": payload.heater_pwm,
                    "light": _flag(payload.light), "light_pct": payload.light_pct,
                    "fan": _flag(payload.fan), "valve_enabled": _flag(payload.valve_enabled),
                    "valve": _flag(payload.valve),
                    "rssi": payload.rssi, "heap_free": payload.heap_free,
                    "heap_min": payload.heap_min, "heap_int": payload.heap_int,
                },
            )
            conn.execute(UPSERT_DEVICE, {
                "device_id": authed_device,
                "label": token_map().get(authed_device, {}).get("label"),
                "now": now, "fw_version": payload.firmware_version,
            })
            record_alerts(conn, authed_device, now, payload, prev)
    finally:
        conn.close()
    ws_seen[authed_device] = now



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
    _store_telemetry(payload, authed_device)
    return Response(status_code=204)


def _token_from_websocket(websocket: WebSocket) -> Optional[str]:
    header = websocket.headers.get("authorization")
    if not header or not header.lower().startswith("bearer "):
        return None
    presented = header[7:].strip()
    matched = None
    for device_id, meta in token_map().items():
        if hmac.compare_digest(presented, meta["token"]):
            matched = device_id
    return matched


async def _send_pending_commands(device_id: str, websocket: WebSocket) -> None:
    now = int(time.time())
    conn = db()
    try:
        rows = conn.execute(
            "SELECT command_id, command, params_json, expires_ts FROM remote_commands "
            "WHERE device_id = ? AND status IN ('queued', 'sent') AND expires_ts > ? "
            "ORDER BY created_ts LIMIT 20", (device_id, now)
        ).fetchall()
        with conn:
            conn.execute(
                "UPDATE remote_commands SET status = 'expired' WHERE device_id = ? "
                "AND status IN ('queued', 'sent') AND expires_ts <= ?", (device_id, now)
            )
            for row in rows:
                await websocket.send_json({
                    "type": "command", "command_id": row["command_id"],
                    "command": row["command"], "params": json.loads(row["params_json"]),
                    "expires_at": row["expires_ts"],
                })
                conn.execute(
                    "UPDATE remote_commands SET status = 'sent', sent_ts = COALESCE(sent_ts, ?) "
                    "WHERE command_id = ?", (now, row["command_id"])
                )
    finally:
        conn.close()


@app.websocket("/api/v1/device-stream")
async def device_stream(websocket: WebSocket) -> None:
    device_id = _token_from_websocket(websocket)
    if device_id is None:
        await websocket.close(code=1008, reason="invalid bearer token")
        return
    await websocket.accept()
    old = ws_clients.get(device_id)
    if old is not None:
        try:
            await old.close(code=1000, reason="replaced by newer connection")
        except Exception:
            pass
    ws_clients[device_id] = websocket
    ws_seen[device_id] = int(time.time())
    try:
        await websocket.send_json({"type": "connected", "device_id": device_id})
        while True:
            try:
                message = await websocket.receive_json()
            except ValueError:
                await websocket.send_json({"type": "error", "reason": "invalid_json"})
                continue
            kind = message.get("type")
            ws_seen[device_id] = int(time.time())
            if kind == "hello":
                await websocket.send_json({"type": "hello_ack", "device_id": device_id})
                await _send_pending_commands(device_id, websocket)
            elif kind == "heartbeat":
                await websocket.send_json({"type": "heartbeat_ack", "server_ts": int(time.time())})
            elif kind == "telemetry":
                payload_data = dict(message)
                payload_data.pop("type", None)
                try:
                    payload = TelemetryIn.model_validate(payload_data)
                    _store_telemetry(payload, device_id)
                    await _send_pending_commands(device_id, websocket)
                except Exception:
                    await websocket.send_json({"type": "error", "reason": "invalid_telemetry"})
            elif kind == "command_ack":
                command_id = str(message.get("command_id", ""))
                status = str(message.get("status", ""))
                if command_id and status in {"applied", "rejected"}:
                    conn = db()
                    try:
                        with conn:
                            conn.execute(
                                "UPDATE remote_commands SET status = ?, ack_ts = ?, ack_reason = ? "
                                "WHERE command_id = ? AND device_id = ? AND status IN ('queued', 'sent')",
                                (status, int(time.time()), message.get("reason"), command_id, device_id),
                            )
                    finally:
                        conn.close()
    except WebSocketDisconnect:
        pass
    finally:
        if ws_clients.get(device_id) is websocket:
            ws_clients.pop(device_id, None)
            ws_seen.pop(device_id, None)


@app.post("/api/v1/devices/{device_id}/commands")
async def create_command(
    device_id: str,
    payload: CommandIn,
    x_requested_with: Optional[str] = Header(default=None),
) -> dict[str, Any]:
    if x_requested_with != "dashboard":
        raise HTTPException(403, "missing control request header")
    if device_id not in token_map():
        raise HTTPException(404, f"unknown device {device_id}")
    reason = _command_valid(payload.command, payload.params)
    if reason:
        raise HTTPException(400, reason)
    now = int(time.time())
    command_id = str(uuid.uuid4())
    expires_ts = now + payload.expires_in
    conn = db()
    try:
        with conn:
            conn.execute(
                "INSERT INTO remote_commands (command_id, device_id, command, params_json, status, "
                "created_ts, expires_ts, requested_by) VALUES (?, ?, ?, ?, 'queued', ?, ?, ?)",
                (command_id, device_id, payload.command, json.dumps(payload.params), now, expires_ts, "dashboard"),
            )
    finally:
        conn.close()
    websocket = ws_clients.get(device_id)
    delivered = False
    if websocket is not None and now - ws_seen.get(device_id, 0) <= WS_HEARTBEAT_TIMEOUT_S:
        try:
            await websocket.send_json({
                "type": "command", "command_id": command_id, "command": payload.command,
                "params": payload.params, "expires_at": expires_ts,
            })
            delivered = True
            conn = db()
            try:
                with conn:
                    conn.execute("UPDATE remote_commands SET status = 'sent', sent_ts = ? WHERE command_id = ?", (now, command_id))
            finally:
                conn.close()
        except Exception:
            pass
    return {"command_id": command_id, "status": "sent" if delivered else "queued", "expires_at": expires_ts}


@app.get("/api/v1/devices/{device_id}/commands/{command_id}")
def command_status(device_id: str, command_id: str) -> dict[str, Any]:
    conn = db()
    try:
        row = conn.execute("SELECT * FROM remote_commands WHERE device_id = ? AND command_id = ?", (device_id, command_id)).fetchone()
    finally:
        conn.close()
    if row is None:
        raise HTTPException(404, "command not found")
    out = dict(row)
    out["params"] = json.loads(out.pop("params_json"))
    return out


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
                    "ws_connected": row["device_id"] in ws_clients and now - ws_seen.get(row["device_id"], 0) <= WS_HEARTBEAT_TIMEOUT_S,
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
    out["ws_connected"] = device_id in ws_clients and now - ws_seen.get(device_id, 0) <= WS_HEARTBEAT_TIMEOUT_S
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
    "valve_enabled",
    "valve",
)


def history_window(hours: float, from_ts: Optional[int], to_ts: Optional[int]) -> tuple[int, int]:
    if from_ts is None and to_ts is None:
        end = int(time.time())
        return end - int(hours * 3600), end
    if from_ts is None or to_ts is None or not 0 < to_ts - from_ts <= 30 * 86400:
        raise HTTPException(400, "provide from_ts and to_ts with a range of at most 30 days")
    return from_ts, to_ts


@app.get("/api/v1/devices/{device_id}/history")
def history(
    device_id: str,
    hours: float = Query(default=1.0, gt=0, le=24 * 30),
    step: Optional[int] = Query(default=None, ge=5, le=3600),
    from_ts: Optional[int] = Query(default=None, ge=0),
    to_ts: Optional[int] = Query(default=None, ge=0),
    cursor: Optional[str] = Query(default=None, max_length=64),
    snapshot_id: Optional[int] = Query(default=None, ge=0),
    limit: int = Query(default=2000, ge=1, le=5000),
) -> dict[str, Any]:
    """Raw samples in timestamp/id order, with a fixed snapshot across pages.

    Repeat from_ts, to_ts and snapshot_id with next_cursor to read all pages.
    Explicit step still enables the legacy aggregate query.
    """
    since, now = history_window(hours, from_ts, to_ts)
    bucket = step or 0
    columns = ", ".join(POINT_COLUMNS)
    after_ts, after_id = -1, 0
    if cursor is not None:
        try:
            after_ts, after_id = map(int, cursor.split(":"))
            if after_ts < 0 or after_id < 1:
                raise ValueError
        except ValueError:
            raise HTTPException(400, "invalid history cursor")
    if bucket and (cursor is not None or snapshot_id is not None):
        raise HTTPException(400, "pagination applies to raw history only")

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
                       MAX(valve_enabled)      AS valve_enabled,
                       MAX(valve)              AS valve
                FROM telemetry
                WHERE device_id = :device_id AND recv_ts BETWEEN :since AND :until
                GROUP BY recv_ts
                ORDER BY recv_ts
                LIMIT :limit
                """,
                {
                    "bucket": bucket,
                    "device_id": device_id,
                    "since": since,
                    "until": now,
                    "limit": HISTORY_POINT_LIMIT,
                },
            ).fetchall()
        else:
            if snapshot_id is None:
                snapshot_id = conn.execute(
                    "SELECT COALESCE(MAX(id), 0) FROM telemetry",
                ).fetchone()[0]
            rows = conn.execute(
                f"SELECT id AS sample_id, {columns} FROM telemetry"
                " WHERE device_id = :device_id AND recv_ts BETWEEN :since AND :until"
                " AND id <= :snapshot_id AND (recv_ts, id) > (:after_ts, :after_id)"
                " ORDER BY recv_ts, id LIMIT :limit",
                {"device_id": device_id, "since": since, "until": now,
                 "snapshot_id": snapshot_id, "after_ts": after_ts, "after_id": after_id,
                 "limit": limit + 1},
            ).fetchall()
    finally:
        conn.close()

    points: list[dict[str, Any]] = []
    has_more = not bucket and len(rows) > limit
    if not bucket:
        rows = rows[:limit]
    for row in rows:
        if not bucket:
            points.append(dict(row))
            continue
        point: dict[str, Any] = {}
        for key in POINT_COLUMNS:
            value = row[key]
            if isinstance(value, float):
                value = round(value, 2)
            elif key == "co2" and value is not None:
                value = int(round(value))
            elif key in ("heater_pwm", "light_pct", "valve_enabled", "valve") and value is not None:
                value = int(round(value))
            point[key] = value
        points.append(point)

    return {
        "device_id": device_id,
        "from_ts": since,
        "to_ts": now,
        "hours": (now - since) / 3600,
        "step_s": bucket or None,
        "count": len(points),
        "points": points,
        "snapshot_id": snapshot_id,
        "last_cursor": f"{rows[-1]['recv_ts']}:{rows[-1]['sample_id']}" if rows and not bucket else cursor,
        "next_cursor": f"{rows[-1]['recv_ts']}:{rows[-1]['sample_id']}" if has_more else None,
    }


@app.get("/api/v1/devices/{device_id}/export.csv")
def export_csv(
    device_id: str,
    hours: float = Query(default=24.0, gt=0, le=24 * 30),
    from_ts: Optional[int] = Query(default=None, ge=0),
    to_ts: Optional[int] = Query(default=None, ge=0),
) -> Response:
    since, now = history_window(hours, from_ts, to_ts)
    conn = db()
    try:
        rows = conn.execute(
            "SELECT recv_ts, temperature, humidity, co2, temperature_ok, humidity_ok, co2_ok "
            "FROM telemetry WHERE device_id = ? AND recv_ts BETWEEN ? AND ? ORDER BY recv_ts, id",
            (device_id, since, now),
        ).fetchall()
    finally:
        conn.close()
    output = io.StringIO(newline="")
    writer = csv.writer(output)
    writer.writerow(["timestamp", "device_id", "temperature", "humidity", "co2", "temperature_ok", "humidity_ok", "co2_ok"])
    for row in rows:
        writer.writerow([
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(row["recv_ts"])), device_id,
            row["temperature"], row["humidity"], row["co2"], row["temperature_ok"],
            row["humidity_ok"], row["co2_ok"],
        ])
    return Response(
        content="\ufeff" + output.getvalue(), media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{device_id}-{now}.csv"'},
    )


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


camera_api = CameraAPI(
    database=db,
    authenticate=authenticate,
    websocket_authenticate=_token_from_websocket,
    photos_dir=Path(os.environ.get("INCUBATOR_CAMERA_PHOTOS") or (DB_PATH.parent / "photos")),
)
app.include_router(camera_api.router)


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
