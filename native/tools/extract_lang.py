#!/usr/bin/env python3
"""
extract_lang.py — emit a C translation unit holding one language module's
static tables, read out of an original SoftVoice language DLL.

WHY THIS EXISTS
---------------
The letter-to-sound rules and the character-class table are DATA, not code.
They are the property of SoftVoice, Inc. and must never be committed to this
repository, exactly as the native DLLs themselves are not committed.

So this runs at BUILD TIME against a DLL the user already owns, and writes a
generated .c into the build tree. Nothing proprietary enters git. The DLL is
opened as a file and parsed; it is never mapped, relocated or executed.

USAGE
    extract_lang.py TIENG32.DLL out/eng_lang_data.c --symbol sv_lang_data_eng

TABLE ADDRESSES
---------------
Recovered by disassembly; see native/REVERSING.md.

  charclass  VA 0x1C209C20  256 x uint16, indexed by unsigned char
  buckets    VA 0x1C24C744  base of a byte* table indexed by SIGNED char,
                            so bucket(c) = *(char**)(base + (signed char)c * 4).
                            Populated entries are 'A'..'Z' (0x1C24C848..8AC),
                            the Latin-1 letters at 0x1C24C8B0..8BC, and the
                            fallback bucket at 0x1C24C8C4.
  fallback   VA 0x1C24C8C4
  qmark_sub  VA 0x1C24C8C8  one byte, substituted for a bare '?' token

These are TIENG32 addresses. TISPAN32 has the same LoadLanguage shape but its
own layout, so pass --auto to locate the tables by signature instead.
"""

import argparse
import struct
import sys

ENG = {
    "image_base": 0x1C200000,
    "charclass": 0x1C209C20,
    "bucket_base": 0x1C24C744,
    "fallback": 0x1C24C8C4,
    "qmark_sub": 0x1C24C8C8,
}


class PE:
    """Minimal read-only PE section mapper. Does not load or relocate."""

    def __init__(self, path):
        self.b = open(path, "rb").read()
        if self.b[:2] != b"MZ":
            raise SystemExit(f"{path}: not a PE image")
        pe = struct.unpack_from("<I", self.b, 0x3C)[0]
        if self.b[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path}: bad PE signature")
        nsec = struct.unpack_from("<H", self.b, pe + 6)[0]
        opt = struct.unpack_from("<H", self.b, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", self.b, pe + 24 + 28)[0]
        self.sections = []
        so = pe + 24 + opt
        for i in range(nsec):
            o = so + i * 40
            name = self.b[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, roff = struct.unpack_from("<IIII", self.b, o + 8)
            self.sections.append((name, va, vsize, roff, rsize))

    def off(self, va):
        rva = va - self.image_base
        for name, sva, vsize, roff, rsize in self.sections:
            if sva <= rva < sva + max(vsize, rsize):
                d = rva - sva
                if d >= rsize:
                    raise SystemExit(f"VA {va:#x} lies in the uninitialised tail of {name}")
                return roff + d
        raise SystemExit(f"VA {va:#x} is not in any section")

    def u16(self, va):
        return struct.unpack_from("<H", self.b, self.off(va))[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.b, self.off(va))[0]

    def cstr_until(self, va, terms=b"\x5c\x60"):
        """Read a rule bucket: rules run until a NUL. Terminators stay in."""
        o = self.off(va)
        e = o
        while e < len(self.b) and self.b[e] != 0:
            e += 1
        return self.b[o:e]


def c_bytes(name, data):
    out = [f"static const unsigned char {name}[{len(data) + 1}] = {{"]
    for i in range(0, len(data), 16):
        row = ", ".join(f"0x{c:02x}" for c in data[i:i + 16])
        out.append(f"    {row},")
    out.append("    0x00")
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("out")
    ap.add_argument("--symbol", default="sv_lang_data_eng")
    ap.add_argument("--layout", default="eng", choices=["eng"])
    args = ap.parse_args()

    pe = PE(args.dll)
    L = ENG
    if pe.image_base != L["image_base"]:
        print(f"warning: image base {pe.image_base:#x} != expected "
              f"{L['image_base']:#x}; table addresses may be wrong",
              file=sys.stderr)

    charclass = [pe.u16(L["charclass"] + i * 2) for i in range(256)]

    # Bucket table is indexed by SIGNED char, matching the original
    # `*(byte **)(&base + (char)c * 4)`.
    # The matcher only ever indexes this table for c < 0x7B plus the four
    # Latin-1 letters it switches on explicitly, so sweeping all 256 slots
    # would drag in unrelated dwords that merely happen to be mapped.
    # Only 'A'..'Z' are reachable through the indexed table: the matcher
    # upper-cases before dispatch, and the four Latin-1 letters it handles come
    # from their own fixed pointers (below), not from the table. Slots outside
    # this range hold unrelated data in the original too — they are simply
    # never read.
    candidates = list(range(0x41, 0x5b))

    def looks_like_rules(ptr):
        """A bucket must point at rule text: '[' ... ']' ... '=' nearby."""
        try:
            o = pe.off(ptr)
        except SystemExit:
            return False
        window = pe.b[o:o + 64]
        lb = window.find(b"[")
        rb = window.find(b"]")
        return 0 <= lb < rb and window.find(b"=", rb) > 0

    buckets = {}
    for c in candidates:
        sc = c - 256 if c >= 128 else c
        va = L["bucket_base"] + sc * 4
        try:
            ptr = pe.u32(va)
        except SystemExit:
            continue
        if ptr and looks_like_rules(ptr):
            buckets[c] = ptr

    # The four Latin-1 letters the matcher switches on explicitly.
    for ch, va in ((0xc4, 0x1C24C8B0), (0xd6, 0x1C24C8B4),
                   (0xdf, 0x1C24C8B8), (0xdc, 0x1C24C8BC)):
        ptr = pe.u32(va)
        if ptr and looks_like_rules(ptr):
            buckets[ch] = ptr

    fallback = pe.u32(L["fallback"])
    qmark = pe.b[pe.off(L["qmark_sub"])]

    # All buckets point INTO one contiguous rule blob; a bucket is just an
    # offset into it, and matching runs off the end of one letter's rules into
    # the next letter's. So emit the blob once and index it by offset.
    all_ptrs = list(buckets.values()) + [fallback]
    blob_start = min(all_ptrs)
    blob_end = max(all_ptrs)
    o = pe.off(blob_end)
    while o < len(pe.b) and pe.b[o] != 0:
        o += 1
    blob = pe.b[pe.off(blob_start):o]

    # Drop pointers that are implausibly far from the cluster: the signed-char
    # sweep can pick up unrelated dwords that merely happen to land in a
    # mapped section.
    span = len(blob)
    buckets = {c: p for c, p in buckets.items()
               if 0 <= p - blob_start < span}

    w = []
    w.append("/* GENERATED by tools/extract_lang.py — do not edit, do not commit. */")
    w.append(f"/* source: {args.dll} */")
    w.append('#include "tispeech/ruleset.h"')
    w.append("")
    w.append(c_bytes("rule_blob", blob))
    w.append("")
    w.append("static const unsigned short charclass[256] = {")
    for i in range(0, 256, 8):
        w.append("    " + ", ".join(f"0x{v:04x}" for v in charclass[i:i + 8]) + ",")
    w.append("};")
    w.append("")
    w.append("static const unsigned char *const buckets[256] = {")
    for c in sorted(buckets):
        w.append(f"    [{c}] = rule_blob + {buckets[c] - blob_start},")
    w.append("};")
    w.append("")
    w.append(f"const sv_ruleset_t {args.symbol} = {{")
    w.append("    charclass,")
    w.append("    buckets,")
    w.append(f"    rule_blob + {fallback - blob_start},")
    w.append(f"    0x{qmark:02x}")
    w.append("};")

    with open(args.out, "w") as f:
        f.write("\n".join(w) + "\n")

    print(f"{args.out}: {len(buckets)} rule buckets, "
          f"{len(blob)} bytes of rule data")


if __name__ == "__main__":
    main()
