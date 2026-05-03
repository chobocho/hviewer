# hviewer

Win32 네이티브 한글 텍스트 뷰어 (외부 의존성 0).

UTF-8/UTF-16/CP949 및 조합형(Johab, KS X 1001-1992 부속서 3) 인코딩을 자동 판별하여 표시한다.

## 빌드

```
MSVC:  build.bat
MinGW: ./build.sh
```

자세한 변경 이력 및 기능 목록은 [HISTORY.md](HISTORY.md) 참고.

## 파일 구성

- `hview.c` — 메인 윈도우, 렌더링, 입력 처리
- `encoding.h` — 인코딩 자동 판별 + UTF-16 변환
- `johab.h` — 조합형(Johab) 변환 테이블 + 비트 디코더
- `build.bat` / `build.sh` — MSVC / MinGW 빌드 스크립트
