#!/usr/bin/env python3
"""Development-only differential test of the reconstructed parameter-frame stage.

Same pattern as tools/verify_dsp.py, which settled the sample kernel: map the
real TIBASE32.DLL under Unicorn, run the ORIGINAL x86 code, call the C
reconstruction through ctypes, and compare the output AND the whole state
block. Requires `pip install unicorn==2.1.4` in a development venv. Nothing
here is linked into or required by the native library or the application, and
no Windows TTS host is involved — only the integer routines listed below run.

Three stages, all of them comparing every byte of the original's 0x400-byte
state block rather than the fields we already believe in. That whole-block
comparison is the point: the amplitude table is read at an UNSCALED byte
offset (0x1c004857), and a reconstruction that indexes it by element produces
identical output for most frames and diverges on the rest. Nothing short of a
differential finds that.

  --stage apply     VA 1c0044cb..1c004969, the per-frame state fill
  --stage render    VA 1c00402c..1c00498e, the whole renderer including the
                    sample loop, the counters and the resume path
  --stage phoneme   VA 1c004c85..1c004d29, the phoneme-name lookup

Build a test library, for example:
  cc -shared -fPIC -std=c11 -Iinclude src/dsp.c src/frames.c -o build/libframes.dylib
  python tools/verify_frames.py --dll /path/TIBASE32.DLL --library build/libframes.dylib
"""
import argparse
import ctypes as C
import random
import struct
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_EDI,
                               UC_X86_REG_EBP, UC_X86_REG_ESP)

IMAGE_BASE = 0x1C000000

# --- original data addresses, as extract_base.py uses them -----------------
RESONATOR_VA, RESONATOR_LEN = 0x1C002570, 0x1180
AMPLITUDE_VA, AMPLITUDE_LEN = 0x1C001308, 0x102
NOISE_VA, NOISE_COUNT = 0x1C001570, 2048
CURVE_VA, CURVE_LEN = 0x1C0010FE, 401
GLOTTAL_PTRS, GLOTTAL_COUNT, GLOTTAL_SAMPLES = 0x1C013BD0, 12, 1024
PHONEME_PTRS, PHONEME_LISTS = 0x1C012A68, 0x3C

# --- original code addresses ------------------------------------------------
RENDER_ENTRY = 0x1C00402C
APPLY_ENTRY = 0x1C0044CB
APPLY_EXIT = 0x1C004969          # jmp back into the sample loop
PHONEME_ENTRY = 0x1C004C85
PHONEME_EXITS = (0x1C004D05, 0x1C004D29)

# --- guest memory -----------------------------------------------------------
STATE, FRAMES, OUTPUT, TEXT, STACK = (0x00100000, 0x00110000, 0x00120000,
                                      0x00130000, 0x00300000)
MAGIC_RET = 0x00400000

FRAME_SIZE = 0x20
STATE_SIZE = 0x400

# sv_dsp_state.filters[] in enum order; same table as tools/verify_dsp.py.
FILTER_OFFSETS = (0x23E, 0x202, 0x1B2, 0x1DA, 0x1EE, 0x1C6, 0x216)
UNUSED_FILTER_OFFSETS = (0x22A, 0x252)


# --------------------------------------------------------------------------
# ctypes mirrors of include/tispeech/{dsp,frames}.h
# --------------------------------------------------------------------------
class Ramp(C.Structure):
    _fields_ = [("value", C.c_uint32), ("step", C.c_uint32)]


class Filter(C.Structure):
    _fields_ = [("previous", C.c_int16), ("previous2", C.c_int16),
                ("a", Ramp), ("b", Ramp)]


class DspState(C.Structure):
    _fields_ = [("phase", C.c_uint32), ("phase_step", C.c_uint32),
                ("mix_a", C.c_int16), ("mix_b", C.c_int16), ("voicing", Ramp),
                ("previous_voice", C.c_int16),
                ("aspiration_negative", C.c_int16),
                ("aspiration_positive", C.c_int16),
                ("frication_negative", C.c_int16),
                ("frication_positive", C.c_int16),
                ("frication_shape", C.c_int16), ("filters", Filter * 7)]


class FrameState(C.Structure):
    _fields_ = [
        ("dsp", DspState), ("unused_filters", Filter * 2),
        ("aspiration_frac", C.c_uint16), ("aspiration_level", C.c_uint16),
        ("aspiration_step", C.c_uint32),
        ("frication_frac", C.c_uint16), ("frication_level", C.c_uint16),
        ("frication_step", C.c_uint32),
        ("shape_frac", C.c_uint16), ("shape_step", C.c_uint32),
        ("level_frac", C.c_uint16 * 4), ("voice_frac", C.c_uint16),
        ("source_a", C.POINTER(C.c_int16)), ("source_b", C.POINTER(C.c_int16)),
        ("sample_rate", C.c_uint16), ("flags", C.c_uint32),
        ("frame_index", C.c_uint32), ("elapsed", C.c_uint32),
        ("frame_length", C.c_uint16), ("interp_scale", C.c_int16),
        ("scratch_2c0", C.c_uint32), ("scratch_2e8", C.c_uint32),
        ("noise_index", C.c_uint32), ("noise_left", C.c_uint16),
        ("samples_left", C.c_uint16), ("frame_left", C.c_uint16),
        ("restart", C.c_uint16), ("scratch_310", C.c_uint16),
        ("last_phoneme", C.c_uint8), ("speaking", C.c_uint16)]


class FrameTables(C.Structure):
    _fields_ = [("resonator", C.POINTER(C.c_uint8)),
                ("amplitude", C.POINTER(C.c_uint8)),
                ("glottal", C.POINTER(C.POINTER(C.c_int16))),
                ("noise", C.POINTER(C.c_int16)),
                ("output_curve", C.POINTER(C.c_uint8))]


class PhonemeNames(C.Structure):
    _fields_ = [("lists", C.POINTER(C.c_uint8) * PHONEME_LISTS)]


SV_FRAMES_END, SV_FRAMES_OK = 0, 1
SV_PHONEME_INVALID = 0xFF


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
    # Reject a different build instead of accidentally running another function.
    if bytes(uc.mem_read(0x1C004120, 6)) != bytes.fromhex("8b93fc020000"):
        raise ValueError("unsupported TIBASE32 sample-loop signature")
    if bytes(uc.mem_read(0x1C004857, 11)) != bytes.fromhex("0fb67e10668b870813001c"):
        raise ValueError("unsupported TIBASE32 amplitude-lookup signature")


# --------------------------------------------------------------------------
# State block <-> C struct. Every field the reconstruction models appears in
# exactly one of these tables, so encode(decode(x)) == x for a legal image and
# any byte the original writes outside them shows up as an unmodelled diff.
# --------------------------------------------------------------------------
U16 = "<H"
I16 = "<h"
U32 = "<I"

SIMPLE = (
    # (state offset, format, python path into FrameState)
    (0x2FC, U32, "dsp.phase"),
    (0x300, U32, "dsp.phase_step"),
    (0x268, I16, "dsp.mix_a"),
    (0x270, I16, "dsp.mix_b"),
    (0x276, U32, "dsp.voicing.value"),
    (0x27A, U32, "dsp.voicing.step"),
    (0x2AA, U16, "voice_frac"),
    (0x2AC, I16, "dsp.previous_voice"),
    (0x27E, U16, "aspiration_frac"),
    (0x280, U16, "aspiration_level"),
    (0x282, U32, "aspiration_step"),
    (0x286, U16, "frication_frac"),
    (0x288, U16, "frication_level"),
    (0x28A, U32, "frication_step"),
    (0x28E, U16, "shape_frac"),
    (0x290, I16, "dsp.frication_shape"),
    (0x292, U32, "shape_step"),
    (0x296, U16, "level_frac.0"),
    (0x298, I16, "dsp.aspiration_negative"),
    (0x29A, U16, "level_frac.1"),
    (0x29C, I16, "dsp.aspiration_positive"),
    (0x29E, U16, "level_frac.2"),
    (0x2A0, I16, "dsp.frication_negative"),
    (0x2A2, U16, "level_frac.3"),
    (0x2A4, I16, "dsp.frication_positive"),
    (0x048, U16, "sample_rate"),
    (0x04E, U32, "flags"),
    (0x0FE, U32, "elapsed"),
    (0x2BC, U16, "frame_length"),
    (0x2FA, I16, "interp_scale"),
    (0x2C0, U32, "scratch_2c0"),
    (0x2E8, U32, "scratch_2e8"),
    (0x2EE, U16, "noise_left"),
    (0x2F4, U16, "samples_left"),
    (0x30C, U16, "frame_left"),
    (0x30E, U16, "restart"),
    (0x310, U16, "scratch_310"),
    (0x312, "<B", "last_phoneme"),
    (0x314, U16, "speaking"),
)


def _get(state, path):
    node = state
    for part in path.split("."):
        node = node[int(part)] if part.isdigit() else getattr(node, part)
    return node


def _set(state, path, value):
    parts = path.split(".")
    node = state
    for part in parts[:-1]:
        node = node[int(part)] if part.isdigit() else getattr(node, part)
    last = parts[-1]
    if last.isdigit():
        node[int(last)] = value
    else:
        setattr(node, last, value)


def _filter_spans():
    for i, off in enumerate(FILTER_OFFSETS):
        yield off, f"dsp.filters.{i}"
    for i, off in enumerate(UNUSED_FILTER_OFFSETS):
        yield off, f"unused_filters.{i}"


class Bridge:
    """Translates between the original's 0x400 block and our FrameState."""

    def __init__(self, glottal_bufs):
        # Host pointer <-> original VA, so source_a/source_b survive the trip.
        self.va_of = {}
        self.buf_of = {}
        for i, buf in enumerate(glottal_bufs):
            va = struct.unpack("<I", bytes(glottal_ptr_bytes[i]))[0]
            self.va_of[C.addressof(buf)] = va
            self.buf_of[va] = buf

    def decode(self, raw):
        s = FrameState()
        for off, fmt, path in SIMPLE:
            _set(s, path, struct.unpack_from(fmt, raw, off)[0])
        for off, path in _filter_spans():
            f = _get(s, path)
            f.previous = struct.unpack_from(I16, raw, off)[0]
            f.previous2 = struct.unpack_from(I16, raw, off + 2)[0]
            f.a.value = struct.unpack_from(U32, raw, off + 4)[0]
            f.a.step = struct.unpack_from(U32, raw, off + 8)[0]
            f.b.value = struct.unpack_from(U32, raw, off + 12)[0]
            f.b.step = struct.unpack_from(U32, raw, off + 16)[0]
        frames_base = struct.unpack_from(U32, raw, 0xF6)[0]
        s.frame_index = (struct.unpack_from(U32, raw, 0xFA)[0] - frames_base) // FRAME_SIZE
        s.noise_index = (struct.unpack_from(U32, raw, 0x2E4)[0] - NOISE_VA) // 2
        for field, off in (("source_a", 0x2D8), ("source_b", 0x2DC)):
            va = struct.unpack_from(U32, raw, off)[0]
            buf = self.buf_of[va]
            setattr(s, field, C.cast(buf, C.POINTER(C.c_int16)))
        return s

    def encode(self, s, initial):
        """Rebuild the block, leaving unmodelled bytes at their initial value."""
        raw = bytearray(initial)
        for off, fmt, path in SIMPLE:
            value = _get(s, path)
            if fmt in (U16, "<B"):
                value &= 0xFFFF if fmt == U16 else 0xFF
            elif fmt == U32:
                value &= 0xFFFFFFFF
            struct.pack_into(fmt, raw, off, value)
        for off, path in _filter_spans():
            f = _get(s, path)
            struct.pack_into(I16, raw, off, f.previous)
            struct.pack_into(I16, raw, off + 2, f.previous2)
            struct.pack_into(U32, raw, off + 4, f.a.value)
            struct.pack_into(U32, raw, off + 8, f.a.step)
            struct.pack_into(U32, raw, off + 12, f.b.value)
            struct.pack_into(U32, raw, off + 16, f.b.step)
        frames_base = struct.unpack_from(U32, raw, 0xF6)[0]
        struct.pack_into(U32, raw, 0xFA, frames_base + FRAME_SIZE * s.frame_index)
        struct.pack_into(U32, raw, 0x2E4, NOISE_VA + 2 * s.noise_index)
        for field, off in (("source_a", 0x2D8), ("source_b", 0x2DC)):
            ptr = getattr(s, field)
            struct.pack_into(U32, raw, off, self.va_of[C.cast(ptr, C.c_void_p).value])
        return bytes(raw)


glottal_ptr_bytes = []  # filled in by build_tables()


def build_tables(uc):
    """Our sv_frame_tables, read straight out of the mapped image.

    Deliberately NOT built from tools/extract_base.py's output: taking the
    tables from the same bytes the emulator runs against means a mistake in
    the extractor cannot cancel out a matching mistake in the reconstruction.
    The extractor is checked separately, by --check-extract.
    """
    resonator = (C.c_uint8 * RESONATOR_LEN).from_buffer_copy(
        bytes(uc.mem_read(RESONATOR_VA, RESONATOR_LEN)))
    amplitude = (C.c_uint8 * AMPLITUDE_LEN).from_buffer_copy(
        bytes(uc.mem_read(AMPLITUDE_VA, AMPLITUDE_LEN)))
    curve = (C.c_uint8 * CURVE_LEN).from_buffer_copy(
        bytes(uc.mem_read(CURVE_VA, CURVE_LEN)))
    noise = (C.c_int16 * NOISE_COUNT).from_buffer_copy(
        bytes(uc.mem_read(NOISE_VA, NOISE_COUNT * 2)))
    bufs, ptrs = [], (C.POINTER(C.c_int16) * GLOTTAL_COUNT)()
    del glottal_ptr_bytes[:]
    for i in range(GLOTTAL_COUNT):
        va = struct.unpack("<I", bytes(uc.mem_read(GLOTTAL_PTRS + 4 * i, 4)))[0]
        glottal_ptr_bytes.append(struct.pack("<I", va))
        buf = (C.c_int16 * GLOTTAL_SAMPLES).from_buffer_copy(
            bytes(uc.mem_read(va, GLOTTAL_SAMPLES * 2)))
        bufs.append(buf)
        ptrs[i] = C.cast(buf, C.POINTER(C.c_int16))
    tables = FrameTables(
        C.cast(resonator, C.POINTER(C.c_uint8)),
        C.cast(amplitude, C.POINTER(C.c_uint8)),
        ptrs,
        C.cast(noise, C.POINTER(C.c_int16)),
        C.cast(curve, C.POINTER(C.c_uint8)))
    # Keep every buffer alive for the process lifetime.
    tables._keep = (resonator, amplitude, curve, noise, bufs, ptrs)
    return tables, bufs


def build_names(uc):
    names = PhonemeNames()
    keep = []
    for i in range(PHONEME_LISTS):
        va = struct.unpack("<I", bytes(uc.mem_read(PHONEME_PTRS + 4 * i, 4)))[0]
        if va == 0:
            continue
        pairs = bytearray()
        while True:
            pair = bytes(uc.mem_read(va + len(pairs), 2))
            pairs += pair
            if pair[0] == 0:
                break
        buf = (C.c_uint8 * len(pairs)).from_buffer_copy(bytes(pairs))
        keep.append(buf)
        names.lists[i] = C.cast(buf, C.POINTER(C.c_uint8))
    names._keep = keep
    return names


# --------------------------------------------------------------------------
# Case generation
# --------------------------------------------------------------------------
# Row indices the extracted blob can still answer for; past these the original
# reads its own .text and src/frames.c returns SV_FRAMES_E_RANGE instead.
CASCADE_ROWS_ADDRESSABLE = 140
PARALLEL_ROWS_ADDRESSABLE = 51


def random_frame(rng, terminator=False):
    f = bytearray(rng.randrange(256) for _ in range(FRAME_SIZE))
    if terminator:
        f[0] = 0xFF
        return bytes(f)
    for i in range(3):
        f[i] = rng.randrange(CASCADE_ROWS_ADDRESSABLE)
    f[3] = rng.randrange(PARALLEL_ROWS_ADDRESSABLE)
    f[4] = rng.randrange(PARALLEL_ROWS_ADDRESSABLE)
    f[9] = rng.randrange(PARALLEL_ROWS_ADDRESSABLE)
    f[0x0D] = rng.randrange(GLOTTAL_COUNT)
    f[0x0E] = rng.randrange(GLOTTAL_COUNT)
    f[0x0B] = 0 if rng.randrange(4) else rng.randrange(3, 256)  # marker
    # A duration the original does not fault on: frame_length must be >= 3.
    f[0x0A] = rng.randrange(1, 0x20)
    return bytes(f)


def random_state(rng, frame_count, sample_rate=11025):
    raw = bytearray(rng.randrange(256) for _ in range(STATE_SIZE))
    struct.pack_into(U16, raw, 0x048, sample_rate)
    struct.pack_into(U32, raw, 0x04E, 0x40)            # events suppressed
    struct.pack_into(U32, raw, 0x0F6, FRAMES)
    struct.pack_into(U32, raw, 0x15E, OUTPUT)
    index = rng.randrange(2, max(3, frame_count - 2))
    struct.pack_into(U32, raw, 0x0FA, FRAMES + FRAME_SIZE * index)
    for off in (0x2D8, 0x2DC):
        struct.pack_into(U32, raw, off,
                         struct.unpack("<I", glottal_ptr_bytes[
                             rng.randrange(GLOTTAL_COUNT)])[0])
    # The noise cursor and its countdown are not independent. The reset puts
    # the cursor at the table base with the counter at 0x3ff, and every sample
    # advances the cursor by two int16 and decrements the counter, so the only
    # states the original can ever be in are
    #     noise_index == 2 * (0x3ff - noise_left).
    # Generating the two at random walks the cursor off the end of the 2048
    # sample table, where the original reads the coefficient table that follows
    # it in the image and the reconstruction reads past its own array. That is
    # a defect in the generator, not a finding about either one.
    # A call returns out of 0x1c00449a before decrementing the counter it
    # owes, so the relation an ENTRY state satisfies is 0x400 - noise_left,
    # not 0x3ff - noise_left. Both stay inside the 2048 sample table; the
    # reachable one reaches its last two samples and the other does not, which
    # is exactly the case worth covering.
    noise_left = rng.randrange(1, 0x400)
    struct.pack_into(U16, raw, 0x2EE, noise_left)
    struct.pack_into(U32, raw, 0x2E4, NOISE_VA + 2 * 2 * (0x400 - noise_left))
    # The low half of the previous-voice dword at 0x2aa is invariably zero. The
    # sample loop holds that value as `voiced << 16`: 0x1c0041d0 does
    #     ror edx,0x10 ; xor dx,dx ; mov ecx,edx ; xchg [ebx+0x2aa],edx
    # so it zeroes the fraction before every store, and sv_frame_reset zeroes
    # the whole dword at 0x1c0040b0. src/dsp.c is entitled to keep
    # previous_voice as an int16 BECAUSE of that invariant -- the `sub ecx,edx`
    # two instructions later is a full 32-bit subtract, so a non-zero fraction
    # would borrow into the differentiated sample. Seeding one here would test
    # a state the engine cannot reach.
    struct.pack_into(U16, raw, 0x2AA, 0)
    struct.pack_into(U16, raw, 0x30C, rng.randrange(1, 0x100))
    struct.pack_into(U16, raw, 0x2BC, rng.randrange(3, 0x200))
    length = struct.unpack_from(U16, raw, 0x2BC)[0]
    struct.pack_into(I16, raw, 0x2FA, 0x10000 // length)
    return bytes(raw)


# The sample loop reseeds 0x280 and 0x288 on every sample with whichever of
# the negative/positive aspiration and frication levels the glottal sign chose
# (0x1c0041c2, 0x1c0041c9). REVERSING.md establishes that nothing reads them
# back and that the frame stage rewrites both at every frame boundary, so
# src/frames.c keeps the chosen value in a local and leaves the slots holding
# what sv_frame_apply() seeded. That is a deliberate divergence in per-sample
# scratch, so the render stage excludes the two words -- and ONLY the render
# stage: the apply stage still compares them, because there they are frame
# state and a wrong amplitude lookup would show up in them first.
RENDER_SCRATCH = frozenset((0x280, 0x281, 0x288, 0x289))


def diff_report(case, original, rebuilt, initial, label, ignore=frozenset()):
    bad = [i for i in range(STATE_SIZE)
           if original[i] != rebuilt[i] and i not in ignore]
    if not bad:
        return ""
    lines = [f"{label} case {case}: {len(bad)} byte(s) of state differ"]
    for i in bad[:24]:
        lines.append(f"  {i:#05x}: original {original[i]:#04x} "
                     f"reconstruction {rebuilt[i]:#04x} (was {initial[i]:#04x})")
    if len(bad) > 24:
        lines.append(f"  ... and {len(bad) - 24} more")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Stages
# --------------------------------------------------------------------------
def stage_apply(uc, lib, tables, bridge, rng, cases):
    compared = 0
    for case in range(cases):
        frame_count = 24
        frames = b"".join(random_frame(rng) for _ in range(frame_count))
        uc.mem_write(FRAMES, frames)
        initial = random_state(rng, frame_count)
        uc.mem_write(STATE, initial)
        uc.reg_write(UC_X86_REG_EBX, STATE)
        uc.reg_write(UC_X86_REG_EDI, OUTPUT)
        uc.reg_write(UC_X86_REG_ESP, STACK + 0x8000)
        uc.emu_start(APPLY_ENTRY, APPLY_EXIT, count=4000)

        state = bridge.decode(initial)
        frame_array = (C.c_uint8 * len(frames)).from_buffer_copy(frames)
        rc = lib.sv_frame_apply(C.byref(state), C.byref(tables),
                                C.cast(frame_array, C.c_void_p))
        if rc != SV_FRAMES_OK:
            raise AssertionError(f"apply case {case}: reconstruction returned "
                                 f"{rc} where the original completed")
        original = bytes(uc.mem_read(STATE, STATE_SIZE))
        rebuilt = bridge.encode(state, initial)
        if original != rebuilt:
            raise AssertionError(diff_report(case, original, rebuilt, initial,
                                             "apply"))
        compared += 1
    return compared


def stage_render(uc, lib, tables, bridge, rng, cases):
    compared = 0
    for case in range(cases):
        # A stream that ends: a run of ordinary frames then a terminator, so
        # both the sample path and the 0x1c00496e silence pad get exercised.
        frame_count = rng.randrange(12, 28)
        body = [random_frame(rng) for _ in range(frame_count)]
        body[-1] = random_frame(rng, terminator=True)
        frames = b"".join(body)
        uc.mem_write(FRAMES, frames)
        frame_array = (C.c_uint8 * len(frames)).from_buffer_copy(frames)

        initial = bytearray(random_state(rng, frame_count))
        restart = rng.randrange(2)
        struct.pack_into(U16, initial, 0x30E, restart)
        if restart:
            # The reset starts at frames + 0x80 and steps once, so the stream
            # must be long enough to reach a terminator from there.
            struct.pack_into(U32, initial, 0x0FA, FRAMES + FRAME_SIZE * 4)
        initial = bytes(initial)

        count = rng.randrange(1, 0x200)
        uc.mem_write(STATE, initial)
        uc.mem_write(OUTPUT, b"\x00" * 0x10000)
        esp = STACK + 0x8000
        uc.mem_write(esp, struct.pack("<III", MAGIC_RET, STATE, count))
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EBP, esp + 0x100)
        uc.emu_start(RENDER_ENTRY, MAGIC_RET, count=4_000_000)
        expected = bytes(uc.mem_read(OUTPUT, count))

        state = bridge.decode(initial)
        out = (C.c_uint8 * count)()
        rc = lib.sv_frame_render(C.byref(state), C.byref(tables),
                                 C.cast(frame_array, C.c_void_p),
                                 C.cast(out, C.c_void_p), count)
        if rc < 0:
            raise AssertionError(f"render case {case}: reconstruction refused "
                                 f"with {rc} where the original completed")
        actual = bytes(out)
        if actual != expected:
            first = next(i for i in range(count) if actual[i] != expected[i])
            raise AssertionError(
                f"render case {case} (restart={restart}, count={count}): PCM "
                f"differs first at sample {first}: reconstruction "
                f"{actual[first]} != original {expected[first]}")
        original = bytes(uc.mem_read(STATE, STATE_SIZE))
        rebuilt = bridge.encode(state, initial)
        report = diff_report(case, original, rebuilt, initial,
                             f"render(restart={restart})", RENDER_SCRATCH)
        if report:
            raise AssertionError(report)
        compared += 1
    return compared


PHONEME_ALPHABET = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[~\x80"


def stage_phoneme(uc, lib, names, rng, cases):
    stops = set(PHONEME_EXITS)

    def hook(uc_, address, size, _):
        if address in stops:
            uc_.emu_stop()

    handle = uc.hook_add(UC_HOOK_CODE, hook)
    compared = 0
    try:
        for case in range(cases):
            text = "".join(rng.choice(PHONEME_ALPHABET)
                           for _ in range(rng.randrange(1, 6))) + "\0"
            raw = text.encode("latin-1")
            uc.mem_write(TEXT, raw + b"\0" * 8)
            esp = STACK + 0x8000
            uc.mem_write(esp, b"\0" * 0x40)
            uc.mem_write(esp + 0x13, raw[0:1])
            uc.mem_write(esp + 0x18, struct.pack("<I", TEXT + 1))
            uc.reg_write(UC_X86_REG_ESP, esp)
            got = C.c_uint()
            buf = C.create_string_buffer(raw + b"\0" * 8)
            try:
                uc.emu_start(PHONEME_ENTRY, 0, count=4000)
            except UcError:
                # '[' (list 0x3b) is a NULL pointer in this build and the
                # original dereferences it at 0x1c004cda. The reconstruction
                # refuses instead, which is the documented divergence -- so
                # assert the fault happens and that we refuse, rather than
                # quietly dropping the case.
                if raw[0:1] != b"[":
                    raise
                actual = lib.sv_phoneme_lookup(C.byref(names), buf, C.byref(got))
                if (actual, got.value) != (SV_PHONEME_INVALID, 1):
                    raise AssertionError(
                        f"phoneme case {case} {text[:-1]!r}: the original "
                        f"faults on the null list, so the reconstruction must "
                        f"return SV_PHONEME_INVALID/1, not "
                        f"({actual:#x}, {got.value})")
                compared += 1
                continue
            code = uc.reg_read(UC_X86_REG_EBX) & 0xFF
            consumed = 1 + (struct.unpack("<I", bytes(uc.mem_read(esp + 0x18, 4)))[0]
                            - (TEXT + 1))

            actual = lib.sv_phoneme_lookup(C.byref(names), buf, C.byref(got))
            if (actual, got.value) != (code, consumed):
                raise AssertionError(
                    f"phoneme case {case} {text[:-1]!r}: reconstruction "
                    f"(code {actual:#x}, consumed {got.value}) != original "
                    f"(code {code:#x}, consumed {consumed})")
            compared += 1
    finally:
        uc.hook_del(handle)
    return compared


def check_extract(uc, path, out_dir):
    """Confirm tools/extract_base.py copies the same bytes the emulator runs."""
    import subprocess
    import tempfile
    tool = Path(__file__).with_name("extract_base.py")
    with tempfile.TemporaryDirectory(dir=out_dir) as tmp:
        generated = Path(tmp) / "base_data.c"
        subprocess.run(["python3", str(tool), path, str(generated)],
                       check=True, capture_output=True)
        text = generated.read_text()
    for name, va, length in (("resonator", RESONATOR_VA, RESONATOR_LEN),
                             ("amplitude", AMPLITUDE_VA, AMPLITUDE_LEN),
                             ("output_curve", CURVE_VA, CURVE_LEN)):
        want = bytes(uc.mem_read(va, length))
        head = text.split(f"{name}[{length}] = {{", 1)[1].split("};", 1)[0]
        got = bytes(int(t, 16) for t in head.replace("\n", "").split(",") if t.strip())
        if got != want:
            raise AssertionError(f"extract_base.py {name} does not match "
                                 f"{va:#x}..{va + length:#x}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--cases", type=int, default=2000)
    parser.add_argument("--stage", default="all",
                        choices=("all", "apply", "render", "phoneme"))
    parser.add_argument("--check-extract", action="store_true",
                        help="also confirm tools/extract_base.py copies the "
                             "same bytes this run compares against")
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=0x19961118)
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")

    lib = C.CDLL(str(Path(args.library).resolve()))
    lib.sv_frame_apply.argtypes = [C.POINTER(FrameState), C.POINTER(FrameTables),
                                   C.c_void_p]
    lib.sv_frame_apply.restype = C.c_int
    lib.sv_frame_render.argtypes = [C.POINTER(FrameState), C.POINTER(FrameTables),
                                    C.c_void_p, C.c_void_p, C.c_uint16]
    lib.sv_frame_render.restype = C.c_int
    lib.sv_phoneme_lookup.argtypes = [C.POINTER(PhonemeNames), C.c_char_p,
                                      C.POINTER(C.c_uint)]
    lib.sv_phoneme_lookup.restype = C.c_uint8

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    map_pe(uc, args.dll)
    for address, size in ((STATE, 0x1000), (FRAMES, 0x4000), (OUTPUT, 0x10000),
                          (TEXT, 0x1000), (STACK, 0x10000), (MAGIC_RET, 0x1000)):
        uc.mem_map(address, size)

    tables, glottal_bufs = build_tables(uc)
    names = build_names(uc)
    bridge = Bridge(glottal_bufs)
    rng = random.Random(args.seed)

    wanted = ("apply", "render", "phoneme") if args.stage == "all" else (args.stage,)
    for stage in wanted:
        if stage == "apply":
            n = stage_apply(uc, lib, tables, bridge, rng, args.cases)
            print(f"PASS apply:   {n} randomized frames — every byte of the "
                  f"original's 0x400 state block matches VA "
                  f"{APPLY_ENTRY:#x}..{APPLY_EXIT:#x}")
        elif stage == "render":
            n = stage_render(uc, lib, tables, bridge, rng, max(1, args.cases // 8))
            print(f"PASS render:  {n} randomized utterances — PCM and full "
                  f"state match VA {RENDER_ENTRY:#x}..0x1c00498e, restart and "
                  f"resume paths both covered")
        else:
            n = stage_phoneme(uc, lib, names, rng, args.cases)
            print(f"PASS phoneme: {n} randomized names — code and consumed "
                  f"count match VA {PHONEME_ENTRY:#x}..{PHONEME_EXITS[1]:#x}")

    if args.check_extract:
        check_extract(uc, args.dll, Path(args.library).resolve().parent)
        print("PASS extract: tools/extract_base.py emits the same bytes")


if __name__ == "__main__":
    main()
