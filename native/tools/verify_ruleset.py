#!/usr/bin/env python3
"""Development-only differential test of the reconstructed letter-to-sound matcher.

Runs the ORIGINAL TIENG32!FUN_1c206860 under Unicorn against the C
reconstruction (sv_rules_apply_ex) loaded through ctypes, word by word, and
reports every disagreement. Requires `pip install unicorn` in a development
venv; nothing here is linked into or required by the native library.

This is the tool REVERSING.md open question #1 asks for. It settles what the
13 hand-written tests cannot: those quote expectations out of the rule table,
so they prove the DATA. This proves the MATCHER.

WHAT IS EMULATED
----------------
Only FUN_1c206860 itself, at its real ImageBase (0x1C200000), reading the real
static tables straight out of the mapped image. The DLL is NOT initialised:
DllMain is not run, because the matcher reaches every table it needs through
absolute addresses (0x1C209C20 charclass, 0x1C24C744 bucket table,
0x1C24C8B0..BC Latin-1 buckets, 0x1C24C8C4 fallback, 0x1C24C8C8 '?' filler)
and never dereferences the language descriptor DllMain publishes.

Four MSVCRT40 imports are called from the function and are stubbed:
  0x1C2560AC _isctype          (unreachable while MB_CUR_MAX == 1; asserted)
  0x1C2560B0 __p___mb_cur_max  -> &1
  0x1C2560BC __p__pctype       -> &(C-locale ctype table)
  0x1C2560C0 strncat           (yes, strncat, not memcpy -- see below)

The C-locale _pctype table is synthesised with _SPACE (0x08) on exactly
0x09..0x0D and 0x20, which is what a 1996 MSVCRT gives a process that never
calls setlocale().

CALLING CONVENTION (recovered at 0x1C206860..0x1C206FFC)
--------------------------------------------------------
__cdecl, one argument: a pointer to a 0x20-byte context block.

  +0x00 int      running count of input characters consumed
  +0x04 char *   output cursor           (in/out)
  +0x08 char *   input cursor            (in/out)
  +0x14 int      remaining output bytes  (in/out, decremented by each emit)
  +0x18 unsigned option bits; only 0x40 is tested (suppress '?' substitution)
  +0x1C unsigned SV_RF_* flags of the last rule that fired (out)

Returns 0 normally, or 1 when an emit would not fit in +0x14, in which case
+0x1C is zeroed and +0x00/+0x04/+0x08 are left untouched.

The output is appended with strncat(ctx->out, rule_output, n), after which the
caller walks ctx->out forward over non-NUL bytes. That only lands on the right
byte because the output buffer is zero-filled, so the harness zero-fills it for
both sides -- that is a real precondition of the original, not a convenience.

USAGE
    cc -shared -fPIC -std=c11 -Iinclude src/ruleset.c build/eng_lang_data.c \
       -o build/librules.dylib
    python tools/verify_ruleset.py --dll /path/TIENG32.DLL \
       --library build/librules.dylib --words /usr/share/dict/words
"""
import argparse
import ctypes as C
import random
import re
import struct
import sys
from pathlib import Path

import pefile
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UcError,
                     UC_PROT_ALL)
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX

# Matcher entry point, C symbol and expected prologue per language, so this
# one harness drives either DLL. TIENG32's matcher and TISPAN32's matcher are
# separately-compiled copies of the same source at different VAs -- see
# native/REVERSING.md, "the language modules are one code base" -- so they
# share the same calling convention, context layout and prologue bytes; only
# the address and the C symbol our extraction generates differ.
#
# TISPAN32's matcher (0x1C4062F0) was located by searching the raw file for
# this exact prologue, which is unique in the image (the same check map_pe()
# below performs, just done ahead of time here). It was then disassembled
# with `objdump -d --start-address=0x1c4062f0 --stop-address=0x1c406c10 -M
# intel TISPAN32.DLL` and confirmed to be the same word-boundary / vowel /
# consonant / suffix dispatch as TIENG32's, not merely a byte-signature
# coincidence.
_PROLOGUE = "8b542404" "83ec14" "53"
LANGUAGES = {
    "eng": {"image_base": 0x1C200000, "fun_addr": 0x1C206860,
            "symbol": "sv_lang_data_eng", "prologue": _PROLOGUE},
    "span": {"image_base": 0x1C400000, "fun_addr": 0x1C4062F0,
             "symbol": "sv_lang_data_span", "prologue": _PROLOGUE},
}

# Scratch pages, all clear of the image (0x1C200000 + ~0x59000).
STUBS = 0x30000000          # import stubs + the tables they hand back
WORK = 0x40000000           # input text, output buffer, context block
STACK = 0x50000000
STACK_SIZE = 0x10000
RETURN_MAGIC = 0x7FFF0000   # sentinel return address: emulation stops here

S_ISCTYPE = STUBS + 0x00
S_MB_CUR_MAX = STUBS + 0x10
S_PCTYPE = STUBS + 0x20
S_STRNCAT = STUBS + 0x30
V_MB_CUR_MAX = STUBS + 0x100    # dword == 1
V_PCTYPE_PTR = STUBS + 0x104    # dword == V_CTYPE_TABLE
V_CTYPE_TABLE = STUBS + 0x200   # 256 x uint16 C-locale ctype flags

CTX = WORK + 0x000
OUT = WORK + 0x100
OUT_SIZE = 0x800
TEXT = WORK + 0x1000        # padded input arena
TEXT_PAD = 0x40             # NUL padding on each side of the framed word

MSVCRT_SPACE = 0x0008

# SV_RF_* from include/tispeech/ruleset.h.
RF_BACKSLASH, RF_BACKTICK, RF_INITIAL, RF_SENTENCE = 1, 2, 4, 8


def map_pe(uc, path, image_base, fun_addr, prologue_hex):
    """Map the image at its preferred base. No relocation, no imports, no init."""
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    file_image_base = struct.unpack_from("<I", data, optional + 28)[0]
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    if machine != 0x14C or file_image_base != image_base:
        raise ValueError(f"{path}: expected an i386 build with image base "
                          f"{image_base:#x}, got machine {machine:#x} base "
                          f"{file_image_base:#x} -- wrong --language?")
    uc.mem_map(image_base, (image_size + 0xFFFF) & ~0xFFFF)
    for i in range(count):
        section = optional + optional_size + 40 * i
        rva, raw_size, raw_offset = struct.unpack_from("<III", data, section + 12)
        uc.mem_write(image_base + rva, data[raw_offset:raw_offset + raw_size])
    # Refuse a different build rather than silently running another function.
    prologue = bytes(uc.mem_read(fun_addr, 8))
    if prologue != bytes.fromhex(prologue_hex):
        raise ValueError(f"{path}: unsupported matcher signature at "
                          f"{fun_addr:#x}: {prologue.hex()}")


def find_iat_slot(path, dll_name, func_name):
    """
    Find one import's IAT slot VA by name.

    TIENG32 and TISPAN32 import the same MSVCRT40.dll functions (confirmed:
    both list toupper, strrchr, strcspn, strncmp, strchr, _isctype,
    __p___mb_cur_max, _initterm, _adjust_fdiv, __p__pctype, strncat, malloc,
    strspn, free, in that order) but at different absolute addresses, since
    each DLL's import table is its own. Looking each one up by name, per DLL,
    is what makes this harness reusable instead of hardcoding TIENG32's own
    IAT addresses and hoping a second language DLL happens to match.
    """
    pe = pefile.PE(path)
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        if entry.dll.decode("ascii").rstrip("\0").lower() != dll_name.lower():
            continue
        for imp in entry.imports:
            if imp.name and imp.name.decode("ascii") == func_name:
                return imp.address
    raise ValueError(f"{path}: import {dll_name}!{func_name} not found")


class Original:
    """The letter-to-sound matcher under Unicorn, driven one word at a time."""

    def __init__(self, dll, image_base=LANGUAGES["eng"]["image_base"],
                fun_addr=LANGUAGES["eng"]["fun_addr"],
                prologue_hex=LANGUAGES["eng"]["prologue"]):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        map_pe(uc, dll, image_base, fun_addr, prologue_hex)
        self.fun_addr = fun_addr
        uc.mem_map(STUBS, 0x10000, UC_PROT_ALL)
        uc.mem_map(WORK, 0x10000, UC_PROT_ALL)
        uc.mem_map(STACK, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(RETURN_MAGIC & ~0xFFF, 0x1000, UC_PROT_ALL)

        # Real x86 for the two cheap, hot stubs: `mov eax, imm32; ret`.
        uc.mem_write(S_MB_CUR_MAX, b"\xb8" + struct.pack("<I", V_MB_CUR_MAX) + b"\xc3")
        uc.mem_write(S_PCTYPE, b"\xb8" + struct.pack("<I", V_PCTYPE_PTR) + b"\xc3")
        uc.mem_write(S_ISCTYPE, b"\xc3")
        uc.mem_write(S_STRNCAT, b"\xc3")
        uc.mem_write(V_MB_CUR_MAX, struct.pack("<I", 1))
        uc.mem_write(V_PCTYPE_PTR, struct.pack("<I", V_CTYPE_TABLE))
        table = bytearray(512)
        for ch in (0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20):
            struct.pack_into("<H", table, ch * 2, MSVCRT_SPACE)
        uc.mem_write(V_CTYPE_TABLE, bytes(table))

        iat_isctype = find_iat_slot(dll, "MSVCRT40.dll", "_isctype")
        iat_mb_cur_max = find_iat_slot(dll, "MSVCRT40.dll", "__p___mb_cur_max")
        iat_pctype = find_iat_slot(dll, "MSVCRT40.dll", "__p__pctype")
        iat_strncat = find_iat_slot(dll, "MSVCRT40.dll", "strncat")
        for slot, stub in ((iat_isctype, S_ISCTYPE), (iat_mb_cur_max, S_MB_CUR_MAX),
                           (iat_pctype, S_PCTYPE), (iat_strncat, S_STRNCAT)):
            uc.mem_write(slot, struct.pack("<I", stub))

        self.strncat_calls = 0
        uc.hook_add(UC_HOOK_CODE, self._hook, begin=STUBS, end=STUBS + 0xFF)

    def _hook(self, uc, address, size, user_data):
        if address == S_STRNCAT:
            esp = uc.reg_read(UC_X86_REG_ESP)
            ret, dst, src, n = struct.unpack("<IIII", uc.mem_read(esp, 16))
            end = dst
            while uc.mem_read(end, 1)[0] != 0:
                end += 1
            copied = bytearray()
            for i in range(n):
                b = uc.mem_read(src + i, 1)[0]
                if b == 0:
                    break
                copied.append(b)
            uc.mem_write(end, bytes(copied) + b"\0")
            uc.reg_write(UC_X86_REG_EAX, dst)
            uc.reg_write(UC_X86_REG_ESP, esp + 4)   # __cdecl: caller pops args
            uc.reg_write(UC_X86_REG_EIP, ret)
            self.strncat_calls += 1
        elif address == S_ISCTYPE:
            # Only reachable if MB_CUR_MAX > 1, which the harness pins to 1.
            raise AssertionError("_isctype called: locale model is wrong")

    def run(self, framed, offset, opts=0, out_size=OUT_SIZE):
        """framed: exact bytes of the input arena. offset: cursor within it."""
        uc = self.uc
        uc.mem_write(TEXT, b"\0" * 0x2000)
        uc.mem_write(TEXT, framed)
        uc.mem_write(OUT, b"\0" * (OUT_SIZE + 0x10))
        # +0x00 count, +0x04 out, +0x08 in, +0x0c, +0x10, +0x14 cap,
        # +0x18 opts, +0x1c flags
        uc.mem_write(CTX, struct.pack("<8I", 0, OUT, TEXT + offset, 0, 0,
                                      out_size - 1, opts, 0))
        esp = STACK + STACK_SIZE - 0x400
        uc.mem_write(esp, struct.pack("<II", RETURN_MAGIC, CTX))
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.emu_start(self.fun_addr, RETURN_MAGIC, count=40_000_000)
        rc = uc.reg_read(UC_X86_REG_EAX)
        count, outp, inp, _, _, cap, _, flags = struct.unpack(
            "<8I", uc.mem_read(CTX, 32))
        raw = bytes(uc.mem_read(OUT, OUT_SIZE))
        text = raw.split(b"\0", 1)[0]
        return {
            "rc": rc, "out": text, "flags": flags,
            "consumed": inp - (TEXT + offset), "cap_left": cap,
            "out_cursor": outp - OUT, "count": count,
        }


class Reconstruction:
    def __init__(self, library, symbol=LANGUAGES["eng"]["symbol"]):
        self.lib = lib = C.CDLL(str(Path(library).resolve()))
        lib.sv_rules_apply_ex.restype = C.c_int
        lib.sv_rules_apply_ex.argtypes = [
            C.c_void_p, C.c_char_p, C.c_char_p, C.c_size_t, C.c_uint,
            C.POINTER(C.c_uint), C.POINTER(C.c_size_t)]
        self.rules = C.c_void_p.in_dll(lib, symbol)
        self.rules_ptr = C.addressof(self.rules)

    def run(self, framed, offset, opts=0, out_size=OUT_SIZE):
        arena = C.create_string_buffer(framed, len(framed) + 0x20)
        out = C.create_string_buffer(out_size)
        flags = C.c_uint(0xDEADBEEF)
        consumed = C.c_size_t(0)
        rc = self.lib.sv_rules_apply_ex(
            self.rules_ptr, C.cast(C.byref(arena, offset), C.c_char_p),
            out, out_size, opts, C.byref(flags), C.byref(consumed))
        return {"rc": rc, "out": out.value, "flags": flags.value,
                "consumed": consumed.value}


def frame(word):
    """Build the input arena both sides see, byte for byte.

    A word must sit between spaces (a great many rules anchor on a word
    boundary) and the left context can walk off the front of it, so the arena is
    NUL-padded on both sides. Both engines read the identical bytes; nothing
    here reads uninitialised memory.
    """
    body = b" " + word + b" "
    return b"\0" * TEXT_PAD + body + b"\0" * TEXT_PAD, TEXT_PAD + 1


def classify(word, got, want):
    """Bucket a disagreement by its root cause, not by its text."""
    if got["rc"] != want["rc"]:
        return "return-code"
    if got["out"] != want["out"]:
        if want["out"].startswith(got["out"]):
            return "output: ours stops early (original emits more)"
        if got["out"].startswith(want["out"]):
            return "output: ours overruns (original stops earlier)"
        return "output: divergent text"
    if got["consumed"] != want["consumed"]:
        return "cursor: consumed input length differs"
    if got["flags"] != want["flags"]:
        return f"flags: ours {got['flags']:#x} original {want['flags']:#x}"
    return "unknown"


def load_words(path, limit, seed, pattern):
    words = []
    rx = re.compile(pattern)
    with open(path, "r", encoding="latin-1") as fh:
        for line in fh:
            w = line.strip().upper()
            if w and rx.fullmatch(w):
                words.append(w.encode("latin-1"))
    total = len(words)
    if limit and limit < total:
        words = random.Random(seed).sample(words, limit)
    return words, total


def random_words(count, seed):
    """Stress cohort: exercises the fallback bucket, metacharacters and
    Latin-1 dispatch, which a plain dictionary never reaches."""
    alphabet = (b"ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 3 + b"0123456789"
                + b"'-.?,!:;#%&+<>@^" + bytes([0xC4, 0xD6, 0xDC, 0xDF,
                                               0xC1, 0xC9, 0xCD, 0xD1,
                                               0xD3, 0xDA, 0xE1, 0xFC]))
    rng = random.Random(seed)
    return [bytes(rng.choice(alphabet) for _ in range(rng.randint(1, 12)))
            for _ in range(count)]


def compare(original, ours, cases, label, verbose, stop_after):
    buckets = {}
    examples = {}
    checked = 0
    errors = 0
    for word in cases:
        framed, offset = frame(word)
        try:
            want = original.run(framed, offset)
        except UcError as exc:
            kind = f"original faulted in emulation: {exc}"
            buckets[kind] = buckets.get(kind, 0) + 1
            examples.setdefault(kind, []).append((word, None, None))
            errors += 1
            continue
        got = ours.run(framed, offset)
        checked += 1
        same = (got["rc"] == want["rc"] and got["out"] == want["out"]
                and got["consumed"] == want["consumed"]
                and got["flags"] == want["flags"])
        if same:
            if verbose:
                print(f"ok   {word.decode('latin-1'):<16} -> "
                      f"{want['out'].decode('latin-1')}")
            continue
        kind = classify(word, got, want)
        buckets[kind] = buckets.get(kind, 0) + 1
        if len(examples.setdefault(kind, [])) < 5:
            examples[kind].append((word, got, want))
        if stop_after and sum(buckets.values()) >= stop_after:
            break
    mismatches = sum(buckets.values())
    print(f"\n=== {label} ===")
    print(f"compared      : {checked}")
    print(f"mismatches    : {mismatches}")
    rate = (mismatches / checked * 100.0) if checked else 0.0
    print(f"mismatch rate : {rate:.4f}%")
    if errors:
        print(f"emulation faults: {errors}")
    for kind, n in sorted(buckets.items(), key=lambda kv: -kv[1]):
        print(f"\n  [{n}] {kind}")
        for word, got, want in examples[kind]:
            if got is None:
                print(f"      {word.decode('latin-1')}")
                continue
            print(f"      {word.decode('latin-1')}: "
                  f"ours rc={got['rc']} out={got['out'].decode('latin-1')!r} "
                  f"flags={got['flags']:#x} consumed={got['consumed']}")
            print(f"      {' ' * len(word)}  "
                  f"orig rc={want['rc']} out={want['out'].decode('latin-1')!r} "
                  f"flags={want['flags']:#x} consumed={want['consumed']}")
    return mismatches, checked


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--library", required=True)
    ap.add_argument("--language", default="eng", choices=sorted(LANGUAGES),
                    help="which matcher entry point / C symbol to use: "
                         "eng = TIENG32!FUN_1c206860 / sv_lang_data_eng, "
                         "span = TISPAN32's own matcher / sv_lang_data_span")
    ap.add_argument("--words", default="/usr/share/dict/words")
    ap.add_argument("--cases", type=int, default=20000,
                    help="dictionary words to sample (0 = all)")
    ap.add_argument("--random-cases", type=int, default=5000,
                    help="randomized stress strings (0 = skip)")
    ap.add_argument("--pattern", default=r"[A-Z]+")
    ap.add_argument("--seed", type=int, default=0x19961118)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stop-after", type=int, default=0,
                    help="give up after this many mismatches in a cohort")
    ap.add_argument("--probe", action="append", default=[],
                    help="run one word and print both sides, then exit")
    args = ap.parse_args()
    preset = LANGUAGES[args.language]

    original = Original(args.dll, preset["image_base"], preset["fun_addr"],
                        preset["prologue"])
    ours = Reconstruction(args.library, preset["symbol"])

    if args.probe:
        for word in args.probe:
            raw = word.upper().encode("latin-1")
            framed, offset = frame(raw)
            want = original.run(framed, offset)
            got = ours.run(framed, offset)
            verdict = "AGREE" if (got["rc"] == want["rc"]
                                  and got["out"] == want["out"]
                                  and got["consumed"] == want["consumed"]
                                  and got["flags"] == want["flags"]) else "DIFFER"
            print(f"{word.upper()}: {verdict}")
            print(f"  original : rc={want['rc']} "
                  f"out={want['out'].decode('latin-1')!r} "
                  f"flags={want['flags']:#x} consumed={want['consumed']} "
                  f"cap_left={want['cap_left']} count={want['count']}")
            print(f"  ours     : rc={got['rc']} "
                  f"out={got['out'].decode('latin-1')!r} "
                  f"flags={got['flags']:#x} consumed={got['consumed']}")
        return 0

    total_bad = 0
    total_checked = 0
    if args.cases != 0 or args.words:
        words, available = load_words(args.words, args.cases, args.seed,
                                      args.pattern)
        print(f"{args.words}: {available} words match /{args.pattern}/, "
              f"testing {len(words)}")
        bad, checked = compare(original, ours, words,
                               f"dictionary cohort ({len(words)} words)",
                               args.verbose, args.stop_after)
        total_bad += bad
        total_checked += checked
    if args.random_cases:
        cases = random_words(args.random_cases, args.seed ^ 0x5A5A5A5A)
        bad, checked = compare(original, ours, cases,
                               f"randomized cohort ({len(cases)} strings)",
                               args.verbose, args.stop_after)
        total_bad += bad
        total_checked += checked

    print(f"\nstrncat calls emulated: {original.strncat_calls}")
    if total_bad:
        print(f"\nFAIL: {total_bad} of {total_checked} inputs disagree with "
              f"the original")
        return 1
    print(f"\nPASS: {total_checked} inputs produce byte-identical output, rule "
          f"flags and cursor position in both the original and the "
          f"reconstruction")
    return 0


if __name__ == "__main__":
    sys.exit(main())
