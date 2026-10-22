#ifndef DC_ANALYSIS_H
#define DC_ANALYSIS_H

#include "loader.h"
#include "sh4.h"

/* What a halfword in the image turned out to be. */
enum {
    BYTE_UNKNOWN = 0,
    BYTE_CODE,        /* reached by control flow */
    BYTE_DATA,        /* literal pool / table, referenced by mov.l/mov.w/mova */
};

typedef struct {
    uint32_t start;   /* entry address */
    uint32_t end;     /* one past the last instruction (incl. delay slot) */
    uint32_t flags;   /* FUNC_* */
} dc_func;

enum {
    FUNC_FROM_ENTRY   = 1u << 0,  /* program entry point */
    FUNC_FROM_BSR     = 1u << 1,  /* direct bsr target */
    FUNC_FROM_JSR     = 1u << 2,  /* jsr @Rn with Rn loaded from a literal */
    FUNC_FROM_TAIL    = 1u << 3,  /* jmp @Rn tail call */
    FUNC_FROM_PTR     = 1u << 4,  /* found in a pointer table */
    FUNC_FROM_GAP     = 1u << 5,  /* found scanning untraced space (prologue or leaf) */
    FUNC_HAS_UNRESOLVED_JUMP = 1u << 8,  /* jmp/braf we could not resolve */
    FUNC_HAS_JUMP_TABLE      = 1u << 9,  /* switch table followed into its cases */
};

typedef struct {
    const dc_image *img;
    uint32_t base, size;   /* analysed range */
    uint8_t *kind;         /* BYTE_* per halfword: kind[(addr - base) / 2] */
    uint8_t *written;      /* per halfword: a literal that code also stores to */
    dc_func *funcs;
    int nfuncs;
} dc_analysis;

#define DC_MAX_TABLE 256   /* most entries a jump table can have */

/* Discover functions starting from img->entry. Returns 1 on success.
 * a->funcs is sorted by start address afterwards. */
int  dc_analyze(dc_analysis *a, const dc_image *img, uint32_t base, uint32_t size);
void dc_analysis_free(dc_analysis *a);

/* ---- queries for later stages (the emitter); valid after dc_analyze ---- */

/* The function starting exactly at addr, or NULL. */
const dc_func *dc_func_at(const dc_analysis *a, uint32_t addr);

/* 1 if the literal-pool word at addr is also written by the game (Shinobi
 * keeps interrupt variables in its literal pools), so it's not a constant. */
int dc_literal_written(const dc_analysis *a, uint32_t addr);

/* The constant held in `reg` just before the instruction at pc, if it can be
 * found by walking back through straight-line code. Returns 1 if found. */
int dc_resolve_reg(const dc_analysis *a, uint32_t pc, unsigned reg, uint32_t *value);

/* Targets of the jump-table jmp/braf at pc, inside the function starting at
 * func_start. Fills up to `max` targets and returns how many (0 if pc is not
 * a table jump). *dispatch is set when the targets are separate functions
 * reached by tail call rather than case labels of this function. */
int dc_jump_table(const dc_analysis *a, uint32_t func_start, uint32_t pc,
                  uint32_t *targets, int max, int *dispatch);

#endif
