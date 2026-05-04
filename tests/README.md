# hview 테스트

`DESIGN.md` §5 검증 전략에 따른 단위 테스트와 인코딩 샘플 코퍼스.

## 구성

```
tests/
├── minitest.h     의존성 없는 assert 매크로 (TEST/RUN/ASSERT_EQ 등)
├── test_main.c    단일 TU 테스트 진입점
├── build.sh       gcc / MinGW
├── build.bat      MSVC
└── samples/       인코딩별 샘플 파일 (수동/end-to-end 검증용)
```

## 빌드 + 실행

```sh
# Linux/macOS (johab.h만 검증)
./build.sh

# Windows MSVC (전체)
build.bat
```

`encoding.h`는 Win32 API(`MultiByteToWideChar` 등)에 의존하므로 비-Windows
플랫폼에서는 `_WIN32` 가드로 자동 스킵된다. johab.h는 순수 C라 어디서나 빌드된다.

## 현재 커버리지

| 모듈 | 테스트 수 | 비고 |
|------|---------:|------|
| `johab.h` | 21 | 디코더, 점수, UTF-16 변환, Hangul 절대 카운트 (Phase 0) |
| `sjis.h`  |  7 | 점수 휴리스틱, lead/trail/kana predicate 경계 (Phase 5) |
| `hanja.h` | 17 | 한자/가나 → 한글 음, CJK/가나 범위, KANA 테이블 경계 (Phase 6) |
| `encoding.h` | 33 | BOM, UTF-8 유효성(2/3/4B+overlong), CP949/Johab 점수, 보조영역 비율, 변환 디스패치, 라운드트립 (Win32 전용) |

## 향후 phase별 추가

`DESIGN.md` 의 Phase 1~7이 모듈을 추가할 때마다 동일 패턴으로
`TEST(...) + RUN(...)`을 `test_main.c`에 추가한다. 자리표시는 파일 하단의
주석 블록 참고.

## 새 테스트 추가 가이드

1. `test_main.c`에 `TEST(name) { ... }` 정의
2. `main()`에 `RUN(name);` 추가
3. `./build.sh` 또는 `build.bat`으로 회귀 확인

테스트 명명 규칙: `<모듈>_<상황>_<기대>` 예) `johab_score_too_short`,
`detect_utf8_no_bom_with_multibyte`.
