#!/usr/bin/env python3 -I
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
"""Turn a Quansheng stock firmware into a multiboot slot image.

Supported inputs, each pinned by SHA-256 (see VARIANTS):

    k5v3  UV-K5 V3, version block 7.00.15
    k1    UV-K1,    version block 7.03.05

The stock image is decoded (128-byte XOR key), the 16-byte version block and
the 2-byte checksum are removed, and the KLEANFW boot stub is appended at
0x08014400. Inside the stock, the SystemInit literal at 0x080028E8 is redirected
at the stub and eight 4-byte SPI address emitters become bl's to the stub's
bank trampolines; nothing else changes.

The result is a raw internal-flash image, the same kind of file as
f4hwn.kleanfw.<version>.bin: the multiboot host tool builds the 4 KiB slot
header and the image goes at slot_base + 0x1000.

Usage (most people want tools/stockpatch/make_multiboot.sh instead):
    patch_stock.py FIRMWARE [-m k1|k5v3] [-o OUT]   # patch
    patch_stock.py FIRMWARE --rebuild               # round-trip self-check
    patch_stock.py FIRMWARE --dump-raw PATH         # decoded stock image
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

STUB = REPO / "build/stockpatch/stub.bin"
OUT_DIR = REPO / "build/stockpatch"

# Quansheng's firmware obfuscation key (128 bytes, applied cyclically).
XOR_KEY = bytes.fromhex(
    "4722c0525d574894b16060db6fe34c7c"
    "d84ad68b30ec25e04cd9007fbfe35405"
    "e93a976bb06e0cfbb11ae2c9c15647e9"
    "baf142b6675f0f96f7c93c841b26e14e"
    "3b6f66e6a06ab0bfc6a5703aba189e27"
    "1a535b71b1941e18f2d6810222fd5a28"
    "91dbba5d64c6fe86839c501c730311d6"
    "af30f42c77b27dbb3f29285722d6928b"
)

VERSION_OFFSET = 0x2000
VERSION_LEN = 16
TRAILER_LEN = 2
CRC16_POLY = 0x1021

IMAGE_BASE = 0x08002800
EXPECTED_RESET = 0x080028D5  # Reset_Handler, same in every supported build

# Boot hook: literal pool word holding the SystemInit pointer (same address in
# every supported build; its value differs).
BOOT_LITERAL = 0x080028E8

# SPI address hooks. Each site holds exactly two Thumb instructions (4 bytes)
# that extract one address byte from r4; the whole pair is replaced by a bl to
# a trampoline in the stub that does the same from the banked address.
ADDR_HI = bytes.fromhex("2002000e")  # lsls r0,r4,#8  ; lsrs r0,r0,#24
ADDR_LO = bytes.fromhex("2004000e")  # lsls r0,r4,#16 ; lsrs r0,r0,#24


def hook_sites(shift):
    """The eight sites of the K5 V3 7.00.15 SPI driver, moved by `shift`.

    The K1 7.03.05 driver is the same code at -0x1DC (AUDIT.md section 15).
    """
    k5 = (
        (0x0800F89C, ADDR_HI, "tramp_addr_hi"),  # SectorErase          @ 0x0800F878
        (0x0800F8A4, ADDR_LO, "tramp_addr_lo"),
        (0x0800F8F4, ADDR_HI, "tramp_addr_hi"),  # SectorErase + cache  @ 0x0800F8D0
        (0x0800F8FC, ADDR_LO, "tramp_addr_lo"),
        (0x0800F97A, ADDR_HI, "tramp_addr_hi"),  # Read(buf,addr,len)   @ 0x0800F958
        (0x0800F982, ADDR_LO, "tramp_addr_lo"),
        (0x0800FBD6, ADDR_HI, "tramp_addr_hi"),  # PageProgram          @ 0x0800FBB0
        (0x0800FBDE, ADDR_LO, "tramp_addr_lo"),
    )
    return tuple((addr + shift, expected, symbol) for addr, expected, symbol in k5)


# Every offset and expected byte below was verified against these exact files.
VARIANTS = {
    "91845f1defa29dd1cad21bea468a05b6e91be9309956a0c34fed2c3756608d2e": {
        "model": "k5v3",
        "radio": "UV-K5 V3",
        "version": "7.00.15",
        "image_size": 71772,          # ends at 0x0801405C
        "sp": 0x20002FA0,
        "system_init": 0x080111E5,
        "region_table": 0x08014008,
        "hooks": hook_sites(0),
    },
    "26d9e46b23ca6a3b2870aa651fe17551042c30b0756a9f860f879c88ce756b6b": {
        "model": "k1",
        "radio": "UV-K1",
        "version": "7.03.05",
        "image_size": 71332,          # ends at 0x08013EA4
        "sp": 0x200031A0,
        "system_init": 0x08011009,
        "region_table": 0x08013E50,
        "hooks": hook_sites(-0x1DC),
    },
}

MODEL_ALIASES = {"k1": "k1", "k5v3": "k5v3", "k5": "k5v3"}

# Stub-side scratch word the trampolines read (see stub_hooks.S).
BANK_WORD = 0x20003FFC

STUB_BASE = 0x08014400
STUB_MAX = 0x08020000 - STUB_BASE
SLOT_LIMIT = 0x1D800


def die(msg):
    sys.exit(f"error: {msg}")


def xor_apply(data):
    key = XOR_KEY
    return bytes(b ^ key[i % len(key)] for i, b in enumerate(data))


def crc16_xmodem(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def unpack(decoded):
    """Decoded file -> (image, version block, checksum bytes)."""
    end = len(decoded) - TRAILER_LEN
    image = decoded[:VERSION_OFFSET] + decoded[VERSION_OFFSET + VERSION_LEN:end]
    return image, decoded[VERSION_OFFSET:VERSION_OFFSET + VERSION_LEN], decoded[end:]


def pack(image, version, trailer):
    """(image, version block, checksum bytes) -> decoded file."""
    return image[:VERSION_OFFSET] + version + image[VERSION_OFFSET:] + trailer


def word(image, addr):
    return struct.unpack_from("<I", image, addr - IMAGE_BASE)[0]


def load_stock(path, model=None):
    if not path.is_file():
        die(f"firmware file not found: {path}")
    raw = path.read_bytes()

    digest = hashlib.sha256(raw).hexdigest()
    variant = VARIANTS.get(digest)
    if variant is None:
        known = ", ".join(f"{v['model']} {v['version']}" for v in VARIANTS.values())
        die(f"{path.name} is not a supported stock firmware (sha256 {digest}).\n"
            f"  supported: {known}")
    if model is not None and variant["model"] != model:
        die(f"{path.name} is the {variant['radio']} firmware ({variant['version']}), "
            f"not {model}; use -m {variant['model']}")

    if crc16_xmodem(raw[:-TRAILER_LEN]) != struct.unpack("<H", raw[-TRAILER_LEN:])[0]:
        die("checksum mismatch: the file is corrupt")

    decoded = xor_apply(raw)
    image, version, trailer = unpack(decoded)

    if len(image) != variant["image_size"]:
        die(f"decoded image is {len(image)} bytes, expected {variant['image_size']}")
    if version.rstrip(b"\0").decode("ascii", "replace") != variant["version"]:
        die(f"version block is {version!r}, expected {variant['version']}")
    if word(image, IMAGE_BASE) != variant["sp"]:
        die(f"initial stack pointer does not read 0x{variant['sp']:08X}")
    if word(image, IMAGE_BASE + 4) != EXPECTED_RESET:
        die(f"reset vector does not point at 0x{EXPECTED_RESET:08X}")

    return variant, raw, decoded, image, version, trailer


def elf_entry(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        die(f"{path} is not a 32-bit little-endian ELF")
    return struct.unpack_from("<I", data, 0x18)[0]


def elf_symbols(path):
    """Name -> address for every symbol in the stub's .symtab.

    The trampoline addresses move with every stub rebuild, so they are read
    from the ELF instead of being pinned in this script.
    """
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        die(f"{path} is not a 32-bit little-endian ELF")

    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    shnum = struct.unpack_from("<H", data, 0x30)[0]

    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, kind, _flags, _addr, offset, size, link, _info, _align, entsize = struct.unpack_from(
            "<IIIIIIIIII", data, off
        )
        sections.append((kind, offset, size, link, entsize))

    symtabs = [s for s in sections if s[0] == 2]  # SHT_SYMTAB
    if not symtabs:
        die(f"{path} has no symbol table")

    symbols = {}
    for _kind, offset, size, link, entsize in symtabs:
        strtab = sections[link]
        for off in range(offset, offset + size, entsize or 16):
            st_name, st_value = struct.unpack_from("<II", data, off)
            start = strtab[1] + st_name
            end = data.find(b"\0", start)
            name = data[start:end].decode("ascii", "replace")
            if name:
                symbols[name] = st_value
    return symbols


def thumb_bl(addr, target):
    """Encode a Thumb-2 BL from addr to target."""
    offset = target - (addr + 4)
    if not -(1 << 24) <= offset < (1 << 24):
        die(f"bl from 0x{addr:08X} to 0x{target:08X} is out of range")

    value = offset & 0x1FFFFFF
    s = (value >> 24) & 1
    i1 = (value >> 23) & 1
    i2 = (value >> 22) & 1
    j1 = 1 - (i1 ^ s)
    j2 = 1 - (i2 ^ s)
    imm10 = (value >> 12) & 0x3FF
    imm11 = (value >> 1) & 0x7FF

    half1 = 0xF000 | (s << 10) | imm10
    half2 = 0xD000 | (j1 << 13) | (1 << 12) | (j2 << 11) | imm11
    return struct.pack("<HH", half1, half2)


def decode_thumb_bl(addr, data):
    """Inverse of thumb_bl(), used to verify what was written."""
    half1, half2 = struct.unpack("<HH", data)
    if (half1 & 0xF800) != 0xF000 or (half2 & 0xD000) != 0xD000:
        die(f"not a bl instruction at 0x{addr:08X}")

    s = (half1 >> 10) & 1
    imm10 = half1 & 0x3FF
    j1 = (half2 >> 13) & 1
    j2 = (half2 >> 11) & 1
    imm11 = half2 & 0x7FF
    i1 = 1 - (j1 ^ s)
    i2 = 1 - (j2 ^ s)

    offset = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if s:
        offset -= 1 << 25
    return addr + 4 + offset


def verify_boot_hook(variant, image):
    found = word(image, BOOT_LITERAL)
    if found != variant["system_init"]:
        die(f"unexpected word at 0x{BOOT_LITERAL:08X}: 0x{found:08X}, "
            f"expected 0x{variant['system_init']:08X}")


def verify_hooks(variant, image):
    for addr, expected, _symbol in variant["hooks"]:
        found = image[addr - IMAGE_BASE:addr - IMAGE_BASE + 4]
        if found != expected:
            die(f"unexpected bytes at 0x{addr:08X}: {found.hex()}, expected {expected.hex()}")


def verify_ram(variant, image):
    """The stub's bank word must sit above everything the stock's __main
    initialises: read the scatter-load region table and check that the last
    zero-init region ends at the initial SP, below BANK_WORD."""
    table = variant["region_table"]
    end = 0
    for entry in range(2):
        _load, dst, size, _fn = struct.unpack_from("<4I", image, table + 16 * entry - IMAGE_BASE)
        if not 0x20000000 <= dst < 0x20004000:
            die(f"region table entry {entry} at 0x{table:08X} does not target RAM")
        end = max(end, dst + size)
    if end != variant["sp"]:
        die(f"__main initialises RAM up to 0x{end:08X}, expected the SP 0x{variant['sp']:08X}")
    if end > BANK_WORD:
        die(f"__main initialises RAM past the bank word 0x{BANK_WORD:08X}")
    if struct.pack("<I", BANK_WORD) in image:
        die(f"the stock references the bank word 0x{BANK_WORD:08X}")


def load_stub(path):
    if not path.is_file():
        die(f"stub not built: {path} (run tools/stockpatch/build_stub.sh)")

    elf = path if path.suffix == ".elf" else path.with_suffix(".elf")
    if not elf.is_file():
        die(f"stub ELF not found next to {path.name}; run tools/stockpatch/build_stub.sh")

    entry = elf_entry(elf)
    if entry & 1 != 1 or (entry & ~1) != STUB_BASE:
        die(f"stub entry is 0x{entry:08X}, expected 0x{STUB_BASE | 1:08X}")

    code = path.read_bytes()
    if not code:
        die("stub binary is empty")
    if len(code) > STUB_MAX:
        die(f"stub is {len(code)} bytes, budget is {STUB_MAX} (ends at 0x08020000)")

    return code, elf


def build(variant, image, stub_path, no_hooks):
    verify_boot_hook(variant, image)
    verify_hooks(variant, image)
    verify_ram(variant, image)
    code, elf = load_stub(stub_path)

    symbols = elf_symbols(elf)
    for symbol in ("tramp_addr_hi", "tramp_addr_lo", "stub_stock_system_init"):
        if symbol not in symbols:
            die(f"{elf.name} has no {symbol} symbol (rebuild the stub)")
    trampolines = {s: symbols[s] & ~1 for s in ("tramp_addr_hi", "tramp_addr_lo")}
    for address in trampolines.values():
        if not STUB_BASE <= address < 0x08020000:
            die(f"trampoline at 0x{address:08X} is outside the stub area")

    # The stub calls the stock's SystemInit through a word of its own: fill it
    # with the value the boot literal is about to lose.
    sysinit_word = symbols["stub_stock_system_init"]
    off = sysinit_word - STUB_BASE
    if sysinit_word & 3 or not 0 <= off <= len(code) - 4:
        die(f"stub_stock_system_init at 0x{sysinit_word:08X} is not an aligned stub word")
    if code[off:off + 4] != b"\xff\xff\xff\xff":
        die("stub_stock_system_init is not the erased placeholder")
    code = bytearray(code)
    struct.pack_into("<I", code, off, variant["system_init"])
    code = bytes(code)

    gap = STUB_BASE - (IMAGE_BASE + len(image))
    if gap < 0:
        die("the stock image already reaches the stub area")

    patched = bytearray(image)
    struct.pack_into("<I", patched, BOOT_LITERAL - IMAGE_BASE, STUB_BASE | 1)

    sites = () if no_hooks else variant["hooks"]
    for addr, _expected, symbol in sites:
        target = trampolines[symbol]
        bl = thumb_bl(addr, target)
        if decode_thumb_bl(addr, bl) != target:
            die(f"bl encoding check failed for 0x{addr:08X}")
        patched[addr - IMAGE_BASE:addr - IMAGE_BASE + 4] = bl

    patched += b"\xff" * gap
    patched += code

    if len(patched) > SLOT_LIMIT:
        die(f"patched image is {len(patched)} bytes, the slot limit is {SLOT_LIMIT}")

    # Self-check: only the boot literal and the hook sites change, the tail is
    # exactly the erased gap plus the stub, and the stub calls the right
    # SystemInit.
    lit = BOOT_LITERAL - IMAGE_BASE
    allowed = set(range(lit, lit + 4))
    for addr, _expected, _symbol in sites:
        allowed.update(range(addr - IMAGE_BASE, addr - IMAGE_BASE + 4))
    diff = {i for i in range(len(image)) if image[i] != patched[i]}
    if not diff or not diff <= allowed:
        die(f"unexpected difference at offsets {[hex(i) for i in sorted(diff)[:8]]}")
    for addr, _expected, symbol in sites:
        if decode_thumb_bl(addr, bytes(patched[addr - IMAGE_BASE:addr - IMAGE_BASE + 4])) != trampolines[symbol]:
            die(f"hook at 0x{addr:08X} does not reach {symbol}")
    if patched[len(image):] != b"\xff" * gap + code:
        die("appended stub area does not match the stub binary")
    if word(patched, sysinit_word) != variant["system_init"]:
        die("stub SystemInit word was not written")

    return bytes(patched), len(code), trampolines, sysinit_word


def default_output(variant, no_hooks):
    suffix = ".nohooks" if no_hooks else ""
    return OUT_DIR / f"f4hwn.{variant['model']}.stock.v{variant['version']}{suffix}.bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="original stock firmware .bin")
    parser.add_argument("-m", "--model", help="expected model: k1 or k5v3 (default: detected from the file)")
    parser.add_argument("--stub", type=Path, default=STUB, help="stub binary (default: %(default)s)")
    parser.add_argument("-o", "--output", type=Path, help="slot image to write "
                        "(default: build/stockpatch/f4hwn.<model>.stock.v<version>.bin)")
    parser.add_argument("--rebuild", action="store_true", help="only check the decode/encode round trip")
    parser.add_argument("--dump-raw", type=Path, metavar="PATH", help="write the decoded stock image and stop")
    parser.add_argument("--no-hooks", action="store_true",
                        help="diagnostic: keep the 8 SPI sites original (stub runs, no bank remap)")
    args = parser.parse_args()

    model = None
    if args.model is not None:
        model = MODEL_ALIASES.get(args.model.lower())
        if model is None:
            die(f"unknown model {args.model!r}; use k1 or k5v3")

    variant, raw, decoded, image, version, trailer = load_stock(args.input, model)

    if pack(image, version, trailer) != decoded or xor_apply(decoded) != raw:
        die("decode/encode round trip does not reproduce the input file")

    if args.rebuild:
        print(f"ok: {args.input} ({variant['model']} {variant['version']}) decodes and re-encodes byte for byte")
        return

    if args.dump_raw:
        args.dump_raw.parent.mkdir(parents=True, exist_ok=True)
        args.dump_raw.write_bytes(image)
        print(f"wrote {args.dump_raw} ({len(image)} bytes, base 0x{IMAGE_BASE:08X})")
        return

    patched, stub_len, trampolines, sysinit_word = build(variant, image, args.stub, args.no_hooks)

    output = args.output or default_output(variant, args.no_hooks)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(patched)

    print(f"wrote {output}")
    print(f"  model    {variant['model']} ({variant['radio']}), stock {variant['version']}")
    print(f"  image    {len(patched)} / {SLOT_LIMIT} bytes (base 0x{IMAGE_BASE:08X}, "
          f"ends 0x{IMAGE_BASE + len(patched):08X})")
    print(f"  stub     {stub_len} bytes at 0x{STUB_BASE:08X}")
    print(f"  patched  0x{BOOT_LITERAL:08X}: 0x{variant['system_init']:08X} -> 0x{STUB_BASE | 1:08X}")
    print(f"  sysinit  stub word 0x{sysinit_word:08X} = 0x{variant['system_init']:08X}")
    if args.no_hooks:
        print("  hooks    none (--no-hooks)")
    else:
        for symbol, address in trampolines.items():
            print(f"  {symbol:<13} 0x{address:08X}")
    print(f"  bank word 0x{BANK_WORD:08X}")


if __name__ == "__main__":
    main()
