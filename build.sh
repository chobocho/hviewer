#!/bin/sh
# MinGW-w64 빌드 (MSYS2 또는 cross-compile)
# -municode: wWinMain 진입점 사용
# -mwindows: 콘솔 창 숨김
# -s: 심볼 제거로 바이너리 크기 축소

gcc -O2 -municode -mwindows -Wall -Wextra -s \
    hview.c -o hview.exe \
    -luser32 -lgdi32 -lcomdlg32 -lshell32 -ladvapi32 -lcomctl32

echo "빌드 완료: hview.exe"
ls -lh hview.exe
