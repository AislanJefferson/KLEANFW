#!/usr/bin/env bash
#
# Disassemble the decoded stock firmware with the same toolchain the project
# uses. Output: build/stockpatch/stock.dis (default) or the path given.
#
# Usage: tools/stockpatch/disasm.sh [input.bin] [output.dis]
#
set -euo pipefail

cd "$(dirname "$0")/../.."

IMAGE=${IMAGE:-uvk1-uvk5v3}
INPUT=${1:-K5ENG_v7.00.15_publish.bin}
OUTPUT=${2:-build/stockpatch/stock.dis}

mkdir -p build/stockpatch

python3 -I tools/stockpatch/patch_stock.py "$INPUT" --dump-raw build/stockpatch/stock.raw

docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src "$IMAGE" bash -c "
set -euo pipefail
arm-none-eabi-objdump -D -b binary -marm -Mforce-thumb \
    --adjust-vma=0x08002800 build/stockpatch/stock.raw > '$OUTPUT'
echo \"wrote $OUTPUT (\$(wc -l < '$OUTPUT') lines)\"
"
