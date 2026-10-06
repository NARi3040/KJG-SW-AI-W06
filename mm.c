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

static char *heap_listp;  /* 프롤로그 블록을 가리키는 포인터 */
static char *last_bp;     /* Next-fit: 마지막 탐색 지점을 가리키는 포인터 */

static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);

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
    heap_listp += (2*WSIZE); /* 시작점 블록을 넣을 시작점 / 빈공간도 찾고 넣을곳도 정하는 포인터 */
    last_bp = heap_listp;

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

/*
 * coalesce - 경계 태그(boundary-tag)를 사용한 가용 블록 병합
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp))); // 이전 블록 할당 여부
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp))); // 다음 블록 할당 여부
    size_t size = GET_SIZE(HDRP(bp)); // 현 블록 사이즈

    if (prev_alloc && next_alloc) {            /* 경우 1: 앞뒤 블록 모두 할당됨 */
        return bp;
    }

    else if (prev_alloc && !next_alloc) {      /* 경우 2: 이전 블록 할당됨, 다음 블록 가용 상태 */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))); // 현 블록에 다음 블록 사이즈 더하기
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }

    else if (!prev_alloc && next_alloc) {      /* 경우 3: 이전 블록 가용 상태, 다음 블록 할당됨 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp); // 이전 블록 bp 반환 
    }

    else {                                     /* 경우 4: 앞뒤 블록 모두 가용 상태 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
            GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp); // 가장 앞에 있는 이전 블록 bp 반환
    }

    /* 
     * Next-Fit last_bp 동기화 이유
     * 인접 블록들이 하나로 병합되면, 병합된 이전/이후 블록들의 기존 헤더 위치는
     * 이제 하나의 커다란 새 블록의 "데이터(페이로드) 영역 한가운데"로 편입됨.
     * 만약 last_bp가 병합된 영역 내부([bp, NEXT_BLKP(bp)))를 가리키고 있었다면,
     * 다음 find_fit 순회 시 데이터 영역의 쓰레기 값을 블록 헤더로 잘못 읽게 됨.
     * 이로 인해 힙 탐색이 깨지거나 이미 할당된 주소를 중복 반환하는
     * 'Payload overlap' 치명적 오류가 발생하므로, 유효한 새 블록 시작점인 bp로 갱신함.
     */
    last_bp = bp; /* 병합 결과 블록에서 다음 탐색 시작 */

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

    /* 오버헤드 및 정렬 요건을 포함하도록 블록 크기 조정 */
    if (size <= DSIZE)
        asize = 2*DSIZE; // 8보다 작으니 8b로 고정
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE); // 8의 배수로 맞춰주기

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

/* Next-fit(다음 적합): 마지막 탐색 지점부터 순환 탐색 */
static void *find_fit(size_t asize)
{
    char *bp;

    /* 1. last_bp부터 힙 끝(에필로그)까지 탐색 */
    for (bp = last_bp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp)))) {
            return bp;
        }
    }

    /* 2. 힙 맨 처음부터 last_bp 직전까지 순환 탐색 */
    for (bp = heap_listp; bp < last_bp; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp)))) {
            return bp;
        }
    }

    return NULL; /* 진짜로 자리가 없음 */
}

/*
 * place - 가용 블록에 요청 블록을 배치하고, 남은 크기가 최소 블록 크기 이상이면 분할
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));

    if ((csize - asize) >= (2*DSIZE)) { // 남는 공간이 최소 블록 크기(16B) 이상이면 분할
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp); // 분할된 가용 블록으로 이동
        PUT(HDRP(bp), PACK(csize-asize, 0));
        PUT(FTRP(bp), PACK(csize-asize, 0));
        /*
         * 분할 시 last_bp 갱신 이유
         * 방금 할당된 앞쪽 블록(ALLOC=1)은 건너뛰고, 분할되어 새로 생성된
         * 뒤쪽의 '남은 가용 블록(bp)'을 다음 탐색 시작점으로 지정함.
         * 이렇게 해야 다음 malloc 요청 시 방금 남겨둔 빈 공간을 즉시 탐색하여 재사용할 수 있음.
         */
        last_bp = bp;       // 다음 탐색은 남은 가용 블록부터 시작
    }
    else { // 분할하지 않고 블록 전체 할당
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        /*
         * 미분할 시 last_bp 갱신 이유
         * 현재 블록 전체가 할당(ALLOC=1)되었으므로, 더 이상 빈 공간이 아님.
         * 만약 last_bp를 그대로 두면 다음 find_fit에서 방금 할당 완료된 블록을
         * 불필요하게 다시 검사하게 되므로, 다음 블록(NEXT_BLKP(bp))으로 넘겨줌.
         */
        last_bp = NEXT_BLKP(bp); // 다음 탐색은 다음 블록부터 시작
    }
}


/*
 * mm_realloc - mm_malloc과 mm_free를 사용하여 단순하게 구현
 */
void *mm_realloc(void *ptr, size_t size)
{
    /*
     * 최적화 Realloc
     * 1) 크기 축소/유지: 제자리 반환 + 16B 이상 남으면 분할
     * 2) 다음 블록이 가용 블록: 흡수하여 크기 충족 시 memcpy 없이 제자리 확장
     * 3) 다음 블록이 에필로그: 힙 끝 부족분만 mem_sbrk로 늘려 memcpy 없이 제자리 확장
     * 4) 불가피할 때만 새로 malloc -> memcpy -> free 수행
     */
    void *oldptr = ptr;
    void *newptr;
    size_t copySize;

    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }
    if (!ptr)
        return mm_malloc(size);

    size_t csize = GET_SIZE(HDRP(oldptr));
    size_t asize;
    if (size <= DSIZE) {
        asize = 2 * DSIZE;
    }
    else {
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    }
    
    /* 케이스 1: 현재 블록 크기 >= 요청 크기 (축소 또는 유지) */
    if (csize >= asize) {
        if ((csize - asize) >= (2 * DSIZE)) {
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
        char *old_next = NEXT_BLKP(oldptr); /* 흡수될 다음 블록 (last_bp 보정용) */
        if ((total_size - asize) >= (2 * DSIZE)) {
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
        /* 흡수된 블록의 헤더는 사라지므로 last_bp가 가리키고 있었다면 유효한 블록으로 이동 */
        if (last_bp == old_next)
            last_bp = NEXT_BLKP(oldptr);
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

        copySize = csize - DSIZE;
        if (size < copySize)
            copySize = size;
        memcpy(newptr, oldptr, copySize);
        mm_free(oldptr);
        return newptr;
    }
}
