#!/bin/sh
# hview 단위 테스트 빌드 + 실행
#
# - Linux/macOS: johab 테스트만 빌드 (encoding.h는 Win32 의존)
# - MinGW/MSYS2: 전체 빌드
#
# 자체 의존성 0 정책에 맞춰 외부 테스트 프레임워크 없이 minitest.h만 사용.

set -e

CC=${CC:-gcc}
OUT=hview_tests

"$CC" -O2 -Wall -Wextra -std=c99 -I.. test_main.c -o "$OUT"
echo "빌드 완료: $OUT"
echo

./"$OUT"
