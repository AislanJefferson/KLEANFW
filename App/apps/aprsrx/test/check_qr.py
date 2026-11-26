#!/usr/bin/env python3
# Copyright 2026 AislanJefferson
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""APRS RX -> QR Map: the real aprsrx_app.c on a stub API (host_qr.c).

   check_qr.py <host_qr> <aprsrx_assets.bin>
"""
import os, struct, subprocess, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mice import _frame, encode

HOST, ASSETS = sys.argv[1], sys.argv[2]
COMPACT, SCROLL = 14, 0
fails = 0


def run(steps, view=COMPACT, installed=True):
    out = subprocess.run([HOST, ASSETS, str(view), "1" if installed else "0", *steps],
                         capture_output=True, text=True, timeout=20)
    if out.returncode:
        raise RuntimeError(f"host exit {out.returncode}: {out.stdout[-200:]}")
    return out.stdout.splitlines()


def f(frame):
    return "f:" + frame.hex()


def k(keys):
    return [f"k:{c}" for c in keys]


def pos_frame(info, call="F4HWN"):
    return _frame(call, "APZK5", (), info.encode("latin-1"))


def handoff(lines):
    h = [l for l in lines if l.startswith("H ")]
    if not h:
        return None
    assert len(h) == 1, h
    name, data = h[0][2:].rsplit(" ", 1)
    raw = bytes.fromhex(data)
    lat, lon = struct.unpack_from("<ii", raw)
    return name, lat, lon, raw[8:].decode("ascii")


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL {name} {detail}")


def cas(deg, minutes, hundredths, negative=False):
    v = ((deg * 60 + minutes) * 100 + hundredths) * 60
    return -v if negative else v


# Positions, in both views: the same value whatever the row layout.
GOOD = [
    ("standard N/E", pos_frame("!4850.90N/00216.25E-test"), cas(48, 50, 90), cas(2, 16, 25)),
    ("standard S/W", pos_frame("=3352.13S\\15112.56W>x"), cas(33, 52, 13, True), cas(151, 12, 56, True)),
    ("timestamp", pos_frame("@300158z4921.07N/00150.50EN OISE"), cas(49, 21, 7), cas(1, 50, 50)),
    ("ambiguity", pos_frame("!49  .  N/00150.50E-"), cas(49, 0, 0), cas(1, 50, 50)),
    ("limits", pos_frame("!9000.00S/18000.00W-"), -32400000, -64800000),
    ("zero", pos_frame("!0000.00N/00000.00E-"), 0, 0),
    ("Mic-E FT3D", _frame("F4HWN", "TXUPX9", (), b"`x,5l \x1c[/`_0\r"), 17585340, 817500),
    ("Mic-E S/E", encode("F4HWN", -33.8688, 151.2093), cas(33, 52, 13, True), cas(151, 12, 56)),
    ("Mic-E N/W", encode("F4HWN", 40.7128, -74.0060), cas(40, 42, 77), cas(74, 0, 36, True)),
    ("Mic-E 179W", encode("F4HWN", 1.0, -179.99), cas(1, 0, 0), cas(179, 59, 40, True)),
]
for view in (COMPACT, SCROLL):
    for name, frame, lat, lon in GOOD:
        lines = run([f(frame), *k("7")], view)
        got = handoff(lines)
        check(f"{name} message", got == ("QR Map", lat, lon, "F4HWN"), f"view {view}: {got}")

# Nothing that is not a usable position opens QR Map.
BAD = [
    ("minutes 60", pos_frame("!4860.00N/00216.25E-")),
    ("lon minutes 60", pos_frame("!4850.90N/00260.25E-")),
    ("lat over 90", pos_frame("!9000.01N/00216.25E-")),
    ("lon over 180", pos_frame("!4850.90N/18000.01E-")),
    ("letter in lat", pos_frame("!48A0.90N/00216.25E-")),
    ("control byte", _frame("F4HWN", "APZK5", (), b"!48\x010.90N/00216.25E-")),
    ("bad hemisphere", pos_frame("!4850.90X/00216.25E-")),
    ("E in latitude", pos_frame("!4850.90E/00216.25E-")),
    ("N in longitude", pos_frame("!4850.90N/00216.25N-")),
    ("compressed", pos_frame("!/5L!!<*e7>7P[")),
    ("status", pos_frame(">status text")),
    ("Mic-E type, not a Mic-E address", _frame("F4HWN", "APZK5", (), b"`x,5l \x1c[/`_0\r")),
]
for view in (COMPACT, SCROLL):
    for name, frame in BAD:
        lines = run([f(frame), *k("7")], view)
        check(f"{name} no hand-off", handoff(lines) is None, f"view {view}")

A = pos_frame("!4850.90N/00216.25E-", "F4AAA")
B = pos_frame("!1000.00S/02000.00W-", "F4BBB-12")
A_POS = ("QR Map", cas(48, 50, 90), cas(2, 16, 25), "F4AAA")
B_POS = ("QR Map", cas(10, 0, 0, True), cas(20, 0, 0, True), "F4BBB-12")

# 7 opens QR Map at once; other keys keep their own job and do not open it.
lines = run([f(A), *k("1"), *k("7")])
check("7 after another key", handoff(lines) == A_POS, f"{handoff(lines)}")
lines = run([f(A), *k("M")])
check("MENU does nothing", handoff(lines) is None)
lines = run([f(A), *k("X")])
check("EXIT quits", handoff(lines) is None and lines[-1] == "R", f"{lines[-3:]}")

# A new frame: 7 applies to the frame shown, the newest.
lines = run([f(A), f(B), *k("7")])
check("the new frame", handoff(lines) == B_POS, f"{handoff(lines)}")

# The frame on screen is the one sent, also an older one.
lines = run([f(A), f(B), *k("D"), *k("7")])
check("older frame", handoff(lines) == A_POS, f"{handoff(lines)}")
lines = run([f(A), f(B), *k("D"), *k("U"), *k("7")])
check("back to the newest", handoff(lines) == B_POS, f"{handoff(lines)}")

# QR Map not installed: capture reopened, frames kept, a notice, keys alive.
# The key after the notice only hides it (key 2 would clear the frames).
lines = run([f(A), *k("7"), *k("2"), *k("7")], installed=False)
check("missing: tried twice", lines.count("H- QR Map") == 2, f"{lines}")
check("missing: capture reopened", lines.count("O") == 3 and "E" not in lines, f"{lines}")
check("missing: notice", any(l.endswith(" NO QR Map") for l in lines), f"{lines}")
after = lines[lines.index("H- QR Map"):]
check("missing: frame kept", any(l.startswith("B -89dBm") for l in after), f"{after}")
between = after[1:after.index("H- QR Map", 1)]
check("missing: key 2 only hides the notice", "B -89dBm" in between, f"{between}")
check("missing: exits", lines[-1] == "R")

# Coming back from QR Map with EXIT still down must not quit APRS RX.
lines = run(["^:X", f(A), *k("7")])
check("held EXIT ignored", handoff(lines) == A_POS, f"{lines[-4:]}")

if fails:
    sys.exit(f"FAIL aprsrx hand-off: {fails} checks")
print(f"APRS RX -> QR Map: {len(GOOD)} positions and {len(BAD)} rejections in both views, "
      "key 7 at once, new frame, older frame, app missing, held EXIT OK")
