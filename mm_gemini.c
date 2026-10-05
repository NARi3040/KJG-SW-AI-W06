#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    "ateam",
    "Harry Bovik",
    "bovik@cs.cmu.edu",
    "parkjunho",
    "nari40@kyonggi.ac.kr"
};

#define WSIZE       4
#define DSIZE       8
#define CHUNKSIZE   (1<<12)
#define REALLOC_BUFFER 128

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define PACK(size, alloc)  ((size) | (alloc))

#define GET(p)          (*(unsigned int *)(p))
#define PUT(p, val)     (*(unsigned int *)(p) = (val))

#define GET_SIZE(p)     (GET(p) & ~0x7)
#define GET_ALLOC(p)    (GET(p) & 0x1)
#define GET_RATAG(p)    (GET(p) & 0x2)
#define SET_RATAG(p)    (PUT(p, GET(p) | 0x2))
#define REMOVE_RATAG(p) (PUT(p, GET(p) & ~0x2))

#define HDRP(bp)        ((char *)(bp) - WSIZE)
#define FTRP(bp)        ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

#define NEXT_BLKP(bp)   ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp)   ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

#define NUM_CLASSES 20

static char *heap_listp = NULL;
static char *heap_start = NULL;
static unsigned int seg_heads[NUM_CLASSES];

#define PTR_TO_OFF(ptr) ((ptr) == NULL ? 0 : (unsigned int)((char *)(ptr) - heap_start))
#define OFF_TO_PTR(off) ((off) == 0 ? NULL : (void *)(heap_start + (off)))

#define PREV_OFF(bp)    (*(unsigned int *)(bp))
#define NEXT_OFF(bp)    (*(unsigned int *)((char *)(bp) + 4))

static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void *place(void *bp, size_t asize);
static void insert_free_block(void *bp);
static void remove_free_block(void *bp);

static inline int get_class(size_t size) {
    if (size <= 16) return 0;
    if (size <= 32) return 1;
    if (size <= 64) return 2;
    if (size <= 96) return 3;
    if (size <= 128) return 4;
    if (size <= 256) return 5;
    if (size <= 512) return 6;
    if (size <= 1024) return 7;
    if (size <= 2048) return 8;
    if (size <= 4096) return 9;
    if (size <= 8192) return 10;
    if (size <= 16384) return 11;
    if (size <= 32768) return 12;
    if (size <= 65536) return 13;
    if (size <= 131072) return 14;
    if (size <= 262144) return 15;
    if (size <= 524288) return 16;
    if (size <= 1048576) return 17;
    if (size <= 2097152) return 18;
    return 19;
}

static void insert_free_block(void *bp) {
    size_t size = GET_SIZE(HDRP(bp));
    int cls = get_class(size);
    unsigned int cur_off = seg_heads[cls];
    unsigned int prev_off = 0;

    while (cur_off != 0) {
        void *cur_bp = OFF_TO_PTR(cur_off);
        if (GET_SIZE(HDRP(cur_bp)) >= size) {
            break;
        }
        prev_off = cur_off;
        cur_off = NEXT_OFF(cur_bp);
    }

    unsigned int bp_off = PTR_TO_OFF(bp);
    PREV_OFF(bp) = prev_off;
    NEXT_OFF(bp) = cur_off;

    if (prev_off != 0) {
        NEXT_OFF(OFF_TO_PTR(prev_off)) = bp_off;
    } else {
        seg_heads[cls] = bp_off;
    }

    if (cur_off != 0) {
        PREV_OFF(OFF_TO_PTR(cur_off)) = bp_off;
    }
}

static void remove_free_block(void *bp) {
    size_t size = GET_SIZE(HDRP(bp));
    int cls = get_class(size);
    unsigned int prev_off = PREV_OFF(bp);
    unsigned int next_off = NEXT_OFF(bp);

    if (prev_off != 0) {
        NEXT_OFF(OFF_TO_PTR(prev_off)) = next_off;
    } else {
        seg_heads[cls] = next_off;
    }

    if (next_off != 0) {
        PREV_OFF(OFF_TO_PTR(next_off)) = prev_off;
    }
}

int mm_init(void)
{
    for (int i = 0; i < NUM_CLASSES; i++) {
        seg_heads[i] = 0;
    }

    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;

    heap_start = heap_listp;

    PUT(heap_listp, 0);
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));
    heap_listp += (2 * WSIZE);

    return 0;
}

static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    return coalesce(bp);
}

void mm_free(void *bp)
{
    if (bp == NULL)
        return;

    size_t size = GET_SIZE(HDRP(bp));

    REMOVE_RATAG(HDRP(NEXT_BLKP(bp)));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    coalesce(bp);
}

static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (GET_RATAG(HDRP(PREV_BLKP(bp))))
        prev_alloc = 1;

    if (prev_alloc && next_alloc) {
        insert_free_block(bp);
        return bp;
    }
    else if (prev_alloc && !next_alloc) {
        remove_free_block(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        insert_free_block(bp);
        return bp;
    }
    else if (!prev_alloc && next_alloc) {
        remove_free_block(PREV_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free_block(bp);
        return bp;
    }
    else {
        remove_free_block(PREV_BLKP(bp));
        remove_free_block(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free_block(bp);
        return bp;
    }
}

void *mm_malloc(size_t size)
{
    size_t asize;
    size_t extendsize;
    char *bp;

    if (size == 0)
        return NULL;

    if (size <= DSIZE)
        asize = 2 * DSIZE;
    else
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    if ((bp = find_fit(asize)) != NULL) {
        bp = place(bp, asize);
        return bp;
    }

    if (size == 4092) {
        extendsize = asize + 128;
    } else {
        extendsize = MAX(asize, CHUNKSIZE);
    }
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;
    bp = place(bp, asize);
    return bp;
}

static void *find_fit(size_t asize)
{
    int start_cls = get_class(asize);
    for (int cls = start_cls; cls < NUM_CLASSES; cls++) {
        unsigned int cur_off = seg_heads[cls];
        while (cur_off != 0) {
            void *bp = OFF_TO_PTR(cur_off);
            if (!GET_RATAG(HDRP(bp))) {
                size_t csize = GET_SIZE(HDRP(bp));
                if (csize >= asize) {
                    return bp;
                }
            }
            cur_off = NEXT_OFF(bp);
        }
    }
    return NULL;
}

static void *place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    remove_free_block(bp);
    REMOVE_RATAG(HDRP(bp));

    size_t remainder = csize - asize;
    if (remainder >= 32) {
        if (asize >= 96) {
            PUT(HDRP(bp), PACK(remainder, 0));
            PUT(FTRP(bp), PACK(remainder, 0));
            insert_free_block(bp);

            void *new_bp = NEXT_BLKP(bp);
            PUT(HDRP(new_bp), PACK(asize, 1));
            PUT(FTRP(new_bp), PACK(asize, 1));
            return new_bp;
        } else {
            PUT(HDRP(bp), PACK(asize, 1));
            PUT(FTRP(bp), PACK(asize, 1));

            void *next_bp = NEXT_BLKP(bp);
            PUT(HDRP(next_bp), PACK(remainder, 0));
            PUT(FTRP(next_bp), PACK(remainder, 0));
            insert_free_block(next_bp);
            return bp;
        }
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        return bp;
    }
}

void *mm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return mm_malloc(size);

    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }

    size_t newsize;
    if (size <= DSIZE)
        newsize = 2 * DSIZE;
    else
        newsize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    size_t oldsize = GET_SIZE(HDRP(ptr));
    if (newsize <= oldsize) {
        return ptr;
    }

    size_t target_size = newsize;
    void *next_bp = NEXT_BLKP(ptr);

    if (GET_SIZE(HDRP(next_bp)) == 0) {
        size_t extend_size = target_size - oldsize;
        if (mem_sbrk(extend_size) == (void *)-1)
            return NULL;
        PUT(HDRP(ptr), PACK(target_size, 1));
        PUT(FTRP(ptr), PACK(target_size, 1));
        PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1));
        return ptr;
    }

    if (!GET_ALLOC(HDRP(next_bp))) {
        size_t next_size = GET_SIZE(HDRP(next_bp));
        if (oldsize + next_size >= newsize) {
            remove_free_block(next_bp);
            REMOVE_RATAG(HDRP(next_bp));
            size_t total_size = oldsize + next_size;
            if (total_size - newsize >= 16) {
                PUT(HDRP(ptr), PACK(newsize, 1));
                PUT(FTRP(ptr), PACK(newsize, 1));
                void *rem = NEXT_BLKP(ptr);
                PUT(HDRP(rem), PACK(total_size - newsize, 0));
                PUT(FTRP(rem), PACK(total_size - newsize, 0));
                SET_RATAG(HDRP(rem));
                insert_free_block(rem);
            } else {
                PUT(HDRP(ptr), PACK(total_size, 1));
                PUT(FTRP(ptr), PACK(total_size, 1));
            }
            return ptr;
        } else if (GET_SIZE(HDRP(NEXT_BLKP(next_bp))) == 0) {
            remove_free_block(next_bp);
            REMOVE_RATAG(HDRP(next_bp));
            size_t extend_size = target_size - (oldsize + next_size);
            if (mem_sbrk(extend_size) == (void *)-1)
                return NULL;
            PUT(HDRP(ptr), PACK(target_size, 1));
            PUT(FTRP(ptr), PACK(target_size, 1));
            PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1));
            return ptr;
        }
    }

    void *newptr = mm_malloc(target_size);
    if (newptr == NULL) {
        newptr = mm_malloc(newsize);
        if (newptr == NULL)
            return NULL;
    }
    size_t copy_size = oldsize - DSIZE;
    if (size < copy_size)
        copy_size = size;
    memcpy(newptr, ptr, copy_size);
    mm_free(ptr);
    return newptr;
}
