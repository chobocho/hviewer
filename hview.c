/*
 * hview.c — Win32 네이티브 한글 텍스트 뷰어
 *
 * 컴파일:
 *   MSVC:  cl /O2 /MT /W3 hview.c user32.lib gdi32.lib comdlg32.lib shell32.lib
 *   MinGW: gcc -O2 -municode -mwindows -Wall hview.c -o hview.exe \
 *               -luser32 -lgdi32 -lcomdlg32 -lshell32
 *
 * 의존성: Win32 API + CRT만. 외부 라이브러리 없음.
 *
 * 설계 메모:
 *   - 64MB 파일 한계: 동기 로딩 (~350ms)이라 백그라운드 스레드 불필요
 *   - 단일 윈도우 앱 → 전역 상태 1개 (g_state)
 *   - 텍스트 저장: UTF-16 평문 + line_offsets 배열로 줄 인덱싱
 *   - 렌더링: WM_PAINT에서 가시 영역 줄만 TextOutW
 *
 * 키 바인딩:
 *   Ctrl+O          파일 열기
 *   ↑/↓             한 줄 스크롤
 *   PageUp/PageDown 한 화면 스크롤
 *   Home/End        문서 처음/끝
 *   Ctrl+Home/End   동일
 *   ←/→             가로 스크롤
 *   Ctrl+,/Ctrl+.   폰트 크기 -/+
 *   F5              인코딩 메뉴
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoding.h"

/* ------------------------------------------------------------------
 * 상수
 * ------------------------------------------------------------------ */
#define MAX_FILE_SIZE       (64 * 1024 * 1024)   /* 64MB */
#define INITIAL_LINE_CAP    1024
#define APP_TITLE           L"hview"

/* 메뉴 ID */
#define IDM_OPEN            1001
#define IDM_EXIT            1002
#define IDM_FONT_INC        1010
#define IDM_FONT_DEC        1011
#define IDM_ENC_AUTO        1020
#define IDM_ENC_UTF8        1021
#define IDM_ENC_UTF16LE     1022
#define IDM_ENC_UTF16BE     1023
#define IDM_ENC_CP949       1024
#define IDM_ENC_JOHAB       1025
#define IDM_ABOUT           1090

/* ------------------------------------------------------------------
 * 전역 상태
 *
 * raw_data: 원본 파일 바이트 (인코딩 재해석용 보관)
 * text:     UTF-16 변환 결과
 * line_offsets[i]: i번째 줄의 text 내 시작 인덱스
 *                  line_offsets[line_count]는 text_len (sentinel)
 * ------------------------------------------------------------------ */
typedef struct {
    /* 파일 데이터 */
    unsigned char *raw_data;
    size_t         raw_len;
    Encoding       encoding;

    wchar_t       *text;
    size_t         text_len;

    int           *line_offsets;
    int            line_count;
    int            line_cap;

    /* 뷰포트 */
    int            top_line;
    int            h_scroll_px;     /* 가로 스크롤 (픽셀) */
    int            max_line_px;     /* 가장 긴 줄의 픽셀 폭 */

    /* 폰트/렌더링 */
    HFONT          font;
    int            font_size;       /* 포인트 */
    int            char_height;     /* 한 줄 픽셀 높이 */
    int            avg_char_width;  /* 영문 평균 폭 (스크롤 단위) */

    /* 윈도우 */
    HWND           hwnd;
    int            client_w;
    int            client_h;
    int            visible_lines;

    wchar_t        filepath[MAX_PATH];
} ViewerState;

static ViewerState g_state;

/* ------------------------------------------------------------------
 * 유틸리티
 * ------------------------------------------------------------------ */
static void show_error(HWND hwnd, const wchar_t *msg) {
    MessageBoxW(hwnd, msg, APP_TITLE, MB_OK | MB_ICONERROR);
}

static void update_title(void) {
    wchar_t title[MAX_PATH + 64];
    if (g_state.filepath[0]) {
        const wchar_t *name = wcsrchr(g_state.filepath, L'\\');
        name = name ? name + 1 : g_state.filepath;
        _snwprintf_s(title, MAX_PATH + 64, _TRUNCATE,
                     L"%s — %s [%s]",
                     name, APP_TITLE, encoding_name(g_state.encoding));
    } else {
        _snwprintf_s(title, MAX_PATH + 64, _TRUNCATE, L"%s", APP_TITLE);
    }
    SetWindowTextW(g_state.hwnd, title);
}

/* ------------------------------------------------------------------
 * 폰트 생성 — Galaxy Fold 7 같은 HiDPI 디스플레이도 자동 대응.
 * 시스템 DPI에 맞춰 픽셀 크기 계산.
 * ------------------------------------------------------------------ */
static void create_font(void) {
    if (g_state.font) DeleteObject(g_state.font);

    HDC hdc = GetDC(g_state.hwnd);
    int dpi_y = GetDeviceCaps(hdc, LOGPIXELSY);
    int height = -MulDiv(g_state.font_size, dpi_y, 72);
    ReleaseDC(g_state.hwnd, hdc);

    /* 한글 모노스페이스: D2Coding > 나눔고딕코딩 > Consolas+맑은고딕 */
    g_state.font = CreateFontW(
        height, 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        HANGUL_CHARSET,                 /* 한글 charset 우선 */
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        L"D2Coding"                     /* 없으면 시스템이 폴백 */
    );

    /* 메트릭 측정 */
    hdc = GetDC(g_state.hwnd);
    HFONT old = (HFONT)SelectObject(hdc, g_state.font);
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    g_state.char_height = tm.tmHeight;
    g_state.avg_char_width = tm.tmAveCharWidth;
    SelectObject(hdc, old);
    ReleaseDC(g_state.hwnd, hdc);
}

/* ------------------------------------------------------------------
 * 줄 인덱스 구축.
 *
 * 텍스트를 한 번 훑어서 각 줄의 시작 오프셋을 line_offsets 배열에 저장.
 * \r\n, \n, \r 모두 줄 끝으로 처리.
 *
 * 64MB 텍스트(최대 ~3천만 줄)도 ~100ms. 동기 처리로 충분.
 *
 * 알고리즘 노트:
 *   - 줄 종결자를 만나면 line_offsets에 다음 문자 위치 추가
 *   - \r\n은 한 번만 카운트 (\r에서 처리하고 \n 스킵)
 *   - 동적 배열 확장: 2배씩 늘리는 표준 amortized O(1) 패턴
 * ------------------------------------------------------------------ */
static int build_line_index(void) {
    free(g_state.line_offsets);
    g_state.line_cap = INITIAL_LINE_CAP;
    g_state.line_offsets = (int*)malloc(g_state.line_cap * sizeof(int));
    if (!g_state.line_offsets) return 0;

    g_state.line_count = 0;
    g_state.line_offsets[g_state.line_count++] = 0;

    const wchar_t *t = g_state.text;
    size_t n = g_state.text_len;

    for (size_t i = 0; i < n; i++) {
        wchar_t c = t[i];
        if (c == L'\r' || c == L'\n') {
            /* \r\n은 한 번만 */
            size_t next = i + 1;
            if (c == L'\r' && next < n && t[next] == L'\n') {
                i = next;  /* \n 스킵, 루프의 i++로 next+1로 이동 */
            }

            /* 다음 줄 시작 등록 */
            if (g_state.line_count >= g_state.line_cap) {
                g_state.line_cap *= 2;
                int *bigger = (int*)realloc(g_state.line_offsets,
                                            g_state.line_cap * sizeof(int));
                if (!bigger) return 0;
                g_state.line_offsets = bigger;
            }
            g_state.line_offsets[g_state.line_count++] = (int)(i + 1);
        }
    }

    /* sentinel: 마지막 줄 끝 */
    if (g_state.line_count >= g_state.line_cap) {
        g_state.line_cap++;
        int *bigger = (int*)realloc(g_state.line_offsets,
                                    g_state.line_cap * sizeof(int));
        if (!bigger) return 0;
        g_state.line_offsets = bigger;
    }
    g_state.line_offsets[g_state.line_count] = (int)n;

    return 1;
}

/* ------------------------------------------------------------------
 * 가장 긴 줄의 픽셀 폭 계산 — 가로 스크롤 범위용.
 *
 * 64MB에 줄이 많으면 비싸므로, 가시 영역에서만 lazy하게 갱신해도 됨.
 * 일단 단순 구현: 전체 훑기. 필요하면 최적화.
 * ------------------------------------------------------------------ */
static void measure_max_line_width(void) {
    HDC hdc = GetDC(g_state.hwnd);
    HFONT old = (HFONT)SelectObject(hdc, g_state.font);

    int max_w = 0;
    SIZE sz;
    /* 성능: 전체 줄 측정은 100MB에서 무거우므로
     * 일단 1만 줄까지만 샘플링. 이후 스크롤 시 갱신. */
    int sample = g_state.line_count < 10000 ? g_state.line_count : 10000;
    for (int i = 0; i < sample; i++) {
        int start = g_state.line_offsets[i];
        int end   = g_state.line_offsets[i + 1];
        int len = end - start;
        /* 줄 종결자 제외 */
        while (len > 0) {
            wchar_t c = g_state.text[start + len - 1];
            if (c == L'\r' || c == L'\n') len--;
            else break;
        }
        if (len > 0) {
            GetTextExtentPoint32W(hdc, &g_state.text[start], len, &sz);
            if (sz.cx > max_w) max_w = sz.cx;
        }
    }

    SelectObject(hdc, old);
    ReleaseDC(g_state.hwnd, hdc);
    g_state.max_line_px = max_w;
}

/* ------------------------------------------------------------------
 * 스크롤바 업데이트
 * ------------------------------------------------------------------ */
static void update_scrollbars(void) {
    /* 세로: 줄 단위 */
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0;
    si.nMax = g_state.line_count > 0 ? g_state.line_count - 1 : 0;
    si.nPage = g_state.visible_lines > 0 ? g_state.visible_lines : 1;
    si.nPos = g_state.top_line;
    SetScrollInfo(g_state.hwnd, SB_VERT, &si, TRUE);

    /* 가로: 평균 글자 폭 단위 */
    int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
    SCROLLINFO sh = { sizeof(sh), SIF_RANGE | SIF_PAGE | SIF_POS };
    sh.nMin = 0;
    sh.nMax = g_state.max_line_px / unit;
    sh.nPage = g_state.client_w / unit;
    sh.nPos = g_state.h_scroll_px / unit;
    SetScrollInfo(g_state.hwnd, SB_HORZ, &sh, TRUE);
}

/* ------------------------------------------------------------------
 * 파일 로드 — 핵심 함수.
 *
 * 흐름:
 *   1. 파일 크기 검증 (64MB 한계)
 *   2. 한 번의 ReadFile로 전체 읽기 (64MB는 동기로 충분)
 *   3. 인코딩 판별 (앞 64KB만)
 *   4. UTF-16 변환
 *   5. 줄 인덱스 구축
 *   6. 가장 긴 줄 폭 측정
 * ------------------------------------------------------------------ */
static int load_file(const wchar_t *path, Encoding force_enc) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        show_error(g_state.hwnd, L"파일을 열 수 없습니다.");
        return 0;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        show_error(g_state.hwnd, L"파일 크기를 알 수 없습니다.");
        return 0;
    }

    if (size.QuadPart > MAX_FILE_SIZE) {
        CloseHandle(h);
        show_error(g_state.hwnd,
                   L"파일이 너무 큽니다 (최대 64MB).");
        return 0;
    }

    size_t fsize = (size_t)size.QuadPart;
    unsigned char *raw = (unsigned char*)malloc(fsize > 0 ? fsize : 1);
    if (!raw) {
        CloseHandle(h);
        show_error(g_state.hwnd, L"메모리 할당 실패.");
        return 0;
    }

    /* 64비트 파일도 한 번에 읽되, ReadFile은 DWORD라 청크 분할 필요 시 처리.
     * 64MB는 단일 호출로 가능 (DWORD 최대 4GB) */
    DWORD read = 0;
    if (fsize > 0 && (!ReadFile(h, raw, (DWORD)fsize, &read, NULL) ||
                      read != fsize)) {
        free(raw);
        CloseHandle(h);
        show_error(g_state.hwnd, L"파일 읽기 실패.");
        return 0;
    }
    CloseHandle(h);

    /* 인코딩 판별 — 앞 64KB만 보면 충분 */
    Encoding enc = force_enc;
    if (enc == ENC_UNKNOWN) {
        size_t sample = fsize < 65536 ? fsize : 65536;
        enc = detect_encoding(raw, sample);
    }

    /* UTF-16 변환 */
    size_t wlen = 0;
    wchar_t *wbuf = convert_to_utf16(raw, fsize, enc, &wlen);
    if (!wbuf) {
        free(raw);
        show_error(g_state.hwnd, L"인코딩 변환 실패.");
        return 0;
    }

    /* 기존 데이터 해제 후 교체 */
    free(g_state.raw_data);
    free(g_state.text);
    g_state.raw_data = raw;
    g_state.raw_len  = fsize;
    g_state.encoding = enc;
    g_state.text     = wbuf;
    g_state.text_len = wlen;
    g_state.top_line = 0;
    g_state.h_scroll_px = 0;
    wcsncpy_s(g_state.filepath, MAX_PATH, path, _TRUNCATE);

    if (!build_line_index()) {
        show_error(g_state.hwnd, L"줄 인덱스 구축 실패.");
        return 0;
    }

    measure_max_line_width();
    update_title();
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
    return 1;
}

/* 인코딩 강제 지정 후 재로드 */
static void reload_with_encoding(Encoding enc) {
    if (!g_state.filepath[0]) return;
    wchar_t path[MAX_PATH];
    wcscpy_s(path, MAX_PATH, g_state.filepath);
    load_file(path, enc);
}

/* ------------------------------------------------------------------
 * 렌더링 — WM_PAINT 처리.
 *
 * 가시 영역의 줄만 TextOutW로 그림. BeginPaint가 클리핑을
 * 자동 적용하므로 추가 최적화 불필요.
 *
 * 깜빡임 방지: 큰 영역 무효화 시에만 더블 버퍼링이 필요한데,
 * 줄 단위 스크롤은 ScrollWindowEx가 자동 처리하므로 필요 없음.
 * ------------------------------------------------------------------ */
static void on_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    /* 배경 — 시스템 윈도우 색 */
    FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

    if (!g_state.text || g_state.line_count == 0) {
        const wchar_t *msg = L"파일을 드래그하거나 Ctrl+O로 여세요.";
        HFONT old = (HFONT)SelectObject(hdc, g_state.font);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, 20, 20, msg, (int)wcslen(msg));
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return;
    }

    HFONT old = (HFONT)SelectObject(hdc, g_state.font);
    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    SetBkMode(hdc, TRANSPARENT);

    /* 가시 영역 줄 범위 계산 */
    int first = ps.rcPaint.top    / g_state.char_height + g_state.top_line;
    int last  = ps.rcPaint.bottom / g_state.char_height + g_state.top_line + 1;
    if (first < 0) first = 0;
    if (last > g_state.line_count) last = g_state.line_count;

    int x0 = -g_state.h_scroll_px;

    for (int i = first; i < last; i++) {
        int y = (i - g_state.top_line) * g_state.char_height;
        int start = g_state.line_offsets[i];
        int end   = g_state.line_offsets[i + 1];
        int len = end - start;

        /* 줄 종결자 제거 */
        while (len > 0) {
            wchar_t c = g_state.text[start + len - 1];
            if (c == L'\r' || c == L'\n') len--;
            else break;
        }

        if (len > 0) {
            /* ExtTextOutW가 TextOutW보다 빠름 (커닝/모양 처리 일부 생략) */
            ExtTextOutW(hdc, x0, y, 0, NULL,
                        &g_state.text[start], len, NULL);
        }
    }

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------
 * 스크롤 처리
 * ------------------------------------------------------------------ */
static void scroll_to_line(int line) {
    if (line < 0) line = 0;
    int max_top = g_state.line_count - g_state.visible_lines;
    if (max_top < 0) max_top = 0;
    if (line > max_top) line = max_top;

    int delta = line - g_state.top_line;
    if (delta == 0) return;

    g_state.top_line = line;
    /* ScrollWindowEx로 부드럽게 — 새 영역만 다시 그림 */
    ScrollWindowEx(g_state.hwnd, 0, -delta * g_state.char_height,
                   NULL, NULL, NULL, NULL,
                   SW_INVALIDATE | SW_ERASE);
    update_scrollbars();
}

static void scroll_h_to(int px) {
    if (px < 0) px = 0;
    int max_h = g_state.max_line_px - g_state.client_w;
    if (max_h < 0) max_h = 0;
    if (px > max_h) px = max_h;

    int delta = px - g_state.h_scroll_px;
    if (delta == 0) return;

    g_state.h_scroll_px = px;
    ScrollWindowEx(g_state.hwnd, -delta, 0,
                   NULL, NULL, NULL, NULL,
                   SW_INVALIDATE | SW_ERASE);
    update_scrollbars();
}

/* ------------------------------------------------------------------
 * 파일 열기 다이얼로그
 * ------------------------------------------------------------------ */
static void cmd_open_file(void) {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner   = g_state.hwnd;
    ofn.lpstrFilter = L"텍스트 파일 (*.txt)\0*.txt\0모든 파일 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        load_file(path, ENC_UNKNOWN);
    }
}

/* ------------------------------------------------------------------
 * 메뉴 생성
 * ------------------------------------------------------------------ */
static HMENU create_menu(void) {
    HMENU menu = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN, L"열기(&O)\tCtrl+O");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"종료(&X)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file_menu, L"파일(&F)");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_INC, L"글자 크게\tCtrl+.");
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_DEC, L"글자 작게\tCtrl+,");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view_menu, L"보기(&V)");

    HMENU enc_menu = CreatePopupMenu();
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_AUTO,    L"자동 판별");
    AppendMenuW(enc_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF8,    L"UTF-8");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16LE, L"UTF-16 LE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16BE, L"UTF-16 BE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_CP949,   L"CP949 (EUC-KR)");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_JOHAB,   L"조합형 (Johab)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)enc_menu, L"인코딩(&E)");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"정보(&A)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help_menu, L"도움말(&H)");

    return menu;
}

/* ------------------------------------------------------------------
 * 윈도우 프로시저
 * ------------------------------------------------------------------ */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_state.hwnd = hwnd;
        g_state.font_size = 11;
        create_font();
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE: {
        g_state.client_w = LOWORD(lp);
        g_state.client_h = HIWORD(lp);
        if (g_state.char_height > 0) {
            g_state.visible_lines = g_state.client_h / g_state.char_height;
        }
        update_scrollbars();
        return 0;
    }

    case WM_PAINT:
        on_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        /* WM_PAINT에서 처리하므로 깜빡임 방지용 1 반환 */
        return 1;

    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int new_pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:        new_pos -= 1; break;
        case SB_LINEDOWN:      new_pos += 1; break;
        case SB_PAGEUP:        new_pos -= g_state.visible_lines; break;
        case SB_PAGEDOWN:      new_pos += g_state.visible_lines; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: new_pos = HIWORD(wp); break;
        case SB_TOP:           new_pos = 0; break;
        case SB_BOTTOM:        new_pos = g_state.line_count; break;
        }
        scroll_to_line(new_pos);
        return 0;
    }

    case WM_HSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_HORZ, &si);
        int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
        int new_pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT:      new_pos -= 1; break;
        case SB_LINERIGHT:     new_pos += 1; break;
        case SB_PAGELEFT:      new_pos -= si.nPage; break;
        case SB_PAGERIGHT:     new_pos += si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: new_pos = HIWORD(wp); break;
        }
        scroll_h_to(new_pos * unit);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        UINT lines_per_notch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0,
                              &lines_per_notch, 0);
        int lines = -(delta / WHEEL_DELTA) * (int)lines_per_notch;
        scroll_to_line(g_state.top_line + lines);
        return 0;
    }

    case WM_KEYDOWN: {
        int ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        switch (wp) {
        case VK_UP:       scroll_to_line(g_state.top_line - 1); break;
        case VK_DOWN:     scroll_to_line(g_state.top_line + 1); break;
        case VK_PRIOR:    scroll_to_line(g_state.top_line -
                                         g_state.visible_lines); break;
        case VK_NEXT:     scroll_to_line(g_state.top_line +
                                         g_state.visible_lines); break;
        case VK_HOME:     scroll_to_line(0); break;
        case VK_END:      scroll_to_line(g_state.line_count); break;
        case VK_LEFT:     scroll_h_to(g_state.h_scroll_px -
                                      g_state.avg_char_width * 4); break;
        case VK_RIGHT:    scroll_h_to(g_state.h_scroll_px +
                                      g_state.avg_char_width * 4); break;
        case 'O':
            if (ctrl) cmd_open_file();
            break;
        case VK_OEM_PERIOD:  /* '.' 키 */
            if (ctrl && g_state.font_size < 48) {
                g_state.font_size++;
                create_font();
                if (g_state.char_height > 0)
                    g_state.visible_lines = g_state.client_h /
                                            g_state.char_height;
                measure_max_line_width();
                update_scrollbars();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        case VK_OEM_COMMA:   /* ',' 키 */
            if (ctrl && g_state.font_size > 6) {
                g_state.font_size--;
                create_font();
                if (g_state.char_height > 0)
                    g_state.visible_lines = g_state.client_h /
                                            g_state.char_height;
                measure_max_line_width();
                update_scrollbars();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:        cmd_open_file(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        case IDM_FONT_INC:
            SendMessageW(hwnd, WM_KEYDOWN, VK_OEM_PERIOD, 0);
            /* Ctrl 상태가 아니라 메뉴에서는 직접 처리 */
            if (g_state.font_size < 48) {
                g_state.font_size++;
                create_font();
                measure_max_line_width();
                update_scrollbars();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        case IDM_FONT_DEC:
            if (g_state.font_size > 6) {
                g_state.font_size--;
                create_font();
                measure_max_line_width();
                update_scrollbars();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        case IDM_ENC_AUTO:    reload_with_encoding(ENC_UNKNOWN); break;
        case IDM_ENC_UTF8:    reload_with_encoding(ENC_UTF8); break;
        case IDM_ENC_UTF16LE: reload_with_encoding(ENC_UTF16_LE); break;
        case IDM_ENC_UTF16BE: reload_with_encoding(ENC_UTF16_BE); break;
        case IDM_ENC_CP949:   reload_with_encoding(ENC_CP949); break;
        case IDM_ENC_JOHAB:   reload_with_encoding(ENC_JOHAB); break;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"hview — Win32 한글 텍스트 뷰어\n"
                L"최대 64MB, 조합형 지원",
                APP_TITLE, MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            load_file(path, ENC_UNKNOWN);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        if (g_state.font) DeleteObject(g_state.font);
        free(g_state.raw_data);
        free(g_state.text);
        free(g_state.line_offsets);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------
 * WinMain
 * ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev,
                    PWSTR cmdline, int nCmdShow) {
    (void)hPrev;

    /* HiDPI — Galaxy Fold 7 같은 고해상도 디스플레이 대응 */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"hview_main";
    RegisterClassExW(&wc);

    HMENU menu = create_menu();
    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"hview_main", APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        NULL, menu, hInst, NULL
    );
    if (!hwnd) return 1;

    /* 명령행 인자로 파일 받기 */
    if (cmdline && cmdline[0]) {
        wchar_t path[MAX_PATH];
        const wchar_t *src = cmdline;
        /* 따옴표 제거 */
        if (*src == L'"') {
            src++;
            int i = 0;
            while (*src && *src != L'"' && i < MAX_PATH - 1)
                path[i++] = *src++;
            path[i] = 0;
        } else {
            wcsncpy_s(path, MAX_PATH, cmdline, _TRUNCATE);
        }
        if (path[0]) load_file(path, ENC_UNKNOWN);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
