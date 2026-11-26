#!/usr/bin/env bash
# Copyright 2026 AislanJefferson
# https://github.com/AislanJefferson/KLEANFW
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build a multiboot slot image from an original Quansheng stock firmware.
#
#   tools/stockpatch/make_multiboot.sh <firmware.bin> -m <k1|k5v3> [-o <out.bin>]
#
# The firmware is checked against the model (SHA-256): a K5 V3 file is refused
# for -m k1 and vice versa. Requires Docker with the uvk1-uvk5v3 image (the
# same one the firmware builds with) and python3.
#
set -euo pipefail

usage() {
    cat <<'TXT'
Usage: make_multiboot.sh <firmware.bin> -m <k1|k5v3> [-o <out.bin>]

Builds a multiboot slot image from an original Quansheng stock firmware.

  <firmware.bin>   original stock firmware, as published (.bin)
  -m, --model      radio model: k1 (UV-K1) or k5v3 (UV-K5 V3, alias k5)
  -o, --output     output image (default: build/stockpatch/f4hwn.<model>.stock.v<version>.bin)
  -h, --help       this help

Supported firmware:
  k1    UV-K1     version block 7.03.05
  k5v3  UV-K5 V3  version block 7.00.15

Examples:
  tools/stockpatch/make_multiboot.sh K1ENG_v7.03.05_publish.bin -m k1
  tools/stockpatch/make_multiboot.sh K5ENG_v7.00.15_publish.bin -m k5v3 -o /tmp/stock.bin
TXT
}

fail() {
    echo "error: $*" >&2
    exit 1
}

firmware=""
model=""
output=""

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -m|--model)
            [ $# -ge 2 ] || { usage >&2; fail "-m needs a model (k1 or k5v3)"; }
            model=$2; shift 2 ;;
        -o|--output)
            [ $# -ge 2 ] || { usage >&2; fail "-o needs a file name"; }
            output=$2; shift 2 ;;
        -*) usage >&2; fail "unknown option: $1" ;;
        *)
            [ -z "$firmware" ] || { usage >&2; fail "only one firmware file at a time"; }
            firmware=$1; shift ;;
    esac
done

[ -n "$firmware" ] || { usage >&2; fail "missing the firmware file"; }
[ -n "$model" ] || { usage >&2; fail "missing -m <k1|k5v3>"; }

case "$(printf '%s' "$model" | tr '[:upper:]' '[:lower:]')" in
    k1) model=k1 ;;
    k5v3|k5) model=k5v3 ;;
    *) usage >&2; fail "unknown model '$model' (use k1 or k5v3)" ;;
esac

[ -f "$firmware" ] || fail "firmware file not found: $firmware"

# Paths given by the user are relative to where they ran the script.
firmware=$(cd "$(dirname "$firmware")" && pwd)/$(basename "$firmware")
if [ -n "$output" ]; then
    mkdir -p "$(dirname "$output")"
    output=$(cd "$(dirname "$output")" && pwd)/$(basename "$output")
fi

command -v python3 >/dev/null || fail "python3 not found"
command -v docker >/dev/null || fail "docker not found"
docker image inspect "${IMAGE:-uvk1-uvk5v3}" >/dev/null 2>&1 ||
    fail "Docker image ${IMAGE:-uvk1-uvk5v3} not found (build it as for the firmware)"

here=$(cd "$(dirname "$0")" && pwd)

# Check the file before spending time on the stub build.
python3 -I "$here/patch_stock.py" "$firmware" -m "$model" --rebuild >/dev/null

echo "==> building the boot stub"
"$here/build_stub.sh" | tail -n 2

echo "==> patching $(basename "$firmware") for $model"
args=("$firmware" -m "$model")
[ -z "$output" ] || args+=(-o "$output")
python3 -I "$here/patch_stock.py" "${args[@]}"

echo
echo "Done. Send the image to a slot 1-4 with UV Studio, then select it in the"
echo "multiboot menu (MENU at power-on). The first boot takes a few seconds"
echo "while the slot's config bank is prepared: do not power off."
