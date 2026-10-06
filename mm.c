/*
 * mm-naive.c - 가장 빠르고 메모리 효율이 가장 낮은 malloc 패키지.
 * 
 * 이 단순한(naive) 접근 방식에서는 brk 포인터를 단순히 증가시켜 블록을 할당함.
 * 블록은 순수 페이로드(payload)만으로 구성되며 헤더나 푸터가 없음.
 * 블록은 결코 병합되거나 재사용되지 않음. Realloc은 mm_malloc과
 * mm_free를 직접 사용하여 구현됨.
 */
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

/* 기본 상수 및 매크로 */
#define WSIZE       4       /* 워드 및 헤더/푸터 크기 (바이트) */
#define DSIZE       8       /* 더블 워드 크기 (바이트) */
/* 
2진수: 1 → 1 0000 0000 0000 (2진수)
10진수: 1 × 2¹² = 4096
이 크기만큼 힙을 확장함 (바이트, 4KB) 
*/
#define CHUNKSIZE  (1<<12)  


#define MAX(x, y) ((x) > (y)? (x) : (y)) // max함수

/* 크기와 할당 비트를 하나의 워드로 패킹 
size  (24) : 0001 1000
alloc ( 1) : 0000 0001
-----------------------
BITWISE OR : 0001 1001 (십진수 25)
0001 또는 0000을 넣어서 alloc 할당 여부를 판별 
*/

/*
크기와 할당 비트를 하나의 워드로 패킹 
1 또는 0을 넣어서 alloc 할당 여부를 판별 
*/
#define PACK(size, alloc)  ((size) | (alloc)) 


/* 주소 p에 있는 워드를 읽기 */
#define GET(p)       (*(unsigned int *)(p))
/* 주소 p에 있는 워드에 쓰기 */
#define PUT(p, val)  (*(unsigned int *)(p) = (val))

/* 주소 p에서 크기 및 할당 비트 읽기 
  GET(p)  (25) : 0001 1001
& ~0x7         : 1111 1000
---------------------------
  결과    (24) : 0001 1000  -> 하위 3비트(할당 플래그)를 마스킹(0으로 클리어)하여 순수한 블록 크기만 추출
  
======================================================
  GET(p)  (25) : 0001 1001
& 0x1          : 0000 0001
---------------------------
  결과    ( 1) : 0000 0001  -> 첫 비트만 AND 연산해서 할당 여부 판별
*/

/*
주소 p에서 크기 반환
하위 3비트(할당 플래그)를 마스킹(0으로 클리어)하여 순수한 블록 크기만 추출
*/
#define GET_SIZE(p)  (GET(p) & ~0x7)
/*
주소 p에서 할당 비트 읽기 
첫 비트만 AND 연산해서 할당 여부 판별
*/
#define GET_ALLOC(p) (GET(p) & 0x1)

/* 블록 포인터 bp가 주어지면, 헤더와 푸터의 주소를 계산 
WSIZE (Word Size): 1워드 크기 (보통 4바이트)
DSIZE (Double Word Size): 2워드 크기 (2 * WSIZE, 보통 8바이트)
전체 블록 크기(size) = 헤더(1워드) + 페이로드 + 푸터(1워드)

HDRP : 페이로드 시작 주소(bp)에서 1워드(WSIZE)만큼 앞으로 되돌아가 헤더의 시작 주소를 반환
FTRP : 페이로드 시작 주소(bp)에서 블록 크기(size)를 더한 뒤, 2워드(DSIZE)만큼 빼서 푸터의 시작 주소를 반환

헤더 주소: bp - WSIZE
블록 전체가 끝나는 지점의 주소: 헤더 주소 + size = (bp - WSIZE) + size
푸터의 시작 주소: 블록 끝 주소에서 푸터 크기(1워드)만큼 앞으로 이동
푸터의 시작 주소 = ((bp - WSIZE) + size) - WSIZE = bp + size - (2 * WSIZE) = (bp + size - DSIZE)
*/

/*
헤더 주소 반환
페이로드 시작 주소(bp)에서 1워드(WSIZE)만큼 앞으로 되돌아가 헤더의 시작 주소를 반환
bp - WSIZE
*/
#define HDRP(bp) ((char *)(bp) - WSIZE)
/*
푸터의 시작 주소 반환
페이로드 시작 주소(bp)에서 블록 크기(size)를 더한 뒤, 2워드(DSIZE)만큼 빼서 푸터의 시작 주소를 반환
((bp - WSIZE) + size) - WSIZE  = (bp + size - DSIZE)
*/
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* 블록 포인터 bp가 주어지면, 다음 블록과 이전 블록의 주소를 계산 */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))/* 블록 포인터 bp를 기준으로 다음 블록 포인터 bp를 반환 */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) /* 블록 포인터 bp를 기준으로 이전 블록 포인터 bp를 반환 */

/* 싱글 워드(4) 또는 더블 워드(8) 정렬 기준 */
#define ALIGNMENT 8 

/* ALIGNMENT의 가장 가까운 배수로 올림 */
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7)

/* size_t의 크기를 8의 배수로 올림(정렬)한 크기 (8바이트) */
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

/* 명시적 가용 리스트: 가용 블록 payload 앞 두 워드(8B 포인터)에 PRED/SUCC 저장 */
#define MINBLK  24  /* 헤더4 + PRED8 + SUCC8 + 푸터4 */
/* PRED: 리스트의 이전 가용 블록 포인터. payload 시작(bp+0)에 저장 */
#define PRED(bp) (*(void *)(bp))
/* SUCC: 리스트의 다음 가용 블록 포인터. bp+8에 저장 (char*로 바이트 단위 계산) */
#define SUCC(bp) (*(void *)((char *)(bp) + DSIZE))

static char *heap_listp;  /* 프롤로그 블록을 가리키는 포인터 */
static void *free_listp;  /* 가용 리스트 head (LIFO) */

static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);
static void add_free(void *bp);
static void remove_free(void *bp);

/*
세팅
void mem_init(void);               
void mem_deinit(void);
void *mem_sbrk(int incr);
void mem_reset_brk(void); 
void *mem_heap_lo(void);
void *mem_heap_hi(void);
size_t mem_heapsize(void);
size_t mem_pagesize(void);

*/

/* 
 mm_init - malloc 패키지 초기화
 순서: 초기힙 생성 - 4b 패딩 후, 프롤로그, 에필로그 8b 및 시작점 설정 - 빈 힙 확장
 */
int mm_init(void)
{
    /* 초기 빈 힙 생성 */
    if ((heap_listp = mem_sbrk(4*WSIZE)) == (void *)-1)
        return -1;
    PUT(heap_listp, 0);                          /* 정렬 패딩 */
    PUT(heap_listp + (1*WSIZE), PACK(DSIZE, 1)); /* 프롤로그 헤더 / 4바이트 패딩 두고 프롤로그 헤더 생성 */
    PUT(heap_listp + (2*WSIZE), PACK(DSIZE, 1)); /* 프롤로그 푸터 / 프롤로그 헤더 바로 뒤에 푸터 생성 */
    PUT(heap_listp + (3*WSIZE), PACK(0, 1));     /* 에필로그 헤더 / 프롤로그 푸터 바로 뒤에 에필로그 헤더 생성 */
    free_listp = NULL;
    heap_listp += (2*WSIZE); /* 시작점 블록을 넣을 시작점 / 빈공간도 찾고 넣을곳도 정하는 포인터 */

    /* CHUNKSIZE 바이트 크기의 가용 블록으로 빈 힙 확장 */
    if (extend_heap(CHUNKSIZE/WSIZE) == NULL) // 4kb 확보
        return -1; // 실패시
    return 0;
}

/*
extend_heap - 새로운 가용 블록으로 힙 확장
정렬 유지를 위해서 무조건 짝수개의 워드 할당
할당한 가용 블록에 헤더, 푸터 생성한 후 늘어난 brk에 새로운 에필로그 헤더 만듬
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* 정렬 유지를 위해 짝수 개의 워드 할당 */
    size = (words % 2) ? (words+1) * WSIZE : words * WSIZE; // 짝수일때는 바로, 홀수 일때는 + 1
    if ((long)(bp = mem_sbrk(size)) == -1) // size만큼 brk 늘리기 / sbrk는 늘리기전 brk의 위치를 보내줌
        return NULL;

    /* 가용 블록의 헤더/푸터 및 에필로그 헤더 초기화 */
    PUT(HDRP(bp), PACK(size, 0));         /* 가용 블록 헤더 */
    PUT(FTRP(bp), PACK(size, 0));         /* 가용 블록 푸터 */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* 새 에필로그 헤더 */

    /* 이전 블록이 가용 상태였으면 병합 수행 */
    return coalesce(bp); // 합처지면서 시작 주소가 바뀌는 경우도 있으니 coalesce 반환값을 전달
}

/*
mm_free - 블록을 반환(해제)하고 인접 가용 블록들과 병합
인자로 들어온 bp는 프리할 블록으로 간주
size에 0부호 계산해서 그냥 할당 비트 제거
헤더 업데이트는 coalesce 함수로 위임
 */
void mm_free(void *bp)
{
    // 전체 크기 계산
    size_t size = GET_SIZE(HDRP(bp));

    // 헤더, 푸터 할당 0 갱신
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));

    coalesce(bp);
}

/* 가용 리스트 맨 앞에 삽입 (LIFO) */
static void add_free(void *bp)
{
    PRED(bp) = NULL;               /* 새 head라서 앞 블록 없음 */
    SUCC(bp) = free_listp;         /* 다음 블록 = 기존 head */
    if (free_listp)                /* 리스트가 비었으면 건너뜀 (NULL 역참조 방지) */
        PRED(free_listp) = bp;     /* 기존 head의 앞 블록 = bp */
    free_listp = bp;               /* head 갱신 */
}

/* 가용 리스트에서 제거 */
static void remove_free(void *bp)
{
    if (PRED(bp))
        SUCC(PRED(bp)) = SUCC(bp); /* 앞 블록의 next = 내 next */
    else
        free_listp = SUCC(bp);     /* 내가 head면 head = 내 next */
    if (SUCC(bp))                  /* 내가 tail이면 건너뜀 */
        PRED(SUCC(bp)) = PRED(bp); /* 뒤 블록의 prev = 내 prev */
}

/*
 * coalesce - 경계 태그(boundary-tag)를 사용한 가용 블록 병합
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp))); // 이전 블록 할당 여부
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp))); // 다음 블록 할당 여부
    size_t size = GET_SIZE(HDRP(bp)); // 현 블록 사이즈

    if (prev_alloc && next_alloc) {            /* 경우 1: 앞뒤 블록 모두 할당됨 */
        add_free(bp);
        return bp;
    }

    else if (prev_alloc && !next_alloc) {      /* 경우 2: 이전 블록 할당됨, 다음 블록 가용 상태 */
        remove_free(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))); // 현 블록에 다음 블록 사이즈 더하기
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }

    else if (!prev_alloc && next_alloc) {      /* 경우 3: 이전 블록 가용 상태, 다음 블록 할당됨 */
        remove_free(PREV_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp); // 이전 블록 bp 반환 
    }

    else {                                     /* 경우 4: 앞뒤 블록 모두 가용 상태 */
        remove_free(PREV_BLKP(bp));
        remove_free(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
            GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp); // 가장 앞에 있는 이전 블록 bp 반환
    }

    add_free(bp);
    return bp;
}

/* 
 * mm_malloc - 가용 리스트에서 블록 할당
 */
void *mm_malloc(size_t size)
{
    size_t asize;      /* 조정된 블록 크기 (헤더/푸터 및 정렬 고려) */
    size_t extendsize; /* 적합한 가용 블록이 없을 때 힙을 확장할 크기 */
    char *bp;

    /* 잘못된 요청 무시 */
    if (size == 0)
        return NULL;

    asize = MAX(MINBLK, ALIGN(size + DSIZE));

    /* 가용 리스트에서 적합한 블록 검색 */
    if ((bp = find_fit(asize)) != NULL) {
        place(bp, asize);
        return bp;
    }

    /* 적합한 블록을 찾지 못한 경우: 힙을 확장한 후 블록 배치 */
    extendsize = MAX(asize,CHUNKSIZE);
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL)
        return NULL;
    place(bp, asize);
    return bp;
}

static void *find_fit(size_t asize)
{
    void *bp;
    void *best_bp = NULL;

    for (bp = free_listp; bp != NULL; bp = SUCC(bp)) {
        if (asize <= GET_SIZE(HDRP(bp))) {
            // 처음 찾았거나(best_bp == NULL), 기존 후보보다 크기가 더 작은 블록을 발견한 경우
            if (best_bp == NULL || GET_SIZE(HDRP(bp)) < GET_SIZE(HDRP(best_bp))) {
                best_bp = bp;

                // 크기가 완벽히 일치하면 더 탐색하지 않고 즉시 반환
                if (GET_SIZE(HDRP(bp)) == asize)
                    return best_bp;
            }
        }
    }
    return best_bp; /* 못 찾았으면 NULL, 찾았으면 가장 알맞은 블록 반환 */
}

/*
 * place - 가용 블록에 요청 블록을 배치하고, 남은 크기가 최소 블록 크기 이상이면 분할
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));

    remove_free(bp);
    if ((csize - asize) >= MINBLK) { // 남는 공간이 최소 블록 크기(24B) 이상이면 분할
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp); // 분할된 가용 블록으로 이동
        PUT(HDRP(bp), PACK(csize-asize, 0));
        PUT(FTRP(bp), PACK(csize-asize, 0));
        add_free(bp); // 뒤 블록은 할당 블록이라 병합 불필요
    }
    else { // 분할하지 않고 블록 전체 할당
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}


/*
 * mm_realloc - mm_malloc과 mm_free를 사용하여 단순하게 구현
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *oldptr = ptr;
    void *newptr;

    if (size == 0) { mm_free(ptr); return NULL; }
    if (!ptr) return mm_malloc(size);

    size_t csize = GET_SIZE(HDRP(oldptr));
    size_t asize = MAX(MINBLK, ALIGN(size + DSIZE));

    /* 케이스 1: 현재 블록 크기 >= 요청 크기 (축소 또는 유지) */
    if (csize >= asize) {
        if ((csize - asize) >= MINBLK) {
            PUT(HDRP(oldptr), PACK(asize, 1));
            PUT(FTRP(oldptr), PACK(asize, 1));

            void *next_bp = NEXT_BLKP(oldptr);
            PUT(HDRP(next_bp), PACK(csize - asize, 0));
            PUT(FTRP(next_bp), PACK(csize - asize, 0));
            coalesce(next_bp); // 뒤쪽 블록 가용 병합
        }
        return oldptr;
    }
    /* 케이스 2: 다음 블록이 FREE이고 합쳐서 충분할 때 병합 */
    else if (!GET_ALLOC(HDRP(NEXT_BLKP(oldptr))) && (csize + GET_SIZE(HDRP(NEXT_BLKP(oldptr)))) >= asize) {
        size_t total_size = csize + GET_SIZE(HDRP(NEXT_BLKP(oldptr)));
        remove_free(NEXT_BLKP(oldptr)); /* 흡수될 다음 블록 */
        if ((total_size - asize) >= MINBLK) {
            PUT(HDRP(oldptr), PACK(asize, 1));
            PUT(FTRP(oldptr), PACK(asize, 1));
            
            void *next_bp = NEXT_BLKP(oldptr);
            PUT(HDRP(next_bp), PACK(total_size - asize, 0));
            PUT(FTRP(next_bp), PACK(total_size - asize, 0));
            coalesce(next_bp);
        } else {
            PUT(HDRP(oldptr), PACK(total_size, 1));
            PUT(FTRP(oldptr), PACK(total_size, 1));
        }
        return oldptr;
    }
    /* 케이스 3: 다음 블록이 힙 끝(에필로그 헤더)인 경우 제자리 확장 */
    else if (GET_SIZE(HDRP(NEXT_BLKP(oldptr))) == 0) {
        size_t extend_size = asize - csize;
        if ((long)(mem_sbrk(extend_size)) == -1)
            return NULL;

        PUT(HDRP(oldptr), PACK(asize, 1));
        PUT(FTRP(oldptr), PACK(asize, 1));
        PUT(HDRP(NEXT_BLKP(oldptr)), PACK(0, 1)); // 새로운 에필로그 헤더 설정
        return oldptr;
    }
    /* 케이스 4: 위 조건 모두 불가능할 때 새로 할당 후 복사 */
    else {
        newptr = mm_malloc(size);
        if (newptr == NULL)
            return NULL;
        size_t copySize;
        copySize = csize - DSIZE;
        if (size < copySize)
            copySize = size;
        memcpy(newptr, oldptr, copySize);
        mm_free(oldptr);
        return newptr;
    }
}
