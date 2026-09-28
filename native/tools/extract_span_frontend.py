#!/usr/bin/env python3
"""
extract_span_frontend.py — emit the combined .data/.text image that Spanish
needs at runtime: the frame generator's (src/langgen.c) relocated globals,
and the Spanish text front end's (src/textphon_span.c) own tables.

WHY THIS EXISTS, AND HOW IT WORKS
----------------------------------
src/langgen.c is TIENG32's frame generator (module vtable +0x08), reused
VERBATIM for TISPAN32 because the two are the same compiled code at
different addresses (REVERSING.md, "the language modules are one code
base": 120/120 identical instruction forms). It addresses ~200 module
globals -- 17 parameter tracks, contour buffers, a five-record window, and
a handful of constant tables -- through TIENG32's OWN literal virtual
addresses via langmod_priv.h's DP()/B()/W()/L() macros, which resolve
`va - dva` into a mutable .data copy. For that file to work UNCHANGED
against TISPAN32, the Spanish module instance must present a .data image
at TIENG32's OWN dva (0x1c24b000): the "VA relocation" its own header
comments call for.

Diffed byte-for-byte against the real DLLs (see REVERSING.md and the
extraction session that produced this tool), the whole region langgen.c
reads is either:

  - pure runtime scratch (the 17 tracks, their contour buffers, the
    five-record window): zero in the FILE for both languages (it is BSS),
    so no relocation is needed there at all -- an all-zero buffer is
    already correct for either language;
  - internal pointers into this same blob (the 17 track-name pointers at
    TIENG32 0x1c24c158): correct only when left as TIENG32's OWN values,
    since every consumer dereferences them in this same TIENG32-addressed
    coordinate space -- copying TISPAN32's real (different) pointer values
    there would point outside the buffer;
  - or genuinely universal constants -- the interpolation-rate table
    (0x1c24c1b0, already known to be byte-identical: extract_generator.py
    cross-checks it), the track-name strings (0x1c24c0d4) and a small
    lookup at 0x1c24c1a0 and 0x1c24c400 -- ALSO confirmed byte-identical
    between TIENG32.DLL and TISPAN32.DLL by this tool's own preflight
    check below.

Exactly THREE sub-tables in that region differ by language: the pitch/
formant transition-percentage table (0x1c24c3f0), the nasalised-vowel
target table (0x1c24c420, (code,low,high) triples) and the manner-class
pair table (0x1c24c460, 10-byte entries). Their Spanish originals sit at a
FIXED offset from TIENG32's copies -- the SAME offset the rate table's own
anchor search finds between the two DLLs (extract_generator.find_rate_table,
no hardcoded address), because all of this is one contiguous constants blob
the 1996 linker placed as a unit. This tool re-derives that offset from the
anchor and overlays just those three tables with TISPAN32's real bytes.

A separate small .text-embedded table (TIENG32 0x1c205fb9..0x1c206780, two
little per-stress lookups langgen.c reads through TB()) is likewise
byte-identical between the DLLs (checked below) and is relocated the same
way, as a `sv_image` text segment at the SAME literal TIENG32 addresses
src/langgen.c already cites.

THE TEXT FRONT END'S OWN TABLES
--------------------------------
src/textphon_span.c is a separate, from-scratch port (not shared code
reused verbatim) written against TISPAN32's OWN real virtual addresses,
because unlike the frame generator its tables -- the exception dictionary,
the number-name tables, the spell-mode table, the pattern-dispatch table --
are genuinely different content and size per language. Because it is its
own file with its own literal Spanish VAs, it needs no coordinate
relocation at all: those VAs just need to resolve inside the SAME .data
copy the generator uses (there is exactly one sv_langgen instance per
module, so exactly one buffer).

So this tool ALSO copies TISPAN32's raw .text and .data sections VERBATIM
into the SAME buffer, at their own real addresses, translated only by
subtracting the buffer's base VA (0x1c24b000) -- never remapped to English
coordinates. Every pointer TISPAN32's own tables hold is already self-
consistent inside that verbatim copy (it was always a Spanish address to
begin with); nothing needs rewriting, only a wide enough buffer to hold it
at its natural offset. The low English-coordinate region (below) and this
high Spanish-coordinate region (above) are checked not to overlap; the
(large) gap between them is simply zero and nothing reads it.

USAGE
    extract_span_frontend.py TIENG32.DLL TISPAN32.DLL out/span_image.c \
        --symbol sv_span_image
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_generator import Image, find_rate_table  # noqa: E402

BUF_DVA = 0x1c24b000          # == TIENG32 .data VA; langgen.c's coordinate space
BUF_HI = 0x1c412000           # covers every address this port's Spanish files use

# langgen.c's TB() range: four 0x100-byte per-stress lookups at 0x1c205ff4,
# 0x1c2060f4, 0x1c2061f4, 0x1c2062f4 (IDX() produces a uint8_t index, so each
# table is exactly 0x100 bytes; the four are contiguous). The wider range
# 0x1c205fb9:0x1c206780 documented for extract_images.py's English --text-range
# includes unrelated bytes that are NOT byte-identical between the two DLLs
# (never read by langgen.c) -- narrowed here to exactly what is read, which
# IS confirmed byte-identical.
TEXT_LO_ENG, TEXT_HI_ENG = 0x1c205ff4, 0x1c2063f4

# (TIENG32 VA, size) -- overlaid with TISPAN32's own bytes at the rate-table delta.
OVERLAY_TABLES = [
    (0x1c24c3f0, 16),   # transition-percentage table (0x1c24c400 code-lookup table
                        # right after it is byte-identical, so NOT overlaid)
    (0x1c24c420, 64),   # nasalised-vowel target table
    (0x1c24c460, 96),   # manner-class pair table
]

# Byte-identical sanity checks (language-independent "universal constants"
# claim): (TIENG32 VA, size). Checked against the data (rate-table) delta.
IDENTITY_CHECKS = [
    (0x1c24c0d4, 0x1c24c117 - 0x1c24c0d4),   # track-name strings
    (0x1c24c1a0, 0x1c24c1b0 - 0x1c24c1a0),   # small lookup before the rate table
    (0x1c24c400, 0x1c24c420 - 0x1c24c400),   # phoneme-code lookup table
]
# Same claim for the small .text table, checked against the CODE delta
# (module +0x00 vtable field), a different region with a different offset.
TEXT_IDENTITY_CHECK = (TEXT_LO_ENG, TEXT_HI_ENG - TEXT_LO_ENG)

# Spanish's own front-end tables, real TISPAN32 addresses -- copied verbatim.
SPAN_TEXT_RANGE = (0x1c409aa0, 0x1c40b600)     # char classes .. end of .text
SPAN_DATA_RANGE = (0x1c40d000, BUF_HI)         # exception dict, numbers, spell...


def c_bytes(name, blob, linkage="static const"):
    out = ["%s unsigned char %s[%d] = {" % (linkage, name, len(blob))]
    for i in range(0, len(blob), 16):
        out.append("    " + "".join("0x%02x," % b for b in blob[i:i + 16]))
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("eng_dll")
    ap.add_argument("span_dll")
    ap.add_argument("out")
    ap.add_argument("--symbol", default="sv_span_image")
    args = ap.parse_args()

    eng = Image(args.eng_dll)
    span = Image(args.span_dll)

    ename, edva, evsize, erawsize, erawoff = eng.section(".data")
    if edva != BUF_DVA:
        raise SystemExit("TIENG32 .data VA is 0x%x, expected 0x%x (this tool "
                         "hardcodes langgen.c's coordinate space)" % (edva, BUF_DVA))

    rate_eng, _ = find_rate_table(eng)
    rate_span, _ = find_rate_table(span)
    delta = rate_span - rate_eng

    from extract_generator import descriptor_fields
    fe = descriptor_fields(eng)
    fs = descriptor_fields(span)
    code_delta = fs[0x00] - fe[0x00]

    # --- preflight: the "universal constants" claim, checked, not assumed ---
    for va, size in IDENTITY_CHECKS:
        e = eng.read(va, size)
        s = span.read(va + delta, size)
        if e != s:
            raise SystemExit("expected 0x%x..0x%x byte-identical between "
                             "TIENG32 and TISPAN32 (delta 0x%x); it is not. "
                             "eng=%s span=%s" % (va, va + size, delta, e.hex(), s.hex()))
    tva, tsize = TEXT_IDENTITY_CHECK
    e = eng.read(tva, tsize)
    s = span.read(tva + code_delta, tsize)
    if e != s:
        raise SystemExit("expected the small .text table 0x%x..0x%x byte-identical "
                         "(code delta 0x%x); it is not." % (tva, tva + tsize, code_delta))

    # --- build the buffer -----------------------------------------------
    size = BUF_HI - BUF_DVA
    buf = bytearray(size)
    # Low region: TIENG32's own .data, byte for byte (scratch reads as zero
    # already; universal constants are correct as-is; internal pointers stay
    # in TIENG32's own coordinate space, which is what langgen.c expects).
    eng_data = eng.read(BUF_DVA, evsize)
    buf[0:len(eng_data)] = eng_data

    # Overlay the three language-specific generator tables with Spanish bytes.
    for va, tsize in OVERLAY_TABLES:
        s = span.read(va + delta, tsize)
        off = va - BUF_DVA
        buf[off:off + tsize] = s

    # High region: TISPAN32's own .text and .data, verbatim, at their own
    # real addresses (not remapped) -- self-consistent internal pointers.
    tlo, thi = SPAN_TEXT_RANGE
    t = span.read(tlo, thi - tlo)
    off = tlo - BUF_DVA
    if off < len(eng_data):
        raise SystemExit("Spanish .text region 0x%x overlaps the English "
                         "generator region" % tlo)
    buf[off:off + len(t)] = t

    dlo, dhi = SPAN_DATA_RANGE
    d = span.read(dlo, dhi - dlo)
    off = dlo - BUF_DVA
    buf[off:off + len(d)] = d

    # --- small .text segment for langgen.c's TB() lookups ----------------
    text_lo_span = TEXT_LO_ENG + code_delta
    text_blob = span.read(text_lo_span, TEXT_HI_ENG - TEXT_LO_ENG)

    with open(args.out, "w") as f:
        f.write("/* GENERATED by tools/extract_span_frontend.py — do not edit, "
               "do not commit. */\n")
        f.write("/* sources: %s, %s */\n" % (os.path.basename(args.eng_dll),
                                              os.path.basename(args.span_dll)))
        f.write('#include "tispeech/narrate.h"\n\n')
        f.write("const uint32_t %s_va = 0x%08xu;\n" % (args.symbol, BUF_DVA))
        f.write("const uint32_t %s_size = 0x%xu;\n" % (args.symbol, size))
        f.write(c_bytes(args.symbol, bytes(buf), linkage="const"))
        f.write("\n\n/* langgen.c's small TB() range, TIENG32 0x%x..0x%x, "
               "byte-identical to TISPAN32 0x%x..0x%x. */\n"
               % (TEXT_LO_ENG, TEXT_HI_ENG, text_lo_span,
                  text_lo_span + (TEXT_HI_ENG - TEXT_LO_ENG)))
        f.write(c_bytes("%s_text0" % args.symbol, text_blob))
        f.write("\nconst sv_image %s_text[1] = {\n" % args.symbol)
        f.write("    {%s_text0, 0x%08xu, 0x%xu},\n"
               % (args.symbol, TEXT_LO_ENG, len(text_blob)))
        f.write("};\n")
        f.write("const size_t %s_text_count = 1;\n" % args.symbol)

    print("%s: buffer 0x%x..0x%x (%d bytes), rate-table delta 0x%x, "
         "code delta 0x%x, span .text [0x%x:0x%x), span .data [0x%x:0x%x)"
         % (args.out, BUF_DVA, BUF_HI, size, delta, code_delta,
            tlo, thi, dlo, dhi))


if __name__ == "__main__":
    main()
