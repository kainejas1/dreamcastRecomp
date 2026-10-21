#ifndef DC_EMIT_H
#define DC_EMIT_H

#include "analysis.h"

typedef struct {
    const char *out_dir;    /* where funcs.h, funcs_NNN.c and lookup.c go */
    uint32_t only;          /* nonzero: emit just this function, to stdout */
    int funcs_per_file;     /* functions per funcs_NNN.c (default 200) */
} dc_emit_options;

/* Translate every function found by dc_analyze into C. Returns 1 on success.
 */
int dc_emit(const dc_analysis *a, const dc_emit_options *opt);

#endif
