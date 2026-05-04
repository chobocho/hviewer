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
 * 각 음절 → johab 2바이트 매핑 검산 (KS X 1001 부속서 3 / CP1361):
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
 *      bits = (2, 27, 9)
 *      code = 0x8000 | (2<<10) | (27<<5) | 9 = 0x8B69 → bytes 0x8B 0x69
 *
 *   '힣' U+D7A3: cho=18(ㅎ), jung=20(ㅣ), jong=27(ㅎ)
 *      bits = (20, 29, 29)
 *      code = 0x8000 | (20<<10) | (29<<5) | 29 = 0xD3BD → bytes 0xD3 0xBD
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
    ASSERT_EQ(johab_decode_syllable(0x8B69), 0xAE00u);
}

TEST(johab_decode_hih) {
    /* '힣' — 마지막 한글 음절 (모든 인덱스 최대) */
    ASSERT_EQ(johab_decode_syllable(0xD3BD), 0xD7A3u);
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
        0x8B, 0x69,  /* 글 */
        0xD3, 0xBD,  /* 힣 */
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
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x69, 0xD3, 0xBD,
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

TEST(johab_hangul_count_empty) {
    ASSERT_EQ(johab_hangul_count(NULL, 0), 0u);
    const unsigned char ascii[] = "Hello";
    ASSERT_EQ(johab_hangul_count(ascii, sizeof(ascii) - 1), 0u);
}

TEST(johab_hangul_count_all_valid) {
    /* '가한글힣' 4 음절 — 절대 카운트 4 */
    const unsigned char buf[] = {
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x69, 0xD3, 0xBD,
    };
    ASSERT_EQ(johab_hangul_count(buf, sizeof(buf)), 4u);
}

TEST(johab_hangul_count_skips_ascii_and_invalid) {
    /* ASCII 섞인 입력에서 한글만 셈. invalid pair는 카운트 안 됨. */
    const unsigned char buf[] = {
        0x88, 0x61,        /* 가 (valid) */
        0x41,              /* 'A' — ASCII, skipped */
        0xD0, 0x65,        /* 한 (valid) */
        0x80, 0x00,        /* invalid (cho_bits=0) */
        0x8B, 0x69,        /* 글 (valid) */
    };
    ASSERT_EQ(johab_hangul_count(buf, sizeof(buf)), 3u);
}

TEST(johab_to_utf16_output_count_bounded_by_src_len) {
    /* dst 버퍼 크기 가정 잠금 — convert_to_utf16의 ENC_JOHAB 분기는
     * (src_len+1) wchar_t를 할당한다. 모든 분기(ASCII/valid/invalid/잘림)에서
     * 출력 wchar_t 개수가 src_len을 넘지 않아야 그 가정이 안전하다. */
    const unsigned char src[] = {
        'A',                /* ASCII: 1B → 1w */
        0x88, 0x61,         /* valid 가: 2B → 1w */
        0x80, 0x00,         /* invalid pair: 2B → 1w (FFFD) */
        0xD0,               /* 잘린 lead: 1B → 1w (FFFD) */
    };
    wchar_t dst[16] = {0};
    size_t n = johab_to_utf16(src, sizeof(src), dst);
    ASSERT_TRUE(n <= sizeof(src));
    ASSERT_EQ(n, 4u);
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

TEST(sjis_predicates_boundaries) {
    /* lead: 0x81-0x9F, 0xE0-0xFC */
    ASSERT_FALSE(sjis_is_lead(0x80));
    ASSERT_TRUE (sjis_is_lead(0x81));
    ASSERT_TRUE (sjis_is_lead(0x9F));
    ASSERT_FALSE(sjis_is_lead(0xA0));
    ASSERT_FALSE(sjis_is_lead(0xDF));   /* 가나 영역 */
    ASSERT_TRUE (sjis_is_lead(0xE0));
    ASSERT_TRUE (sjis_is_lead(0xFC));
    ASSERT_FALSE(sjis_is_lead(0xFD));

    /* trail: 0x40-0x7E, 0x80-0xFC */
    ASSERT_FALSE(sjis_is_trail(0x3F));
    ASSERT_TRUE (sjis_is_trail(0x40));
    ASSERT_TRUE (sjis_is_trail(0x7E));
    ASSERT_FALSE(sjis_is_trail(0x7F));
    ASSERT_TRUE (sjis_is_trail(0x80));
    ASSERT_TRUE (sjis_is_trail(0xFC));
    ASSERT_FALSE(sjis_is_trail(0xFD));

    /* 반각 가나: 0xA1-0xDF */
    ASSERT_FALSE(sjis_is_kana(0xA0));
    ASSERT_TRUE (sjis_is_kana(0xA1));
    ASSERT_TRUE (sjis_is_kana(0xDF));
    ASSERT_FALSE(sjis_is_kana(0xE0));
}

TEST(sjis_score_lead_without_trail) {
    /* 첫 페어는 valid kanji, 둘째 lead 0x82는 trail 0x00 — invalid trail.
     *   attempts=2, hits=1 → 50점. 잘린 lead가 hit로 잡히지 않음을 확인. */
    const unsigned char buf[] = { 0x82, 0xA0, 0x82, 0x00 };
    ASSERT_EQ(sjis_score(buf, sizeof(buf)), 50);
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

TEST(kana_table_first_and_last) {
    /* KANA_TABLE 첫 엔트리 ぁ(0x3041) 와 마지막 엔트리 ん(0x3093) — 이진
     * 탐색 경계가 양 끝을 빠뜨리지 않는지 확인. */
    const wchar_t *first = kana_to_hangul(0x3041);
    ASSERT_TRUE(first != NULL);
    ASSERT_EQ(first[0], 0xC544);    /* 아 */

    const wchar_t *last = kana_to_hangul(0x3093);
    ASSERT_TRUE(last != NULL);
    ASSERT_EQ(last[0], 0xC751);     /* 응 */
}

TEST(kana_table_gap_returns_null) {
    /* KANA_TABLE에 비어 있는 코드포인트 (ゐ U+3090, ゑ U+3091 — 폐자
     * 히라가나로 현대 일본어 미사용) — 이진 탐색이 NULL 반환해야 함. */
    ASSERT_TRUE(kana_to_hangul(0x3090) == NULL);
    ASSERT_TRUE(kana_to_hangul(0x3091) == NULL);
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

TEST(detect_empty_buffer_falls_back_to_cp949) {
    /* 빈 입력은 BOM 검사·점수 휴리스틱 모두 0 → CP949 default 분기.
     * NULL 호출도 안전해야 함 (모든 score 함수가 길이 0에서 즉시 0 반환). */
    ASSERT_EQ(detect_encoding(NULL, 0), ENC_CP949);
    ASSERT_EQ(detect_encoding((const unsigned char*)"", 0), ENC_CP949);
}

TEST(detect_sjis_japanese_text) {
    /* "日本語日本語" SJIS — sjis_score > cp949_score + 20 분기 검증.
     *   日 = 0x93 0xFA,  本 = 0x96 0x7B,  語 = 0x8C 0xEA
     * 0x96 0x7B의 trail 0x7B는 SJIS valid(0x40-0x7E)이지만 CP949 invalid
     * (CP949 trail은 0x7A까지)이므로 cp949_score가 떨어진다 → SJIS 우선. */
    const unsigned char buf[] = {
        0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA,
        0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA,
    };
    ASSERT_EQ(detect_encoding(buf, sizeof(buf)), ENC_SJIS);
}

TEST(detect_johab_real_korean_text) {
    /* "에이치 뷰어는 한글코드를 완"의 실제 Johab 바이트. 일반 한글
     * 텍스트라 cp949_score와 johab_score가 모두 100인 동률 케이스 —
     * 점수만으로는 구분 불가.
     *
     * 보조 영역 휴리스틱이 동작해야 정확히 ENC_JOHAB으로 판별됨:
     *   trail 바이트가 0x41/0x61/0x65/0x69 등 0x41~0x7A에 자주 떨어짐 →
     *   cp949_extension_ratio >= 30% → Johab으로 결정. */
    const unsigned char buf[] = {
        0xB5, 0x41,  /* 에 */
        0xB7, 0xA1,  /* 이 */
        0xC3, 0xA1,  /* 치 */
        0x20,        /* space */
        0xA7, 0x41,  /* 뷰 */
        0xB4, 0xE1,  /* 어 */
        0x93, 0x65,  /* 는 */
        0x20,        /* space */
        0xD0, 0x65,  /* 한 */
        0x8B, 0x69,  /* 글 */
        0xC5, 0xA1,  /* 코 */
        0x97, 0x61,  /* 드 */
        0x9F, 0x69,  /* 를 */
        0x20,        /* space */
        0xB5, 0xC5,  /* 완 */
    };
    Encoding got = detect_encoding(buf, sizeof(buf));
    ASSERT_EQ(got, ENC_JOHAB);
}

TEST(detect_johab_bytes) {
    /* CP949 valid trail 영역(0x41~0x5A, 0x61~0x7A, 0x81~0xFE)을
     * 모두 벗어나는 trail 바이트로 구성된 Johab 음절들.
     *
     * Johab 비트 구조:  code = 1<<15 | cho_bits<<10 | jung_bits<<5 | jong_bits
     *                  trail = (jung_bits & 7) << 5 | jong_bits
     *
     * jung_bits=10(ㅔ) → 하위 3비트=2 → trail = 0x40 + jong_bits
     *   jong_bits=27..29 (ㅋ,ㅌ,ㅍ) → trail 0x5B..0x5D — CP949 invalid trail.
     *
     * 그러므로 cp949_score = 0, sjis_score = 0, johab_score = 100
     *   → 점수 비교에서 동률 없이 ENC_JOHAB로 판별. */
    const unsigned char buf[] = {
        0x89, 0x5B,  /* cho_bits=2(ㄱ), jung_bits=10(ㅔ), jong_bits=27(ㅋ) */
        0x91, 0x5B,  /* cho_bits=4(ㄴ), jung_bits=10,    jong_bits=27 */
        0x89, 0x5C,  /* cho_bits=2,    jung_bits=10,    jong_bits=28(ㅌ) */
        0x91, 0x5C,  /* cho_bits=4,    jung_bits=10,    jong_bits=28 */
        0x89, 0x5D,  /* cho_bits=2,    jung_bits=10,    jong_bits=29(ㅍ) */
        0x91, 0x5D,  /* cho_bits=4,    jung_bits=10,    jong_bits=29 */
        0x89, 0x5B,  /* (반복으로 8 음절 채움) */
        0x91, 0x5B,
    };
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

/* ------------------------------------------------------------------
 * 라운드트립 통합 테스트 — bytes → wide → bytes 후 원본과 동치 확인.
 *
 * Save As (cmd_save_as)가 사용하는 인코딩 변환 경로의 회귀 보호.
 * UTF-8 / UTF-16 / CP949 / Shift-JIS 케이스를 다룸. Johab은 디코더만
 * 있으므로 라운드트립 불가, 단방향 검증으로 대체.
 * ------------------------------------------------------------------ */
static int roundtrip_via_wcb(const unsigned char *src, size_t src_len,
                             Encoding enc, UINT cp) {
    size_t wlen = 0;
    wchar_t *w = convert_to_utf16(src, src_len, enc, &wlen);
    if (!w) return 0;
    int n = WideCharToMultiByte(cp, 0, w, (int)wlen, NULL, 0, NULL, NULL);
    unsigned char *back = (unsigned char*)malloc((size_t)n);
    int ok = 0;
    if (back) {
        WideCharToMultiByte(cp, 0, w, (int)wlen, (char*)back, n, NULL, NULL);
        /* BOM 사용 인코딩은 convert_to_utf16에서 BOM이 떨어져 나가므로
         * 비교 시 src에서 BOM 길이만큼 건너뜀 (호출자가 prefix 지정) */
        ok = ((size_t)n == src_len) && (memcmp(back, src, src_len) == 0);
        free(back);
    }
    free(w);
    return ok;
}

TEST(roundtrip_utf8_korean) {
    /* "안녕" UTF-8 */
    const unsigned char src[] = { 0xEC, 0x95, 0x88, 0xEB, 0x85, 0x95 };
    ASSERT_TRUE(roundtrip_via_wcb(src, sizeof(src), ENC_UTF8, CP_UTF8));
}

TEST(roundtrip_cp949_korean) {
    /* "가한" CP949: 0xB0 0xA1 0xC7 0xD1 */
    const unsigned char src[] = { 0xB0, 0xA1, 0xC7, 0xD1 };
    ASSERT_TRUE(roundtrip_via_wcb(src, sizeof(src), ENC_CP949, 949));
}

TEST(roundtrip_sjis_kana) {
    /* "あい" SJIS: 0x82 0xA0, 0x82 0xA2 */
    const unsigned char src[] = { 0x82, 0xA0, 0x82, 0xA2 };
    ASSERT_TRUE(roundtrip_via_wcb(src, sizeof(src), ENC_SJIS, 932));
}

TEST(roundtrip_utf8_bom_strips_then_restores) {
    /* BOM 부 입력을 ENC_UTF8_BOM로 변환 → BOM 제거됨. 다시 인코드하면
     * BOM 없는 본문만 나와야 함 (cmd_save_as는 BOM을 별도로 prepend). */
    const unsigned char src[] = { 0xEF, 0xBB, 0xBF, 'a', 'b', 'c' };
    size_t wlen = 0;
    wchar_t *w = convert_to_utf16(src, sizeof(src), ENC_UTF8_BOM, &wlen);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(wlen, 3u);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, NULL, 0, NULL, NULL);
    ASSERT_EQ(n, 3);
    unsigned char back[8] = {0};
    WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, (char*)back, 8, NULL, NULL);
    ASSERT_EQ(back[0], 'a');
    ASSERT_EQ(back[1], 'b');
    ASSERT_EQ(back[2], 'c');
    free(w);
}

TEST(roundtrip_johab_to_utf16_one_way) {
    /* Johab은 디코더 only — utf-16 변환 결과만 검증 */
    const unsigned char src[] = { 0x88, 0x61, 0xD0, 0x65 }; /* "가한" */
    size_t wlen = 0;
    wchar_t *w = convert_to_utf16(src, sizeof(src), ENC_JOHAB, &wlen);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(wlen, 2u);
    ASSERT_EQ(w[0], 0xAC00);
    ASSERT_EQ(w[1], 0xD55C);
    free(w);
}

TEST(encoding_name_lookup_known) {
    /* 상태바/메뉴 표시에 쓰는 라벨 — 모든 enum이 비-NULL을 반환해야 한다. */
    ASSERT_TRUE(wcscmp(encoding_name(ENC_UTF8_BOM), L"UTF-8 (BOM)") == 0);
    ASSERT_TRUE(wcscmp(encoding_name(ENC_UTF16_LE), L"UTF-16 LE")   == 0);
    ASSERT_TRUE(wcscmp(encoding_name(ENC_UTF16_BE), L"UTF-16 BE")   == 0);
    ASSERT_TRUE(wcscmp(encoding_name(ENC_UTF8),     L"UTF-8")       == 0);
    ASSERT_TRUE(wcscmp(encoding_name(ENC_SJIS),     L"Shift-JIS")   == 0);
    ASSERT_TRUE(wcscmp(encoding_name(ENC_UNKNOWN),  L"Unknown")     == 0);
    /* CP949/Johab는 한글 라벨 — wchar_t 비교만 확인 */
    ASSERT_TRUE(encoding_name(ENC_CP949) != NULL);
    ASSERT_TRUE(encoding_name(ENC_JOHAB) != NULL);
}

TEST(cp949_extension_ratio_pure_cp949_low) {
    /* "가한" CP949: 0xB0 0xA1 0xC7 0xD1 — 두 페어 모두 표준 영역.
     *   lead 0xB0,0xC7 ∉ 0x81-0xA0,  trail 0xA1,0xD1 ∉ 0x41-0x7A → ext=0% */
    const unsigned char buf[] = { 0xB0, 0xA1, 0xC7, 0xD1 };
    ASSERT_EQ(cp949_extension_ratio(buf, sizeof(buf)), 0);
}

TEST(cp949_extension_ratio_johab_high) {
    /* Johab "가한글힣" — 비트 패킹이 보조 영역에 자주 떨어진다.
     *   0x88,0x61: lead 0x88 ∈ 0x81-0xA0 → ext
     *   0xD0,0x65: trail 0x65 ∈ 0x41-0x7A → ext
     *   0x8B,0x69: lead 0x8B ∈ 0x81-0xA0 → ext
     *   0xD3,0xBD: 둘 다 보조영역 밖 → not ext
     *   ratio = 3/4 = 75% — Johab 식별 휴리스틱(>=30%)을 만족 */
    const unsigned char buf[] = {
        0x88, 0x61, 0xD0, 0x65, 0x8B, 0x69, 0xD3, 0xBD,
    };
    ASSERT_EQ(cp949_extension_ratio(buf, sizeof(buf)), 75);
}

TEST(utf8_validity_2byte_sequence) {
    /* "©" U+00A9 = 0xC2 0xA9 — 2바이트 시퀀스 단독 */
    const unsigned char buf[] = { 0xC2, 0xA9 };
    ASSERT_GE(utf8_validity_score(buf, sizeof(buf)), 1);
}

TEST(utf8_validity_4byte_emoji) {
    /* "🎉" U+1F389 = 0xF0 0x9F 0x8E 0x89 — 4바이트 시퀀스 */
    const unsigned char buf[] = { 0xF0, 0x9F, 0x8E, 0x89 };
    ASSERT_GE(utf8_validity_score(buf, sizeof(buf)), 1);
}

TEST(utf8_validity_rejects_f5_lead) {
    /* 0xF5 이상의 lead byte는 U+10FFFF 초과 → 거부 */
    const unsigned char buf[] = { 0xF5, 0x80, 0x80, 0x80 };
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf)), 0);
}

TEST(utf8_validity_truncated_last_after_hits) {
    /* 청크 경계에서 마지막 시퀀스가 잘려도 이전에 hit이 있었다면
     * 그 hit 카운트만큼 인정 (encoding.h:97~101 특수 처리).
     * '가' 1자(3B) + 잘린 한글 lead 2바이트 → score=1 */
    const unsigned char buf[] = { 0xEA, 0xB0, 0x80, 0xEA, 0xB0 };
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf)), 1);
}

TEST(utf8_validity_truncated_first_returns_zero) {
    /* hit이 한 번도 없는 상태에서 시퀀스가 잘리면 거부. */
    const unsigned char buf[] = { 0xEA, 0xB0 };
    ASSERT_EQ(utf8_validity_score(buf, sizeof(buf)), 0);
}

TEST(convert_unknown_returns_null) {
    /* default 분기 — ENC_UNKNOWN으로 호출하면 NULL.
     *   src_len > 0 이어야 default switch 경로로 진입. */
    const unsigned char buf[] = { 'a' };
    size_t wlen = 0xDEAD;
    wchar_t *out = convert_to_utf16(buf, sizeof(buf), ENC_UNKNOWN, &wlen);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(wlen, 0u);
}

TEST(convert_unknown_with_empty_returns_empty_buffer) {
    /* src_len=0 early-return은 인코딩과 무관하게 빈 버퍼를 돌려준다
     * (encoding.h:262~266) — caller가 항상 free할 수 있도록 NULL 아닌
     * 1-wchar 종료 버퍼 보장. */
    size_t wlen = 0xDEAD;
    wchar_t *out = convert_to_utf16(NULL, 0, ENC_UNKNOWN, &wlen);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(wlen, 0u);
    ASSERT_EQ(out[0], 0);
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
    RUN(johab_hangul_count_empty);
    RUN(johab_hangul_count_all_valid);
    RUN(johab_hangul_count_skips_ascii_and_invalid);
    RUN(johab_to_utf16_output_count_bounded_by_src_len);

    printf("\n[sjis.h]\n");
    RUN(sjis_score_pure_ascii_zero);
    RUN(sjis_score_too_short);
    RUN(sjis_score_half_width_kana_all_hits);
    RUN(sjis_score_two_byte_kanji_all_hits);
    RUN(sjis_score_invalid_lead_zero);
    RUN(sjis_predicates_boundaries);
    RUN(sjis_score_lead_without_trail);

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
    RUN(kana_table_first_and_last);
    RUN(kana_table_gap_returns_null);

#ifdef _WIN32
    printf("\n[encoding.h]\n");
    RUN(detect_utf8_bom);
    RUN(detect_utf16_le_bom);
    RUN(detect_utf16_be_bom);
    RUN(detect_utf8_no_bom_with_multibyte);
    RUN(detect_pure_ascii_defaults_to_cp949);
    RUN(detect_empty_buffer_falls_back_to_cp949);
    RUN(detect_sjis_japanese_text);
    RUN(detect_johab_real_korean_text);
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
    RUN(convert_sjis_hiragana_a);
    RUN(convert_empty_input);
    RUN(roundtrip_utf8_korean);
    RUN(roundtrip_cp949_korean);
    RUN(roundtrip_sjis_kana);
    RUN(roundtrip_utf8_bom_strips_then_restores);
    RUN(roundtrip_johab_to_utf16_one_way);
    RUN(encoding_name_lookup_known);
    RUN(cp949_extension_ratio_pure_cp949_low);
    RUN(cp949_extension_ratio_johab_high);
    RUN(utf8_validity_2byte_sequence);
    RUN(utf8_validity_4byte_emoji);
    RUN(utf8_validity_rejects_f5_lead);
    RUN(utf8_validity_truncated_last_after_hits);
    RUN(utf8_validity_truncated_first_returns_zero);
    RUN(convert_unknown_returns_null);
    RUN(convert_unknown_with_empty_returns_empty_buffer);
#else
    printf("\n[encoding.h] skipped — requires Win32 (build on MinGW or MSVC)\n");
#endif

    TEST_SUMMARY();
}
