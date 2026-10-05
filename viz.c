/*
 * viz.c - Advanced heap visualizer + checker for Malloc Lab. Does NOT touch mm.c.
 *
 * Linker --wrap intercepts the driver's mm_malloc/mm_free/mm_realloc calls,
 * runs the real ones, dumps the heap to stderr, tracks stats and checks invariants.
 * Problems print in bold red. On SIGSEGV/SIGBUS it prints the faulting call,
 * fault address, recent operations history, and the heap state at crash.
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
 * Advanced features:
 *   - Recent operation history ring buffer (printed on crash)
 *   - External fragmentation & memory utilization profiling
 *   - Unfreed block / memory leak detector on exit
 *   - Free block explicit list (next/prev pointer) heuristic inspection
 *   - Double-free, invalid pointer, alignment, heap-boundary checks
 *   - Header/footer consistency & coalescing verification
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#include "mm.h"
#include "memlib.h"

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
static char errs[32][128];
static int nerr = 0, total_err = 0, nops = 0;

/* filter range */
static int filter_start = -1;
static int filter_end = -1;
static int show_link_hints = -1;

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
        if (s) filter_start = atoi(s);
        if (e) filter_end = atoi(e);
        if (o) { filter_start = atoi(o); filter_end = atoi(o); }
        if (l) show_link_hints = atoi(l);
        initialized = 1;
    }
}

static int should_print(int is_error_occurred)
{
    enum viz_mode mode = get_mode();
    init_filters();

    if (filter_start >= 0 && nops < filter_start)
        return 0;
    if (filter_end >= 0 && nops > filter_end)
        return 0;

    if (mode == MODE_ERROR)
        return is_error_occurred;

    return 1;
}

static void err(const char *fmt, ...)
{
    if (nerr < 32) {
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
    fprintf(stderr, "\033[1;31m");
    for (int i = 0; i < nerr && i < 32; i++)
        fprintf(stderr, " !! %s\n", errs[i]);
    if (nerr > 32)
        fprintf(stderr, " !! ... and %d more\n", nerr - 32);
    fprintf(stderr, "\033[0m");
    total_err += nerr;
    nerr = 0;
}

#define OFF(p) ((long)((char *)(p) - lo))

typedef struct {
    size_t alloc_bytes;
    size_t free_bytes;
    size_t max_free_bytes;
    int num_alloc;
    int num_free;
} heap_stats_t;

/* Walk and check heap, and optionally print visualization */
static void walk(void *chk, int mode, size_t n, int do_print)
{
    enum viz_mode vmode = get_mode();
    int prev_alloc = 1, found = 0;
    char *lo = (char *)mem_heap_lo(), *hi = (char *)mem_heap_hi(), *p = lo + 8;
    heap_stats_t st = {0, 0, 0, 0, 0};

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
                char detail[64] = {0};
                if (p == lo + 8)
                    snprintf(detail, sizeof(detail), "(prologue)");
                else if (s == 0)
                    snprintf(detail, sizeof(detail), "(epilogue)");
                else if (!is_alloc && (show_link_hints > 0 || getenv("VIZ_LINK"))) {
                    /* Heuristic: check if first words look like heap pointers (explicit free list) */
                    if (s >= 16) {
                        void *succ = *(void **)p;
                        void *pred = *(void **)(p + sizeof(void *));
                        char succ_str[24] = "NULL", pred_str[24] = "NULL";
                        if (succ && (char *)succ >= lo && (char *)succ <= hi)
                            snprintf(succ_str, sizeof(succ_str), "+0x%lx", (long)((char *)succ - lo));
                        if (pred && (char *)pred >= lo && (char *)pred <= hi)
                            snprintf(pred_str, sizeof(pred_str), "+0x%lx", (long)((char *)pred - lo));
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

    flush_errs();
}

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

        walk(bp, bp ? mode : CHK_NONE, n, 1);

        if (get_mode() == MODE_STEP && isatty(fileno(stdin))) {
            fprintf(stderr, "\033[1;36m[STEP] Press Enter to continue...\033[0m");
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
        }
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
    bad = nerr;
    flush_errs();
    return bad;
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

/* fprintf is not async-signal-safe; fine for a debug crash report */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "\n\033[1;31m*** CRASH: %s ***\033[0m\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS");
    if (cur_op)
        fprintf(stderr, "  in %s(%zu)\n", cur_op, cur_n);
    else
        fprintf(stderr, "  outside mm_* call (driver code, or after return)\n");

    where("fault addr", si->si_addr);
    if (cur_op)
        where("arg ptr   ", cur_ptr);

    print_ring_history();

    fprintf(stderr, "\nHeap state at crash:\n");
    walk(NULL, CHK_NONE, 0, 1);
    /* SA_RESETHAND: returning re-faults with the default action (core/exit) */
}

/* Report unfreed blocks / memory leaks at program exit */
static void check_leaks(void)
{
    char *lo = (char *)mem_heap_lo(), *hi = (char *)mem_heap_hi(), *p = lo + 8;
    if (mem_heapsize() < 16 || HDR(p) != 9)
        return;

    int leak_count = 0;
    size_t leak_bytes = 0;

    for (;; p += SIZE(p)) {
        size_t s = SIZE(p);
        if (s == 0 || p + s > hi + 1)
            break;
        if (p != lo + 8 && ALLOC(p)) {   /* Exclude prologue */
            leak_count++;
            leak_bytes += s;
        }
    }

    if (leak_count > 0) {
        fprintf(stderr, "\033[1;33mviz: [Leak Detector] %d unfreed block(s) remaining (%zu bytes total)\033[0m\n",
                leak_count, leak_bytes);
    }
}

static void summary(void)
{
    check_leaks();
    if (total_err)
        fprintf(stderr, "\033[1;31mviz: %d heap problem(s) over %d calls\033[0m\n", total_err, nops);
    else
        fprintf(stderr, "\033[1;32mviz: heap OK over %d calls\033[0m\n", nops);
}

__attribute__((constructor)) static void install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    atexit(summary);
}

void *__wrap_mm_malloc(size_t size)
{
    cur_op = "malloc"; cur_n = size; cur_ptr = NULL;
    void *bp = __real_mm_malloc(size);
    cur_op = NULL;
    dump("malloc", size, bp, CHK_ALLOC);
    return bp;
}

void __wrap_mm_free(void *ptr)
{
    int bad = pre_free("free", ptr);
    size_t n = ptr && !bad ? SIZE(ptr) : 0;
    cur_op = "free"; cur_n = n; cur_ptr = ptr;
    __real_mm_free(ptr);
    cur_op = NULL;
    dump("free", n, ptr, CHK_FREED);
}

void *__wrap_mm_realloc(void *ptr, size_t size)
{
    pre_free("realloc", ptr);
    cur_op = "realloc"; cur_n = size; cur_ptr = ptr;
    void *bp = __real_mm_realloc(ptr, size);
    cur_op = NULL;
    dump("realloc", size, bp, size ? CHK_ALLOC : CHK_NONE);
    return bp;
}
