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
# Build the stock boot stub. Same cross toolchain and Docker image the firmware
# and the overlay apps use; the result is build/stockpatch/stub.{elf,bin}.
#
# Objects are compiled separately (not one gcc command) so the map names them
# the way cmake/check_mb_ramfunc.cmake expects, and the restore copier's
# isolation can be audited with the same gate the firmware uses.
#
# Usage: tools/stockpatch/build_stub.sh
#
set -euo pipefail

cd "$(dirname "$0")/../.."

IMAGE=${IMAGE:-uvk1-uvk5v3}
OUT=build/stockpatch

mkdir -p "$OUT"

docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src "$IMAGE" bash -c '
set -euo pipefail

OUT=build/stockpatch
OBJ=$OUT/obj
INC="-IApp
     -IDrivers/CMSIS/Include
     -IDrivers/CMSIS/Device/PY32F071/Include
     -IDrivers/PY32F071_HAL_Driver/Inc
     -Itools/stockpatch/stub"
DEFS="-DPY32F071x8 -DUSE_FULL_LL_DRIVER
      -DENABLE_FEAT_F4HWN_MULTIBOOT
      -DENABLE_FEAT_F4HWN_MULTIBOOT_OVERLAY
      -DEDITION_STRING=\"STOCK\" -DVERSION_STRING_2=\"v7.00.15\""
CFLAGS="-mcpu=cortex-m0plus -mthumb -Os -std=gnu11
        -ffreestanding -fno-builtin -fno-common -fomit-frame-pointer
        -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter"
LDFLAGS="-nostdlib -nostartfiles
         -T tools/stockpatch/stub/stub.ld
         -Wl,--gc-sections -Wl,--build-id=none -Wl,--no-warn-rwx-segments
         -Wl,-Map=$OUT/stub.map"

rm -rf "$OBJ"
mkdir -p "$OBJ"

SOURCES="
tools/stockpatch/stub/stub_entry.S
tools/stockpatch/stub/stub_hooks.S
tools/stockpatch/stub/stub_main.c
tools/stockpatch/stub/stub_hw.c
tools/stockpatch/stub/stub_shim.c
tools/stockpatch/stub/stub_ui.c
tools/stockpatch/stub/stub_mb.c
tools/stockpatch/stub/stub_sel.c
App/font.c
App/driver/mb_flash.c
App/driver/mb_flash_logic.c
Drivers/PY32F071_HAL_Driver/Src/py32f071_ll_gpio.c
Drivers/PY32F071_HAL_Driver/Src/py32f071_ll_spi.c
"

OBJECTS=""
for src in $SOURCES; do
    # bash: name the object "<file>.obj", the convention the map checker looks for
    obj="$OBJ/$(basename "$src").obj"
    # shellcheck disable=SC2086
    arm-none-eabi-gcc $CFLAGS $INC $DEFS -c "$src" -o "$obj"
    OBJECTS="$OBJECTS $obj"
done

# shellcheck disable=SC2086
arm-none-eabi-gcc $LDFLAGS $OBJECTS -lgcc -o "$OUT/stub.elf"

arm-none-eabi-objcopy -O binary "$OUT/stub.elf" "$OUT/stub.bin"
arm-none-eabi-size "$OUT/stub.elf"

# Same isolation gate the firmware applies to its own restore copier: the RAM
# routine must contain no external code/data reference.
cmake -DOBJDUMP=/opt/toolchain/bin/arm-none-eabi-objdump \
      -DELF=$OUT/stub.elf -DMAP=$OUT/stub.map -DBINARY_DIR=/src \
      -P cmake/check_mb_ramfunc.cmake

echo "stub.bin: $(wc -c < "$OUT/stub.bin") bytes (budget 48128)"
'
