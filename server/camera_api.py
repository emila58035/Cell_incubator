"""Single-camera capture, interval scheduling and image upload API."""

from __future__ import annotations

import asyncio
import os
import re
import sqlite3
import time
import uuid
from contextlib import contextmanager, suppress
from pathlib import Path
from typing import Any, Callable, Iterator, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, ValidationError
from starlette.requests import ClientDisconnect


class CaptureUpdate(BaseModel):
    task_id: str
    status: Literal["capturing", "uploading", "failed"]
    reason: str | None = Field(default=None, max_length=512)


class ScheduleUpdate(BaseModel):
    enabled: bool = Field(strict=True)
    interval_s: int = Field(default=600, ge=1, strict=True)


class CameraAPI:
    def __init__(
        self,
        database: Callable[[], sqlite3.Connection],
        authenticate: Callable[..., str],
        websocket_authenticate: Callable[[WebSocket], str | None],
        photos_dir: Path,
        wall_clock: Callable[[], float] = time.time,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        self.database = database
        self.wall_clock = wall_clock
        self.monotonic = monotonic
        self.device_id = os.environ.get("INCUBATOR_CAMERA_ID", "camera-001")
        self.photos_dir = photos_dir
        # Development defaults; tune these with the eventual camera hardware.
        self.heartbeat_timeout_s = int(os.environ.get("INCUBATOR_CAMERA_OFFLINE_S", "45"))
        self.max_upload_bytes = int(os.environ.get("INCUBATOR_CAMERA_MAX_BYTES", str(16 * 1024 * 1024)))
        self.task_timeout_s = float(os.environ.get("INCUBATOR_CAMERA_TASK_TIMEOUT_S", "120"))
        self.socket: WebSocket | None = None
        self.last_seen = 0.0
        self.ready = False
        self.device_busy = False
        self.active_task_id: str | None = None
        self.active_deadline: float | None = None
        self.upload_task_id: str | None = None
        self.schedule_enabled = False
        self.interval_s = 600
        self.next_due: float | None = None
        self.worker: asyncio.Task | None = None

        self.router = APIRouter()
        self.router.add_api_websocket_route("/api/v1/camera-stream", self.stream)
        self.router.add_api_route("/api/v1/camera/status", self.status, methods=["GET"])
        self.router.add_api_route("/api/v1/camera/schedule", self.schedule, methods=["GET"])
        self.router.add_api_route("/api/v1/camera/schedule", self.update_schedule, methods=["PUT"])
        self.router.add_api_route(
            "/api/v1/camera/captures", self.capture, methods=["POST"], status_code=201,
        )
        self.router.add_api_route(
            "/api/v1/camera/captures/{task_id}", self.task, methods=["GET"],
        )
        self.router.add_api_route(
            "/api/v1/camera/photos/{photo_id}", self.photo, methods=["GET"],
        )
        self.router.add_api_route(
            "/api/v1/camera/photos/{photo_id}/file", self.photo_file, methods=["GET"],
        )

        @self.router.post("/api/v1/camera/captures/{task_id}/image", status_code=201)
        async def upload_image(
            task_id: str,
            request: Request,
            captured_at: float = Query(gt=0, le=253402300799),
            device_id: str = Depends(authenticate),
        ) -> dict[str, Any]:
            return await self.upload(task_id, request, captured_at, device_id)

        self.websocket_authenticate = websocket_authenticate

    @contextmanager
    def connection(self) -> Iterator[sqlite3.Connection]:
        conn = self.database()
        try:
            yield conn
        finally:
            conn.close()

    def online(self) -> bool:
        return self.socket is not None and self.monotonic() - self.last_seen <= self.heartbeat_timeout_s

    async def start(self) -> None:
        with self.connection() as conn, conn:
            conn.execute(
                "UPDATE camera_captures SET status = 'failed', finished_at = ?, reason = 'server_restarted' "
                "WHERE status IN ('pending', 'capturing', 'uploading')",
                (self.wall_clock(),),
            )
            row = conn.execute("SELECT enabled, interval_s FROM camera_schedule WHERE id = 1").fetchone()
        self.schedule_enabled = bool(row["enabled"])
        self.interval_s = row["interval_s"]
        if self.photos_dir.is_dir():
            for partial in self.photos_dir.glob("*.part"):
                partial.unlink()
        # Restored schedules try once at startup, even if the camera is not connected yet.
        if self.schedule_enabled:
            self.next_due = self.monotonic()
            await self.tick()
        self.worker = asyncio.create_task(self.run(), name="camera-scheduler")

    async def stop(self) -> None:
        if self.worker is not None:
            self.worker.cancel()
            with suppress(asyncio.CancelledError):
                await self.worker
            self.worker = None

    async def run(self) -> None:
        while True:
            await asyncio.sleep(0.25)
            await self.tick()

    async def tick(self) -> None:
        now = self.monotonic()
        if self.active_task_id is not None and self.active_deadline is not None and now >= self.active_deadline:
            self.fail(self.active_task_id, "capture_timeout")
        if self.socket is not None and not self.online():
            if self.active_task_id is not None:
                self.fail(self.active_task_id, "camera_offline")
            socket = self.socket
            self.socket = None
            self.ready = False
            self.device_busy = False
            await socket.close(code=1000, reason="camera heartbeat timeout")
        if self.schedule_enabled and self.next_due is not None and now >= self.next_due:
            # Keep the original phase, but collapse missed intervals to one attempt.
            intervals = int((now - self.next_due) // self.interval_s) + 1
            self.next_due += intervals * self.interval_s
            await self.create_capture("scheduled")

    async def schedule(self) -> dict[str, Any]:
        return {
            "enabled": self.schedule_enabled,
            "interval_s": self.interval_s,
            "next_capture_at": (
                self.wall_clock() + max(0, self.next_due - self.monotonic())
                if self.next_due is not None else None
            ),
        }

    async def update_schedule(self, payload: ScheduleUpdate) -> dict[str, Any]:
        was_enabled = self.schedule_enabled
        changed_interval = payload.interval_s != self.interval_s
        with self.connection() as conn, conn:
            conn.execute(
                "UPDATE camera_schedule SET enabled = ?, interval_s = ? WHERE id = 1",
                (int(payload.enabled), payload.interval_s),
            )
        self.schedule_enabled = payload.enabled
        self.interval_s = payload.interval_s
        if not payload.enabled:
            self.next_due = None
        elif not was_enabled:
            self.next_due = self.monotonic() + self.interval_s
            await self.create_capture("scheduled")
        elif changed_interval:
            self.next_due = self.monotonic() + self.interval_s
        return await self.schedule()

    async def status(self) -> dict[str, Any]:
        return {
            "device_id": self.device_id,
            "online": self.online(),
            "ready": self.online() and self.ready,
            "busy": self.device_busy or self.active_task_id is not None,
            "current_task_id": self.active_task_id,
            "schedule": await self.schedule(),
        }

    async def task(self, task_id: str) -> dict[str, Any]:
        with self.connection() as conn:
            row = conn.execute("SELECT * FROM camera_captures WHERE task_id = ?", (task_id,)).fetchone()
        if row is None:
            raise HTTPException(404, "capture not found")
        return dict(row)

    def fail(self, task_id: str, reason: str) -> None:
        with self.connection() as conn, conn:
            conn.execute(
                "UPDATE camera_captures SET status = 'failed', finished_at = ?, reason = ? "
                "WHERE task_id = ? AND status IN ('pending', 'capturing', 'uploading')",
                (self.wall_clock(), reason, task_id),
            )
        if self.active_task_id == task_id:
            self.active_task_id = None
            self.active_deadline = None

    async def capture(self) -> dict[str, Any]:
        return await self.create_capture("manual")

    async def create_capture(self, source: Literal["manual", "scheduled"]) -> dict[str, Any]:
        task_id = str(uuid.uuid4())
        reason = None
        code = 409
        if not self.online():
            reason, code = "camera_offline", 503
        elif not self.ready:
            reason = "camera_not_ready"
        elif self.device_busy or self.active_task_id is not None or self.upload_task_id is not None:
            reason = "camera_busy"
        with self.connection() as conn, conn:
            conn.execute(
                "INSERT INTO camera_captures "
                "(task_id, device_id, source, status, created_at, finished_at, reason) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (task_id, self.device_id, source,
                 ("skipped" if source == "scheduled" else "failed") if reason else "pending",
                 self.wall_clock(), self.wall_clock() if reason else None, reason),
            )
        if reason:
            if source == "scheduled":
                return await self.task(task_id)
            raise HTTPException(code, {"task_id": task_id, "reason": reason})

        # Reserve before sending (the first await) so concurrent requests cannot queue.
        self.active_task_id = task_id
        self.active_deadline = self.monotonic() + self.task_timeout_s
        self.device_busy = True
        socket = self.socket
        try:
            await socket.send_json({"type": "capture", "task_id": task_id})
        except (RuntimeError, OSError, WebSocketDisconnect):
            self.fail(task_id, "camera_disconnected")
            if self.socket is socket:
                self.socket = None
                self.ready = False
            if source == "manual":
                raise HTTPException(503, {"task_id": task_id, "reason": "camera_disconnected"})
        return await self.task(task_id)

    async def stream(self, websocket: WebSocket) -> None:
        if self.websocket_authenticate(websocket) != self.device_id:
            await websocket.close(code=1008, reason="invalid camera bearer token")
            return
        # One connection owns the physical camera; disconnect is required before reconnect.
        if self.socket is not None and self.online():
            await websocket.close(code=1008, reason="camera already connected")
            return
        if self.active_task_id is not None:
            self.fail(self.active_task_id, "camera_disconnected")
        old = self.socket
        self.socket = websocket
        self.ready = False
        self.device_busy = False
        self.last_seen = self.monotonic()
        await websocket.accept()
        if old is not None:
            await old.close(code=1000, reason="stale camera connection")
        try:
            await websocket.send_json({"type": "connected", "device_id": self.device_id})
            while True:
                try:
                    message = await websocket.receive_json()
                    if not isinstance(message, dict):
                        raise ValueError
                except ValueError:
                    await websocket.send_json({"type": "error", "reason": "invalid_json"})
                    continue
                if self.socket is not websocket:
                    break
                self.last_seen = self.monotonic()
                kind = message.get("type")
                if kind in {"hello", "heartbeat"}:
                    if not isinstance(message.get("busy"), bool):
                        await websocket.send_json({"type": "error", "reason": "busy_must_be_boolean"})
                        continue
                    self.ready = True
                    self.device_busy = message["busy"]
                    await websocket.send_json({"type": f"{kind}_ack", "server_ts": self.wall_clock()})
                elif kind == "capture_status":
                    try:
                        update = CaptureUpdate.model_validate(message)
                    except ValidationError:
                        await websocket.send_json({"type": "error", "reason": "invalid_capture_status"})
                        continue
                    if update.task_id != self.active_task_id:
                        await websocket.send_json({"type": "error", "reason": "capture_not_active"})
                        continue
                    current = await self.task(update.task_id)
                    transitions = {
                        "pending": {"capturing", "failed"},
                        "capturing": {"uploading", "failed"},
                        "uploading": {"failed"},
                    }
                    if update.status not in transitions.get(current["status"], set()):
                        await websocket.send_json({"type": "error", "reason": "invalid_capture_transition"})
                        continue
                    if update.status == "failed":
                        self.fail(update.task_id, update.reason or "camera_failed")
                    else:
                        with self.connection() as conn, conn:
                            conn.execute(
                                "UPDATE camera_captures SET status = ? WHERE task_id = ?",
                                (update.status, update.task_id),
                            )
                    await websocket.send_json({
                        "type": "capture_status_ack", "task_id": update.task_id, "status": update.status,
                    })
                else:
                    await websocket.send_json({"type": "error", "reason": "unknown_message_type"})
        except WebSocketDisconnect:
            pass
        finally:
            if self.socket is websocket:
                self.socket = None
                self.ready = False
                self.device_busy = False
                if self.active_task_id is not None:
                    self.fail(self.active_task_id, "camera_disconnected")

    async def upload(
        self, task_id: str, request: Request, captured_at: float, device_id: str,
    ) -> dict[str, Any]:
        if device_id != self.device_id:
            raise HTTPException(403, "camera token required")
        task = await self.task(task_id)
        if task["device_id"] != device_id:
            raise HTTPException(403, "capture belongs to another device")
        if task["status"] != "uploading" or task_id != self.active_task_id:
            raise HTTPException(409, "capture is not awaiting an image")
        if self.upload_task_id is not None:
            raise HTTPException(409, "image upload already in progress")
        media_type = request.headers.get("content-type", "").split(";", 1)[0].strip().lower()
        formats = {"image/jpeg": ("jpg", b"\xff\xd8\xff"), "image/png": ("png", b"\x89PNG\r\n\x1a\n")}
        if media_type not in formats:
            self.fail(task_id, "unsupported_image_type")
            raise HTTPException(415, "use image/jpeg or image/png")
        extension, signature = formats[media_type]
        safe_device_id = re.sub(r"[^A-Za-z0-9_-]", "_", device_id)
        filename = f"{safe_device_id}-{int(captured_at * 1000)}-{task_id}.{extension}"
        destination = self.photos_dir / filename
        partial = self.photos_dir / f"{task_id}.part"
        self.upload_task_id = task_id
        saved = False
        try:
            self.photos_dir.mkdir(parents=True, exist_ok=True)
            remaining = max(0, self.active_deadline - self.monotonic())
            await asyncio.wait_for(self.write_image(request, partial), timeout=remaining)
            with partial.open("rb") as image:
                if image.read(len(signature)) != signature:
                    raise HTTPException(400, "image header does not match content type")
            current = await self.task(task_id)
            if current["status"] != "uploading" or self.active_task_id != task_id:
                raise HTTPException(409, "capture ended before upload completed")
            partial.replace(destination)
            photo_id = str(uuid.uuid4())
            with self.connection() as conn, conn:
                conn.execute(
                    "INSERT INTO camera_photos (photo_id, task_id, captured_at, filename) VALUES (?, ?, ?, ?)",
                    (photo_id, task_id, captured_at, filename),
                )
                conn.execute(
                    "UPDATE camera_captures SET status = 'completed', finished_at = ? WHERE task_id = ?",
                    (self.wall_clock(), task_id),
                )
            saved = True
            self.active_task_id = None
            self.active_deadline = None
            return await self.photo(photo_id)
        except asyncio.TimeoutError:
            self.fail(task_id, "capture_timeout")
            raise HTTPException(408, "capture timeout")
        except HTTPException as exc:
            self.fail(task_id, str(exc.detail))
            raise
        except ClientDisconnect:
            self.fail(task_id, "upload_interrupted")
            raise HTTPException(400, "upload interrupted")
        except (OSError, sqlite3.Error):
            self.fail(task_id, "image_storage_failed")
            raise HTTPException(500, "image storage failed")
        finally:
            self.upload_task_id = None
            partial.unlink(missing_ok=True)
            if not saved:
                destination.unlink(missing_ok=True)

    async def write_image(self, request: Request, partial: Path) -> None:
        size = 0
        with partial.open("wb") as target:
            async for chunk in request.stream():
                size += len(chunk)
                if size > self.max_upload_bytes:
                    raise HTTPException(413, "image exceeds configured upload limit")
                target.write(chunk)

    async def photo(self, photo_id: str) -> dict[str, Any]:
        with self.connection() as conn:
            row = conn.execute("SELECT * FROM camera_photos WHERE photo_id = ?", (photo_id,)).fetchone()
        if row is None:
            raise HTTPException(404, "photo not found")
        return {**dict(row), "file_url": f"/api/v1/camera/photos/{photo_id}/file"}

    async def photo_file(self, photo_id: str, download: bool = False) -> FileResponse:
        photo = await self.photo(photo_id)
        path = self.photos_dir / photo["filename"]
        if not path.is_file():
            raise HTTPException(404, "photo file not found")
        return FileResponse(
            path, filename=path.name,
            content_disposition_type="attachment" if download else "inline",
        )
