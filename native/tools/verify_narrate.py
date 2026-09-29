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

--language span --frontend-only is a different, narrower mode: TISPAN32's
synthesis stages (duration, frame generation) are not reconstructed, so there
is no narrate pipeline to walk for Spanish yet. This mode instead calls the
ORIGINAL's _SVTextToPhon@24 with TISPAN32 selected (tools/sv_emu.py's
Emu(languages=("TISPAN32.DLL",)).open(lang=2), i.e. SV_LANG_SPANISH) and
compares byte for byte against src/textphon_span.c through
tools/textphon_span_oracle.c, over a matrix of texts, output-buffer sizes
(the SVTTS retry contract) and flags (word marks, forced stress, spell mode).
--user-dict-library also takes --language span to run the SAME dictionary
differential (tools/userdict_oracle.c's synthetic-dictionary cases) against
the Spanish front end instead of English.

USAGE
    verify_narrate.py --dlls DIR --library build/libnarrate_oracle.dylib \\
        [--text "..."]... [--words N] [--stage NAME] [-v]

    verify_narrate.py --dlls DIR --library build/libverify_frontend_span.dylib \\
        --language span --frontend-only [--words N] [-v]
"""
import argparse
import ctypes
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sv_emu import Emu, HWND  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EBX, UC_X86_REG_ESP  # noqa: E402

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

# Raw phoneme strings (SVTextToPhon's English front end never emits `{...}`
# itself; these exercise FUN_1c005180 directly). " /HEH5LOW WER5LD" is
# "hello world"'s own phoneme string, reused here so the commands sit in
# front of real, audible content and --pcm is a meaningful check.
_HELLO = " /HEH5LOW WER5LD"
DEFAULT_INLINE_PHONEMES = [
    "{100}" + _HELLO,                       # bare number -> command 0x28
    "{c4}" + _HELLO,                        # musical note
    "{c#4 50}" + _HELLO,                    # sharp note + extra word
    "{g8}" + _HELLO,                        # note at the top of the octave range
    "{p5}" + _HELLO,                        # "p" + digit, no space (== "pitch 5")
    "{pitch 50}" + _HELLO,                  # keyword + plain number
    "{pitch 50 10}" + _HELLO,               # keyword + number + extra
    "{rate 150}" + _HELLO,
    "{voice female}" + _HELLO,              # named-list commands
    "{language english}" + _HELLO,
    "{tract child}" + _HELLO,
    "{glot mellow}" + _HELLO,
    "{voicing breathy}" + _HELLO,
    "{f0style monotone}" + _HELLO,
    "{speak word}" + _HELLO,
    "{mouths on}" + _HELLO,
    "{sentsync on}" + _HELLO,                # on/off *sync command
    "{voice female;pitch 50}" + _HELLO,      # ';'-chained within one brace
    "{voice female}{pitch 50}" + _HELLO,     # chained "{...}{...}" pairs
]

DEFAULT_BAD_INLINE_PHONEMES = [
    "{bogus 1}" + _HELLO,                    # unknown keyword
    "{voice bogus}" + _HELLO,                # unmatched name in a value list
    "{pitch abc}" + _HELLO,                  # unparseable number
    "{c9}" + _HELLO,                         # octave out of 0..8, and not a keyword either
    "{pitch -32768}" + _HELLO,               # value collides with the parser's own 0x8000
                                             # "malformed" sentinel -- an original ambiguity
    "{}" + _HELLO,                           # empty braces: no word at all
]

# Phoneme strings whose entire sentence is inline commands with no audible
# content at all: e->groups ends up 0 (no record ever gets the "stressed
# syllable" flag). See narrate.c's classify_caf0/classify_cb60: the original
# computes `array[e->groups - 1]` there, which on its 32-bit target silently
# wraps back to one byte before the array (still inside the same padded
# allocation) but is a wild 64-bit offset in the reconstruction, so those two
# passes refuse to run at all when groups == 0 ("ours"). The one observable
# difference is a single scratch byte in the intonation block that nothing
# downstream reads -- PCM output is bit-identical -- so these are compared
# only through the 'into_alloc' stage plus PCM, not the full per-stage walk
# the other cases above get; see check_empty_content() below. Also covers
# "{voice male" with no closing '}' at all: the original still treats that
# as an implicit close at end-of-string (0x1c0051f7..0x1c005219).
EMPTY_CONTENT_PHONEMES = [
    "{voice male}",
    "{voice male",
]

# wordsync (0x334) and usync (0x352): the ONLY two command codes the frame
# generator reads to decide whether to populate a frame's event
# parameter/word (+0x1b/+0x1d) for real, per REVERSING.md's narrate-pipeline
# notes ("Frame fields +0x1b..+0x1d ... are copied from two locals the
# generator only sets when a 0x334/0x352 command is present"). The
# event-reporting block itself (0x1c004543..0x1c004605) is not reconstructed
# -- a pre-existing, already documented gap, not something this parser can
# fix -- so these two commands are the only ones whose record-level "commands"
# field is verified (through the last pre-frame stage, 'compact') without
# also demanding the frame bytes match; PCM output is unaffected and checked
# separately, in full.
EVENT_FIELD_PHONEMES = [
    "{wordsync 1}" + _HELLO,
    "{usync 1}" + _HELLO,
]

# SVSetSpeakingMode (0x1c00ecd0): value & 7 must be nonzero or it refuses
# with 0x1b62; bit 0x2 of value then sets (1) or clears (0) handle+0xcc bit
# 0x10, which persists across calls. 0 and 8 are the "bad value" cases
# (value & 7 == 0, so refused without narrating at all); the managed
# TiSpeakingMode.Natural == 0 is one of them (REVERSING.md open question 9).
SPEAKING_MODE_VALUES = [0, 1, 2, 3, 4, 5, 6, 7, 8]
SPEAKING_MODE_TEXTS = [
    "hello world",
    "The quick brown fox jumps over the lazy dog.",
    "Dr. Smith paid $42.50 on Jan. 3rd at 10:30 AM; call 555-1234 or e-mail me.",
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


def _snapshot_and_narrate(e, phon):
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
    return sentences, pcm


def run_original(dlls, text):
    e = Emu(dlls)
    e.open()
    r, phon = e.text_to_phon(text.encode("latin-1"))
    if r:
        raise RuntimeError("SVTextToPhon -> %#x" % r)
    sentences, pcm = _snapshot_and_narrate(e, phon)
    return phon, sentences, pcm


def run_original_phon(dlls, phon):
    """Like run_original, but `phon` is already a phoneme string -- used for
    inline `{...}` command syntax, which SVTextToPhon's English front end
    does not itself produce (REVERSING.md: "Inline commands can be converted
    to phoneme text, but synthesis still rejects them")."""
    e = Emu(dlls)
    e.open()
    sentences, pcm = _snapshot_and_narrate(e, phon)
    return sentences, pcm


def run_original_speaking_mode(dlls, text, speaking_mode):
    """Like run_original, but calls _SVSetSpeakingMode@8 (0x1c00ecd0) on the
    handle first, the same way tools/verify_voice.py exercises the other
    SVSet* setters. Returns (set_rc, phon, sentences, pcm); if set_rc is
    nonzero (an invalid `value`, i.e. value & 7 == 0), phon/sentences/pcm are
    None -- the caller compares set_rc against sv_engine_set_speaking_mode's
    own return instead of narrating."""
    e = Emu(dlls)
    e.open()
    set_rc = e.call(e.exp("_SVSetSpeakingMode@8"), [e.h, speaking_mode]) & 0xffffffff
    if set_rc:
        return set_rc, None, None, None
    r, phon = e.text_to_phon(text.encode("latin-1"))
    if r:
        raise RuntimeError("SVTextToPhon -> %#x" % r)
    sentences, pcm = _snapshot_and_narrate(e, phon)
    return set_rc, phon, sentences, pcm


def original_narrate_rc(dlls, phon):
    """Call the original's SVNarrate directly and return its raw return
    code, without pumping waveOut or raising on error -- for malformed
    inline commands, which the text-based path above never reaches without
    aborting the whole run."""
    e = Emu(dlls)
    e.open()
    p = e.malloc(len(phon) + 1)
    e.mu.mem_write(p, phon + b"\0")
    return e.call(e.exp("_SVNarrate@20"), [e.h, p, HWND, 0, 0])


# ---------------------------------------------------------------------------
# Renderer events (0x1c004543..0x1c004605 -> FUN_1c00498f's queue).
#
# The original is hooked at 0x1c00498f itself, so every report is captured in
# order, before the 50 ms timer (0x1c00f700) would post it and before the
# 100-entry ring could overwrite it. Each is compared as (sample, time_ms,
# code, value): time_ms is the record's own [ebx+0xfe] >> 6, the sample is
# the PCM already handed to waveOutWrite plus what this render call
# (0x1c00402c, its count argument) has written, count - [ebx+0x2f4].
# ---------------------------------------------------------------------------

EVENT_FLAGS = [0, 0x08, 0x0f, 0x4f]
EVENT_PHONEMES = [
    "{wordsync 5}" + _HELLO + " {usync 3} AE5ND {wordsync 300}GUH5DBAY.",
    " {usync 200} /HEH5LOW. {wordsync 1} WER5LD, {wordsync 2} AH5GEYN?",
]


def original_events(e, phon, flags):
    events = []
    render = {"count": 0}

    def on_render(emu):
        render["count"] = emu.u16(emu.mu.reg_read(UC_X86_REG_ESP) + 8)

    def on_event(emu):
        esp = emu.mu.reg_read(UC_X86_REG_ESP)
        st = emu.mu.reg_read(UC_X86_REG_EBX)
        before = sum(len(w) for w in emu.wave)
        events.append((before + render["count"] - emu.u16(st + 0x2f4),
                       emu.u32(st + 0xfe) >> 6, emu.u32(esp + 4) & 0xffff,
                       emu.u16(esp + 8), emu.u32(esp + 0xa)))

    e.hook_at(0x1c00402c, on_render)
    e.hook_at(0x1c00498f, on_event)
    pcm = e.narrate(phon, flags=flags)
    return events, pcm


class PublicEvents:
    """libtispeech's tispeech_text_to_phonemes_flags + tispeech_synthesize_events."""
    class Event(ctypes.Structure):
        _fields_ = [("sample", ctypes.c_int32), ("time_ms", ctypes.c_int32),
                    ("code", ctypes.c_uint16), ("value", ctypes.c_uint16)]

    def __init__(self, path, language):
        self.lib = ctypes.CDLL(os.path.abspath(path))
        self.language = language
        self.lib.tispeech_text_to_phonemes_flags.argtypes = [
            ctypes.c_uint32, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32,
            ctypes.c_char_p, ctypes.c_int32]
        self.lib.tispeech_synthesize_events.argtypes = [
            ctypes.c_uint32, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_int32)]
        self.lib.tispeech_free_samples.argtypes = [ctypes.c_void_p]
        self.lib.tispeech_free_events.argtypes = [ctypes.c_void_p]

    def text(self, text):
        out = ctypes.create_string_buffer(1 << 16)
        rc = self.lib.tispeech_text_to_phonemes_flags(self.language, text.encode("utf-8"), None,
                                                      8, out, len(out))
        return rc, out.value

    def synthesize(self, phon, flags):
        pcm, n, rate = ctypes.c_void_p(), ctypes.c_int32(), ctypes.c_int32()
        evp, nev = ctypes.c_void_p(), ctypes.c_int32()
        rc = self.lib.tispeech_synthesize_events(self.language, phon, None, flags,
                                                 ctypes.byref(pcm), ctypes.byref(n),
                                                 ctypes.byref(rate), ctypes.byref(evp),
                                                 ctypes.byref(nev))
        try:
            samples = ctypes.string_at(pcm, n.value) if pcm.value else b""
            arr = ctypes.cast(evp, ctypes.POINTER(self.Event)) if evp.value else None
            events = [(arr[k].sample, arr[k].time_ms, arr[k].code, arr[k].value)
                      for k in range(nev.value)]
        finally:
            self.lib.tispeech_free_samples(pcm)
            self.lib.tispeech_free_events(evp)
        return rc, events, samples


def check_events(new_emu, recon, texts, phonemes, verbose=False, public=None):
    """Every text through the original's SVTextToPhon with word marks (flag
    8, which is what puts @w markers -- and so 0x3eb reports -- into the
    phonemes), plus raw phoneme strings with {wordsync}/{usync}, narrated
    under each EVENT_FLAGS value. Returns (cases, failures)."""
    cases = failures = seen = 0
    codes = {}
    inputs = []
    for text in texts:
        e = new_emu()
        rc, phon = e.text_to_phon(text.encode("latin-1"), flags=8, cap=1 << 16)
        if rc:
            raise RuntimeError("SVTextToPhon -> %#x" % rc)
        inputs.append(phon)
        if public:
            cases += 1
            prc, pphon = public.text(text)
            if (prc, pphon) != (0, phon):
                failures += 1
                print("FAIL public word marks %r: rc=%#x\n  original %r\n  native   %r"
                      % (text, prc, phon, pphon))
    inputs += [p.encode("latin-1") for p in phonemes]
    for phon in inputs:
        for flags in EVENT_FLAGS:
            cases += 1
            orig, pcm = original_events(new_emu(), phon, flags)
            bad = [ev for ev in orig if ev[3] != ev[4]]
            mine, mine_pcm = recon.events(phon, flags)
            want = [ev[:4] for ev in orig]
            # The public API takes only the four enable bits, not 0x40.
            if public and not (flags & ~0xf) and not bad and mine == want and mine_pcm == pcm:
                prc, mine, mine_pcm = public.synthesize(phon, flags)
                if prc:
                    mine = "rc=%#x" % prc
            if bad or mine != want or mine_pcm != pcm:
                failures += 1
                first = next((k for k, (x, y) in enumerate(zip(want, mine)) if x != y),
                             min(len(want), len(mine)))
                print("FAIL events flags=%#x %r: %d original vs %d native, first difference at "
                      "#%d: %r vs %r%s%s" % (
                          flags, phon[:60], len(want), len(mine), first,
                          want[first] if first < len(want) else None,
                          mine[first] if first < len(mine) else None,
                          "" if mine_pcm == pcm else ", PCM differs",
                          ", dword value != word value" if bad else ""))
                continue
            seen += len(want)
            for ev in want:
                codes[ev[2]] = codes.get(ev[2], 0) + 1
            if verbose:
                print("ok events flags=%#x %r: %d events" % (flags, phon[:50], len(want)))
    print("events: %d reports compared, by code %s" % (
        seen, ", ".join("%#x:%d" % kv for kv in sorted(codes.items()))))
    return cases, failures


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
        self.lib.oracle_set_speaking_mode.restype = ctypes.c_int
        self.lib.oracle_set_speaking_mode.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    def events(self, phon, flags):
        e = self.lib.oracle_new(0)
        fn = self.lib.oracle_events
        fn.restype = ctypes.c_int
        fn.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_void_p,
                       ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        out = (ctypes.c_int32 * (4 * 20000))()
        pcm = (ctypes.c_uint8 * (8 << 20))()
        n_pcm = ctypes.c_int()
        n = fn(e, phon, flags, out, 20000, pcm, 8 << 20, ctypes.byref(n_pcm))
        if n < 0:
            return n, None
        return ([tuple(out[4 * k:4 * k + 4]) for k in range(min(n, 20000))],
                bytes(pcm[:n_pcm.value]))

    def pcm(self, phon):
        e = self.lib.oracle_new(0)
        buf = (ctypes.c_uint8 * (8 << 20))()
        n = self.lib.oracle_pcm(e, phon, buf, 8 << 20)
        return bytes(buf[:n]) if n >= 0 else n

    def pcm_with_mode(self, phon, speaking_mode):
        """Like pcm(), but calls sv_engine_set_speaking_mode (0x1c00ecd0)
        first -- mirrors run_original_speaking_mode(). Returns
        (set_rc, pcm_bytes_or_negative_error)."""
        e = self.lib.oracle_new(0)
        set_rc = self.lib.oracle_set_speaking_mode(e, speaking_mode)
        if set_rc:
            return set_rc, None
        buf = (ctypes.c_uint8 * (8 << 20))()
        n = self.lib.oracle_pcm(e, phon, buf, 8 << 20)
        return 0, (bytes(buf[:n]) if n >= 0 else n)

    def narrate_rc(self, phon):
        """sv_narrate_sentence's raw return code for one sentence -- for
        comparing malformed inline commands against original_narrate_rc()."""
        e = self.lib.oracle_new(0)
        self.lib.oracle_begin(e, phon, 0)
        return self.lib.oracle_sentence(e, 0)

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


def check_empty_content(dlls, recon, verbose=False):
    """EMPTY_CONTENT_PHONEMES: compared only through 'into_alloc' plus PCM,
    since e->groups == 0 makes one intonation-block scratch byte an already
    understood, harmless divergence (see the comment on the list above)."""
    failures = 0
    names = [s[0] for s in STAGES]
    last = names.index("into_alloc")
    for phon_str in EMPTY_CONTENT_PHONEMES:
        phon = phon_str.encode("latin-1")
        sentences, pcm = run_original_phon(dlls, phon)
        bad = None
        for s, snaps in enumerate(sentences):
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
        if not bad:
            mine = recon.pcm(phon)
            if isinstance(mine, int):
                bad = (-1, "pcm", "reconstruction returned %#x" % -mine)
            elif mine != pcm:
                k = next((i for i in range(min(len(pcm), len(mine))) if pcm[i] != mine[i]),
                         min(len(pcm), len(mine)))
                bad = (-1, "pcm", "PCM differs at sample %d (len %d vs %d)" % (k, len(pcm), len(mine)))
        if bad:
            failures += 1
            print("FAIL empty-content phoneme %r\n  sentence %d, first bad stage '%s': %s"
                  % (phon, bad[0], bad[1], bad[2]))
        elif verbose:
            print("empty-content phoneme %r: records match through 'into_alloc', PCM identical (%d bytes)"
                  % (phon, len(pcm)))
    return failures


def check_event_field(dlls, recon, verbose=False):
    """EVENT_FIELD_PHONEMES: compared through 'compact' (the last stage
    before frames exist) plus PCM, skipping the frame-level walk -- see the
    comment on the list above for why."""
    failures = 0
    names = [s[0] for s in STAGES]
    last = names.index("compact")
    for phon_str in EVENT_FIELD_PHONEMES:
        phon = phon_str.encode("latin-1")
        sentences, pcm = run_original_phon(dlls, phon)
        bad = None
        for s, snaps in enumerate(sentences):
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
        if not bad:
            mine = recon.pcm(phon)
            if isinstance(mine, int):
                bad = (-1, "pcm", "reconstruction returned %#x" % -mine)
            elif mine != pcm:
                k = next((i for i in range(min(len(pcm), len(mine))) if pcm[i] != mine[i]),
                         min(len(pcm), len(mine)))
                bad = (-1, "pcm", "PCM differs at sample %d (len %d vs %d)" % (k, len(pcm), len(mine)))
        if bad:
            failures += 1
            print("FAIL event-field phoneme %r\n  sentence %d, first bad stage '%s': %s"
                  % (phon, bad[0], bad[1], bad[2]))
        elif verbose:
            print("event-field phoneme %r: records match through 'compact', PCM identical (%d bytes)"
                  % (phon, len(pcm)))
    return failures


def check_speaking_mode(dlls, recon, verbose=False):
    """SVSetSpeakingMode (0x1c00ecd0) / sv_engine_set_speaking_mode: for
    every text x value pair, checks that (a) an invalid value (value & 7 ==
    0) is refused with the same code on both sides without narrating, and
    (b) a valid value produces PCM matching between the original and the
    reconstruction. Also checks, per text, whether the ORIGINAL's own PCM
    output differs at all across the valid values -- rule_context's
    e->flags & 0x10 check (its only reader) is unreachable from apply_rules
    (every rule with that bit set also has 0x02 clear, so it fails the
    caller's `!(rule[0xb] & 0x12)` gate before rule_context ever runs), so
    none of these texts are expected to differ, and this is where that
    would show up if the analysis were wrong for some other rule."""
    failures = 0
    any_orig_difference = False
    for text in SPEAKING_MODE_TEXTS:
        baseline, baseline_value = None, None
        for value in SPEAKING_MODE_VALUES:
            orig_rc, phon, _, orig_pcm = run_original_speaking_mode(dlls, text, value)
            expect_bad = (value & 7) == 0
            if expect_bad:
                mine_rc = recon.pcm_with_mode(b"", value)[0]
                # oracle_pcm's phon argument is irrelevant here: set_rc is
                # returned before it would ever be used.
                ok = bool(orig_rc) and bool(mine_rc) and orig_rc == 0x1b62 and mine_rc == 0x1b62
                if not ok:
                    failures += 1
                    print("FAIL speaking mode %r value=%d: expected both refused with 0x1b62, "
                          "got original=%#x reconstruction=%#x" % (text, value, orig_rc, mine_rc))
                elif verbose:
                    print("speaking mode %r value=%d: both refused (0x1b62), as expected" % (text, value))
                continue
            mine_set_rc, mine_pcm = recon.pcm_with_mode(phon, value)
            ok = orig_rc == 0 and mine_set_rc == 0 and mine_pcm == orig_pcm
            if not ok:
                failures += 1
                print("FAIL speaking mode %r value=%d: original set_rc=%#x reconstruction "
                      "set_rc=%#x, pcm match=%s"
                      % (text, value, orig_rc, mine_set_rc, mine_pcm == orig_pcm))
            elif verbose:
                print("speaking mode %r value=%d: PCM matches (%d bytes)" % (text, value, len(orig_pcm)))
            if baseline is None:
                baseline, baseline_value = orig_pcm, value
            elif orig_pcm != baseline:
                any_orig_difference = True
                print("NOTE speaking mode %r value=%d: original PCM differs from value=%d's -- "
                      "the 'no effect' analysis does not hold for this text"
                      % (text, value, baseline_value))
    if not any_orig_difference and verbose:
        print("speaking mode: original PCM identical across all valid values, for every text tried "
              "-- consistent with rule_context's e->flags & 0x10 check being unreachable")
    return failures


# ---------------------------------------------------------------------------
# User dictionary (src/userdict.c, include/tispeech/userdict.h).
#
# The "SVXF" file format and TIENG32!FUN_1c2097a0's lookup, both reconstructed
# by reading the disassembly against the original under tools/sv_emu.py (see
# userdict.h for the format and REVERSING.md for the verification log). This
# builds small synthetic dictionaries -- never anything derived from
# proprietary bytes -- loads them into the ORIGINAL engine via
# SVLoadUserDictionary (tools/sv_emu.py's register_file() stubs fopen/fread),
# and compares sv_text_to_phon output against tools/userdict_oracle.c through
# a --user-dict-library.
# ---------------------------------------------------------------------------
UD_HEADER_LEN = 4 + 20 + 4 + 4 + 28 * 4


def ud_header(field0, field1, buckets, blob, magic=b"SVXF"):
    assert len(buckets) == 28
    h = magic + b"\x00" * 20
    h += struct.pack("<I", field0) + struct.pack("<I", field1)
    for o in buckets:
        h += struct.pack("<I", o)
    assert len(h) == UD_HEADER_LEN
    return h + blob


def ud_entry(word, phon, h2=0x02):
    """A real word entry (userdict.h): H0 = len(word)+len(phon), H1 =
    len(word), H2 bit 1 set (bit 0 -- compare against original vs normalised
    text -- taken from the caller)."""
    h1 = len(word)
    h0 = h1 + len(phon)
    assert h0 <= 0xff and h1 <= 0xff
    return bytes([h0, h1, h2]) + word + phon


def ud_bucket_index(ch):
    if b"A"[0] <= ch <= b"Z"[0]:
        return ch - b"A"[0]
    if b"0"[0] <= ch <= b"9"[0]:
        return 26
    return 27


def ud_build(word_entries, field0=1):
    """word_entries: [(word_bytes, phon_bytes, h2), ...], grouped by bucket
    and written in the given order (the caller is responsible for ascending
    sort order within letter buckets 0..25 -- see userdict.h)."""
    by_bucket = {}
    for word, phon, h2 in word_entries:
        by_bucket.setdefault(ud_bucket_index(word[0]), []).append(ud_entry(word, phon, h2))
    blob = bytearray()
    offsets = [0] * 28
    for idx in sorted(by_bucket):
        offsets[idx] = len(blob)
        for e in by_bucket[idx]:
            blob += e
        blob += bytes([0, 0, 0])  # terminator
    if not blob:
        blob = bytes([0, 0, 0])
    return ud_header(field0, len(blob), offsets, bytes(blob))


class UserDictOracle:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        self.lib.userdict_oracle_text_to_phon.restype = ctypes.c_int32
        self.lib.userdict_oracle_text_to_phon.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
            ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32]

    def text_to_phon(self, dict_bytes, text, flags=0, cap=4096):
        buf = ctypes.create_string_buffer(cap)
        db = ctypes.c_char_p(dict_bytes) if dict_bytes is not None else None
        dn = len(dict_bytes) if dict_bytes is not None else 0
        rc = self.lib.userdict_oracle_text_to_phon(db, dn, text, buf, cap, flags)
        return rc, buf.value


def run_userdict_check(dlls, oracle_path, verbose=False):
    """Differential coverage for the user dictionary: load a synthetic
    dictionary into the ORIGINAL via SVLoadUserDictionary and compare
    sv_text_to_phon (tools/userdict_oracle.c) against it, word matches,
    misses, case handling, multi-word runs and malformed-file error codes
    all included."""
    failures = 0
    oracle = UserDictOracle(oracle_path)

    def orig_load_and_convert(dict_bytes, text, path=b"t.dict"):
        e = Emu(dlls)
        e.open()
        h = e.h
        if dict_bytes is not None:
            e.register_file(path.decode(), dict_bytes)
        rc = e.call(e.exp("_SVLoadUserDictionary@8"),
                    [h, e.alloc_data(path + b"\0")])
        r, phon = e.text_to_phon(text)
        return rc & 0xffffffff, r, phon

    def check(label, dict_bytes, text):
        nonlocal failures
        load_rc, orig_rc, orig_phon = orig_load_and_convert(dict_bytes, text)
        if load_rc != 0:
            # A dictionary that fails to load behaves as if none was
            # loaded; the oracle is given no dictionary either.
            mine_rc, mine_phon = oracle.text_to_phon(None, text)
        else:
            mine_rc, mine_phon = oracle.text_to_phon(dict_bytes, text)
        ok = orig_rc == mine_rc and orig_phon == mine_phon
        if verbose or not ok:
            print("%s %-55s -> orig(load=%#x rc=%d %r) recon(rc=%d %r)"
                  % ("ok  " if ok else "FAIL", label, load_rc, orig_rc, orig_phon,
                     mine_rc, mine_phon))
        if not ok:
            failures += 1
        return load_rc

    # Ordinary matches, several letters and the digit/default fallback
    # buckets, exercised together against a baseline with no entry at all.
    d = ud_build([
        (b"HELLO", b" /HAH5LOW", 0x02),
        (b"HELP", b" /HEHLP5", 0x02),
        (b"WORLD", b" /WER5LD", 0x02),
        (b"911", b" NAY5N /HUN5DRIHD ILEH5VIN", 0x02),
        (b"#TAG", b" HAE5SHTAEG", 0x02),
    ])
    for text in (b"HELLO", b"HELP", b"WORLD", b"HELLO WORLD", b"911",
                 b"#TAG", b"HELPME", b"HELLS", b"NOWHERE IN THE DICT"):
        check("match/miss %r" % text, d, text)

    # Prefix vs. whole-word boundary: HELL must not falsely match HELLO.
    d2 = ud_build([(b"HELL", b" HEL", 0x02), (b"HELLO", b" HELO", 0x02)])
    check("prefix-then-full HELLO", d2, b"HELLO")
    check("prefix-then-full HELL", d2, b"HELL")

    # Case handling (H2 bit 0): normalised (default) vs. original text.
    d3 = ud_build([(b"HELLO", b" NORM", 0x02)])
    check("bit0=0 normalised, mixed-case input", d3, b"Hello")
    d4 = ud_build([(b"Hello", b" ORIG", 0x03)])
    check("bit0=1 original, matching mixed case", d4, b"Hello")
    check("bit0=1 original, non-matching case", d4, b"HELLO")

    # Sorted-bucket requirement: out of order, the second entry must not be
    # reachable (letter buckets only -- digit/default buckets don't sort).
    d5 = ud_build([(b"APPLE", b" A1", 0x02), (b"ANT", b" A2", 0x02)])
    check("out-of-order letter bucket (should miss)", d5, b"ANT")

    # Malformed files: the format's own error paths.
    check("bad magic", ud_header(1, 3, [0] * 28, b"\0\0\0", magic=b"XXXX"), b"HELLO")
    check("truncated header", b"SVXF", b"HELLO")
    check("no dictionary at all", None, b"HELLO WORLD")

    print("%s: user dictionary — %d checks failed" % ("PASS" if failures == 0 else "FAIL", failures))
    return failures


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
    ap.add_argument("--user-dict-library",
                    help="also run the user-dictionary differential (tools/userdict_oracle.c) "
                         "against synthetic SVXF dictionaries")
    ap.add_argument("--phoneme", action="append",
                    help="raw phoneme string bypassing SVTextToPhon, e.g. to exercise inline "
                         "{...} commands (FUN_1c005180); repeatable")
    ap.add_argument("--bad-phoneme", action="append",
                    help="raw phoneme string expected to fail with SV_NAR_E_COMMAND on both "
                         "the original and the reconstruction; repeatable")
    ap.add_argument("--no-default-commands", action="store_true",
                    help="skip the built-in inline-command phoneme cases (DEFAULT_INLINE_PHONEMES "
                         "/ DEFAULT_BAD_INLINE_PHONEMES) when --phoneme/--bad-phoneme are also unset")
    ap.add_argument("--events", action="store_true",
                    help="compare the renderer's event reports instead (word marks, "
                         "{wordsync}/{usync}, mouth shapes) for the texts and EVENT_PHONEMES")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.events:
        texts = (a.text or DEFAULT_TEXTS) + word_texts(a.words, a.seed)

        def new_emu():
            e = Emu(a.dlls)
            e.open()
            return e
        public = PublicEvents(a.frontend_library, 1) if a.frontend_library else None
        cases, failures = check_events(new_emu, Recon(a.library), texts, EVENT_PHONEMES,
                                       a.verbose, public)
        print("%s: %d/%d event cases match" % ("PASS" if not failures else "FAIL",
                                              cases - failures, cases))
        return 1 if failures else 0

    if a.user_dict_library:
        failures = run_userdict_check(a.dlls, a.user_dict_library, a.verbose)
        return 1 if failures else 0

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
    total = len(texts)

    phonemes = a.phoneme or ([] if a.no_default_commands else list(DEFAULT_INLINE_PHONEMES))
    for phon_str in phonemes:
        phon = phon_str.encode("latin-1")
        total += 1
        sentences, pcm = run_original_phon(a.dlls, phon)
        if a.verbose:
            print("phoneme %r (%d sentences)" % (phon, len(sentences)))
        bad = None
        for s, snaps in enumerate(sentences):
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
            print("FAIL phoneme %r\n  sentence %d, first bad stage '%s': %s"
                  % (phon, bad[0], bad[1], bad[2]))
        elif a.verbose:
            print("  ok through '%s'" % a.stage)

    bad_phonemes = a.bad_phoneme or ([] if a.no_default_commands else list(DEFAULT_BAD_INLINE_PHONEMES))
    for phon_str in bad_phonemes:
        phon = phon_str.encode("latin-1")
        total += 1
        orig_rc = original_narrate_rc(a.dlls, phon)
        mine_rc = recon.narrate_rc(phon)
        if orig_rc != mine_rc:
            failures += 1
            print("FAIL malformed phoneme %r: original rc=%#x, reconstruction rc=%#x"
                  % (phon, orig_rc, mine_rc))
        elif a.verbose:
            print("malformed phoneme %r: both rc=%#x" % (phon, orig_rc))

    if not a.phoneme and not a.no_default_commands:
        total += len(EMPTY_CONTENT_PHONEMES)
        failures += check_empty_content(a.dlls, recon, a.verbose)
        total += len(EVENT_FIELD_PHONEMES)
        failures += check_event_field(a.dlls, recon, a.verbose)
        total += len(SPEAKING_MODE_TEXTS) * len(SPEAKING_MODE_VALUES)
        failures += check_speaking_mode(a.dlls, recon, a.verbose)

    print("%s: %d/%d cases match through stage '%s'"
          % ("PASS" if failures == 0 else "FAIL", total - failures, total, a.stage))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
