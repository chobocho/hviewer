/*
 * encoding.h — 인코딩 자동 판별 및 UTF-16 변환
 *
 * 지원 인코딩:
 *   - UTF-8 (BOM 유무 모두)
 *   - UTF-16 LE/BE (BOM 필수)
 *   - CP949 (EUC-KR 확장, Windows 한글 코드페이지)
 *   - Johab (조합형, 자체 구현)
 *   - Shift-JIS (일본어, CP932)
 *
 * 판별 알고리즘 (우선순위):
 *   1. BOM 검사 (가장 신뢰도 높음)
 *   2. UTF-8 시퀀스 유효성 검사 (false positive 거의 없음)
 *   3. CP949 vs Johab 휴리스틱 점수 비교
 *      - 둘 다 MSB=1인 2바이트 시퀀스라 통계적 구분 필요
 *      - 양쪽 디코드 성공률 비교
 *
 * 다른 언어 비교:
 *   - Python: chardet 라이브러리 (수만 줄짜리 ML 기반)
 *   - Go: golang.org/x/text/encoding/charmap + 자체 휴리스틱
 *   - Win32 C: 이런 거 없음 → 직접 구현
 *
 * 참고: ezView 수준의 정확도는 수년간의 사용자 피드백으로 다듬어진 것.
 * 첫 버전은 80~90% 케이스 정확도를 목표로 하고, 사용자 수동 지정으로 보완.
 */

#ifndef ENCODING_H
#define ENCODING_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "johab.h"
#include "sjis.h"

typedef enum {
    ENC_UNKNOWN = 0,
    ENC_UTF8_BOM,
    ENC_UTF16_LE,
    ENC_UTF16_BE,
    ENC_UTF8,
    ENC_CP949,
    ENC_JOHAB,
    ENC_SJIS
} Encoding;

static const wchar_t *encoding_name(Encoding e) {
    switch (e) {
    case ENC_UTF8_BOM: return L"UTF-8 (BOM)";
    case ENC_UTF16_LE: return L"UTF-16 LE";
    case ENC_UTF16_BE: return L"UTF-16 BE";
    case ENC_UTF8:     return L"UTF-8";
    case ENC_CP949:    return L"CP949 (EUC-KR)";
    case ENC_JOHAB:    return L"Johab (조합형)";
    case ENC_SJIS:     return L"Shift-JIS";
    default:           return L"Unknown";
    }
}

/* ------------------------------------------------------------------
 * UTF-8 유효성 검사.
 *
 * RFC 3629 기준. lead byte 패턴으로 시퀀스 길이 결정 후
 * continuation byte (10xxxxxx) 확인.
 *
 * 추가로 false positive 회피를 위한 점수도 반환:
 *   - ASCII만 있으면 신뢰도 낮음 (어떤 인코딩이든 마찬가지)
 *   - 멀티바이트 시퀀스가 있고 모두 valid면 신뢰도 높음
 *
 * 반환: 0=invalid, 그 외=신뢰도 점수 (멀티바이트 성공 횟수)
 * ------------------------------------------------------------------ */
static int utf8_validity_score(const unsigned char *buf, size_t len) {
    int multibyte_hits = 0;
    size_t i = 0;

    while (i < len) {
        unsigned char b = buf[i];

        if (b < 0x80) {
            /* ASCII */
            i++;
            continue;
        }

        /* 멀티바이트 시퀀스 길이 결정 */
        int extra;
        if      ((b & 0xE0) == 0xC0) extra = 1;  /* 110xxxxx */
        else if ((b & 0xF0) == 0xE0) extra = 2;  /* 1110xxxx */
        else if ((b & 0xF8) == 0xF0) extra = 3;  /* 11110xxx */
        else return 0;  /* invalid lead byte */

        /* overlong 인코딩 검사 */
        if (b == 0xC0 || b == 0xC1) return 0;
        if (b == 0xF5 || b > 0xF5) return 0;  /* > U+10FFFF */

        if (i + extra >= len) {
            /* 잘림 — 파일이 청크 경계에서 잘릴 수 있으니
             * 마지막 시퀀스만 잘렸다면 valid로 인정 */
            if (i + extra >= len && multibyte_hits > 0) break;
            return 0;
        }

        /* continuation byte 검사 */
        for (int k = 1; k <= extra; k++) {
            if ((buf[i + k] & 0xC0) != 0x80) return 0;
        }

        multibyte_hits++;
        i += extra + 1;
    }

    /* 멀티바이트 한 번도 없으면 ASCII — 어떤 인코딩이든 valid이므로
     * 점수 0 반환해서 다른 휴리스틱이 결정하도록 함.
     * 단 호출 측에서 ASCII 케이스는 별도 처리. */
    return multibyte_hits;
}

/* ------------------------------------------------------------------
 * CP949 점수.
 *
 * CP949 lead byte 범위: 0x81~0xFE
 * trail byte 범위:      0x41~0x5A, 0x61~0x7A, 0x81~0xFE
 *
 * 조합형과 구분하려면 분포 차이를 봐야 함:
 *   - CP949는 lead 0x81~0xC8이 한글, 0xCA~0xFD가 한자/특수
 *     일상 한글 텍스트는 lead가 주로 0x81~0xC8 영역
 *   - 조합형은 lead의 비트 패턴이 [1][cho 5비트][중성 상위 2비트]라
 *     특정 lead 값들에 집중됨 (cho 인덱스 0~18에 해당하는 값들)
 *
 * 단순 휴리스틱: 유효한 CP949 lead/trail 짝 비율
 * ------------------------------------------------------------------ */
static int cp949_score(const unsigned char *buf, size_t len) {
    size_t attempts = 0, hits = 0;
    size_t i = 0;

    while (i + 1 < len) {
        unsigned char b0 = buf[i];

        if (b0 < 0x80) { i++; continue; }

        attempts++;
        unsigned char b1 = buf[i + 1];
        int lead_ok  = (b0 >= 0x81 && b0 <= 0xFE);
        int trail_ok = (b1 >= 0x41 && b1 <= 0x5A) ||
                       (b1 >= 0x61 && b1 <= 0x7A) ||
                       (b1 >= 0x81 && b1 <= 0xFE);

        if (lead_ok && trail_ok) hits++;
        i += 2;
    }

    if (attempts == 0) return 0;
    return (int)((hits * 100) / attempts);
}

/* ------------------------------------------------------------------
 * CP949 "확장 영역" 비율 — Johab 식별 보조.
 *
 * 표준 KS X 1001(EUC-KR) 한글은 lead 0xA1~0xC6 + trail 0xA1~0xFE 영역에
 * 모여 있다. 그 외 lead 0x81~0xA0 또는 trail 0x41~0x7A는 CP949가
 * UHC로 확장하면서 추가한 "사용 빈도가 낮은 보조 영역"이다.
 *
 * 일반 한글 텍스트(소설, 기사, 문서)를 CP949로 저장하면 보조 영역
 * 비율이 거의 0%에 가깝다. 반면 같은 텍스트가 Johab으로 저장되면
 * 비트 압축 구조 때문에 trail 바이트가 0x41~0x7A에 자주 떨어진다.
 *
 * 따라서 보조 영역 비율이 높으면(>=30%) Johab일 확률이 압도적이다.
 * detect_encoding의 동률(cp_score == jh_score) 처리에 사용.
 *
 * 반환: 0~100 (모든 2바이트 시퀀스 중 보조 영역 위치한 쌍의 비율)
 * ------------------------------------------------------------------ */
static int cp949_extension_ratio(const unsigned char *buf, size_t len) {
    size_t pairs = 0, ext = 0;
    size_t i = 0;
    while (i + 1 < len) {
        unsigned char b0 = buf[i];
        if (b0 < 0x80) { i++; continue; }
        unsigned char b1 = buf[i + 1];
        pairs++;
        if (b0 >= 0x81 && b0 <= 0xA0) ext++;
        else if (b1 >= 0x41 && b1 <= 0x7A) ext++;
        i += 2;
    }
    if (pairs == 0) return 0;
    return (int)((ext * 100) / pairs);
}

/* ------------------------------------------------------------------
 * 인코딩 자동 판별.
 *
 * 입력: 파일 앞부분 (최소 4바이트, 권장 64KB)
 * 반환: Encoding enum
 *
 * 호출 예: detect_encoding(buf, min(file_size, 65536))
 * ------------------------------------------------------------------ */
static Encoding detect_encoding(const unsigned char *buf, size_t len) {
    /* 1. BOM 검사 */
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        return ENC_UTF8_BOM;
    if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xFE)
        return ENC_UTF16_LE;
    if (len >= 2 && buf[0] == 0xFE && buf[1] == 0xFF)
        return ENC_UTF16_BE;

    /* 2. UTF-8 휴리스틱 — 멀티바이트 시퀀스가 valid하면 높은 신뢰도 */
    int u8 = utf8_validity_score(buf, len);
    if (u8 >= 3) return ENC_UTF8;  /* 멀티바이트 3개 이상 모두 valid */

    /* 3. Shift-JIS: 반각 가나(0xA1-0xDF) 비율이 높으면 우선 판별 */
    int sj = sjis_score(buf, len);

    /* 4. CP949 vs Johab — 점수 높은 쪽 */
    int cp = cp949_score(buf, len);
    int jh = johab_score(buf, len);

    /* SJIS는 한글 인코딩과 바이트 패턴이 겹치므로
     * 반각 가나(0xA1-0xDF)나 SJIS 전용 lead(0x81-0x9F, 0xE0-0xFC)가
     * 충분히 많을 때만 SJIS로 판단 */
    if (sj > cp + 20 && sj > jh + 20 && sj >= 60) return ENC_SJIS;

    /* 둘 다 낮으면 ASCII거나 식별 불가 — CP949 기본값
     * (Windows 한글 환경 표준) */
    if (cp < 50 && jh < 50) return ENC_CP949;

    /* 동률 또는 비슷한 점수일 때:
     * Johab은 비트 패킹 구조상 trail 0x41~0x7A에 한글 자주 떨어진다.
     * 같은 한글 텍스트를 CP949로 저장하면 보조 영역(lead 0x81~0xA0 또는
     * trail 0x41~0x7A) 비율이 보통 0%에 가깝다.
     * 따라서 johab도 valid하면서 보조 영역 비율이 높으면 Johab으로 판정. */
    if (jh >= 80 && cp949_extension_ratio(buf, len) >= 30)
        return ENC_JOHAB;

    /* 조합형은 더 엄격한 패턴이라 비슷한 점수면 CP949 우선 */
    if (jh > cp + 10) return ENC_JOHAB;
    return ENC_CP949;
}

/* ------------------------------------------------------------------
 * 인코딩별 → UTF-16 변환.
 *
 * 반환: 호출자가 free()할 wchar_t 버퍼와 길이.
 *       실패 시 NULL.
 *
 * 호출자는 결과를 free()해야 함.
 * ------------------------------------------------------------------ */
static wchar_t *convert_to_utf16(const unsigned char *src, size_t src_len,
                                  Encoding enc, size_t *out_len) {
    *out_len = 0;
    if (src_len == 0) {
        wchar_t *empty = (wchar_t*)malloc(sizeof(wchar_t));
        if (empty) empty[0] = 0;
        return empty;
    }

    /* BOM 스킵 + 변환 위임 */
    const unsigned char *p = src;
    size_t n = src_len;

    switch (enc) {
    case ENC_UTF8_BOM:
        if (n >= 3) { p += 3; n -= 3; }
        /* fall through */
    case ENC_UTF8: {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)p,
                                        (int)n, NULL, 0);
        if (wlen <= 0) return NULL;
        wchar_t *buf = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, buf, wlen);
        buf[wlen] = 0;
        *out_len = (size_t)wlen;
        return buf;
    }

    case ENC_UTF16_LE: {
        if (n >= 2) { p += 2; n -= 2; }
        size_t wlen = n / 2;
        wchar_t *buf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        memcpy(buf, p, wlen * 2);
        buf[wlen] = 0;
        *out_len = wlen;
        return buf;
    }

    case ENC_UTF16_BE: {
        if (n >= 2) { p += 2; n -= 2; }
        size_t wlen = n / 2;
        wchar_t *buf = (wchar_t*)malloc((wlen + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        /* byte swap */
        for (size_t i = 0; i < wlen; i++) {
            buf[i] = (wchar_t)(((uint16_t)p[i*2] << 8) | p[i*2 + 1]);
        }
        buf[wlen] = 0;
        *out_len = wlen;
        return buf;
    }

    case ENC_CP949: {
        int wlen = MultiByteToWideChar(949, 0, (const char*)p,
                                        (int)n, NULL, 0);
        if (wlen <= 0) return NULL;
        wchar_t *buf = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        MultiByteToWideChar(949, 0, (const char*)p, (int)n, buf, wlen);
        buf[wlen] = 0;
        *out_len = (size_t)wlen;
        return buf;
    }

    case ENC_JOHAB: {
        wchar_t *buf = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        size_t wlen = johab_to_utf16(p, n, buf);
        buf[wlen] = 0;
        *out_len = wlen;
        return buf;
    }

    case ENC_SJIS: {
        int wlen = MultiByteToWideChar(932, 0, (const char*)p, (int)n, NULL, 0);
        if (wlen <= 0) return NULL;
        wchar_t *buf = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
        if (!buf) return NULL;
        MultiByteToWideChar(932, 0, (const char*)p, (int)n, buf, wlen);
        buf[wlen] = 0;
        *out_len = (size_t)wlen;
        return buf;
    }

    default:
        return NULL;
    }
}

#endif /* ENCODING_H */
