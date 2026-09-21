#!/usr/bin/env python3
"""Development-only differential test of the reconstructed waveform kernel.

Requires `pip install unicorn==2.1.4` in a development venv. Nothing here is
linked into or required by the native library/application. Runs only the
original integer sample loop (VA 1c004120..1c004493), not a Windows TTS host.

Build a test library, for example:
  cc -shared -fPIC -std=c11 -Iinclude src/dsp.c -o build/libdsp.dylib
  python tools/verify_dsp.py --dll /path/TIBASE32.DLL --library build/libdsp.dylib
"""
import argparse
import ctypes as C
import random
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESP


class Ramp(C.Structure):
    _fields_ = [("value", C.c_uint32), ("step", C.c_uint32)]


class Filter(C.Structure):
    _fields_ = [("previous", C.c_int16), ("previous2", C.c_int16),
                ("a", Ramp), ("b", Ramp)]


class State(C.Structure):
    _fields_ = [("phase", C.c_uint32), ("phase_step", C.c_uint32),
                ("mix_a", C.c_int16), ("mix_b", C.c_int16), ("voicing", Ramp),
                ("previous_voice", C.c_int16),
                ("aspiration_negative", C.c_int16), ("aspiration_positive", C.c_int16),
                ("frication_negative", C.c_int16), ("frication_positive", C.c_int16),
                ("frication_shape", C.c_int16), ("filters", Filter * 7)]


class Tables(C.Structure):
    _fields_ = [("source_a", C.POINTER(C.c_int16)),
                ("source_b", C.POINTER(C.c_int16)),
                ("output_curve", C.POINTER(C.c_uint8))]


FILTER_OFFSETS = (0x23e, 0x202, 0x1b2, 0x1da, 0x1ee, 0x1c6, 0x216)
STATE = 0x100000
SOURCE_A, SOURCE_B, NOISE, OUTPUT, STACK = (0x200000, 0x201000, 0x202000, 0x203000, 0x300000)


def map_pe(uc, path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    image_base = struct.unpack_from("<I", data, optional + 28)[0]
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    if machine != 0x14c or image_base != 0x1c000000:
        raise ValueError("expected the supplied i386 TIBASE32 build")
    uc.mem_map(image_base, (image_size + 4095) & ~4095)
    for i in range(count):
        section = optional + optional_size + 40 * i
        rva, raw_size, raw_offset = struct.unpack_from("<III", data, section + 12)
        uc.mem_write(image_base + rva, data[raw_offset:raw_offset + raw_size])
    # Reject a different build instead of accidentally running another function.
    if bytes(uc.mem_read(0x1c004120, 6)) != bytes.fromhex("8b93fc020000"):
        raise ValueError("unsupported TIBASE32 sample-loop signature")


def encode_state(s):
    raw = bytearray(0x400)

    def u16(offset, value):
        struct.pack_into("<H", raw, offset, value & 0xffff)

    def u32(offset, value):
        struct.pack_into("<I", raw, offset, value & 0xffffffff)

    for offset, value in ((0x2fc, s.phase), (0x300, s.phase_step),
                          (0x276, s.voicing.value), (0x27a, s.voicing.step),
                          (0x2aa, (s.previous_voice & 0xffff) << 16),
                          (0x2d8, SOURCE_A), (0x2dc, SOURCE_B), (0x2e4, NOISE)):
        u32(offset, value)
    for offset, value in ((0x268, s.mix_a), (0x270, s.mix_b),
                          (0x298, s.aspiration_negative), (0x29c, s.aspiration_positive),
                          (0x2a0, s.frication_negative), (0x2a4, s.frication_positive),
                          (0x290, s.frication_shape)):
        u16(offset, value)
    for offset, f in zip(FILTER_OFFSETS, s.filters):
        u16(offset, f.previous)
        u16(offset + 2, f.previous2)
        u32(offset + 4, f.a.value)
        u32(offset + 8, f.a.step)
        u32(offset + 12, f.b.value)
        u32(offset + 16, f.b.step)
    return bytes(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--cases", type=int, default=10000)
    parser.add_argument("--audit-state", action="store_true",
                        help="also report every byte of the 0x400 state block "
                             "the original writes that our model does not "
                             "reproduce, instead of only the known spans")
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")
    lib = C.CDLL(str(Path(args.library).resolve()))
    lib.sv_dsp_sample.argtypes = [C.POINTER(State), C.POINTER(Tables), C.c_int16, C.c_int16]
    lib.sv_dsp_sample.restype = C.c_uint8
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    map_pe(uc, args.dll)
    for address, size in ((STATE, 0x1000), (SOURCE_A, 0x4000), (STACK, 0x10000)):
        uc.mem_map(address, size)
    rng = random.Random(0x19961118)
    sample_array = C.c_int16 * 1024
    source_a = sample_array(*(rng.randrange(-32768, 32768) for _ in range(1024)))
    source_b = sample_array(*(rng.randrange(-32768, 32768) for _ in range(1024)))
    uc.mem_write(SOURCE_A, bytes(source_a))
    uc.mem_write(SOURCE_B, bytes(source_b))
    curve = (C.c_uint8 * 401).from_buffer_copy(bytes(uc.mem_read(0x1c0010fe, 401)))
    tables = Tables(source_a, source_b, curve)
    compared = 0
    unmodelled = {}
    for case in range(args.cases):
        s = State.from_buffer_copy(bytes(rng.randrange(256) for _ in range(C.sizeof(State))))
        noise_a, noise_b = (rng.randrange(-32768, 32768) for _ in range(2))
        initial = encode_state(s)
        uc.mem_write(STATE, initial)
        uc.mem_write(NOISE, struct.pack("<hh", noise_a, noise_b))
        uc.reg_write(UC_X86_REG_EBX, STATE)
        uc.reg_write(UC_X86_REG_EDI, OUTPUT)
        uc.reg_write(UC_X86_REG_ESP, STACK + 0x8000)
        uc.emu_start(0x1c004120, 0x1c004493, count=1000)
        expected_sample = uc.mem_read(OUTPUT, 1)[0]
        actual_sample = lib.sv_dsp_sample(C.byref(s), C.byref(tables), noise_a, noise_b)
        if expected_sample != actual_sample:
            raise AssertionError(f"case {case}: PCM {actual_sample} != original {expected_sample}")
        original = bytes(uc.mem_read(STATE, 0x400))
        rebuilt = encode_state(s)
        # Compare all translated state, not only output (clipping can hide errors).
        spans = [(0x2fc, 4), (0x276, 4), (0x2aa, 4)]
        spans += [(offset, 20) for offset in FILTER_OFFSETS]
        for offset, length in spans:
            if original[offset:offset + length] != rebuilt[offset:offset + length]:
                raise AssertionError(f"case {case}: state mismatch at {offset:#x}")
        if args.audit_state:
            # The spans above cover the state we chose to model. Anything the
            # original writes OUTSIDE them is state we are silently dropping —
            # harmless for a single sample, potentially not across a frame.
            # Comparing the whole block is the only way to see it, because our
            # rebuilt image leaves unmodelled bytes at their initial value.
            covered = set()
            for offset, length in spans:
                covered.update(range(offset, offset + length))
            for i in range(0x400):
                if i in covered or original[i] == rebuilt[i]:
                    continue
                if original[i] != initial[i]:
                    unmodelled.setdefault(i, 0)
                    unmodelled[i] += 1
        compared += 1
    print(f"PASS: {compared} randomized native samples and filter states match the original x86 loop")
    if args.audit_state:
        if unmodelled:
            print(f"AUDIT: the original also writes {len(unmodelled)} byte(s) of "
                  f"state this reconstruction does not model:")
            for offset in sorted(unmodelled):
                print(f"  {offset:#05x}  changed in {unmodelled[offset]}/{compared} cases")
            print("These are unmodelled, not wrong: the sample output and every "
                  "modelled field still match. They matter once frames are "
                  "driven in sequence — see REVERSING.md.")
        else:
            print(f"AUDIT: the original writes no state outside the modelled "
                  f"spans across {compared} cases")


if __name__ == "__main__":
    main()
