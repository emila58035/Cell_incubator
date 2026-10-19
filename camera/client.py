#!/usr/bin/env python3
"""USB camera command client for Raspberry Pi / Python 3.7."""

import argparse
import asyncio
import http.client
import json
import logging
import os
from pathlib import Path
import signal
import ssl
import sys
import urllib.error
import urllib.request
from urllib.parse import urlencode, urlsplit, urlunsplit
import uuid

from websockets.exceptions import WebSocketException
from websockets.legacy.client import connect

from usb_capture import CaptureError, capture_photo


LOG = logging.getLogger("incubator-camera")


class SessionLost(RuntimeError):
    pass


class StatusRejected(RuntimeError):
    pass


class CameraClient:
    def __init__(self, server_url, token, temp_dir, ca_file=None, device=0):
        self.server_url = server_url.rstrip("/")
        parts = urlsplit(self.server_url)
        if parts.scheme not in ("http", "https") or not parts.netloc:
            raise ValueError("server must be an HTTP(S) URL")
        self.stream_url = urlunsplit((
            "wss" if parts.scheme == "https" else "ws", parts.netloc,
            "/api/v1/camera-stream", "", "",
        ))
        self.ssl_context = ssl.create_default_context(cafile=ca_file)
        self.token = token
        self.temp_dir = Path(temp_dir)
        self.device = device
        self.capture_timeout = 30
        self.upload_timeout = 60
        self.ack_timeout = 10
        self.heartbeat_interval = 10
        self.reconnect_delay = 5
        self.socket = None
        self.busy = False
        self.capture_task = None
        self.acks = {}

    def require_session(self, socket):
        if self.socket is not socket or socket.closed:
            raise SessionLost("camera control connection lost")

    async def update(self, socket, task_id, status, reason=None):
        self.require_session(socket)
        key = (socket, task_id, status)
        waiter = asyncio.get_running_loop().create_future()
        self.acks[key] = waiter
        try:
            await socket.send(json.dumps({
                "type": "capture_status", "task_id": task_id,
                "status": status, "reason": reason,
            }))
            try:
                await asyncio.wait_for(waiter, self.ack_timeout)
            except asyncio.TimeoutError:
                raise StatusRejected("{} status acknowledgement timed out".format(status))
        finally:
            self.acks.pop(key, None)

    def upload(self, task_id, photo, captured_at):
        query = urlencode({"captured_at": captured_at})
        request = urllib.request.Request(
            "{}/api/v1/camera/captures/{}/image?{}".format(self.server_url, task_id, query),
            data=photo.read_bytes(), method="POST",
            headers={"Authorization": "Bearer " + self.token, "Content-Type": "image/jpeg"},
        )
        # Do not follow redirects: each capture issues exactly one upload request.
        opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=self.ssl_context), NoRedirect(),
        )
        with opener.open(request, timeout=self.upload_timeout) as response:
            result = json.load(response)
        LOG.info("uploaded task=%s photo=%s", task_id, result["photo_id"])

    async def capture(self, socket, task_id):
        photo = self.temp_dir / ("capture-" + uuid.uuid4().hex + ".jpg")
        try:
            await self.update(socket, task_id, "capturing")
            captured_at = await capture_photo(photo, self.device, self.capture_timeout)
            await self.update(socket, task_id, "uploading")
            self.require_session(socket)
            upload = asyncio.get_running_loop().run_in_executor(
                None, self.upload, task_id, photo, captured_at,
            )
            try:
                # Cancelling a Future cannot stop urllib's thread. Keep the image
                # and busy flag until the real request has finished, including shutdown.
                await asyncio.shield(upload)
            except asyncio.CancelledError:
                await asyncio.gather(asyncio.shield(upload), return_exceptions=True)
                raise
        except asyncio.CancelledError:
            raise
        except (OSError, ValueError, KeyError, CaptureError, SessionLost, StatusRejected,
                http.client.HTTPException, WebSocketException) as exc:
            reason = str(exc) or type(exc).__name__
            if isinstance(exc, urllib.error.HTTPError):
                with exc:
                    reason = "HTTP {}: {}".format(exc.code, exc.read().decode("utf-8", errors="replace"))
            LOG.error("capture failed task=%s: %s", task_id, reason)
            try:
                await self.update(socket, task_id, "failed", reason[:512])
            except (SessionLost, StatusRejected, OSError, WebSocketException):
                # The old session owns this task; never report it on a new socket.
                pass
        finally:
            if photo.exists():
                photo.unlink()
            self.busy = False
            current = self.socket
            if current is not None:
                try:
                    await current.send(json.dumps({"type": "heartbeat", "busy": False}))
                except (OSError, WebSocketException):
                    pass

    async def read(self, socket):
        async for raw in socket:
            message = json.loads(raw)
            kind = message["type"]
            if kind == "capture":
                task_id = message["task_id"]
                if self.busy or (self.capture_task is not None and not self.capture_task.done()):
                    # Do not await an ACK inside the sole WebSocket reader.
                    await socket.send(json.dumps({
                        "type": "capture_status", "task_id": task_id,
                        "status": "failed", "reason": "camera_busy",
                    }))
                    LOG.warning("rejected busy task=%s", task_id)
                    continue
                if self.capture_task is not None:
                    # Surface unexpected errors (e.g. failure to delete a file).
                    self.capture_task.result()
                self.busy = True
                self.capture_task = asyncio.create_task(self.capture(socket, task_id))
            elif kind == "capture_status_ack":
                waiter = self.acks.get((socket, message["task_id"], message["status"]))
                if waiter is not None and not waiter.done():
                    waiter.set_result(message)
            elif kind == "error":
                LOG.error("server error: %s", message["reason"])
                for (owner, _, _), waiter in list(self.acks.items()):
                    if owner is socket and not waiter.done():
                        waiter.set_exception(StatusRejected(message["reason"]))

    async def heartbeat(self, socket):
        while True:
            await socket.send(json.dumps({"type": "heartbeat", "busy": self.busy}))
            await asyncio.sleep(self.heartbeat_interval)

    async def session(self, socket):
        connected = json.loads(await asyncio.wait_for(socket.recv(), self.ack_timeout))
        if connected.get("type") != "connected":
            raise StatusRejected("expected connected message")
        await socket.send(json.dumps({"type": "hello", "busy": self.busy}))
        hello = json.loads(await asyncio.wait_for(socket.recv(), self.ack_timeout))
        if hello.get("type") != "hello_ack":
            raise StatusRejected("expected hello acknowledgement")
        self.socket = socket
        LOG.info("connected device=%s busy=%s", connected["device_id"], self.busy)
        reader = asyncio.create_task(self.read(socket))
        heartbeat = asyncio.create_task(self.heartbeat(socket))
        try:
            done, _ = await asyncio.wait((reader, heartbeat), return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            self.socket = None
            for (owner, _, _), waiter in list(self.acks.items()):
                if owner is socket and not waiter.done():
                    waiter.set_exception(SessionLost("camera control connection lost"))
            reader.cancel()
            heartbeat.cancel()
            await asyncio.gather(reader, heartbeat, return_exceptions=True)

    async def run(self):
        self.temp_dir.mkdir(parents=True, exist_ok=True)
        # Only our own temporary images in the configured dedicated directory.
        for photo in self.temp_dir.glob("capture-*.jpg"):
            photo.unlink()
        kwargs = {"ssl": self.ssl_context} if self.stream_url.startswith("wss:") else {}
        try:
            while True:
                try:
                    async with connect(
                        self.stream_url, extra_headers={"Authorization": "Bearer " + self.token},
                        open_timeout=self.ack_timeout, close_timeout=5, **kwargs
                    ) as socket:
                        await self.session(socket)
                except (OSError, WebSocketException, asyncio.TimeoutError, StatusRejected,
                        ValueError, KeyError) as exc:
                    LOG.warning("control connection unavailable: %s", exc)
                LOG.info("reconnecting in %ss", self.reconnect_delay)
                await asyncio.sleep(self.reconnect_delay)
        finally:
            if self.capture_task is not None:
                self.capture_task.cancel()
                await asyncio.gather(self.capture_task, return_exceptions=True)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


async def run_client(camera):
    # systemd's SIGTERM follows the same resource cleanup as a foreground stop.
    if sys.platform != "win32":
        task = asyncio.current_task()
        for sig in (signal.SIGINT, signal.SIGTERM):
            asyncio.get_running_loop().add_signal_handler(sig, task.cancel)
    try:
        await camera.run()
    except asyncio.CancelledError:
        LOG.info("camera client stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default=os.environ.get("INCUBATOR_CAMERA_SERVER"))
    parser.add_argument("--ca", default=os.environ.get("INCUBATOR_CAMERA_CA"))
    parser.add_argument("--device", type=int, default=os.environ.get("INCUBATOR_CAMERA_DEVICE", "0"))
    parser.add_argument("--temp-dir", type=Path, default=os.environ.get(
        "INCUBATOR_CAMERA_TEMP_DIR", "/var/lib/incubator-camera/tmp",
    ))
    args = parser.parse_args()
    token = os.environ.get("INCUBATOR_CAMERA_TOKEN")
    if not args.server or not token:
        parser.error("set --server/INCUBATOR_CAMERA_SERVER and INCUBATOR_CAMERA_TOKEN")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    camera = CameraClient(args.server, token, args.temp_dir, args.ca, args.device)
    try:
        asyncio.run(run_client(camera))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
