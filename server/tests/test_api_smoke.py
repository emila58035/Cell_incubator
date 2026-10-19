#!/usr/bin/env python3
"""Contract smoke test for server/app.py. Standard library only, no pytest.

    /opt/incubator/app/venv/bin/python server/tests/test_api_smoke.py

Starts a real uvicorn on a loopback port against a throwaway SQLite file in a
temporary directory and drives it over HTTP, so it exercises the same code path
the device and the browser will use (real ASGI server, real status codes, real
204 handling) instead of an in-process test client. Nothing outside the
temporary directory is touched and the server is shut down at the end.

Exits non-zero if any check fails.
"""

from __future__ import annotations

import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER_DIR = HERE.parent
PORT = int(os.environ.get("INCUBATOR_TEST_PORT") or 8099)

SANDBOX = Path(tempfile.mkdtemp(prefix="incubator-apitest-"))
DB_PATH = SANDBOX / "data.db"
TOKENS_PATH = SANDBOX / "devices.json"
LOG_PATH = SANDBOX / "uvicorn.log"

os.environ["INCUBATOR_DB"] = str(DB_PATH)
os.environ["INCUBATOR_DEVICES"] = str(TOKENS_PATH)
os.environ["INCUBATOR_STATIC"] = str(SERVER_DIR / "static")

TOKEN = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
OTHER_TOKEN = "f" * 64
TOKENS_PATH.write_text(
    json.dumps({"incubator-001": {"token": TOKEN, "label": "box one"}}),
    encoding="utf-8",
)

conn = sqlite3.connect(DB_PATH)
# Exercise upgrading a database created before fan PWM support.
conn.executescript((SERVER_DIR / "schema.sql").read_text(encoding="utf-8").replace(
    "  fan_pct     INTEGER,\n", ""
))
conn.commit()
conn.close()

FAILURES: list[str] = []


def check(name: str, condition: bool, extra: object = "") -> None:
    if not condition:
        FAILURES.append(name)
    print(f"[{'PASS' if condition else 'FAIL'}] {name}" + (f"   {extra}" if extra != "" else ""))


def free_to_bind(port: int) -> bool:
    with socket.socket() as probe:
        probe.settimeout(1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def http(method: str, path: str, payload: dict | None = None, token: str | None = TOKEN):
    request = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method=method)
    if payload is not None:
        request.data = json.dumps(payload).encode()
        request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def body(raw: bytes):
    try:
        return json.loads(raw)
    except ValueError:
        return raw


if not free_to_bind(PORT):
    print(f"port {PORT} is already in use; set INCUBATOR_TEST_PORT")
    sys.exit(1)

print(f"sandbox: {SANDBOX}")
log = LOG_PATH.open("wb")
server = subprocess.Popen(
    [sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1",
     "--port", str(PORT), "--log-level", "warning"],
    cwd=str(SERVER_DIR), env=os.environ, stdout=log, stderr=subprocess.STDOUT,
)

try:
    deadline = time.time() + 30
    while True:
        if server.poll() is not None:
            print("server exited during startup:")
            print(LOG_PATH.read_text(encoding="utf-8", errors="replace"))
            sys.exit(1)
        try:
            if http("GET", "/healthz", token=None)[0] == 200:
                break
        except OSError:
            pass
        if time.time() > deadline:
            print("server did not become ready in 30s")
            print(LOG_PATH.read_text(encoding="utf-8", errors="replace"))
            sys.exit(1)
        time.sleep(0.2)

    BASE = {
        "device_id": "incubator-001",
        "uptime_s": 183420,
        "firmware_version": "0.1.0",
        "temperature": 37.02,
        "temperature_ok": True,
        "humidity": 55.3,
        "humidity_ok": True,
        "co2": 51000,
        "co2_ok": True,
        "co2_available": True,
        "target_temperature": 37.0,
        "heater_on": True,
        "heater_pwm": 128,
        "light": False,
        "light_pct": 0,
        "fan": True,
        "fan_pct": 60,
        "valve": False,
        "rssi": -57,
        "heap_free": 210000,
        "heap_min": 180000,
        "heap_int": 120000,
    }

    def post(payload: dict, token: str | None = TOKEN):
        return http("POST", "/api/v1/telemetry", payload, token)

    def latest() -> dict:
        return body(http("GET", "/api/v1/devices/incubator-001/latest")[1])

    print("--- health and auth ---")
    status, raw = http("GET", "/healthz", token=None)
    health = body(raw)
    check("healthz 200", status == 200, health)
    check("healthz sees the database", health["db"] is True)
    check("healthz counts the configured device", health["devices_configured"] == 1)
    check("healthz answers without a token", status == 200)
    check("no docs endpoint exposed", http("GET", "/docs", token=None)[0] == 404)
    check("no openapi endpoint exposed", http("GET", "/openapi.json", token=None)[0] == 404)

    check("missing token -> 401", post(BASE, token=None)[0] == 401)
    check("wrong token -> 401", post(BASE, token=OTHER_TOKEN)[0] == 401)
    check("token/device_id mismatch -> 403",
          post({**BASE, "device_id": "someone-else"})[0] == 403)

    print("--- ingest and read back ---")
    status, raw = post(BASE)
    check("valid sample -> 204", status == 204, status)
    check("204 has an empty body", raw == b"", raw)

    status, raw = http("GET", "/api/v1/devices/incubator-001/latest")
    check("latest 200", status == 200)
    L = body(raw)
    check("temperature stored verbatim", L["temperature"] == 37.02, L["temperature"])
    check("co2 stored without conversion", L["co2"] == 51000, L["co2"])
    check("heater_pwm stored", L["heater_pwm"] == 128)
    check("fan_pct migrated and stored", L["fan_pct"] == 60)
    check("heater_pct derived (128/255)", L["heater_pct"] == 50.2, L["heater_pct"])
    check("device online", L["online"] is True)
    check("label taken from the token file", L["label"] == "box one", L["label"])
    check("uptime_s stored", L["uptime_s"] == 183420)
    check("recv_ts is server time", abs(L["recv_ts"] - int(time.time())) <= 10)
    check("firmware_version stored", L["firmware_version"] == "0.1.0")

    print("--- validation ---")
    status, raw = post({**BASE, "temperature": 200})
    check("out-of-range temperature -> 400", status == 400, status)
    check("400 body names the field", "temperature" in json.dumps(body(raw)), body(raw))
    check("out-of-range humidity -> 400", post({**BASE, "humidity": -5})[0] == 400)
    check("out-of-range co2 -> 400", post({**BASE, "co2": 9999999})[0] == 400)
    check("out-of-range rssi -> 400", post({**BASE, "rssi": 5})[0] == 400)
    check("negative fan_pct -> 400", post({**BASE, "fan_pct": -1})[0] == 400)
    check("fan_pct over 100 -> 400", post({**BASE, "fan_pct": 101})[0] == 400)
    check("oversized device_id -> 400", post({**BASE, "device_id": "x" * 65})[0] == 400)
    check("minimal payload accepted -> 204", post({"device_id": "incubator-001"})[0] == 204)
    check("missing fan_pct stays unknown", latest()["fan_pct"] is None)
    check("unknown field ignored -> 204", post({**BASE, "future_field": 1})[0] == 204)
    check("ignored field created no column", "future_field" not in latest())

    print("--- alert transitions ---")
    check("reporting a fault -> 204", post({**BASE, "temperature_ok": False})[0] == 204)
    kinds = [a["kind"] for a in latest()["open_alerts"]]
    check("sensor_fault raised", kinds.count("sensor_fault") == 1, kinds)

    post(BASE)
    check("sensor_fault cleared on recovery", latest()["open_alerts"] == [],
          latest()["open_alerts"])
    alert_rows = body(http("GET", "/api/v1/devices/incubator-001/alerts")[1])["alerts"]
    check("cleared fault is kept in history",
          any(a["kind"] == "sensor_fault" and a["cleared_ts"] for a in alert_rows),
          [(a["kind"], a["cleared_ts"]) for a in alert_rows])

    post({**BASE, "heap_int": 10000})
    check("low_heap raised", [a["kind"] for a in latest()["open_alerts"]] == ["low_heap"],
          latest()["open_alerts"])
    post(BASE)
    check("low_heap cleared", latest()["open_alerts"] == [], latest()["open_alerts"])

    print("--- offline detection (server side, no browser involved) ---")
    conn = sqlite3.connect(DB_PATH)
    conn.execute("UPDATE telemetry SET recv_ts = recv_ts - 300")
    conn.commit()
    conn.close()
    post(BASE)
    offline = [a for a in body(http("GET", "/api/v1/devices/incubator-001/alerts")[1])["alerts"]
               if a["kind"] == "offline"]
    check("offline gap recorded", len(offline) == 1, offline)
    check("offline gap marked as recovered",
          bool(offline) and offline[0]["cleared_ts"] is not None)
    check("offline detail reports the gap",
          bool(offline) and "silent for" in (offline[0]["detail"] or ""),
          offline[0]["detail"] if offline else None)

    print("--- devices list ---")
    devices = body(http("GET", "/api/v1/devices")[1])
    check("one device listed", len(devices) == 1, devices)
    check("sample count matches", devices[0]["samples"] == 8, devices[0]["samples"])
    check("device online", devices[0]["online"] is True)

    print("--- history and downsampling ---")
    hist = body(http("GET", "/api/v1/devices/incubator-001/history?hours=1")[1])
    check("history 1h is raw (no bucket)", hist["step_s"] is None, hist["step_s"])
    check("history returns points", hist["count"] == 8, hist["count"])
    check("raw history includes fan_pct", any(p["fan_pct"] == 60 for p in hist["points"]))
    stamps = [p["recv_ts"] for p in hist["points"]]
    check("history points ascend", stamps == sorted(stamps))
    hist24 = body(http("GET", "/api/v1/devices/incubator-001/history?hours=24")[1])
    check("history 24h uses 60s buckets", hist24["step_s"] == 60, hist24["step_s"])
    check("24h window covers the backdated rows", hist24["count"] >= 1, hist24["count"])
    check("bucketed history includes fan_pct", any(p["fan_pct"] == 60 for p in hist24["points"]))
    explicit = body(http("GET", "/api/v1/devices/incubator-001/history?hours=2&step=300")[1])
    check("explicit step honoured", explicit["step_s"] == 300, explicit["step_s"])
    csv_status, csv_raw = http("GET", "/api/v1/devices/incubator-001/export.csv?hours=1")
    check("CSV export succeeds", csv_status == 200, csv_status)
    check("CSV export has requested columns",
          csv_raw.decode("utf-8-sig").splitlines()[0] ==
          "timestamp,device_id,temperature,humidity,co2,temperature_ok,humidity_ok,co2_ok")
    check("history rejects hours<=0",
          http("GET", "/api/v1/devices/incubator-001/history?hours=0")[0] == 400)
    check("history rejects step<5",
          http("GET", "/api/v1/devices/incubator-001/history?step=1")[0] == 400)

    print("--- not found and static dashboard ---")
    check("unknown device latest -> 404",
          http("GET", "/api/v1/devices/nope/latest")[0] == 404)
    check("unknown device history is empty, not an error",
          body(http("GET", "/api/v1/devices/nope/history")[1])["count"] == 0)
    status, raw = http("GET", "/", token=None)
    check("dashboard served at /", status == 200, status)
    html = raw.decode("utf-8", errors="replace")
    check("dashboard is HTML", "<!doctype html>" in html.lower())
    check("dashboard contains no hardcoded device id", "incubator-001" not in html)
    check("dashboard loads no external resource",
          "http://" not in html and "https://" not in html)
finally:
    server.terminate()
    try:
        server.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
    log.close()

print()
if FAILURES:
    print(f"FAILED: {len(FAILURES)} check(s)")
    for name in FAILURES:
        print(f"  - {name}")
    print(f"server log: {LOG_PATH}")
    sys.exit(1)

print("ALL CHECKS PASSED")
print(f"server stderr/stdout: {LOG_PATH.read_text(encoding='utf-8', errors='replace').strip() or '(empty)'}")
