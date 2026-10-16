#!/usr/bin/env python3
"""Real HTTP/WebSocket capture checks using a temporary DB and simulated camera."""

from __future__ import annotations

import asyncio
import base64
from contextlib import closing
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

import websockets

SERVER_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER_DIR.parent / "camera"))
from simulator import SimulatedCamera

TOKEN = "c" * 64
ESP_TOKEN = "e" * 64
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ1cAAAAASUVORK5CYII="
)


async def run(root: Path, base: str) -> None:
    checks = 0

    def check(name: str, condition: bool) -> None:
        nonlocal checks
        assert condition, name
        checks += 1
        print(f"[PASS] {name}", flush=True)

    async def http(
        method: str, path: str, data: bytes | None = None,
        token: str | None = None, content_type: str = "image/png",
    ) -> tuple[int, bytes]:
        def request() -> tuple[int, bytes]:
            headers = {"Content-Type": content_type}
            if token:
                headers["Authorization"] = f"Bearer {token}"
            req = urllib.request.Request(base + path, method=method, data=data, headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=5) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as exc:
                return exc.code, exc.read()
        return await asyncio.to_thread(request)

    async def state(task_id: str) -> dict:
        code, raw = await http("GET", f"/api/v1/camera/captures/{task_id}")
        assert code == 200
        return json.loads(raw)

    async def wait_for(predicate, description: str):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            value = await predicate()
            if value:
                return value
            await asyncio.sleep(0.02)
        raise AssertionError(f"timed out: {description}")

    async def ready() -> bool:
        _, raw = await http("GET", "/api/v1/camera/status")
        status = json.loads(raw)
        return status["ready"] and not status["busy"]

    code, raw = await http("POST", "/api/v1/camera/captures", b"")
    rejected = json.loads(raw)["detail"]
    check("offline request fails immediately and stores reason", code == 503 and
          (await state(rejected["task_id"]))["reason"] == "camera_offline")
    check("unknown task is 404", (await http("GET", "/api/v1/camera/captures/missing"))[0] == 404)
    check("unknown photo is 404", (await http("GET", "/api/v1/camera/photos/missing/file"))[0] == 404)

    stream_url = base.replace("http://", "ws://") + "/api/v1/camera-stream"
    for token in ("wrong", ESP_TOKEN):
        try:
            async with websockets.connect(stream_url, additional_headers={"Authorization": f"Bearer {token}"}):
                raise AssertionError("unauthorized camera connected")
        except websockets.exceptions.InvalidStatus as exc:
            check("non-camera token cannot connect", exc.response.status_code == 403)

    image = root / "simulated.png"
    image.write_bytes(PNG)
    temporary = root / "camera-temp"
    simulator = SimulatedCamera(base, TOKEN, image, temporary, capture_delay_s=0.15)
    simulator_task = asyncio.create_task(simulator.run())
    try:
        await wait_for(ready, "simulator ready")
        responses = await asyncio.gather(*[
            http("POST", "/api/v1/camera/captures", b"") for _ in range(3)
        ])
        check("concurrent requests accept exactly one capture", sorted(r[0] for r in responses) == [201, 409, 409])
        task_id = json.loads(next(raw for code, raw in responses if code == 201))["task_id"]
        for code, raw in responses:
            if code == 409:
                rejected_id = json.loads(raw)["detail"]["task_id"]
                check("busy rejection has a failed task record", (await state(rejected_id))["reason"] == "camera_busy")

        async def completed():
            task = await state(task_id)
            return task if task["status"] == "completed" else None
        await wait_for(completed, "uploaded photo")
        await wait_for(ready, "camera becomes idle")
        with closing(sqlite3.connect(root / "data.db")) as conn:
            photo_id, captured_at, filename = conn.execute(
                "SELECT photo_id, captured_at, filename FROM camera_photos WHERE task_id = ?", (task_id,),
            ).fetchone()
        check("stored filename contains camera, capture time and task", filename.startswith("camera-001-") and task_id in filename)
        check("capture timestamp is stored", abs(captured_at - time.time()) < 10)
        check("temporary camera photo is deleted", not list(temporary.iterdir()))
        code, raw = await http("GET", f"/api/v1/camera/photos/{photo_id}")
        check("photo information is available", code == 200 and json.loads(raw)["task_id"] == task_id)
        check("inline original is byte-for-byte unchanged", await http("GET", f"/api/v1/camera/photos/{photo_id}/file") == (200, PNG))
        check("download original is unchanged", await http("GET", f"/api/v1/camera/photos/{photo_id}/file?download=true") == (200, PNG))
        upload_path = f"/api/v1/camera/captures/{task_id}/image?captured_at={time.time()}"
        check("upload requires bearer token", (await http("POST", upload_path, PNG))[0] == 401)
        check("ESP32 token cannot upload", (await http("POST", upload_path, PNG, ESP_TOKEN))[0] == 403)
        check("completed task cannot upload twice", (await http("POST", upload_path, PNG, TOKEN))[0] == 409)
        with closing(sqlite3.connect(root / "data.db")) as conn:
            check("duplicate upload creates no extra record", conn.execute("SELECT COUNT(*) FROM camera_photos").fetchone()[0] == 1)
    finally:
        simulator_task.cancel()
        await asyncio.gather(simulator_task, return_exceptions=True)

    async def disconnected():
        _, raw = await http("GET", "/api/v1/camera/status")
        return not json.loads(raw)["online"]
    await wait_for(disconnected, "camera disconnect")

    async with websockets.connect(stream_url, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "hello", "busy": False}))
        await ws.recv()

        async def start_upload() -> tuple[str, str]:
            code, raw = await http("POST", "/api/v1/camera/captures", b"")
            assert code == 201, raw
            task_id = json.loads(raw)["task_id"]
            command = json.loads(await ws.recv())
            assert command == {"type": "capture", "task_id": task_id}
            for status in ("capturing", "uploading"):
                await ws.send(json.dumps({"type": "capture_status", "task_id": task_id, "status": status}))
                assert json.loads(await ws.recv())["type"] == "capture_status_ack"
                check(f"task reports {status}", (await state(task_id))["status"] == status)
            return task_id, f"/api/v1/camera/captures/{task_id}/image?captured_at={time.time()}"

        for payload, content_type, expected in (
            (b"not an image", "image/png", 400),
            (PNG, "text/plain", 415),
            (PNG + b"x" * 4096, "image/png", 413),
        ):
            task_id, path = await start_upload()
            code, _ = await http("POST", path, payload, TOKEN, content_type)
            check(f"bad upload fails with {expected}", code == expected and (await state(task_id))["status"] == "failed")
            check("failed upload leaves no partial file", not list((root / "photos").glob("*.part")))
            await ws.send(json.dumps({"type": "heartbeat", "busy": False}))
            await ws.recv()

        task_id, _ = await start_upload()
        await ws.send(json.dumps({"type": "capture_status", "task_id": task_id, "status": "failed", "reason": "camera unavailable"}))
        await ws.recv()
        check("camera failure reason is stored", (await state(task_id))["reason"] == "camera unavailable")
        await ws.send(json.dumps({"type": "heartbeat", "busy": True}))
        await ws.recv()
        check("physical busy state blocks capture after failure", (await http("POST", "/api/v1/camera/captures", b""))[0] == 409)
        await ws.send(json.dumps({"type": "heartbeat", "busy": False}))
        await ws.recv()
        task_id, late_path = await start_upload()

    await wait_for(disconnected, "raw camera disconnect")
    check("disconnect terminates pending capture", (await state(task_id))["reason"] == "camera_disconnected")
    check("late upload is rejected", (await http("POST", late_path, PNG, TOKEN))[0] == 409)

    # Same simulator failure path must delete its local file and must not retry.
    image.write_bytes(b"invalid PNG")
    simulator_task = asyncio.create_task(SimulatedCamera(base, TOKEN, image, temporary).run())
    try:
        await wait_for(ready, "failure simulator ready")
        _, raw = await http("POST", "/api/v1/camera/captures", b"")
        task_id = json.loads(raw)["task_id"]
        async def failed():
            return (await state(task_id))["status"] == "failed"
        await wait_for(failed, "upload failure")
        await wait_for(ready, "failure cleanup")
        check("failed simulator upload deletes local photo", not list(temporary.iterdir()))
        with closing(sqlite3.connect(root / "data.db")) as conn:
            check("failure does not save a new photo", conn.execute("SELECT COUNT(*) FROM camera_photos").fetchone()[0] == 1)
        check("only successfully saved original remains", len(list((root / "photos").iterdir())) == 1)
    finally:
        simulator_task.cancel()
        await asyncio.gather(simulator_task, return_exceptions=True)
    print(f"CAMERA CAPTURE CHECK PASSED ({checks} checks)")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="incubator-camera-test-") as sandbox:
        root = Path(sandbox)
        with closing(sqlite3.connect(root / "data.db")) as conn:
            schema = (SERVER_DIR / "schema.sql").read_text(encoding="utf-8")
            conn.executescript(schema)
            conn.executescript(schema)  # Deployment must remain repeatable.
        (root / "devices.json").write_text(json.dumps({
            "camera-001": {"token": TOKEN}, "incubator-001": {"token": ESP_TOKEN},
        }), encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        env = {**os.environ,
               "INCUBATOR_DB": str(root / "data.db"),
               "INCUBATOR_DEVICES": str(root / "devices.json"),
               "INCUBATOR_CAMERA_ID": "camera-001",
               "INCUBATOR_CAMERA_PHOTOS": str(root / "photos"),
               "INCUBATOR_CAMERA_MAX_BYTES": "4096"}
        with (root / "server.log").open("wb") as log:
            server = subprocess.Popen(
                [sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1", "--port", str(port)],
                cwd=SERVER_DIR, env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        with urllib.request.urlopen(base + "/healthz", timeout=1):
                            break
                    except OSError:
                        if server.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("test server did not start")
                        time.sleep(0.1)
                asyncio.run(run(root, base))
            except BaseException:
                print((root / "server.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


if __name__ == "__main__":
    main()
