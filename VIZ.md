# viz 사용법

`viz.c`는 mm.c를 수정하지 않고 힙을 시각화하고 검사하는 디버깅 도구임. 링커 `--wrap`으로 드라이버의 `mm_init` / `mm_malloc` / `mm_free` / `mm_realloc` 호출을 가로채서, 실제 함수를 실행한 뒤 힙을 검사하고 stderr로 출력함.

## 빌드

```bash
make -f Makefile.viz        # mdriver-viz 생성
make -f Makefile.viz clean  # 정리
```

- `mm.c`를 고친 뒤에는 다시 `make -f Makefile.viz`를 실행해야 함.
- `-g`로 컴파일되어야 파일:줄 번호가 나옴 (Makefile.viz는 이미 `-g` 사용).
- 소스 위치 해석에 `addr2line`이 필요함.

## 기본 실행

```bash
./mdriver-viz -V -f short1-bal.rep      # 트레이스 하나만, 연산마다 힙 출력
./mdriver-viz -V                        # 기본 트레이스 전체
./mdriver-viz -f short1-bal.rep         # -V 없이 (드라이버 출력은 최소, viz 검사는 동작)
```

mdriver 옵션(`-f`, `-t`, `-V` 등)은 그대로 사용 가능함. 출력은 stderr이므로 파일로 저장하려면 `2> out.log`를 씀.

## 출력 모드 (`VIZ` 환경변수)

| 명령 | 설명 |
|---|---|
| `./mdriver-viz -V -f X.rep` | 텍스트 표 + 통계 (기본) |
| `VIZ=bar ./mdriver-viz ...` | ANSI 색상 막대 |
| `VIZ=map ./mdriver-viz ...` | 한 줄짜리 블록 맵 (`[2048A][2048F]...`) |
| `VIZ=error ./mdriver-viz ...` | 평소엔 조용, 오류/크래시 때만 덤프 |
| `VIZ=step ./mdriver-viz ...` | 연산마다 Enter를 눌러 한 단계씩 진행 (터미널에서만 동작) |

표 모드 예시:

```
[#3    free 2048] bp=+0x810  heap=4112B
 offset    size  state   details
 +0x0008      8  ALLOC  (prologue)
 +0x0010   2048  ALLOC
 +0x0810   2048  FREE
 +0x1010      0  ALLOC  (epilogue)
  [stats] alloc:2056B (2 blks)  free:2048B (1 blks, max:2048B)  ext_frag:0.0%  util:50.0%
```

- 오프셋(`+0x...`)은 힙 시작 기준임.
- `ext_frag` = 외부 단편화, `util` = 힙 대비 할당 비율.

## 필터

특정 연산만 보고 싶을 때 씀. 번호는 실행(run)마다 1부터 시작함.

```bash
VIZ_OP=390 ./mdriver-viz -V -f cccp-bal.rep                 # 390번 연산만
VIZ_START=380 VIZ_END=395 ./mdriver-viz -V -f cccp-bal.rep  # 380~395번
VIZ_LINK=1 ./mdriver-viz -V -f X.rep                        # free 블록의 next/prev 링크 표시
```

- `VIZ_LINK=1`은 명시적 free list를 쓸 때 의미 있음. 블록의 첫 8바이트를 next, 다음 8바이트를 prev로 해석함. 링크가 힙 안인데 free 블록 시작이 아니면 `BAD(+0x...)`, 힙 밖이면 `?포인터`로 표시함. 이 구조(next=+0, prev=+8, 최소 24바이트)와 다르면 표시 내용을 믿으면 안 됨.
- 필터가 걸려 있어도 오류 메시지는 항상 출력됨. 단, `VIZ=error`가 아니면 필터 범위 밖의 힙 덤프는 생략됨.

## 오류 시 즉시 중단

```bash
VIZ_STOP=1 ./mdriver-viz -V -f X.rep      # 첫 오류에서 abort()
```

코어 덤프가 남거나 gdb 아래에서 멈춤. 오류는 `mm_*` 호출이 끝난 뒤에 감지하므로, 멈추는 위치는 래퍼 쪽임. mm.c 내부의 정확한 줄은 아님.

## 검사 항목

매 연산 후 힙을 순회하며 다음을 확인함. 문제가 있으면 빨간색 `!!`로 출력함.

- 프롤로그/에필로그 형식, 블록이 힙을 빈틈없이 덮는지
- 예약 비트 설정, 최소 블록 크기(16) 미만
- free 블록의 header/footer 불일치
- 연속된 free 블록 (병합 안 됨)
- malloc 결과가 실제 블록 시작인지, 요청 크기보다 작지 않은지
- free한 블록이 여전히 ALLOC 표시인지
- free/realloc 인자: 힙 밖 포인터, 8바이트 정렬 위반, 이중 free, malloc이 반환한 적 없는 포인터
- 새로 할당한 블록의 payload가 live 블록과 겹치는지
- 0이 아닌 크기 요청에 NULL이 반환되는지
- 실행 종료 시 해제되지 않은 블록 (누수) 목록

## 오류 출력 읽는 법

```
 !! freed block +0x848 is still marked ALLOC
    while running op #6 free(): called at eval_mm_valid at mdriver.c:674
    implemented at mm_free at mm.c:196
    test case: short1-bal.rep (eval_mm_valid), op #6 = short1-bal.rep line 10: 'f 3'
```

- `called at`: 드라이버가 호출한 위치
- `implemented at`: 해당 진입 함수가 구현된 위치
- `test case`: 어느 트레이스, 어느 mdriver 단계, 트레이스 파일 몇 번째 줄에서 난 오류인지
- 트레이스 줄 번호는 `연산 번호 + 4` (헤더 4줄) 규칙으로 계산함.
- 같은 힙 손상은 이후 연산에서도 계속 감지되어 반복 출력될 수 있음. 가장 먼저 나온 오류를 기준으로 볼 것.
- 실행에서 처음 오류가 났을 때만 최근 호출 히스토리(16개)를 함께 출력함.

## 크래시 리포트

세그폴트(SIGSEGV), SIGBUS, SIGABRT, SIGFPE, SIGILL이 나면 다음을 출력함.

```
*** CRASH: SIGSEGV ***
  in op #390: malloc(72)
  faulted at: place at mm.c:330
    test case: cccp-bal.rep (eval_mm_valid), op #390 = cccp-bal.rep line 394: 'a 365 72'
  fault addr: 0x7d844eb53658 (OUTSIDE heap ...)

--- Recent Operations History (last 16 calls) ---
--- Call stack (innermost first) ---
  #0 place at mm.c:330
  #1 mm_malloc at mm.c:...
viz: test case(s) with heap problems before/at crash: amptjp-bal.rep cccp-bal.rep
Heap state at crash:
```

- `faulted at`: 크래시가 난 함수와 파일:줄
- `Recent Operations History`: 직전 호출들
- `Call stack`: 해석된 호출 스택 (viz 래퍼와 mdriver 프레임 포함)
- 마지막에 크래시 시점의 힙 상태를 출력함.

## 종료 요약

- 문제가 있는 실행마다: `viz: run N [트레이스, 단계]: K heap problem(s) over M calls`
- 마지막 줄: `viz: heap OK over ... calls` 또는 `viz: ... heap problem(s) ...` + `failing test case(s): ...`
- mdriver가 속도 측정을 위해 같은 트레이스를 여러 번 재실행하므로 `(12 run(s))`처럼 실행 횟수가 여러 번으로 집계됨.
- 누수 목록은 처음 누수가 발견된 실행에서만 출력함.

## 추천 디버깅 흐름

1. `VIZ=error ./mdriver-viz -V` 로 전체 트레이스를 돌려 어떤 테스트 케이스가 실패하는지 확인
2. 마지막 요약의 `failing test case(s)`에서 트레이스 하나를 고름
3. `VIZ=error ./mdriver-viz -V -f <트레이스>` 로 첫 오류와 `test case: ... line N` 확인
4. `VIZ_START=<op-10> VIZ_END=<op> ./mdriver-viz -V -f <트레이스>` 로 직전 상태를 표로 확인 (`VIZ=map`이면 더 짧게 볼 수 있음)
5. 필요하면 `gdb`에서 `mm_malloc` / `mm_free` 등에 브레이크포인트를 걸고 해당 트레이스를 재현

## 한계

- 힙 레이아웃을 프롤로그(8바이트, ALLOC), 4바이트 헤더/푸터, 에필로그 구조로 가정함. 다른 레이아웃이면 `(unrecognized heap layout)`이 출력됨.
- 오류는 `mm_*` 호출이 끝난 뒤에 감지하므로 힙을 망가뜨린 정확한 줄은 알 수 없음.
- 크래시 리포트는 `fprintf`, `popen` 등 async-signal-safe가 아닌 함수를 씀. 디버깅 용도로만 쓸 것.
- 힙 전체를 매 연산마다 순회하므로 큰 트레이스는 느림. 성능 점수 측정에는 쓰지 말고, 일반 `mdriver`로 측정할 것.
