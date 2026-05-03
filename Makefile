# hview Makefile
#
# 사용법:
#   make            # 기본 = build (MinGW-w64 가정)
#   make build      # hview.exe 빌드 (MinGW-w64 / cross-compile)
#   make test       # 단위 테스트 빌드 + 실행 (Linux/macOS/MinGW)
#   make clean      # 빌드 산출물 제거
#   make install    # PREFIX=/usr/local 또는 INSTALL_DIR로 설치
#
# MSVC 빌드는 별도로 build.bat 사용. Makefile은 GCC/Clang 계열 (MinGW 포함) 전용.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -municode -mwindows -s
LIBS    := -luser32 -lgdi32 -lcomdlg32 -lshell32 -ladvapi32

OUT     := hview.exe
SRC     := hview.c

PREFIX      ?= /usr/local
INSTALL_DIR ?= $(PREFIX)/bin

.PHONY: all build test clean install help

all: build

build: $(OUT)

$(OUT): $(SRC) encoding.h hanja.h johab.h sjis.h hview_resources.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -o $(OUT) $(LIBS)

test:
	$(MAKE) -C tests

clean:
	rm -f $(OUT)
	$(MAKE) -C tests clean

install: $(OUT)
	mkdir -p $(INSTALL_DIR)
	cp $(OUT) $(INSTALL_DIR)/

help:
	@echo "타겟: build (기본), test, clean, install [PREFIX=/path], help"
