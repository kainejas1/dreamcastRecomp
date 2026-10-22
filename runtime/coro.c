#include "sh4_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#endif

/*
 * The game's setjmp/longjmp, for games that switch threads with them (SA2's
 * scheduler runs from the vsync interrupt: setjmp(scheduler), longjmp(thread),
 * and a thread yields with if (!setjmp(self)) longjmp(scheduler)).
 *
 * The game's own setjmp/longjmp code still runs and saves/loads its registers.
 * On top of that:
 *  - each setjmp call site takes a host setjmp (DC_SETJMP), remembered here
 *    under the game's jmp_buf address, with the fiber it ran on
 *  - longjmp finds that, switches to the fiber and longjmps there on the host.
 *    That frame is still alive: it's a game thread that is waiting
 *  - a jmp_buf nobody setjmp'd (the game builds new threads' contexts by hand)
 *    gets a new fiber that starts at the pc the game's longjmp loaded (pr)
 *
 * A record is only used if r15 and pr match what setjmp saw, so a thread slot
 * reused for a new thread doesn't resume the old one.
 */

#define MAX_JMPS    64
#define FIBER_STACK (8u << 20)   // reserved, not committed

typedef struct fiber {
#ifdef _WIN32
    void *h;
#else
    ucontext_t uc;
    void *stack;
#endif
} fiber;

typedef struct {
    uint32_t buf;        // the game's jmp_buf, 0 = free
    uint32_t sp, pr;     // r15 and pr at the setjmp
    fiber *f;
    dc_jmpbuf host;
} jmp_rec;

static jmp_rec recs[MAX_JMPS];
static fiber main_fiber, *current;
static jmp_rec *resume;      // set before switching to a fiber that should longjmp somewhere
static fiber *dead;          // fiber nothing can come back to, freed by the next one to run
static sh4_ctx *start_ctx;   // for a new fiber
static int nfibers;

static fiber *fiber_current(void)
{
    if (!current) {
#ifdef _WIN32
        main_fiber.h = ConvertThreadToFiber(NULL);
        if (!main_fiber.h) { fprintf(stderr, "[coro] ConvertThreadToFiber failed\n"); exit(1); }
#endif
        current = &main_fiber;
    }
    return current;
}

static void fiber_switch(fiber *to)
{
    fiber *from = fiber_current();
    current = to;
#ifdef _WIN32
    (void)from;
    SwitchToFiber(to->h);
#else
    swapcontext(&from->uc, &to->uc);
#endif
}

static void free_dead(void)
{
    if (!dead) return;
#ifdef _WIN32
    DeleteFiber(dead->h);
#else
    free(dead->stack);
#endif
    free(dead);
    dead = NULL;
    nfibers--;
}

// we just got switched to: continue where the record says
static void arrived(void)
{
    jmp_rec *r = resume;
    resume = NULL;
    free_dead();
    if (!r) { fprintf(stderr, "[coro] fiber resumed with nowhere to go\n"); exit(1); }
#if defined(__GNUC__)
    __builtin_longjmp(r->host, 1);
#else
    longjmp(r->host, 1);
#endif
}

#ifdef _WIN32
static void WINAPI fiber_start(void *arg)
#else
static void fiber_start(void)
#endif
{
#ifdef _WIN32
    (void)arg;
#endif
    sh4_ctx *ctx = start_ctx;
    free_dead();
    // the game's longjmp ended with rts: carry on at pr
    dc_call(ctx, ctx->pr);
    fprintf(stderr, "[coro] game thread started at %08x returned\n", ctx->pr);
    exit(1);
}

static fiber *fiber_new(void)
{
    fiber *f = calloc(1, sizeof *f);
    if (!f) { fprintf(stderr, "[coro] out of memory\n"); exit(1); }
#ifdef _WIN32
    f->h = CreateFiberEx(64 * 1024, FIBER_STACK, 0, fiber_start, NULL);
    if (!f->h) { fprintf(stderr, "[coro] CreateFiber failed\n"); exit(1); }
#else
    f->stack = malloc(FIBER_STACK);
    if (!f->stack || getcontext(&f->uc)) { fprintf(stderr, "[coro] can't make a fiber\n"); exit(1); }
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = FIBER_STACK;
    f->uc.uc_link = NULL;
    makecontext(&f->uc, fiber_start, 0);
#endif
    nfibers++;
    return f;
}

static jmp_rec *find(uint32_t buf)
{
    for (int i = 0; i < MAX_JMPS; i++)
        if (recs[i].buf == buf) return &recs[i];
    return NULL;
}

dc_jmpbuf *dc_setjmp_here(sh4_ctx *ctx, uint32_t buf)
{
    jmp_rec *r = find(buf);
    if (!r) r = find(0);
    if (!r) { fprintf(stderr, "[coro] more than %d jmp_bufs in use\n", MAX_JMPS); exit(1); }
    r->buf = buf;
    r->sp = ctx->r[15];
    r->pr = ctx->pr;
    r->f = fiber_current();
    return &r->host;
}

// can anything still switch back to f?
static int waited_on(const fiber *f)
{
    if (f == &main_fiber) return 1;
    for (int i = 0; i < MAX_JMPS; i++)
        if (recs[i].buf && recs[i].f == f) return 1;
    return 0;
}

void dc_longjmp(sh4_ctx *ctx, uint32_t buf)
{
    static int coro_log = -1;   // DC_COROLOG=n: print the first n switches
    if (coro_log < 0) coro_log = getenv("DC_COROLOG") ? atoi(getenv("DC_COROLOG")) : 0;

    fiber *from = fiber_current();
    jmp_rec *r = find(buf);
    if (r && r->sp == ctx->r[15] && r->pr == ctx->pr) {
        if (coro_log > 0) { coro_log--; fprintf(stderr, "[coro] longjmp %08x: back to setjmp (sp %08x pr %08x)%s\n",
                                      buf, r->sp, r->pr, r->f == from ? "" : ", other fiber"); }
        if (r->f == from) {
#if defined(__GNUC__)
            __builtin_longjmp(r->host, 1);
#else
            longjmp(r->host, 1);
#endif
        }
        resume = r;
        if (!waited_on(from)) dead = from;
        fiber_switch(r->f);
        arrived();
    }

    // a context the game made itself: a new thread
    if (r) r->buf = 0;   // stale
    if (coro_log > 0) { coro_log--; fprintf(stderr, "[coro] longjmp %08x: new thread at %08x, sp %08x (%d fibers)\n",
                                  buf, ctx->pr, ctx->r[15], nfibers + 1); }
    fiber *f = fiber_new();
    start_ctx = ctx;
    resume = NULL;
    if (!waited_on(from)) dead = from;
    fiber_switch(f);
    arrived();
}
