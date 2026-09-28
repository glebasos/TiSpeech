/*
 * langmod_priv.h — private state of a language module instance, shared by
 * the frame generator (langgen.c) and the text front end (textphon_eng.c).
 *
 * Both are ports of code that keeps its state in module globals. Each module
 * instance owns a mutable copy of the module's .data section, and globals
 * and tables are addressed by their ORIGINAL virtual address through the
 * accessors below. Read-only tables the module keeps inside .text are
 * extracted as separate segments and reached through TB().
 */

#ifndef TISPEECH_LANGMOD_PRIV_H
#define TISPEECH_LANGMOD_PRIV_H

#include "tispeech/narrate.h"

#if defined(__GNUC__) || defined(__clang__)
typedef int16_t sv_a16 __attribute__((may_alias, aligned(1)));
typedef uint16_t sv_au16 __attribute__((may_alias, aligned(1)));
typedef int32_t sv_a32 __attribute__((may_alias, aligned(1)));
#else
typedef int16_t sv_a16;
typedef uint16_t sv_au16;
typedef int32_t sv_a32;
#endif

#define SV_LANGGEN_MAX_TEXT 4

typedef struct sv_langgen {
    uint8_t *d;            /* mutable copy of .data */
    uint32_t dva, dsize;
    sv_image text[SV_LANGGEN_MAX_TEXT]; /* read-only tables in .text */
    size_t ntext;
    sv_frame *frame;       /* 0x1c250e58, the frame being written */
    /* Stack locals of 0x1c204690 that are read before they are written in
     * some calls, and so carry over from the previous call exactly as the
     * original's stack slots do (5840 calls the generator back to back at
     * one stack depth). Offsets are the original's [esp+N]. */
    int32_t s3c, s40, s44, s48; /* smoothed higher-formant values, (re)set
                                 * only when INIT is set */
    int32_t s28;                /* B2: not set on the nasal path */
    uint32_t s10;               /* bytes +1..+3: event param / event word */
    uint32_t s1c;               /* seeds F1..F3 registers for the frame loop */
} sv_langgen;

#define G ((sv_langgen *)e->lang->priv)

/* .data by original VA: byte, int16, uint16, int32. */
#define DP(a)  (G->d + ((uint32_t)(a) - G->dva))
#define B(a)   (*(uint8_t *)DP(a))
#define W(a)   (*(sv_a16 *)DP(a))
#define UW(a)  (*(sv_au16 *)DP(a))
#define L(a)   (*(sv_a32 *)DP(a))
/* .text tables by original VA. An address outside every extracted segment
 * would read the original's code; 0 is returned instead. */
static inline uint8_t sv_tb(const sv_langgen *g, uint32_t va)
{
    for (size_t k = 0; k < g->ntext; k++)
        if (va - g->text[k].va < g->text[k].size)
            return g->text[k].bytes[va - g->text[k].va];
    return 0;
}
static inline const uint8_t *sv_tp_ptr(const sv_langgen *g, uint32_t va)
{
    for (size_t k = 0; k < g->ntext; k++)
        if (va - g->text[k].va < g->text[k].size)
            return g->text[k].bytes + (va - g->text[k].va);
    return NULL;
}
#define TB(a)  sv_tb(G, (uint32_t)(a))


#endif /* TISPEECH_LANGMOD_PRIV_H */
