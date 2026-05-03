/*
 * test_main.c — hview 단위 테스트 진입점.
 *
 * 빌드:
 *   Linux/macOS (johab만):       ./build.sh
 *   Windows MinGW (전체):        ./build.sh
 *   Windows MSVC (전체):          build.bat
 *
 * 단일 TU 빌드:
 *   gcc -O2 -Wall -I.. test_main.c -o hview_tests
 *
 * encoding.h는 Win32 API(MultiByteToWideChar 등)에 의존하므로 _WIN32에서만 빌드.
 * johab.h는 순수 C라 어디서든 빌드된다.
 *
 * 향후 phase별 모듈이 추가되면 같은 패턴으로 테스트 스위트를 늘린다
 * (search_tests, selection_tests 등).
 */

#include "minitest.h"

/* ------------------------------------------------------------------
 * 대상 모듈 인클루드.
 *
 * johab.h / encoding.h는 모두 static 함수 모음(헤더-온리)이라
 * 단일 TU에서 직접 인클루드하면 충분하다.
 * ------------------------------------------------------------------ */
#include "../johab.h"
#include "../sjis.h"
#include "../hanja.h"

#ifdef _WIN32
# include "../encoding.h"
#endif

/* ==================================================================
 * Phase 0 — johab.h (현재 v0.1.0 동작 잠금)
 * ================================================================== */

/*
 * 각 음절 → johab 2바이트 매핑 검산 (직접 비트 계산):
 *
 *   '가' U+AC00: cho=0(ㄱ), jung=0(ㅏ), jong=0
 *      bits = (cho_bit=2, jung_bit=3, jong_bit=1)
 *      code = 0x8000 | (2<<10) | (3<<5) | 1 = 0x8861 → bytes 0x88 0x61
 *
 *   '한' U+D55C: cho=18(ㅎ), jung=0(ㅏ), jong=4(ㄴ)
 *      bits = (20, 3, 5)
 *      code = 0x8000 | (20<<10) | (3<<5) | 5 = 0xD065 → bytes 0xD0 0x65
 *
 *   '글' U+AE00: cho=0(ㄱ), jung=18(ㅡ), jong=8(ㄹ)
 *      bits = (2, 26, 10)
 *      code = 0x8000 | (2<<10) | (26<<5) | 10 = 0x8B4A → bytes 0x8B 0x4A
 *
 *   '힣' U+D7A3: cho=18(ㅎ), jung=20(ㅣ), jong=27(ㅎ)
 *      bits = (20, 30, 31)
 *      code = 0x8000 | (20<<10) | (30<<5) | 31 = 0xD3DF → bytes 0xD3 0xDF
 */

TEST(johab_decode_ga) {
    /* '가' */
    ASSERT_EQ(johab_decode_syllable(0x8861), 0xAC00u);
}

TEST(johab_decode_han) {
    /* '한' */
    ASSERT_EQ(johab_decode_syllable(0xD065), 0xD55Cu);
}

TEST(johab_decode_geul) {
    /* '글' */
    ASSERT_EQ(johab_decode_syllable(0x8B4A), 0xAE00u);
}

TEST(johab_decode_hih) {
    /* '힣' — 마지막 한글 음절 (모든 인덱스 최대) */
    ASSERT_EQ(johab_decode_syllable(0xD3DF), 0xD7A3u);
}

TEST(johab_decode_rejects_msb_zero) {
    /* MSB=0이면 ASCII 영역 → 한글 아님 */
    ASSERT_EQ(johab_decode_syllable(0x4861), 0u);
    ASSERT_EQ(johab_decode_syllable(0x0000), 0u);
    ASSERT_EQ(johab_decode_syllable(0x7FFF), 0u);
}

TEST(johab_decode_rejects_invalid_cho) {
    /* cho_bits=0 (invalid in JOHAB_CHO) */
    uint16_t code = 0x8000 | (0 << 10) | (3 << 5) | 1;
    ASSERT_EQ(johab_decode_syllable(code), 0u);
}

TEST(johab_decode_rejects_invalid_jung) {
    /* jung_bits=0 (invalid) */
    uint16_t code = 0x8000 | (2 << 10) | (0 << 5) | 1;
    ASSERT_EQ(johab_decode_syllable(code), 0u);
}

TEST(johab_decode_rejects_invalid_jong) {
    /* jong_bits=0 (invalid) */
    uint16_t code = 0x8000 | (2 << 10) | (3 << 5) | 0;
    ASSERT_EQ(johab_decode_syllable(code), 0u);
}

TEST(johab_score_pure_ascii_is_zero) {
    /* MSB=0인 바이트만 있으면 시도 횟수 0 → score 0 */
    const unsigned char buf[] = "Hello, world.\n";
    ASSERT_EQ(johab_score(buf, sizeof(buf) - 1), 0);
}

TEST(johab_score_too_short) {
    const unsigned char buf[] = { 0x88, 0x61 };
    /* len < 4면 점수 산정 거부 */
    ASSERT_EQ(johab_score(buf, sizeof(buf)), 0);
}

TEST(johab_score_all_valid_johab_is_100) {
    /* '가한글힣' — 4 음절 모두 정상 디코드 */
    const unsigned char buf[] = {
        0x88, 0x61,  /* 가 */
        0xD0, 0x65,  /* 한 */
        0x8B, 0x4A,  /* 글 */
        0xD3, 0xDF,  /* 힣 */
    };
    ASSERT_EQ(johab_score(buf, sizeof(buf)), 100);
}

TEST(johab_score_partial_valid) {
    /* 4 시도 중 2 성공 → 50% */
    const unsigned char buf[] = {
        0x88, 0x61,  /* 가 (valid) */
        0x80, 0x00,  /* invalid (cho_bits=0) */
        0xD0, 0x65,  /* 한 (valid) */
        0x80, 0x00,  /* invalid */
    };
    ASSERT_EQ(johab_score(buf, sizeof(buf)), 50);
}

TEST(johab_to_utf16_ascii_passthrough) {
    const unsigned char src[] = "abc\n";
    wchar_t dst[16] = {0};
    size_t n = johab_to_utf16(src, sizeof(src) - 1, dst);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(dst[0], L'a');
    ASSERT_EQ(dst[1], L'b');
    ASSERT_EQ(dst[2], L'c');
    ASSERT_EQ(dst[3], L'\n');
}

TEST(johab_to_utf16_known_syllables) {
    /* '가한글힣' */
    const unsigned char src[] = {
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x4A, 0xD3, 0xDF,
    };
    wchar_t dst[16] = {0};
    size_t n = johab_to_utf16(src, sizeof(src), dst);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(dst[0], 0xAC00);
    ASSERT_EQ(dst[1], 0xD55C);
    ASSERT_EQ(dst[2], 0xAE00);
    ASSERT_EQ(dst[3], 0xD7A3);
}

TEST(johab_to_utf16_truncated_lead_yields_replacement) {
    /* MSB=1인 단일 바이트로 끝나는 입력 → U+FFFD */
    const unsigned char src[] = { 0x88 };
    wchar_t dst[4] = {0};
    size_t n = johab_to_utf16(src, sizeof(src), dst);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(dst[0], 0xFFFD);
}

TEST(johab_to_utf16_invalid_pair_yields_replacement) {
    /* 잘못된 비트 조합 → U+FFFD */
    const unsigned char src[] = { 0x80, 0x00 };
    wchar_t dst[4] = {0};
    size_t n = johab_to_utf16(src, sizeof(src), dst);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(dst[0], 0xFFFD);
}

TEST(johab_to_utf16_mixed) {
    /* "가A한" — ASCII 섞임 */
    const unsigned char src[] = {
        0x88, 0x61,        /* 가 */
        0x41,              /* A */
        0xD0, 0x65,        /* 한 */
    };
    wchar_t dst[16] = {0};
    size_t n = johab_to_utf16(src, sizeof(src), dst);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(dst[0], 0xAC00);
    ASSERT_EQ(dst[1], L'A');
    ASSERT_EQ(dst[2], 0xD55C);
}

TEST(johab_to_utf16_empty) {
    wchar_t dst[4] = { 0xDEAD };
    size_t n = johab_to_utf16(NULL, 0, dst);
    ASSERT_EQ(n, 0u);
    /* dst는 건드리지 않음 */
    ASSERT_EQ(dst[0], 0xDEAD);
}

/* ==================================================================
 * Phase 5 — sjis.h (휴리스틱 스코어, 순수 C — 어디서나 빌드)
 * ================================================================== */

TEST(sjis_score_pure_ascii_zero) {
    const unsigned char buf[] = "Hello, world.\n";
    ASSERT_EQ(sjis_score(buf, sizeof(buf) - 1), 0);
}

TEST(sjis_score_too_short) {
    const unsigned char buf[] = { 0xA1, 0xA2, 0xA3 };
    ASSERT_EQ(sjis_score(buf, sizeof(buf)), 0);
}

TEST(sjis_score_half_width_kana_all_hits) {
    /* 0xA1-0xDF 영역만으로 4 바이트 — 전부 가나 */
    const unsigned char buf[] = { 0xA1, 0xB0, 0xC5, 0xDF };
    ASSERT_EQ(sjis_score(buf, sizeof(buf)), 100);
}

TEST(sjis_score_two_byte_kanji_all_hits) {
    /* "あいうえお" — 5 hiragana × 2 = 10 bytes
     *   あ = 0x82 0xA0,  い = 0x82 0xA2,  う = 0x82 0xA4
     *   え = 0x82 0xA6,  お = 0x82 0xA8 */
    const unsigned char buf[] = {
        0x82, 0xA0, 0x82, 0xA2, 0x82, 0xA4, 0x82, 0xA6, 0x82, 0xA8,
    };
    ASSERT_EQ(sjis_score(buf, sizeof(buf)), 100);
}

TEST(sjis_score_invalid_lead_zero) {
    /* 0x80, 0xA0, 0xFD-0xFF — SJIS lead/kana 어디에도 안 맞음 */
    const unsigned char buf[] = { 0x80, 0xA0, 0xFE, 0xFF };
    ASSERT_EQ(sjis_score(buf, sizeof(buf)), 0);
}

/* ==================================================================
 * Phase 6 — hanja.h (한자 → 한글 음, 가나 → 한글 음, 범위 판별)
 *
 * 순수 C — 어디서나 빌드. hview.c의 한자 음 표시 토글이 사용.
 * ================================================================== */

TEST(hanja_known_mapping_il) {
    /* 一 (U+4E00) → 일 (U+C77C) */
    ASSERT_EQ(hanja_to_hangul(0x4E00), 0xC77Cu);
}

TEST(hanja_known_mapping_in) {
    /* 人 (U+4EBA) → 인 (U+C778) */
    ASSERT_EQ(hanja_to_hangul(0x4EBA), 0xC778u);
}

TEST(hanja_known_mapping_il_day) {
    /* 日 (U+65E5) → 일 (U+C77C) — 동음이체자도 같은 한글 음 */
    ASSERT_EQ(hanja_to_hangul(0x65E5), 0xC77Cu);
}

TEST(hanja_known_mapping_han) {
    /* 漢 (U+6F22) → 한 (U+D55C) */
    ASSERT_EQ(hanja_to_hangul(0x6F22), 0xD55Cu);
}

TEST(hanja_below_range_is_zero) {
    /* CJK 영역 미만 — 한글 영역(0xAC00) 자체도 한자 아님 */
    ASSERT_EQ(hanja_to_hangul(0x4DFF), 0u);
    ASSERT_EQ(hanja_to_hangul(0xAC00), 0u);
    ASSERT_EQ(hanja_to_hangul(L'A'),   0u);
}

TEST(hanja_above_range_is_zero) {
    /* CJK Unified Ideographs 0x4E00~0x9FFF 초과 */
    ASSERT_EQ(hanja_to_hangul(0xA000), 0u);
    ASSERT_EQ(hanja_to_hangul(0xFFFF), 0u);
}

TEST(hanja_in_range_unmapped_is_zero) {
    /* 영역 내지만 테이블에 없는 한자 — 0 반환 (호출자가 원본 유지하도록 신호) */
    ASSERT_EQ(hanja_to_hangul(0x4E02), 0u);
}

TEST(kana_hiragana_a) {
    /* あ (U+3042) → 아 */
    const wchar_t *r = kana_to_hangul(0x3042);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r[0], 0xC544);   /* 아 */
}

TEST(kana_hiragana_n) {
    /* ん (U+3093) → 응 */
    const wchar_t *r = kana_to_hangul(0x3093);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r[0], 0xC751);   /* 응 */
}

TEST(kana_katakana_normalized_to_hiragana) {
    /* ア (U+30A2) → あ → 아 — 카타카나도 동일 음 반환 */
    const wchar_t *r = kana_to_hangul(0x30A2);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r[0], 0xC544);
}

TEST(kana_out_of_range_is_null) {
    ASSERT_TRUE(kana_to_hangul(L'A')   == NULL);
    ASSERT_TRUE(kana_to_hangul(0x3000) == NULL);
    ASSERT_TRUE(kana_to_hangul(0xAC00) == NULL);
}

TEST(is_cjk_unified_range) {
    ASSERT_TRUE(is_cjk(0x4E00));
    ASSERT_TRUE(is_cjk(0x9FFF));
    ASSERT_FALSE(is_cjk(0x4DFF));
    ASSERT_FALSE(is_cjk(0xA000));
}

TEST(is_cjk_extension_a_and_compat) {
    ASSERT_TRUE(is_cjk(0x3400));    /* Extension A */
    ASSERT_TRUE(is_cjk(0x4DBF));
    ASSERT_TRUE(is_cjk(0xF900));    /* Compatibility Ideographs */
    ASSERT_TRUE(is_cjk(0xFAFF));
    ASSERT_FALSE(is_cjk(0xFB00));
}

TEST(is_cjk_rejects_hangul_and_ascii) {
    ASSERT_FALSE(is_cjk(0xAC00));   /* 가 — 한글 음절 */
    ASSERT_FALSE(is_cjk(L'A'));
    ASSERT_FALSE(is_cjk(0x3042));   /* 히라가나 */
}

TEST(is_kana_hiragana_and_katakana) {
    ASSERT_TRUE(is_kana(0x3041));   /* ぁ */
    ASSERT_TRUE(is_kana(0x309F));
    ASSERT_TRUE(is_kana(0x30A0));   /* ・ */
    ASSERT_TRUE(is_kana(0x30FF));
    ASSERT_FALSE(is_kana(0x3040));  /* 가나 직전 */
    ASSERT_FALSE(is_kana(0x3100));  /* 가나 직후 */
    ASSERT_FALSE(is_kana(0x4E00));  /* 한자 */
}

/* ==================================================================
 * Phase 0 — encoding.h (Win32 의존)
 *
 * BOM 검출, UTF-8 유효성, CP949 점수 등은 Win32 API를 호출하지 않으므로
 * 사실 어디서나 컴파일된다. 그러나 encoding.h가 windows.h를 include하므로
 * 통째로 _WIN32 가드 안에서만 빌드.
 * ================================================================== */

#ifdef _WIN32

TEST(detect_utf8_bom) {
    const unsigned char buf[] = { 0xEF, 0xBB, 0xBF, 'a', 'b' };
    ASSERT_EQ(detect_encoding(buf, sizeof(buf)), ENC_UTF8_BOM);
}

TEST(detect_utf16_le_bom) {
    const unsigned char buf[] = { 0xFF, 0xFE, 'a', 0, 'b', 0 };
    ASSERT_EQ(detect_encoding(buf, sizeof(buf)), ENC_UTF16_LE);
}

TEST(detect_utf16_be_bom) {
    const unsigned char buf[] = { 0xFE, 0xFF, 0, 'a', 0, 'b' };
    ASSERT_EQ(detect_encoding(buf, sizeof(buf)), ENC_UTF16_BE);
}

TEST(detect_utf8_no_bom_with_multibyte) {
    /* '한' UTF-8: 0xED 0x95 0x9C, 3개 시퀀스 반복하여 신뢰도 ≥3 */
    const unsigned char buf[] = {
        0xED, 0x95, 0x9C,        /* 한 */
        0xEA, 0xB8, 0x80,        /* 글 */
        0xEC, 0x82, 0xAC,        /* 사 */
        0xEB, 0x9E, 0x8C,        /* 람 */
    };
    ASSERT_EQ(detect_encoding(buf, sizeof(buf)), ENC_UTF8);
}

TEST(detect_pure_ascii_defaults_to_cp949) {
    /* ASCII만 있으면 CP949(Windows 한글 환경 기본) */
    const unsigned char buf[] = "Hello, world.\n";
    ASSERT_EQ(detect_encoding(buf, sizeof(buf) - 1), ENC_CP949);
}

TEST(detect_johab_bytes) {
    /* '가한글힣' 조합형 — 점수 차이로 johab 우선 */
    const unsigned char buf[] = {
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x4A, 0xD3, 0xDF,
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x4A, 0xD3, 0xDF,
    };
    /* johab_score = 100, cp949_score는 (lead 0x80~ 일부) 낮음
     * → ENC_JOHAB로 판별되어야 함 */
    Encoding got = detect_encoding(buf, sizeof(buf));
    ASSERT_EQ(got, ENC_JOHAB);
}

TEST(utf8_validity_rejects_invalid_lead) {
    const unsigned char buf[] = { 0xC0, 0x80 };  /* overlong NUL */
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf)), 0);
}

TEST(utf8_validity_rejects_bad_continuation) {
    /* 110xxxxx 다음에 10xxxxxx가 와야 하는데 0x41('A')이 옴 */
    const unsigned char buf[] = { 0xC2, 0x41 };
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf)), 0);
}

TEST(utf8_validity_accepts_valid_3byte) {
    /* '가' = 0xEA 0xB0 0x80 */
    const unsigned char buf[] = { 0xEA, 0xB0, 0x80 };
    ASSERT_GE(utf8_validity_score(buf, sizeof(buf)), 1);
}

TEST(utf8_validity_pure_ascii_zero) {
    const unsigned char buf[] = "abc";
    /* 멀티바이트 없으면 0 (다른 휴리스틱이 결정하라는 의미) */
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf) - 1), 0);
}

TEST(cp949_score_pure_ascii_zero) {
    const unsigned char buf[] = "abc\n";
    ASSERT_EQ(cp949_score(buf, sizeof(buf) - 1), 0);
}

TEST(cp949_score_valid_pair) {
    /* CP949 '가' = 0xB0 0xA1 — lead/trail 모두 valid 영역 */
    const unsigned char buf[] = { 0xB0, 0xA1, 0xC7, 0xD1 };  /* 가한 */
    ASSERT_EQ(cp949_score(buf, sizeof(buf)), 100);
}

TEST(convert_utf8_bom_strips_bom) {
    const unsigned char buf[] = { 0xEF, 0xBB, 0xBF, 'a', 'b', 'c' };
    size_t wlen = 0;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_UTF8_BOM, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 3u);
    ASSERT_EQ(out[0], L'a');
    ASSERT_EQ(out[1], L'b');
    ASSERT_EQ(out[2], L'c');
    free(out);
}

TEST(convert_utf16_le_strips_bom) {
    /* BOM + "ab" in UTF-16 LE */
    const unsigned char buf[] = { 0xFF, 0xFE, 'a', 0, 'b', 0 };
    size_t wlen = 0;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_UTF16_LE, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 2u);
    ASSERT_EQ(out[0], L'a');
    ASSERT_EQ(out[1], L'b');
    free(out);
}

TEST(convert_utf16_be_byteswap) {
    /* BOM + "ab" in UTF-16 BE */
    const unsigned char buf[] = { 0xFE, 0xFF, 0, 'a', 0, 'b' };
    size_t wlen = 0;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_UTF16_BE, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 2u);
    ASSERT_EQ(out[0], L'a');
    ASSERT_EQ(out[1], L'b');
    free(out);
}

TEST(convert_johab_dispatches_correctly) {
    /* '가' 조합형 */
    const unsigned char buf[] = { 0x88, 0x61 };
    size_t wlen = 0;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_JOHAB, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 1u);
    ASSERT_EQ(out[0], 0xAC00);
    free(out);
}

TEST(convert_sjis_hiragana_a) {
    /* "あ" SJIS 0x82 0xA0 → U+3042 (UTF-16 1 단위) */
    const unsigned char buf[] = { 0x82, 0xA0 };
    size_t wlen = 0;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_SJIS, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 1u);
    ASSERT_EQ(out[0], 0x3042);
    free(out);
}

TEST(convert_empty_input) {
    size_t wlen = 0xDEAD;
    wchar_t *out = convert_to_utf16((const unsigned char*)"", 0,
                                     ENC_UTF8, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 0u);
    free(out);
}

#endif /* _WIN32 */

/* ==================================================================
 * 향후 phase 테스트 자리표시 (구현 시 활성화)
 * ==================================================================
 *
 * Phase 1 (refactor): text.c — line index 추출 후
 *   TEST(line_index_lf_only) ...
 *   TEST(line_index_crlf_collapsed) ...
 *   TEST(line_index_mixed_endings) ...
 *   TEST(line_index_no_trailing_newline) ...
 *   TEST(line_index_giant_single_line_64mb) ...
 *
 * Phase 2: search.c
 *   TEST(search_forward_finds_first_occurrence) ...
 *   TEST(search_wraps_around_at_end) ...
 *   TEST(search_case_insensitive_korean) ...
 *
 * Phase 4: wrap.c
 *   TEST(wrap_breaks_on_word_boundary) ...
 *   TEST(wrap_handles_long_token_overflow) ...
 *
 * Phase 5: sjis.h
 *   TEST(detect_sjis_kana_range) ...
 *   TEST(detect_sjis_kanji_lead) ...
 *
 * 각 phase 도입 시 위 자리에 실제 TEST(...) + RUN(...) 추가.
 */

/* ------------------------------------------------------------------
 * 진입점
 * ------------------------------------------------------------------ */
int main(void) {
    printf("hview unit tests\n");
    printf("================\n\n");

    printf("[johab.h]\n");
    RUN(johab_decode_ga);
    RUN(johab_decode_han);
    RUN(johab_decode_geul);
    RUN(johab_decode_hih);
    RUN(johab_decode_rejects_msb_zero);
    RUN(johab_decode_rejects_invalid_cho);
    RUN(johab_decode_rejects_invalid_jung);
    RUN(johab_decode_rejects_invalid_jong);
    RUN(johab_score_pure_ascii_is_zero);
    RUN(johab_score_too_short);
    RUN(johab_score_all_valid_johab_is_100);
    RUN(johab_score_partial_valid);
    RUN(johab_to_utf16_ascii_passthrough);
    RUN(johab_to_utf16_known_syllables);
    RUN(johab_to_utf16_truncated_lead_yields_replacement);
    RUN(johab_to_utf16_invalid_pair_yields_replacement);
    RUN(johab_to_utf16_mixed);
    RUN(johab_to_utf16_empty);

    printf("\n[sjis.h]\n");
    RUN(sjis_score_pure_ascii_zero);
    RUN(sjis_score_too_short);
    RUN(sjis_score_half_width_kana_all_hits);
    RUN(sjis_score_two_byte_kanji_all_hits);
    RUN(sjis_score_invalid_lead_zero);

    printf("\n[hanja.h]\n");
    RUN(hanja_known_mapping_il);
    RUN(hanja_known_mapping_in);
    RUN(hanja_known_mapping_il_day);
    RUN(hanja_known_mapping_han);
    RUN(hanja_below_range_is_zero);
    RUN(hanja_above_range_is_zero);
    RUN(hanja_in_range_unmapped_is_zero);
    RUN(kana_hiragana_a);
    RUN(kana_hiragana_n);
    RUN(kana_katakana_normalized_to_hiragana);
    RUN(kana_out_of_range_is_null);
    RUN(is_cjk_unified_range);
    RUN(is_cjk_extension_a_and_compat);
    RUN(is_cjk_rejects_hangul_and_ascii);
    RUN(is_kana_hiragana_and_katakana);

#ifdef _WIN32
    printf("\n[encoding.h]\n");
    RUN(detect_utf8_bom);
    RUN(detect_utf16_le_bom);
    RUN(detect_utf16_be_bom);
    RUN(detect_utf8_no_bom_with_multibyte);
    RUN(detect_pure_ascii_defaults_to_cp949);
    RUN(detect_johab_bytes);
    RUN(utf8_validity_rejects_invalid_lead);
    RUN(utf8_validity_rejects_bad_continuation);
    RUN(utf8_validity_accepts_valid_3byte);
    RUN(utf8_validity_pure_ascii_zero);
    RUN(cp949_score_pure_ascii_zero);
    RUN(cp949_score_valid_pair);
    RUN(convert_utf8_bom_strips_bom);
    RUN(convert_utf16_le_strips_bom);
    RUN(convert_utf16_be_byteswap);
    RUN(convert_johab_dispatches_correctly);
    RUN(convert_sjis_hiragana_a);
    RUN(convert_empty_input);
#else
    printf("\n[encoding.h] skipped — requires Win32 (build on MinGW or MSVC)\n");
#endif

    TEST_SUMMARY();
}
