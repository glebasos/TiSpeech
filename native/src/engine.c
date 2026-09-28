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

/* Resolve `count` consecutive VA pointers at `va` into host string pointers,
 * asserting shape the way extract_base.py does for the other tables: the
 * entry right after the last one must be NULL or an empty string, which is
 * how the original's own list-matcher (FUN_1c0057e0) knows a list has ended,
 * and how a differently-built DLL is caught rather than silently truncated. */
static int resolve_name_list(const sv_image *base, uint32_t va, int count,
                             const char **out)
{
    const uint8_t *ptrs = sv_image_ptr(base, va);
    if (!ptrs)
        return -1;
    for (int i = 0; i < count; i++) {
        out[i] = (const char *)sv_image_ptr(base, sv_rd32(ptrs + 4 * i));
        if (!out[i] || !out[i][0])
            return -1;
    }
    const uint8_t *sentinel = sv_image_ptr(base, sv_rd32(ptrs + 4 * count));
    if (sentinel && sentinel[0])
        return -1;
    return 0;
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

    /* Inline-command parser tables, FUN_1c005180 and callees (narrate.c). */
    const uint8_t *kw = sv_image_ptr(base, 0x1c0126b0u);
    const uint8_t *notes = sv_image_ptr(base, 0x1c012b58u);
    if (!kw || !notes)
        return -1;
    for (int k = 0; k < SV_CMD_KEYWORD_COUNT; k++) {
        t->cmd_keywords[k].name = (const char *)sv_image_ptr(base, sv_rd32(kw + 6 * k));
        t->cmd_keywords[k].code = sv_rd16(kw + 6 * k + 4);
        if (!t->cmd_keywords[k].name)
            return -1;
    }
    for (int k = 0; k < 7; k++)
        t->cmd_notes[k] = (int16_t)sv_rd16(notes + 2 * k);

    if (resolve_name_list(base, 0x1c012918u, 3, t->cmd_language) ||
        resolve_name_list(base, 0x1c012830u, 20, t->cmd_voice) ||
        resolve_name_list(base, 0x1c012888u, 4, t->cmd_tract) ||
        resolve_name_list(base, 0x1c0128a0u, 9, t->cmd_glot) ||
        resolve_name_list(base, 0x1c0128c8u, 3, t->cmd_voicing) ||
        resolve_name_list(base, 0x1c0128d8u, 5, t->cmd_f0style) ||
        resolve_name_list(base, 0x1c0128f0u, 4, t->cmd_speak) ||
        resolve_name_list(base, 0x1c012908u, 2, t->cmd_onoff))
        return -1;

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
