#!/usr/bin/env python3
# Copyright 2026 Johan Denoyer F4WAT, https://github.com/jdenoy
# Licensed under the Apache License, Version 2.0 (see the C sources).
# Modified by AislanJefferson for KLEANFW, 2026 (see git history).
"""QR Map end to end on a stub API (host_app.c): what the screen shows for
each hand-off message and key, decoded from the frame buffer with OpenCV and
ZXing (the engine of many phone scanner apps), which must agree.
With APRS RX's host build, received frames also go through its key 7 + MENU
message: the code must hold the position of the frame, rounded to 1e-6 degree,
and the source callsign must show as the label.

   ./check_app.py ./host_app ../qrmap_assets.bin [host_aprsrx aprsrx_assets.bin]
"""
import os, struct, subprocess, sys
from fractions import Fraction
import numpy as np, cv2, zxingcpp

HOST, ASSETS = sys.argv[1], sys.argv[2]
APRSRX = sys.argv[3:5] if len(sys.argv) > 4 else None
det = cv2.QRCodeDetector()
fails = 0

def run(msg, keys="", cfg=0):
    out = subprocess.run([HOST, ASSETS, "none" if msg is None else msg.hex(), keys, str(cfg)],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    frames, texts, handoff, saved, i = [], [], None, None, 0
    while i < len(out):
        l = out[i]
        if l == "F":
            frames.append((texts, out[i + 1:i + 65])); texts = []; i += 65; continue
        if l.startswith("T "):
            _, x, y, s = l.split(" ", 3); texts.append((int(x), int(y), s))
        elif l.startswith("H "):
            handoff = l[2:]
        elif l.startswith("C "):
            saved = int(l[2:])
        i += 1
    return frames, handoff, saved

def decode(rows):
    img = np.array([[c == "#" for c in r] for r in rows])
    lcd = np.where(img, 30, 200).astype(np.uint8)
    lcd = cv2.copyMakeBorder(lcd, 6, 6, 6, 6, cv2.BORDER_CONSTANT, value=200)   # blank glass
    img = cv2.resize(lcd, None, fx=4, fy=4, interpolation=cv2.INTER_NEAREST)
    cv = det.detectAndDecode(img)[0]
    zx = [r.text for r in zxingcpp.read_barcodes(img) if r.format == zxingcpp.BarcodeFormat.QRCode]
    return cv if [cv] == zx or (not cv and not zx) else f"OpenCV {cv!r} / ZXing {zx!r}"   # both must agree

def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL {name} {detail}")

def pos(lat, lon, label=b""):          # centi-arcseconds
    return struct.pack("<ii", lat, lon) + label

def deg(v):                            # what the text must say: exact, rounded
    m = round(Fraction(abs(v), 360000) * 10**6)
    return ("-" if v < 0 and m else "") + f"{m // 10**6}.{m % 10**6:06d}"

# A sender's position as centi-arcseconds (48 07'24" N, 1 14'04" W), then its label.
LAT, LON = (48 * 3600 + 7 * 60 + 24) * 100, -(1 * 3600 + 14 * 60 + 4) * 100
SENT = pos(LAT, LON, b"F4HWN-7\0")
C = f"{deg(LAT)},{deg(LON)}"
assert C == "48.123333,-1.234444", C
GEO, GOOGLE, TEXT = "geo:" + C, "https://maps.google.com/?q=" + C, C
APPLE = "https://maps.apple.com/?q=" + C
OSM = f"https://osm.org/?mlat={deg(LAT)}&mlon={deg(LON)}"

# A position from APRS RX: every format, UP/DOWN wrap, direct keys, return.
frames, handoff, saved = run(SENT, "DDDDD" + "U" + "4" + "1", cfg=0)
want = [GOOGLE, TEXT, OSM, GEO, APPLE, GOOGLE, APPLE, OSM, GOOGLE]   # start, D x5 (back), U, 4, 1
got = [decode(f[1]) for f in frames]
check("formats", got == want, f"{got}")
t = {s for _, _, s in frames[0][0]}
check("panel", {"1/5 GOOGLE", "LAT 48.123333", "LON -1.234444", "F4HWN-7",
                "UP/DN FORMAT", "EXIT  BACK"} <= t, f"{sorted(t)}")
check("return hand-off", handoff == "APRS RX 0", f"{handoff}")
check("format saved", saved == 0, f"{saved}")

# The saved format comes back; junk in the config falls back to geo:.
frames, _, saved = run(SENT, "", cfg=2)
check("cfg 2", decode(frames[0][1]) == GEO and saved == 2)
frames, _, _ = run(SENT, "", cfg=0xFF)
check("cfg junk", decode(frames[0][1]) == GOOGLE)

# MENU, which confirmed in APRS RX, still held at launch is not a key press.
frames, _, _ = run(SENT, "^", cfg=0)
check("held MENU", len(frames) == 1 and decode(frames[0][1]) == GOOGLE, f"{len(frames)} frames")

# Every centi-arcsecond rounds to the nearest 1e-6 degree: extremes, both
# signs, a rest of 359999 (0.999997, the largest), a sweep, no "-0.000000".
cases = [(32400000, 64800000), (-32400000, -64800000), (0, 0), (-1, 1), (-17, 17),
         (-18, 18), (32399999, -64799999), (359999, -360001), (-12040008, 54444444)]
cases += [(v, -v - 7) for v in range(0, 3600, 61)]
for lat, lon in cases:
    frames, _, _ = run(pos(lat, lon), "5")
    want, got = f"{deg(lat)},{deg(lon)}", decode(frames[1][1])
    check(f"text {lat},{lon}", got == want, f"{got} != {want}")

# No position: from the menu (no hand-off back), too short, out of range.
for name, msg in [("menu", None), ("short", pos(1, 2)[:7]),
                  ("lat range", pos(32400001, 0)), ("lon range", pos(0, -64800001))]:
    frames, handoff, _ = run(msg)
    t = [s for _, _, s in frames[0][0]]
    check(f"nopos {name}", "NO POSITION RECEIVED." in t and decode(frames[0][1]) == "", f"{t}")
    check(f"nopos {name} hand-off", (handoff is None) == (msg is None), f"{handoff}")

# A sender in the sondes' layout (sonde_info_t: id[12] before latS / lonS,
# both in centi-arcseconds): it copies latitude, longitude, then its ID into
# a 20-byte message, as the README shows. M20-like ID, metre-level position.
SONDE_LAT, SONDE_LON = 17589123, 1234567          # 48.858675 N, 3.429353 E
frames, handoff, _ = run(pos(SONDE_LAT, SONDE_LON, b"ME1234567\0\0\0"), "5")
want = f"{deg(SONDE_LAT)},{deg(SONDE_LON)}"
t = [s for x, y, s in frames[1][0] if y == 27]
check("sonde layout", decode(frames[1][1]) == want and t == ["ME1234567"], f"{decode(frames[1][1])} {t}")

# Label: up to 15 characters shown, unprintable bytes as '?', stops at a NUL.
for label, shown in [(b"F4WAT-9", "F4WAT-9"), (b"\x01ab\x7f", "?ab?"),
                     (b"ABCDEFGHIJKLMNOPQRSTUVWX", "ABCDEFGHIJKLMNO"), (b"AB\0CD", "AB")]:
    frames, _, _ = run(pos(0, 0, label))
    t = [s for x, y, s in frames[0][0] if y == 27]
    check(f"label {label!r}", t == [shown], f"{t}")

# APRS RX -> QR Map: a received frame, key 7, MENU, then the code on screen.
naprs = 0
if APRSRX:
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "aprsrx", "test"))
    from mice import _frame, encode
    def cas(d, m, h, neg=False):
        v = ((d * 60 + m) * 100 + h) * 60
        return -v if neg else v
    for name, frame, lat, lon, call in [
        ("standard", _frame("F4HWN-7", "APZK5", (), b"!4850.90N/00216.25E-test"),
         cas(48, 50, 90), cas(2, 16, 25), "F4HWN-7"),
        ("standard S/W", _frame("PY2ABC-15", "APZK5", (), b"=2332.87S/04637.65W>x"),
         cas(23, 32, 87, True), cas(46, 37, 65, True), "PY2ABC-15"),
        ("limits", _frame("F4HWN", "APZK5", (), b"!9000.00S/18000.00W-"),
         -32400000, -64800000, "F4HWN"),
        ("Mic-E FT3D", _frame("F4HWN", "TXUPX9", (), b"`x,5l \x1c[/`_0\r"),
         17585340, 817500, "F4HWN"),
        ("Mic-E S/E", encode("F4HWN-9", -33.8688, 151.2093),
         cas(33, 52, 13, True), cas(151, 12, 56), "F4HWN-9"),
    ]:
        out = subprocess.run([APRSRX[0], APRSRX[1], "14", "1", "f:" + frame.hex(), "k:7", "k:M"],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        sent = [l for l in out if l.startswith("H QR Map ")]
        check(f"aprs {name} sent", len(sent) == 1, f"{out}")
        if not sent:
            continue
        frames, handoff, _ = run(bytes.fromhex(sent[0].split()[-1]), "", cfg=4)   # TEXT
        want, got = f"{deg(lat)},{deg(lon)}", decode(frames[0][1])
        label = [s for x, y, s in frames[0][0] if y == 27]
        check(f"aprs {name}", got == want and label == [call], f"{got} {label} != {want} {call}")
        check(f"aprs {name} return", handoff == "APRS RX 0", f"{handoff}")
        naprs += 1

if fails:
    sys.exit(f"FAIL app: {fails} checks")
print("app: formats, keys, panel, config, extremes, no-position pages and labels OK"
      + (f"; {naprs} APRS frames exact through key 7 + MENU" if APRSRX else ""))
