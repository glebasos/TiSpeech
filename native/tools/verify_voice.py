#!/usr/bin/env python3
"""Compare the public voice-options ABI with SoftVoice's original setters.

Uses the same original-engine emulator as verify_narrate.py. Checks all twenty
personality rows, alternate phoneme tables, and explicit parameter overrides.
The application never imports this development-only tool.
"""
import argparse
import ctypes as C
from sv_emu import Emu


class Options(C.Structure):
    _fields_ = [(name, C.c_int32) for name in (
        "personality", "pitch", "rate", "voicing", "f0_style", "f0_range",
        "f0_perturb", "vowel_factor", "glottal_source")]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dlls", required=True)
    ap.add_argument("--library", required=True)
    a = ap.parse_args()
    lib = C.CDLL(a.library)
    synth = lib.tispeech_synthesize_ex
    synth.argtypes = [C.c_uint32, C.c_char_p, C.POINTER(Options),
                      C.POINTER(C.c_void_p), C.POINTER(C.c_int32), C.POINTER(C.c_int32)]
    lib.tispeech_free_samples.argtypes = [C.c_void_p]
    cases = [(i,) + (-1,) * 8 for i in range(20)] + [
        (0, 200, 250, 1, 2, 30, 10, 120, 2),
        (1, 350, 130, 2, 4, -1, -1, -1, -1),
    ]
    setters = ["Personality", "Pitch", "Rate", "VoicingMode", "F0Style",
               "F0Range", "F0Perturb", "VowelFactor", "GlottalSource"]
    failures = 0
    for values in cases:
        emu = Emu(a.dlls)
        emu.open()
        for name, value in zip(setters, values):
            if value >= 0:
                rc = emu.call(emu.exp("_SVSet" + name + "@8"), [emu.h, value])
                if rc:
                    raise RuntimeError("original setter %s returned %#x" % (name, rc))
        rc, phon = emu.text_to_phon(b"Hello world.")
        if rc:
            raise RuntimeError("original TextToPhon returned %#x" % rc)
        expected = emu.narrate(phon)
        ptr, count, rate = C.c_void_p(), C.c_int32(), C.c_int32()
        rc = synth(1, phon, C.byref(Options(*values)), C.byref(ptr), C.byref(count), C.byref(rate))
        try:
            actual = C.string_at(ptr, count.value) if ptr.value else b""
            if rc or rate.value != 11025 or actual != expected:
                failures += 1
                print("FAIL %r: rc=%#x, rate=%d, samples=%d vs %d" %
                      (values, rc, rate.value, len(actual), len(expected)))
        finally:
            lib.tispeech_free_samples(ptr)
    print("%s: %d/%d voice settings produce sample-identical PCM" %
          ("FAIL" if failures else "PASS", len(cases) - failures, len(cases)))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
