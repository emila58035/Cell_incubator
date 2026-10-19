"""Subprocess lifecycle checks with a fake cv2, without camera hardware."""

import asyncio
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from usb_capture import CaptureError, capture_photo


FAKE_CV2 = '''
import os
from pathlib import Path
import time

MODE = os.environ.get("CAMERA_TEST_MODE", "success")
LOG = Path(os.environ["CAMERA_TEST_LOG"])

def log(event):
    with LOG.open("a") as handle:
        handle.write(event + "\\n")

class VideoCapture:
    def __init__(self, device):
        log("open:" + str(device))

    def isOpened(self):
        return MODE != "open_failed"

    def read(self):
        log("read")
        if MODE == "blocked":
            time.sleep(60)
        if MODE == "read_exception":
            raise RuntimeError("driver read error")
        if MODE == "read_failed":
            return False, None
        if MODE == "empty_frame":
            return True, None
        return True, object()

    def release(self):
        log("release")

def imwrite(filename, frame):
    log("write")
    if MODE == "write_failed":
        Path(filename).write_bytes(b"partial")
        return False
    if MODE == "empty_file":
        Path(filename).touch()
        return True
    Path(filename).write_bytes(b"fake JPEG for lifecycle checks")
    return True
'''


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "cv2.py").write_text(FAKE_CV2, encoding="utf-8")
        self.output = self.root / "photo.jpg"
        self.log = self.root / "events.txt"
        self.env = patch.dict(os.environ, {
            "PYTHONPATH": str(self.root),
            "CAMERA_TEST_LOG": str(self.log),
            "CAMERA_TEST_MODE": "success",
        })
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_success_records_frame_time_and_releases_camera(self):
        before = time.time()
        captured_at = asyncio.run(capture_photo(self.output, device=2))
        self.assertLessEqual(before, captured_at)
        self.assertLessEqual(captured_at, time.time())
        self.assertTrue(self.output.read_bytes())
        self.assertEqual(self.log.read_text().splitlines(), ["open:2", "read", "write", "release"])

    def test_device_and_encoding_failures_release_and_remove_partial_file(self):
        cases = {
            "open_failed": "cannot open USB camera",
            "read_failed": "cannot read USB camera frame",
            "empty_frame": "cannot read USB camera frame",
            "read_exception": "driver read error",
            "write_failed": "cannot write JPEG image",
            "empty_file": "produced no image",
        }
        for mode, reason in cases.items():
            with self.subTest(mode=mode):
                self.log.write_text("")
                os.environ["CAMERA_TEST_MODE"] = mode
                with self.assertRaisesRegex(CaptureError, reason):
                    asyncio.run(capture_photo(self.output))
                self.assertFalse(self.output.exists())
                self.assertEqual(self.log.read_text().splitlines()[-1], "release")

    async def wait_for_read(self):
        deadline = time.monotonic() + 5
        while not self.log.exists() or "read" not in self.log.read_text():
            if time.monotonic() >= deadline:
                self.fail("worker did not start reading")
            await asyncio.sleep(0.01)

    def test_timeout_stops_worker_and_keeps_event_loop_responsive(self):
        async def run():
            os.environ["CAMERA_TEST_MODE"] = "blocked"
            task = asyncio.create_task(capture_photo(self.output, timeout=1.5))
            await self.wait_for_read()
            # Stand in for client heartbeats while the driver's read is blocked.
            ticks = 0
            while not task.done():
                ticks += 1
                await asyncio.sleep(0.02)
            with self.assertRaisesRegex(CaptureError, "timed out"):
                await task
            self.assertGreater(ticks, 5)
            self.assertFalse(self.output.exists())
            # A new capture can proceed after the old subprocess has been reaped.
            os.environ["CAMERA_TEST_MODE"] = "success"
            await capture_photo(self.output)

        before = time.monotonic()
        asyncio.run(run())
        self.assertLess(time.monotonic() - before, 8)

    def test_cancellation_stops_worker_and_removes_partial_file(self):
        async def run():
            os.environ["CAMERA_TEST_MODE"] = "blocked"
            task = asyncio.create_task(capture_photo(self.output))
            await self.wait_for_read()
            self.output.write_bytes(b"partial")
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertFalse(self.output.exists())

        asyncio.run(run())

    def test_existing_file_is_not_overwritten_or_deleted(self):
        self.output.write_bytes(b"existing")
        with self.assertRaisesRegex(ValueError, "already exists"):
            asyncio.run(capture_photo(self.output))
        self.assertEqual(self.output.read_bytes(), b"existing")

    def test_requires_jpeg_and_positive_timeout(self):
        with self.assertRaisesRegex(ValueError, "JPEG"):
            asyncio.run(capture_photo(self.root / "photo.png"))
        with self.assertRaisesRegex(ValueError, "positive"):
            asyncio.run(capture_photo(self.output, timeout=0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
