/*
 * sjis.h — Shift-JIS 인코딩 점수 및 판별 보조
 *
 * 변환은 Win32 MultiByteToWideChar(932, ...) 에 위임.
 * 이 파일은 detect_encoding() 에서 사용할 점수 함수만 제공.
 *
 * Shift-JIS 바이트 구조:
 *   ASCII      : 0x00-0x7F (한 바이트)
 *   반각 가나   : 0xA1-0xDF (한 바이트)
 *   2바이트 한자: lead 0x81-0x9F 또는 0xE0-0xFC
 *                trail 0x40-0x7E 또는 0x80-0xFC
 */

#ifndef SJIS_H
#define SJIS_H

#include <stddef.h>

static inline int sjis_is_lead(unsigned char b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

static inline int sjis_is_trail(unsigned char b) {
    return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC);
}

static inline int sjis_is_kana(unsigned char b) {
    return b >= 0xA1 && b <= 0xDF;
}

/* 0~100. 높을수록 Shift-JIS일 가능성 높음. */
static int sjis_score(const unsigned char *buf, size_t len) {
    if (len < 4) return 0;

    size_t attempts = 0, hits = 0;
    size_t i = 0;

    while (i < len) {
        unsigned char b0 = buf[i];

        if (b0 < 0x80) { i++; continue; }

        if (sjis_is_kana(b0)) {
            attempts++;
            hits++;
            i++;
            continue;
        }

        if (sjis_is_lead(b0)) {
            attempts++;
            if (i + 1 < len && sjis_is_trail(buf[i + 1])) {
                hits++;
                i += 2;
            } else {
                i++;
            }
            continue;
        }

        /* 0x80, 0xA0, 0xFD-0xFF — CP949/Johab 영역과 겹치지 않는 무효 바이트 */
        attempts++;
        i++;
    }

    if (attempts == 0) return 0;
    return (int)((hits * 100) / attempts);
}

#endif /* SJIS_H */
