#!/usr/bin/env python3
"""Schedule, timeout and restart checks for the single-camera server."""

from __future__ import annotations

import asyncio
import base64
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
from contextlib import closing
from pathlib import Path

import websockets

SERVER_DIR = Path(__file__).resolve().parents[1]
TOKEN = "s" * 64
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ1cAAAAASUVORK5CYII="
)


def request(base: str, method: str, path: str, payload: dict | None = None) -> tuple[int, bytes]:
    req = urllib.request.Request(base + path, method=method)
    if payload is not None:
        req.data = json.dumps(payload).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def start_server(root: Path, port: int, timeout_s: float = 1) -> tuple[subprocess.Popen, str]:
    env = {
        **os.environ,
        "INCUBATOR_DB": str(root / "data.db"),
        "INCUBATOR_DEVICES": str(root / "devices.json"),
        "INCUBATOR_CAMERA_ID": "camera-001",
        "INCUBATOR_CAMERA_PHOTOS": str(root / "photos"),
        "INCUBATOR_CAMERA_TASK_TIMEOUT_S": str(timeout_s),
    }
    with (root / f"server-{port}.log").open("wb") as log:
        process = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1", "--port", str(port)],
            cwd=SERVER_DIR, env=env, stdout=log, stderr=subprocess.STDOUT,
        )
    base = f"http://127.0.0.1:{port}"
    deadline = time.monotonic() + 15
    while True:
        try:
            if request(base, "GET", "/healthz")[0] == 200:
                return process, base
        except OSError:
            pass
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError((root / f"server-{port}.log").read_text(encoding="utf-8", errors="replace"))
        time.sleep(0.1)


def stop_server(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


async def wait_for(predicate, description: str, timeout: float = 5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = await predicate()
        if value:
            return value
        await asyncio.sleep(0.02)
    raise AssertionError(f"timed out: {description}")


async def main_async(root: Path, base: str, process: subprocess.Popen) -> None:
    checks = 0

    def check(name: str, condition: bool) -> None:
        nonlocal checks
        assert condition, name
        checks += 1
        print(f"[PASS] {name}", flush=True)

    async def http(method: str, path: str, payload: dict | None = None) -> tuple[int, dict | bytes]:
        status, raw = await asyncio.to_thread(request, base, method, path, payload)
        try:
            return status, json.loads(raw)
        except (ValueError, TypeError):
            return status, raw

    async def db_rows(query: str, params: tuple = ()) -> list[tuple]:
        def read() -> list[tuple]:
            with closing(sqlite3.connect(root / "data.db")) as conn:
                return conn.execute(query, params).fetchall()
        return await asyncio.to_thread(read)

    stream = base.replace("http://", "ws://") + "/api/v1/camera-stream"
    async with websockets.connect(stream, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
        check("schedule starts disabled", (await http("GET", "/api/v1/camera/schedule"))[1]["enabled"] is False)
        status, result = await http("PUT", "/api/v1/camera/schedule", {"enabled": True, "interval_s": 1})
        check("enabling schedule succeeds", status == 200 and result["enabled"] is True and result["interval_s"] == 1)
        connected = json.loads(await ws.recv())
        check("camera stream connects", connected["type"] == "connected")
        await ws.send(json.dumps({"type": "hello", "busy": False}))
        check("camera hello acknowledged", json.loads(await ws.recv())["type"] == "hello_ack")

        # The first attempt happened before the camera connected and must be skipped.
        skipped = await wait_for(
            lambda: db_rows("SELECT status FROM camera_captures WHERE source = 'scheduled' ORDER BY created_at"),
            "startup scheduled attempt",
        )
        check("startup attempt is skipped while offline", skipped[0][0] == "skipped")

        async def command():
            try:
                return json.loads(await asyncio.wait_for(ws.recv(), timeout=0.1))
            except asyncio.TimeoutError:
                return None

        scheduled = await wait_for(command, "next scheduled command")
        check("scheduled command arrives after interval", scheduled["type"] == "capture")
        task_id = scheduled["task_id"]
        for state in ("capturing", "uploading"):
            await ws.send(json.dumps({"type": "capture_status", "task_id": task_id, "status": state}))
            check(f"scheduled task accepts {state}", json.loads(await ws.recv())["type"] == "capture_status_ack")
        upload = urllib.request.Request(
            f"{base}/api/v1/camera/captures/{task_id}/image?captured_at={time.time()}",
            method="POST", data=PNG,
            headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "image/png"},
        )
        code = await asyncio.to_thread(lambda: urllib.request.urlopen(upload, timeout=5).status)
        check("scheduled photo uploads", code == 201)

        await ws.send(json.dumps({"type": "heartbeat", "busy": True}))
        check("busy heartbeat acknowledged", json.loads(await ws.recv())["type"] == "heartbeat_ack")
        await asyncio.sleep(1.35)
        skipped_busy = await db_rows(
            "SELECT reason FROM camera_captures WHERE source = 'scheduled' AND status = 'skipped' AND reason = 'camera_busy'"
        )
        check("busy schedule tick is skipped", len(skipped_busy) >= 1)
        await ws.send(json.dumps({"type": "heartbeat", "busy": False}))
        check("idle heartbeat acknowledged", json.loads(await ws.recv())["type"] == "heartbeat_ack")

        status, result = await http("PUT", "/api/v1/camera/schedule", {"enabled": False, "interval_s": 1})
        check("disabling schedule succeeds", status == 200 and result["enabled"] is False)
        before = len(await db_rows("SELECT task_id FROM camera_captures WHERE source = 'scheduled'"))
        await asyncio.sleep(1.25)
        after = len(await db_rows("SELECT task_id FROM camera_captures WHERE source = 'scheduled'"))
        check("disabled schedule creates no new task", before == after)

        status, result = await http("PUT", "/api/v1/camera/schedule", {"enabled": True, "interval_s": 1})
        check("schedule can be re-enabled", status == 200 and result["enabled"] is True)
        command = await wait_for(command, "re-enabled scheduled command")
        active_task = command["task_id"]
        await ws.send(json.dumps({"type": "heartbeat", "busy": True}))
        await ws.recv()
        await asyncio.sleep(1.25)
        # Leave active_task without a status response; the scheduler should timeout it.
        timed = await wait_for(
            lambda: db_rows("SELECT status, reason FROM camera_captures WHERE task_id = ?", (active_task,)),
            "capture timeout record",
        )
        check("capture timeout marks task failed", timed[0] == ("failed", "capture_timeout"))
        await ws.send(json.dumps({"type": "heartbeat", "busy": False}))
        await ws.recv()

        status, result = await http("PUT", "/api/v1/camera/schedule", {"enabled": True, "interval_s": 2})
        check("schedule configuration prepared for restart", status == 200 and result["enabled"] is True)

        status, pending = await http("POST", "/api/v1/camera/captures")
        restart_task_id = pending["task_id"]
        check("unfinished task is created before restart", status == 201 and pending["status"] == "pending")
        # Stop while the camera connection and pending task still exist. Startup must close it as server_restarted.
        await asyncio.to_thread(stop_server, process)

    process, base = start_server(root, int(base.rsplit(":", 1)[1]), timeout_s=1)
    try:
        status, result = await http("GET", "/api/v1/camera/schedule")
        check(
            "enabled schedule configuration survives restart",
            status == 200 and result["enabled"] is True and result["interval_s"] == 2,
        )
        startup_skipped = await db_rows(
            "SELECT status, reason FROM camera_captures WHERE source = 'scheduled' "
            "ORDER BY created_at DESC LIMIT 1"
        )
        check("restored schedule attempts immediately while offline", startup_skipped[0] == ("skipped", "camera_offline"))
        restarted_task = await db_rows(
            "SELECT status, reason FROM camera_captures WHERE task_id = ?", (restart_task_id,)
        )
        check("unfinished task is failed during restart", restarted_task[0] == ("failed", "server_restarted"))

        status, result = await http("PUT", "/api/v1/camera/schedule", {"enabled": False, "interval_s": 2})
        check("restored schedule can be disabled", status == 200 and result["enabled"] is False)
    finally:
        stop_server(process)
    status, rows = await http("GET", "/api/v1/camera/schedule") if False else (None, None)
    print(f"CAMERA SCHEDULE CHECK PASSED ({checks} checks)")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="incubator-camera-schedule-") as sandbox:
        root = Path(sandbox)
        with closing(sqlite3.connect(root / "data.db")) as conn:
            schema = (SERVER_DIR / "schema.sql").read_text(encoding="utf-8")
            conn.executescript(schema)
            conn.commit()
        (root / "devices.json").write_text(json.dumps({"camera-001": {"token": TOKEN}}), encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process, base = start_server(root, port, timeout_s=1)
        try:
            asyncio.run(main_async(root, base, process))
        finally:
            if process.poll() is None:
                stop_server(process)


if __name__ == "__main__":
    main()
