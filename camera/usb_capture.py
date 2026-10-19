#!/usr/bin/env python3
"""Single USB JPEG capture, isolated from the client's event loop (Python 3.7)."""

import argparse
import asyncio
import json
from pathlib import Path
import sys
import time


class CaptureError(RuntimeError):
    pass


def capture_once(output, device=0):
    """Run only in the worker process; always release an opened camera."""
    import cv2

    camera = cv2.VideoCapture(device)
    try:
        if not camera.isOpened():
            raise CaptureError("cannot open USB camera")
        ok, frame = camera.read()
        if not ok or frame is None:
            raise CaptureError("cannot read USB camera frame")
        captured_at = time.time()
        if not cv2.imwrite(str(output), frame):
            raise CaptureError("cannot write JPEG image")
        return captured_at
    finally:
        camera.release()


async def capture_photo(output, device=0, timeout=30):
    """Return the frame's Unix timestamp; remove partial files on failure.

    The caller provides a new .jpg path in its temporary directory and removes
    the successful image after upload. Timeout and cancellation kill and reap
    the worker before returning, so another task may safely open the camera.
    """
    output = Path(output).resolve()
    if output.suffix.lower() not in (".jpg", ".jpeg"):
        raise ValueError("capture output must be a JPEG path")
    if timeout <= 0:
        raise ValueError("capture timeout must be positive")
    if output.exists():
        raise ValueError("capture output already exists")

    process = None
    succeeded = False
    try:
        process = await asyncio.create_subprocess_exec(
            sys.executable, str(Path(__file__).resolve()), "--worker",
            "--device", str(device), "--output", str(output),
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), timeout)
        except asyncio.TimeoutError:
            raise CaptureError("USB camera capture timed out after {}s".format(timeout))
        if process.returncode != 0:
            detail = stderr.decode("utf-8", errors="replace").strip()
            raise CaptureError(detail or "USB camera worker failed")
        captured_at = json.loads(stdout.decode("utf-8"))["captured_at"]
        if not output.is_file() or output.stat().st_size == 0:
            raise CaptureError("USB camera worker produced no image")
        succeeded = True
        return captured_at
    finally:
        if process is not None and process.returncode is None:
            process.kill()
            await process.communicate()
        if not succeeded and output.exists():
            output.unlink()


def main():
    parser = argparse.ArgumentParser(description="Capture one JPEG without a desktop")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker:
            captured_at = capture_once(args.output, args.device)
        else:
            captured_at = asyncio.run(capture_photo(args.output, args.device, args.timeout))
        print(json.dumps({"captured_at": captured_at}), flush=True)
    except Exception as exc:
        print("capture failed: {}".format(exc), file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
