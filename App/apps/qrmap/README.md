# QR Map: a position as a QR code, for a phone

QR Map shows a position that another app hands over as a QR code on the
radio's screen. A phone scans it and opens the position on a map, so a
received position goes to the phone without typing a digit. In KLEANFW,
APRS RX, EPIRB 406 and the Sonde M10, MRZ and RS41 apps send it (key 7,
at once); any app can do the same (see
[Hand-off message](#hand-off-message)).

Imported from F4HWN's firmware and adapted for KLEANFW. Needs app API level 3
(`handoff` / `handoff_read`).

## Using it

1. In APRS RX, show a frame with a position and press 7 (no confirmation).
   This closes APRS RX: its received frames are lost and nothing is received
   while the code is on screen.
2. QR Map opens with the code on the left and, on the right, the format, the
   latitude and longitude it holds, and the label sent with it (the callsign).
3. UP / DOWN (UV-K1: LEFT / RIGHT) change the format, or 1 to 5 choose one
   directly. The choice is saved on exit; the first time it is GOOGLE.
4. Scan the code with the phone's camera or a QR code reader.
5. EXIT goes back to the app that sent the position, which starts empty.

The backlight stays on while the code is shown, for the camera.

| Key | Format | What the code holds | Android | iPhone |
|---|---|---|---|---|
| 1 | GOOGLE | `https://maps.google.com/?q=48.123333,-1.234444` | Google Maps app, or the browser, with a marker | Google Maps app if installed, else Safari |
| 2 | APPLE | `https://maps.apple.com/?q=48.123333,-1.234444` | The browser (Apple Maps on the web) | Apple Maps, with a pin |
| 3 | GEO | `geo:48.123333,-1.234444` | The map app chooser (Google Maps, Organic Maps, OsmAnd...), offline maps included | Not reliable: the Camera may show it as text only |
| 4 | OSM | `https://osm.org/?mlat=48.123333&mlon=-1.234444` | The browser: OpenStreetMap with a marker | Same, in Safari |
| 5 | TEXT | `48.123333,-1.234444` | The coordinates as text, to copy into any app | Same |

Both phones' cameras open `https://` links directly, so GOOGLE, the default,
works everywhere. GEO is the one for offline map apps on Android, but how a
scanner handles `geo:` depends on the scanner: scanner apps of the ZXing family
offer to open a map, the camera or Google Lens may show the text only.

Launched from the Apps menu, or with a message without a valid position, QR
Map shows a page explaining where the position comes from; EXIT quits.

## Accuracy

The position is never converted on the way: the sender hands its own
centi-arcseconds over. QR Map writes decimal degrees with 6 decimals,
rounded to the nearest: the text is within 0.5e-6 degree (5 cm) of the
position received. For comparison, an APRS position resolves 0.01 minute of
arc (about 18 m).

## Hand-off message

The sending app calls `handoff("QR Map", msg, len)` and quits. The message,
little-endian:

| Bytes | Field |
|---|---|
| 0-3 | Latitude, `int32_t`, centi-arcseconds (1/100 second of arc), north positive, ±90 degrees (±32,400,000) |
| 4-7 | Longitude, `int32_t`, centi-arcseconds, east positive, ±180 degrees (±64,800,000) |
| 8-31 | Optional label, ASCII, up to the end of the message or a NUL; 15 characters are shown |

APRS RX converts the position of the frame shown to this unit. A message
shorter than 8 bytes or out of range shows the "no position" page.

On EXIT QR Map hands off back to the sender, without a message: the sender
starts again as when launched from the menu.

A sender builds the message and quits:

```c
int32_t m[5] = { latS, lonS };                 /* then up to 12 label bytes */
memcpy(m + 2, label, label_len);
if (g.A->handoff("QR Map", m, 8u + label_len)) /* false: QR Map not installed */
    quit();                                    /* the loader launches QR Map */
```

In this firmware `handoff()` closes the RX audio capture before it looks the
app up: a sender that keeps running after `false` must open it again.

## The code

`qr.c` encodes one fixed size: version 3 (29 × 29 modules), error correction
level L, byte mode, one block of 55 data and 15 error correction codewords, so
up to 53 bytes. The longest text, an OSM or Google link at -90 / -180
degrees, is 49 bytes. The encoder follows ISO/IEC 18004: bit stream,
Reed-Solomon over GF(256), function patterns, zigzag placement, then the mask
with the lowest penalty score and its format information. A row of the
symbol is one `uint32_t`, so the symbol and its function map take 232 bytes of
stack; there is no division (the masks keep `x mod 3` as counters).

On screen, each module is 2 × 2 pixels: 58 × 58 pixels at (4, 3), status line
included, about 13 mm wide on the LCD. The quiet zone is 1.5 modules above and
below (the screen is 64 pixels high), 2 on the left and 3 on the right before
the text. Scanners usually accept that, and the glass around the pixels
widens it.

## Size

**2,516 bytes** of the 4,096-byte overlay, plus 324 bytes of assets (texts and
the five formats).

## Tests (`test/`)

```
test/run_tests.sh
```

Needs a host C compiler, python3, and the `qrcode`, `opencv-python` and
`zxing-cpp` packages.

- `host_qr.c` + `check_qr.py`: 289 codes (every length from 0 to 53, random
  bytes, the five formats at the coordinate extremes) are bit for bit the ones
  the `qrcode` library makes with the same version, level and mask. The map
  codes, drawn as on the LCD with text beside them, decode at three scales
  with OpenCV and with ZXing, the decoder of many phone scanner apps. With a
  dark bezel right at the screen edge, ZXing still reads 105 of 105 and
  OpenCV 103 of 105: the radio test tells how much margin the real LCD
  leaves.
- `host_app.c` + `check_app.py`: the whole app on a stub API. The codes
  decoded from the frame buffer (OpenCV and ZXing must agree) for each format
  and key, the text beside them, the saved format, the return hand-off, the
  "no position" page, the labels, a key still held at launch, and the rounding
  of every case against exact fractions. Through the real APRS RX code on a
  stub API (`../../aprsrx/test/host_qr.c`), received frames go from key 7 to
  the code: each one holds the position of the frame, rounded to 1e-6
  degree, with the source callsign as the label.

## Credits

- The encoder (`qr.c`) follows the structure of Project Nayuki's
  [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library)
  (MIT License): Reed-Solomon divisor and remainder, function patterns, format
  bits, zigzag placement, mask penalty. It is rewritten for one fixed version,
  `uint32_t` rows and no division; its MIT notice is in `qr.c`.
- The symbol is specified by ISO/IEC 18004:2015. QR Code is a registered
  trademark of DENSO WAVE INCORPORATED.
- The tests use the [`qrcode`](https://github.com/lincolnloop/python-qrcode)
  Python library as the reference encoder, and
  [OpenCV](https://opencv.org) and [zxing-cpp](https://github.com/zxing-cpp/zxing-cpp)
  as decoders; none of their code is in the app.
- `build.sh` and `app.ld` are the repository's overlay-app templates.

## Version

`APP_VER` in `build.sh` is bumped for every build that goes on a radio.
Current source: **v1.1-klean** (F4HWN's QR Map 1.1, adapted for APRS RX).
