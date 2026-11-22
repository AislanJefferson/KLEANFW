#!/usr/bin/env bash
# Copyright 2026 AislanJefferson
# Licensed under the Apache License, Version 2.0 (see LICENSE).
# APRS RX's QR Map hand-off on the host: the real aprsrx_app.c on a stub API.
# With a directory argument, the host binary and assets are left there for
# ../../qrmap/test/run_tests.sh.
set -euo pipefail
cd "$(dirname "$0")"
CC=${HOSTCC:-cc}
if [ "$#" -ge 1 ]; then T=$1; mkdir -p "$T"
else T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; fi

# app_api.h without its ARM layout asserts (64-bit pointers), in a copy of the
# tree so the app's "../app_api.h" finds it.
mkdir -p "$T/apps/aprsrx/test"
python3 - "$T/apps/app_api.h" <<'PY'
import re, sys
api = open("../../app_api.h").read()
open(sys.argv[1], "w").write(re.sub(r"_Static_assert\(.*?\);", "", api, flags=re.S))
PY
python3 ../gen_assets.py "$T/apps/aprsrx/aprsrx_assets.bin" "$T/apps/aprsrx/aprsrx_assets.h"
cp ../aprsrx_app.c "$T/apps/aprsrx/"
cp host_qr.c "$T/apps/aprsrx/test/"
# The app defines its own memset/memcpy: keep the compiler from calling them
# from inside themselves.
$CC -O1 -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Wextra \
    -o "$T/host_aprsrx" "$T/apps/aprsrx/test/host_qr.c"
python3 check_qr.py "$T/host_aprsrx" "$T/apps/aprsrx/aprsrx_assets.bin"
