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
    extract_lang.py TISPAN32.DLL out/span_lang_data.c --language span

--symbol defaults from --language (sv_lang_data_eng / sv_lang_data_span) if
omitted. --language defaults to eng for backward compatibility with existing
invocations that predate Spanish support.

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

These are TIENG32 addresses. TISPAN32's matcher is a separately-compiled copy
of the same source at a different VA (0x1C4062F0, found by searching for the
matcher's own prologue bytes -- see below), with its own table layout at
--language span. It was disassembled, not assumed: the disassembly shows
`cmp cl, 0x7a; ja <fallback>` gates the general (non-special-cased) bucket
lookup for EVERY value above 0x7a, so the six accented letters Spanish
actually uses (Á É Í Ñ Ó Ú) never reach a per-letter bucket at all -- they
fall to the shared fallback bucket regardless of the ALPHA bit being set in
their charclass entry (confirmed: charclass[0xC9] etc. all have 0x0080 set).
Only four Latin-1 codepoints are special-cased in EITHER binary's compiled
switch: 0xC4, 0xD6, 0xDC, 0xDF -- German-alphabet code points, present
verbatim in TISPAN32 too because this dispatch is shared, language-independent
machinery (see REVERSING.md, "the language modules are one code base"). So
--language span needs exactly the same shape of table as --language eng, just
at Spanish's own addresses; no extra negative-index sweep is needed.

TISPAN32 also confirms the "fallback = last Latin-1 special + 8, qmark_sub =
fallback + 4" spacing holds in both binaries (checked byte-for-byte, not
assumed): ENG 0x1C24C8BC + 8 = 0x1C24C8C4 (fallback), + 4 = 0x1C24C8C8
(qmark); SPAN 0x1C40E3B4 + 8 = 0x1C40E3BC (fallback), + 4 = 0x1C40E3C0
(qmark). Both qmark_sub bytes are literally '?' (0x3F) -- i.e. the bare-'?'
substitution is a no-op in both shipped languages, not something distinct we
mis-extracted.
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
    # The four Latin-1 codepoints the compiled switch special-cases, VA of
    # each one's own bucket pointer (see module docstring).
    "latin1": {0xC4: 0x1C24C8B0, 0xD6: 0x1C24C8B4, 0xDC: 0x1C24C8BC, 0xDF: 0x1C24C8B8},
}

# TISPAN32.DLL, matcher at VA 0x1C4062F0 (module +0x62F0; found by searching the
# raw file for the matcher's prologue bytes 8b 54 24 04 83 ec 14 53, which is
# unique in the image -- the same signature map_pe() in verify_ruleset.py
# checks for). Disassembled with `objdump -d --start-address=0x1c4062f0
# --stop-address=0x1c406c10 -M intel TISPAN32.DLL`; every VA below was read
# directly out of that disassembly, not inferred from TIENG32's layout.
SPAN = {
    "image_base": 0x1C400000,
    "charclass": 0x1C409AA0,
    "bucket_base": 0x1C40E23C,
    "fallback": 0x1C40E3BC,
    "qmark_sub": 0x1C40E3C0,
    "latin1": {0xC4: 0x1C40E3A8, 0xD6: 0x1C40E3AC, 0xDC: 0x1C40E3B4, 0xDF: 0x1C40E3B0},
}

LANGUAGES = {"eng": ENG, "span": SPAN}
DEFAULT_SYMBOL = {"eng": "sv_lang_data_eng", "span": "sv_lang_data_span"}


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
    ap.add_argument("--symbol", default=None,
                    help="C symbol for the generated sv_ruleset_t; defaults "
                         "to sv_lang_data_eng / sv_lang_data_span depending "
                         "on --language")
    ap.add_argument("--language", default="eng", choices=sorted(LANGUAGES),
                    help="which table layout/addresses to read (see module "
                         "docstring): eng = TIENG32, span = TISPAN32")
    ap.add_argument("--layout", default=None, choices=["eng"],
                    help="deprecated alias for --language eng; kept only "
                         "so an existing eng-only invocation keeps working")
    args = ap.parse_args()
    if args.layout is not None:
        args.language = args.layout
    if args.symbol is None:
        args.symbol = DEFAULT_SYMBOL[args.language]

    pe = PE(args.dll)
    L = LANGUAGES[args.language]
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

    # The four Latin-1 letters the matcher switches on explicitly. Same four
    # codepoints in both languages (0xC4, 0xD6, 0xDC, 0xDF); see the module
    # docstring for why TISPAN32 special-cases this German-alphabet set too.
    for ch, va in L["latin1"].items():
        ptr = pe.u32(va)
        if ptr and looks_like_rules(ptr):
            buckets[ch] = ptr

    fallback = pe.u32(L["fallback"])
    qmark = pe.b[pe.off(L["qmark_sub"])]

    # All buckets point INTO one contiguous rule blob; a bucket is just an
    # offset into it, and matching runs off the end of one letter's rules into
    # the next letter's. So emit the blob once and index it by offset.
    #
    # The blob is not laid out in its own data section in either binary: its
    # VAs land inside .text (checked by mapping them through the section
    # table), i.e. this is MSVC's read-only-constant-in-.text placement. A
    # bare "first zero byte after blob_end" terminator is what the ENGLISH
    # extraction has always used, and it is differentially proven correct
    # over the entire system dictionary plus randomized stress input (see
    # REVERSING.md / tools/verify_ruleset.py) -- so it stays the default for
    # every language, unchanged, rather than being replaced by a heuristic
    # that was only checked against one binary.
    #
    # TISPAN32 specifically: past its fallback bucket's last practically
    # reachable rule ([1-2-3]=...), the file has a short stretch of dead
    # filler and 0xCC (INT3) linker padding before the next function's real
    # code, and the first zero byte after that sits inside that code (an
    # instruction operand byte, not a terminator) -- confirmed by
    # disassembling that far past the bucket. Cutting the blob there instead
    # (a run of 0xCC) was tried and rejected: a 259KB blob the size of
    # TIENG32's can and does contain two coincidentally-adjacent 0xCC bytes
    # well before its real end, which silently truncated ENGLISH's blob from
    # 259393 bytes to 10799 in testing -- catastrophically wrong despite
    # looking like a more principled rule. The few reachable-only-in-theory
    # bytes of raw code TISPAN32's blob may pick up as a result are
    # unreachable by any letter, digit or the rule table's own punctuation
    # entries, all of which are matched earlier in the same bucket; if that
    # is ever wrong, tools/verify_ruleset.py run against the real DLL is what
    # will prove it, not a guess made here.
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
