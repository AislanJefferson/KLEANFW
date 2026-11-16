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
# Disassemble the decoded stock firmware with the same toolchain the project
# uses. Output: build/stockpatch/stock.dis (default) or the path given.
#
# Usage: tools/stockpatch/disasm.sh <input.bin> [-m k1|k5v3] [output.dis]
#
set -euo pipefail

INPUT=""
MODEL=()
OUTPUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -m|--model) MODEL=(-m "$2"); shift 2 ;;
        *) if [ -z "$INPUT" ]; then INPUT=$1; else OUTPUT=$1; fi; shift ;;
    esac
done
[ -n "$INPUT" ] || { echo "usage: $0 <input.bin> [-m k1|k5v3] [output.dis]" >&2; exit 1; }
INPUT=$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")

cd "$(dirname "$0")/../.."

IMAGE=${IMAGE:-uvk1-uvk5v3}
OUTPUT=${OUTPUT:-build/stockpatch/stock.dis}

mkdir -p build/stockpatch

python3 -I tools/stockpatch/patch_stock.py "$INPUT" "${MODEL[@]}" --dump-raw build/stockpatch/stock.raw

docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src "$IMAGE" bash -c "
set -euo pipefail
arm-none-eabi-objdump -D -b binary -marm -Mforce-thumb \
    --adjust-vma=0x08002800 build/stockpatch/stock.raw > '$OUTPUT'
echo \"wrote $OUTPUT (\$(wc -l < '$OUTPUT') lines)\"
"
