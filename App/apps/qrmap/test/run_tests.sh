#!/usr/bin/env bash
# Copyright 2026 Johan Denoyer F4WAT, https://github.com/jdenoy
# Licensed under the Apache License, Version 2.0 (see the C sources).
# Modified by AislanJefferson for KLEANFW, 2026 (see git history).
# QR Map host tests. The encoder (qr.c) built for the host is checked against
# the qrcode library and decoded with OpenCV from a simulated LCD; then the
# whole app runs on a stub API and its screens are decoded, positions sent by
# the real APRS RX code included. Needs a host C compiler, python3, qrcode,
# opencv-python and zxing-cpp
# (pip install qrcode opencv-python-headless zxing-cpp).
set -euo pipefail
cd "$(dirname "$0")"
CC=${HOSTCC:-clang}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

$CC -O2 -Wall -Wextra -o "$T/host_qr" host_qr.c
python3 check_qr.py "$T/host_qr"

# The app on the host: app_api.h without its ARM layout asserts (64-bit
# pointers), in a copy of the tree so the app's "../app_api.h" finds it.
mkdir -p "$T/apps/qrmap/test"
python3 - "$T/apps/app_api.h" <<'PY'
import re, sys
api = open("../../app_api.h").read()
open(sys.argv[1], "w").write(re.sub(r"_Static_assert\(.*?\);", "", api, flags=re.S))
PY
python3 ../gen_assets.py "$T/apps/qrmap/qrmap_assets.bin" "$T/apps/qrmap/qrmap_assets.h"
cp ../qrmap_app.c ../qr.c ../qr.h "$T/apps/qrmap/"
cp host_app.c "$T/apps/qrmap/test/"
$CC -O1 -Wall -Wextra -o "$T/host_app" "$T/apps/qrmap/test/host_app.c"
# APRS RX on the same kind of stub, for its key 7 + MENU message
HOSTCC=$CC ../../aprsrx/test/run_qr_tests.sh "$T/aprs"
python3 check_app.py "$T/host_app" "$T/apps/qrmap/qrmap_assets.bin" \
    "$T/aprs/host_aprsrx" "$T/aprs/apps/aprsrx/aprsrx_assets.bin"
