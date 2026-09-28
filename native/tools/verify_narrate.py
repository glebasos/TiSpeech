#!/usr/bin/env python3
"""
verify_narrate.py — stage-by-stage differential of the narrate pipeline.

Runs the ORIGINAL engine end to end under Unicorn (tools/sv_emu.py): text ->
SVTextToPhon -> SVNarrate, snapshotting the engine at every stage boundary
inside FUN_1c003870. Then runs the reconstruction (src/narrate.c and the
language modules, through tools/narrate_oracle.c) on the SAME phoneme string,
stopping at the same boundaries, and compares records, intonation arrays and
frames. The first stage that differs is where the bug is.

One tool for the whole middle of the pipeline, instead of a verifier per
function: every stage is checked on the inputs the previous stage of the
original actually produced for real text.

USAGE
    verify_narrate.py --dlls DIR --library build/libnarrate_oracle.dylib \\
        [--text "..."]... [--words N] [--stage NAME] [-v]
"""
import argparse
import ctypes
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sv_emu import Emu  # noqa: E402

# name, C stop address (the function), emulator address just after its call
STAGES = [
    ("parse",      0x1c004a10, 0x1c00394d),
    ("switches",   0x1c003e40, 0x1c003965),
    ("onsets",     0x1c00c550, 0x1c00396e),
    ("rules",      0x1c005910, 0x1c003977),
    ("group",      0x1c00c6d0, 0x1c003980),
    ("pause",      0x1c003de0, 0x1c003989),
    ("into_alloc", 0x1c00cc10, 0x1c003992),
    ("classify",   0x1c00c8c0, 0x1c0039bc),
    ("intonation", 0x1c00cdc0, 0x1c0039c5),
    ("durations",  0x1c00c3e0, 0x1c0039ce),
    ("rate",       0x1c00f7c0, 0x1c0039d7),
    ("compact",    0x1c00c420, 0x1c0039e4),
    ("generate",   0x1c005840, 0x1c0039f7),
    ("smooth",     0x1c00de60, 0x1c003a0f),
    ("expression", 0x1c00be40, 0x1c003a1b),
]
INTO_FROM = "into_alloc"
FRAMES_FROM = "generate"

DEFAULT_TEXTS = [
    "hello world",
    "The quick brown fox jumps over the lazy dog.",
    "It was 1996, and speech synthesis sounded like this!",
    "Dr. Smith paid $42.50 on Jan. 3rd at 10:30 AM; call 555-1234 or e-mail me.",
    "Hello, world? Yes... \"quoted\" (parenthetical) and CAPS WORDS, ok.",
]


def emu_records(e, st):
    recs = e.u32(st + 0x1c)
    out = []
    for i in range(4096):
        a = recs + 26 * i
        cmds, ph, lang, code, dur, stress, attrs, flags = struct.unpack("<IIIHHHii", e.rd(a, 26))
        langid = e.u32(lang + 0x10) if lang else -1
        if not lang:
            table = 3
        elif ph == e.u32(lang + 0x14):
            table = 0
        elif ph == e.u32(lang + 0x18):
            table = 1
        else:
            table = 2
        words = []
        if cmds:
            c = cmds
            while e.s16(c) != 0x1e and len(words) + 3 <= 8:
                words += [e.s16(c), e.s16(c + 2), e.s16(c + 4)]
                c += 6
        row = [code, dur, stress, attrs, flags, langid, table, len(words)] + words
        out.append(tuple(row + [0] * (16 - len(row))))
        if code == 0xff:
            break
    return out


def emu_snapshot(e, st, stage_index):
    snap = {"records": emu_records(e, st), "count": e.u16(st + 0xe)}
    names = [s[0] for s in STAGES]
    if stage_index >= names.index(INTO_FROM):
        groups = e.u16(st + 0xbe)
        base = e.u32(st + 0xc0)
        snap["into"] = e.rd(base, 8 * (groups + 3))
    if stage_index >= names.index(FRAMES_FROM):
        frames = e.u32(st + 0xf6)
        nbytes = e.u32(st + 0x102)
        snap["frames"] = e.rd(frames, nbytes)
    return snap


def run_original(dlls, text):
    e = Emu(dlls)
    e.open()
    r, phon = e.text_to_phon(text.encode("latin-1"))
    if r:
        raise RuntimeError("SVTextToPhon -> %#x" % r)
    sentences = []

    def make_hook(k):
        def hook(emu):
            st = emu.h + 0x13a
            if k == 0:
                sentences.append({})
            sentences[-1][STAGES[k][0]] = emu_snapshot(emu, st, k)
        return hook

    for k, (_, _, addr) in enumerate(STAGES):
        e.hooks[addr] = make_hook(k)
    pcm = e.narrate(phon)
    return phon, sentences, pcm


class Recon:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        self.lib.oracle_new.restype = ctypes.c_void_p
        for fn in ("oracle_begin", "oracle_sentence", "oracle_records",
                   "oracle_intonation", "oracle_frames", "oracle_count"):
            getattr(self.lib, fn).restype = ctypes.c_int
        self.lib.oracle_begin.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
        self.lib.oracle_sentence.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.lib.oracle_records.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        self.lib.oracle_intonation.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        self.lib.oracle_frames.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        self.lib.oracle_count.argtypes = [ctypes.c_void_p]
        self.lib.oracle_pcm.restype = ctypes.c_int
        self.lib.oracle_pcm.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]

    def pcm(self, phon):
        e = self.lib.oracle_new(0)
        buf = (ctypes.c_uint8 * (8 << 20))()
        n = self.lib.oracle_pcm(e, phon, buf, 8 << 20)
        return bytes(buf[:n]) if n >= 0 else n

    def run(self, phon, sentence, stage_index):
        """Run sentences 0..sentence, the last one stopping after stage."""
        e = self.lib.oracle_new(0)
        if not e:
            raise RuntimeError("oracle_new failed")
        self.lib.oracle_begin(e, phon, 0)
        for s in range(sentence + 1):
            stop = STAGES[stage_index][1] if s == sentence else 0
            rc = self.lib.oracle_sentence(e, stop)
            if rc:
                return {"rc": rc}
        snap = {"rc": 0}
        buf = (ctypes.c_int32 * (16 * 4096))()
        n = self.lib.oracle_records(e, buf, 4096)
        snap["records"] = [tuple(buf[16 * i:16 * i + 16]) for i in range(n)]
        snap["count"] = self.lib.oracle_count(e)
        names = [s[0] for s in STAGES]
        if stage_index >= names.index(INTO_FROM):
            ib = (ctypes.c_uint8 * 65536)()
            n = self.lib.oracle_intonation(e, ib, 65536)
            snap["into"] = bytes(ib[:n]) if n >= 0 else None
        if stage_index >= names.index(FRAMES_FROM):
            fb = (ctypes.c_uint8 * (1 << 20))()
            n = self.lib.oracle_frames(e, fb, 1 << 20)
            snap["frames"] = bytes(fb[:n]) if n >= 0 else None
        return snap


REC_FIELDS = ["code", "dur", "stress", "attrs", "flags", "lang", "table", "ncmd"]


def diff_records(a, b):
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x != y:
            if x is None or y is None:
                return "record %d: original %s, reconstruction %s" % (i, x, y)
            f = [REC_FIELDS[k] if k < 8 else "cmd%d" % (k - 8) for k in range(16) if x[k] != y[k]]
            return "record %d differs in %s:\n      original %s\n      recon    %s" % (i, ",".join(f), x, y)
    return None


def mask_events(frames):
    """Blank a frame's event parameter/word (+0x1b..+0x1d) unless its event
    bits (+0x17 & 0x28) say they are live. The generator (0x1c205d5d) fills
    them from two stack locals it only initialises when a 0x334/0x352
    command is present; otherwise the original copies whatever the stack
    held, which no reconstruction can or should reproduce."""
    b = bytearray(frames)
    for i in range(0, len(b) - 31, 32):
        if not (b[i + 0x17] & 0x28):
            b[i + 0x1b:i + 0x1e] = b"\0\0\0"
    return bytes(b)


def compare(orig, rec):
    if rec.get("rc"):
        return "reconstruction returned %#x" % rec["rc"]
    d = diff_records(orig["records"], rec["records"])
    if d:
        return d
    if orig["count"] != rec["count"]:
        return "record_count %d vs %d" % (orig["count"], rec["count"])
    if "into" in orig and orig["into"] != rec.get("into"):
        a, b = orig["into"], rec.get("into") or b""
        k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        return "intonation block differs at byte %d (len %d vs %d)" % (k, len(a), len(b))
    if "frames" in orig and mask_events(orig["frames"]) != mask_events(rec.get("frames") or b""):
        a, b = mask_events(orig["frames"]), mask_events(rec.get("frames") or b"")
        k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        return "frames differ at byte %d: frame %d field +%#x (len %d vs %d)" % (
            k, k // 32, k % 32, len(a), len(b))
    return None


def word_texts(n, seed):
    try:
        words = [w.strip() for w in open("/usr/share/dict/words") if w.strip().isalpha()]
    except OSError:
        return []
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        k = rng.randint(1, 8)
        s = " ".join(rng.choice(words) for _ in range(k))
        out.append(s + rng.choice([".", "?", "!", ",", ""]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dlls", required=True, help="directory with TIBASE32.DLL, TIENG32.DLL")
    ap.add_argument("--library", required=True)
    ap.add_argument("--text", action="append")
    ap.add_argument("--frontend-library", help="also check public text conversion and PCM using libtispeech")
    ap.add_argument("--words", type=int, default=0, help="add N random dictionary sentences")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x19961118)
    ap.add_argument("--stage", default=STAGES[-1][0], help="last stage to compare")
    ap.add_argument("--pcm", action="store_true",
                    help="also compare the whole utterance's PCM stream")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    texts = (a.text or DEFAULT_TEXTS) + word_texts(a.words, a.seed)
    names = [s[0] for s in STAGES]
    last = names.index(a.stage)
    recon = Recon(a.library)
    frontend = None
    if a.frontend_library:
        frontend = ctypes.CDLL(os.path.abspath(a.frontend_library))
        frontend.tispeech_text_to_phonemes.argtypes = [ctypes.c_uint32, ctypes.c_char_p,
                                                      ctypes.c_void_p, ctypes.c_int32]
        frontend.tispeech_synthesize.argtypes = [ctypes.c_uint32, ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_int32)]
        frontend.tispeech_free_samples.argtypes = [ctypes.c_void_p]
    failures = 0
    for text in texts:
        phon, sentences, pcm = run_original(a.dlls, text)
        if a.verbose:
            print("%r -> %r (%d sentences)" % (text, phon, len(sentences)))
        bad = None
        if frontend:
            buf = ctypes.create_string_buffer(65536)
            rc = frontend.tispeech_text_to_phonemes(1, text.encode("utf-8"), buf, len(buf))
            if rc or buf.value != phon:
                failures += 1
                print("FAIL %r: public text conversion rc=%#x\n  original %r\n  native   %r"
                      % (text, rc, phon, buf.value))
                continue
            if a.pcm:
                samples, count, rate = ctypes.c_void_p(), ctypes.c_int32(), ctypes.c_int32()
                rc = frontend.tispeech_synthesize(1, buf.value, ctypes.byref(samples),
                                                 ctypes.byref(count), ctypes.byref(rate))
                try:
                    mine = ctypes.string_at(samples, count.value) if samples.value else b""
                    if rc or rate.value != 11025 or mine != pcm:
                        bad = (-1, "public PCM", "rc=%#x, rate=%d, samples=%d vs %d"
                               % (rc, rate.value, count.value, len(pcm)))
                finally:
                    frontend.tispeech_free_samples(samples)
        for s, snaps in enumerate(sentences if not bad else []):
            for k in range(last + 1):
                name = names[k]
                if name not in snaps:
                    continue
                rec = recon.run(phon, s, k)
                d = compare(snaps[name], rec)
                if d:
                    bad = (s, name, d)
                    break
            if bad:
                break
        if not bad and a.pcm:
            mine = recon.pcm(phon)
            if isinstance(mine, int):
                bad = (-1, "pcm", "reconstruction returned %#x" % -mine)
            elif mine != pcm:
                k = next((i for i in range(min(len(pcm), len(mine))) if pcm[i] != mine[i]),
                         min(len(pcm), len(mine)))
                bad = (-1, "pcm", "PCM differs at sample %d (len %d vs %d)" % (k, len(pcm), len(mine)))
            elif a.verbose:
                print("  PCM identical, %d samples" % len(pcm))
        if bad:
            failures += 1
            print("FAIL %r\n  phonemes %r\n  sentence %d, first bad stage '%s': %s"
                  % (text, phon, bad[0], bad[1], bad[2]))
        elif a.verbose:
            print("  ok through '%s'" % a.stage)
    print("%s: %d/%d texts match through stage '%s'"
          % ("PASS" if failures == 0 else "FAIL", len(texts) - failures, len(texts), a.stage))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
