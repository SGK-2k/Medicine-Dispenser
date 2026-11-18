#!/usr/bin/env python3
"""
MediDispense - Doctor console for the Pico W medicine dispenser (Python version)
================================================================================

Python / Tkinter version of MediDispense_GUI.html. Works with
Medicine_Dispenser.ino version 6.x over a USB cable.

Run:
    pip install pyserial
    python medidispense_gui.py

Tkinter comes with Python on Windows and macOS. On Linux install it with
"sudo apt install python3-tk" if needed.

Features (same as the HTML version):
  * Connect to the Pico over USB (pick the COM port), or run Demo mode
  * Live copy of the LCD, every box's prescription status and IR sensor
  * Start-up self-check results
  * Doctor login (PIN checked by the Pico)
  * Add / select patients, prescribe one medicine per severity level
  * Live refill / dispense progress, "Proceed with Patient N" (reset mode)
  * Activity log with CSV export, per-patient history
  * Serial console to send raw commands
  * Demo mode: a simulated dispenser with keypad, IR sensors and RFID cards

Program structure (classes):
  Store           - saves patients and activity to a JSON file
  LiveState       - what the dispenser has reported (mode, LCD, slots, ...)
  Transport       - abstract base: send() / close()
    SerialTransport - real dispenser over USB (pyserial, background reader thread)
    DemoTransport   - simulated dispenser (wraps Simulator)
  Simulator       - behaves like Medicine_Dispenser.ino v6 (runs in a thread)
  ScrollFrame     - scrollable container for the doctor page
  App             - the Tkinter window: protocol handling and drawing

Threads never touch Tkinter directly: they put messages into a queue that
the main thread reads every 40 ms (App._poll_inbox).
"""

import csv
import datetime
import json
import os
import queue
import threading
import time
import traceback
from abc import ABC, abstractmethod

import tkinter as tk
from tkinter import ttk, filedialog

try:
    import serial
    from serial.tools import list_ports
except ImportError:                     # the app still runs in Demo mode
    serial = None
    list_ports = None


# =============================================================================
# 1. CONSTANTS - medicine list, texts, colours
# =============================================================================

LEVELS = {
    1: {"title": "LEVEL 1 · BOX 1", "name": "General",
        "meds": ["Paracetamol 500 mg", "ORS sachet", "Antacid tablet", "Cetirizine 10 mg"]},
    2: {"title": "LEVEL 2 · BOX 2", "name": "Severe",
        "meds": ["Amoxicillin 500 mg", "Azithromycin 500 mg", "Metformin 500 mg", "Amlodipine 5 mg"]},
    3: {"title": "LEVEL 3 · BOX 3", "name": "High severity",
        "meds": ["Morphine 10 mg", "Tramadol 50 mg", "Lorazepam 1 mg"]},
}

MODE_TEXT = {
    "BOOT":        ("Starting up", "Running the start-up check"),
    "IDLE":        ("Waiting for a prescription", "Choose a patient and prescribe"),
    "REFILL_AUTH": ("Refill: waiting for the code", "Enter the refill code + # on the dispenser"),
    "REFILL":      ("Refilling", "Put each medicine in the open box"),
    "REFILL_DONE": ("Refill complete", "Enter the refill code + # again to finish"),
    "READY":       ("Ready to dispense", "Select a box and log in with card or PIN"),
    "AUTH":        ("Logging in", "Card or PIN for the selected box"),
    "DISPENSE":    ("Dispensing", "Box open - waiting for the medicine to be taken"),
    "COMPLETE":    ("All medicine dispensed", "Refill the same prescription (code on the dispenser) or prescribe again"),
    "RESET_AUTH":  ("Reset: waiting for the code", "Enter the refill code + # to open all boxes"),
    "RESET":       ("Reset: emptying boxes", "Remove everything from all open boxes"),
    "LOCKED":      ("Locked", "Too many wrong tries"),
}

SLOT_TEXT = {"EMPTY": ("Empty", "info"), "AWAIT": ("Needs refill", "warn"),
             "LOADED": ("Loaded", "blue"), "DISPENSED": ("Dispensed", "ok")}

C = {   # colours
    "bg": "#f3f5f7", "card": "#ffffff", "ink": "#17212b", "muted": "#5f6b77", "line": "#e2e7ec",
    "primary": "#0f766e", "primary_soft": "#e6f4f2",
    "l1": "#15803d", "l1_soft": "#e8f6ec", "l2": "#b45309", "l2_soft": "#fdf3e4",
    "l3": "#b91c1c", "l3_soft": "#fdecec", "blue": "#1d4ed8", "blue_soft": "#e8eefc",
    "violet": "#4338ca", "violet_soft": "#eef0ff", "lcd_bg": "#2f5d1f", "lcd_fg": "#d8ffb8",
    "ir_on": "#0ea5e9", "grey": "#eef1f4", "grey_ink": "#4b5563",
}
TAG = {  # tag kind -> (background, text colour)
    "ok": (C["l1_soft"], C["l1"]), "warn": (C["l2_soft"], C["l2"]), "err": (C["l3_soft"], C["l3"]),
    "info": (C["grey"], C["grey_ink"]), "blue": (C["blue_soft"], C["blue"]),
}
BOX_COLOUR = {1: C["l1"], 2: C["l2"], 3: C["l3"]}
BOX_SOFT = {1: C["l1_soft"], 2: C["l2_soft"], 3: C["l3_soft"]}

PICO_VID = 0x2E8A           # Raspberry Pi USB vendor ID
BAUD = 115200
DATA_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "medidispense_data.json")


def now_ms():
    return int(time.time() * 1000)


def time_str(ms):
    return datetime.datetime.fromtimestamp(ms / 1000).strftime("%H:%M:%S")


# =============================================================================
# 2. STORE - patients and activity saved to a JSON file
# =============================================================================

class Store:
    """Keeps patients and the activity log, and saves them next to this script."""

    def __init__(self, path):
        self.path = path
        self.patients = []
        self.next_id = 1
        self.activity = []
        self.load()

    def load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                data = json.load(f)
            self.patients = data.get("patients", [])
            self.next_id = data.get("nextId", 1)
            self.activity = data.get("activity", [])
        except (OSError, ValueError):
            pass

    def save(self):
        try:
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump({"patients": self.patients, "nextId": self.next_id,
                           "activity": self.activity}, f, indent=1)
        except OSError:
            pass

    # ---- patients ----
    def add_patient(self, name=None):
        pid = self.next_id
        self.next_id += 1
        p = {"id": pid, "name": name or f"Patient {pid}", "rx": None, "status": "new",
             "boxes": {}, "log": []}
        self.patients.append(p)
        self.save()
        return p

    def by_name(self, name):
        return next((p for p in self.patients if p["name"] == name), None)

    def by_id(self, pid):
        return next((p for p in self.patients if p["id"] == pid), None)

    def plog(self, p, text):
        if p is None:
            return
        p["log"].insert(0, {"t": now_ms(), "text": text})
        del p["log"][50:]
        self.save()

    # ---- activity ----
    def add_activity(self, kind, event, details=""):
        self.activity.insert(0, {"t": now_ms(), "kind": kind, "event": event, "details": details})
        del self.activity[300:]
        self.save()


# =============================================================================
# 3. LIVE STATE - what the dispenser has reported
# =============================================================================

class LiveState:
    def __init__(self):
        self.conn = "off"            # off | serial | demo
        self.selected = None         # selected patient id
        self.draft = {}              # patient id -> {1,2,3: medicine or "-"}
        self.pending_patient = None
        self.reset()

    def reset(self):
        """Clear everything that comes from the dispenser."""
        self.ready = False
        self.fw = None
        self.mode = "BOOT"
        self.box = 0
        self.pin_len = 0
        self.attempts = 0
        self.lcd = ["", ""]
        self.ir = {1: False, 2: False, 3: False}
        self.slots = {b: {"st": "EMPTY", "med": "-"} for b in (1, 2, 3)}
        self.rx = None               # {"patient": str, "meds": {1,2,3}}
        self.doctor = None
        self.roles = []
        self.checks = []
        self.check_done = None
        self.need_reset = False


# =============================================================================
# 4. TRANSPORTS - real USB or the demo simulator
# =============================================================================

class Transport(ABC):
    """Something the app can send command lines to."""

    @abstractmethod
    def send(self, line):
        ...

    @abstractmethod
    def close(self):
        ...


class SerialTransport(Transport):
    """Real dispenser over USB. A background thread reads lines into the inbox."""

    def __init__(self, port, inbox):
        self.inbox = inbox
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        try:                                     # the Pico only sends once DTR is set
            self.ser.dtr = True
            self.ser.rts = True
        except (OSError, serial.SerialException):
            pass
        self.alive = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        buf = b""
        while self.alive:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except (OSError, serial.SerialException, TypeError, AttributeError) as e:
                if self.alive:
                    self.inbox.put(("lost", f"The dispenser was unplugged or stopped responding ({e})."))
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.inbox.put(("line", line.decode("utf-8", "replace").rstrip("\r")))

    def send(self, line):
        self.ser.write((line + "\n").encode("utf-8"))

    def close(self):
        self.alive = False
        try:
            self.ser.close()
        except Exception:
            pass


class DemoTransport(Transport):
    """Simulated dispenser running inside the app."""

    def __init__(self, inbox):
        self.sim = Simulator(lambda line: inbox.put(("line", line)))

    def send(self, line):
        self.sim.command(line)

    def close(self):
        self.sim.stop()


# =============================================================================
# 5. SIMULATOR - behaves like Medicine_Dispenser.ino v6
# =============================================================================

class Simulator:
    """
    One worker thread runs tasks one at a time (keys, cards, PC commands and a
    timer tick every 150 ms), exactly like the real loop handles one input at a
    time. Delays use wait(), which stops early when the demo is closed.
    """

    CODE = "1234"
    CLOSE = 5000
    COLLECT = 30000
    EMPTY = 3000
    LOGIN = 30000
    MAX = 3
    LOCK = 10
    NAMES = ["", "General", "Severe", "High Sev"]
    USERS = [
        {"name": "Patient", "pin": "1111", "uid": "9DB9BE21", "level": 1, "doc": False},
        {"name": "Head Nurse", "pin": "3333", "uid": "F3A0EEE3", "level": 2, "doc": False},
        {"name": "Doctor", "pin": "4444", "uid": "CFDE401C", "level": 3, "doc": True},
    ]

    def __init__(self, out):
        self.out = out
        self.slots = {b: {"med": "-", "pres": False, "st": "EMPTY"} for b in (1, 2, 3)}
        self.ir = {1: False, 2: False, 3: False}
        self.rx = None
        self.pending = None
        self.doctor = None
        self.mode = "BOOT"
        self.box = 0
        self.pin = ""
        self.attempts = 0
        self.locked = False
        self.idle_msg = "Waiting for Rx"
        self.lcd = ["", ""]
        self.t = {}
        self.refill_queue = []
        self.alive = True
        self.tasks = queue.Queue()
        self.tick_pending = threading.Event()
        threading.Thread(target=self._worker, daemon=True).start()
        threading.Thread(target=self._ticker, daemon=True).start()
        self.q(self._boot)

    # ---- threading helpers ----
    def _worker(self):
        while self.alive:
            fn = self.tasks.get()
            if fn is None or not self.alive:
                break
            try:
                fn()
            except Exception:
                traceback.print_exc()

    def _ticker(self):
        while self.alive:
            time.sleep(0.15)
            if not self.tick_pending.is_set():
                self.tick_pending.set()
                self.q(self._tick)

    def _tick(self):
        try:
            self.update()
        finally:
            self.tick_pending.clear()

    def q(self, fn):
        self.tasks.put(fn)

    def stop(self):
        self.alive = False
        self.tasks.put(None)

    def wait(self, ms):
        end = time.time() + ms / 1000
        while self.alive and time.time() < end:
            time.sleep(0.02)

    # ---- output ----
    def emit(self, s):
        if self.alive:
            self.out("EVT " + s)

    def show(self, a, b):
        a, b = a[:16], b[:16]
        if [a, b] == self.lcd:
            return
        self.lcd = [a, b]
        self.emit(f"LCD {a}|{b}")

    def slot(self, b, st):
        self.slots[b]["st"] = st
        self.emit_slot(b)

    def emit_slot(self, b):
        s = self.slots[b]
        self.emit(f"SLOT {b} {s['st']} {s['med']}")

    def servo(self, b, open_):
        self.emit(("OPEN " if open_ else "CLOSE ") + str(b))

    def send_rx(self):
        if self.rx:
            m = self.rx["m"]
            self.emit(f"RX {self.rx['patient']}|{m[1]}|{m[2]}|{m[3]}")
        else:
            self.emit("RX NONE")

    # ---- phases ----
    def set_mode(self, m):
        self.mode = m
        self.emit(f"MODE {m} {self.box}")
        self.enter(m)

    def enter(self, m):
        if m in ("IDLE", "READY"):
            if m == "IDLE" or m == "READY":
                self.box = 0
            self.pin = ""
            self.attempts = 0
            self.redraw()
        elif m in ("REFILL_AUTH", "REFILL_DONE", "RESET_AUTH", "COMPLETE"):
            self.pin = ""
            self.attempts = 0
            self.redraw()
        elif m == "REFILL":
            self.refill_queue = [b for b in (1, 2, 3) if self.slots[b]["pres"]]
            self.next_refill()
        elif m == "AUTH":
            self.pin = ""
            self.t["act"] = time.time()
            self.redraw()
        elif m == "DISPENSE":
            self.servo(self.box, True)
            self.t["open"] = time.time()
            self.t["taken"] = 0
            self.show("Take medicine", self.slots[self.box]["med"])
        elif m == "RESET":
            for b in (1, 2, 3):
                self.servo(b, True)
            self.t["empty"] = 0

    def redraw(self):
        m = self.mode
        titles = {"REFILL_AUTH": "REFILLING", "REFILL_DONE": "Refill complete", "RESET_AUTH": "RESET MODE"}
        stars = "*" * len(self.pin)
        if m == "IDLE":
            self.show("System Ready", self.idle_msg)
        elif m == "COMPLETE":
            if self.pin:
                self.show("Refill: code", "Code: " + stars)
            else:
                self.show("All dispensed", "Refill/Prescribe")
            self.emit(f"PINLEN {len(self.pin)}")
        elif m in titles:
            self.show(titles[m], "Code: " + stars if self.pin else "Code + #")
            self.emit(f"PINLEN {len(self.pin)}")
        elif m == "READY":
            self.show("Dispenser Ready", "Select Box 1-3")
        elif m == "AUTH":
            self.show(f"Box{self.box}:{self.NAMES[self.box]}", "PIN: " + stars if self.pin else "Card/PIN# 0=Exit")
            self.emit(f"PINLEN {len(self.pin)}")
        elif m == "REFILL" and self.box:
            self.show(f"REFILLING Box {self.box}", self.slots[self.box]["med"])

    def hello(self):
        self.emit("READY 6.2-demo")
        for b in (1, 2, 3):
            self.emit(f"BOX {b} {self.NAMES[b]}")
        for u in self.USERS:
            self.emit(f"ROLE {u['level']} {u['name']}")

    def status(self):
        self.emit(f"MODE {self.mode} {self.box}")
        self.emit(f"DOC OK {self.doctor}" if self.doctor else "DOC OUT")
        self.send_rx()
        for b in (1, 2, 3):
            self.emit_slot(b)
            self.emit(f"IR {b} {1 if self.ir[b] else 0}")
        self.emit(f"LCD {self.lcd[0]}|{self.lcd[1]}")

    def _boot(self):
        self.show("MED DISPENSER", "Self-check...")
        self.hello()
        for part, res in (("LCD", "OK found at 0x27"), ("RFID", "OK reader found"), ("KEYPAD", "OK no stuck keys")):
            self.show("Check: " + part.capitalize(), "")
            self.wait(250)
            self.emit(f"CHECK {part} {res}")
        for b in (1, 2, 3):
            self.emit(f"IR {b} 0")
            self.emit(f"CHECK IR{b} OK box {b} empty")
        for b in (1, 2, 3):
            self.show(f"Check: Servo {b}", "")
            self.wait(250)
            self.emit(f"CHECK SERVO{b} OK moved")
        self.emit("CHECK DONE 0")
        self.show("Self-check OK", "All parts ready")
        self.wait(1000)
        self.show("Initialising", "dispenser...")
        self.wait(500)
        self.idle_msg = "Waiting for Rx"
        self.set_mode("IDLE")

    # ---- PC commands ----
    def command(self, raw):
        self.q(lambda: self._command(raw))

    def _command(self, raw):
        cmd = raw.strip()
        up = cmd.upper()
        if up == "HELLO":
            self.hello()
            self.status()
        elif up == "STATUS":
            self.status()
        elif up == "PING":
            self.emit("PONG")
        elif up == "HISTORY":
            self.emit("HIST END 0")
        elif up == "DOCLOGOUT":
            self.doctor = None
            self.emit("DOC OUT")
        elif up.startswith("DOCLOGIN "):
            u = next((x for x in self.USERS if x["pin"] == cmd[9:].strip()), None)
            if u and u["doc"]:
                self.doctor = u["name"]
                self.emit("DOC OK " + u["name"])
            else:
                self.emit("DOC FAIL")
        elif up.startswith("RX "):
            self._receive_rx(cmd[3:].strip())
        elif up.startswith("SELECT "):
            if self.mode == "COMPLETE":
                self.emit("ERR all medicine dispensed - refill or prescribe")
            elif self.mode == "READY":
                try:
                    self._choose(int(up[7:].strip()))
                except ValueError:
                    self.emit("ERR box must be 1, 2 or 3")
        else:
            self.emit("ERR unknown command: " + cmd)

    def _receive_rx(self, body):
        if not self.doctor:
            self.emit("ERR doctor not logged in")
            return
        if self.mode in ("REFILL", "DISPENSE", "RESET") or self.locked:
            self.emit("ERR dispenser busy - try again when the boxes are closed")
            return
        p = body.split("|")
        if len(p) < 4:
            self.emit("ERR bad prescription")
            return
        rx = {"patient": p[0].strip(), "m": {b: (p[b].strip() or "-") for b in (1, 2, 3)}}
        if not any(rx["m"][b] != "-" for b in (1, 2, 3)):
            self.emit("ERR bad prescription")
            return
        self.pending = rx
        need_reset = self.rx is not None and any(self.slots[b]["st"] == "LOADED" for b in (1, 2, 3))
        if need_reset:
            self.emit("NEEDRESET")
            self.set_mode("RESET_AUTH")
        else:
            self._activate()
            self.set_mode("REFILL_AUTH")

    def _activate(self):
        if not self.pending:
            return
        self.rx, self.pending = self.pending, None
        for b in (1, 2, 3):
            m = self.rx["m"][b]
            self.slots[b] = {"med": m, "pres": m != "-", "st": "AWAIT" if m != "-" else "EMPTY"}
        self.send_rx()
        for b in (1, 2, 3):
            self.emit_slot(b)

    # ---- keypad / card / IR (from the demo panel) ----
    def key(self, k):
        self.q(lambda: self._key(k))

    def _key(self, k):
        self.emit("KEY " + k)
        m = self.mode
        if m == "IDLE":
            self.show("No prescription", "Ask the doctor")
            self.wait(1200)
            self.redraw()
            return
        if m in ("REFILL_AUTH", "REFILL_DONE", "RESET_AUTH", "COMPLETE"):
            if k.isdigit():
                if len(self.pin) < 4:
                    self.pin += k
                    self.redraw()
            elif k == "B":
                self.pin = self.pin[:-1]
                self.redraw()
            elif k == "*":
                self.pin = ""
                self.redraw()
            elif k == "#":
                code, self.pin = self.pin, ""
                if code == self.CODE:
                    self.emit("CODE OK")
                    self.wait(200)
                    if m == "REFILL_AUTH":
                        self.set_mode("REFILL")
                    elif m == "RESET_AUTH":
                        self.set_mode("RESET")
                    elif m == "COMPLETE":
                        for b in (1, 2, 3):
                            if self.slots[b]["pres"]:
                                self.slots[b]["st"] = "AWAIT"
                            self.emit_slot(b)
                        self.set_mode("REFILL")
                    else:
                        self.show("Medicine", "Dispenser Ready")
                        self.wait(2000)
                        self.set_mode("READY")
                else:
                    self._fail("Wrong code", "CODE")
            return
        if m == "READY":
            if k in "123":
                self._choose(int(k))
            return
        if m == "AUTH":
            self.t["act"] = time.time()
            if k == "0":
                self.show("Exiting...", "")
                self.wait(500)
                self.set_mode("READY")
            elif k in "123456789":
                if len(self.pin) < 4:
                    self.pin += k
                    self.redraw()
            elif k == "B":
                self.pin = self.pin[:-1]
                self.redraw()
            elif k == "*":
                self.pin = ""
                self.redraw()
            elif k == "#":
                if len(self.pin) == 4:
                    pin, self.pin = self.pin, ""
                    self._login(next((u for u in self.USERS if u["pin"] == pin), None), "PIN", "Invalid PIN")
                else:
                    self.show("Enter 4 Digit", "PIN First")
                    self.wait(1000)
                    self.redraw()

    def card(self, uid):
        self.q(lambda: self._card(uid))

    def _card(self, uid):
        if self.mode != "AUTH":
            return
        self.emit("CARD " + uid)
        self._login(next((u for u in self.USERS if u["uid"] == uid), None), "CARD", "Unknown Card")

    def set_ir(self, b, on):
        if self.ir[b] == on:
            return
        self.ir[b] = on
        self.emit(f"IR {b} {1 if on else 0}")

    def _choose(self, b):
        s = self.slots.get(b)
        if s is None:
            self.emit("ERR box must be 1, 2 or 3")
            return
        if s["st"] == "LOADED":
            self.box = b
            self.set_mode("AUTH")
            return
        why = "Not prescribed" if not s["pres"] else "Already given" if s["st"] == "DISPENSED" else "Empty"
        self.emit(f"ERR box {b}: {why}")
        self.show(f"Box {b}", why)
        self.wait(1700)
        self.redraw()

    def _login(self, u, method, fail_text):
        if u is None:
            self._fail(fail_text, method)
            return
        self.emit(f"AUTH {method} {u['name']}")
        self.show("Welcome!", u["name"])
        self.wait(1200)
        if self.box <= u["level"]:
            self.set_mode("DISPENSE")
            return
        self.emit(f"DENIED {self.box} {u['name']}")
        self.show("Access Denied", "Not Authorised")
        self.wait(2000)
        self.t["act"] = time.time()
        self.redraw()

    def _fail(self, text, method):
        self.attempts += 1
        self.emit(f"FAIL {method} {self.attempts} {self.MAX}")
        self.show(text, "Access Denied")
        self.wait(1700)
        if self.attempts >= self.MAX:
            self.locked = True
            self.emit(f"LOCK {self.LOCK}")
            for s in range(self.LOCK, 0, -1):
                self.show("Too Many Tries", f"Wait {s} s")
                self.wait(1000)
            self.attempts = 0
            self.locked = False
            self.emit("UNLOCK")
        self.pin = ""
        self.t["act"] = time.time()
        self.redraw()

    # ---- timed phases (called every 150 ms) ----
    def next_refill(self):
        if not self.refill_queue:
            self.box = 0
            self.emit("REFILLED")
            self.set_mode("REFILL_DONE")
            return
        b = self.refill_queue.pop(0)
        self.box = b
        self.t["det"] = 0
        self.slot(b, "AWAIT")
        self.emit(f"REFILL {b}")
        self.servo(b, True)
        self.redraw()

    def update(self):
        now = time.time()
        m = self.mode
        if m == "REFILL" and self.box:
            b = self.box
            if not self.ir[b]:
                if self.t.get("det"):
                    self.t["det"] = 0
                    self.redraw()
                return
            if not self.t.get("det"):
                self.t["det"] = now
            el = (now - self.t["det"]) * 1000
            if el >= self.CLOSE:
                self.servo(b, False)
                self.slot(b, "LOADED")
                self.emit(f"LOADED {b}")
                self.wait(600)
                self.next_refill()
            else:
                self.show(f"Box {b} detected", f"Closing in {int((self.CLOSE - el + 999) // 1000)} s")
        elif m == "DISPENSE":
            b = self.box
            if not self.ir[b]:
                if not self.t.get("taken"):
                    self.t["taken"] = now
                el = (now - self.t["taken"]) * 1000
                if el >= self.CLOSE:
                    self.servo(b, False)
                    self._finish_dispense(True)
                else:
                    self.show("Medicine taken", f"Closing in {int((self.CLOSE - el + 999) // 1000)} s")
                return
            self.t["taken"] = 0
            opened = (now - self.t["open"]) * 1000
            if opened >= self.COLLECT:
                self.servo(b, False)
                self._finish_dispense(False)
                return
            self.show(f"Take med: {int((self.COLLECT - opened + 999) // 1000)} s", self.slots[b]["med"])
        elif m == "RESET":
            left = [b for b in (1, 2, 3) if self.ir[b]]
            if left:
                self.t["empty"] = 0
                self.show("RESET: empty box", "Remove: " + " ".join(map(str, left)))
                return
            if not self.t.get("empty"):
                self.t["empty"] = now
            if (now - self.t["empty"]) * 1000 >= self.EMPTY:
                self._finish_reset()
            else:
                self.show("All boxes empty", "Checking...")
        elif m == "AUTH" and (now - self.t.get("act", now)) * 1000 > self.LOGIN:
            self.emit("TIMEOUT")
            self.show("Session Timeout", "Start Again")
            self.wait(1500)
            self.set_mode("READY")

    def _finish_dispense(self, taken):
        b = self.box
        s = self.slots[b]
        if taken:
            self.slot(b, "DISPENSED")
            self.emit(f"DISPENSED {b} {s['med']}")
            self.show("Dispensed", s["med"])
        else:
            self.emit(f"NOTTAKEN {b}")
            self.show("Not collected", f"Box {b} closed")
            self.wait(500)
        self.wait(1500)
        pres = [x for x in (1, 2, 3) if self.slots[x]["pres"]]
        if pres and all(self.slots[x]["st"] == "DISPENSED" for x in pres):
            self.emit("RXCOMPLETE " + self.rx["patient"])
            self.set_mode("COMPLETE")
        else:
            self.set_mode("READY")

    def _finish_reset(self):
        for b in (1, 2, 3):
            self.servo(b, False)
        self.slots = {b: {"med": "-", "pres": False, "st": "EMPTY"} for b in (1, 2, 3)}
        self.rx = None
        self.emit("EMPTIED")
        self.show("Emptied", "")
        self.wait(2000)
        if self.pending:
            self._activate()
            self.set_mode("REFILL_AUTH")
        else:
            self.send_rx()
            for b in (1, 2, 3):
                self.emit_slot(b)
            self.idle_msg = "Waiting for Rx"
            self.set_mode("IDLE")


# =============================================================================
# 6. UI HELPERS
# =============================================================================

class ScrollFrame(ttk.Frame):
    """A frame with a vertical scrollbar. Put widgets inside .inner"""

    def __init__(self, parent, bg):
        super().__init__(parent)
        self.canvas = tk.Canvas(self, bg=bg, highlightthickness=0)
        self.bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = tk.Frame(self.canvas, bg=bg)
        self.inner.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.win = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(self.win, width=e.width))
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.bar.pack(side="right", fill="y")
        self.canvas.bind_all("<MouseWheel>", self._wheel, add="+")
        self.canvas.bind_all("<Button-4>", lambda e: self.canvas.yview_scroll(-2, "units"), add="+")
        self.canvas.bind_all("<Button-5>", lambda e: self.canvas.yview_scroll(2, "units"), add="+")

    def _wheel(self, e):
        self.canvas.yview_scroll(int(-e.delta / 120) * 2, "units")


def card(parent, title=None, sub=None, bg=C["card"]):
    """A white rounded-looking panel with an optional heading."""
    f = tk.Frame(parent, bg=bg, highlightbackground=C["line"], highlightthickness=1, padx=14, pady=12)
    if title:
        tk.Label(f, text=title, bg=bg, fg=C["ink"], font=("Segoe UI", 11, "bold"), anchor="w").pack(fill="x")
    if sub:
        tk.Label(f, text=sub, bg=bg, fg=C["muted"], font=("Segoe UI", 9), anchor="w",
                 justify="left").pack(fill="x", pady=(0, 8))
    return f


def tag(parent, text, kind):
    bg, fg = TAG.get(kind, TAG["info"])
    return tk.Label(parent, text=text, bg=bg, fg=fg, font=("Segoe UI", 9, "bold"), padx=6, pady=1)


def alert(parent, kind, title, body):
    colours = {"warn": (C["l2_soft"], "#6b3407"), "info": (C["blue_soft"], "#1e3a8a"), "ok": (C["l1_soft"], "#14532d")}
    bg, fg = colours.get(kind, colours["info"])
    f = tk.Frame(parent, bg=bg, padx=12, pady=10)
    if title:
        tk.Label(f, text=title, bg=bg, fg=fg, font=("Segoe UI", 10, "bold"), anchor="w",
                 justify="left").pack(fill="x")
    tk.Label(f, text=body, bg=bg, fg=fg, font=("Segoe UI", 9), anchor="w", justify="left",
             wraplength=760).pack(fill="x")
    return f


# =============================================================================
# 7. THE APP
# =============================================================================

class App(tk.Tk):

    def __init__(self):
        super().__init__()
        self.title("MediDispense - Doctor Console")
        self.geometry("1360x880")
        self.minsize(1100, 700)
        self.configure(bg=C["bg"])

        self.store = Store(DATA_FILE)
        self.S = LiveState()
        self.inbox = queue.Queue()
        self.transport = None
        self.ready_job = None
        self.toast_job = None
        self._main_sig = None
        self._checks_sig = None
        self._activity_sig = None
        self._port_map = {}

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(40, self._poll_inbox)
        self.after(1000, self._tick)
        self.refresh_ports()
        if serial is None:
            self.banner("warn", "pyserial is not installed, so only Demo mode is available. "
                                "Install it with:  pip install pyserial")
        self.render()

    # ------------------------------------------------------------------
    # 7.1 Building the window
    # ------------------------------------------------------------------
    def _build_ui(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("Treeview", rowheight=24, font=("Segoe UI", 9))
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"))
        style.configure("TNotebook.Tab", padding=(14, 6), font=("Segoe UI", 10))

        # ---- header ----
        head = tk.Frame(self, bg=C["card"], padx=16, pady=10, highlightbackground=C["line"], highlightthickness=1)
        head.pack(fill="x")
        self.head = head
        logo = tk.Label(head, text="+", bg=C["primary"], fg="white", font=("Segoe UI", 16, "bold"), width=2)
        logo.pack(side="left")
        brand = tk.Frame(head, bg=C["card"])
        brand.pack(side="left", padx=10)
        tk.Label(brand, text="MediDispense", bg=C["card"], fg=C["ink"], font=("Segoe UI", 13, "bold")).pack(anchor="w")
        tk.Label(brand, text="Doctor console for the Pico W dispenser", bg=C["card"], fg=C["muted"],
                 font=("Segoe UI", 9)).pack(anchor="w")

        self.btn_disconnect = tk.Button(head, text="Disconnect", command=self.disconnect, fg=C["l3"],
                                        relief="groove", padx=10)
        self.btn_demo = tk.Button(head, text="Demo mode", command=self.start_demo, relief="groove", padx=10)
        self.btn_connect = tk.Button(head, text="Connect dispenser", command=self.connect_serial,
                                     bg=C["primary"], fg="white", activebackground=C["primary"],
                                     activeforeground="white", relief="flat", padx=12)
        self.btn_refresh = tk.Button(head, text="⟳", command=self.refresh_ports, relief="groove", width=3)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(head, textvariable=self.port_var, state="readonly", width=34)
        self.conn_label = tk.Label(head, text="● Not connected", bg=C["grey"], fg=C["muted"],
                                   font=("Segoe UI", 9, "bold"), padx=10, pady=3)
        self.btn_logout = tk.Button(head, text="Log out", command=lambda: self.send("DOCLOGOUT"), relief="groove")
        self.doc_label = tk.Label(head, text="", bg=C["blue_soft"], fg=C["blue"], font=("Segoe UI", 9, "bold"),
                                  padx=10, pady=3)
        self._header_order = [self.btn_disconnect, self.btn_demo, self.btn_connect, self.btn_refresh,
                              self.port_box, self.conn_label, self.btn_logout, self.doc_label]
        self._header_shown = None

        # ---- banner ----
        self.banner_label = tk.Label(self, text="", anchor="w", justify="left", padx=14, pady=8,
                                     font=("Segoe UI", 9, "bold"), wraplength=1250)

        # ---- body ----
        self.body = tk.Frame(self, bg=C["bg"])
        self.body.pack(fill="both", expand=True, padx=16, pady=12)
        left_scroll = ScrollFrame(self.body, C["bg"])
        left_scroll.canvas.configure(width=360)
        left_scroll.pack(side="left", fill="y")
        left = left_scroll.inner
        right = tk.Frame(self.body, bg=C["bg"])
        right.pack(side="left", fill="both", expand=True, padx=(16, 0))

        # LEFT 1: dispenser LCD + mode
        c1 = card(left, "Dispenser", "Live copy of the dispenser's screen.")
        c1.pack(fill="x")
        lcd_frame = tk.Frame(c1, bg="#1d2a17", padx=6, pady=6)
        lcd_frame.pack(fill="x")
        self.lcd_rows = []
        for _ in range(2):
            lbl = tk.Label(lcd_frame, text=" " * 16, bg=C["lcd_bg"], fg=C["lcd_fg"],
                           font=("Courier New", 17, "bold"), anchor="w", padx=8)
            lbl.pack(fill="x")
            self.lcd_rows.append(lbl)
        self.mode_title = tk.Label(c1, text="Offline", bg="#f6f8fa", fg=C["ink"], font=("Segoe UI", 10, "bold"),
                                   anchor="w", padx=10, pady=4)
        self.mode_title.pack(fill="x", pady=(10, 0))
        self.mode_sub = tk.Label(c1, text="", bg="#f6f8fa", fg=C["muted"], font=("Segoe UI", 9), anchor="w",
                                 justify="left", padx=10, pady=4, wraplength=300)
        self.mode_sub.pack(fill="x")

        # LEFT 2: boxes
        c2 = card(left, "Boxes", "What each box should hold, and what its IR sensor sees.")
        c2.pack(fill="x", pady=(12, 0))
        self.box_rows = {}
        for b in (1, 2, 3):
            row = tk.Frame(c2, bg=C["card"], highlightbackground=C["line"], highlightthickness=1, padx=8, pady=6)
            row.pack(fill="x", pady=3)
            tk.Label(row, text=str(b), bg=BOX_COLOUR[b], fg="white", font=("Segoe UI", 11, "bold"),
                     width=2).pack(side="left")
            mid = tk.Frame(row, bg=C["card"])
            mid.pack(side="left", fill="x", expand=True, padx=8)
            med = tk.Label(mid, text="No medicine", bg=C["card"], fg=C["muted"], font=("Segoe UI", 10, "bold"),
                           anchor="w")
            med.pack(fill="x")
            sub = tk.Frame(mid, bg=C["card"])
            sub.pack(fill="x")
            tk.Label(sub, text=LEVELS[b]["name"] + " · ", bg=C["card"], fg=C["muted"],
                     font=("Segoe UI", 8)).pack(side="left")
            st = tag(sub, "Empty", "info")
            st.pack(side="left")
            ir = tk.Label(row, text="○ Empty", bg=C["card"], fg=C["muted"], font=("Segoe UI", 9))
            ir.pack(side="right")
            self.box_rows[b] = {"med": med, "st": st, "ir": ir}

        # LEFT 3: start-up check
        c3 = card(left, "Start-up check")
        c3.pack(fill="x", pady=(12, 0))
        self.check_sub = tk.Label(c3, text="Runs every time the dispenser starts.", bg=C["card"], fg=C["muted"],
                                  font=("Segoe UI", 9), anchor="w")
        self.check_sub.pack(fill="x", pady=(0, 6))
        self.check_list = tk.Frame(c3, bg=C["card"])
        self.check_list.pack(fill="both", expand=True)

        # RIGHT: tabs
        self.tabs = ttk.Notebook(right)
        self.tabs.pack(fill="both", expand=True)

        self.doctor_tab = ScrollFrame(self.tabs, C["bg"])
        self.tabs.add(self.doctor_tab, text="Doctor console")
        self.main_area = self.doctor_tab.inner

        self.demo_tab = tk.Frame(self.tabs, bg=C["bg"])
        self._build_demo_tab(self.demo_tab)

        act = tk.Frame(self.tabs, bg=C["bg"])
        self.tabs.add(act, text="Activity")
        self._build_activity_tab(act)

        con = tk.Frame(self.tabs, bg=C["bg"])
        self.tabs.add(con, text="Serial console")
        self._build_console_tab(con)

        # ---- toast / status bar ----
        self.toast_label = tk.Label(self, text="", bg=C["ink"], fg="white", font=("Segoe UI", 10), anchor="w",
                                    padx=14, pady=6)

    def _build_demo_tab(self, parent):
        c = card(parent, "Demo: pretend you are at the dispenser",
                 "The refill code is 1234. Patient PIN 1111, Head Nurse 3333, Doctor 4444.",
                 bg=C["violet_soft"])
        c.pack(fill="x", padx=4, pady=8)
        grid = tk.Frame(c, bg=C["violet_soft"])
        grid.pack(fill="x")

        kp = tk.Frame(grid, bg=C["violet_soft"])
        kp.pack(side="left", padx=(0, 30), anchor="n")
        tk.Label(kp, text="Keypad", bg=C["violet_soft"], fg=C["muted"]).grid(row=0, column=0, columnspan=4, sticky="w")
        keys = ["1", "2", "3", "A", "4", "5", "6", "B", "7", "8", "9", "C", "*", "0", "#", "D"]
        for i, k in enumerate(keys):
            tk.Button(kp, text=k, width=4, height=1, font=("Segoe UI", 11, "bold"), bg="white",
                      command=lambda k=k: self.demo_key(k)).grid(row=1 + i // 4, column=i % 4, padx=2, pady=2)

        irf = tk.Frame(grid, bg=C["violet_soft"])
        irf.pack(side="left", padx=(0, 30), anchor="n")
        tk.Label(irf, text="IR sensors (put medicine in / take it out)", bg=C["violet_soft"],
                 fg=C["muted"]).pack(anchor="w")
        self.demo_ir_buttons = {}
        for b in (1, 2, 3):
            btn = tk.Button(irf, text=f"Box {b}: put medicine in", width=28, anchor="w", bg="white",
                            command=lambda b=b: self.demo_ir(b))
            btn.pack(anchor="w", pady=2)
            self.demo_ir_buttons[b] = btn

        cf = tk.Frame(grid, bg=C["violet_soft"])
        cf.pack(side="left", anchor="n")
        tk.Label(cf, text="Tap an RFID card", bg=C["violet_soft"], fg=C["muted"]).pack(anchor="w")
        for label, uid in (("Patient card", "9DB9BE21"), ("Head Nurse card", "F3A0EEE3"),
                           ("Doctor card", "CFDE401C"), ("Unknown card", "DEADBEEF")):
            tk.Button(cf, text=label, width=18, anchor="w", bg="white",
                      command=lambda u=uid: self.demo_card(u)).pack(anchor="w", pady=2)

    def _build_activity_tab(self, parent):
        bar = tk.Frame(parent, bg=C["bg"])
        bar.pack(fill="x", pady=(8, 6))
        tk.Label(bar, text="Activity", bg=C["bg"], font=("Segoe UI", 11, "bold")).pack(side="left")
        tk.Button(bar, text="Clear", command=self.clear_activity, relief="groove").pack(side="right", padx=4)
        tk.Button(bar, text="Export CSV", command=self.export_csv, relief="groove").pack(side="right", padx=4)
        frame = tk.Frame(parent)
        frame.pack(fill="both", expand=True)
        self.act_tree = ttk.Treeview(frame, columns=("time", "event", "details"), show="headings")
        for col, w in (("time", 90), ("event", 180), ("details", 700)):
            self.act_tree.heading(col, text=col.capitalize())
            self.act_tree.column(col, width=w, anchor="w", stretch=(col == "details"))
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.act_tree.yview)
        self.act_tree.configure(yscrollcommand=sb.set)
        self.act_tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        for kind, (bg, fg) in TAG.items():
            self.act_tree.tag_configure(kind, foreground=fg)

    def _build_console_tab(self, parent):
        self.console = tk.Text(parent, bg="#0f1720", fg="#cfe3f2", insertbackground="white",
                               font=("Consolas", 10), state="disabled", wrap="word")
        self.console.pack(fill="both", expand=True, pady=(8, 6))
        self.console.tag_configure("tx", foreground="#7dd3fc")
        self.console.tag_configure("evt", foreground="#a7f3d0")
        self.console.tag_configure("sys", foreground="#fcd34d")
        row = tk.Frame(parent, bg=C["bg"])
        row.pack(fill="x")
        self.cmd_var = tk.StringVar()
        ent = ttk.Entry(row, textvariable=self.cmd_var, font=("Consolas", 10))
        ent.pack(side="left", fill="x", expand=True)
        ent.bind("<Return>", lambda e: self.send_raw())
        tk.Button(row, text="Send", command=self.send_raw, relief="groove", padx=12).pack(side="left", padx=6)
        tk.Label(parent, text="e.g. STATUS, HISTORY, PING", bg=C["bg"], fg=C["muted"]).pack(anchor="w")

    # ------------------------------------------------------------------
    # 7.2 Small helpers
    # ------------------------------------------------------------------
    def toast(self, msg):
        self.toast_label.configure(text=msg)
        self.toast_label.pack(side="bottom", fill="x", before=self.body)
        if self.toast_job:
            self.after_cancel(self.toast_job)
        self.toast_job = self.after(2800, self.toast_label.pack_forget)

    def banner(self, kind, text=None):
        if not kind:
            self.banner_label.pack_forget()
            self.banner_kind = None
            return
        colours = {"warn": (C["l2_soft"], "#7c3a06"), "err": (C["l3_soft"], "#7f1414"),
                   "info": (C["primary_soft"], "#0b4f4a")}
        bg, fg = colours[kind]
        self.banner_label.configure(text=text, bg=bg, fg=fg)
        self.banner_label.pack(fill="x", padx=16, pady=(10, 0), after=self.head)
        self.banner_kind = kind

    banner_kind = None

    def console_line(self, text, kind=None):
        self.console.configure(state="normal")
        self.console.insert("end", text + "\n", kind or ())
        lines = int(self.console.index("end-1c").split(".")[0])
        if lines > 600:
            self.console.delete("1.0", f"{lines - 600}.0")
        self.console.see("end")
        self.console.configure(state="disabled")

    def add_activity(self, kind, event, details=""):
        self.store.add_activity(kind, event, details)
        self.render_activity()

    def active_patient(self):
        return self.store.by_name(self.S.rx["patient"]) if self.S.rx else None

    @staticmethod
    def rx_boxes(rx):
        return [b for b in (1, 2, 3) if rx and rx["meds"].get(b) and rx["meds"][b] != "-"]

    def rx_complete(self):
        if not self.S.rx:
            return True
        boxes = self.rx_boxes(self.S.rx)
        return bool(boxes) and all(self.S.slots[b]["st"] == "DISPENSED" for b in boxes)

    # ------------------------------------------------------------------
    # 7.3 Connection
    # ------------------------------------------------------------------
    def refresh_ports(self):
        self._port_map = {}
        values = []
        best = None
        if list_ports is not None:
            for p in list_ports.comports():
                label = f"{p.device} - {p.description}"
                self._port_map[label] = p.device
                values.append(label)
                if p.vid == PICO_VID and best is None:
                    best = label
        self.port_box["values"] = values
        if best:
            self.port_var.set(best)
        elif values and self.port_var.get() not in values:
            self.port_var.set(values[0])
        elif not values:
            self.port_var.set("(no USB ports found)")

    def connect_serial(self):
        if serial is None:
            self.banner("err", "pyserial is not installed. Run:  pip install pyserial   then restart the app.")
            return
        device = self._port_map.get(self.port_var.get())
        if not device:
            self.banner("info", "Plug in the Pico, click ⟳ to refresh the list, and choose its port.")
            return
        try:
            self.transport = SerialTransport(device, self.inbox)
        except (serial.SerialException, OSError) as e:
            self.banner("err", f"Could not open {device}. Close the Arduino Serial Monitor (or any other program "
                               f"using the port) and try again.\n{e}")
            self.transport = None
            return
        self.S.reset()
        self.S.conn = "serial"
        self.banner(None)
        self.console_line(f"# USB port {device} opened", "sys")
        self.render()
        self.after(150, lambda: self.send("HELLO"))
        if self.ready_job:
            self.after_cancel(self.ready_job)
        self.ready_job = self.after(3500, self._check_ready)

    def _check_ready(self):
        if self.S.conn == "serial" and not self.S.ready:
            self.banner("warn", "Connected, but the dispenser is not answering. Check that Medicine_Dispenser.ino "
                                "version 6 is uploaded, then Disconnect and Connect again.")

    def start_demo(self):
        if self.S.conn != "off":
            return
        self.S.reset()
        self.transport = DemoTransport(self.inbox)
        self.S.conn = "demo"
        self.banner("info", "Demo mode: a simulated dispenser runs inside this app. Use the 'Demo controls' tab "
                            "to press keys, move medicine in and out of boxes, and tap cards.")
        self.tabs.insert(1, self.demo_tab, text="Demo controls")
        self.console_line("# demo started", "sys")
        self.render()

    def disconnect(self):
        if self.ready_job:
            self.after_cancel(self.ready_job)
            self.ready_job = None
        was_demo = self.S.conn == "demo"
        if self.transport:
            self.transport.close()
        self.transport = None
        self.S.conn = "off"
        self.S.reset()
        if was_demo:
            try:
                self.tabs.forget(self.demo_tab)
            except tk.TclError:
                pass
        self.banner(None)
        self.console_line("# demo stopped" if was_demo else "# USB port closed", "sys")
        self._drain_inbox()
        self.render()

    def _drain_inbox(self):
        try:
            while True:
                self.inbox.get_nowait()
        except queue.Empty:
            pass

    def lost_connection(self, msg):
        if self.transport:
            self.transport.close()
        self.transport = None
        self.S.conn = "off"
        self.S.reset()
        self.banner("warn", msg + " Plug it back in and click Connect dispenser.")
        self.add_activity("err", "Disconnected", msg)
        self.render()

    def send(self, line):
        if self.S.conn == "off" or not self.transport:
            self.toast("Connect the dispenser first")
            return False
        shown = "DOCLOGIN ****" if line.upper().startswith("DOCLOGIN ") else line
        self.console_line("> " + shown, "tx")
        try:
            self.transport.send(line)
            return True
        except Exception as e:
            self.console_line(f"! write failed: {e}", "sys")
            self.toast("Could not send to the dispenser")
            return False

    def send_raw(self):
        v = self.cmd_var.get().strip()
        if v:
            self.cmd_var.set("")
            self.send(v)

    # demo panel
    def _sim(self):
        return self.transport.sim if isinstance(self.transport, DemoTransport) else None

    def demo_key(self, k):
        sim = self._sim()
        if sim:
            sim.key(k)

    def demo_card(self, uid):
        sim = self._sim()
        if not sim:
            return
        if self.S.mode != "AUTH":
            self.toast("Choose a box on the keypad first - the reader is only active on the login screen")
            return
        sim.card(uid)

    def demo_ir(self, b):
        sim = self._sim()
        if sim:
            sim.set_ir(b, not sim.ir[b])

    # ------------------------------------------------------------------
    # 7.4 Messages from the dispenser (main thread)
    # ------------------------------------------------------------------
    def _poll_inbox(self):
        changed = False
        try:
            for _ in range(400):
                kind, payload = self.inbox.get_nowait()
                if kind == "line":
                    self.handle_line(payload)
                elif kind == "lost" and self.S.conn == "serial":
                    self.lost_connection(payload)
                changed = True
        except queue.Empty:
            pass
        if changed:
            self.render()
        self.after(40, self._poll_inbox)

    def _tick(self):
        if self.S.conn != "off":
            self.render()
        self.after(1000, self._tick)

    def handle_line(self, line):
        S, st = self.S, self.store
        if not line.startswith("EVT "):
            if line.strip():
                self.console_line(line)
            return
        self.console_line(line, "evt")
        body = line[4:]
        typ, _, rest = body.partition(" ")
        p = rest.split(" ")
        ap = self.active_patient()

        if typ == "READY":
            S.ready, S.fw, S.roles = True, rest, []
            if self.ready_job:
                self.after_cancel(self.ready_job)
                self.ready_job = None
            if self.banner_kind == "warn":
                self.banner(None)
            self.add_activity("info", "Connected",
                              ("Demo dispenser" if S.conn == "demo" else "Dispenser") + " firmware " + rest)
        elif typ == "ROLE":
            name = " ".join(p[1:])
            if not any(r["name"] == name for r in S.roles):
                S.roles.append({"level": int(p[0]), "name": name})
        elif typ == "CHECK":
            if p[0] == "DONE":
                S.check_done = int(p[1])
                self.add_activity("err" if S.check_done else "ok", "Start-up check",
                                  f"{S.check_done} part(s) failed" if S.check_done else "All parts OK")
            else:
                c = {"part": p[0], "res": p[1] if len(p) > 1 else "", "detail": " ".join(p[2:])}
                for i, old in enumerate(S.checks):
                    if old["part"] == c["part"]:
                        S.checks[i] = c
                        break
                else:
                    S.checks.append(c)
        elif typ == "MODE":
            S.mode = p[0]
            S.box = int(p[1]) if len(p) > 1 and p[1].isdigit() else 0
            if S.mode not in ("RESET_AUTH", "RESET"):
                S.need_reset = False
        elif typ == "STATUS":
            S.attempts = int(p[2]) if len(p) > 2 and p[2].isdigit() else 0
        elif typ == "DOC":
            if p[0] == "OK":
                S.doctor = " ".join(p[1:])
                self.add_activity("ok", "Doctor logged in", S.doctor)
            elif p[0] == "FAIL":
                self.toast("Wrong doctor PIN")
                self.add_activity("warn", "Doctor login failed", "Wrong PIN")
                if hasattr(self, "doc_pin_var"):
                    self.doc_pin_var.set("")
            else:
                if S.doctor:
                    self.add_activity("info", "Doctor logged out", S.doctor)
                S.doctor = None
        elif typ == "RX":
            if rest == "NONE":
                S.rx = None
            else:
                parts = rest.split("|") + ["-", "-", "-"]
                if S.pending_patient == parts[0]:
                    S.pending_patient = None
                S.rx = {"patient": parts[0], "meds": {1: parts[1], 2: parts[2], 3: parts[3]}}
                pt = st.by_name(parts[0]) or st.add_patient(parts[0])
                if pt["status"] != "active":
                    pt["status"] = "active"
                    pt["rx"] = {"1": parts[1], "2": parts[2], "3": parts[3]}
                    pt["boxes"] = {}
                    st.plog(pt, "Prescription stored on the dispenser")
                st.save()
        elif typ == "SLOT":
            n = int(p[0])
            S.slots[n] = {"st": p[1], "med": " ".join(p[2:])}
            if ap and S.slots[n]["med"] != "-":
                ap["boxes"][str(n)] = p[1]
                st.save()
        elif typ == "IR":
            S.ir[int(p[0])] = p[1] == "1"
        elif typ == "NEEDRESET":
            S.need_reset = True
            self.add_activity("warn", "Reset needed",
                              "Medicine is still in the boxes - enter the refill code on the dispenser to empty them")
        elif typ == "REFILL":
            S.box = int(p[0])
            if ap and ap["status"] == "complete":
                ap["status"] = "active"
                st.plog(ap, "Refilling the same prescription")
            self.add_activity("info", "Refill", f"Box {p[0]} opened - put in {S.slots[int(p[0])]['med']}")
        elif typ == "LOADED":
            med = S.slots[int(p[0])]["med"]
            self.add_activity("ok", "Loaded", f"Box {p[0]} - {med}")
            st.plog(ap, f"Box {p[0]} loaded with {med}")
        elif typ == "REFILLED":
            self.add_activity("ok", "Refill done", "All prescribed boxes are filled")
        elif typ == "DISPENSED":
            n, med = p[0], " ".join(p[1:])
            self.add_activity("ok", "Medicine dispensed", f"Box {n} - {med}" + (f" for {ap['name']}" if ap else ""))
            st.plog(ap, f"Dispensed {med} (Box {n})")
            self.toast(f"Medicine dispensed: {med}")
        elif typ == "NOTTAKEN":
            self.add_activity("warn", "Not collected", f"Box {p[0]} closed without the medicine being taken")
            st.plog(ap, f"Box {p[0]}: not collected")
        elif typ == "RXCOMPLETE":
            pt = st.by_name(rest)
            if pt:
                pt["status"] = "complete"
                st.plog(pt, "All medicine dispensed")
            self.add_activity("ok", "Prescription complete", rest)
            self.toast(f"{rest}: all medicine dispensed")
        elif typ == "EMPTIED":
            if ap and ap["status"] == "active":
                ap["status"] = "incomplete"
                st.plog(ap, "Boxes emptied before all medicine was dispensed")
            self.add_activity("info", "Emptied", "All boxes are empty")
        elif typ == "LCD":
            a, _, b = rest.partition("|")
            S.lcd = [a, b]
        elif typ == "PINLEN":
            S.pin_len = int(p[0]) if p[0].isdigit() else 0
        elif typ == "AUTH":
            how = {"CARD": "RFID card", "PC": "PIN from PC"}.get(p[0], "keypad PIN")
            self.add_activity("ok", "Logged in", f"{' '.join(p[1:])} ({how}) for Box {S.box}")
        elif typ == "FAIL":
            what = {"CARD": "Unknown card", "CODE": "Wrong refill code"}.get(p[0], "Wrong PIN")
            self.add_activity("warn", "Wrong code" if p[0] == "CODE" else "Login failed",
                              f"{what} - try {p[1]} of {p[2]}")
        elif typ == "DENIED":
            self.add_activity("err", "Access denied", f"{' '.join(p[1:])} may not open Box {p[0]}")
        elif typ == "LOCK":
            self.add_activity("err", "Locked", f"Too many wrong tries - locked for {p[0]} s")
            S.mode = "LOCKED"
        elif typ == "TIMEOUT":
            self.add_activity("warn", "Timed out", "No input for 30 s")
        elif typ == "ERR":
            self.add_activity("err", "Dispenser message", rest)
            self.toast(rest)
        elif typ == "PONG":
            self.toast("Dispenser answered")

    # ------------------------------------------------------------------
    # 7.5 Drawing
    # ------------------------------------------------------------------
    def render(self):
        S = self.S
        connected = S.conn != "off"
        live = connected and S.ready

        # header
        if S.conn == "demo":
            self.conn_label.configure(text="● Demo dispenser", bg=C["violet_soft"], fg=C["violet"])
        elif live:
            self.conn_label.configure(text="● Dispenser connected", bg=C["l1_soft"], fg=C["l1"])
        elif connected:
            self.conn_label.configure(text="● Waiting for dispenser...", bg=C["l2_soft"], fg=C["l2"])
        else:
            self.conn_label.configure(text="● Not connected", bg=C["grey"], fg=C["muted"])
        visible = {self.btn_disconnect: connected, self.btn_demo: not connected,
                   self.btn_connect: not connected, self.btn_refresh: not connected,
                   self.port_box: not connected, self.conn_label: True,
                   self.btn_logout: bool(S.doctor), self.doc_label: bool(S.doctor)}
        shown = tuple(w for w in self._header_order if visible[w])
        if shown != self._header_shown:          # re-pack only when the set changes
            for w in self._header_order:
                w.pack_forget()
            for w in shown:
                w.pack(side="right", padx=4)
            self._header_shown = shown
        self.btn_disconnect.configure(text="Stop demo" if S.conn == "demo" else "Disconnect")
        self.doc_label.configure(text=f"Dr. logged in: {S.doctor}" if S.doctor else "")

        # LCD
        for r in range(2):
            text = (S.lcd[r] or "").ljust(16)[:16]
            self.lcd_rows[r].configure(text=text, fg=C["lcd_fg"] if connected else C["lcd_bg"])
        title, sub = MODE_TEXT.get(S.mode, (S.mode, ""))
        if not live:
            self.mode_title.configure(text="Offline")
            self.mode_sub.configure(text="")
        else:
            box = f" · Box {S.box}" if S.box and S.mode in ("AUTH", "DISPENSE") else ""
            self.mode_title.configure(text=title + box)
            self.mode_sub.configure(text=sub)

        # boxes
        for b in (1, 2, 3):
            s = S.slots[b]
            row = self.box_rows[b]
            txt, kind = SLOT_TEXT.get(s["st"], (s["st"], "info"))
            bg, fg = TAG[kind]
            row["med"].configure(text=s["med"] if s["med"] != "-" else "No medicine",
                                 fg=C["ink"] if s["med"] != "-" else C["muted"])
            row["st"].configure(text=txt, bg=bg, fg=fg)
            row["ir"].configure(text="● Item inside" if S.ir[b] else "○ Empty",
                                fg=C["ir_on"] if S.ir[b] else C["muted"])

        self._render_checks()
        self._render_main(live)
        self._render_demo()

    def _render_checks(self):
        S = self.S
        sig = (tuple((c["part"], c["res"], c["detail"]) for c in S.checks), S.check_done)
        if sig == self._checks_sig:
            return
        self._checks_sig = sig
        self.check_sub.configure(text="Runs every time the dispenser starts." if S.check_done is None
                                 else f"{S.check_done} part(s) failed - see below." if S.check_done
                                 else "All parts passed.")
        for w in self.check_list.winfo_children():
            w.destroy()
        if not S.checks:
            tk.Label(self.check_list, text="No results yet.", bg=C["card"], fg=C["muted"]).pack(anchor="w")
            return
        for c in S.checks:
            row = tk.Frame(self.check_list, bg=C["card"])
            row.pack(fill="x", pady=1)
            tk.Label(row, text=c["part"], bg=C["card"], font=("Segoe UI", 9, "bold"), width=8,
                     anchor="w").pack(side="left")
            tag(row, c["res"], "ok" if c["res"] == "OK" else "warn" if c["res"] == "WARN" else "err").pack(side="left")
            tk.Label(row, text=c["detail"], bg=C["card"], fg=C["muted"], font=("Segoe UI", 8), anchor="w",
                     justify="left", wraplength=210).pack(side="left", padx=6)

    def render_activity(self):
        sig = (len(self.store.activity), self.store.activity[0]["t"] if self.store.activity else 0)
        if sig == self._activity_sig:
            return
        self._activity_sig = sig
        self.act_tree.delete(*self.act_tree.get_children())
        for a in self.store.activity:
            self.act_tree.insert("", "end", values=(time_str(a["t"]), a["event"], a["details"]), tags=(a["kind"],))

    def _render_demo(self):
        sim = self._sim()
        if not sim:
            return
        for b, btn in self.demo_ir_buttons.items():
            on = self.S.ir[b]
            btn.configure(text=f"Box {b}: {'take medicine out' if on else 'put medicine in'}",
                          bg="#e0f2fe" if on else "white")

    # ---- the doctor page ----
    def _render_main(self, live):
        S, st = self.S, self.store
        if live and S.doctor and (S.selected is None or st.by_id(S.selected) is None):
            ap = self.active_patient()
            S.selected = ap["id"] if ap else (st.patients[0]["id"] if st.patients else None)

        sig = (live, S.conn, S.doctor, S.selected, S.mode, S.box, S.pending_patient,
               json.dumps(S.rx, sort_keys=True), json.dumps(S.slots, sort_keys=True),
               tuple((p["id"], p["name"], p["status"], len(p["log"])) for p in st.patients),
               json.dumps(S.draft.get(S.selected), sort_keys=True), tuple(r["name"] for r in S.roles))
        if sig == self._main_sig:
            return
        self._main_sig = sig

        for w in self.main_area.winfo_children():
            w.destroy()
        area = self.main_area

        if not live:
            c = card(area, "Welcome", "Connect the dispenser with a USB cable, or try Demo mode.")
            c.pack(fill="x", padx=4, pady=8)
            tk.Label(c, text="Not connected." if S.conn == "off" else "Waiting for the dispenser to answer...",
                     bg=C["card"], fg=C["muted"], pady=20).pack(fill="x")
            return

        if not S.doctor:
            c = card(area, "Doctor login", "Enter your 4-digit doctor PIN. The dispenser checks it.")
            c.pack(anchor="w", padx=4, pady=8)
            row = tk.Frame(c, bg=C["card"])
            row.pack(anchor="w")
            self.doc_pin_var = tk.StringVar()
            self.doc_pin_var.trace_add("write", lambda *_: self._limit_pin())
            ent = tk.Entry(row, textvariable=self.doc_pin_var, show="•", width=8, justify="center",
                           font=("Consolas", 20, "bold"), relief="solid", bd=1)
            ent.pack(side="left")
            ent.bind("<Return>", lambda e: self._doctor_login())
            tk.Button(row, text="Log in", command=self._doctor_login, bg=C["primary"], fg="white",
                      relief="flat", padx=14, pady=4).pack(side="left", padx=10)
            ent.focus_set()
            return

        # patients bar
        c = card(area, "Patients", "Select a patient, or add a new one.")
        c.pack(fill="x", padx=4, pady=(8, 0))
        bar = tk.Frame(c, bg=C["card"])
        bar.pack(fill="x")
        ap = self.active_patient()
        for i, p in enumerate(st.patients):
            is_active = ap is not None and ap["id"] == p["id"]
            if is_active:
                txt, kind = ("Completed", "ok") if self.rx_complete() else ("On dispenser", "blue")
            else:
                txt, kind = {"complete": ("Completed", "ok"), "incomplete": ("Not finished", "warn"),
                             "active": ("Stopped", "warn")}.get(p["status"], ("No prescription", "info"))
            sel = S.selected == p["id"]
            bg, fg = TAG[kind]
            b = tk.Button(bar, text=f"{p['name']}\n{txt}", justify="left", anchor="w", width=16,
                          bg=C["primary_soft"] if sel else "white", relief="solid" if sel else "groove",
                          bd=2 if sel else 1, fg=C["ink"], font=("Segoe UI", 9, "bold"),
                          command=lambda pid=p["id"]: self._select_patient(pid))
            b.grid(row=i // 6, column=i % 6, padx=3, pady=3, sticky="w")
        n = len(st.patients)
        tk.Button(bar, text="+ Add patient", width=16, fg=C["primary"], relief="groove",
                  font=("Segoe UI", 9, "bold"), command=self._add_patient).grid(row=n // 6, column=n % 6,
                                                                                 padx=3, pady=3, sticky="w")

        p = st.by_id(S.selected) if S.selected is not None else None
        if p is None:
            c2 = card(area)
            c2.pack(fill="x", padx=4, pady=8)
            tk.Label(c2, text="Add a patient to start.", bg=C["card"], fg=C["muted"], pady=20).pack()
            return
        if ap is not None and ap["id"] == p["id"]:
            self._active_card(area, p)
        else:
            self._prescribe_card(area, p, ap)

    def _limit_pin(self):
        v = "".join(ch for ch in self.doc_pin_var.get() if ch.isdigit())[:4]
        if v != self.doc_pin_var.get():
            self.doc_pin_var.set(v)

    def _doctor_login(self):
        v = self.doc_pin_var.get().strip()
        if len(v) != 4:
            self.toast("Enter 4 digits")
            return
        self.send("DOCLOGIN " + v)

    def _select_patient(self, pid):
        self.S.selected = pid
        self.render()

    def _add_patient(self):
        p = self.store.add_patient()
        self.S.selected = p["id"]
        self.add_activity("info", "Patient added", p["name"])
        self.render()

    # patient currently on the dispenser
    def _active_card(self, area, p):
        S = self.S
        m = S.mode
        boxes = self.rx_boxes(S.rx)
        c = card(area)
        c.pack(fill="x", padx=4, pady=8)
        top = tk.Frame(c, bg=C["card"])
        top.pack(fill="x")
        tk.Label(top, text=p["name"], bg=C["card"], font=("Segoe UI", 12, "bold")).pack(side="left")
        tag(top, "On dispenser", "blue").pack(side="left", padx=8)
        tk.Label(c, text="Prescription stored on the dispenser. Progress updates live.", bg=C["card"],
                 fg=C["muted"], anchor="w").pack(fill="x", pady=(2, 8))

        phases = [("Prescribed", True),
                  ("Refill", m in ("REFILL_AUTH", "REFILL", "REFILL_DONE")),
                  ("Ready", m in ("READY", "AUTH", "DISPENSE", "LOCKED")),
                  ("Completed", self.rx_complete())]
        now_idx = next((i for i, (_, on) in enumerate(phases) if i > 0 and on), 0)
        if self.rx_complete():
            now_idx = 3
        if m in ("RESET_AUTH", "RESET"):
            now_idx = -1
        steps = tk.Frame(c, bg=C["card"])
        steps.pack(fill="x", pady=(0, 10))
        for i, (name, _) in enumerate(phases):
            if i < now_idx:
                bg, fg = C["l1_soft"], C["l1"]
            elif i == now_idx:
                bg, fg = C["primary"], "white"
            else:
                bg, fg = "#f1f3f5", "#9aa5b1"
            tk.Label(steps, text=name, bg=bg, fg=fg, font=("Segoe UI", 9, "bold"), pady=6).pack(
                side="left", fill="x", expand=True, padx=2)

        guide = None
        if m == "REFILL_AUTH":
            guide = ("info", "Refill phase", "The dispenser shows REFILLING. Enter the refill code and press # on its "
                                             "keypad. Only the boxes below will open, one at a time.")
        elif m == "REFILL":
            guide = ("info", f"Box {S.box or ''} is open", "Put the medicine shown below into the open box. The IR "
                     "sensor detects it and the box closes 5 seconds later, then the next box opens.")
        elif m == "REFILL_DONE":
            guide = ("ok", "All boxes filled", "Enter the refill code and press # again on the dispenser to finish. "
                                               "It will then show Dispenser Ready.")
        elif m in ("READY", "AUTH", "DISPENSE"):
            guide = ("ok", "Ready to dispense", "At the dispenser: choose a box, then log in with an RFID card or "
                     "PIN. The box opens and closes 5 s after the medicine is taken.")
        elif self.rx_complete() or m == "COMPLETE":
            guide = ("ok", "All medicine dispensed", f"The dispenser shows 'All dispensed - Refill/Prescribe' and no "
                     f"box can be opened. Either prescribe again (for {p['name']} or another patient), or enter the "
                     f"refill code + # on the dispenser to refill the same prescription.")
        elif m in ("RESET_AUTH", "RESET"):
            guide = ("warn", "Reset mode", "Enter the refill code and press # on the dispenser. All boxes will open."
                     if m == "RESET_AUTH" else "All boxes are open. Take everything out; when every IR sensor reads "
                                               "empty the dispenser shows Emptied.")
        if guide:
            alert(c, *guide).pack(fill="x", pady=(0, 10))

        tk.Label(c, text="Which medicine goes in which box", bg=C["card"], font=("Segoe UI", 10, "bold"),
                 anchor="w").pack(fill="x")
        for b in boxes:
            stt = S.slots[b]["st"]
            txt, kind = SLOT_TEXT.get(stt, (stt, "info"))
            cur = m in ("REFILL", "DISPENSE") and S.box == b
            row = tk.Frame(c, bg=C["card"], highlightbackground=C["primary"] if cur else C["line"],
                           highlightthickness=2 if cur else 1, padx=10, pady=8)
            row.pack(fill="x", pady=3)
            tk.Label(row, text=str(b), bg=BOX_COLOUR[b], fg="white", font=("Segoe UI", 11, "bold"),
                     width=2).pack(side="left")
            mid = tk.Frame(row, bg=C["card"])
            mid.pack(side="left", fill="x", expand=True, padx=10)
            tk.Label(mid, text=S.rx["meds"][b], bg=C["card"], font=("Segoe UI", 10, "bold"), anchor="w").pack(fill="x")
            hint = (f"Fill into Box {b} ({LEVELS[b]['name']})" if m.startswith("REFILL") and stt == "AWAIT"
                    else f"Box {b} · {LEVELS[b]['name']}")
            tk.Label(mid, text=hint, bg=C["card"], fg=C["muted"], font=("Segoe UI", 9), anchor="w").pack(fill="x")
            tag(row, txt, kind).pack(side="right")

        self._patient_log(c, p)

    # any other patient: prescribe
    def _prescribe_card(self, area, p, ap):
        S = self.S
        if p["id"] not in S.draft:
            keep = p["rx"] and p["status"] != "complete"
            S.draft[p["id"]] = {b: (p["rx"].get(str(b), "-") if keep else "-") for b in (1, 2, 3)}
        d = S.draft[p["id"]]
        chosen = [b for b in (1, 2, 3) if d.get(b) and d[b] != "-"]

        c = card(area)
        c.pack(fill="x", padx=4, pady=8)
        tk.Label(c, text=p["name"], bg=C["card"], font=("Segoe UI", 12, "bold"), anchor="w").pack(fill="x")
        tk.Label(c, text="Choose at most one medicine per severity level. Each level has its own box.",
                 bg=C["card"], fg=C["muted"], anchor="w").pack(fill="x", pady=(2, 6))

        busy = S.mode in ("REFILL", "DISPENSE", "RESET", "LOCKED")
        label, primary = "Send prescription to dispenser", True
        alerts = []
        loaded = any(S.slots[b]["st"] == "LOADED" for b in (1, 2, 3))
        if p["status"] == "incomplete":
            alerts.append(("warn", None, f"{p['name']}'s last prescription was stopped before all medicine was dispensed."))
        if p["status"] == "complete":
            alerts.append(("ok", None, f"{p['name']}'s last prescription was completed. You can prescribe again."))
        if S.pending_patient == p["name"] and S.mode in ("RESET_AUTH", "RESET"):
            alerts.append(("info", "Waiting for the reset",
                           f"The prescription for {p['name']} has been sent. The dispenser is in reset mode: enter "
                           f"the refill code on it, empty all boxes, and the refill for {p['name']} starts automatically."))
            label = "Send again"
        elif ap and not self.rx_complete():
            lst = "\n".join(f"  •  Box {b}: {S.rx['meds'][b]} - {SLOT_TEXT.get(S.slots[b]['st'], (S.slots[b]['st'],))[0]}"
                            for b in self.rx_boxes(S.rx))
            if loaded:
                alerts.append(("warn", f"{ap['name']}'s prescription is not finished",
                               f"Current status on the dispenser:\n{lst}\n\nProceeding puts the dispenser in reset "
                               f"mode: after the refill code is entered, all boxes open so the remaining medicine can "
                               f"be taken out. When the IR sensors read empty it shows Emptied and starts the refill "
                               f"for {p['name']}."))
                primary = False
            else:
                alerts.append(("info", f"{ap['name']}'s prescription has not been loaded yet",
                               f"Nothing is in the boxes, so {p['name']}'s prescription replaces it and the refill "
                               f"starts straight away (no reset needed).\n{lst}"))
            label = f"Proceed with {p['name']}"
        elif ap and self.rx_complete():
            alerts.append(("ok", None, f"{ap['name']}'s medicine has all been dispensed. The dispenser is free."))
        for kind, title, text in alerts:
            alert(c, kind, title, text).pack(fill="x", pady=(0, 8))

        levels = tk.Frame(c, bg=C["card"])
        levels.pack(fill="x")
        for col, b in enumerate((1, 2, 3)):
            L = LEVELS[b]
            has = bool(d.get(b)) and d[b] != "-"
            bg = BOX_SOFT[b] if has else C["card"]
            box = tk.Frame(levels, bg=bg, highlightbackground=BOX_COLOUR[b] if has else C["line"],
                           highlightthickness=2, padx=10, pady=8)
            box.grid(row=0, column=col, sticky="nsew", padx=4)
            levels.columnconfigure(col, weight=1)
            tk.Label(box, text=L["title"], bg=bg, fg=BOX_COLOUR[b], font=("Segoe UI", 8, "bold"),
                     anchor="w").pack(fill="x")
            tk.Label(box, text=L["name"], bg=bg, font=("Segoe UI", 12, "bold"), anchor="w").pack(fill="x", pady=(0, 6))
            var = tk.StringVar(value=d.get(b, "-"))
            for value, text in [("-", "None")] + [(m, m) for m in L["meds"]]:
                tk.Radiobutton(box, text=text, value=value, variable=var, bg=bg, anchor="w",
                               activebackground=bg, selectcolor="white",
                               command=lambda b=b, var=var: self._pick(b, var.get())).pack(fill="x")
            who = ", ".join(r["name"] for r in S.roles if r["level"] >= b) or "-"
            tk.Label(box, text=f"Can be collected by: {who}", bg=bg, fg=C["muted"], font=("Segoe UI", 8),
                     anchor="w", wraplength=220, justify="left").pack(fill="x", pady=(6, 0))

        row = tk.Frame(c, bg=C["card"])
        row.pack(fill="x", pady=(12, 0))
        enabled = bool(chosen) and not busy
        colour = C["primary"] if primary else C["l2"]
        tk.Button(row, text=label, command=self._send_prescription, state="normal" if enabled else "disabled",
                  bg=colour if enabled else "#c9d1d9", fg="white", disabledforeground="white", relief="flat",
                  padx=14, pady=6, font=("Segoe UI", 10, "bold")).pack(side="left")
        hint = ("The dispenser is busy - wait until the boxes are closed." if busy else
                "  ·  ".join(f"Box {b}: {d[b]}" for b in chosen) if chosen else "Pick at least one medicine.")
        tk.Label(row, text=hint, bg=C["card"], fg=C["muted"], padx=10).pack(side="left")

        self._patient_log(c, p)

    def _pick(self, box, value):
        self.S.draft[self.S.selected][box] = value
        self.after_idle(self.render)

    def _send_prescription(self):
        S, st = self.S, self.store
        p = st.by_id(S.selected)
        d = S.draft.get(p["id"], {})
        meds = {b: (d.get(b) or "-") for b in (1, 2, 3)}
        chosen = [b for b in (1, 2, 3) if meds[b] != "-"]
        st.plog(p, "Prescription sent: " + ", ".join(f"Box {b} {meds[b]}" for b in chosen))
        self.add_activity("info", "Prescription sent", f"{p['name']}: " + ", ".join(meds[b] for b in chosen))
        p["status"] = "sent"
        p["rx"] = {str(b): meds[b] for b in (1, 2, 3)}
        st.save()
        S.pending_patient = p["name"]
        S.draft.pop(p["id"], None)
        self.send(f"RX {p['name']}|{meds[1]}|{meds[2]}|{meds[3]}")
        self.render()

    def _patient_log(self, parent, p):
        if not p["log"]:
            return
        tk.Label(parent, text=f"History for {p['name']}", bg=C["card"], font=("Segoe UI", 10, "bold"),
                 anchor="w").pack(fill="x", pady=(14, 4))
        box = tk.Text(parent, height=min(8, len(p["log"])), font=("Segoe UI", 9), relief="solid", bd=1,
                      wrap="word", bg="#fbfcfd")
        for entry in p["log"]:
            box.insert("end", f"{time_str(entry['t'])}   {entry['text']}\n")
        box.configure(state="disabled")
        box.pack(fill="x")

    # ------------------------------------------------------------------
    # 7.6 Activity actions and closing
    # ------------------------------------------------------------------
    def export_csv(self):
        if not self.store.activity:
            self.toast("Nothing to export yet")
            return
        path = filedialog.asksaveasfilename(defaultextension=".csv", initialfile="medidispense_activity.csv",
                                            filetypes=[("CSV files", "*.csv")])
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["Date", "Time", "Event", "Details"])
            for a in reversed(self.store.activity):
                dt = datetime.datetime.fromtimestamp(a["t"] / 1000)
                w.writerow([dt.strftime("%Y-%m-%d"), dt.strftime("%H:%M:%S"), a["event"], a["details"]])
        self.toast(f"Saved {os.path.basename(path)}")

    def clear_activity(self):
        self.store.activity = []
        self.store.save()
        self.render_activity()

    def _on_close(self):
        try:
            if self.transport:
                self.transport.close()
        finally:
            self.store.save()
            self.destroy()


# =============================================================================
# 8. START
# =============================================================================

if __name__ == "__main__":
    app = App()
    app.render_activity()
    app.mainloop()
