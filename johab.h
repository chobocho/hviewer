/*
 * johab.h — 조합형(Johab, KS X 1001-1992 부속서 3) 한글 디코더
 *
 * 조합형 2바이트 비트 구조:
 *   비트:  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 *          1  [---초성 5비트--] [--중성 5비트--] [--종성 5비트-]
 *
 *   - MSB(bit 15) = 1 이면 한글, 0 이면 ASCII
 *   - 초성 비트값 0x02~0x14 (19개)
 *   - 중성 비트값 불연속 (21개)
 *   - 종성 비트값 0x01(받침없음) ~ 0x1D 불연속 (28개)
 *
 * 유니코드 변환:
 *   U+AC00 = '가', U+D7A3 = '힣'
 *   syllable = 0xAC00 + (cho * 21 + jung) * 28 + jong
 *
 * 비한글 영역(한자/특수)은 실제 사용 사례가 미미해 이 구현에서는 미지원.
 *
 * 다른 언어 비교:
 *   - Python: codecs.lookup('johab') 내장
 *   - Java: Charset.forName("x-Johab") 내장
 *   - Go/C/Win32: 표준 지원 없음 → 직접 구현 (이 파일)
 */

#ifndef JOHAB_H
#define JOHAB_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* 비트값 → 인덱스 매핑. 0xFF = invalid. */

/* 초성: 비트값 0x02~0x14가 인덱스 0~18 */
static const unsigned char JOHAB_CHO[32] = {
    0xFF, 0xFF, 0,    1,    2,    3,    4,    5,
    6,    7,    8,    9,    10,   11,   12,   13,
    14,   15,   16,   17,   18,   0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* 중성: 21개. KS X 1001 부속서 3(조합형) 비트값 매핑.
 *   bits 3..7   → ㅏㅐㅑㅒㅓ (idx 0..4)
 *   bits 10..15 → ㅔㅕㅖㅗㅘㅙ (idx 5..10)
 *   bits 18..23 → ㅚㅛㅜㅝㅞㅟ (idx 11..16)
 *   bits 26..29 → ㅠㅡㅢㅣ    (idx 17..20)
 * 빈 비트값(0,1,2 / 8,9 / 16,17 / 24,25 / 30,31)은 invalid.
 *
 * 빈 칸 패턴: trail 바이트 = (jung_bits & 7)<<5 | jong_bits 가
 * 제어문자(0x00~0x1F)와 겹치지 않도록 jung 그룹의 시작을 맞춘 결과.
 * Microsoft CP1361 (Windows Korean Johab) 인코딩과 동등. */
static const unsigned char JOHAB_JUNG[32] = {
    0xFF, 0xFF, 0xFF,  0,    1,    2,    3,    4,
    0xFF, 0xFF,  5,    6,    7,    8,    9,    10,
    0xFF, 0xFF, 11,   12,   13,   14,   15,   16,
    0xFF, 0xFF, 17,   18,   19,   20,   0xFF, 0xFF
};

/* 종성: 28개. KS X 1001 부속서 3(조합형) 비트값 매핑.
 *   bits 1..17  → 받침없음/ㄱ/ㄲ/ㄳ/ㄴ/ㄵ/ㄶ/ㄷ/ㄹ/ㄺ/ㄻ/ㄼ/ㄽ/ㄾ/ㄿ/ㅀ/ㅁ (idx 0..16)
 *   bits 19..29 → ㅂ/ㅄ/ㅅ/ㅆ/ㅇ/ㅈ/ㅊ/ㅋ/ㅌ/ㅍ/ㅎ                       (idx 17..27)
 * 빈 비트값(0, 18, 30, 31)은 invalid.
 *
 * 종성 idx 0(받침없음)은 비트값 1에 매핑 — 종성 부재 음절도 trail
 * 바이트가 0이 되지 않도록 한 표준의 의도. Microsoft CP1361과 동등. */
static const unsigned char JOHAB_JONG[32] = {
    0xFF,  0,    1,    2,    3,    4,    5,    6,
     7,    8,    9,    10,   11,   12,   13,   14,
    15,   16,   0xFF, 17,   18,   19,   20,   21,
    22,   23,   24,   25,   26,   27,   0xFF, 0xFF
};

/* ------------------------------------------------------------------
 * 한 음절 디코드.
 * 입력: 2바이트 (big-endian으로 결합한 16비트 값)
 * 반환: 유니코드 코드포인트 (성공) 또는 0 (실패)
 * ------------------------------------------------------------------ */
static inline uint32_t johab_decode_syllable(uint16_t code) {
    /* MSB가 1이어야 한글 */
    if ((code & 0x8000) == 0) return 0;

    unsigned cho_bits  = (code >> 10) & 0x1F;
    unsigned jung_bits = (code >>  5) & 0x1F;
    unsigned jong_bits =  code        & 0x1F;

    unsigned cho  = JOHAB_CHO[cho_bits];
    unsigned jung = JOHAB_JUNG[jung_bits];
    unsigned jong = JOHAB_JONG[jong_bits];

    if (cho == 0xFF || jung == 0xFF || jong == 0xFF) return 0;

    return 0xAC00 + (cho * 21 + jung) * 28 + jong;
}

/* ------------------------------------------------------------------
 * 조합형 바이트 스트림이 "그럴듯한지" 점수를 매김.
 *
 * 인코딩 자동 판별용. 0~100점 (높을수록 조합형일 가능성 높음).
 *
 * 알고리즘:
 *   1. MSB=1인 바이트를 lead로 보고 다음 바이트와 짝지어 디코드 시도
 *   2. 성공한 음절 수 / 시도한 음절 수의 비율
 *   3. ASCII 영역(MSB=0)은 양쪽 인코딩 모두 동일하므로 점수에서 제외
 *
 * 짧은 파일에서는 신뢰도가 낮으므로 호출 측에서 길이 체크 필요.
 * ------------------------------------------------------------------ */
static int johab_score(const unsigned char *buf, size_t len) {
    if (len < 4) return 0;

    size_t attempts = 0;
    size_t hits = 0;
    size_t i = 0;

    while (i + 1 < len) {
        unsigned char b0 = buf[i];

        if ((b0 & 0x80) == 0) {
            /* ASCII는 평가 제외 */
            i++;
            continue;
        }

        /* 한글 후보 — 2바이트 시도 */
        uint16_t code = ((uint16_t)b0 << 8) | buf[i + 1];
        attempts++;
        if (johab_decode_syllable(code) != 0) {
            hits++;
        }
        i += 2;
    }

    if (attempts == 0) return 0;
    return (int)((hits * 100) / attempts);
}

/* ------------------------------------------------------------------
 * 조합형 → UTF-16 변환.
 *
 * 입력: src (조합형 바이트), src_len
 * 출력: dst (호출자가 할당, 최소 src_len * sizeof(wchar_t) 바이트)
 * 반환: 변환된 wchar_t 개수
 *
 * 변환 규칙:
 *   - MSB=0인 바이트: ASCII로 그대로 통과
 *   - MSB=1인 바이트 + 다음 바이트: 조합형 음절 시도
 *     - 성공: 유니코드 한글 음절
 *     - 실패: U+FFFD (replacement character)로 대체
 *
 * 모든 한글 음절(11172자)은 BMP 내부라 surrogate pair 불필요.
 * ------------------------------------------------------------------ */
static size_t johab_to_utf16(const unsigned char *src, size_t src_len,
                              wchar_t *dst) {
    size_t out = 0;
    size_t i = 0;

    while (i < src_len) {
        unsigned char b0 = src[i];

        if ((b0 & 0x80) == 0) {
            /* ASCII */
            dst[out++] = (wchar_t)b0;
            i++;
            continue;
        }

        if (i + 1 >= src_len) {
            /* 잘린 멀티바이트 — replacement */
            dst[out++] = 0xFFFD;
            i++;
            continue;
        }

        uint16_t code = ((uint16_t)b0 << 8) | src[i + 1];
        uint32_t uc = johab_decode_syllable(code);
        dst[out++] = uc ? (wchar_t)uc : (wchar_t)0xFFFD;
        i += 2;
    }

    return out;
}

#endif /* JOHAB_H */
