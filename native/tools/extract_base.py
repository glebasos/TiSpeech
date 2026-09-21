#!/usr/bin/env python3
"""
extract_base.py — emit a C translation unit holding TIBASE32's static synthesis
tables, read out of an original SoftVoice TIBASE32.DLL.

WHY THIS EXISTS
---------------
The resonator coefficients, the amplitude curve, the glottal waveforms, the
noise table and the output curve are DATA. They are the property of SoftVoice,
Inc. and must never be committed to this repository, exactly as the native DLLs
themselves are not committed.

So this runs at BUILD TIME against a DLL the user already owns and writes a
generated .c into the build tree. Nothing proprietary enters git. The DLL is
opened as a file and parsed; it is never mapped, relocated or executed, and no
emulator is involved — that is tools/verify_frames.py, which is development
only.

USAGE
    extract_base.py TIBASE32.DLL out/base_data.c [--symbol sv_base_tables]

The generated unit defines

    const sv_frame_tables   <symbol>;          (default sv_base_tables)
    const sv_phoneme_names  <symbol>_phonemes;

TABLE ADDRESSES
---------------
Recovered by disassembly of TIBASE32!FUN_1c00402c and FUN_1c004a10; see
native/REVERSING.md and include/tispeech/frames.h, which cite the instruction
behind each one.

  resonator     VA 0x1C002570  0x1180 bytes, six contiguous coefficient tables
  amplitude     VA 0x1C001308  0x102 bytes (80 uint16 levels plus the padding a
                               byte-offset index can reach; see frames.h)
  glottal       VA 0x1C013BD0  12 pointers, 1024 int16 each
  noise         VA 0x1C001570  2048 int16
  output_curve  VA 0x1C0010FE  401 bytes
  phoneme names VA 0x1C012A68  60 list pointers, ' '..'[', entry 0x3b is NULL
  frame rate    VA 0x1C0120E8  two uint16 scalars, 150 and 60, checked against
                               the constants inlined in src/frames.c

Every one of those is verified for shape before it is written out, so pointing
this at a different build fails loudly instead of producing a plausible-looking
table of the wrong thing.
"""

import argparse
import struct
import sys

IMAGE_BASE = 0x1C000000

RESONATOR = (0x1C002570, 0x1180)
AMPLITUDE = (0x1C001308, 0x102)
NOISE = (0x1C001570, 2048)          # int16 count
OUTPUT_CURVE = (0x1C0010FE, 401)
GLOTTAL_PTRS = 0x1C013BD0
GLOTTAL_COUNT = 12
GLOTTAL_SAMPLES = 1024
PHONEME_PTRS = 0x1C012A68
PHONEME_LISTS = 0x3C
FRAME_RATE = 0x1C0120E8

# src/frames.c inlines these two as SV_FRAME_RATE_NUMERATOR/DENOMINATOR.
EXPECTED_RATE = (150, 60)

# The first six bytes of the verified sample loop, VA 0x1C004120:
#   mov edx, dword ptr [ebx + 0x2fc]
# Same guard tools/verify_dsp.py uses, for the same reason.
LOOP_VA = 0x1C004120
LOOP_SIGNATURE = bytes.fromhex("8b93fc020000")

# The unscaled amplitude read at VA 0x1C004857, which is the single most
# surprising thing in this stage and the one a "cleaned up" port loses:
#   movzx edi, byte ptr [esi + 0x10]
#   mov   ax,  word ptr [edi + 0x1c001308]
AMPLITUDE_READ_VA = 0x1C004857
AMPLITUDE_READ = bytes.fromhex("0fb67e10668b870813001c")


class PE:
    """Minimal read-only PE section mapper. Does not load or relocate."""

    def __init__(self, path):
        self.b = open(path, "rb").read()
        if self.b[:2] != b"MZ":
            raise SystemExit(f"{path}: not a PE image")
        pe = struct.unpack_from("<I", self.b, 0x3C)[0]
        if self.b[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path}: bad PE signature")
        machine = struct.unpack_from("<H", self.b, pe + 4)[0]
        nsec = struct.unpack_from("<H", self.b, pe + 6)[0]
        opt = struct.unpack_from("<H", self.b, pe + 20)[0]
        head = pe + 24
        self.base = struct.unpack_from("<I", self.b, head + 28)[0]
        size = struct.unpack_from("<I", self.b, head + 56)[0]
        if machine != 0x14C or self.base != IMAGE_BASE:
            raise SystemExit(
                f"{path}: expected the i386 TIBASE32 build at {IMAGE_BASE:#x}, "
                f"got machine {machine:#x} base {self.base:#x}")
        img = bytearray(size)
        for i in range(nsec):
            sec = head + opt + 40 * i
            rva, raw_size, raw_off = struct.unpack_from("<III", self.b, sec + 12)
            img[rva:rva + raw_size] = self.b[raw_off:raw_off + raw_size]
        self.img = bytes(img)

    def read(self, va, n):
        off = va - self.base
        if off < 0 or off + n > len(self.img):
            raise SystemExit(f"VA {va:#x}+{n:#x} is outside the image")
        return self.img[off:off + n]

    def u32(self, va):
        return struct.unpack_from("<I", self.read(va, 4))[0]

    def u16(self, va):
        return struct.unpack_from("<H", self.read(va, 2))[0]


def check_build(pe):
    """Refuse a DLL that is not the build these addresses were read from."""
    if pe.read(LOOP_VA, len(LOOP_SIGNATURE)) != LOOP_SIGNATURE:
        raise SystemExit("unsupported TIBASE32 build: sample-loop signature "
                         f"at {LOOP_VA:#x} does not match")
    if pe.read(AMPLITUDE_READ_VA, len(AMPLITUDE_READ)) != AMPLITUDE_READ:
        raise SystemExit("unsupported TIBASE32 build: the amplitude lookup at "
                         f"{AMPLITUDE_READ_VA:#x} is not the expected "
                         "byte-offset form; src/frames.c would read the wrong "
                         "levels")
    rate = (pe.u16(FRAME_RATE), pe.u16(FRAME_RATE + 2))
    if rate != EXPECTED_RATE:
        raise SystemExit(f"frame-rate constants at {FRAME_RATE:#x} are {rate}, "
                         f"but src/frames.c inlines {EXPECTED_RATE}")


def glottal_tables(pe):
    """The 12 waveform pointers, checked for the stride the data actually has.

    Entry 12 of the pointer table is not a pointer: it is the first dword of an
    ASCII string ("Error in index out..."), which is how the count of 12 was
    established rather than assumed.
    """
    out = []
    for i in range(GLOTTAL_COUNT):
        va = pe.u32(GLOTTAL_PTRS + 4 * i)
        if not (IMAGE_BASE <= va < IMAGE_BASE + len(pe.img)):
            raise SystemExit(f"glottal pointer {i} is {va:#x}, not in the image")
        out.append(struct.unpack(f"<{GLOTTAL_SAMPLES}h",
                                 pe.read(va, GLOTTAL_SAMPLES * 2)))
    sentinel = pe.u32(GLOTTAL_PTRS + 4 * GLOTTAL_COUNT)
    if IMAGE_BASE <= sentinel < IMAGE_BASE + len(pe.img):
        raise SystemExit(f"glottal pointer {GLOTTAL_COUNT} looks like a real "
                         f"pointer ({sentinel:#x}); the table may be longer "
                         "than the 12 entries frames.h assumes")
    return out


def phoneme_lists(pe):
    """The (second character, code) pair lists, one per first character.

    A list runs until a pair whose second character is zero, which carries the
    code for the one-character name and is what the matcher at 0x1c004ce4 stops
    on. Entry 0x3b ('[') is a null pointer in this build; the original
    dereferences it, so it is emitted as NULL and src/frames.c refuses it.
    """
    lists = []
    for i in range(PHONEME_LISTS):
        va = pe.u32(PHONEME_PTRS + 4 * i)
        if va == 0:
            lists.append(None)
            continue
        if not (IMAGE_BASE <= va < IMAGE_BASE + len(pe.img)):
            raise SystemExit(f"phoneme list {i} points at {va:#x}")
        pairs = bytearray()
        for step in range(64):
            pair = pe.read(va + 2 * step, 2)
            pairs += pair
            if pair[0] == 0:
                break
        else:
            raise SystemExit(f"phoneme list {i} at {va:#x} is unterminated")
        lists.append(bytes(pairs))
    if lists[0x3b] is not None:
        raise SystemExit("phoneme list 0x3b is not NULL in this build; "
                         "src/frames.c documents it as the one the original "
                         "dereferences")
    return lists


def emit_bytes(out, name, data, per_line=16):
    out.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), per_line):
        out.write("    " + "".join(f"0x{b:02x}," for b in data[i:i + per_line])
                  + "\n")
    out.write("};\n\n")


def emit_int16(out, name, values, per_line=12):
    out.write(f"static const int16_t {name}[{len(values)}] = {{\n")
    for i in range(0, len(values), per_line):
        out.write("    " + " ".join(f"{v}," for v in values[i:i + per_line])
                  + "\n")
    out.write("};\n\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dll")
    ap.add_argument("out")
    ap.add_argument("--symbol", default="sv_base_tables")
    args = ap.parse_args()

    pe = PE(args.dll)
    check_build(pe)

    resonator = pe.read(*RESONATOR)
    amplitude = pe.read(*AMPLITUDE)
    curve = pe.read(*OUTPUT_CURVE)
    noise = struct.unpack(f"<{NOISE[1]}h", pe.read(NOISE[0], NOISE[1] * 2))
    glottal = glottal_tables(pe)
    names = phoneme_lists(pe)

    with open(args.out, "w") as out:
        out.write(f"""\
/* GENERATED by tools/extract_base.py from an original TIBASE32.DLL.
 *
 * DO NOT COMMIT THIS FILE. It contains data owned by SoftVoice, Inc.,
 * extracted at build time from a copy the user already has. It is written into
 * the build tree and nowhere else. See native/REVERSING.md.
 */
#include "tispeech/frames.h"

""")
        emit_bytes(out, "resonator", resonator)
        emit_bytes(out, "amplitude", amplitude)
        emit_bytes(out, "output_curve", curve)
        emit_int16(out, "noise", noise)
        for i, wave in enumerate(glottal):
            emit_int16(out, f"glottal_{i}", wave)
        out.write(f"static const int16_t *const glottal[{len(glottal)}] = {{\n")
        for i in range(len(glottal)):
            out.write(f"    glottal_{i},\n")
        out.write("};\n\n")

        out.write(f"const sv_frame_tables {args.symbol} = {{\n"
                  "    resonator, amplitude, glottal, noise, output_curve\n"
                  "};\n\n")

        for i, pairs in enumerate(names):
            if pairs is not None:
                emit_bytes(out, f"phoneme_{i:02x}", pairs)
        out.write(f"const sv_phoneme_names {args.symbol}_phonemes = {{{{\n")
        for i, pairs in enumerate(names):
            out.write(f"    {'0' if pairs is None else f'phoneme_{i:02x}'},\n")
        out.write("}};\n")

    sys.stderr.write(
        f"extract_base.py: {args.out}: {len(resonator)} coefficient bytes, "
        f"{len(amplitude)} amplitude bytes, {len(glottal)} glottal waveforms, "
        f"{len(noise)} noise samples, "
        f"{sum(1 for n in names if n)} phoneme lists\n")


if __name__ == "__main__":
    main()
