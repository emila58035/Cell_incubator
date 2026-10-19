#!/usr/bin/env python3
"""Gallery API and browser checks against a temporary server, never real equipment."""
from __future__ import annotations

import argparse
import json
import os
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zlib
from contextlib import closing
from pathlib import Path

from playwright.sync_api import sync_playwright
from test_camera_schedule import SERVER_DIR, TOKEN, request, start_server, stop_server


def test_image() -> bytes:
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = b"".join(b"\0" + b"".join(bytes((x * 255 // 319, y * 255 // 159, 120)) for x in range(320)) for y in range(160))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320,160,8,2,0,0,0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


PNG = test_image()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser-executable")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    checks = 0

    def check(name, condition):
        nonlocal checks
        assert condition, name
        checks += 1
        print("[PASS] " + name, flush=True)

    with tempfile.TemporaryDirectory(prefix="incubator-gallery-") as sandbox:
        root = Path(sandbox)
        photos = root / "photos"
        photos.mkdir()
        with closing(sqlite3.connect(root / "data.db")) as conn:
            conn.executescript((SERVER_DIR / "schema.sql").read_text(encoding="utf-8"))
        (root / "devices.json").write_text(json.dumps({"camera-001":{"token":TOKEN}}), encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server, base = start_server(root, port, timeout_s=120)
        simulator = None

        def api(method, path, payload=None):
            code, raw = request(base, method, "/api/v1/camera" + path, payload)
            assert code == 200, (code, raw)
            return json.loads(raw)

        def seed(photo_id, ts, file=True):
            with closing(sqlite3.connect(root / "data.db")) as conn, conn:
                conn.execute("INSERT INTO camera_captures VALUES (?, 'camera-001', 'manual', 'completed', ?, ?, NULL)", (photo_id, ts, ts))
                conn.execute("INSERT INTO camera_photos VALUES (?, ?, ?, ?)", (photo_id, photo_id, ts, photo_id + ".png"))
            if file:
                (photos / (photo_id + ".png")).write_bytes(PNG)

        try:
            check("empty latest is null", api("GET", "/photos/latest") is None)
            check("empty gallery has no items", api("GET", "/photos")["items"] == [])
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(executable_path=args.browser_executable, headless=True)
                page = browser.new_page(viewport={"width":1280,"height":900}, accept_downloads=True)
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(base + "/camera.html")
                page.wait_for_function("state.initialized")
                check("browser displays empty and offline state", page.locator("#latestEmpty").is_visible() and page.locator("#online").inner_text() == "离线" and page.locator("#capture").is_disabled())
                for i in range(14):
                    seed(f"p{i:02}", 1000 + i)
                with closing(sqlite3.connect(root / "data.db")) as conn, conn:
                    conn.execute("INSERT INTO devices (device_id) VALUES ('box-001')")
                    conn.executemany(
                        "INSERT INTO telemetry (device_id,recv_ts,temperature,temperature_ok,humidity,humidity_ok,co2,co2_ok,co2_available) VALUES ('box-001',?, ?,1,50,0,51000,1,1)",
                        [(1010,37.01),(1016,37.02)],
                    )
                first, second = api("GET", "/photos?page=1"), api("GET", "/photos?page=2")
                check("pagination has no duplicate or omission", len(first["items"]) == 12 and len(second["items"]) == 2 and len({p["photo_id"] for p in first["items"]+second["items"]}) == 14)
                check("latest follows capture time", api("GET", "/photos/latest")["photo_id"] == "p13")
                env = api("GET", "/photos/p13")["environment"]
                check("equidistant environment uses earlier same record", env["recv_ts"] == 1010 and env["temperature"] == 37.01 and env["humidity_ok"] == 0)
                seed("boundary", 1046)
                seed("outside", 1047)
                check("30 second boundary included", api("GET", "/photos/boundary")["environment"]["recv_ts"] == 1016)
                check("outside 30 seconds is null", api("GET", "/photos/outside")["environment"] is None)
                for photo_id in ("boundary","outside"):
                    api("POST", "/photos/delete", {"photo_ids":[photo_id]})

                page.reload()
                page.wait_for_function("state.initialized && state.items.length === 12")
                page.locator("#latestButton").click()
                page.wait_for_function("state.image !== null")
                check("detail shows environment and invalid humidity", "37.01" in page.locator("#environment").inner_text() and "湿度 无有效数据" in page.locator("#environment").inner_text())
                page.locator("#rotate").click()
                page.locator("#flip").click()
                page.locator("#cropX").fill("20")
                page.locator("#cropX").dispatch_event("input")
                check("view changes rotation flip and crop", page.evaluate("state.rotation === 90 && state.flipped && document.getElementById('photoCanvas').width === 160 && document.getElementById('photoCanvas').height === 192"))
                with page.expect_download() as download_info:
                    page.locator("#download").click()
                check("download retains original bytes after view edits", Path(download_info.value.path()).read_bytes() == PNG and (photos / "p13.png").read_bytes() == PNG)
                page.locator("#closeDetail").click()
                page.locator("#next").click()
                page.wait_for_function("state.page === 2 && state.items.length === 2")
                page.locator("#selectPage").check()
                page.once("dialog", lambda dialog: dialog.dismiss())
                page.locator("#delete").click()
                check("cancel deletion preserves photos", api("GET", "/photos")["total"] == 14)
                page.once("dialog", lambda dialog: dialog.accept())
                page.locator("#delete").click()
                page.wait_for_function("state.page === 1 && state.items.length === 12")
                check("batch deletion removes files and task records", not (photos / "p00.png").exists() and not (photos / "p01.png").exists())
                with closing(sqlite3.connect(root / "data.db")) as conn:
                    check("telemetry preserved after deletion", conn.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0] == 2)
                    check("deleted task removed", conn.execute("SELECT COUNT(*) FROM camera_captures WHERE task_id IN ('p00','p01')").fetchone()[0] == 0)
                page.goto(base)
                page.wait_for_function("!document.getElementById('cameraPhoto').hidden")
                check("dashboard links to camera and latest photo", page.locator(".camera-preview a").first.get_attribute("href") == "/camera.html")
                page.goto(base + "/camera.html")
                page.wait_for_function("state.initialized")
                page.locator("#interval").fill("300")
                page.locator("#enabled").check()
                page.locator("#saveSchedule").click()
                page.wait_for_function("!state.scheduleDirty && document.getElementById('message').textContent === '定时配置已保存'")
                check("browser saves interval schedule", api("GET", "/schedule")["interval_s"] == 300)
                api("PUT", "/schedule", {"enabled":False,"interval_s":300})
                image_path = root / "simulated.png"
                image_path.write_bytes(PNG)
                with (root / "simulator.log").open("wb") as log:
                    simulator = subprocess.Popen(
                        [sys.executable, str(SERVER_DIR.parent / "camera" / "simulator.py"), "--server", base,
                         "--image", str(image_path), "--capture-delay", "0.5"],
                        env={**os.environ,"INCUBATOR_CAMERA_TOKEN":TOKEN}, stdout=log, stderr=log,
                    )
                page.wait_for_function("!document.getElementById('capture').disabled")
                page.locator("#capture").click()
                page.wait_for_function("document.getElementById('taskStatus').textContent.includes('完成') && state.latest.captured_at > 2000")
                check("browser manual capture completes through simulator", api("GET", "/photos")["total"] == 13)
                page.locator("#latestButton").click()
                page.wait_for_function("state.image !== null")
                check("detail shows missing environment explicitly", page.locator("#environment").inner_text() == "无对应环境数据")
                page.locator("#closeDetail").click()
                if args.output:
                    args.output.mkdir(parents=True, exist_ok=True)
                    page.screenshot(path=str(args.output / "camera-desktop.png"), full_page=True)
                page.set_viewport_size({"width":390,"height":844})
                check("mobile page has no horizontal overflow", page.evaluate("document.documentElement.scrollWidth <= innerWidth"))
                if args.output:
                    page.screenshot(path=str(args.output / "camera-mobile.png"), full_page=True)
                check("browser has no script errors", not errors)
                browser.close()

            seed("blocked", 2000, file=False)
            (photos / "blocked.png").mkdir()
            result = api("POST", "/photos/delete", {"photo_ids":["blocked","missing","p02","p02"]})
            check("partial deletion reports each failure and deduplicates", result["deleted"] == ["p02"] and {item["reason"] for item in result["failed"]} == {"photo_delete_failed","photo_not_found"})
            check("failed deletion retains record", api("GET", "/photos/blocked")["photo_id"] == "blocked")
            (photos / "blocked.png").rmdir()
            result = api("POST", "/photos/delete", {"photo_ids":["blocked"]})
            check("missing file still allows metadata cleanup", result["deleted"] == ["blocked"])
        finally:
            if simulator is not None:
                stop_server(simulator)
            stop_server(server)
    print(f"CAMERA GALLERY CHECK PASSED ({checks} checks)")


if __name__ == "__main__":
    main()
