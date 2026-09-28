/*
 * engine.c — wiring between the extracted .data images and the pipeline.
 *
 * No reconstructed logic here: this resolves the virtual addresses the
 * original hardcodes (or publishes through LoadLanguage) into host pointers
 * into the extracted sections.
 */

#include "tispeech/narrate.h"

const uint8_t *sv_image_ptr(const sv_image *im, uint32_t va)
{
    if (!im || !im->bytes || va < im->va || va - im->va >= im->size)
        return NULL;
    return im->bytes + (va - im->va);
}

int sv_nar_tables_init(sv_nar_tables *t, const sv_image *base)
{
    /* 0x1c012a68: 60 name-list pointers. The last ('[') is NULL in the
     * original, which the parser dereferences; kept NULL here. */
    const uint8_t *names = sv_image_ptr(base, 0x1c012a68u);
    t->voices = sv_image_ptr(base, 0x1c013600u);
    if (!names || !t->voices)
        return -1;
    for (int k = 0; k < 60; k++)
        t->phoneme_names[k] = sv_image_ptr(base, sv_rd32(names + 4 * k));
    return 0;
}

int sv_langmod_init(sv_langmod *m, const sv_image *lang, const uint32_t desc[10])
{
    m->id = (int)desc[0x10 / 4];
    m->phonemes_a = sv_image_ptr(lang, desc[0x14 / 4]);
    m->phonemes_b = sv_image_ptr(lang, desc[0x18 / 4]);
    m->classes = sv_image_ptr(lang, desc[0x1c / 4]);
    m->voices = sv_image_ptr(lang, desc[0x20 / 4]);
    m->rules = sv_image_ptr(lang, desc[0x24 / 4]);
    if (!m->phonemes_a || !m->phonemes_b || !m->classes || !m->voices || !m->rules)
        return -1;
    return 0;
}
