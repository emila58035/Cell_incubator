#!/usr/bin/env python3
"""Development camera: upload a supplied test image, without camera hardware."""

from __future__ import annotations

import argparse
import asyncio
import json
import mimetypes
import os
import ssl
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urlencode, urlsplit, urlunsplit

import websockets


class SimulatedCamera:
    def __init__(
        self, server_url: str, token: str, image: Path, temp_dir: Path,
        ca_file: str | None = None, capture_delay_s: float = 0,
    ) -> None:
        self.server_url = server_url.rstrip("/")
        self.token = token
        self.image = image
        self.temp_dir = temp_dir
        self.capture_delay_s = capture_delay_s
        self.ssl_context = ssl.create_default_context(cafile=ca_file)
        self.socket = None
        self.busy = False
        self.acks: dict[str, asyncio.Future] = {}

    async def update(self, task_id: str, status: str, reason: str | None = None) -> None:
        waiter = asyncio.get_running_loop().create_future()
        self.acks[task_id] = waiter
        try:
            await self.socket.send(json.dumps({
                "type": "capture_status", "task_id": task_id, "status": status, "reason": reason,
            }))
            await asyncio.wait_for(waiter, timeout=10)
        finally:
            self.acks.pop(task_id, None)

    def upload(self, task_id: str, photo: Path, captured_at: float) -> None:
        query = urlencode({"captured_at": captured_at})
        request = urllib.request.Request(
            f"{self.server_url}/api/v1/camera/captures/{task_id}/image?{query}",
            data=photo.read_bytes(), method="POST",
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": mimetypes.guess_type(self.image.name)[0] or "application/octet-stream",
            },
        )
        with urllib.request.urlopen(request, timeout=60, context=self.ssl_context) as response:
            result = json.load(response)
        print(f"[SIMULATOR] uploaded task={task_id} photo={result['photo_id']}", flush=True)

    async def capture(self, task_id: str) -> None:
        photo = self.temp_dir / f"{task_id}.tmp"
        try:
            await self.update(task_id, "capturing")
            await asyncio.sleep(self.capture_delay_s)
            photo.write_bytes(self.image.read_bytes())
            captured_at = time.time()
            await self.update(task_id, "uploading")
            # Let the WebSocket reader continue handling ACKs while HTTP is in progress.
            await asyncio.to_thread(self.upload, task_id, photo, captured_at)
        except (OSError, ValueError, asyncio.TimeoutError, websockets.exceptions.ConnectionClosed) as exc:
            reason = str(exc)
            if isinstance(exc, urllib.error.HTTPError):
                reason = exc.read().decode("utf-8", errors="replace")
            print(f"[SIMULATOR] failed task={task_id}: {reason}", flush=True)
            # An HTTP rejection or status rejection already has a server-side result.
            if not isinstance(exc, (urllib.error.HTTPError, ValueError)):
                try:
                    await self.update(task_id, "failed", reason[:512])
                except (OSError, ValueError, asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
                    pass
        finally:
            photo.unlink(missing_ok=True)
            self.busy = False
            try:
                await self.socket.send(json.dumps({"type": "heartbeat", "busy": False}))
            except websockets.exceptions.ConnectionClosed:
                pass

    async def heartbeat(self) -> None:
        while True:
            await asyncio.sleep(10)
            await self.socket.send(json.dumps({"type": "heartbeat", "busy": self.busy}))

    async def run(self) -> None:
        self.temp_dir.mkdir(parents=True, exist_ok=True)
        parts = urlsplit(self.server_url)
        stream_url = urlunsplit((
            "wss" if parts.scheme == "https" else "ws", parts.netloc,
            "/api/v1/camera-stream", "", "",
        ))
        kwargs = {"ssl": self.ssl_context} if parts.scheme == "https" else {}
        async with websockets.connect(
            stream_url, additional_headers={"Authorization": f"Bearer {self.token}"}, **kwargs,
        ) as socket:
            self.socket = socket
            connected = json.loads(await socket.recv())
            print(f"[SIMULATOR] connected as {connected['device_id']}; images are simulated", flush=True)
            await socket.send(json.dumps({"type": "hello", "busy": False}))
            heartbeat = asyncio.create_task(self.heartbeat())
            capture_task: asyncio.Task | None = None
            try:
                async for raw in socket:
                    message = json.loads(raw)
                    if message["type"] == "capture":
                        self.busy = True
                        capture_task = asyncio.create_task(self.capture(message["task_id"]))
                    elif message["type"] == "capture_status_ack":
                        waiter = self.acks.get(message["task_id"])
                        if waiter is not None and not waiter.done():
                            waiter.set_result(message)
                    elif message["type"] == "error":
                        print(f"[SIMULATOR] server error: {message['reason']}", flush=True)
                        for waiter in self.acks.values():
                            if not waiter.done():
                                waiter.set_exception(ValueError(message["reason"]))
            finally:
                heartbeat.cancel()
                if capture_task is not None:
                    capture_task.cancel()
                await asyncio.gather(
                    heartbeat, *([capture_task] if capture_task is not None else []),
                    return_exceptions=True,
                )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="HTTP(S) API base URL")
    parser.add_argument("--image", required=True, type=Path, help="JPEG/PNG test image (not a live capture)")
    parser.add_argument("--ca", help="CA certificate for the HTTPS server")
    parser.add_argument("--capture-delay", type=float, default=0, help="simulated capture delay in seconds")
    args = parser.parse_args()
    token = os.environ.get("INCUBATOR_CAMERA_TOKEN")
    if not token:
        parser.error("set INCUBATOR_CAMERA_TOKEN")
    if not args.image.is_file():
        parser.error("test image does not exist")
    with tempfile.TemporaryDirectory(prefix="incubator-simulated-camera-") as temp_dir:
        camera = SimulatedCamera(
            args.server, token, args.image, Path(temp_dir), args.ca, args.capture_delay,
        )
        try:
            asyncio.run(camera.run())
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
