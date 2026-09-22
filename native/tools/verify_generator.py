#!/usr/bin/env python3
"""Development-only differential test of the reconstructed generator stages.

Same pattern as tools/verify_dsp.py and tools/verify_frames.py, which settled
the sample kernel and the frame renderer: map the real TIENG32.DLL under
Unicorn, run the ORIGINAL x86 code, call the C reconstruction through ctypes,
and compare the output AND the whole state.

Two stages, both leaf functions out of the phoneme -> parameter-frame
generator at TIENG32 0x1C204690 (which as a whole is NOT reconstructed):

  --stage class    VA 1c203010..1c2030aa, phoneme -> manner class
  --stage track    VA 1c203870..1c203a27, one parameter track -> a per-frame
                   int16 contour

"Whole state" for the track stage means every byte of the module's .data
section. The original writes its two scratch contours and its output through
fixed globals, so a reconstruction that got a buffer length wrong, or that
wrote one entry too many, shows up as a .data byte outside the three spans we
claim. The comparison is: snapshot .data, run, diff, and assert the changed
bytes lie exactly in the ramp, the forward contour and the destination — then
compare the contents of all three against ours. An output-only check would
not have caught the ramp's off-by-one at 0x1c20391a (it writes ramp[n-1], not
ramp[n], because the base in the instruction is the ramp base MINUS two).

Requires `pip install unicorn==2.1.4` in a development venv. Nothing here is
linked into or required by the native library or the application.

Build a test library, for example:
  cc -shared -fPIC -std=c11 -Wall -Wextra -Wconversion -Iinclude \
     src/generator.c -o build/libgenerator.dylib
  python tools/verify_generator.py --dll /path/TIENG32.DLL \
         --library build/libgenerator.dylib
"""
import argparse
import ctypes as C
import random
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

IMAGE_BASE = 0x1C200000

# --- original code addresses ------------------------------------------------
CLASS_ENTRY = 0x1C203010
CLASS_EXITS = (0x1C203022, 0x1C203041, 0x1C20304B, 0x1C203055,
               0x1C20306B, 0x1C203084, 0x1C20308D, 0x1C203099, 0x1C2030AA)
TRACK_ENTRY = 0x1C203870
TRACK_EXIT = 0x1C203A27

# --- original data addresses ------------------------------------------------
RATE_VA = 0x1C24C1B0          # 9 rows x 16 int32
RATE_ROWS, RATE_COLS = 9, 16
PHONEME_VA = 0x1C24D720       # module descriptor +0x14
PHONEME_VA_ALT = 0x1C24E250   # module descriptor +0x18
PHONEME_STRIDE = 26
# 0x1C24E250 - 0x1C24D720 is 2864 = 110*26 + 4, so the first table holds 110
# entries and four bytes of alignment padding. Index 109 is ' ' and index 108
# is '~R'; anything past that is the SECOND table read four bytes out of
# phase, which is why a sloppier bound still looks like plausible data.
PHONEME_COUNT = (PHONEME_VA_ALT - PHONEME_VA) // PHONEME_STRIDE   # 110

NFRAMES_VA = 0x1C250566       # uint16, the generator's frame count for the
                              # phoneme being emitted (0x1c204aec writes it)
RAMP_VA = 0x1C250A50          # int16[], the crossfade ramp
FORWARD_VA = 0x1C250150       # int16[], the forward pass
DEST_VA = 0x1C250EF0          # track[0]'s contour buffer, 0x400 bytes
TRACK_VA = 0x1C250690         # track[0]'s 0x40-byte struct
TRACK_SIZE = 0x40

# All three buffers are 0x400 bytes = 512 int16, which is what bounds a
# contour. The per-track contour buffers are 0x400 apart (0x1C250EF0,
# 0x1C2512F0, ...); the forward buffer runs 0x1C250150..0x1C250550, where the
# generator's scalar globals start (0x1C250550, 0x1C250566 = the frame count);
# the ramp runs 0x1C250A50..0x1C250E50, where 0x1C250E50 is the next global.
# Getting this wrong is not academic: poisoning past the forward buffer
# overwrites the track struct and the frame count, and the original then
# faults on a wild `rep stos`.
MAX_FRAMES = 512
RAMP_CAPACITY = MAX_FRAMES
FORWARD_CAPACITY = MAX_FRAMES

# Field offsets inside the 0x40-byte track struct, as FUN_1c203870 reads them.
TRACK_FIELDS = (
    ("fwd_target", 0x08), ("bwd_target", 0x0C),
    ("fwd_start", 0x20), ("bwd_start", 0x24),
    ("fwd_rate_col", 0x2C), ("bwd_rate_col", 0x30),
    ("rate_row", 0x34), ("hold_percent", 0x38), ("fall_percent", 0x3C),
)

STACK = 0x00300000
MAGIC_RET = 0x00400000

SV_GEN_OK = 0


# --------------------------------------------------------------------------
# ctypes mirrors of include/tispeech/generator.h
# --------------------------------------------------------------------------
class GenPhonemes(C.Structure):
    _fields_ = [("entries", C.POINTER(C.c_uint8)), ("count", C.c_size_t)]


class GenTrack(C.Structure):
    _fields_ = [(name, C.c_int32) for name, _ in TRACK_FIELDS]


class GenRates(C.Structure):
    _fields_ = [("table", C.POINTER(C.c_int32)), ("count", C.c_size_t)]


class GenScratch(C.Structure):
    _fields_ = [("ramp", C.POINTER(C.c_int16)),
                ("forward", C.POINTER(C.c_int16)),
                ("capacity", C.c_size_t)]


def load_library(path):
    lib = C.CDLL(str(path))
    lib.sv_gen_phoneme_class.restype = C.c_int
    lib.sv_gen_phoneme_class.argtypes = [C.POINTER(GenPhonemes), C.c_int]
    lib.sv_gen_track_contour.restype = C.c_int
    lib.sv_gen_track_contour.argtypes = [C.POINTER(GenTrack), C.c_int,
                                         C.POINTER(GenRates),
                                         C.POINTER(C.c_int16),
                                         C.POINTER(GenScratch)]
    lib.sv_gen_crossfade_ramp.restype = C.c_int
    lib.sv_gen_crossfade_ramp.argtypes = [C.POINTER(GenTrack), C.c_int,
                                          C.POINTER(C.c_int16), C.c_size_t]
    return lib


# --------------------------------------------------------------------------
# The emulator
# --------------------------------------------------------------------------
class Original:
    """The real TIENG32 code, mapped and run in place."""

    def __init__(self, dll):
        data = Path(dll).read_bytes()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        machine, count = struct.unpack_from("<HH", data, pe + 4)
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        image_base = struct.unpack_from("<I", data, optional + 28)[0]
        image_size = struct.unpack_from("<I", data, optional + 56)[0]
        if machine != 0x14C or image_base != IMAGE_BASE:
            raise ValueError("expected the supplied i386 TIENG32 build")

        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(image_base, (image_size + 4095) & ~4095)
        self.data_va = self.data_size = None
        for i in range(count):
            section = optional + optional_size + 40 * i
            name = data[section:section + 8].rstrip(b"\0").decode()
            rva, raw_size, raw_offset = struct.unpack_from("<III", data,
                                                           section + 12)
            virtual_size = struct.unpack_from("<I", data, section + 8)[0]
            self.uc.mem_write(image_base + rva,
                              data[raw_offset:raw_offset + raw_size])
            if name == ".data":
                self.data_va = image_base + rva
                self.data_size = max(virtual_size, raw_size)
        if self.data_va is None:
            raise ValueError("no .data section")
        self.uc.mem_map(STACK - 0x10000, 0x20000)
        self.uc.mem_map(MAGIC_RET & ~0xFFF, 0x1000)

        # Reject a different build rather than silently running other code.
        self._expect(CLASS_ENTRY, "8b4c240466 8b410c".replace(" ", ""),
                     "manner-class entry")
        self._expect(0x1C203028, "8d1492", "26*code index arithmetic")
        self._expect(TRACK_ENTRY, "83ec085356", "track entry")
        self._expect(0x1C20391A, "66c70445 4e0a251c 0000".replace(" ", ""),
                     "ramp[n-1] store")
        self._expect(0x1C20394E, "8b1c85b0c1241c", "rate-table load")

    def _expect(self, va, hexbytes, what):
        want = bytes.fromhex(hexbytes)
        got = bytes(self.uc.mem_read(va, len(want)))
        if got != want:
            raise ValueError("unsupported TIENG32 %s signature at %08x: %s"
                             % (what, va, got.hex()))

    def read(self, va, n):
        return bytes(self.uc.mem_read(va, n))

    def write(self, va, payload):
        self.uc.mem_write(va, bytes(payload))

    def call(self, entry, exits, args):
        esp = STACK
        for value in reversed(args):
            esp -= 4
            self.uc.mem_write(esp, struct.pack("<I", value & 0xFFFFFFFF))
        esp -= 4
        self.uc.mem_write(esp, struct.pack("<I", MAGIC_RET))
        self.uc.reg_write(UC_X86_REG_ESP, esp)
        self.uc.emu_start(entry, MAGIC_RET, count=4_000_000)
        if self.uc.reg_read(UC_X86_REG_ESP) != esp + 4:
            raise RuntimeError("stack not balanced leaving %08x" % entry)
        return self.uc.reg_read(UC_X86_REG_EAX)

    def snapshot_data(self):
        return self.read(self.data_va, self.data_size)


def spans(before, after, base):
    """Contiguous runs of bytes that changed, as (va, length)."""
    out = []
    start = None
    for i in range(len(before)):
        if before[i] != after[i]:
            if start is None:
                start = i
        elif start is not None:
            out.append((base + start, i - start))
            start = None
    if start is not None:
        out.append((base + start, len(before) - start))
    return out


def merge(runs, gap=8):
    """Join runs separated by less than `gap` untouched bytes.

    A contour that happens to write the same value a slot already held leaves a
    hole; merging keeps the report about regions rather than about luck."""
    merged = []
    for va, length in runs:
        if merged and va - (merged[-1][0] + merged[-1][1]) < gap:
            merged[-1] = (merged[-1][0], va + length - merged[-1][0])
        else:
            merged.append((va, length))
    return merged


def i16(raw, count):
    return list(struct.unpack_from("<%dh" % count, raw, 0))


# --------------------------------------------------------------------------
# Stage: manner class
# --------------------------------------------------------------------------
def stage_class(original, lib, cases, rng, verbose):
    RECORD = 0x00500000
    SYNTH = 0x00501000
    original.uc.mem_map(RECORD, 0x4000)

    failures = 0
    checked = 0
    histogram = {}

    # --- cohort 1: synthetic flags ---------------------------------------
    # The 110 real English entries do not cover the decision tree: several
    # branches are only reachable with bit combinations no English phoneme
    # carries, and the tree's ORDER is only observable when two bits are set
    # at once. So the first cohort is random flag words drawn from the bits
    # the tree actually tests, which makes every branch and every precedence
    # between branches reachable.
    BITS = (0x10000000, 0x00002000, 0x00008000, 0x00000040, 0x00040000,
            0x00000004, 0x20000000, 0x00000100, 0x08000000)
    synth_count = 256
    synth = bytearray(PHONEME_STRIDE * synth_count)
    for i in range(synth_count):
        flags = 0
        for bit in BITS:
            if rng.random() < 0.35:
                flags |= bit
        flags |= rng.getrandbits(32) & 0x00F31A9B   # noise in untested bits
        struct.pack_into("<I", synth, i * PHONEME_STRIDE + 2, flags)
    original.write(SYNTH, synth)
    buf = (C.c_uint8 * len(synth)).from_buffer_copy(bytes(synth))
    table = GenPhonemes(entries=C.cast(buf, C.POINTER(C.c_uint8)),
                        count=synth_count)
    for code in range(synth_count):
        original.write(RECORD + 0x04, struct.pack("<I", SYNTH))
        original.write(RECORD + 0x0C, struct.pack("<H", code))
        want = original.call(CLASS_ENTRY, CLASS_EXITS, [RECORD]) & 0xFFFF
        got = lib.sv_gen_phoneme_class(C.byref(table), code)
        checked += 1
        histogram[want] = histogram.get(want, 0) + 1
        if got != want:
            failures += 1
            if failures <= 10:
                print("  synthetic mismatch code=%3d flags=%08x "
                      "original=%d ours=%d"
                      % (code, struct.unpack_from(
                          "<I", synth, code * PHONEME_STRIDE + 2)[0],
                         want, got))

    # --- cohort 2: the real inventory, both published tables --------------
    for table_va in (PHONEME_VA, PHONEME_VA_ALT):
        raw = original.read(table_va, PHONEME_STRIDE * PHONEME_COUNT)
        buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
        table = GenPhonemes(entries=C.cast(buf, C.POINTER(C.c_uint8)),
                            count=PHONEME_COUNT)

        codes = list(range(PHONEME_COUNT))
        codes += [rng.randrange(0, PHONEME_COUNT) for _ in range(cases)]
        codes += [0xFF] * 4
        for code in codes:
            original.write(RECORD + 0x04, struct.pack("<I", table_va))
            original.write(RECORD + 0x0C, struct.pack("<H", code & 0xFFFF))
            want = original.call(CLASS_ENTRY, CLASS_EXITS, [RECORD]) & 0xFFFF
            got = lib.sv_gen_phoneme_class(C.byref(table), code)
            checked += 1
            histogram[want] = histogram.get(want, 0) + 1
            if got != want:
                failures += 1
                if failures <= 10:
                    print("  class mismatch table=%08x code=%3d "
                          "original=%d ours=%d" % (table_va, code, want, got))

    # The reconstruction must refuse an out-of-range code rather than read on.
    raw = original.read(PHONEME_VA, PHONEME_STRIDE * PHONEME_COUNT)
    buf = (C.c_uint8 * len(raw)).from_buffer_copy(raw)
    table = GenPhonemes(entries=C.cast(buf, C.POINTER(C.c_uint8)),
                        count=PHONEME_COUNT)
    if lib.sv_gen_phoneme_class(C.byref(table), PHONEME_COUNT) >= 0:
        print("  out-of-range code was not refused")
        failures += 1
    if lib.sv_gen_phoneme_class(None, 0x00FF) != 9:
        print("  the 0x00FF terminator must be answered without a table")
        failures += 1

    if verbose:
        print("  classes seen: " + ", ".join(
            "%d:%d" % kv for kv in sorted(histogram.items())))
    return checked, failures


# --------------------------------------------------------------------------
# Stage: one parameter track
# --------------------------------------------------------------------------
def random_track(rng):
    """Field values spanning the domain the generator itself produces.

    The generator's init at 0x1c204bf1 gives every track hold_percent=0x19,
    fall_percent=0x4b, rate_row=3, both rate columns 7; the targets and starts
    come from the phoneme tables and are int16-sized. The cohort covers that
    and a good deal more, including the degenerate spans."""
    pick = rng.random()
    if pick < 0.15:
        hold, fall = 25, 75            # the initialised defaults
    elif pick < 0.30:
        hold, fall = rng.randrange(0, 5), rng.randrange(0, 5)   # span <= 1
    elif pick < 0.40:
        hold, fall = rng.randrange(90, 150), rng.randrange(0, 200)
    else:
        hold, fall = rng.randrange(0, 100), rng.randrange(0, 120)
    return {
        "fwd_target": rng.randrange(-32768, 32768),
        "bwd_target": rng.randrange(-32768, 32768),
        "fwd_start": rng.randrange(-32768, 32768),
        "bwd_start": rng.randrange(-32768, 32768),
        "fwd_rate_col": rng.randrange(0, RATE_COLS),
        "bwd_rate_col": rng.randrange(0, RATE_COLS),
        "rate_row": rng.randrange(-2, 11),      # exercises both clamp arms
        "hold_percent": hold,
        "fall_percent": fall,
    }


def stage_track(original, lib, cases, rng, verbose):
    rate_raw = original.read(RATE_VA, RATE_ROWS * RATE_COLS * 4)
    rate_buf = (C.c_int32 * (RATE_ROWS * RATE_COLS)).from_buffer_copy(rate_raw)
    rates = GenRates(table=C.cast(rate_buf, C.POINTER(C.c_int32)),
                     count=RATE_ROWS * RATE_COLS)

    contour = (C.c_int16 * MAX_FRAMES)()
    ramp = (C.c_int16 * RAMP_CAPACITY)()
    forward = (C.c_int16 * RAMP_CAPACITY)()
    scratch = GenScratch(ramp=C.cast(ramp, C.POINTER(C.c_int16)),
                         forward=C.cast(forward, C.POINTER(C.c_int16)),
                         capacity=RAMP_CAPACITY)

    failures = 0
    checked = 0
    skipped = 0
    audited = 0
    unexpected = {}

    for case in range(cases):
        fields = random_track(rng)
        # Durations 1..31 in eighths of a frame is what the frame allocator
        # feeds, so (d+4)/8 lands in 0..4; wider counts are exercised too
        # because nothing in this function limits them.
        if rng.random() < 0.5:
            n = (rng.randrange(1, 32) + 4) // 8
            n = max(n, 1)
        else:
            n = rng.randrange(1, 120)

        hold = (fields["hold_percent"] * n) // 100 + 1
        if hold <= 0 or hold > RAMP_CAPACITY or n > RAMP_CAPACITY:
            skipped += 1
            continue

        # --- run the original -------------------------------------------
        block = bytearray(TRACK_SIZE)
        struct.pack_into("<I", block, 0x04, DEST_VA)
        for name, off in TRACK_FIELDS:
            struct.pack_into("<i", block, off, fields[name])
        original.write(TRACK_VA, block)
        original.write(NFRAMES_VA, struct.pack("<H", n))
        # Poison the three buffers so a short write is visible as a leftover.
        original.write(RAMP_VA, b"\xA5" * (RAMP_CAPACITY * 2))
        original.write(FORWARD_VA, b"\x5A" * (FORWARD_CAPACITY * 2))
        original.write(DEST_VA, b"\xC3" * (MAX_FRAMES * 2))

        before = original.snapshot_data()
        original.call(TRACK_ENTRY, (TRACK_EXIT,), [TRACK_VA])
        after = original.snapshot_data()

        want_ramp = i16(original.read(RAMP_VA, n * 2), n)
        want_forward = i16(original.read(FORWARD_VA, n * 2), n)
        want_dest = i16(original.read(DEST_VA, n * 2), n)

        # --- whole-.data audit ------------------------------------------
        allowed = ((RAMP_VA, max(n, hold) * 2),
                   (FORWARD_VA, n * 2),
                   (DEST_VA, n * 2))
        for va, length in merge(spans(before, after, original.data_va)):
            ok = any(va >= a and va + length <= a + l for a, l in allowed)
            if not ok:
                key = (va, length)
                unexpected[key] = unexpected.get(key, 0) + 1
                failures += 1
        audited += 1

        # --- run ours ----------------------------------------------------
        track = GenTrack(**fields)
        for k in range(RAMP_CAPACITY):
            ramp[k] = -23131      # 0xA5A5
            forward[k] = 23130    # 0x5A5A
        for k in range(MAX_FRAMES):
            contour[k] = -15421   # 0xC3C3

        status = lib.sv_gen_track_contour(C.byref(track), n, C.byref(rates),
                                          contour, C.byref(scratch))
        checked += 1
        if status != SV_GEN_OK:
            print("  case %d: reconstruction refused (%d) a case the original "
                  "ran: n=%d %r" % (case, status, n, fields))
            failures += 1
            continue

        got_ramp = list(ramp[:n])
        got_forward = list(forward[:n])
        got_dest = list(contour[:n])
        for label, want, got in (("ramp", want_ramp, got_ramp),
                                 ("forward", want_forward, got_forward),
                                 ("contour", want_dest, got_dest)):
            if want != got:
                failures += 1
                if failures <= 10:
                    bad = next(i for i in range(n) if want[i] != got[i])
                    print("  case %d %s[%d]: original=%d ours=%d "
                          "(n=%d %r)" % (case, label, bad, want[bad],
                                         got[bad], n, fields))
                break

        # The standalone ramp entry point must agree with the fused one.
        for k in range(RAMP_CAPACITY):
            ramp[k] = 0
        if lib.sv_gen_crossfade_ramp(C.byref(track), n, ramp,
                                     RAMP_CAPACITY) != SV_GEN_OK \
                or list(ramp[:n]) != want_ramp:
            print("  case %d: sv_gen_crossfade_ramp disagrees" % case)
            failures += 1

    if unexpected:
        print("  .data bytes the original wrote OUTSIDE the modelled spans:")
        for (va, length), count in sorted(unexpected.items()):
            print("    %08x + %d  (%d cases)" % (va, length, count))
    elif verbose:
        print("  .data audit: %d cases, no writes outside ramp/forward/contour"
              % audited)
    if skipped and verbose:
        print("  %d cases skipped (hold outside the original's own buffer)"
              % skipped)
    return checked, failures


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", required=True, help="path to the real TIENG32.DLL")
    ap.add_argument("--library", required=True,
                    help="the built reconstruction (libgenerator.dylib/.so)")
    ap.add_argument("--cases", type=int, default=5000)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x19961118)
    ap.add_argument("--stage", choices=("class", "track", "all"), default="all")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    original = Original(args.dll)
    lib = load_library(args.library)

    stages = []
    if args.stage in ("class", "all"):
        stages.append(("class", stage_class))
    if args.stage in ("track", "all"):
        stages.append(("track", stage_track))

    total_failures = 0
    for name, fn in stages:
        rng = random.Random(args.seed)
        print("=== stage %s ===" % name)
        checked, failures = fn(original, lib, args.cases, rng, args.verbose)
        total_failures += failures
        verdict = "PASS" if failures == 0 else "FAIL"
        print("%s %-6s %d cases, %d mismatches" % (verdict, name, checked,
                                                   failures))

    if total_failures:
        print("\nFAIL: %d mismatches" % total_failures)
        return 1
    print("\nPASS: the reconstruction matches the original TIENG32 code, "
          "output and whole .data state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
