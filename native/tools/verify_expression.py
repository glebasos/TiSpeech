#!/usr/bin/env python3
"""Development-only differential test of the reconstructed voice-expression pass.

Same pattern as tools/verify_dsp.py, verify_frames.py and verify_smoothing.py:
map the real TIBASE32.DLL under Unicorn, run the ORIGINAL x86 code at VA
0x1c00be40..0x1c00c2b5, call the C reconstruction (src/expression.c,
sv_expression_apply()) through ctypes on a copy of the same input, and compare
the whole frame array AND every byte of the 0x400-byte state block.

This pass has a much wider input surface than the pitch passes, so the
generator below builds a whole scenario per case: a phoneme record array, a
command list per record (sometimes absent, sometimes empty), a frame array with
random pitch, amplitudes and bandwidth nibbles, and a randomized engine state.
Coverage of the nine commands and of the glottal-source classes is counted and
asserted at the end, so a generator that quietly stopped exercising one of them
fails the run rather than passing a narrower test.

The tables come out of the EMULATOR IMAGE rather than from extract_base.py, so
a matching mistake in the extractor cannot cancel out against one here.

Requires `pip install unicorn==2.1.4` in a development venv. Nothing here is
linked into or required by the native library or the application.

Build a test library, for example:
  cc -shared -fPIC -std=c11 -Iinclude src/expression.c -o build/libexpression.dylib
  python tools/verify_expression.py --dll /path/TIBASE32.DLL \\
         --library build/libexpression.dylib --cases 5000
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
RECORD_SIZE = 0x1A
COMMAND_SIZE = 6

ENTRY = 0x1C00BE40
# sub esp,0x24; mov dword [esp+0x18],0x1c001470 -- guards against running a
# different build's code.
ENTRY_SIGNATURE = bytes.fromhex("83ec24c74424187014001c")

# Tables, with the bounds derived in include/tispeech/expression.h.
FLUTTER_VA, FLUTTER_BYTES = 0x1C001470, 0x100
LFO_VA, LFO_BYTES = 0x1C013430, 0x100
VOICES_VA, VOICE_STRIDE, VOICE_ROWS = 0x1C013600, 74, 20
SOURCE_MAP_VA, SOURCE_MAP_PITCHES, SOURCE_MAP_STRIDE = 0x1C012EE8, 0x10E, 5
SOURCE_MAP_BYTES = SOURCE_MAP_PITCHES * SOURCE_MAP_STRIDE

CMD_VOICE, CMD_VIBRATO, CMD_TREMOLO, CMD_SOURCE_MOD = 0x32, 0xA0, 0xA2, 0xA4
CMD_VIBRATO_RATE, CMD_PITCH, CMD_RATE = 0xAA, 0xAC, 0xAE
CMD_SOURCE_CLASS, CMD_FLUTTER, CMD_END = 0xD2, 0x104, 0x1E
COMMANDS = (CMD_VOICE, CMD_VIBRATO, CMD_TREMOLO, CMD_SOURCE_MOD,
            CMD_VIBRATO_RATE, CMD_PITCH, CMD_RATE, CMD_SOURCE_CLASS,
            CMD_FLUTTER)

# State-block offsets the pass touches (include/tispeech/expression.h).
OFF_RECORDS = 0x1C
OFF_PITCH_DIVISOR = 0x48
OFF_SOURCE_LOCK = 0x52
OFF_SOURCE_CLASS = 0x5A
OFF_FLUTTER_DEPTH = 0x62
OFF_VIBRATO_RATE = 0x68
OFF_PITCH_SCALED = 0x6A
OFF_GLIDE_RATE = 0x6C
OFF_GLIDE_RISING = 0x6E
OFF_PITCH_PREVIOUS = 0x72
OFF_GLIDE = 0x74
OFF_VIBRATO_DEPTH = 0x78
OFF_TREMOLO_DEPTH = 0x7A
OFF_SOURCE_DEPTH = 0x7C
OFF_FRAMES_PTR = 0xF6

STATE_SIZE = 0x400

# --- guest memory ------------------------------------------------------------
STATE, FRAMES, RECORDS, CMDS, STACK, MAGIC_RET = (
    0x00100000, 0x00110000, 0x00130000, 0x00140000, 0x00300000, 0x00400000)
FRAMES_CAP = 0x10000
MAX_FRAMES = 400          # per case; keeps the emulated instruction count sane
MAX_RECORDS = 8


class State:
    """The subset of the engine state block this pass reads or writes, in both
    representations: guest bytes at their original offsets, and the C struct."""

    FIELDS = (
        ("pitch_divisor", OFF_PITCH_DIVISOR, "<H", C.c_uint16),
        ("source_lock", OFF_SOURCE_LOCK, "<H", C.c_uint16),
        ("source_class", OFF_SOURCE_CLASS, "<H", C.c_uint16),
        ("flutter_depth", OFF_FLUTTER_DEPTH, "<H", C.c_uint16),
        ("vibrato_rate", OFF_VIBRATO_RATE, "<h", C.c_int16),
        ("pitch_scaled", OFF_PITCH_SCALED, "<h", C.c_int16),
        ("glide_rate", OFF_GLIDE_RATE, "<h", C.c_int16),
        ("glide_rising", OFF_GLIDE_RISING, "<i", C.c_int32),
        ("pitch_previous", OFF_PITCH_PREVIOUS, "<h", C.c_int16),
        ("glide", OFF_GLIDE, "<i", C.c_int32),
        ("vibrato_depth", OFF_VIBRATO_DEPTH, "<h", C.c_int16),
        ("tremolo_depth", OFF_TREMOLO_DEPTH, "<h", C.c_int16),
        ("source_depth", OFF_SOURCE_DEPTH, "<h", C.c_int16),
    )

    def __init__(self, values):
        self.values = dict(values)

    def to_guest(self, filler):
        """The 0x400-byte block, over a randomized background so that a byte the
        original writes outside the modelled fields shows up as a difference."""
        block = bytearray(filler)
        struct.pack_into("<I", block, OFF_RECORDS, RECORDS)
        struct.pack_into("<I", block, OFF_FRAMES_PTR, FRAMES)
        for name, offset, fmt, _ in self.FIELDS:
            struct.pack_into(fmt, block, offset, self.values[name])
        return bytes(block)

    def to_struct(self):
        st = CState()
        for name, _, _, _ in self.FIELDS:
            setattr(st, name, self.values[name])
        return st


class CState(C.Structure):
    _fields_ = [(name, ctype) for name, _, _, ctype in State.FIELDS]


class CRecord(C.Structure):
    _fields_ = [("commands", C.POINTER(C.c_uint8)),
                ("code", C.c_uint16),
                ("duration", C.c_uint16)]


class CTables(C.Structure):
    _fields_ = [("flutter", C.POINTER(C.c_uint8)),
                ("lfo", C.POINTER(C.c_uint8)),
                ("voices", C.POINTER(C.c_uint8)),
                ("source_map", C.POINTER(C.c_uint8))]


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
    got = bytes(uc.mem_read(ENTRY, len(ENTRY_SIGNATURE)))
    if got != ENTRY_SIGNATURE:
        raise ValueError(
            f"unsupported TIBASE32 build: expected {ENTRY_SIGNATURE.hex()} "
            f"at {ENTRY:#x}, got {got.hex()}")


def read_tables(uc):
    """Straight out of the mapped image, so the extractor is not in the loop."""
    return {
        "flutter": bytes(uc.mem_read(FLUTTER_VA, FLUTTER_BYTES)),
        "lfo": bytes(uc.mem_read(LFO_VA, LFO_BYTES)),
        "voices": bytes(uc.mem_read(VOICES_VA, VOICE_STRIDE * VOICE_ROWS)),
        "source_map": bytes(uc.mem_read(SOURCE_MAP_VA, SOURCE_MAP_BYTES)),
    }


# --- case generation ---------------------------------------------------------

def random_state(rng):
    """Engine state. The two divisors are kept nonzero because zero faults in
    the original -- probe_faults() covers those separately."""
    return State({
        "pitch_divisor": rng.choice((0x2000, 0x4000, 0x8000,
                                     rng.randrange(1, 0x10000))),
        "source_lock": rng.choice((0, 0, 0, rng.randrange(0, 0x10000))),
        "source_class": rng.choice(tuple(range(0, 10)) + (rng.randrange(0, 0x10000),)),
        "flutter_depth": rng.choice((0, 10, 15, 20, 30, 90, 120,
                                     rng.randrange(0, 0x10000))),
        "vibrato_rate": rng.choice((0, 5, 20, 60, rng.randrange(-0x8000, 0x8000))),
        "pitch_scaled": rng.choice((0, rng.randrange(-0x8000, 0x8000))),
        "glide_rate": rng.choice((1, 10, 100, 1000,
                                  rng.choice((-1, 1)) * rng.randrange(1, 0x8000))),
        "glide_rising": rng.choice((0, 1, 1, rng.randrange(-0x80000000, 0x80000000))),
        "pitch_previous": rng.randrange(-0x8000, 0x8000),
        "glide": rng.choice((0, 0x1000, -0x1000,
                             rng.randrange(-0x80000000, 0x80000000))),
        "vibrato_depth": rng.choice((0, 0, rng.randrange(-0x8000, 0x8000))),
        "tremolo_depth": rng.choice((0, 0, rng.randrange(-0x8000, 0x8000))),
        "source_depth": rng.choice((0, 0, rng.randrange(-0x8000, 0x8000))),
    })


def random_command_value(rng, code):
    if code == CMD_VOICE:
        return rng.randrange(0, VOICE_ROWS)
    if code == CMD_RATE:
        # Zero faults in the original; probe_faults() covers it.
        value = 0
        while value == 0:
            value = rng.randrange(-0x8000, 0x8000)
        return value
    if code == CMD_SOURCE_CLASS:
        return rng.choice(tuple(range(0, 10)) + (rng.randrange(0, 0x10000),))
    if code == CMD_PITCH:
        return rng.choice((50, 100, 150, 200, rng.randrange(-0x8000, 0x8000)))
    return rng.randrange(-0x8000, 0x8000)


def random_commands(rng, used, allowed=COMMANDS):
    """A command list, or None. Ignored codes are mixed in so the dispatch
    table's 202 no-op entries are exercised alongside the nine live ones.
    `allowed` exists for the parked-glide cohort, which must not let a pitch
    command move the target it has deliberately pinned at zero."""
    if rng.random() < 0.2:
        return None
    entries = []
    for _ in range(rng.randrange(0, 5)):
        if rng.random() < 0.25:
            code = rng.choice((0x33, 0x40, 0x99, 0xA1, 0x103, 0x11, 0x200, -5))
            value = rng.randrange(-0x8000, 0x8000)
        else:
            code = rng.choice(allowed)
            value = random_command_value(rng, code)
            used.add(code)
        entries.append((code, value))
    raw = bytearray()
    for code, value in entries:
        # The third word is the two bytes at +0x04 that this pass never reads;
        # randomizing them is part of the claim that it never reads them.
        raw += struct.pack("<hhH", _i16(code), _i16(value),
                           rng.randrange(0, 0x10000))
    raw += struct.pack("<hhH", CMD_END, 0, 0)
    return bytes(raw)


def _i16(value):
    value &= 0xFFFF
    return value - 0x10000 if value >= 0x8000 else value


def random_frame(rng):
    frame = bytearray(rng.randrange(0, 256) for _ in range(FRAME_SIZE))
    # A pitch drawn from the whole range would almost never land in the
    # source map's 0..0x10d window, which is where the interesting path is.
    pitch = rng.choice((rng.randrange(0, SOURCE_MAP_PITCHES),
                        rng.randrange(0, 0x10000),
                        rng.randrange(0x8000, 0x10000)))
    struct.pack_into("<H", frame, 0x18, pitch)
    return frame


def random_case(rng, used_commands, allowed=COMMANDS):
    """A record array (the first entry is skipped by the pass), its command
    lists, and enough frames to cover every record."""
    count = rng.randrange(1, MAX_RECORDS)
    records = []
    frames_needed = 0
    for _ in range(count):
        duration = rng.choice((0, 1, 4, 8, 12, 40, rng.randrange(0, 400)))
        frames_needed += (duration + 4) >> 3
        if frames_needed > MAX_FRAMES:
            frames_needed -= (duration + 4) >> 3
            break
        records.append({"commands": random_commands(rng, used_commands, allowed),
                        "code": rng.randrange(0, 0xFF),
                        "duration": duration})
    # The leading record the pass steps over, and the 0x00ff terminator.
    records.insert(0, {"commands": random_commands(rng, set(), allowed),
                       "code": rng.randrange(0, 0xFF),
                       "duration": rng.randrange(0, 400)})
    records.append({"commands": None, "code": 0xFF, "duration": 0})

    frame_count = max(frames_needed, 1)
    frames = bytearray()
    for _ in range(frame_count):
        frames += random_frame(rng)
    return records, frames, frame_count


# --- the two runs ------------------------------------------------------------

def run_original(uc, rng, state, filler, records, frames, frame_count):
    uc.mem_write(FRAMES, bytes(frames))
    uc.mem_write(STATE, state.to_guest(filler))

    # Command lists first: each record's +0x00 is a guest pointer to one.
    blob = bytearray()
    pointers = []
    for record in records:
        if record["commands"] is None:
            pointers.append(0)
        else:
            pointers.append(CMDS + len(blob))
            blob += record["commands"]
    uc.mem_write(CMDS, bytes(blob) if blob else b"\0")

    raw = bytearray()
    for record, pointer in zip(records, pointers):
        # Every byte of the record except +0x00, +0x0c and +0x0e is filled with
        # noise: the reconstruction is never shown them, so if the original
        # read one the frame comparison would diverge.
        entry = bytearray(rng.randrange(0, 256) for _ in range(RECORD_SIZE))
        struct.pack_into("<I", entry, 0x00, pointer)
        struct.pack_into("<H", entry, 0x0C, record["code"])
        struct.pack_into("<H", entry, 0x0E, record["duration"])
        raw += entry
    uc.mem_write(RECORDS, bytes(raw))

    esp = STACK + 0x8000
    uc.mem_write(esp, struct.pack("<II", MAGIC_RET, STATE))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.emu_start(ENTRY, MAGIC_RET, count=20_000_000)
    return (bytes(uc.mem_read(FRAMES, frame_count * FRAME_SIZE)),
            bytes(uc.mem_read(STATE, STATE_SIZE)))


def run_reconstruction(lib, state, records, frames, frame_count, tables):
    mirror = (C.c_uint8 * len(frames)).from_buffer_copy(bytes(frames))
    st = state.to_struct()

    buffers = []
    array = (CRecord * len(records))()
    for i, record in enumerate(records):
        if record["commands"] is None:
            array[i].commands = None
        else:
            buf = (C.c_uint8 * len(record["commands"])).from_buffer_copy(
                record["commands"])
            buffers.append(buf)
            array[i].commands = C.cast(buf, C.POINTER(C.c_uint8))
        array[i].code = record["code"]
        array[i].duration = record["duration"]

    status = lib.sv_expression_apply(C.byref(st), C.cast(mirror, C.c_void_p),
                                     frame_count, array, C.byref(tables))
    return status, bytes(mirror), st


def c_tables(tables):
    keep = {}
    holder = CTables()
    for name in ("flutter", "lfo", "voices", "source_map"):
        buf = (C.c_uint8 * len(tables[name])).from_buffer_copy(tables[name])
        keep[name] = buf
        setattr(holder, name, C.cast(buf, C.POINTER(C.c_uint8)))
    holder._keep = keep
    return holder


# --- comparison --------------------------------------------------------------

def diff_frames(before, expected, actual, count):
    for i in range(count):
        off = i * FRAME_SIZE
        e, a = expected[off:off + FRAME_SIZE], actual[off:off + FRAME_SIZE]
        if e != a:
            b = before[off:off + FRAME_SIZE]
            return (f"frame {i}/{count} differs\n"
                    f"  before   {b.hex()}\n"
                    f"  original {e.hex()}\n"
                    f"  recon    {a.hex()}")
    return None


def diff_state(state, filler, expected, actual_struct):
    """Rebuild the block from the reconstruction's struct and compare all
    0x400 bytes, so a field the original writes that we do not model shows up
    as a difference rather than hiding outside the comparison."""
    rebuilt = bytearray(filler)
    struct.pack_into("<I", rebuilt, OFF_RECORDS, RECORDS)
    struct.pack_into("<I", rebuilt, OFF_FRAMES_PTR, FRAMES)
    for name, offset, fmt, _ in State.FIELDS:
        struct.pack_into(fmt, rebuilt, offset, getattr(actual_struct, name))
    if bytes(rebuilt) == expected:
        return None
    for i, (want, got) in enumerate(zip(rebuilt, expected)):
        if want != got:
            named = [n for n, o, _, _ in State.FIELDS if o <= i < o + 4]
            return (f"state byte {i:#x} differs: reconstruction {want:#04x}, "
                    f"original {got:#04x}"
                    + (f" (within {named[0]})" if named else
                       " -- OUTSIDE every modelled field"))
    return "state blocks differ in length"


# --- fault probes ------------------------------------------------------------

def probe_faults(uc, rng):
    """The two divisors the reconstruction refuses: confirm the original really
    does fault, rather than taking it on trust that refusing is the divergence.
    """
    faults = 0
    for field, offset in (("glide_rate", OFF_GLIDE_RATE),
                          ("pitch_divisor", OFF_PITCH_DIVISOR)):
        state = random_state(rng)
        state.values[field] = 0
        records = [{"commands": None, "code": 1, "duration": 0},
                   {"commands": None, "code": 2, "duration": 40},
                   {"commands": None, "code": 0xFF, "duration": 0}]
        frames = bytearray()
        for _ in range(16):
            frames += random_frame(rng)
        try:
            run_original(uc, rng, state, bytes(STATE_SIZE), records, frames, 16)
        except UcError:
            faults += 1
            continue
        raise AssertionError(
            f"the original did NOT fault with {field} == 0, so refusing it in "
            f"sv_expression_apply() is a divergence with no justification")
    return faults


# --- main --------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--cases", type=int, default=5000)
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=0x19961118)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")

    lib = C.CDLL(str(Path(args.library).resolve()))
    lib.sv_expression_apply.restype = C.c_int
    lib.sv_expression_apply.argtypes = [C.POINTER(CState), C.c_void_p,
                                        C.c_size_t, C.POINTER(CRecord),
                                        C.POINTER(CTables)]

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    map_pe(uc, args.dll)
    for address, size in ((STATE, 0x1000), (FRAMES, FRAMES_CAP),
                          (RECORDS, 0x1000), (CMDS, 0x1000),
                          (STACK, 0x10000), (MAGIC_RET, 0x1000)):
        uc.mem_map(address, size)

    tables = read_tables(uc)
    holder = c_tables(tables)

    rng = random.Random(args.seed)
    faults = probe_faults(uc, rng)

    used_commands = set()
    source_classes = {}
    frames_compared = 0
    modulated_cases = 0
    parked_cases = 0

    # Every seventh case parks the glide at zero with the three depths set.
    # Without it the `glide != 0` gate on the three modulations is never
    # observed: removing it from the reconstruction survived 200 ordinary
    # cases, because a randomized glide is essentially never zero by the time
    # the gate is reached. A glide_target of zero makes it zero on the first
    # frame, whichever way the glide runs, since both branches clamp.
    PARKED = tuple(c for c in COMMANDS if c != CMD_PITCH)

    for case in range(args.cases):
        parked = (case % 7 == 0)
        records, frames, frame_count = random_case(
            rng, used_commands, PARKED if parked else COMMANDS)
        state = random_state(rng)
        if parked:
            state.values["pitch_scaled"] = 0
            for field in ("vibrato_depth", "tremolo_depth", "source_depth"):
                if state.values[field] == 0:
                    state.values[field] = rng.randrange(1, 0x8000)
            parked_cases += 1
        filler = bytes(rng.randrange(0, 256) for _ in range(STATE_SIZE))

        expected_frames, expected_state = run_original(
            uc, rng, state, filler, records, frames, frame_count)
        status, actual_frames, actual_state = run_reconstruction(
            lib, state, records, frames, frame_count, holder)

        if status != 0:
            raise AssertionError(
                f"case {case}: the reconstruction refused with {status} on an "
                f"input the original accepted")

        mismatch = diff_frames(frames, expected_frames, actual_frames, frame_count)
        if mismatch:
            raise AssertionError(f"case {case}: {mismatch}")
        mismatch = diff_state(state, filler, expected_state, actual_state)
        if mismatch:
            raise AssertionError(f"case {case}: {mismatch}")

        frames_compared += frame_count
        cls = state.values["source_class"]
        source_classes[cls if cls <= 8 else "over"] = \
            source_classes.get(cls if cls <= 8 else "over", 0) + 1
        if expected_frames != bytes(frames):
            modulated_cases += 1

    missing = set(COMMANDS) - used_commands
    if missing:
        raise AssertionError(
            "these commands were never generated, so the run proves nothing "
            "about them: " + ", ".join(hex(c) for c in sorted(missing)))
    if len(source_classes) < 9:
        raise AssertionError(
            f"only {len(source_classes)} glottal-source classes were reached")
    if modulated_cases == 0:
        raise AssertionError("no case changed a frame -- the run is vacuous")
    if parked_cases == 0:
        raise AssertionError(
            "no parked-glide case ran, so the gate on the three modulations "
            "is unproven -- see the comment on PARKED above")

    print(f"PASS: {args.cases} randomized scenarios ({frames_compared} frames) "
          f"match TIBASE32 {ENTRY:#x}..0x1c00c2b5 byte-for-byte, frame array "
          f"and whole {STATE_SIZE:#x}-byte state block")
    print(f"      all nine commands exercised; source classes reached: "
          f"{len(source_classes)}")
    print(f"      cases where the pass changed a frame: {modulated_cases}")
    print(f"      cases with the glide parked at zero (gate exercised): "
          f"{parked_cases}")
    print(f"      divisor faults confirmed in the original: {faults}/2")
    if args.verbose:
        for key in sorted(source_classes, key=str):
            print(f"        class {key}: {source_classes[key]} cases")


if __name__ == "__main__":
    try:
        main()
    except UcError as exc:
        raise SystemExit(f"unicorn error: {exc}")
