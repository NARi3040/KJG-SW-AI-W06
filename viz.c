/*
 * viz.c - Advanced heap visualizer + checker for Malloc Lab. Does NOT touch mm.c.
 *
 * Linker --wrap intercepts the driver's mm_init/mm_malloc/mm_free/mm_realloc calls,
 * runs the real ones, dumps the heap to stderr, tracks stats and checks invariants.
 * Problems print in bold red. On SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL it prints the
 * faulting call, fault address, recent operations history, a backtrace and the heap
 * state at crash.
 *
 * Build:
 *   make -f Makefile.viz
 *
 * Visualization modes (via VIZ environment variable):
 *   ./mdriver-viz -V -f short1-bal.rep                 Text table with stats (default)
 *   VIZ=bar ./mdriver-viz -V -f short1-bal.rep         ANSI colored bars
 *   VIZ=map ./mdriver-viz -V -f short1-bal.rep         Compact one-line block map
 *   VIZ=error ./mdriver-viz -V -f short1-bal.rep       Quiet mode: dumps ONLY on error/crash
 *   VIZ=step ./mdriver-viz -V -f short1-bal.rep        Interactive step-by-step (press Enter)
 *
 * Filtering options:
 *   VIZ_START=10 VIZ_END=20 ./mdriver-viz ...          Dump only operations 10 to 20
 *   VIZ_OP=15 ./mdriver-viz ...                        Dump only operation 15
 *   VIZ_LINK=1 ./mdriver-viz ...                       Show explicit free list link hints (next/prev)
 *
 * Source locations: crashes print the faulting file:line and a resolved call stack; heap
 * errors print where the driver called mm_* and where that entry point is implemented
 * (addr2line, needs -g).
 *
 * Debugging options:
 *   VIZ_STOP=1 ./mdriver-viz ...                       abort() at the first detected problem
 *                                                      (core dump, or break inside gdb)
 *
 * Advanced features:
 *   - Recent operation history ring buffer (printed on crash and on the first error)
 *   - External fragmentation & memory utilization profiling
 *   - Live block table: overlapping allocations, free of a pointer that malloc never
 *     returned, NULL returns for non-zero requests
 *   - Per-run report (state is reset at every mm_init): leak list with the op that
 *     allocated each block
 *   - Free block explicit list (next/prev pointer) heuristic inspection + validation
 *   - Double-free, invalid pointer, alignment, heap-boundary checks
 *   - Header/footer consistency & coalescing verification
 *   - Crash backtrace (link with -rdynamic for symbol names)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <elf.h>
#include <ucontext.h>

#include "mm.h"
#include "memlib.h"

FILE *__real_fopen(const char *path, const char *mode);
int __real_mm_init(void);
void *__real_mm_malloc(size_t size);
void __real_mm_free(void *ptr);
void *__real_mm_realloc(void *ptr, size_t size);

#define HDR(p)    (*(unsigned int *)((char *)(p) - 4))
#define SIZE(p)   (HDR(p) & ~0x7u)
#define ALLOC(p)  (HDR(p) & 0x1u)

#define CHK_NONE  0
#define CHK_ALLOC 1   /* chk must be the start of an allocated block >= n */
#define CHK_FREED 2   /* block containing chk must be free */

enum viz_mode {
    MODE_TABLE = 0,
    MODE_BAR,
    MODE_MAP,
    MODE_ERROR,
    MODE_STEP
};

/* call in progress, for the crash report (NULL op = not inside mm_*) */
static const char *cur_op = NULL;
static size_t cur_n = 0;
static void *cur_ptr = NULL;

/* problems found in the current call */
#define ERR_CAP 32
static char errs[ERR_CAP][160];
static int nerr = 0;
static int err_muted = 0;     /* set during the print pass so errors are not collected twice */

/* counters: total_* are over the whole process, run_err/nops are per mm_init run */
static int total_err = 0, run_err = 0, nops = 0;
static int total_runs = 0, total_calls = 0;

/* filter range */
static int filter_start = -1;
static int filter_end = -1;
static int show_link_hints = -1;
static int stop_on_error = 0;

/* ring buffer for recent operations */
#define RING_CAP 16
typedef struct {
    int op_num;
    const char *op_name;
    size_t size;
    void *ptr;
    void *res_bp;
} op_log_t;

static op_log_t op_ring[RING_CAP];
static int op_ring_head = 0;
static int op_ring_count = 0;

static void ring_push(const char *op, size_t size, void *ptr, void *res_bp)
{
    int idx = (op_ring_head + op_ring_count) % RING_CAP;
    if (op_ring_count == RING_CAP) {
        op_ring_head = (op_ring_head + 1) % RING_CAP;
    } else {
        op_ring_count++;
    }
    op_ring[idx].op_num = nops;
    op_ring[idx].op_name = op;
    op_ring[idx].size = size;
    op_ring[idx].ptr = ptr;
    op_ring[idx].res_bp = res_bp;
}

static void print_ring_history(void)
{
    char *lo = (char *)mem_heap_lo();
    if (op_ring_count == 0)
        return;

    fprintf(stderr, "\n\033[1;33m--- Recent Operations History (last %d calls) ---\033[0m\n", op_ring_count);
    for (int i = 0; i < op_ring_count; i++) {
        int idx = (op_ring_head + i) % RING_CAP;
        op_log_t *r = &op_ring[idx];
        fprintf(stderr, "  #%-4d %-7s ", r->op_num, r->op_name);
        if (!strcmp(r->op_name, "malloc")) {
            if (r->res_bp)
                fprintf(stderr, "(size=%zu) -> bp=+0x%lx\n", r->size, (long)((char *)r->res_bp - lo));
            else
                fprintf(stderr, "(size=%zu) -> NULL\n", r->size);
        } else if (!strcmp(r->op_name, "free")) {
            if (r->ptr)
                fprintf(stderr, "(ptr=+0x%lx)\n", (long)((char *)r->ptr - lo));
            else
                fprintf(stderr, "(ptr=NULL)\n");
        } else if (!strcmp(r->op_name, "realloc")) {
            long p_off = r->ptr ? (long)((char *)r->ptr - lo) : -1;
            long r_off = r->res_bp ? (long)((char *)r->res_bp - lo) : -1;
            fprintf(stderr, "(ptr=+0x%lx, size=%zu) -> bp=+0x%lx\n", p_off, r->size, r_off);
        }
    }
    fprintf(stderr, "\033[1;33m-------------------------------------------------\033[0m\n");
}

/* live (allocated, not yet freed) blocks as seen through the wrappers */
typedef struct {
    char *bp;
    size_t n;     /* requested payload size */
    int op;       /* op number that produced this block */
} live_t;

static live_t *live = NULL;
static int nlive = 0, cap_live = 0;

static int live_find(void *bp)
{
    for (int i = 0; i < nlive; i++)
        if (live[i].bp == (char *)bp)
            return i;
    return -1;
}

/* index of a live block whose payload [bp, bp+n) overlaps [bp, bp+n) of the argument */
static int live_overlap(void *bp, size_t n)
{
    char *a = (char *)bp;
    for (int i = 0; i < nlive; i++)
        if (a < live[i].bp + live[i].n && live[i].bp < a + n)
            return i;
    return -1;
}

static void live_add(void *bp, size_t n, int op)
{
    if (nlive == cap_live) {
        int ncap = cap_live ? cap_live * 2 : 256;
        live_t *nl = realloc(live, ncap * sizeof *live);
        if (!nl)
            return;               /* tracking is best-effort */
        live = nl;
        cap_live = ncap;
    }
    live[nlive].bp = (char *)bp;
    live[nlive].n = n;
    live[nlive].op = op;
    nlive++;
}

static void live_del(int i)
{
    live[i] = live[nlive - 1];    /* order is not needed except for leak listing */
    nlive--;
}

/*
 * Source location lookup: addr -> "func at file:line" via addr2line on our own executable.
 * Needs the program built with -g (Makefile.viz does). Returns NULL if unknown.
 */
static char exe_path[4096];
static int exe_is_pie = 1;
static void *exe_base = NULL;

static void init_exe_info(void)
{
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);
    Dl_info di;
    FILE *f;
    Elf64_Ehdr eh;

    exe_path[n > 0 ? n : 0] = 0;
    if (dladdr((void *)init_exe_info, &di))
        exe_base = di.dli_fbase;
    if (n > 0 && (f = fopen(exe_path, "rb"))) {
        if (fread(&eh, sizeof eh, 1, f) == 1)
            exe_is_pie = (eh.e_type != ET_EXEC);
        fclose(f);
    }
}

/* is_ret: addr is a return address (look up the call instruction, not the next one) */
static const char *resolve(void *addr, int is_ret)
{
    enum { CACHE = 64 };
    static struct { void *addr; int ret; char txt[300]; int ok; } cache[CACHE];
    static int ncache = 0;
    static char overflow[300];
    Dl_info di;
    char cmd[4400], line[300], *nl, *cwd_pfx;
    char cwd[1024];
    unsigned long off;
    FILE *fp;

    for (int i = 0; i < ncache; i++)
        if (cache[i].addr == addr && cache[i].ret == is_ret)
            return cache[i].ok ? cache[i].txt : NULL;

    if (!addr || !exe_path[0] || !dladdr(addr, &di) || di.dli_fbase != exe_base)
        return NULL;                       /* libc, vdso, ... */
    off = (unsigned long)addr - (is_ret ? 1 : 0);
    if (exe_is_pie)
        off -= (unsigned long)exe_base;
    snprintf(cmd, sizeof cmd, "addr2line -e '%s' -f -C -p 0x%lx 2>/dev/null", exe_path, off);

    line[0] = 0;
    if ((fp = popen(cmd, "r"))) {
        if (!fgets(line, sizeof line, fp))
            line[0] = 0;
        pclose(fp);
    }
    if ((nl = strchr(line, '\n')))
        *nl = 0;

    if ((nl = strstr(line, " (discriminator")))
        *nl = 0;
    int ok = line[0] && !strstr(line, "??");
    if (ok && getcwd(cwd, sizeof cwd) && (cwd_pfx = strstr(line, cwd)) && cwd[1]) {
        size_t l = strlen(cwd);       /* show paths relative to the cwd */
        char *src = cwd_pfx + l + (cwd_pfx[l] == '/');
        memmove(cwd_pfx, src, strlen(src) + 1);
    }
    if (ncache < CACHE) {
        cache[ncache].addr = addr;
        cache[ncache].ret = is_ret;
        snprintf(cache[ncache].txt, sizeof cache[ncache].txt, "%s", line);
        cache[ncache].ok = ok;
        return ok ? cache[ncache++].txt : (ncache++, NULL);
    }
    snprintf(overflow, sizeof overflow, "%s", line);   /* cache full: result valid until next call */
    return ok ? overflow : NULL;
}

/* where the driver called mm_*, and the entry point being exercised (set by the wrappers) */
static void *cur_site = NULL;
static void *cur_entry = NULL;
static const char *ctx_op = "";   /* last wrapped op, kept after the call for error reports */

/*
 * Test case tracking. mdriver opens each .rep trace with fopen right before running it
 * (wrapped below), and calls mm_init from eval_mm_valid / eval_mm_util / eval_mm_speed.
 * Op #N of a run is trace line N+4 (the file has a 4-line header).
 */
#define TRACE_HDR_LINES 4
static char last_opened[512];     /* most recent *.rep passed to fopen */
static char cur_trace[512];       /* trace of the current run */
static char cur_phase[64];        /* mdriver function that started the run */
static int op_in_flight = 0;      /* op number of the wrapped call being executed */
static char failed_traces[16][64];
static int nfailed = 0;

static void mark_failed(const char *path);

static const char *base_name(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static void mark_failed(const char *path)
{
    const char *n = base_name(path);
    for (int i = 0; i < nfailed; i++)
        if (!strcmp(failed_traces[i], n))
            return;
    if (nfailed < 16)
        snprintf(failed_traces[nfailed++], sizeof failed_traces[0], "%.63s", n);
}

static int trace_line_text(int opnum, char *out, size_t cap)
{
    FILE *f;
    int ok = 0;
    if (!cur_trace[0] || opnum < 1 || !(f = __real_fopen(cur_trace, "r")))
        return 0;
    for (int i = 0; i < opnum + TRACE_HDR_LINES; i++)
        if (!fgets(out, cap, f))
            goto done;
    out[strcspn(out, "\r\n")] = 0;
    ok = 1;
done:
    fclose(f);
    return ok;
}

static void print_trace_ctx(const char *color)
{
    char t[128];
    if (!cur_trace[0])
        return;
    fprintf(stderr, "%s    test case: %s (%s)", color, base_name(cur_trace), cur_phase);
    if (trace_line_text(op_in_flight, t, sizeof t))
        fprintf(stderr, ", op #%d = %s line %d: '%s'", op_in_flight, base_name(cur_trace),
                op_in_flight + TRACE_HDR_LINES, t);
    fprintf(stderr, "\033[0m\n");
}

static enum viz_mode get_mode(void)
{
    static int cached_mode = -1;
    if (cached_mode < 0) {
        const char *v = getenv("VIZ");
        if (!v || !strcmp(v, "table"))
            cached_mode = MODE_TABLE;
        else if (!strcmp(v, "bar"))
            cached_mode = MODE_BAR;
        else if (!strcmp(v, "map") || !strcmp(v, "compact"))
            cached_mode = MODE_MAP;
        else if (!strcmp(v, "error") || !strcmp(v, "quiet"))
            cached_mode = MODE_ERROR;
        else if (!strcmp(v, "step"))
            cached_mode = MODE_STEP;
        else
            cached_mode = MODE_TABLE;
    }
    return (enum viz_mode)cached_mode;
}

static void init_filters(void)
{
    static int initialized = 0;
    if (!initialized) {
        const char *s = getenv("VIZ_START");
        const char *e = getenv("VIZ_END");
        const char *o = getenv("VIZ_OP");
        const char *l = getenv("VIZ_LINK");
        const char *st = getenv("VIZ_STOP");
        if (s) filter_start = atoi(s);
        if (e) filter_end = atoi(e);
        if (o) { filter_start = atoi(o); filter_end = atoi(o); }
        if (l) show_link_hints = atoi(l);
        if (st) stop_on_error = atoi(st);
        initialized = 1;
    }
}

static int should_print(int is_error_occurred)
{
    enum viz_mode mode = get_mode();
    init_filters();

    if (mode == MODE_ERROR)
        return is_error_occurred;

    if (filter_start >= 0 && nops < filter_start)
        return 0;
    if (filter_end >= 0 && nops > filter_end)
        return 0;

    return 1;
}

static void err(const char *fmt, ...)
{
    if (err_muted)
        return;
    if (nerr < ERR_CAP) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(errs[nerr], sizeof errs[0], fmt, ap);
        va_end(ap);
    }
    nerr++;
}

static void flush_errs(void)
{
    if (!nerr)
        return;
    int first = (run_err == 0);
    fprintf(stderr, "\033[1;31m");
    for (int i = 0; i < nerr && i < ERR_CAP; i++)
        fprintf(stderr, " !! %s\n", errs[i]);
    if (nerr > ERR_CAP)
        fprintf(stderr, " !! ... and %d more\n", nerr - ERR_CAP);
    fprintf(stderr, "\033[0m");
    {
        const char *site = resolve(cur_site, 1), *impl = resolve(cur_entry, 0);
        fprintf(stderr, "\033[31m    while running op #%d %s(): called at %s\n"
                        "    implemented at %s\033[0m\n",
                op_in_flight, ctx_op, site ? site : "?", impl ? impl : "? (build mm.c with -g)");
        print_trace_ctx("\033[31m");
    }
    total_err += nerr;
    run_err += nerr;
    nerr = 0;

    if (first)                    /* context for the first problem of the run only */
        print_ring_history();
    if (stop_on_error) {
        fprintf(stderr, "\033[1;31mviz: VIZ_STOP set, aborting at first problem (op #%d)\033[0m\n", op_in_flight);
        signal(SIGABRT, SIG_DFL);
        abort();
    }
}

#define OFF(p) ((long)((char *)(p) - lo))

typedef struct {
    size_t alloc_bytes;
    size_t free_bytes;
    size_t max_free_bytes;
    int num_alloc;
    int num_free;
} heap_stats_t;

/* is q the start of a plausible free block? (for explicit free list link validation) */
static int link_ok(void *q, char *lo, char *hi)
{
    char *b = (char *)q;
    if (b < lo + 12 || b > hi || (uintptr_t)b % 8)
        return 0;
    return !ALLOC(b) && SIZE(b) >= 16 && b + SIZE(b) - 4 <= hi + 1;
}

/* format one free-list link: NULL / +0xOFF / BAD(+0xOFF) (in heap, not a free block) / ?ptr (not in heap) */
static void fmt_link(char *out, size_t cap, void *q, char *lo, char *hi)
{
    if (!q)
        snprintf(out, cap, "NULL");
    else if ((char *)q < lo || (char *)q > hi)
        snprintf(out, cap, "?%p", q);
    else if (link_ok(q, lo, hi))
        snprintf(out, cap, "+0x%lx", (long)((char *)q - lo));
    else
        snprintf(out, cap, "BAD(+0x%lx)", (long)((char *)q - lo));
}

/* Walk and check heap, and optionally print visualization */
static void walk(void *chk, int mode, size_t n, int do_print)
{
    enum viz_mode vmode = get_mode();
    int prev_alloc = 1, found = 0;
    char *lo = (char *)mem_heap_lo(), *hi = (char *)mem_heap_hi(), *p = lo + 8;
    heap_stats_t st = {0, 0, 0, 0, 0};

    init_filters();

    if (mem_heapsize() < 16 || HDR(p) != 9) {   /* prologue = size 8, alloc */
        if (do_print)
            fprintf(stderr, " (unrecognized heap layout)\n");
        return;
    }

    if (do_print && vmode == MODE_TABLE)
        fprintf(stderr, " offset    size  state   details\n");

    for (;; p += SIZE(p)) {
        size_t s = SIZE(p);
        unsigned h = HDR(p);
        int is_alloc = ALLOC(p);

        if (s > 0) {
            if (is_alloc) {
                st.alloc_bytes += s;
                st.num_alloc++;
            } else {
                st.free_bytes += s;
                st.num_free++;
                if (s > st.max_free_bytes)
                    st.max_free_bytes = s;
            }
        }

        if (do_print) {
            if (vmode == MODE_BAR) {
                int w = (int)(s / 32);
                w = w < 5 ? 5 : w > 40 ? 40 : w;
                fprintf(stderr, "\033[%dm%*zu\033[0m ", is_alloc ? 41 : 42, w, s);
            } else if (vmode == MODE_MAP) {
                if (p == lo + 8)
                    fprintf(stderr, "\033[44;37m[P:8]\033[0m");
                else if (s == 0) {
                    fprintf(stderr, "\033[44;37m[E:0]\033[0m");
                    long leftover_heap = (hi + 1) - p;
                    if (leftover_heap > 0)
                        fprintf(stderr, "\033[45;37m[HEAP_UNUSED:%ldB]\033[0m", leftover_heap);
                    size_t pagesz = mem_pagesize();
                    size_t hsz = mem_heapsize();
                    size_t page_slack = (pagesz - (hsz % pagesz)) % pagesz;
                    if (page_slack > 0)
                        fprintf(stderr, "\033[90m[PAGE_SLACK:%zuB]\033[0m", page_slack);
                    fprintf(stderr, "\n");
                } else if (is_alloc)
                    fprintf(stderr, "\033[41;37m[%zuA]\033[0m", s);
                else
                    fprintf(stderr, "\033[42;30m[%zuF]\033[0m", s);
            } else if (vmode == MODE_TABLE || vmode == MODE_ERROR || vmode == MODE_STEP) {
                char detail[96] = {0};
                if (p == lo + 8)
                    snprintf(detail, sizeof(detail), "(prologue)");
                else if (s == 0)
                    snprintf(detail, sizeof(detail), "(epilogue)");
                else if (!is_alloc && show_link_hints > 0) {
                    /* Heuristic: check if first words look like heap pointers (explicit free list) */
                    if (s >= 24 && p + 16 <= hi + 1) {
                        char succ_str[32], pred_str[32];
                        fmt_link(succ_str, sizeof succ_str, *(void **)p, lo, hi);
                        fmt_link(pred_str, sizeof pred_str, *(void **)(p + sizeof(void *)), lo, hi);
                        snprintf(detail, sizeof(detail), "next=%s, prev=%s", succ_str, pred_str);
                    }
                }
                fprintf(stderr, " +0x%04lx %6zu  %s  %s\n", OFF(p), s,
                        is_alloc ? "ALLOC" : "FREE ", detail);
                if (s == 0 && (hi + 1) > p) {
                    fprintf(stderr, " +0x%04lx %6ld  UNUSED  \033[1;35m(unmapped tail heap space!)\033[0m\n",
                            OFF(p), (long)((hi + 1) - p));
                }
            }
        }

        if (mode == CHK_ALLOC && p == (char *)chk) {
            found = 1;
            if (!ALLOC(p))
                err("returned block +0x%lx is marked FREE", OFF(p));
            else if (s < n + 4)
                err("block +0x%lx is %zuB, too small for request %zu", OFF(p), s, n);
        }
        if (mode == CHK_FREED && s && (char *)chk >= p && (char *)chk < p + s) {
            found = 1;
            if (ALLOC(p))
                err("freed block +0x%lx is still marked ALLOC", OFF(p));
        }

        if (s == 0) {                           /* epilogue */
            if (!ALLOC(p))
                err("epilogue is not marked allocated");
            if (p != hi + 1)
                err("epilogue at +0x%lx but heap ends at +0x%lx (blocks do not tile heap)",
                    OFF(p), OFF(hi + 1));
            break;
        }
        if (p + s > hi + 1) {                   /* corrupt heap, stop walking */
            err("corrupt block +0x%lx: size %zu runs past heap end", OFF(p), s);
            break;
        }
        if (h & 6)
            err("block +0x%lx: reserved header bits set (0x%x)", OFF(p), h);
        if (p != lo + 8 && s < 16)
            err("block +0x%lx: size %zu < minimum 16", OFF(p), s);
        if (!ALLOC(p) && *(unsigned int *)(p + s - 8) != h)
            err("free block +0x%lx: header 0x%x != footer 0x%x", OFF(p), h,
                *(unsigned int *)(p + s - 8));
        if (!prev_alloc && !ALLOC(p))
            err("free block +0x%lx follows a free block: not coalesced", OFF(p));
        prev_alloc = ALLOC(p) != 0;
    }

    if (do_print && vmode == MODE_BAR)
        fputc('\n', stderr);

    if (mode == CHK_ALLOC && !found)
        err("returned pointer +0x%lx is not the start of any block", OFF(chk));

    if (do_print) {
        /* Print fragmentation and memory stats */
        double ext_frag = (st.free_bytes > 0)
            ? (1.0 - ((double)st.max_free_bytes / (double)st.free_bytes)) * 100.0
            : 0.0;
        size_t hsz = mem_heapsize();
        double util = hsz > 0 ? ((double)st.alloc_bytes / (double)hsz) * 100.0 : 0.0;

        fprintf(stderr, "  [stats] alloc:%zuB (%d blks)  free:%zuB (%d blks, max:%zuB)  ext_frag:%.1f%%  util:%.1f%%\n",
                st.alloc_bytes, st.num_alloc, st.free_bytes, st.num_free,
                st.max_free_bytes, ext_frag, util);
    }
}

/*
 * Called after the real call. The invariant check runs first (errors are collected,
 * not printed), then the dump is printed if mode/filters allow, then errors are
 * flushed under the dump so they are not interleaved with the table.
 */
static void dump(const char *op, size_t n, void *bp, int mode)
{
    nops++;
    ring_push(op, n, cur_ptr, bp);

    /* First pass: check invariants and collect errors */
    walk(bp, bp ? mode : CHK_NONE, n, 0);
    int had_err = (nerr > 0);

    /* Decide whether to print based on mode and filters */
    if (should_print(had_err)) {
        if (bp)
            fprintf(stderr, "[#%-4d %s %zu] bp=+0x%lx  heap=%zuB\n", nops, op, n,
                    (long)((char *)bp - (char *)mem_heap_lo()), mem_heapsize());
        else
            fprintf(stderr, "[#%-4d %s %zu] bp=NULL  heap=%zuB\n", nops, op, n, mem_heapsize());

        err_muted = 1;            /* the same problems were already collected above */
        walk(bp, bp ? mode : CHK_NONE, n, 1);
        err_muted = 0;

        flush_errs();

        if (get_mode() == MODE_STEP && isatty(fileno(stdin))) {
            fprintf(stderr, "\033[1;36m[STEP] Press Enter to continue...\033[0m");
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
        }
    } else {
        flush_errs();
    }
}

/* bad pointer handed to free/realloc, checked before the real call */
static int pre_free(const char *op, void *ptr)
{
    char *lo = (char *)mem_heap_lo(), *hi = (char *)mem_heap_hi();
    int bad;
    if (!ptr)
        return 0;
    if ((char *)ptr < lo + 12 || (char *)ptr > hi)
        err("%s(%p): pointer is outside the heap", op, ptr);
    else if ((uintptr_t)ptr % 8)
        err("%s(+0x%lx): pointer is not 8-aligned", op, OFF(ptr));
    else if (!ALLOC(ptr))
        err("%s(+0x%lx): block is already free (double free?)", op, OFF(ptr));
    else if (live_find(ptr) < 0)
        err("%s(+0x%lx): not the start of a live block (never returned by malloc/realloc?)",
            op, OFF(ptr));
    bad = nerr;
    flush_errs();
    return bad;
}

/* record a block handed out by malloc/realloc, reporting payload overlap with live blocks */
static void track_new(const char *op, void *bp, size_t n)
{
    char *lo = (char *)mem_heap_lo();
    int i = live_overlap(bp, n);
    if (i >= 0)
        err("%s(%zu) = +0x%lx overlaps live block +0x%lx (%zuB, allocated at op #%d)",
            op, n, OFF(bp), OFF(live[i].bp), live[i].n, live[i].op);
    live_add(bp, n, nops + 1);
}

/* print an address as heap offset when inside the heap */
static void where(const char *label, void *a)
{
    char *lo = (char *)mem_heap_lo(), *hi = (char *)mem_heap_hi();
    if (!a)
        fprintf(stderr, "  %s: NULL\n", label);
    else if ((char *)a >= lo && (char *)a <= hi)
        fprintf(stderr, "  %s: %p (heap +0x%lx)\n", label, a, (long)((char *)a - lo));
    else
        fprintf(stderr, "  %s: %p (OUTSIDE heap %p..%p)\n", label, a, (void *)lo, (void *)hi);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    default:      return "signal";
    }
}

/* fprintf is not async-signal-safe; fine for a debug crash report */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    void *bt[32];
    int nbt, shown = 0;
    const char *loc;
    void *pc = NULL;
#if defined(__x86_64__)
    pc = (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (void *)((ucontext_t *)ctx)->uc_mcontext.pc;
#else
    (void)ctx;
#endif

    fprintf(stderr, "\n\033[1;31m*** CRASH: %s ***\033[0m\n", sig_name(sig));
    if (cur_op)
        fprintf(stderr, "  in op #%d: %s(%zu)\n", op_in_flight, cur_op, cur_n);
    else
        fprintf(stderr, "  outside mm_* call (driver code, or after return)\n");

    loc = resolve(pc, 0);
    if (loc)
        fprintf(stderr, "  \033[1;31mfaulted at: %s\033[0m\n", loc);
    else if (pc)
        fprintf(stderr, "  faulted at: %p (not in this program: libc? stack overflow in mm?)\n", pc);
    if (cur_op)
        print_trace_ctx("\033[1;31m");
    else if (cur_trace[0])
        fprintf(stderr, "  test case: %s (%s)\n", base_name(cur_trace), cur_phase);
    where("fault addr", si->si_addr);
    if (cur_op)
        where("arg ptr   ", cur_ptr);

    print_ring_history();

    nbt = backtrace(bt, 32);
    fprintf(stderr, "\n\033[1;33m--- Call stack (innermost first) ---\033[0m\n");
    for (int i = 1; i < nbt; i++) {          /* frame 0 is this handler */
        const char *f = resolve(bt[i], 1);
        if (f)
            fprintf(stderr, "  #%d %s\n", shown++, f);
    }
    if (!shown) {
        fprintf(stderr, "  (no source info; raw frames:)\n");
        fflush(stderr);
        backtrace_symbols_fd(bt, nbt, STDERR_FILENO);
    }
    fprintf(stderr, "\033[1;33m-------------------------------------\033[0m\n");

    if (cur_trace[0] && (run_err || cur_op))
        mark_failed(cur_trace);
    if (nfailed) {
        fprintf(stderr, "\n\033[1;31mviz: test case(s) with heap problems before/at crash:");
        for (int i = 0; i < nfailed; i++)
            fprintf(stderr, " %s", failed_traces[i]);
        fprintf(stderr, "\033[0m\n");
    }

    fprintf(stderr, "\nHeap state at crash:\n");
    err_muted = 0;
    walk(NULL, CHK_NONE, 0, 1);
    flush_errs();
    /* SA_RESETHAND: returning re-faults with the default action (core/exit) */
}

/* Per-run report (leaks + verdict), then reset per-run state */
static int leak_reported = 0;

static void run_report(void)
{
    char *lo = (char *)mem_heap_lo();

    if (nops == 0 && nlive == 0)
        return;

    if (nlive > 0 && !leak_reported) {
        leak_reported = 1;       /* timing re-runs would repeat the same list */
        size_t leak_bytes = 0;
        for (int i = 0; i < nlive; i++)
            leak_bytes += live[i].n;
        fprintf(stderr, "\033[1;33mviz: [Leak Detector] %d unfreed block(s) remaining (%zu payload bytes total)\033[0m\n",
                nlive, leak_bytes);
        for (int i = 0; i < nlive && i < 10; i++)
            fprintf(stderr, "\033[33m  leak: +0x%lx  %zuB  (allocated at op #%d)\033[0m\n",
                    OFF(live[i].bp), live[i].n, live[i].op);
        if (nlive > 10)
            fprintf(stderr, "\033[33m  ... and %d more\033[0m\n", nlive - 10);
    }

    /* mdriver re-runs the trace many times while timing, so only problems print per run */
    if (run_err)
    {
        fprintf(stderr, "\033[1;31mviz: run %d [%s, %s]: %d heap problem(s) over %d calls\033[0m\n",
                total_runs + 1, base_name(cur_trace), cur_phase, run_err, nops);
        mark_failed(cur_trace);
    }
    total_runs++;
    total_calls += nops;

    nops = 0;
    nlive = 0;
    run_err = 0;
    op_ring_count = 0;
    op_ring_head = 0;
}

static void summary(void)
{
    run_report();
    if (total_err)
    {
        fprintf(stderr, "\033[1;31mviz: %d heap problem(s) over %d calls (%d run(s))\033[0m\n",
                total_err, total_calls, total_runs);
        fprintf(stderr, "\033[1;31mviz: failing test case(s):");
        for (int i = 0; i < nfailed; i++)
            fprintf(stderr, " %s", failed_traces[i]);
        fprintf(stderr, "\033[0m\n");
    }
    else
        fprintf(stderr, "\033[1;32mviz: heap OK over %d calls (%d run(s))\033[0m\n",
                total_calls, total_runs);
}

__attribute__((constructor)) static void install(void)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    init_exe_info();
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    atexit(summary);
}

/* each mm_init starts a new run: report the previous one, then reset */
int __wrap_mm_init(void)
{
    const char *f = resolve(__builtin_return_address(0), 1), *at;
    run_report();
    snprintf(cur_trace, sizeof cur_trace, "%s", last_opened);
    snprintf(cur_phase, sizeof cur_phase, "%s", f ? f : "?");
    if ((at = strstr(cur_phase, " at ")))
        cur_phase[at - cur_phase] = 0;
    return __real_mm_init();
}

/* remember which trace file the driver is about to run */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    size_t l = strlen(path);
    if (l > 4 && !strcmp(path + l - 4, ".rep"))
        snprintf(last_opened, sizeof last_opened, "%s", path);
    return __real_fopen(path, mode);
}

void *__wrap_mm_malloc(size_t size)
{
    op_in_flight = nops + 1;
    cur_op = ctx_op = "malloc"; cur_n = size; cur_ptr = NULL;
    cur_site = __builtin_return_address(0); cur_entry = (void *)__real_mm_malloc;
    void *bp = __real_mm_malloc(size);
    cur_op = NULL;
    if (bp)
        track_new("malloc", bp, size);
    else if (size)
        err("malloc(%zu) returned NULL", size);
    dump("malloc", size, bp, CHK_ALLOC);
    return bp;
}

void __wrap_mm_free(void *ptr)
{
    op_in_flight = nops + 1;
    cur_site = __builtin_return_address(0); cur_entry = (void *)__real_mm_free;
    ctx_op = "free";
    int bad = pre_free("free", ptr);
    size_t n = ptr && !bad ? SIZE(ptr) : 0;
    cur_op = ctx_op = "free"; cur_n = n; cur_ptr = ptr;
    cur_site = __builtin_return_address(0); cur_entry = (void *)__real_mm_free;
    __real_mm_free(ptr);
    cur_op = NULL;
    if (ptr && !bad) {
        int i = live_find(ptr);
        if (i >= 0)
            live_del(i);
    }
    dump("free", n, ptr, CHK_FREED);
}

void *__wrap_mm_realloc(void *ptr, size_t size)
{
    cur_site = __builtin_return_address(0); cur_entry = (void *)__real_mm_realloc;
    op_in_flight = nops + 1;
    ctx_op = "realloc";
    int bad = pre_free("realloc", ptr);
    cur_op = "realloc"; cur_n = size; cur_ptr = ptr;
    void *bp = __real_mm_realloc(ptr, size);
    cur_op = NULL;

    int old = (ptr && !bad) ? live_find(ptr) : -1;
    if (size == 0) {                      /* realloc(ptr, 0) == free(ptr) */
        if (old >= 0)
            live_del(old);
    } else if (bp) {
        if (old >= 0)
            live_del(old);                /* old payload may legitimately be reused */
        track_new("realloc", bp, size);
    } else {
        err("realloc(%zu) returned NULL", size);   /* old block stays live */
    }

    dump("realloc", size, bp, size ? CHK_ALLOC : CHK_NONE);
    return bp;
}
