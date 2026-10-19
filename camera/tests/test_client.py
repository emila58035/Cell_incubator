"""Client integration checks against temporary HTTP/WebSocket servers."""

import asyncio
from contextlib import asynccontextmanager, closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request

from websockets.legacy.server import serve

CAMERA_DIR = Path(__file__).resolve().parents[1]
SERVER_DIR = CAMERA_DIR.parent / "server"
sys.path.insert(0, str(CAMERA_DIR))
from client import CameraClient

TOKEN = "test-camera-token"
JPEG = b"\xff\xd8\xfftest image bytes"
FAKE_CV2 = '''
import os
from pathlib import Path
import time

class VideoCapture:
    def __init__(self, device):
        with open(os.environ["CAMERA_TEST_LOG"], "a") as log:
            log.write("open\\n")
    def isOpened(self):
        return os.environ.get("CAMERA_TEST_MODE") != "open_failed"
    def read(self):
        gate = os.environ.get("CAMERA_TEST_GATE")
        if gate:
            while not Path(gate).exists():
                time.sleep(0.01)
        time.sleep(0.15)
        return True, object()
    def release(self):
        pass

def imwrite(filename, frame):
    data = bytes.fromhex("ffd8ff") + b"test image bytes"
    if os.environ.get("CAMERA_TEST_MODE") == "bad_image":
        data = b"invalid JPEG"
    Path(filename).write_bytes(data)
    return True
'''


async def wait_for(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if asyncio.iscoroutine(result):
            result = await result
        if result:
            return result
        await asyncio.sleep(0.02)
    raise AssertionError("condition not reached within {}s".format(timeout))


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.images = self.root / "camera-tmp"
        self.log = self.root / "camera.log"
        (self.root / "cv2.py").write_text(FAKE_CV2, encoding="utf-8")
        env = patch.dict(os.environ, {
            "PYTHONPATH": str(self.root), "CAMERA_TEST_LOG": str(self.log),
            "CAMERA_TEST_MODE": "success", "CAMERA_TEST_GATE": "",
        })
        env.start()
        self.addCleanup(env.stop)

    @asynccontextmanager
    async def backend(self):
        with closing(sqlite3.connect(str(self.root / "data.db"))) as conn:
            conn.executescript((SERVER_DIR / "schema.sql").read_text(encoding="utf-8"))
        (self.root / "devices.json").write_text(json.dumps({"camera-001": {"token": TOKEN}}))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        self.base = "http://127.0.0.1:{}".format(port)
        env = dict(os.environ, INCUBATOR_DB=str(self.root / "data.db"),
                   INCUBATOR_DEVICES=str(self.root / "devices.json"),
                   INCUBATOR_CAMERA_ID="camera-001", INCUBATOR_CAMERA_PHOTOS=str(self.root / "photos"))
        with (self.root / "server.log").open("wb") as log:
            server = subprocess.Popen([
                sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1", "--port", str(port),
            ], cwd=str(SERVER_DIR), env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                async def health():
                    try:
                        return (await self.http("GET", "/healthz"))[0] == 200
                    except OSError:
                        if server.poll() is not None:
                            self.fail("temporary backend exited")
                        return False
                await wait_for(health, timeout=15)
                yield
            except BaseException:
                print((self.root / "server.log").read_text(errors="replace"))
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)

    async def http(self, method, path):
        def request():
            req = urllib.request.Request(self.base + path, method=method,
                                         data=b"" if method == "POST" else None)
            try:
                with urllib.request.urlopen(req, timeout=1) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as exc:
                with exc:
                    return exc.code, exc.read()
        return await asyncio.get_running_loop().run_in_executor(None, request)

    async def state(self, path="/status"):
        code, raw = await self.http("GET", "/api/v1/camera" + path)
        self.assertEqual(code, 200)
        return json.loads(raw)

    async def ready(self):
        state = await self.state()
        return state["ready"] and not state["busy"]

    async def start_capture(self):
        code, raw = await self.http("POST", "/api/v1/camera/captures")
        self.assertEqual(code, 201, raw)
        return json.loads(raw)["task_id"]

    async def terminal(self, task_id, status):
        async def check():
            state = await self.state("/captures/" + task_id)
            return state if state["status"] == status else None
        return await wait_for(check)

    @asynccontextmanager
    async def running(self, camera):
        camera.heartbeat_interval = 0.05
        camera.reconnect_delay = 0.05
        camera.ack_timeout = 0.5
        task = asyncio.create_task(camera.run())
        try:
            yield task
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            if camera.capture_task is not None and not camera.capture_task.cancelled():
                camera.capture_task.result()

    def camera(self, base=None):
        return CameraClient(base or self.base, TOKEN, self.images)

    def test_full_capture_download_busy_rejection_and_startup_cleanup(self):
        async def run():
            self.images.mkdir()
            stale = self.images / "capture-old.jpg"
            stale.write_bytes(b"stale")
            unrelated = self.images / "keep.jpg"
            unrelated.write_bytes(b"keep")
            async with self.backend():
                camera = self.camera()
                async with self.running(camera):
                    await wait_for(self.ready)
                    self.assertFalse(stale.exists())
                    task_id = await self.start_capture()
                    code, _ = await self.http("POST", "/api/v1/camera/captures")
                    self.assertEqual(code, 409)
                    await self.terminal(task_id, "completed")
                    await wait_for(self.ready)
                    photo = await self.state("/photos/latest")
                    code, raw = await self.http("GET", photo["file_url"] + "?download=true")
                    self.assertEqual((code, raw), (200, JPEG))
                    self.assertLess(abs(photo["captured_at"] - time.time()), 10)
                    self.assertFalse(list(self.images.glob("capture-*.jpg")))
                    self.assertEqual(unrelated.read_bytes(), b"keep")
                    self.assertEqual(self.log.read_text().splitlines(), ["open"])
        asyncio.run(run())

    def test_capture_and_upload_failure_have_reasons_cleanup_and_no_retry(self):
        async def run():
            async with self.backend():
                camera = self.camera()
                async with self.running(camera):
                    await wait_for(self.ready)
                    for mode, reason in (("open_failed", "cannot open USB camera"),
                                         ("bad_image", "image header does not match content type")):
                        os.environ["CAMERA_TEST_MODE"] = mode
                        task_id = await self.start_capture()
                        state = await self.terminal(task_id, "failed")
                        self.assertIn(reason, state["reason"])
                        await wait_for(self.ready)
                        self.assertFalse(list(self.images.iterdir()))
                    await asyncio.sleep(0.2)
                    self.assertEqual(self.log.read_text().splitlines(), ["open", "open"])
                    self.assertEqual((await self.state("/photos"))["total"], 0)
        asyncio.run(run())

    def test_disconnect_during_capture_reconnects_busy_without_old_upload(self):
        async def run():
            gate = self.root / "release-camera"
            os.environ["CAMERA_TEST_GATE"] = str(gate)
            async with self.backend():
                camera = self.camera()
                async with self.running(camera):
                    await wait_for(self.ready)
                    task_id = await self.start_capture()
                    await wait_for(lambda: self.log.exists())
                    old = camera.socket
                    await old.close()
                    await wait_for(lambda: camera.socket is not None and camera.socket is not old)
                    self.assertTrue(camera.busy)
                    self.assertTrue((await self.state())["busy"])
                    code, _ = await self.http("POST", "/api/v1/camera/captures")
                    self.assertEqual(code, 409)
                    gate.touch()
                    await wait_for(self.ready)
                    self.assertEqual((await self.terminal(task_id, "failed"))["reason"], "camera_disconnected")
                    self.assertEqual((await self.state("/photos"))["total"], 0)
                    self.assertFalse(list(self.images.iterdir()))
                    # A genuinely new task works; the old one was never replayed.
                    new_id = await self.start_capture()
                    await self.terminal(new_id, "completed")
                    await wait_for(self.ready)
                    self.assertEqual(self.log.read_text().splitlines(), ["open", "open"])
        asyncio.run(run())

    @asynccontextmanager
    async def slow_upload(self):
        entered, release = threading.Event(), threading.Event()
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                requests.append((self.path, self.headers["Content-Type"],
                                 self.rfile.read(int(self.headers["Content-Length"]))))
                entered.set()
                release.wait(10)
                body = b'{"photo_id":"temporary-http-photo"}'
                self.send_response(201)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield "http://127.0.0.1:{}".format(server.server_port), entered, release, requests
        finally:
            release.set()
            await asyncio.get_running_loop().run_in_executor(None, server.shutdown)
            server.server_close()
            thread.join(timeout=2)

    def test_upload_keeps_busy_and_file_across_disconnect_until_http_finishes(self):
        async def run():
            async with self.backend(), self.slow_upload() as slow:
                upload_base, entered, release, requests = slow
                camera = self.camera()
                # Real control backend and a delayed HTTP peer exercise independent channels.
                camera.server_url = upload_base
                async with self.running(camera):
                    await wait_for(self.ready)
                    task_id = await self.start_capture()
                    await wait_for(entered.is_set)
                    old = camera.socket
                    await old.close()
                    await wait_for(lambda: camera.socket is not None and camera.socket is not old)
                    self.assertTrue((await self.state())["busy"])
                    self.assertTrue(camera.busy)
                    self.assertEqual(len(list(self.images.iterdir())), 1)
                    release.set()
                    await wait_for(self.ready)
                    self.assertEqual(len(requests), 1)
                    self.assertIn(task_id, requests[0][0])
                    self.assertEqual(requests[0][1:], ("image/jpeg", JPEG))
                    self.assertFalse(list(self.images.iterdir()))
                    self.assertEqual((await self.terminal(task_id, "failed"))["reason"], "camera_disconnected")
        asyncio.run(run())

    def test_shutdown_waits_for_actual_http_request_before_clearing_busy(self):
        async def run():
            async with self.backend(), self.slow_upload() as slow:
                upload_base, entered, release, requests = slow
                camera = self.camera()
                camera.server_url = upload_base
                async with self.running(camera) as task:
                    await wait_for(self.ready)
                    await self.start_capture()
                    await wait_for(entered.is_set)
                    task.cancel()
                    await asyncio.sleep(0.15)
                    self.assertFalse(task.done())
                    self.assertTrue(camera.busy)
                    self.assertEqual(len(list(self.images.iterdir())), 1)
                    release.set()
                    await asyncio.gather(task, return_exceptions=True)
                    self.assertFalse(camera.busy)
                    self.assertFalse(list(self.images.iterdir()))
                    self.assertEqual(len(requests), 1)
        asyncio.run(run())

    def test_capture_timeout_reports_failure_and_allows_next_request(self):
        async def run():
            os.environ["CAMERA_TEST_GATE"] = str(self.root / "never-released")
            async with self.backend():
                camera = self.camera()
                camera.capture_timeout = 0.8
                async with self.running(camera):
                    await wait_for(self.ready)
                    task_id = await self.start_capture()
                    state = await self.terminal(task_id, "failed")
                    self.assertIn("timed out", state["reason"])
                    await wait_for(self.ready)
                    os.environ["CAMERA_TEST_GATE"] = ""
                    task_id = await self.start_capture()
                    await self.terminal(task_id, "completed")
                    await wait_for(self.ready)
        asyncio.run(run())

    def test_ack_timeout_prevents_capture_and_upload(self):
        async def run():
            messages = []

            async def peer(socket, path):
                await socket.send(json.dumps({"type": "connected", "device_id": "camera-001"}))
                self.assertEqual(json.loads(await socket.recv())["type"], "hello")
                await socket.send(json.dumps({"type": "hello_ack"}))
                await socket.send(json.dumps({"type": "capture", "task_id": "unacked"}))
                async for raw in socket:
                    message = json.loads(raw)
                    messages.append(message)
                    if message.get("status") == "failed":
                        await socket.send(json.dumps({"type": "capture_status_ack",
                                                      "task_id": "unacked", "status": "failed"}))

            async with serve(peer, "127.0.0.1", 0) as server:
                camera = self.camera("http://127.0.0.1:{}".format(server.sockets[0].getsockname()[1]))
                async with self.running(camera):
                    await wait_for(lambda: any(m.get("status") == "failed" for m in messages))
                    await wait_for(lambda: not camera.busy)
                    self.assertFalse(self.log.exists())
                    self.assertFalse(list(self.images.iterdir()))
                    self.assertEqual([m["status"] for m in messages if m["type"] == "capture_status"],
                                     ["capturing", "failed"])
                    self.assertIn("acknowledgement timed out", next(
                        m["reason"] for m in messages if m.get("status") == "failed"))
        asyncio.run(run())

    def test_upload_ack_timeout_removes_photo_without_upload(self):
        async def run():
            messages, uploads = [], []

            async def peer(socket, path):
                await socket.send(json.dumps({"type": "connected", "device_id": "camera-001"}))
                await socket.recv()
                await socket.send(json.dumps({"type": "hello_ack"}))
                await socket.send(json.dumps({"type": "capture", "task_id": "no-upload-ack"}))
                async for raw in socket:
                    message = json.loads(raw)
                    messages.append(message)
                    if message.get("status") in ("capturing", "failed"):
                        await socket.send(json.dumps({"type": "capture_status_ack",
                                                      "task_id": message["task_id"],
                                                      "status": message["status"]}))
                    elif message.get("status") == "uploading":
                        # A delayed ACK for the previous phase must not authorize upload.
                        await socket.send(json.dumps({"type": "capture_status_ack",
                                                      "task_id": message["task_id"], "status": "capturing"}))

            async with serve(peer, "127.0.0.1", 0) as server:
                camera = self.camera("http://127.0.0.1:{}".format(server.sockets[0].getsockname()[1]))
                with patch.object(camera, "upload", side_effect=lambda *args: uploads.append(args)):
                    async with self.running(camera):
                        await wait_for(lambda: any(m.get("status") == "failed" for m in messages))
                        await wait_for(lambda: not camera.busy)
                        self.assertEqual(uploads, [])
                        self.assertFalse(list(self.images.iterdir()))
                        self.assertEqual([m["status"] for m in messages if m["type"] == "capture_status"],
                                         ["capturing", "uploading", "failed"])
        asyncio.run(run())

    def test_busy_command_is_rejected_while_heartbeats_continue(self):
        async def run():
            gate = self.root / "release-camera"
            os.environ["CAMERA_TEST_GATE"] = str(gate)
            messages, uploads = [], []

            async def peer(socket, path):
                await socket.send(json.dumps({"type": "connected", "device_id": "camera-001"}))
                await socket.recv()
                await socket.send(json.dumps({"type": "hello_ack"}))
                await socket.send(json.dumps({"type": "capture", "task_id": "first"}))
                async for raw in socket:
                    message = json.loads(raw)
                    messages.append(message)
                    if message["type"] == "capture_status":
                        await socket.send(json.dumps({"type": "capture_status_ack",
                                                      "task_id": message["task_id"], "status": message["status"]}))
                        if message.get("status") == "capturing":
                            await socket.send(json.dumps({"type": "capture", "task_id": "second"}))

            async with serve(peer, "127.0.0.1", 0) as server:
                camera = self.camera("http://127.0.0.1:{}".format(server.sockets[0].getsockname()[1]))
                with patch.object(camera, "upload", side_effect=lambda *args: uploads.append(args[0])):
                    async with self.running(camera):
                        await wait_for(lambda: len([m for m in messages
                                                   if m["type"] == "heartbeat" and m["busy"]]) >= 3)
                        self.assertTrue(any(m.get("reason") == "camera_busy" and m["task_id"] == "second"
                                            for m in messages))
                        gate.touch()
                        await wait_for(lambda: bool(uploads) and not camera.busy)
                        self.assertEqual(uploads, ["first"])
                        self.assertEqual(self.log.read_text().splitlines(), ["open"])
        asyncio.run(run())

    def test_final_status_send_still_rejects_an_overlapping_command(self):
        async def run():
            camera = self.camera("http://127.0.0.1:1")
            pending = asyncio.create_task(asyncio.sleep(10))
            camera.capture_task = pending
            camera.busy = False  # image cleanup is done; final heartbeat send is pending
            messages = []

            class Socket:
                def __aiter__(self):
                    async def commands():
                        yield json.dumps({"type": "capture", "task_id": "next"})
                    return commands()

                async def send(self, raw):
                    messages.append(json.loads(raw))

            try:
                await camera.read(Socket())
                self.assertEqual(messages[0]["reason"], "camera_busy")
                self.assertIs(camera.capture_task, pending)
            finally:
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)
        asyncio.run(run())


if __name__ == "__main__":
    unittest.main(verbosity=2)
