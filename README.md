# hviewer

Win32 네이티브 한글 텍스트 뷰어 — 외부 의존성 0, 단일 실행 파일 < 200 KB.

UTF-8 / UTF-16 LE/BE / CP949(EUC-KR · UHC) / Shift-JIS / 조합형(Johab, KS X 1001-1992 부속서 3) 인코딩을 자동 판별하여 표시한다. 한자 음 표시(한자 → 한글 음), 가나 음 표시도 지원.

## 빌드

```
MSVC:    build.bat
MinGW:   ./build.sh   또는   make
Tests:   make test    (Linux/macOS에서는 johab/sjis/hanja만 빌드)
설치:    make install PREFIX=/usr/local
```

## 주요 기능

| 분류 | 기능 |
|------|------|
| 인코딩 | UTF-8/UTF-16(BOM 자동), CP949, Shift-JIS, 조합형 — 휴리스틱 자동 판별 + 메뉴 수동 지정 |
| 표시 | 다크 모드, 줄 번호 거터, 자동 줄바꿈, 분할 보기(2 페인 독립 스크롤), 한자→한글 음 표시 |
| 탐색 | 검색(Ctrl+F, F3, Shift+F3) — 대/소문자 구분 토글, 단어 단위 토글; 줄 이동(Ctrl+G); 책갈피(Ctrl+B / F2 / Shift+F2); 목차(Ctrl+T) |
| 읽기 편의 | 자동 스크롤(Space), 줄 간격(Shift+휠), 자간(Ctrl+Shift+휠), 여백(Alt+방향키), 폰트 크기(Ctrl+휠 또는 Ctrl+, / Ctrl+.), 폰트 종류(ChooseFont) |
| 화면 | 전체화면(F11), 두 페인 분할(Alt+1) |
| 영속화 | 최근 파일, 표시 설정, 윈도우 위치/크기, 책갈피(파일별), 검색 옵션 — 모두 HKCU 레지스트리 |
| 입출력 | 명령행 인자, 드래그&드롭, 다른 이름으로 저장(Ctrl+S, 인코딩 변환 + 손실 변환 경고) |

## 단축키 요약

| 키 | 동작 |
|----|------|
| `Ctrl+O` | 파일 열기 |
| `Ctrl+S` | 다른 이름으로 저장 |
| `Ctrl+F` / `F3` / `Shift+F3` | 찾기 / 다음 / 이전 |
| `Ctrl+G` | 줄 이동 |
| `Ctrl+T` | 목차 (Markdown 헤딩) |
| `Ctrl+B` / `F2` / `Shift+F2` | 책갈피 토글 / 다음 / 이전 |
| `Ctrl+W` | 자동 줄바꿈 토글 |
| `Ctrl+L` | 줄 번호 토글 |
| `Ctrl+D` | 다크 모드 토글 |
| `Ctrl+C` / `Ctrl+A` | 복사 / 모두 선택 |
| `Alt+1` | 분할 보기 |
| `Alt+←/→/↑/↓` | 좌/우/상/하 여백 조정 |
| `F2` / `Shift+F2` | (파일 미열림 시 인코딩 순환 — 보기 메뉴 참조) |
| `F11` | 전체화면 |
| `Space` | 자동 스크롤 토글 |
| `Esc` | 자동 스크롤 중지 / 전체화면 해제 |

## 인코딩 지원 행렬

| 인코딩 | 자동 판별 | 읽기 | 쓰기(저장) | 비고 |
|--------|-----------|------|------------|------|
| UTF-8 (BOM 유무) | ✓ | ✓ | ✓ | BOM 자동 감지 / 저장 시 옵션 |
| UTF-16 LE | ✓ (BOM 필수) | ✓ | ✓ | |
| UTF-16 BE | ✓ (BOM 필수) | ✓ | ✓ | |
| CP949 / EUC-KR | ✓ (휴리스틱) | ✓ | ✓ | UHC 슈퍼셋 — Windows MultiByteToWideChar(949) |
| Shift-JIS | ✓ (휴리스틱) | ✓ | ✓ | |
| 조합형 (Johab) | ✓ (자체 휴리스틱) | ✓ | — | KS X 1001-1992 부속서 3 / CP1361 자체 디코더 |

조합형 자동 판별은 ezView 수준에 미치지 못함 — 짧은 파일/혼합 인코딩에서는 메뉴(F2/Shift+F2)로 수동 지정 권장.

## 파일 구성

- `hview.c` — 메인 윈도우, 렌더링, 입력 처리
- `encoding.h` — 인코딩 자동 판별 + UTF-16 변환 통합
- `johab.h` — 조합형 변환 테이블 + 비트 디코더 (자체 구현)
- `sjis.h` — Shift-JIS 휴리스틱 점수
- `hanja.h` — 한자 → 한글 음 / 가나 → 한글 음 변환 테이블 (이진 탐색)
- `Makefile` / `build.bat` / `build.sh` — 빌드 스크립트
- `tests/` — 단위 테스트 (외부 프레임워크 없는 minitest.h)
- `DESIGN.md` — 아키텍처 / 모듈 분리 계획
- `HISTORY.md` — 변경 이력 + TODO

## 성능 목표

- 시작 시간 < 50 ms (빈 윈도우)
- 64 MB 파일 로드 < 500 ms
- 바이너리 크기 < 200 KB (CRT 정적 링크 시)

자세한 변경 이력 및 TODO는 [HISTORY.md](HISTORY.md) 참고.
