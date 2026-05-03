# hview 변경 설계 (v0.2.0 ~ v0.9.0)

## 0. 배경

현재 v0.1.0은 단일 `hview.c` (765줄) + header-only `encoding.h` / `johab.h`.
TODO에 추가된 ezView 대비 빠진 기능 ~30개를 그대로 한 파일에 얹으면 유지가 어렵다.
본 문서는 ① 모듈 분리, ② 전역 상태 재설계, ③ 단계별 도입 순서, ④ "외부 의존성 0" 원칙 충돌 검토를 정리한다.

---

## 1. 모듈 분리

### 1.1 빌드 모델

- header-only → 일반 `.c` + `.h` 다중 TU.
- 빌드 스크립트는 모든 `.c`를 한 명령에 나열 (Makefile 도입 보류).

```
gcc -O2 ... hview.c wnd.c view.c text.c font.c search.c selection.c \
            wrap.c bookmark.c toc.c recent.c theme.c autoscroll.c \
            settings.c saveas.c -o hview.exe ...
```

### 1.2 파일 구성

| 파일 | 역할 | 상태 |
|------|------|------|
| `hview.c` | `WinMain`, 명령행 처리, 메시지 루프 | 기존 (축소) |
| `wnd.c/.h` | `wnd_proc` 디스패치, 단축키 → 커맨드 매핑 | 신규 |
| `view.c/.h` | `WM_PAINT`, 스크롤바, 뷰포트, ScrollWindowEx | 신규 |
| `text.c/.h` | 줄 인덱스, 줄 조회, 줄 너비 측정 | 신규 |
| `font.c/.h` | 폰트 생성/메트릭, 줄 간격, 자간(`lpDx`) | 신규 |
| `encoding.h` | 자동 판별 + UTF-16 변환 | 기존 |
| `johab.h` | 조합형 디코더 | 기존 |
| `sjis.h` | Shift-JIS → UTF-16 (`CP_932` 폴백) | 신규 (Phase 5) |
| `search.c/.h` | 검색/다음/이전, 하이라이트 | 신규 (Phase 2) |
| `selection.c/.h` | 캐럿/선택, 클립보드 복사 | 신규 (Phase 2) |
| `wrap.c/.h` | 소프트 랩 라인 매핑 | 신규 (Phase 4) |
| `bookmark.c/.h` | 책갈피 배열 + 이동 | 신규 (Phase 4) |
| `toc.c/.h` | 제목 자동 추출 (Markdown 우선) | 신규 (Phase 4) |
| `recent.c/.h` | 최근 파일 (레지스트리) | 신규 (Phase 4) |
| `theme.c/.h` | 라이트/다크 팔레트 | 신규 (Phase 3) |
| `autoscroll.c/.h` | 타이머 기반 자동 스크롤 | 신규 (Phase 3) |
| `hanja.c/.h` | 한자 → 한글 음 테이블 | 신규 (Phase 6) |
| `kana.c/.h` | 카나 → 한글 음 | 신규 (Phase 6) |
| `mmap.c/.h` | 대용량 파일 메모리 매핑 | 신규 (Phase 7) |
| `saveas.c/.h` | 인코딩 변환 후 저장 | 신규 (Phase 5) |
| `settings.c/.h` | INI/레지스트리 영속화 | 신규 (Phase 3) |
| `archive/*` | HWP/ZIP/EPUB (옵션 빌드) | 신규 (Phase 8) |

---

## 2. 전역 상태 재설계

`ViewerState` (20필드) → 도메인별 구조체로 분해. `g_app` 하나만 유지하되 내부에 중첩.

```c
typedef struct {
    Document    doc;     // raw_data, text, line_offsets, encoding, filepath
    Viewport    vp;      // top_line, h_scroll_px, max_line_px, client_w/h, visible_lines
    Style       style;   // font, font_size, line_spacing, char_spacing, margin_lr, theme_id
    Search      search;  // needle, last_pos, options, match[]
    Selection   sel;     // anchor, caret
    Bookmarks   bm;
    RecentFiles recent;
    WrapState   wrap;
    AutoScroll  autos;
    SplitView   split;   // pane_count, per-pane Viewport
    HWND        hwnd;
    Settings    cfg;     // 디스크에 저장되는 사용자 설정
} App;

extern App g_app;
```

규칙:
- 각 모듈은 자기 도메인만 직접 변경 (`search.c` → `g_app.search` 만).
- 모듈 간 결합은 `view.c`가 조합 (예: 검색 결과 위치 → 스크롤).
- `Settings` 직렬화는 도메인 구조체별 `*_serialize` 함수 위임.

---

## 3. 단계별 도입 (Phases)

각 phase는 독립 릴리스 가능. 회귀 검증 통과해야 다음 phase 시작.

### Phase 1 — 리팩터링 (v0.2.0) · 기능 변화 없음
- `ViewerState` → `App` 분해
- `hview.c` 분해: `wnd.c`, `view.c`, `text.c`, `font.c`
- 빌드 스크립트 갱신
- 회귀: 드래그, Ctrl+O, 휠, 폰트 확대, 인코딩 메뉴, HiDPI 모두 동일

### Phase 2 — 핵심 텍스트 (v0.3.0)
- 검색 Ctrl+F (정/역방향, 대소문자) + F3/Shift+F3 + 매칭 하이라이트
- 줄 이동 Ctrl+G
- 텍스트 선택 (마우스 드래그, Shift+방향)
- 클립보드 복사 Ctrl+C
- 줄 번호 거터(gutter) 토글

### Phase 3 — 읽기 편의 (v0.4.0)
- 줄 간격 (Shift+휠), 자간 (Ctrl+Shift+휠, `ExtTextOutW` `lpDx`)
- 여백 (Alt+방향키 — Ctrl+방향키는 가로스크롤과 충돌하므로 변경)
- Ctrl+휠 폰트 크기
- 자동 스크롤 (Space 토글, 휠로 속도)
- 전체화면 F11
- 다크 모드 (`theme.c`)
- 폰트 종류 (`ChooseFont`)
- `settings.c`로 위 설정 모두 영속화

### Phase 4 — 탐색 (v0.5.0)
- 책갈피 (Ctrl+B 추가, F2 다음, Shift+F2 이전)
- 제목 리스트(목차) — Markdown `#` 우선, 일반 텍스트는 빈 줄 + 짧은 줄 휴리스틱
- 최근 파일 (레지스트리, 메뉴 자동 생성)
- 자동 줄바꿈 토글 (`wrap.c`)
- 두 쪽 보기 / 화면 분할 (Alt+1)

### Phase 5 — 인코딩 확장 (v0.6.0)
- Shift-JIS 지원 (`MultiByteToWideChar(932)`)
- UHC = CP949 + 추가 영역 명시 검증
- `detect_encoding`에 SJIS 휴리스틱 추가 (kana 0xA1~0xDF, 한자 lead 0x81~0x9F/0xE0~0xFC)
- `saveas.c`: 인코딩 변환 후 저장 (Save As 다이얼로그 + 인코딩 선택)

### Phase 6 — 한국어 읽기 보조 (v0.7.0)
- 한자 → 한글 음 (Unihan `kHangul` 부분 테이블 약 10K 항목, 빌드 시 `hanja_table.h`로 임베드, ~100KB)
- 가나 → 한글 음 (~100 항목)
- 메뉴 토글: 보기 → "한자 음 표시", "가나 음 표시"
- 렌더러: 단일 글리프를 음 문자열로 치환하므로 줄 너비 재계산 필요

### Phase 7 — 대용량 파일 (v0.8.0)
- `CreateFileMapping` + `MapViewOfFile`
- 라인 인덱스 lazy 구축 (가시 영역 우선 + 워커 스레드)
- 한계 64MB → 2GB
- 결정: UTF-16 변환 결과를 (a) 메모리에 통째로 저장 vs (b) 청크 단위 디코딩
  - 권장 (a) — 단순성 우선, 2GB raw → 4GB UTF-16에 64비트 빌드만 권고

### Phase 8 — 파일 형식 (v0.9.0) · 스코프 결정 필요
HWP 5.x / EPUB / ZIP 모두 deflate 필요.

| 옵션 | 의존성 0 | 바이너리 증가 | 검증 비용 |
|------|---------|--------------|----------|
| A. zlib 정적 링크 | 깨짐 | +60KB | 낮음 |
| B. **miniz 임베드 (public domain, 1파일)** | 유지 | +30KB | 낮음 |
| C. RFC1951 자체 구현 | 유지 | +10KB | 높음 (~1K줄 검증) |
| D. 별도 빌드 옵션 `-DHVIEW_ARCHIVES`만 | 기본 유지 | 0 (기본) | 사용자 분리 |

**권장: B + D 조합.** miniz를 `archive/` 하위에 두고 기본 빌드에 포함하지 않는 옵션 빌드.

---

## 4. 단축키 충돌 정리

| 키 | 현재 | 변경 후 |
|----|------|---------|
| Ctrl+,/Ctrl+. | 폰트 -/+ | 유지 |
| Ctrl+휠 | (없음) | 폰트 -/+ (ezView 호환) |
| Shift+휠 | (없음) | 줄 간격 |
| Ctrl+Shift+휠 | (없음) | 자간 |
| Ctrl+←/→ | 가로 스크롤 | **변경** → 단어 단위 이동(캐럿 도입 후) |
| Alt+←/→/↑/↓ | (없음) | 여백 조절 |
| Ctrl+F / F3 / Shift+F3 | (없음) | 검색 / 다음 / 이전 |
| Ctrl+G | (없음) | 줄 이동 |
| Ctrl+B / F2 | (없음) | 책갈피 추가 / 다음 |
| Ctrl+C | (없음) | 선택 복사 |
| F11 | (없음) | 전체화면 |
| Alt+1 | (없음) | 화면 분할 |
| Space | (없음) | 자동 스크롤 토글 |

---

## 5. 검증 / 회귀 방어

- `tests/samples/` — 인코딩별 코퍼스 (`utf8.txt`, `cp949.txt`, `johab.txt`, `sjis.txt`, 빈 파일, 단일 줄 거대 파일, 64MB 경계).
- 단위 테스트 별도 TU `test.c` (`#ifdef HVIEW_TEST`로 `main` 분기).
- Phase 1 종료 시: 동일 파일 열어 동일 화면 시각 비교.
- 인코딩 추가 phase마다 코퍼스 재실행.

---

## 6. 비-목표 (명시적 제외)

- 텍스트 편집 (뷰어 한정)
- 정규식 검색 (substring만; v1.x 검토)
- 신택스 하이라이팅
- 플러그인 시스템
- 자체 폰트 렌더러 (GDI에 위임)
- 모바일/타 OS 포팅 (Win32 한정)

---

## 7. 의존성 0 원칙과의 정합성

| 기능군 | 의존성 추가? | 비고 |
|--------|-------------|------|
| Phase 1~5 | 없음 | Win32 API + CRT만 |
| Phase 6 (한자/가나) | 없음 (테이블 임베드) | 빌드 시 헤더 생성 |
| Phase 7 (mmap) | 없음 | Win32 `CreateFileMapping` |
| Phase 8 (HWP/EPUB) | **선택적** | miniz 임베드(권장) 또는 옵션 빌드 |

기본 빌드 산출물은 v0.9.0까지도 외부 라이브러리 0 유지 가능.
