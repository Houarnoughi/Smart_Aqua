#!/usr/bin/env python3
"""Mobile dashboard for Arduino Uno smart aquaponics system."""

from __future__ import annotations

import json
import threading
import time
from datetime import datetime
from pathlib import Path

import serial
from flask import Flask, jsonify, render_template, request


BASE_DIR = Path(__file__).resolve().parent
SERIAL_PORT = "/dev/ttyACM0"
BAUD_RATE = 115200

app = Flask(__name__)

state_lock = threading.Lock()
latest_data: dict = {
    "status": "starting",
    "updated_at": None,
    "error": None,
}
serial_handle: serial.Serial | None = None


COMMANDS = {
    "fan_auto": "FAN:AUTO",
    "fan_on": "FAN:ON",
    "fan_off": "FAN:OFF",
    "solenoid_auto": "SOL:AUTO",
    "solenoid_on": "SOL:ON",
    "solenoid_off": "SOL:OFF",
    "pump_on": "PUMP:ON",
    "pump_off": "PUMP:OFF",
    "light1_on": "L1:ON",
    "light1_off": "L1:OFF",
    "light2_on": "L2:ON",
    "light2_off": "L2:OFF",
    "light3_on": "L3:ON",
    "light3_off": "L3:OFF",
    "feed_now": "FEED:NOW",
    "feed_auto": "FEED:AUTO",
    "feed_off": "FEED:OFF",
}


def update_state(**values: object) -> None:
    with state_lock:
        latest_data.update(values)


def serial_reader() -> None:
    global serial_handle

    while True:
        try:
            serial_handle = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2)
            time.sleep(2)
            update_state(status="connected", error=None)

            while True:
                line = serial_handle.readline().decode("utf-8", errors="ignore").strip()
                if not line or not line.startswith("{"):
                    continue

                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    continue
                data["status"] = "connected"
                data["updated_at"] = datetime.now().isoformat(timespec="seconds")
                data["error"] = None
                update_state(**data)
        except Exception as exc:  # Keep dashboard alive if Arduino disconnects.
            update_state(status="disconnected", error=str(exc), updated_at=datetime.now().isoformat(timespec="seconds"))
            try:
                if serial_handle:
                    serial_handle.close()
            except Exception:
                pass
            serial_handle = None
            time.sleep(3)


def send_command(command: str) -> bool:
    if serial_handle is None or not serial_handle.is_open:
        return False

    serial_handle.write((command + "\n").encode("utf-8"))
    serial_handle.flush()
    return True


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/data")
def api_data():
    with state_lock:
        return jsonify(latest_data.copy())


@app.route("/api/command", methods=["POST"])
def api_command():
    payload = request.get_json(silent=True) or {}
    action = str(payload.get("action", ""))
    command = COMMANDS.get(action)

    if not command:
        return jsonify({"ok": False, "error": "unknown action"}), 400

    if not send_command(command):
        return jsonify({"ok": False, "error": "serial not connected"}), 503

    return jsonify({"ok": True, "command": command})


@app.route("/api/light_schedule", methods=["POST"])
def api_light_schedule():
    payload = request.get_json(silent=True) or {}
    light = int(payload.get("light", 0))
    on_seconds = int(payload.get("on_seconds", 0))
    off_seconds = int(payload.get("off_seconds", 0))

    if light not in (1, 2, 3) or on_seconds <= 0 or off_seconds <= 0:
        return jsonify({"ok": False, "error": "invalid light schedule"}), 400

    command = f"L{light}:AUTO:{on_seconds}:{off_seconds}"
    if not send_command(command):
        return jsonify({"ok": False, "error": "serial not connected"}), 503

    return jsonify({"ok": True, "command": command})


@app.route("/api/feed_schedule", methods=["POST"])
def api_feed_schedule():
    payload = request.get_json(silent=True) or {}
    interval_seconds = int(payload.get("interval_seconds", 0))
    duration_ms = int(payload.get("duration_ms", 0))

    if interval_seconds <= 0 or duration_ms <= 0:
        return jsonify({"ok": False, "error": "invalid feeder schedule"}), 400

    command = f"FEED:SET:{interval_seconds}:{duration_ms}"
    if not send_command(command):
        return jsonify({"ok": False, "error": "serial not connected"}), 503

    return jsonify({"ok": True, "command": command})


def main() -> None:
    thread = threading.Thread(target=serial_reader, daemon=True)
    thread.start()
    app.run(host="0.0.0.0", port=5000, debug=False)


if __name__ == "__main__":
    main()
