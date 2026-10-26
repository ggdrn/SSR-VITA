#!/usr/bin/env python3
"""Summarises a Vita crash dump (.psp2dmp): every thread's name, PC and LR,
resolved to "lib+offset" using the module addresses printed in srr_log.txt.

Usage: scripts/parse_core.py <dump.psp2dmp> [srr_log.txt]
Then disassemble with:
  arm-vita-eabi-objdump -d --start-address=0xOFF-0x40 --stop-address=0xOFF+0x10 libX.so
"""
import gzip
import re
import struct
import sys


def notes(core):
    phoff, = struct.unpack_from("<I", core, 28)
    phnum, = struct.unpack_from("<H", core, 44)
    out = {}
    for i in range(phnum):
        ptype, off = struct.unpack_from("<2I", core, phoff + i * 32)
        if ptype != 4:
            continue
        nsz, dsz, _ = struct.unpack_from("<3I", core, off)
        name = core[off + 12:off + 12 + nsz].rstrip(b"\0").decode(errors="replace")
        start = off + 12 + ((nsz + 3) & ~3)
        out[name] = core[start:start + dsz]
    return out


def entries(blob):
    # Both THREAD_INFO and THREAD_REG_INFO: u32 ?, u32 count, then sized records.
    count, = struct.unpack_from("<I", blob, 4)
    off = 8
    for _ in range(count):
        size, = struct.unpack_from("<I", blob, off)
        yield blob[off:off + size]
        off += size


def load_modules(log_path):
    mods = []
    if log_path:
        for line in open(log_path, errors="replace"):
            m = re.search(r"so: (\S+) loaded at 0x([0-9A-Fa-f]+)-0x([0-9A-Fa-f]+)", line)
            if m:
                mods.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return mods


def where(addr, mods):
    for name, lo, hi in mods:
        if lo <= addr < hi:
            return f"{name}+0x{addr - lo:X}"
    if 0x81000000 <= addr < 0x82000000:
        return "eboot (loader)"
    return "?"


def main():
    raw = open(sys.argv[1], "rb").read()
    core = gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw
    mods = load_modules(sys.argv[2] if len(sys.argv) > 2 else None)
    n = notes(core)

    names = {}
    for t in entries(n["THREAD_INFO"]):
        uid, = struct.unpack_from("<I", t, 4)
        names[uid] = t[8:40].split(b"\0")[0].decode(errors="replace")

    for r in entries(n["THREAD_REG_INFO"]):
        uid, = struct.unpack_from("<I", r, 4)
        gpr = struct.unpack_from("<16I", r, 8)
        cpsr, = struct.unpack_from("<I", r, 8 + 64)
        pc, lr, sp = gpr[15], gpr[14], gpr[13]
        print(f"{names.get(uid, '?'):32s} uid=0x{uid:08X} PC=0x{pc:08X} [{where(pc, mods)}] "
              f"LR=0x{lr:08X} [{where(lr, mods)}] SP=0x{sp:08X} {'thumb' if cpsr & 0x20 else 'arm'}")
        if pc and where(pc, mods) not in ("?",):
            print("    " + " ".join(f"r{i}={v:08X}" for i, v in enumerate(gpr[:13])))


if __name__ == "__main__":
    main()
