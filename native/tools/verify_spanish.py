#!/usr/bin/env python3
"""Compare the Spanish front end and optional full PCM pipeline with TISPAN32.
The original runs under Unicorn only in this development tool. No dictionary
or phoneme tables are embedded here; inputs are ordinary text and synthetic
user dictionaries. --numbers adds random numeric tokens of every shape the
normaliser dispatches on (cardinals, currency, percent, decimals, years, times,
durations, dates, phone numbers) plus malformed ones that fall back to spelling.
"""
import argparse
import ctypes
import random

from sv_emu import Emu
from verify_narrate import Recon, STAGES, compare, _snapshot_and_narrate, ud_build

TEXTS = [
    'hola mundo', 'Buenos días. ¿Cómo estás?', 'El niño come una manzana.',
    'La rápida zorra marrón salta sobre el perro perezoso.',
    'España, México y Argentina hablan español.', 'canción árbol café lápiz',
    'Hoy hace mucho frío, pero mañana hará calor.', 'pingüino vergüenza lingüística',
    'El ferrocarril atraviesa la ciudad.', 'acción examen reloj salud virtud',
    'a e i o u agua aire tierra fuego', 'Hola, mundo? Sí... "bien" (gracias)!',
    'CASA casa Casa', 'uno dos tres cuatro cinco seis siete ocho nueve diez',
    'El sol brilla. La luna sale.', 'instrucción transporte psicología atleta',
    'héroe país poeta suave viaje ruido', 'muy bien, muchas gracias',
    'Tengo 21 años y 100 pesos.', 'Son las 12:30, el 5/4/96.', 'Cuesta $1.01 o $5.20.',
    'Llama al (555) 123-4567 o 1-800-555-1212.', 'En 1996 y 2000, 12% y 3.14.',
    'Hay 1,234,567 personas y 2 millones de libros.', 'Duró 1:02:03.',
]
WORDS = ('hola mundo casa perro gato calle ciudad campo árbol sol luna agua aire fuego '
         'tierra suave fuerte rápido lento rojo verde blanco negro pan leche café niño '
         'niña hombre mujer ferrocarril pueblo pequeño grande bueno acción nación '
         'corazón reloj virtud azúcar lápiz música pájaro pingüino mañana hoy ayer').split()


ONSETS = ['', 'b', 'c', 'ch', 'd', 'f', 'g', 'gu', 'j', 'l', 'll', 'm', 'n', 'ñ', 'p', 'qu',
          'r', 'rr', 's', 't', 'v', 'y', 'z', 'bl', 'br', 'cl', 'cr', 'dr', 'fl', 'fr', 'gr',
          'pl', 'pr', 'tr']
NUCLEI = ['a', 'e', 'i', 'o', 'u', 'á', 'é', 'í', 'ó', 'ú', 'ai', 'au', 'ei', 'eu', 'ia',
          'ie', 'io', 'iu', 'oi', 'ua', 'ue', 'ui', 'uo', 'üe', 'üi']
CODAS = ['', '', '', 'n', 's', 'r', 'l', 'd', 'z', 'x', 'm']
# All twenty personalities through the inline command: every st_52 voice
# type, the breathy glottal source (st_2f2 = 1) and both alternate F0 modes.
VOICES = [''] + ['{voice %s}' % v for v in (
    'male female largemale child giantmale mellowfemale mellowmale crispmale thefly robotoid '
    'martian colossus fastfred oldwoman munchkin troll nerd milktoast tipsy choirboy').split()]


def random_words(rng, n):
    """Pseudo-Spanish words: every onset, glide and accent in combination."""
    out = []
    for _ in range(n):
        w = ''.join(rng.choice(ONSETS) + rng.choice(NUCLEI) + rng.choice(CODAS)
                    for _ in range(rng.randint(1, 4)))
        out.append(w.capitalize() if rng.random() < 0.1 else w)
    return out


def random_number(rng):
    """One numeric token, in the shapes 0x1C407e20's pattern table and n2w
    recognise, and a few they reject."""
    d = lambda n: ''.join(rng.choice('0123456789') for _ in range(n))
    nz = lambda: rng.choice('123456789')
    def grouped():
        head = nz() + d(rng.randint(0, 2))
        return ','.join([head] + [d(3) for _ in range(rng.randint(0, 4))])
    shape = rng.randrange(16)
    if shape == 0:
        return str(rng.choice([0, 1, 2, 10, 11, 15, 16, 20, 21, 29, 30, 31, 99, 100, 101,
                               110, 115, 120, 121, 200, 201, 500, 555, 700, 777, 900, 999]))
    if shape == 1:
        return nz() + d(rng.randint(0, 2))
    if shape == 2:
        return grouped()
    if shape == 3:
        return '$' + rng.choice([grouped(), '1', '0', '']) + rng.choice(['', '.' + d(2), '.' + d(1), '.' + d(3)])
    if shape == 4:
        return rng.choice([grouped(), '0', '']) + '.' + d(rng.randint(1, 4)) + rng.choice(['', '%'])
    if shape == 5:
        return grouped() + '%'
    if shape == 6:
        return rng.choice([nz() + d(3), '0' + d(3), '1' + d(3), '2' + d(3)])
    if shape == 7:
        return '%d:%s' % (rng.randint(0, 13), d(2))
    if shape == 8:
        return '%d:%s:%s' % (rng.randint(0, 120), d(2), d(2))
    if shape == 9:
        return '%d/%d/%s' % (rng.randint(1, 12), rng.randint(1, 31), d(2))
    if shape == 10:
        return d(3) + '-' + d(4)
    if shape == 11:
        return '(%s) %s-%s' % (d(3), d(3), d(4))
    if shape == 12:
        return '%s-%s-%s-%s' % (d(1), d(3), d(3), d(4))
    if shape == 13:
        return rng.choice(['%s-%s-%s', '%s/%s-%s']) % (d(3), d(3), d(4))
    if shape == 14:
        return d(5) + rng.choice(['', '-' + d(4)])
    # Malformed or odd: bad grouping, stray punctuation, letters, scale words.
    return rng.choice([d(4) + ',' + d(2), '1,00', '12,34,567', '+' + d(3), d(2) + '-' + d(2),
                       '1.2.3', '(' + d(2), d(3) + 'A', '$' + d(2) + ' MILLONES',
                       grouped() + ' BILLONES', '$' + grouped() + ' TRILLONES',
                       d(2) + ' QUADRILLONES)', '5 MILLON', '(3 MILLONES)'])


def original_tts_phonemes(e, raw, flags=0):
    """_SVTTS@32's phoneme half (TIBASE32 0x1c00fa03..0x1c00fb68), driving the
    ORIGINAL SVTextToPhon: a (strlen + 10) * 3 buffer (* 6 when spelling);
    a partial result -1 - ofs restarts the text at ofs in a new buffer, -1
    (not even one word fit) retries with the multiplier grown by 2. What the
    public ABI must reproduce when the phonemes outgrow the first buffer,
    which number expansion easily does."""
    mult = 6 if flags & 4 else 3
    chunks, rc = [], 0
    while True:
        if rc == -1:
            mult += 2
        rc, phon = e.text_to_phon(raw, flags=flags, cap=(len(raw) + 10) * mult)
        rc = ctypes.c_int32(rc).value
        if rc == 0:
            return b''.join(chunks + [phon])
        if rc > 0 or mult > 64:
            raise RuntimeError('SVTextToPhon -> %#x' % rc)
        if rc != -1:
            chunks.append(phon)
            raw = raw[-(rc + 1):]


def new_emu(dlls):
    e = Emu(dlls, languages=('TISPAN32.DLL',))
    e.open(lang=2)
    return e


class Frontend:
    def __init__(self, library):
        self.lib = ctypes.CDLL(library)
        self.fn = self.lib.textphon_span_oracle_text_to_phon
        self.fn.restype = ctypes.c_int32
        self.fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
                            ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32]

    def convert(self, text, flags=0, cap=4096, dictionary=None):
        out = ctypes.create_string_buffer(cap)
        rc = self.fn(dictionary, len(dictionary or b''), text, out, cap, flags)
        return rc, out.value


class Public:
    """libtispeech's own ABI: UTF-8 text in, phonemes, then PCM."""
    def __init__(self, library):
        self.lib = ctypes.CDLL(library)
        self.lib.tispeech_text_to_phonemes.argtypes = [
            ctypes.c_uint32, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32]
        self.lib.tispeech_synthesize.argtypes = [
            ctypes.c_uint32, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]
        self.lib.tispeech_free_samples.argtypes = [ctypes.c_void_p]

    def run(self, text):
        out = ctypes.create_string_buffer(4096)
        rc = self.lib.tispeech_text_to_phonemes(2, text.encode('utf-8'), out, 4096)
        if rc:
            return rc, None, None
        ptr, n, rate = ctypes.c_void_p(), ctypes.c_int32(), ctypes.c_int32()
        rc = self.lib.tispeech_synthesize(2, out.value, ctypes.byref(ptr), ctypes.byref(n),
                                          ctypes.byref(rate))
        pcm = ctypes.string_at(ptr, n.value) if ptr.value else b''
        self.lib.tispeech_free_samples(ptr)
        return rc, out.value, pcm


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dlls', required=True)
    ap.add_argument('--library', required=True)
    ap.add_argument('--narrate-library')
    ap.add_argument('--public-library',
                    help='libtispeech: also check text -> PCM through the public ABI')
    ap.add_argument('--words', type=int, default=0)
    ap.add_argument('--random', type=int, default=0,
                    help='also this many texts of random pseudo-Spanish words')
    ap.add_argument('--numbers', type=int, default=0,
                    help='also this many texts of random numeric tokens')
    ap.add_argument('--voices', action='store_true',
                    help='narrate each text under every VOICES prefix')
    ap.add_argument('--text', action='append')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()
    rng = random.Random(19961118)
    texts = (a.text or TEXTS) + [' '.join(rng.choices(WORDS, k=rng.randint(1, 12)))
                                + rng.choice(['.', '?', '!', ',']) for _ in range(a.words)]
    texts += [' '.join(random_words(rng, rng.randint(1, 10))) + rng.choice(['.', '?', '!', ','])
              for _ in range(a.random)]
    texts += [' '.join(rng.choice([random_number(rng)] * 3 + random_words(rng, 1))
                       for _ in range(rng.randint(1, 5))) + rng.choice(['.', '?', '', ','])
              for _ in range(a.numbers)]
    frontend = Frontend(a.library)
    recon = Recon(a.narrate_library) if a.narrate_library else None
    public = Public(a.public_library) if a.public_library else None
    failures, total = 0, 0
    for text in texts:
        raw = text.encode('latin-1')
        # Reset the original's per-handle state between texts.
        e = new_emu(a.dlls)
        cases = [(0, 4096)] if recon else [(f, c) for f in (0, 2, 4, 8, 10, 16)
                                         for c in (8, 16, 32, 64, 4096)]
        for flags, cap in cases:
            total += 1
            rc, phon = e.text_to_phon(raw, flags=flags, cap=cap)
            original = (ctypes.c_int32(rc).value, phon)
            mine = frontend.convert(raw, flags=flags, cap=cap)
            if original != mine:
                failures += 1
                if failures <= 12:
                    print('FAIL frontend %r flags=%d cap=%d original=%r native=%r' %
                          (text, flags, cap, (original[0], original[1][:180]), (mine[0], mine[1][:180])))
                continue
            for voice in (VOICES if recon and a.voices else [''] if recon else []):
                if voice:
                    total += 1
                    e = new_emu(a.dlls)
                phon_v = voice.encode() + phon
                sentences, pcm = _snapshot_and_narrate(e, phon_v)
                bad = None
                for s, snaps in enumerate(sentences):
                    for k, (name, _, _) in enumerate(STAGES):
                        if name in snaps:
                            difference = compare(snaps[name], recon.run(phon_v, s, k))
                            if difference:
                                bad = '%s: %s' % (name, difference)
                                break
                    if bad:
                        break
                mine_pcm = recon.pcm(phon_v) if not bad else None
                if not bad and mine_pcm != pcm:
                    bad = 'PCM differs: original=%d samples, native=%s' % (
                        len(pcm), len(mine_pcm) if isinstance(mine_pcm, bytes) else mine_pcm)
                if not bad and public and not voice:
                    total += 1
                    rc, pub_phon, pub_pcm = public.run(text)
                    tts_phon = original_tts_phonemes(new_emu(a.dlls), raw)
                    if tts_phon != phon:
                        # SVTTS re-chunked: narrate what it produced instead.
                        _, pcm = _snapshot_and_narrate(new_emu(a.dlls), tts_phon)
                        phon = tts_phon
                    if (rc, pub_phon, pub_pcm) != (0, phon, pcm):
                        bad = 'public ABI: rc=%#x phonemes %s, PCM %s' % (
                            rc, 'match' if pub_phon == phon else 'differ',
                            'matches' if pub_pcm == pcm else 'differs')
                if bad:
                    failures += 1
                    print('FAIL pipeline %s%r: %s' % (voice, text, bad))
                elif a.verbose:
                    print('ok %s%r: %d exact samples' % (voice, text, len(pcm)))
    if not recon:
        # Exercise the Spanish dictionary hook before exceptions/LTS.
        dictionary = ud_build([(b'HOLA', b' OW5LAH', 2), (b'MUNDO', b' MUH5NDOW', 2)])
        e = new_emu(a.dlls)
        e.register_file('span.svx', dictionary)
        path = e.malloc(9)
        e.mu.mem_write(path, b'span.svx\0')
        assert e.call(e.exp('_SVLoadUserDictionary@8'), [e.h, path]) == 0
        for text in (b'hola', b'Hola mundo', b'la casa', b'HOLA MUNDO'):
            total += 1
            rc, phon = e.text_to_phon(text)
            mine = frontend.convert(text, dictionary=dictionary)
            if (ctypes.c_int32(rc).value, phon) != mine:
                failures += 1
                print('FAIL dictionary %r: original=%r native=%r' % (text, (rc, phon), mine))
    print('%s: Spanish %d/%d comparisons match%s' % (
        'PASS' if not failures else 'FAIL', total - failures, total,
        ' through PCM' if recon else ' (flags, buffer retries, user dictionary)'))
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
