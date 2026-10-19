#!/usr/bin/env python3
"""Small WebSocket command-path check for the local server."""

from __future__ import annotations

import asyncio
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
from pathlib import Path

import websockets

SERVER_DIR = Path(__file__).resolve().parents[1]
SANDBOX = Path(tempfile.mkdtemp(prefix="incubator-ws-test-"))
DB_PATH = SANDBOX / "data.db"
TOKENS_PATH = SANDBOX / "devices.json"
PORT = 8111
TOKEN = "t" * 64
TOKENS_PATH.write_text(json.dumps({"incubator-001": {"token": TOKEN}}), encoding="utf-8")
conn = sqlite3.connect(DB_PATH)
conn.executescript((SERVER_DIR / "schema.sql").read_text(encoding="utf-8"))
conn.commit()
conn.close()

env = os.environ.copy()
env.update({
    "INCUBATOR_DB": str(DB_PATH),
    "INCUBATOR_DEVICES": str(TOKENS_PATH),
    "INCUBATOR_STATIC": str(SERVER_DIR / "static"),
})
server = subprocess.Popen(
    [sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1", "--port", str(PORT)],
    cwd=SERVER_DIR,
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)


async def run() -> None:
    await asyncio.sleep(0.6)
    headers = {"Authorization": f"Bearer {TOKEN}"}
    async with websockets.connect(
        f"ws://127.0.0.1:{PORT}/api/v1/device-stream", additional_headers=headers
    ) as socket:
        connected = json.loads(await socket.recv())
        assert connected["type"] == "connected"
        await socket.send(json.dumps({"type": "hello"}))
        assert json.loads(await socket.recv())["type"] == "hello_ack"

        def request_for(params: dict) -> urllib.request.Request:
            return urllib.request.Request(
                f"http://127.0.0.1:{PORT}/api/v1/devices/incubator-001/commands",
                method="POST",
                data=json.dumps({"command": "set_fan", "params": params}).encode(),
                headers={"Content-Type": "application/json", "X-Requested-With": "dashboard"},
            )

        for params in (
            {"enabled": True}, {"enabled": False},
            *({"enabled": True, "power": power} for power in (0, 5, 60, 100)),
            {"enabled": False, "power": 60},
        ):
            with urllib.request.urlopen(request_for(params), timeout=5) as response:
                command = json.load(response)
            pushed = json.loads(await socket.recv())
            assert pushed["command_id"] == command["command_id"]
            assert pushed["command"] == "set_fan"
            assert pushed["params"] == params

            await socket.send(json.dumps({
                "type": "command_ack", "command_id": command["command_id"], "status": "applied"
            }))
            await asyncio.sleep(0.1)
            status_request = urllib.request.Request(
                f"http://127.0.0.1:{PORT}/api/v1/devices/incubator-001/commands/{command['command_id']}"
            )
            with urllib.request.urlopen(status_request, timeout=5) as response:
                status = json.load(response)
            assert status["status"] == "applied"

        for power in (-1, 101, True, 1.5, "60", None):
            try:
                urllib.request.urlopen(request_for({"enabled": True, "power": power}), timeout=5)
            except urllib.error.HTTPError as exc:
                assert exc.code == 400
            else:
                raise AssertionError(f"invalid fan power accepted: {power!r}")


try:
    asyncio.run(run())
    print("WEBSOCKET CONTROL CHECK PASSED")
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
