# hview 단위 테스트 — make test로 호출.
#
# Linux/macOS: johab/sjis/hanja만 빌드 (encoding.h는 Win32 의존)
# MinGW/MSYS2: 전체 빌드 + 실행

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -I..

OUT := hview_tests
SRC := test_main.c

.PHONY: all run clean

all: run

$(OUT): $(SRC) ../johab.h ../sjis.h ../hanja.h ../encoding.h minitest.h
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)

run: $(OUT)
	./$(OUT)

clean:
	rm -f $(OUT)
