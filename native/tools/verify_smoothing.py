#!/usr/bin/env python3
"""Development-only differential test of the reconstructed pitch-smoothing passes.

Same pattern as tools/verify_dsp.py and tools/verify_frames.py: map the real
TIBASE32.DLL under Unicorn, run the ORIGINAL x86 code, call the C
reconstruction (src/smoothing.c) through ctypes on a copy of the same input,
and compare every byte of the frame array afterward -- not just the `pitch`
fields we already believe are the only ones touched.

Two stages, the two passes the engine runs back to back (0x1c0039f8 and
0x1c003a01):

  interpolate   0x1c00cd40..0x1c00cdb3   sv_smooth_pitch()
  slew          0x1c00de60..0x1c00df00   sv_smooth_pitch_slew()

Requires `pip install unicorn==2.1.4` in a development venv. Nothing here is linked into or
required by the native library or the application, and no Windows TTS host is
involved -- only this one routine runs.

Unlike verify_frames.py and verify_dsp.py, these passes need no extracted data
table at all: they do arithmetic over the frame array's `pitch` field, nothing
else. There is therefore no --check-extract here and no proprietary bytes enter
this script beyond the DLL path the user supplies on the command line.

Build a test library, for example:
  cc -shared -fPIC -std=c11 -Iinclude src/smoothing.c -o build/libsmoothing.dylib
  python tools/verify_smoothing.py --dll /path/TIBASE32.DLL \\
         --library build/libsmoothing.dylib [--stage interpolate|slew|all]
"""
import argparse
import ctypes as C
import random
import struct
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP

IMAGE_BASE = 0x1C000000
FRAME_SIZE = 0x20

ENTRY = 0x1C00CD40
# First 10 bytes of the function: mov eax,[esp+4]; sub esp,4; push ebx; push
# esi; push edi. Guards against silently running a different build's code.
ENTRY_SIGNATURE = bytes.fromhex("8b44240483ec04535657")

# The slew pass, 0x1c00de60: mov eax,[esp+4]; sub esp,4; push ebx;
# mov edx,[eax+0xf6].
SLEW_ENTRY = 0x1C00DE60
SLEW_SIGNATURE = bytes.fromhex("8b44240483ec04538b90")
SLEW_RATE_RISE = 0x64   # state+0x64, read at 0x1c00de79
SLEW_RATE_FALL = 0x66   # state+0x66, read at 0x1c00de71

# --- guest memory ------------------------------------------------------------
STATE, FRAMES, STACK, MAGIC_RET = (0x00100000, 0x00110000, 0x00300000,
                                   0x00400000)
STATE_SIZE = 0x200
FRAMES_CAP = 0x10000  # bytes; room for up to 0x800 frames, far past any case
MAX_FRAMES = FRAMES_CAP // FRAME_SIZE


def map_pe(uc, path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    image_base = struct.unpack_from("<I", data, optional + 28)[0]
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    if machine != 0x14C or image_base != IMAGE_BASE:
        raise ValueError("expected the supplied i386 TIBASE32 build")
    uc.mem_map(image_base, (image_size + 4095) & ~4095)
    for i in range(count):
        section = optional + optional_size + 40 * i
        rva, raw_size, raw_offset = struct.unpack_from("<III", data, section + 12)
        uc.mem_write(image_base + rva, data[raw_offset:raw_offset + raw_size])
    for entry, signature in ((ENTRY, ENTRY_SIGNATURE),
                             (SLEW_ENTRY, SLEW_SIGNATURE)):
        got = bytes(uc.mem_read(entry, len(signature)))
        if got != signature:
            raise ValueError(
                f"unsupported TIBASE32 build: expected {signature.hex()} "
                f"at {entry:#x}, got {got.hex()}")


def set_pitch(buf, index, value):
    struct.pack_into("<H", buf, index * FRAME_SIZE + 0x18, value)


def get_pitch(buf, index):
    return struct.unpack_from("<H", buf, index * FRAME_SIZE + 0x18)[0]


def set_terminator(buf, index):
    """TIBASE32 0x1c0058c5..0x1c0058d5: formant_freq[0..2]=0xff,
    amp_voicing=0xff, pitch=0xffff. Only formant_freq[0] and pitch matter to
    this pass, but all five bytes are set here for fidelity to the real
    array-writer this pass's input always comes from."""
    off = index * FRAME_SIZE
    buf[off + 0] = 0xFF
    buf[off + 1] = 0xFF
    buf[off + 2] = 0xFF
    buf[off + 0x10] = 0xFF
    struct.pack_into("<H", buf, off + 0x18, 0xFFFF)


def random_frames(rng, min_frames=6, max_frames=300, p_zero=0.35):
    """A frame array with random runs of zero pitch between random nonzero
    anchors, terminated per set_terminator(). `p_zero` controls how often a
    given interior frame starts as a gap versus its own anchor, so both long
    runs and back-to-back anchors (run == 0) show up across many cases."""
    n = rng.randrange(min_frames, max_frames)
    buf = bytearray(n * FRAME_SIZE)
    for i in range(n):
        pitch = 0 if rng.random() < p_zero else rng.randrange(1, 0xFFFE)
        set_pitch(buf, i, pitch)
    set_terminator(buf, n - 1)
    return buf, n


def run_original(uc, buf, n):
    uc.mem_write(FRAMES, bytes(buf))
    state = bytearray(STATE_SIZE)
    struct.pack_into("<I", state, 0xF6, FRAMES)
    uc.mem_write(STATE, bytes(state))
    esp = STACK + 0x8000
    uc.mem_write(esp, struct.pack("<II", MAGIC_RET, STATE))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.emu_start(ENTRY, MAGIC_RET, count=2_000_000)
    state_after = bytes(uc.mem_read(STATE, STATE_SIZE))
    frames_after = bytes(uc.mem_read(FRAMES, n * FRAME_SIZE))
    return frames_after, state_after


def run_reconstruction(lib, buf, n):
    mirror = (C.c_uint8 * (n * FRAME_SIZE)).from_buffer_copy(bytes(buf))
    lib.sv_smooth_pitch(C.cast(mirror, C.c_void_p))
    return bytes(mirror)


def diff_frames(case, before, expected, actual, n):
    for i in range(n):
        off = i * FRAME_SIZE
        e = expected[off:off + FRAME_SIZE]
        a = actual[off:off + FRAME_SIZE]
        if e != a:
            be = before[off:off + FRAME_SIZE]
            return (f"case {case}: frame {i}/{n} differs\n"
                    f"  before   pitch={struct.unpack_from('<H', be, 0x18)[0]:#06x} "
                    f"formant0={be[0]:#04x}\n"
                    f"  original pitch={struct.unpack_from('<H', e, 0x18)[0]:#06x} "
                    f"bytes={e.hex()}\n"
                    f"  recon    pitch={struct.unpack_from('<H', a, 0x18)[0]:#06x} "
                    f"bytes={a.hex()}")
    return None


def stage_interpolate(uc, lib, cases, rng):
    """0x1c00cd40: fill zero-pitch runs between anchors."""
    compared = 0
    max_run_seen = 0
    trailing_untouched_seen = False
    multi_run_seen = False

    for case in range(cases):
        buf, n = random_frames(rng)
        assert n <= MAX_FRAMES

        expected, state_after = run_original(uc, buf, n)
        actual = run_reconstruction(lib, buf, n)

        mismatch = diff_frames(case, buf, expected, actual, n)
        if mismatch:
            raise AssertionError(mismatch)

        # The function never writes to the state block; only offset 0xf6 was
        # ever set, and only ever read. Rebuild what we wrote and compare.
        want_state = bytearray(STATE_SIZE)
        struct.pack_into("<I", want_state, 0xF6, FRAMES)
        if state_after != bytes(want_state):
            raise AssertionError(
                f"case {case}: the original wrote to its state block, which "
                f"this pass is assumed never to touch -- offsets differ from "
                f"the {STATE_SIZE}-byte image with only +0xf6 set")

        # Coverage bookkeeping, over the PRE-smoothing buffer (the
        # post-smoothing one has the gaps filled in, so scanning it for
        # pitch==0 would only ever find an unresolved trailing run).
        # Anchors are frame 0, every nonzero-pitch frame, and the terminator
        # at n-1 (pitch 0xffff, never touched). A run between two anchors
        # gets interpolated; a run whose right neighbour is the terminator
        # does not (0x1c00cd73..0x1c00cd76 checks that before using `run`).
        anchors = [0] + [k for k in range(1, n) if get_pitch(buf, k) != 0]
        resolved_gaps = 0
        for a, b in zip(anchors, anchors[1:]):
            run = b - a - 1
            if run > max_run_seen:
                max_run_seen = run
            if run > 0:
                if b == n - 1:
                    trailing_untouched_seen = True
                else:
                    resolved_gaps += 1
        if resolved_gaps > 1:
            multi_run_seen = True

        compared += 1

    if max_run_seen == 0:
        raise AssertionError("generator produced no interpolation runs at all "
                             "-- the differential would be vacuous")

    print(f"PASS interpolate: {compared} randomized frame arrays match TIBASE32 "
          f"{ENTRY:#x}..0x1c00cdb3 byte-for-byte (frame array and state block)")
    print(f"      longest interior run interpolated: {max_run_seen} frames")
    print(f"      trailing zero-run left untouched at least once: "
          f"{trailing_untouched_seen}")
    print(f"      multiple independent runs in one array at least once: "
          f"{multi_run_seen}")
    return compared


def random_rate(rng):
    """A rate pair as the engine supplies it -- Q8, 0..256, where 256 snaps to
    the frame's own pitch and 0 freezes the accumulator -- plus, one case in
    eight, an arbitrary uint16. The engine cannot produce the second kind
    (0x1c00df20 copies both from the voice block, which holds Q8 rates), but
    the reconstruction reproduces the original's 32-bit wrapping rather than
    assuming its caller, so the wrapping needs cases that reach it."""
    if rng.random() < 0.125:
        return rng.randrange(0, 0x10000)
    return rng.choice((0, 256, rng.randrange(0, 257)))


def random_slew_frames(rng, min_frames=2, max_frames=300):
    """Pitch tracks for the slew pass. This one reads every frame's pitch
    rather than looking for zero runs, so the interesting axis is the SHAPE of
    the track: flat stretches (which exercise the tie that sends both passes
    to the falling rate), steps, and noise."""
    n = rng.randrange(min_frames, max_frames)
    buf = bytearray(n * FRAME_SIZE)
    shape = rng.choice(("noise", "steps", "flat", "ramp"))
    value = rng.randrange(0, 0x10000)
    for i in range(n):
        if shape == "noise":
            value = rng.randrange(0, 0x10000)
        elif shape == "steps":
            if rng.random() < 0.2:
                value = rng.randrange(0, 0x10000)
        elif shape == "ramp":
            value = min(0xFFFF, max(0, value + rng.randrange(-400, 401)))
        set_pitch(buf, i, value)
    set_terminator(buf, n - 1)
    return buf, n


def run_slew_original(uc, buf, n, rise, fall):
    uc.mem_write(FRAMES, bytes(buf))
    state = bytearray(STATE_SIZE)
    struct.pack_into("<I", state, 0xF6, FRAMES)
    struct.pack_into("<H", state, SLEW_RATE_RISE, rise)
    struct.pack_into("<H", state, SLEW_RATE_FALL, fall)
    uc.mem_write(STATE, bytes(state))
    esp = STACK + 0x8000
    uc.mem_write(esp, struct.pack("<II", MAGIC_RET, STATE))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.emu_start(SLEW_ENTRY, MAGIC_RET, count=2_000_000)
    state_after = bytes(uc.mem_read(STATE, STATE_SIZE))
    frames_after = bytes(uc.mem_read(FRAMES, n * FRAME_SIZE))
    return frames_after, state_after


def run_slew_reconstruction(lib, buf, n, rise, fall):
    mirror = (C.c_uint8 * (n * FRAME_SIZE)).from_buffer_copy(bytes(buf))
    lib.sv_smooth_pitch_slew(C.cast(mirror, C.c_void_p), rise, fall)
    return bytes(mirror)


def stage_slew(uc, lib, cases, rng):
    """0x1c00de60: the two-rate lag, forward then backward."""
    compared = 0
    rises = falls = ties = 0
    wrapping_cases = 0
    changed_cases = 0

    for case in range(cases):
        buf, n = random_slew_frames(rng)
        assert n <= MAX_FRAMES
        rise, fall = random_rate(rng), random_rate(rng)

        expected, state_after = run_slew_original(uc, buf, n, rise, fall)
        actual = run_slew_reconstruction(lib, buf, n, rise, fall)

        mismatch = diff_frames(case, buf, expected, actual, n)
        if mismatch:
            raise AssertionError(f"rise={rise} fall={fall}\n{mismatch}")

        want_state = bytearray(STATE_SIZE)
        struct.pack_into("<I", want_state, 0xF6, FRAMES)
        struct.pack_into("<H", want_state, SLEW_RATE_RISE, rise)
        struct.pack_into("<H", want_state, SLEW_RATE_FALL, fall)
        if state_after != bytes(want_state):
            raise AssertionError(
                f"case {case}: the original wrote to its state block, which "
                f"this pass is assumed never to touch")

        # Coverage. The edge counts are taken from the INPUT track: what the
        # forward pass compares against is the accumulator, not the previous
        # frame, but a track with no rises cannot reach the rising rate at all
        # and a flat one is the only way to reach the tie.
        if rise > 256 or fall > 256:
            wrapping_cases += 1
        if expected != bytes(buf):
            changed_cases += 1
        previous = get_pitch(buf, 0)
        for i in range(1, n - 1):
            pitch = get_pitch(buf, i)
            if pitch > previous:
                rises += 1
            elif pitch < previous:
                falls += 1
            else:
                ties += 1
            previous = pitch

        compared += 1

    if not (rises and falls and ties):
        raise AssertionError(
            f"the generated tracks miss an edge kind (rises={rises} "
            f"falls={falls} ties={ties}) -- the rate selection would be "
            f"only partly exercised")
    if changed_cases == 0:
        raise AssertionError("no case changed the track -- vacuous")

    print(f"PASS slew:        {compared} randomized pitch tracks match TIBASE32 "
          f"{SLEW_ENTRY:#x}..0x1c00df00 byte-for-byte (frame array and state block)")
    print(f"      input edges seen: {rises} rising, {falls} falling, {ties} flat")
    print(f"      cases with out-of-range rates (32-bit wrapping reachable): "
          f"{wrapping_cases}")
    print(f"      cases where the pass changed the track: {changed_cases}")
    return compared


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--cases", type=int, default=5000)
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=0x19961118)
    parser.add_argument("--stage", choices=("interpolate", "slew", "all"),
                        default="all")
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")

    lib = C.CDLL(str(Path(args.library).resolve()))
    lib.sv_smooth_pitch.argtypes = [C.c_void_p]
    lib.sv_smooth_pitch.restype = None
    lib.sv_smooth_pitch_slew.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
    lib.sv_smooth_pitch_slew.restype = None

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    map_pe(uc, args.dll)
    # mem_map requires page-aligned sizes; STATE_SIZE (0x200) is the logical
    # buffer size used for comparisons, not the mapping size.
    for address, size in ((STATE, 0x1000), (FRAMES, FRAMES_CAP),
                          (STACK, 0x10000), (MAGIC_RET, 0x1000)):
        uc.mem_map(address, size)

    rng = random.Random(args.seed)
    compared = 0
    if args.stage in ("interpolate", "all"):
        compared += stage_interpolate(uc, lib, args.cases, rng)
    if args.stage in ("slew", "all"):
        compared += stage_slew(uc, lib, args.cases, rng)

    if compared == 0:
        raise AssertionError("no cases were run")


if __name__ == "__main__":
    try:
        main()
    except UcError as exc:
        raise SystemExit(f"unicorn error: {exc}")
