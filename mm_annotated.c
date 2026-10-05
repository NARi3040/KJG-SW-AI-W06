/*
 * mm.c - 묵시적 가용 리스트(Implicit Free List) 기반 동적 메모리 할당기
 *
 * 구조 및 정책 요약:
 *  - 블록 구조: 헤더(4B) + [페이로드 + 패딩] + 푸터(4B) (경계 태그 방식)
 *  - 정렬 기준: 8바이트 (더블 워드) 정렬
 *  - 가용 리스트 관리: 묵시적 가용 리스트 (전체 힙 블록을 순차 순회)
 *  - 탐색 정책: 최초 적합 (First-fit)
 *  - 가용 블록 병합: 상수 시간 경계 태그 병합 (Immediate Boundary-tag Coalescing)
 *  - 힙 최소 구성: 미사용 패딩(4B) + 프롤로그 블록(8B) + ... + 에필로그 헤더(4B)
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

/* ========================================================================= */
/* 기본 상수 및 매크로 정의                                                 */
/* ========================================================================= */

#define WSIZE       4       /* 워드 및 헤더/푸터 크기 (바이트) */
#define DSIZE       8       /* 더블 워드 크기 (바이트) */
#define CHUNKSIZE  (1<<12)  /* 힙 확장 기본 크기 (4096 바이트, 4KB) */

#define MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * PACK(size, alloc):
 *  - 블록 크기와 할당 여부 비트를 하나의 워드로 결합 (BITWISE OR).
 *  - 8바이트 정렬로 인해 블록 크기의 하위 3비트는 항상 0이므로,
 *    최하위 비트에 할당 여부(0: 가용, 1: 할당)를 플래그로 저장함.
 *  예: size = 24(0x18), alloc = 1 -> 0x19
 */
#define PACK(size, alloc)  ((size) | (alloc))

/* 주소 p가 가리키는 워드 읽기 및 쓰기 */
#define GET(p)          (*(unsigned int *)(p))
#define PUT(p, val)     (*(unsigned int *)(p) = (val))

/*
 * GET_SIZE(p):
 *  - 하위 3비트(~0x7 마스킹)를 제거하여 순수 블록 크기 추출
 * GET_ALLOC(p):
 *  - 최하위 비트(0x1)와 AND 연산하여 할당 여부 판별 (1: 할당됨, 0: 가용)
 */
#define GET_SIZE(p)     (GET(p) & ~0x7)
#define GET_ALLOC(p)    (GET(p) & 0x1)

/*
 * 블록 포인터(bp: 페이로드 시작점) 기준 헤더 및 푸터 주소 계산:
 *  - HDRP(bp): bp에서 1워드 앞 (bp - WSIZE)
 *  - FTRP(bp): bp + 블록 크기 - 2워드 (bp + size - DSIZE)
 *    (헤더 주소 + size = 다음 블록 시작점; 푸터는 그 직전 1워드)
 */
#define HDRP(bp)        ((char *)(bp) - WSIZE)
#define FTRP(bp)        ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/*
 * 블록 포인터(bp) 기준 인접 블록 포인터 계산:
 *  - NEXT_BLKP(bp): 현재 블록 헤더에 적힌 크기만큼 앞으로 이동하여 다음 블록 페이로드 반환
 *  - PREV_BLKP(bp): 직전 블록 푸터에 적힌 크기만큼 뒤로 이동하여 이전 블록 페이로드 반환
 */
#define NEXT_BLKP(bp)   ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp)   ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/* 정렬 관련 상수 및 매크로 */
#define ALIGNMENT       8
#define ALIGN(size)     (((size) + (ALIGNMENT - 1)) & ~0x7)
#define SIZE_T_SIZE     (ALIGN(sizeof(size_t)))

/* 전역 변수 */
static char *heap_listp;  /* 항상 프롤로그 블록의 페이로드를 가리키는 포인터 */

/* 내부 헬퍼 함수 선언 */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);

/* ========================================================================= */
/* 메모리 할당기 인터페이스 및 핵심 함수 구현                                */
/* ========================================================================= */

/**
 * @brief 동적 메모리 할당기 초기화
 *
 * 초기 빈 힙을 구성하고 초기 가용 블록을 확보함.
 * [힙 레이아웃]:
 *   1. 패딩 (4B)       : 8바이트 정렬을 맞추기 위한 미사용 공간
 *   2. 프롤로그 헤더 (4B): 크기 8B, 할당 상태(1)
 *   3. 프롤로그 푸터 (4B): 크기 8B, 할당 상태(1)
 *   4. 에필로그 헤더 (4B): 크기 0B, 할당 상태(1) (힙의 끝을 표시)
 * 이후 CHUNKSIZE 크기만큼 빈 힙을 확장함.
 *
 * @return int 초기화 성공 시 0, sbrk 실패 시 -1 반환
 */
int mm_init(void)
{
    /* 4워드(16바이트) 크기의 초기 힙 영역 요청 */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;

    PUT(heap_listp, 0);                          /* 미사용 정렬 패딩 */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* 프롤로그 헤더 */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* 프롤로그 푸터 */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     /* 에필로그 헤더 */
    heap_listp += (2 * WSIZE);                   /* 프롤로그 블록의 페이로드로 포인터 설정 */

    /* CHUNKSIZE 바이트 크기의 가용 블록으로 빈 힙 확장 */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;

    return 0;
}

/**
 * @brief 새로운 가용 블록을 할당받아 힙 영역을 확장함
 *
 * 요청된 워드 수를 8바이트 정렬을 위해 짝수 워드로 올림한 후,
 * 시스템 호출(mem_sbrk)을 통해 힙을 확장함. 새 가용 블록의 헤더와 푸터,
 * 그리고 새 에필로그 헤더를 생성한 뒤 이전 블록과의 병합을 시도함.
 *
 * @param words 확장할 크기 (바이트 단위가 아닌 워드 단위)
 * @return void* 병합 처리된 새 가용 블록의 페이로드 포인터 (확장 실패 시 NULL)
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* 8바이트 정렬 유지를 위해 짝수 개의 워드로 할당 크기 조정 */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* 새 가용 블록의 헤더/푸터 및 새로운 에필로그 헤더 설정 */
    PUT(HDRP(bp), PACK(size, 0));         /* 새 가용 블록 헤더 */
    PUT(FTRP(bp), PACK(size, 0));         /* 새 가용 블록 푸터 */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* 새 에필로그 헤더 (크기 0, 할당 1) */

    /* 이전 블록이 가용 상태였을 경우 즉시 병합 */
    return coalesce(bp);
}

/**
 * @brief 할당된 메모리 블록을 해제하고 인접 가용 블록과 병합
 *
 * 지정된 블록 포인터(bp)의 헤더와 푸터에 가용 플래그(0)를 설정한 뒤,
 * coalesce를 호출하여 물리적으로 인접한 빈 블록들을 즉시 하나로 병합함.
 *
 * @param bp 해제할 블록의 페이로드 포인터
 */
void mm_free(void *bp)
{
    if (bp == NULL)
        return;

    size_t size = GET_SIZE(HDRP(bp));

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    coalesce(bp);
}

/**
 * @brief 경계 태그(Boundary Tag)를 이용해 인접 가용 블록들을 하나로 병합
 *
 * 앞뒤 블록의 할당 상태에 따라 4가지 경우를 처리함:
 *  - Case 1: 이전/다음 블록 모두 할당됨 -> 병합 없이 bp 반환
 *  - Case 2: 이전 블록 할당됨, 다음 블록 가용 -> 다음 블록과 병합
 *  - Case 3: 이전 블록 가용, 다음 블록 할당됨 -> 이전 블록과 병합 (bp는 이전 블록으로 이동)
 *  - Case 4: 이전/다음 블록 모두 가용 -> 세 블록 모두 병합 (bp는 이전 블록으로 이동)
 *
 * @param bp 병합 대상 가용 블록의 페이로드 포인터
 * @return void* 병합 완료된 가용 블록의 페이로드 포인터
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) {            /* Case 1: 인접 블록 모두 할당 상태 */
        return bp;
    }
    else if (prev_alloc && !next_alloc) {      /* Case 2: 다음 블록만 가용 상태 */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }
    else if (!prev_alloc && next_alloc) {      /* Case 3: 이전 블록만 가용 상태 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    else {                                     /* Case 4: 이전/다음 블록 모두 가용 상태 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }

    return bp;
}

/**
 * @brief 요청한 크기(바이트) 이상의 메모리 블록을 할당
 *
 * 요청 크기에 헤더(4B) + 푸터(4B) 오버헤드를 더하고 8바이트 정렬을 충족하도록
 * 조정된 블록 크기(asize)를 계산함 (최소 블록 크기 = 16B).
 * 묵시적 가용 리스트에서 적합한 블록을 탐색(find_fit)하여 배치(place)하고,
 * 적합 블록이 없으면 힙을 확장(extend_heap)한 뒤 배치함.
 *
 * @param size 할당받고자 하는 페이로드 크기 (바이트 단위)
 * @return void* 할당된 블록의 페이로드 포인터 (실패 시 NULL)
 */
void *mm_malloc(size_t size)
{
    size_t asize;      /* 오버헤드와 정렬을 반영한 조정 블록 크기 */
    size_t extendsize; /* 적합 블록 부재 시 확장할 크기 */
    char *bp;

    /* 유효하지 않은 요청 무시 */
    if (size == 0)
        return NULL;

    /* 최소 블록 크기(16바이트) 보장 및 8바이트 배수 올림 */
    if (size <= DSIZE)
        asize = 2 * DSIZE; /* 헤더(4) + 푸터(4) + 최소 페이로드(8) */
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE);

    /* 묵시적 가용 리스트에서 적합한 블록 검색 */
    if ((bp = find_fit(asize)) != NULL) {
        place(bp, asize);
        return bp;
    }

    /* 적합 블록이 없을 경우: 힙 확장 후 배치 */
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;

    place(bp, asize);
    return bp;
}

/**
 * @brief 묵시적 가용 리스트에서 First-fit(최초 적합) 방식으로 블록 탐색
 *
 * 프롤로그 블록 다음부터 에필로그 블록(크기 0)을 만날 때까지
 * 힙 상의 블록들을 순차적으로 순회하며 요청 크기를 수용 가능한 최초의 가용 블록을 반환함.
 *
 * @param asize 필요로 하는 조정 블록 크기 (바이트 단위)
 * @return void* 조건을 만족하는 가용 블록 포인터 (발견 실패 시 NULL)
 */
static void *find_fit(size_t asize)
{
    void *bp;

    /* GET_SIZE(HDRP(bp)) > 0: 크기가 0인 에필로그 헤더를 만날 때까지 순회 */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp)))) {
            return bp;
        }
    }
    return NULL;
}

/**
 * @brief 가용 블록에 요청 크기만큼 할당 표시를 하고 남은 부분을 분할
 *
 * 블록 크기(csize)에서 요청 크기(asize)를 뺀 나머지 공간이 최소 블록 크기(16B) 이상이면
 * 블록을 앞부분(할당)과 뒷부분(새 가용 블록)으로 분할함. 미만이면 블록 전체를 할당함.
 *
 * @param bp 배치할 가용 블록의 페이로드 포인터
 * @param asize 할당할 조정 블록 크기
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));

    /* 분할 후 남은 공간이 최소 블록 크기(2*DSIZE = 16B) 이상이면 분할 수행 */
    if ((csize - asize) >= (2 * DSIZE)) {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    }
    else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/**
 * @brief 기존 메모리 블록의 크기를 변경하여 재할당
 *
 * 표준 realloc 규격을 준수하여 구현됨:
 *  - ptr == NULL인 경우: mm_malloc(size)와 동일하게 동작
 *  - size == 0인 경우: mm_free(ptr)를 호출하고 NULL 반환
 *  - 일반 재할당: 새 크기로 메모리를 할당받은 뒤, 이전 페이로드 데이터 중
 *    min(이전 크기, 새 크기) 만큼 복사하고 기존 블록은 해제함.
 *
 * @param ptr 기존에 할당된 블록의 페이로드 포인터
 * @param size 새로 요청하는 페이로드 크기
 * @return void* 재할당된 블록의 페이로드 포인터 (실패 시 NULL)
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* 포인터가 NULL이면 일반 malloc과 동일 */
    if (ptr == NULL)
        return mm_malloc(size);

    /* 크기가 0이면 메모리 해제 후 NULL 반환 */
    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }

    void *newptr = mm_malloc(size);
    if (newptr == NULL)
        return NULL;

    /* 기존 블록의 순수 페이로드 크기 = 블록 전체 크기 - 헤더/푸터(DSIZE) */
    size_t oldSize = GET_SIZE(HDRP(ptr)) - DSIZE;
    size_t copySize = (size < oldSize) ? size : oldSize;

    memcpy(newptr, ptr, copySize);
    mm_free(ptr);
    return newptr;
}
