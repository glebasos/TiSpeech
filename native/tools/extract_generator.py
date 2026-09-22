#!/usr/bin/env python3
"""
extract_generator.py — emit a C translation unit holding the generator tables
a language module carries, read out of an original SoftVoice language DLL.

WHY THIS EXISTS
---------------
The interpolation-rate table and the phoneme definition table are DATA, and
they are the property of SoftVoice, Inc. They must never be committed to this
repository, exactly as the DLLs themselves are not. This runs at BUILD TIME
against a DLL the user already owns and writes a generated .c into the build
tree. The DLL is opened as a file and parsed; it is never mapped, relocated or
executed. Same contract as tools/extract_lang.py and tools/extract_base.py.

USAGE
    extract_generator.py TIENG32.DLL out/eng_gen_data.c --language eng
    extract_generator.py TISPAN32.DLL out/span_gen_data.c --language span

WHAT IS EXTRACTED, AND HOW IT IS FOUND
--------------------------------------
Nothing here is a hardcoded address. REVERSING.md's standing advice is to
follow the module descriptor rather than hardcode, because the two language
modules are the same program compiled twice at different addresses; this tool
does that, and additionally anchors the one table the descriptor does NOT
publish on the instruction that loads it.

  phoneme tables   The descriptor's +0x14 and +0x18. The descriptor lives in
                   BSS and is FILLED BY CODE, not present in the file, so the
                   addresses are decoded out of `LoadLanguage` (module +0x1030)
                   by walking its `mov [eax+disp8], imm32` stores. That
                   function has a byte-identical instruction layout in
                   TIENG32 and TISPAN32.

                   Entry stride is 26. The count is taken from the distance
                   between the two tables: 0xB30 = 110*26 + 4 in BOTH modules,
                   i.e. 110 entries and four bytes of alignment padding.

  rate table       Not a descriptor field. Found by searching .text for the
                   ramp memset constant `mov eax, 0x01000100` — one hit per
                   module, inside FUN_1c203870 — and then taking the imm32 of
                   the first `mov ebx, [<table> + eax*4]` after it. 9 rows of
                   16 int32.

                   A cross-check the tool applies: the last row must be all
                   256 and the first column of rows 3..8 must be 256, which is
                   what makes the table an interpolation-rate table rather
                   than whatever else a dword array might be. The English and
                   Spanish tables are byte-identical, consistent with slot
                   +0x08 being language-independent machinery.
"""
import argparse
import struct
import sys

PHONEME_STRIDE = 26
RATE_ROWS, RATE_COLS = 9, 16


class Image:
    """A PE image read as a file. Nothing is mapped or executed."""

    def __init__(self, path):
        self.b = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.b, 0x3C)[0]
        if self.b[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("%s: not a PE image" % path)
        machine, self.nsections = struct.unpack_from("<HH", self.b, pe + 4)
        optional_size = struct.unpack_from("<H", self.b, pe + 20)[0]
        optional = pe + 24
        self.base = struct.unpack_from("<I", self.b, optional + 28)[0]
        if machine != 0x14C:
            raise SystemExit("%s: not an i386 image" % path)
        self.sections = []
        for i in range(self.nsections):
            s = optional + optional_size + 40 * i
            name = self.b[s:s + 8].rstrip(b"\0").decode("latin1")
            vsize = struct.unpack_from("<I", self.b, s + 8)[0]
            rva, rawsize, rawoff = struct.unpack_from("<III", self.b, s + 12)
            self.sections.append((name, self.base + rva,
                                  max(vsize, rawsize), rawsize, rawoff))

    def read(self, va, n):
        for name, start, vsize, rawsize, rawoff in self.sections:
            if start <= va < start + vsize:
                off = va - start
                have = max(0, min(n, rawsize - off))
                data = self.b[rawoff + off:rawoff + off + have]
                # Anything past the raw data is BSS: zero in the file.
                return data + b"\0" * (n - len(data))
        raise SystemExit("VA %08x is not in any section" % va)

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise SystemExit("no %s section" % name)

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]


def descriptor_fields(img):
    """Decode LoadLanguage's stores into {descriptor offset: value}.

    LoadLanguage is at module +0x1030 and is a straight run of
       a1 <slot>                  mov eax, [slot]
       c7 40 <disp8> <imm32>      mov [eax+disp8], imm32
       c7 00 <imm32>              mov [eax], imm32
    in both modules. Decoding it is what makes this tool address-free."""
    va = img.base + 0x1030
    end = va + 0xA0
    fields = {}
    while va < end:
        op = img.read(va, 2)
        if op[0] == 0xA1:                      # mov eax, [imm32]
            va += 5
        elif op == b"\xC7\x40":                # mov [eax+disp8], imm32
            disp = img.read(va + 2, 1)[0]
            fields[disp] = img.u32(va + 3)
            va += 7
        elif op == b"\xC7\x00":                # mov [eax], imm32
            fields[0] = img.u32(va + 2)
            va += 6
        elif op[0] == 0x33:                    # xor reg,reg
            va += 2
        elif op[0] == 0xC3:
            break
        else:
            va += 1
    for need in (0x14, 0x18):
        if need not in fields:
            raise SystemExit("LoadLanguage did not publish +0x%02x" % need)
    return fields


def find_rate_table(img):
    name, start, vsize, rawsize, rawoff = img.section(".text")
    raw = img.b[rawoff:rawoff + rawsize]
    anchor = raw.find(bytes.fromhex("b800010001"))      # mov eax, 0x01000100
    if anchor < 0:
        raise SystemExit("ramp constant 0x01000100 not found in .text; this "
                         "module does not look like it contains FUN_1c203870")
    if raw.find(bytes.fromhex("b800010001"), anchor + 1) >= 0:
        raise SystemExit("ramp constant is not unique in .text")
    load = raw.find(bytes.fromhex("8b1c85"), anchor, anchor + 0x100)
    if load < 0:
        raise SystemExit("no `mov ebx,[table + eax*4]` after the ramp constant")
    return struct.unpack_from("<I", raw, load + 3)[0], start + load


def c_bytes(name, blob):
    out = ["static const unsigned char %s[%d] = {" % (name, len(blob))]
    for i in range(0, len(blob), 12):
        out.append("    " + "".join("0x%02x, " % b for b in blob[i:i + 12]).rstrip())
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dll")
    ap.add_argument("out")
    ap.add_argument("--language", default="eng", choices=("eng", "span"))
    ap.add_argument("--symbol", default=None,
                    help="base symbol name; defaults from --language")
    args = ap.parse_args()
    stem = args.symbol or ("sv_gen_data_" + args.language)

    img = Image(args.dll)
    fields = descriptor_fields(img)
    lang_id = fields.get(0x10)

    # --- phoneme tables --------------------------------------------------
    t14, t18 = fields[0x14], fields[0x18]
    gap = t18 - t14
    count = gap // PHONEME_STRIDE
    padding = gap % PHONEME_STRIDE
    if count <= 0 or count > 4096:
        raise SystemExit("implausible phoneme count %d from a %d-byte gap"
                         % (count, gap))
    tables = [img.read(va, count * PHONEME_STRIDE) for va in (t14, t18)]

    # A table of phoneme definitions has 2-char names; the punctuation entries
    # at the front are the easiest thing to insist on, and they are the same in
    # both languages because this part of the module is shared.
    names = {bytes(reversed(tables[0][i * PHONEME_STRIDE:
                                     i * PHONEME_STRIDE + 2])).decode("latin1")
             for i in range(min(count, 8))}
    if not {".\0", "?\0", ",\0"} & names:
        print("warning: %s+0x14 does not look like a phoneme table (%r)"
              % (args.dll, sorted(names)), file=sys.stderr)

    # --- rate table ------------------------------------------------------
    rate_va, load_va = find_rate_table(img)
    n = RATE_ROWS * RATE_COLS
    rates = list(struct.unpack("<%di" % n, img.read(rate_va, n * 4)))
    last = rates[(RATE_ROWS - 1) * RATE_COLS:]
    if any(v != 256 for v in last):
        raise SystemExit("rate table's last row is not all 256 (%r) — the "
                         "anchor found something else" % last[:4])
    for row in range(3, RATE_ROWS):
        if rates[row * RATE_COLS] != 256:
            raise SystemExit("rate table row %d does not start at 256" % row)

    # --- emit ------------------------------------------------------------
    w = []
    w.append("/* GENERATED by tools/extract_generator.py — do not edit, do not commit. */")
    w.append("/* source: %s */" % args.dll)
    w.append('#include "tispeech/generator.h"')
    w.append("")
    w.append("/* TIENG32/TISPAN32 rate table, found via the ramp constant at")
    w.append(" * %08x and the table load at %08x -> %08x. */"
             % (img.base + 0x1000, load_va, rate_va))
    w.append("static const int %s_rate_values[%d] = {" % (stem, n))
    for row in range(RATE_ROWS):
        w.append("    " + ", ".join("%d" % v for v in
                                    rates[row * RATE_COLS:(row + 1) * RATE_COLS]) + ",")
    w.append("};")
    w.append("")
    w.append("const sv_gen_rates %s_rates = { %s_rate_values, %d };"
             % (stem, stem, n))
    w.append("")
    for i, (va, blob) in enumerate(zip((t14, t18), tables)):
        w.append("/* module descriptor +0x%02x -> %08x, %d entries of %d bytes */"
                 % (0x14 + 4 * i, va, count, PHONEME_STRIDE))
        w.append(c_bytes("%s_phonemes_%d" % (stem, i), blob))
        w.append("")
    w.append("const sv_gen_phonemes %s_phonemes[2] = {" % stem)
    for i in range(2):
        w.append("    { %s_phonemes_%d, %d }," % (stem, i, count))
    w.append("};")

    with open(args.out, "w") as f:
        f.write("\n".join(w) + "\n")

    print("%s: language id %s, %d phoneme entries x2 (%d bytes of padding "
          "between the tables), %dx%d rate table at %08x"
          % (args.out, lang_id, count, padding, RATE_ROWS, RATE_COLS, rate_va))


if __name__ == "__main__":
    main()
