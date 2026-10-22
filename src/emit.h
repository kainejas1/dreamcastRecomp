#ifndef DC_EMIT_H
#define DC_EMIT_H

#include "analysis.h"

typedef struct {
    const char *out_dir;    /* where funcs.h, funcs_NNN.c and lookup.c go */
    uint32_t only;          /* nonzero: emit just this function, to stdout */
    int funcs_per_file;     /* functions per funcs_NNN.c (default 200) */
    uint32_t setjmp_fn;     /* the game's setjmp/longjmp, 0 if none: calls to */
    uint32_t longjmp_fn;    /* them go through DC_SETJMP / DC_LONGJMP */
} dc_emit_options;

/* Translate every function found by dc_analyze into C. Returns 1 on success.
 */
int dc_emit(const dc_analysis *a, const dc_emit_options *opt);

/* A module: code the game loads at runtime (SA2 unpacks one at a time to
 * 0x8C500000), analysed on its own. What the runtime needs to know about it: */
typedef struct {
    char name[32];          /* "STG09"; its functions are m_stg09_XXXXXXXX */
    uint32_t base, size;
    uint8_t magic[8];       /* its first 8 bytes */
    uint32_t sig_addr, sig_len, sig_hash;   /* FNV-1a of code there, tells
                                               modules with the same magic apart */
    int nfuncs;
} dc_module_info;

/* Emit a module's functions (m_NAME.h, m_NAME_NNN.c, m_NAME_lookup.c). Calls
 * into the main program, `main` (can be NULL), are direct. */
int dc_emit_module(const dc_analysis *a, const dc_analysis *main, const dc_emit_options *opt,
                   const char *name, dc_module_info *info);

/* modules.c: the runtime's table of modules (written even with n == 0). */
int dc_emit_registry(const dc_emit_options *opt, const dc_module_info *mods, int n);

#endif
