# hview 변경 이력

## v0.1.0 (2026-05-03) — 초기 골격

### 기능
- Win32 네이티브 한글 텍스트 뷰어 (외부 의존성 0)
- 최대 64MB 텍스트 파일 지원
- 인코딩 자동 판별
  - UTF-8 (BOM 유무)
  - UTF-16 LE/BE (BOM 필수)
  - CP949 (EUC-KR)
  - **조합형 (Johab, KS X 1001-1992 부속서 3)** — 자체 구현
- 인코딩 수동 지정 메뉴 (자동 판별 실패 시)
- 키보드 + 마우스 휠 + 스크롤바 스크롤
- 가로/세로 스크롤
- 파일 드래그 앤 드롭
- 명령행 인자로 파일 받기 (탐색기 "연결 프로그램" 지원)
- HiDPI 인식 (Galaxy Fold 7 등 고해상도 디스플레이 대응)
- 폰트 크기 조절 (Ctrl+, / Ctrl+.)

### 구조
- `johab.h` — 조합형 변환 테이블 + 비트 디코더
- `encoding.h` — 인코딩 판별 + UTF-16 변환 통합
- `hview.c` — 메인 윈도우/렌더링/입력

### 알려진 한계
- 검색 기능 없음 (Ctrl+F)
- 자동 줄바꿈 없음
- 텍스트 선택/복사 없음
- 최근 파일 목록 없음
- 조합형 인코딩 판별 정확도는 ezView 수준에 미치지 못함
  - 짧은 파일/혼합 인코딩에서는 수동 지정 필요

### TODO
#### 기존
- [x] Ctrl+F 검색 (정방향/역방향) — 대소문자 구분 옵션은 후속
- [x] 텍스트 선택 + 클립보드 복사 (마우스 드래그, Ctrl+C/Ctrl+A; Shift+arrow 선택은 후속)
- [x] 자동 줄바꿈 토글 (Ctrl+W) — doc/render 라인 분리, 거터/북마크/goto는 doc 의미 유지
- [x] 최근 파일 목록 (레지스트리)
- [x] 줄 번호 표시 토글
- [x] 다크 모드

#### ezView 대비 빠진 기능 (인터넷 자료 기반)

탐색/검색
- [x] F3 다음 검색 / Shift+F3 이전 검색
- [x] 줄 이동 (Go-to-line, Ctrl+G)
- [x] 책갈피 추가/이동/삭제 (북마크) — 세션 한정, 영속화는 후속
- [x] 제목 리스트(목차) — Markdown 헤딩만, 일반 텍스트 휴리스틱은 후속
- [x] 페이지 단위 탐색 (Page Up/Down, Home/End, Ctrl+Home/End)

읽기 편의
- [x] 자동 스크롤 (Space 토글, 휠로 속도 조절)
- [x] 줄 간격 조절 (Shift+휠)
- [x] 자간 조절 (Ctrl+Shift+휠)
- [x] 여백 조절 (Alt+방향키)
- [x] 글자 크기 마우스휠 조절 (Ctrl+휠)
- [x] 두 쪽 보기 / 화면 분할 (Alt+1) — 두 페인 독립 스크롤, 휠/클릭으로 활성 페인 전환
- [x] 전체화면 토글 (F11 또는 F)
- [x] 폰트 종류 변경 (ChooseFont)

인코딩/문자
- [x] Shift-JIS 인코딩 지원 (일본어 텍스트)
- [x] 확장 완성형(Unified Hangul Code) 지원 — Windows CP949(`MultiByteToWideChar(949)`)가 UHC 슈퍼셋이라 별도 코드 없이 자동 처리
- [ ] 한자 → 한글 음 변환 표시 (한자 폰트 미설치 환경 대응)
- [x] 인코딩 변환하여 다른 이름으로 저장 (Save As)

#### 개선 사항 (코드 분석 기반)

코드 구조 / 유지보수
- [ ] Phase 1 모듈 분리 완료 — `hview.c` (3279줄)을 DESIGN.md §1.2에 따라 `wnd.c`, `view.c`, `text.c`, `font.c`, `search.c`, `selection.c` 등으로 분해
- [ ] 전역 상태 도메인 구조체로 분해 — DESIGN.md §2의 `App { Document, Viewport, Style, Search, ... }` 중첩 구조로 재설계
- [ ] Makefile 추가 — `build`, `test`, `clean`, `install` 타겟 정의 (현재 `build.bat`/`build.sh` 스크립트만 존재)

메모리 / 안전성
- [ ] `build_render_lines()` 정수 오버플로 방어 — `cap = dn + dn/4 + 16` 계산 전 `size_t` 오버플로 검증
- [ ] `realloc` 실패 시 기존 포인터 보존 — `doc_line_offsets` 등 재할당 결과를 임시 변수에 받아 NULL 체크 후 교체 (`hview.c:build_render_lines` 등 dangling pointer / 누수 방지)
- [ ] `find_substr_offset()` 대용량 파일 검색 취소 가능성 — 64MB 파일 선형 검색 시 UI 멈춤 방지 (취소 플래그 또는 진행률 콜백)

버그
- [ ] `cmd_save_as`: Best-fit 매핑 감지 불가 — `WideCharToMultiByte`(`hview.c:2684,2689`)에 `WC_NO_BEST_FIT_CHARS` 플래그 누락. 전각/반각 등이 무관 문자로 자동 치환되어도 `lpUsedDefaultChar`가 FALSE로 남아 사용자에게 경고 없이 저장됨
- [ ] `find_substr_offset()` O(n·m) 브루트포스 탐색 (`hview.c:1610`) — Boyer-Moore-Horspool 또는 KMP로 교체 (64MB × 짧은 needle 회귀 시 체감 지연)
- [ ] `hanja_to_hangul` / `kana_to_hangul` 선형 탐색 (`hanja.h:82,309`) — 테이블이 코드포인트 정렬되어 있으므로 이진 탐색으로 교체 (~600 엔트리)
- [ ] `hanja.h` 테이블 엔트리 검증 / 정렬 — 일부 엔트리가 코드포인트 순서를 벗어남 (예: `0x9F9C` 뒤에 `0x9F99`). 잘못된 음 매핑 일괄 점검 + 정렬
- [ ] Ctrl+S 키 바인딩 누락 — 도움말 텍스트(`hview.c:2163`)는 "Ctrl+S 다른 이름으로 저장"을 안내하지만 `WM_KEYDOWN` 스위치에 `'S'` 핸들러 없음. `cmd_save_as()` 직접 호출 추가 필요
- [ ] `SB_THUMBTRACK` 스크롤바 16비트 절단 (`hview.c:2907,2926`) — `HIWORD(wp)`는 16비트 한정이라 65535줄 초과 파일에서 썸 드래그 위치가 깨짐. `GetScrollInfo`로 32비트 위치 조회
- [ ] `measure_max_line_width()` 가짜 샘플링 (`hview.c:760-762`) — 주석은 "1만 줄 샘플링"이라 적혀 있지만 실제로는 앞쪽 10000줄만 측정하고 나머지는 무시. 10000줄 초과 파일에서 가장 긴 줄이 후반부에 있으면 `max_line_px` 과소 추정 → 가로 스크롤 부족. 등간격 stride 샘플링(`step = doc_line_count / 10000`) 또는 문자 수 기반 후보 선별 후 GDI 측정으로 교체

검색 / UX
- [ ] 대소문자 구분 옵션 (검색 다이얼로그 + 설정 영속화) — 현재 `hview.c:1604`에서 케이스-민감 고정
- [ ] 단어 단위 검색 (whole-word) 토글 — 단어 경계만 매칭
- [ ] 검색어 히스토리 (최근 10개) — 레지스트리에 저장
- [ ] 키보드 텍스트 선택 (Shift+방향키, Shift+Home/End, Shift+Ctrl+방향키) — 현재 마우스 드래그만 지원
- [ ] 상태 표시줄 — 현재 줄/열, 파일 크기, 인코딩, 줄 수 등 표시

설정 영속화
- [ ] 책갈피 영속화 — 파일별 책갈피를 레지스트리에 저장/복원 (현재 세션 한정)
- [ ] 윈도우 위치/크기 복원 — 종료 시 `WM_SIZE`/`WM_MOVE` 상태 저장, 시작 시 복원
- [ ] 검색 하이라이트 색상 사용자 지정 — 테마/설정 다이얼로그에서 변경 가능

문서 / 테스트
- [ ] README 확장 — 기능 요약, 빌드 방법, 단축키 표, 인코딩 지원 행렬 추가 (현재 2줄)
- [ ] 인코딩 라운드트립 통합 테스트 — 빈 파일, 단일 줄 거대 파일, 64MB 경계, BOM 처리, 혼합 인코딩 회귀 검증


### 빌드
```
MSVC:  build.bat
MinGW: ./build.sh
```

### 성능 목표
- 시작 시간 < 50ms (빈 윈도우)
- 64MB 파일 로드 < 500ms
- 바이너리 크기 < 200KB (CRT 정적 링크 시)
