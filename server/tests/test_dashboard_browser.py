#!/usr/bin/env python3
"""Browser integration check; needs playwright and an installed Chromium browser.

python server/tests/test_dashboard_browser.py --browser-executable <path> --output <dir>
Uses a temporary database and a simulated WebSocket device, never real equipment.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

from playwright.sync_api import sync_playwright
from websockets.sync.client import connect

parser = argparse.ArgumentParser()
parser.add_argument("--browser-executable")
parser.add_argument("--output", default=tempfile.mkdtemp(prefix="incubator-browser-images-"))
args = parser.parse_args()
output = Path(args.output)
output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
sandbox = Path(tempfile.mkdtemp(prefix="incubator-browser-"))
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
base = f"http://127.0.0.1:{port}"
token = "test-only-" + "f" * 54
(sandbox / "devices.json").write_text(json.dumps({"browser-device": {"token": token, "label": "培养箱 1 号"}}), encoding="utf-8")
conn = sqlite3.connect(sandbox / "data.db")
conn.executescript((root / "schema.sql").read_text(encoding="utf-8"))
now = int(time.time())
conn.execute("INSERT INTO devices(device_id, first_seen, last_seen, fw_version) VALUES ('browser-device', ?, ?, 'browser-test')", (now - 604800, now))
conn.executemany(
    "INSERT INTO telemetry(device_id, recv_ts, temperature, humidity, co2, target_temperature) VALUES ('browser-device', ?, ?, ?, ?, 37)",
    [(now - (60480 - i) * 10 + 60, 37 + math.sin(i / 150) * .25, 55 + math.sin(i / 220) * 3,
      100000 if i == 12345 else 51000 + round(math.sin(i / 450) * 2500)) for i in range(60474)],
)
conn.commit()
conn.close()
env = os.environ.copy()
env.update(INCUBATOR_DB=str(sandbox / "data.db"), INCUBATOR_DEVICES=str(sandbox / "devices.json"), INCUBATOR_STATIC=str(root / "static"))
log = (sandbox / "server.log").open("wb")
server = subprocess.Popen([sys.executable, "-m", "uvicorn", "app:app", "--host", "127.0.0.1", "--port", str(port)], cwd=root, env=env, stdout=log, stderr=log)
stop = threading.Event()
ready = threading.Event()
commands = []
device_errors = []
sample = {"device_id": "browser-device", "temperature":37.02, "temperature_ok":True, "humidity":55.3, "humidity_ok":True,
          "co2":51000, "co2_ok":True, "co2_available":True, "target_temperature":37.0, "heater_on":True, "heater_pwm":128,
          "light":False, "light_pct":30, "fan":True, "valve_enabled":True, "valve":False,
          "rssi":-57, "heap_int":120000, "firmware_version":"browser-test"}


def simulated_device():
    try:
        with connect(base.replace("http:", "ws:") + "/api/v1/device-stream", additional_headers={"Authorization": "Bearer " + token}) as ws:
            assert json.loads(ws.recv())["type"] == "connected"
            ws.send(json.dumps({"type":"telemetry", **sample}))
            ready.set()
            while not stop.is_set():
                try:
                    message = json.loads(ws.recv(timeout=.2))
                except TimeoutError:
                    continue
                if message.get("type") != "command":
                    continue
                command, params = message["command"], message["params"]
                commands.append(message)
                if command == "set_temperature":
                    sample["target_temperature"] = params["value"]
                elif command == "set_light":
                    sample["light"], sample["light_pct"] = params["enabled"] and params["brightness"] > 0, params["brightness"]
                else:
                    sample[{"set_heater":"heater_on", "set_fan":"fan", "set_valve_enabled":"valve_enabled"}[command]] = params["enabled"]
                ws.send(json.dumps({"type":"command_ack", "command_id":message["command_id"], "status":"applied"}))
                ws.send(json.dumps({"type":"telemetry", **sample}))
    except Exception as exc:
        device_errors.append(repr(exc))
        ready.set()


try:
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            urllib.request.urlopen(base + "/healthz", timeout=1).close()
            break
        except OSError:
            if server.poll() is not None:
                raise RuntimeError((sandbox / "server.log").read_text())
            time.sleep(.1)
    thread = threading.Thread(target=simulated_device, daemon=True)
    thread.start()
    assert ready.wait(10) and not device_errors, device_errors
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=args.browser_executable, headless=True)
        context = browser.new_context(viewport={"width":1440, "height":1000}, accept_downloads=True)
        page = context.new_page()
        errors = []
        history_requests = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.on("request", lambda request: history_requests.append(request.url) if "/history?" in request.url else None)
        page.goto(base)
        page.wait_for_function("state.history && !state.loading && state.latest.ws_connected")
        assert page.locator("#connection").inner_text() == "远控已连接"
        page.locator("[data-hours='168']").click()
        page.wait_for_function("state.hours === 168 && state.history && !state.loading")
        count = page.evaluate("state.history.points.length")
        assert count >= 60475, count
        assert page.evaluate("state.history.points.some(p => p.co2 === 100000)")
        assert page.evaluate("document.getElementById('chartHumidity')._plot.points.length") == count
        assert page.evaluate("charts[2].min === 0 && charts[2].max === 100000")
        before = len(history_requests)
        page.evaluate("loadHistory()")
        page.wait_for_function("!state.loading")
        assert len(history_requests) - before == 1, "incremental refresh reloaded all pages"
        assert "cursor=" in history_requests[-1]
        start_time = time.perf_counter()
        page.evaluate("drawCharts()")
        render_ms = (time.perf_counter() - start_time) * 1000
        canvas = page.locator("#chartTemp")
        canvas.scroll_into_view_if_needed()
        box = canvas.bounding_box()
        page.mouse.move(box["x"] + box["width"] * .55, box["y"] + 80)
        assert page.locator(".tooltip:visible").count() == 3, page.evaluate("charts.map(c => ({plot:!!document.getElementById(c.id)._plot, points:document.getElementById(c.id)._plot?.points.length, tooltip:document.getElementById(c.id).parentElement.querySelector('.tooltip').style.display}))")
        page.mouse.move(box["x"] + 160, box["y"] + 80)
        page.mouse.down()
        page.mouse.move(box["x"] + 480, box["y"] + 80, steps=10)
        page.mouse.up()
        assert page.evaluate("state.zoom !== null")
        assert page.evaluate("new Set(charts.map(c => document.getElementById(c.id)._plot.from)).size") == 1
        with page.expect_download() as download_info:
            page.locator("#downloadCsv").click()
        download = download_info.value
        assert "from_ts=" in download.url and "to_ts=" in download.url
        download.save_as(output / "zoom-export.csv")
        page.locator("#resetZoom").click()
        assert page.evaluate("state.zoom === null")
        page.locator("#temperatureInput").fill("38.5")
        page.evaluate("refresh()")
        page.wait_for_function("!state.refreshing")
        assert page.locator("#temperatureInput").input_value() == "38.5"
        page.locator("#temperatureForm button").click()
        page.wait_for_function("document.getElementById('heaterStatus').textContent.includes('设备已执行')")
        assert commands[-1]["command"] == "set_temperature" and commands[-1]["params"]["value"] == 38.5
        command_count = len(commands)
        page.locator("#brightnessInput").fill("42")
        assert page.locator("#brightnessRange").input_value() == "42"
        page.evaluate("refresh()")
        page.wait_for_function("!state.refreshing")
        assert page.locator("#brightnessInput").input_value() == "42"
        assert len(commands) == command_count
        page.locator("#brightnessForm button").click()
        page.wait_for_function("document.getElementById('lightStatus').textContent.includes('设备已执行')")
        assert commands[-1]["params"] == {"enabled":False, "brightness":42}
        page.locator("#lightToggle").click()
        page.wait_for_function("document.getElementById('lightToggle').textContent === '关闭照明'")
        assert sample["light"] is True
        for group, command in [("fan","set_fan"),("valve","set_valve_enabled"),("heater","set_heater")]:
            page.locator("#" + group + "Toggle").click()
            page.wait_for_function(f"document.getElementById('{group}Status').textContent.includes('设备已执行')")
            assert commands[-1]["command"] == command
        page.locator("#customToggle").click()
        page.locator("#fromTime").fill(time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(now - 7200)))
        page.locator("#toTime").fill(time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(now - 3600)))
        page.locator("#customForm button").click()
        page.wait_for_function("state.custom !== null && state.history && !state.loading")
        assert page.evaluate("state.history.points.every(p => p.recv_ts >= state.custom[0] && p.recv_ts <= state.custom[1])")
        page.locator("[data-hours='24']").click()
        page.wait_for_function("state.hours === 24 && state.history && !state.loading")
        page.evaluate("setTheme('light')")
        page.screenshot(path=str(output / "dashboard-desktop-light.png"), full_page=True)
        page.locator("#themeToggle").click()
        page.screenshot(path=str(output / "dashboard-desktop-dark.png"), full_page=True)
        page.reload()
        page.wait_for_function("state.history && !state.loading")
        assert page.evaluate("document.documentElement.dataset.theme") == "dark"
        mobile_context = browser.new_context(viewport={"width":390,"height":844}, is_mobile=True, has_touch=True, device_scale_factor=2)
        mobile = mobile_context.new_page()
        mobile.on("pageerror", lambda error: errors.append(str(error)))
        mobile.goto(base)
        mobile.wait_for_function("state.history && !state.loading")
        mobile.locator("[data-hours='168']").click()
        mobile.wait_for_function("state.hours === 168 && state.history && !state.loading")
        assert mobile.evaluate("document.documentElement.scrollWidth <= window.innerWidth"), "mobile horizontal overflow"
        mobile.locator("#chartHumidity").tap(position={"x":200,"y":80})
        assert mobile.locator(".tooltip:visible").count() == 3
        mobile.locator("#zoomIn").click()
        assert mobile.evaluate("state.zoom !== null")
        mobile.screenshot(path=str(output / "dashboard-mobile-light.png"), full_page=True)
        mobile.locator("#themeToggle").click()
        mobile.screenshot(path=str(output / "dashboard-mobile-dark.png"), full_page=True)
        mobile.set_viewport_size({"width":320,"height":720})
        mobile.evaluate("renderLatest({...state.latest, co2:100000})")
        assert mobile.evaluate("document.documentElement.scrollWidth <= window.innerWidth"), "small mobile horizontal overflow"
        page.evaluate("renderLatest({...state.latest, online:true, ws_connected:false})")
        assert page.locator("#connection").inner_text() == "仅数据上报"
        assert page.locator("#heaterToggle").is_disabled()
        assert not errors, errors
        browser.close()
        print(f"BROWSER CHECKS PASSED; {count} raw points; desktop redraw {render_ms:.1f} ms; images: {output}")
finally:
    stop.set()
    server.terminate()
    try:
        server.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
    log.close()
