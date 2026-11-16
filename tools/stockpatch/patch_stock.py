#!/usr/bin/env python3 -I
"""Turn the Quansheng stock firmware into a multiboot slot image.

The stock image is decoded (128-byte XOR key), the 16-byte version block and
the 2-byte checksum are removed, and the KLEANFW boot stub is appended at
0x08014400. The only change inside the stock is the SystemInit pointer at
0x080028E8, which is redirected at the stub; no instruction is rewritten.

The resulting file is a raw internal-flash image, the same kind of file as
f4hwn.kleanfw.<version>.bin: the multiboot host tool builds the 4 KiB slot
header (magic "FMB1", image CRC-32) and the image goes at slot_base + 0x1000.

Usage:
    tools/stockpatch/patch_stock.py                  # patch
    tools/stockpatch/patch_stock.py --rebuild        # round-trip self-check
    tools/stockpatch/patch_stock.py --dump-raw PATH  # decoded stock image
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

INPUT = REPO / "K5ENG_v7.00.15_publish.bin"
STUB = REPO / "build/stockpatch/stub.bin"
OUTPUT = REPO / "build/stockpatch/f4hwn.stock.v7.00.15.bin"

# v7.00.15 as published by Quansheng. Anything else is refused: every offset
# and every expected byte below was verified against this exact file.
EXPECTED_SHA256 = "91845f1defa29dd1cad21bea468a05b6e91be9309956a0c34fed2c3756608d2e"

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
IMAGE_SIZE = 71772  # ends at 0x0801405C
EXPECTED_SP = 0x20002FA0
EXPECTED_RESET = 0x080028D5  # Reset_Handler

# Boot hook: literal pool word holding the SystemInit pointer.
BOOT_LITERAL = 0x080028E8
STOCK_SYSTEM_INIT = 0x080111E5

# SPI address hooks. Each site holds exactly two Thumb instructions (4 bytes)
# that extract one address byte from r4; the whole pair is replaced by a bl to
# a trampoline in the stub that does the same from the banked address.
ADDR_HI = bytes.fromhex("2002000e")  # lsls r0,r4,#8  ; lsrs r0,r0,#24
ADDR_LO = bytes.fromhex("2004000e")  # lsls r0,r4,#16 ; lsrs r0,r0,#24
HOOK_SITES = (
    # SectorErase(addr)                        @ 0x0800F878
    (0x0800F89C, ADDR_HI, "tramp_addr_hi"),
    (0x0800F8A4, ADDR_LO, "tramp_addr_lo"),
    # SectorErase(addr) + sector-cache invalidate @ 0x0800F8D0
    (0x0800F8F4, ADDR_HI, "tramp_addr_hi"),
    (0x0800F8FC, ADDR_LO, "tramp_addr_lo"),
    # Read(buf,addr,len)                       @ 0x0800F958
    (0x0800F97A, ADDR_HI, "tramp_addr_hi"),
    (0x0800F982, ADDR_LO, "tramp_addr_lo"),
    # PageProgram(buf,addr,len)                @ 0x0800FBB0
    (0x0800FBD6, ADDR_HI, "tramp_addr_hi"),
    (0x0800FBDE, ADDR_LO, "tramp_addr_lo"),
)

# Stub-side scratch word the trampolines read (see stub_hooks.S).
BANK_WORD = 0x20003FFC

STUB_BASE = 0x08014400
STUB_MAX = 0x08020000 - STUB_BASE


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
    decoded = (
        image[:VERSION_OFFSET]
        + version
        + image[VERSION_OFFSET:]
        + trailer
    )
    return decoded


def load_stock(path):
    raw = path.read_bytes()

    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        die(f"unexpected firmware file\n  expected sha256 {EXPECTED_SHA256}\n  got      {digest}")

    if crc16_xmodem(raw[:-TRAILER_LEN]) != struct.unpack("<H", raw[-TRAILER_LEN:])[0]:
        die("checksum mismatch: the file is corrupt")

    decoded = xor_apply(raw)
    image, version, trailer = unpack(decoded)

    if len(image) != IMAGE_SIZE:
        die(f"decoded image is {len(image)} bytes, expected {IMAGE_SIZE}")
    if not version.startswith(b"7.00.15"):
        die(f"version block is {version!r}, expected 7.00.15")
    if struct.unpack_from("<I", image, 0)[0] != EXPECTED_SP:
        die("initial stack pointer does not read 0x20002FA0")
    if struct.unpack_from("<I", image, 4)[0] != EXPECTED_RESET:
        die("reset vector does not point at 0x080028D5")

    return raw, decoded, image, version, trailer


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


def verify_boot_hook(image):
    found = struct.unpack_from("<I", image, BOOT_LITERAL - IMAGE_BASE)[0]
    if found != STOCK_SYSTEM_INIT:
        die(
            f"unexpected word at 0x{BOOT_LITERAL:08X}: 0x{found:08X}, "
            f"expected 0x{STOCK_SYSTEM_INIT:08X}"
        )


def verify_hooks(image):
    for addr, expected, _symbol in HOOK_SITES:
        found = image[addr - IMAGE_BASE:addr - IMAGE_BASE + 4]
        if found != expected:
            die(
                f"unexpected bytes at 0x{addr:08X}: {found.hex()}, "
                f"expected {expected.hex()}"
            )


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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", nargs="?", type=Path, default=INPUT, help="stock .bin (default: %(default)s)")
    parser.add_argument("--stub", type=Path, default=STUB, help="stub binary (default: %(default)s)")
    parser.add_argument("-o", "--output", type=Path, default=OUTPUT, help="slot image to write (default: %(default)s)")
    parser.add_argument("--rebuild", action="store_true", help="only check the decode/encode round trip")
    parser.add_argument("--dump-raw", type=Path, metavar="PATH", help="write the decoded stock image and stop")
    args = parser.parse_args()

    raw, decoded, image, version, trailer = load_stock(args.input)

    if pack(image, version, trailer) != decoded or xor_apply(decoded) != raw:
        die("decode/encode round trip does not reproduce the input file")

    if args.rebuild:
        print(f"ok: {args.input} decodes and re-encodes byte for byte")
        return

    if args.dump_raw:
        args.dump_raw.parent.mkdir(parents=True, exist_ok=True)
        args.dump_raw.write_bytes(image)
        print(f"wrote {args.dump_raw} ({len(image)} bytes, base 0x{IMAGE_BASE:08X})")
        return

    verify_boot_hook(image)
    verify_hooks(image)
    code, elf = load_stub(args.stub)

    symbols = elf_symbols(elf)
    trampolines = {}
    for symbol in ("tramp_addr_hi", "tramp_addr_lo"):
        if symbol not in symbols:
            die(f"{elf.name} has no {symbol} symbol")
        trampolines[symbol] = symbols[symbol] & ~1

    for address in trampolines.values():
        if not STUB_BASE <= address < 0x08020000:
            die(f"trampoline at 0x{address:08X} is outside the stub area")

    gap = STUB_BASE - (IMAGE_BASE + len(image))
    if gap < 0:
        die("the stock image already reaches the stub area")

    if struct.pack("<I", BANK_WORD) in image:
        die(f"the stock references the bank word 0x{BANK_WORD:08X}")

    patched = bytearray(image)
    struct.pack_into("<I", patched, BOOT_LITERAL - IMAGE_BASE, STUB_BASE | 1)

    for addr, _expected, symbol in HOOK_SITES:
        target = trampolines[symbol]
        bl = thumb_bl(addr, target)
        if decode_thumb_bl(addr, bl) != target:
            die(f"bl encoding check failed for 0x{addr:08X}")
        off = addr - IMAGE_BASE
        patched[off:off + 4] = bl

    patched += b"\xff" * gap
    patched += code

    if len(patched) > 0x1D800:
        die(f"patched image is {len(patched)} bytes, the slot limit is {0x1D800}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(patched)

    # Self-check: only the boot literal and the eight hook sites change, and
    # the tail is exactly the erased gap plus the stub.
    lit = BOOT_LITERAL - IMAGE_BASE
    allowed = set(range(lit, lit + 4))
    for addr, _expected, _symbol in HOOK_SITES:
        allowed.update(range(addr - IMAGE_BASE, addr - IMAGE_BASE + 4))
    diff = {i for i in range(len(image)) if image[i] != patched[i]}
    if not diff or not diff <= allowed:
        die(f"unexpected difference at file offsets {[hex(i) for i in sorted(diff)[:8]]}")
    if patched[len(image):] != b"\xff" * gap + code:
        die("appended stub area does not match the stub binary")

    print(f"wrote {args.output}")
    print(f"  image    {len(patched)} / {0x1D800} bytes (base 0x{IMAGE_BASE:08X}, ends 0x{IMAGE_BASE + len(patched):08X})")
    print(f"  stub     {len(code)} bytes at 0x{STUB_BASE:08X}")
    print(f"  patched  0x{BOOT_LITERAL:08X}: 0x{STOCK_SYSTEM_INIT:08X} -> 0x{STUB_BASE | 1:08X}")
    for symbol, address in trampolines.items():
        print(f"  {symbol:<13} 0x{address:08X}")
    print(f"  bank word 0x{BANK_WORD:08X}")


if __name__ == "__main__":
    main()
