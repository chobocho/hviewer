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
 *   Ctrl+F          찾기
 *   F3 / Shift+F3   다음/이전 찾기
 *   Ctrl+G          줄 이동
 *   Ctrl+L          줄 번호 표시 토글
 *   Ctrl+D          다크 모드 토글
 *   Ctrl+B          책갈피 추가/제거 (현재 위치)
 *   F2 / Shift+F2   다음/이전 책갈피
 *   F11             전체화면 토글 (Esc로도 빠져나옴)
 *   Space           자동 스크롤 토글 (진행 중 휠로 속도 조절)
 *   ↑/↓             한 줄 스크롤
 *   PageUp/PageDown 한 화면 스크롤
 *   Home/End        문서 처음/끝
 *   Ctrl+Home/End   동일
 *   ←/→             가로 스크롤
 *   Ctrl+,/Ctrl+.   폰트 크기 -/+
 *   Ctrl+휠         폰트 크기 -/+
 *   Shift+휠        줄 간격 조절
 *   Ctrl+Shift+휠   자간 조절
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

/* 자동 스크롤 타이머 */
#define AUTOSCROLL_TIMER_ID 1
#define AUTOSCROLL_DEFAULT_MS 250
#define AUTOSCROLL_MIN_MS     30
#define AUTOSCROLL_MAX_MS     2000

#define MAX_BOOKMARKS       32

/* 메뉴 ID */
#define IDM_OPEN            1001
#define IDM_EXIT            1002
#define IDM_GOTO            1003
#define IDM_FONT_INC        1010
#define IDM_FONT_DEC        1011
#define IDM_LINENO          1012
#define IDM_FULLSCREEN      1013
#define IDM_FIND            1014
#define IDM_FIND_NEXT       1015
#define IDM_FIND_PREV       1016
#define IDM_DARK_MODE       1017
#define IDM_BM_TOGGLE       1040
#define IDM_BM_NEXT         1041
#define IDM_BM_PREV         1042
#define IDM_BM_CLEAR        1043
#define IDM_ENC_AUTO        1020
#define IDM_RECENT_BASE     1100        /* 1100..1109 */
#define RECENT_MAX          10
#define RECENT_REG_PATH     L"Software\\hview\\Recent"
#define IDM_ENC_UTF8        1021
#define IDM_ENC_UTF16LE     1022
#define IDM_ENC_UTF16BE     1023
#define IDM_ENC_CP949       1024
#define IDM_ENC_JOHAB       1025
#define IDM_ENC_SJIS        1026
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
    int            font_size;          /* 포인트 */
    int            char_height;        /* 한 줄 픽셀 높이 */
    int            avg_char_width;     /* 영문 평균 폭 (스크롤 단위) */
    int            line_spacing_extra; /* 줄 간격 추가 픽셀 (Shift+휠) */
    int            char_spacing_extra; /* 자간 추가 픽셀 (Ctrl+Shift+휠) */

    /* 윈도우 */
    HWND           hwnd;
    int            client_w;
    int            client_h;
    int            visible_lines;

    /* 줄 번호 거터 */
    BOOL           show_line_numbers;

    /* 검색 (Ctrl+F / F3 / Shift+F3) */
    wchar_t        search_needle[256];
    int            search_needle_len;
    int            search_match_pos;    /* text 내 매칭 시작 오프셋, -1=없음 */

    /* 자동 스크롤 (Space) */
    BOOL           autoscroll_active;
    int            autoscroll_delay_ms;

    /* 다크 모드 */
    BOOL           dark_mode;

    /* 책갈피 (Ctrl+B / F2 / Shift+F2) — 줄 번호 정렬 보관 */
    int            bookmarks[MAX_BOOKMARKS];
    int            bookmark_count;

    /* 최근 파일 (HKCU\Software\hview\Recent) */
    wchar_t        recent_paths[RECENT_MAX][MAX_PATH];
    int            recent_count;
    HMENU          recent_menu;

    /* 전체화면 (F11) — 토글 시 원상복구용 */
    BOOL           fs_active;
    DWORD          fs_style;
    DWORD          fs_ex_style;
    HMENU          fs_menu;
    RECT           fs_rect;
    BOOL           fs_was_maximized;

    wchar_t        filepath[MAX_PATH];
} ViewerState;

static ViewerState g_state;

/* ------------------------------------------------------------------
 * 색 테마 — 다크 모드 토글로 전환.
 * 시스템 색(GetSysColor)에 의존하지 않고 고정 팔레트 사용 — 윈도우
 * 테마와 무관하게 일관된 모양을 보장 (특히 한글 글꼴 가독성).
 * ------------------------------------------------------------------ */
typedef struct {
    COLORREF bg;          /* 본문 배경 */
    COLORREF fg;          /* 본문 글자 */
    COLORREF gutter_bg;
    COLORREF gutter_fg;
    COLORREF hl_bg;       /* 검색 매칭 배경 */
    COLORREF hl_fg;
    COLORREF dim_fg;      /* 빈 화면 안내 메시지 */
    COLORREF bm_fg;       /* 책갈피 표시 (거터 줄 번호 색) */
} Theme;

static const Theme THEME_LIGHT = {
    RGB(255, 255, 255), RGB(0, 0, 0),
    RGB(240, 240, 240), RGB(128, 128, 128),
    RGB(255, 230, 80),  RGB(0, 0, 0),
    RGB(128, 128, 128),
    RGB(220, 100,   0)
};

static const Theme THEME_DARK = {
    RGB( 30,  30,  30), RGB(220, 220, 220),
    RGB( 45,  45,  45), RGB(140, 140, 140),
    RGB(180, 130,   0), RGB(  0,   0,   0),
    RGB(140, 140, 140),
    RGB(255, 180, 100)
};

static const Theme *theme(void) {
    return g_state.dark_mode ? &THEME_DARK : &THEME_LIGHT;
}

/* 전방 선언 — 정의 순서가 어긋나는 경우만 */
static void recent_add(const wchar_t *path);

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
 * 입력 프롬프트 — 줄 이동(Ctrl+G), 검색(Ctrl+F)에서 공용 사용.
 *
 * .rc 자원 없이 코드만으로 모달 다이얼로그 구성 (외부 의존성 0 정책).
 * 부모 윈도우 비활성화 + 자체 메시지 펌프로 모달 효과 구현.
 * ------------------------------------------------------------------ */
typedef struct {
    const wchar_t *prompt;
    BOOL           numeric_only;
    wchar_t       *out_buf;
    int            out_buf_len;
    int            done;            /* 0=대기, 1=확인, 2=취소 */
    HWND           edit;
} PromptCtx;

static PromptCtx *g_prompt;

static BOOL CALLBACK prompt_set_font(HWND hwnd, LPARAM font) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static LRESULT CALLBACK prompt_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        DWORD edit_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                           WS_BORDER | ES_AUTOHSCROLL;
        if (g_prompt->numeric_only) edit_style |= ES_NUMBER;

        CreateWindowW(L"STATIC", g_prompt->prompt,
                      WS_CHILD | WS_VISIBLE,
                      12, 12, 280, 18, hwnd, NULL, hi, NULL);
        g_prompt->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", g_prompt->out_buf,
            edit_style,
            12, 34, 280, 24, hwnd, (HMENU)(UINT_PTR)100, hi, NULL);
        CreateWindowW(L"BUTTON", L"확인",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      138, 70, 76, 26, hwnd, (HMENU)(UINT_PTR)IDOK,
                      hi, NULL);
        CreateWindowW(L"BUTTON", L"취소",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      218, 70, 76, 26, hwnd, (HMENU)(UINT_PTR)IDCANCEL,
                      hi, NULL);
        EnumChildWindows(hwnd, prompt_set_font,
                         (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(g_prompt->edit, EM_SETSEL, 0, -1);
        SetFocus(g_prompt->edit);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            GetWindowTextW(g_prompt->edit, g_prompt->out_buf,
                           g_prompt->out_buf_len);
            g_prompt->done = 1;
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            g_prompt->done = 2;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_prompt->done = 2;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int prompt_input(HWND parent, const wchar_t *title,
                        const wchar_t *prompt, BOOL numeric_only,
                        wchar_t *buf, int buflen) {
    static int registered = 0;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = prompt_wnd_proc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"hview_prompt";
        RegisterClassExW(&wc);
        registered = 1;
    }

    PromptCtx ctx;
    ctx.prompt       = prompt;
    ctx.numeric_only = numeric_only;
    ctx.out_buf      = buf;
    ctx.out_buf_len  = buflen;
    ctx.done         = 0;
    ctx.edit         = NULL;
    g_prompt = &ctx;

    /* 부모 중앙 배치 */
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = 320, h = 140;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top  + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"hview_prompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, parent, NULL, hi, NULL);
    if (!dlg) { g_prompt = NULL; return 0; }

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (ctx.done == 0 && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    g_prompt = NULL;
    return ctx.done == 1;
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
    int cextra = g_state.char_spacing_extra;
    /* 성능: 전체 줄 측정은 100MB에서 무거우므로
     * 일단 1만 줄까지만 샘플링. 이후 스크롤 시 갱신.
     * GetTextExtentPoint32W는 자간을 반영하지 않으므로 len * cextra 보정. */
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
            int total = sz.cx + len * cextra;
            if (total > max_w) max_w = total;
        }
    }

    SelectObject(hdc, old);
    ReleaseDC(g_state.hwnd, hdc);
    g_state.max_line_px = max_w;
}

/* ------------------------------------------------------------------
 * 줄 번호 거터 폭 — show_line_numbers가 켜져 있으면 양수, 아니면 0.
 *
 * 자릿수 기반으로 동적 계산. 최소 4자리(1,000줄까지) 보장하여
 * 짧은 파일에서도 거터 폭이 들쑥날쑥하지 않게 함.
 * ------------------------------------------------------------------ */
static int gutter_pixel_width(void) {
    if (!g_state.show_line_numbers || g_state.line_count == 0) return 0;
    int digits = 1, n = g_state.line_count;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 4) digits = 4;
    int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
    return (digits + 2) * unit;
}

/* 텍스트 영역 가용 폭 (거터 제외) */
static int text_area_width(void) {
    int w = g_state.client_w - gutter_pixel_width();
    return w > 0 ? w : 0;
}

/* 줄 사이 간격 포함 한 줄 총 높이 */
static int line_total_height(void) {
    int h = g_state.char_height + g_state.line_spacing_extra;
    return h > 0 ? h : 1;
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

    /* 가로: 평균 글자 폭 단위, 거터 제외 */
    int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
    int avail = text_area_width();
    SCROLLINFO sh = { sizeof(sh), SIF_RANGE | SIF_PAGE | SIF_POS };
    sh.nMin = 0;
    sh.nMax = g_state.max_line_px / unit;
    sh.nPage = avail / unit;
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
    g_state.search_match_pos = -1;       /* 새 파일 → 이전 매칭 무효 */
    g_state.bookmark_count = 0;          /* 줄 번호 의미가 달라지므로 초기화 */
    autoscroll_stop();                   /* 자동 스크롤 중지 */
    wcsncpy_s(g_state.filepath, MAX_PATH, path, _TRUNCATE);

    if (!build_line_index()) {
        show_error(g_state.hwnd, L"줄 인덱스 구축 실패.");
        return 0;
    }

    measure_max_line_width();
    update_title();
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);

    recent_add(path);
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
    const Theme *th = theme();

    /* 배경 — 테마 색 (시스템 색 무시) */
    HBRUSH bg_brush = CreateSolidBrush(th->bg);
    FillRect(hdc, &ps.rcPaint, bg_brush);
    DeleteObject(bg_brush);

    if (!g_state.text || g_state.line_count == 0) {
        const wchar_t *msg = L"파일을 드래그하거나 Ctrl+O로 여세요.";
        HFONT old = (HFONT)SelectObject(hdc, g_state.font);
        SetTextColor(hdc, th->dim_fg);
        SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, 20, 20, msg, (int)wcslen(msg));
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return;
    }

    HFONT old = (HFONT)SelectObject(hdc, g_state.font);
    SetBkMode(hdc, TRANSPARENT);

    int gutter = gutter_pixel_width();
    int lh     = line_total_height();
    int cextra = g_state.char_spacing_extra;

    /* 가시 영역 줄 범위 계산 */
    int first = ps.rcPaint.top    / lh + g_state.top_line;
    int last  = ps.rcPaint.bottom / lh + g_state.top_line + 1;
    if (first < 0) first = 0;
    if (last > g_state.line_count) last = g_state.line_count;

    /* 본문 — 거터 만큼 클리핑하여 가로 스크롤한 텍스트가
     * 거터 영역으로 새지 않게 함. SaveDC/RestoreDC로 클립 영역 복원. */
    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, gutter, 0,
                      g_state.client_w, g_state.client_h);
    SetTextColor(hdc, th->fg);
    SetTextCharacterExtra(hdc, cextra);
    int x0 = gutter - g_state.h_scroll_px;

    for (int i = first; i < last; i++) {
        int y = (i - g_state.top_line) * lh;
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
            ExtTextOutW(hdc, x0, y, 0, NULL,
                        &g_state.text[start], len, NULL);
        }
    }

    /* 검색 매칭 하이라이트 — 같은 클립 영역 안에서 본문 위에 덮어 그림.
     * 매칭이 줄바꿈을 가로지르면 그리지 않음(드문 경우).
     * GetTextExtentPoint32W는 SetTextCharacterExtra를 반영하지 않으므로
     * len * cextra를 수동으로 더해 정확한 픽셀 위치를 구함. */
    if (g_state.search_match_pos >= 0 && g_state.search_needle_len > 0) {
        int mp = g_state.search_match_pos;
        int mline = line_for_offset(mp);
        if (mline >= first && mline < last) {
            int ls = g_state.line_offsets[mline];
            int le = g_state.line_offsets[mline + 1];
            int mlen = g_state.search_needle_len;
            if (mp >= ls && mp + mlen <= le) {
                SIZE pre, mw;
                GetTextExtentPoint32W(hdc, &g_state.text[ls], mp - ls, &pre);
                GetTextExtentPoint32W(hdc, &g_state.text[mp], mlen, &mw);
                int pre_cx = pre.cx + (mp - ls) * cextra;
                int mw_cx  = mw.cx  + mlen * cextra;
                int my = (mline - g_state.top_line) * lh;
                int mx = x0 + pre_cx;
                RECT rh;
                rh.left   = mx;
                rh.top    = my;
                rh.right  = mx + mw_cx;
                rh.bottom = my + lh;
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, th->hl_bg);
                SetTextColor(hdc, th->hl_fg);
                ExtTextOutW(hdc, mx, my, ETO_OPAQUE, &rh,
                            &g_state.text[mp], mlen, NULL);
                SetBkMode(hdc, TRANSPARENT);
            }
        }
    }

    RestoreDC(hdc, saved);

    /* 거터 — 본문 위에 덮어 그림. 줄 번호는 자간 영향 받지 않음. */
    if (gutter > 0) {
        RECT gr;
        gr.left   = 0;
        gr.top    = ps.rcPaint.top;
        gr.right  = gutter;
        gr.bottom = ps.rcPaint.bottom;
        HBRUSH gb = CreateSolidBrush(th->gutter_bg);
        FillRect(hdc, &gr, gb);
        DeleteObject(gb);

        SetTextCharacterExtra(hdc, 0);
        int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
        for (int i = first; i < last; i++) {
            wchar_t numbuf[16];
            int len = _snwprintf_s(numbuf, 16, _TRUNCATE, L"%d", i + 1);
            SIZE sz;
            GetTextExtentPoint32W(hdc, numbuf, len, &sz);
            int y = (i - g_state.top_line) * lh;
            int x = gutter - sz.cx - unit;
            if (x < 0) x = 0;
            SetTextColor(hdc, bookmark_has(i) ? th->bm_fg : th->gutter_fg);
            ExtTextOutW(hdc, x, y, 0, NULL, numbuf, len, NULL);
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
    ScrollWindowEx(g_state.hwnd, 0, -delta * line_total_height(),
                   NULL, NULL, NULL, NULL,
                   SW_INVALIDATE | SW_ERASE);
    update_scrollbars();
}

static void scroll_h_to(int px) {
    if (px < 0) px = 0;
    int max_h = g_state.max_line_px - text_area_width();
    if (max_h < 0) max_h = 0;
    if (px > max_h) px = max_h;

    int delta = px - g_state.h_scroll_px;
    if (delta == 0) return;

    g_state.h_scroll_px = px;
    /* 거터는 가로 스크롤 시 고정. 텍스트 영역만 ScrollWindowEx. */
    int gutter = gutter_pixel_width();
    RECT rc;
    rc.left   = gutter;
    rc.top    = 0;
    rc.right  = g_state.client_w;
    rc.bottom = g_state.client_h;
    ScrollWindowEx(g_state.hwnd, -delta, 0,
                   &rc, &rc, NULL, NULL,
                   SW_INVALIDATE | SW_ERASE);
    update_scrollbars();
}

/* ------------------------------------------------------------------
 * 자동 스크롤 — Space로 토글, 진행 중 휠로 속도 조절.
 * ------------------------------------------------------------------ */
static void autoscroll_start(void) {
    if (g_state.autoscroll_active) return;
    if (g_state.autoscroll_delay_ms <= 0)
        g_state.autoscroll_delay_ms = AUTOSCROLL_DEFAULT_MS;
    SetTimer(g_state.hwnd, AUTOSCROLL_TIMER_ID,
             (UINT)g_state.autoscroll_delay_ms, NULL);
    g_state.autoscroll_active = TRUE;
}

static void autoscroll_stop(void) {
    if (!g_state.autoscroll_active) return;
    KillTimer(g_state.hwnd, AUTOSCROLL_TIMER_ID);
    g_state.autoscroll_active = FALSE;
}

static void autoscroll_toggle(void) {
    if (g_state.autoscroll_active) autoscroll_stop();
    else autoscroll_start();
}

/* delta_ms > 0 = 빠르게 (지연 줄임) */
static void autoscroll_change_speed(int delta_ms) {
    int v = g_state.autoscroll_delay_ms - delta_ms;
    if (v < AUTOSCROLL_MIN_MS) v = AUTOSCROLL_MIN_MS;
    if (v > AUTOSCROLL_MAX_MS) v = AUTOSCROLL_MAX_MS;
    if (v == g_state.autoscroll_delay_ms) return;
    g_state.autoscroll_delay_ms = v;
    if (g_state.autoscroll_active) {
        KillTimer(g_state.hwnd, AUTOSCROLL_TIMER_ID);
        SetTimer(g_state.hwnd, AUTOSCROLL_TIMER_ID, (UINT)v, NULL);
    }
}

/* ------------------------------------------------------------------
 * 폰트 크기 / 줄 간격 / 자간 변경 — 휠+모디파이어 또는 메뉴/단축키 공용.
 * ------------------------------------------------------------------ */
static void font_change(int delta) {
    int new_size = g_state.font_size + delta;
    if (new_size < 6) new_size = 6;
    if (new_size > 48) new_size = 48;
    if (new_size == g_state.font_size) return;
    g_state.font_size = new_size;
    create_font();
    g_state.visible_lines = g_state.client_h / line_total_height();
    measure_max_line_width();
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

static void line_spacing_change(int delta) {
    int v = g_state.line_spacing_extra + delta;
    if (v < 0)  v = 0;
    if (v > 32) v = 32;
    if (v == g_state.line_spacing_extra) return;
    g_state.line_spacing_extra = v;
    g_state.visible_lines = g_state.client_h / line_total_height();
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

static void char_spacing_change(int delta) {
    int v = g_state.char_spacing_extra + delta;
    if (v < 0)  v = 0;
    if (v > 16) v = 16;
    if (v == g_state.char_spacing_extra) return;
    g_state.char_spacing_extra = v;
    measure_max_line_width();
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 줄 이진탐색 — text 오프셋 → 줄 번호.
 *
 * line_offsets는 단조증가이므로 binary search.
 * ------------------------------------------------------------------ */
static int line_for_offset(int offset) {
    if (g_state.line_count == 0) return 0;
    int lo = 0, hi = g_state.line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_state.line_offsets[mid] <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ------------------------------------------------------------------
 * 검색 (Ctrl+F / F3 / Shift+F3)
 *
 * 단순 substring 검색. 케이스-민감(case-sensitive) 고정.
 * - 텍스트 끝에 도달하면 처음으로 wrap-around
 * - BMP 외 surrogate pair 정확도 부족 (한글 영역엔 영향 없음)
 *
 * 향후 확장: ezView 호환 옵션(대소문자 구분 토글, 정규식)은 Phase 후속.
 * ------------------------------------------------------------------ */
static int find_substr_offset(int start, BOOL forward) {
    if (g_state.search_needle_len == 0 || g_state.text_len == 0) return -1;
    int n = (int)g_state.text_len;
    int needlelen = g_state.search_needle_len;
    if (needlelen > n) return -1;

    if (forward) {
        if (start < 0) start = 0;
        for (int i = start; i <= n - needlelen; i++) {
            int k;
            for (k = 0; k < needlelen; k++) {
                if (g_state.text[i + k] != g_state.search_needle[k]) break;
            }
            if (k == needlelen) return i;
        }
    } else {
        if (start > n - needlelen) start = n - needlelen;
        for (int i = start; i >= 0; i--) {
            int k;
            for (k = 0; k < needlelen; k++) {
                if (g_state.text[i + k] != g_state.search_needle[k]) break;
            }
            if (k == needlelen) return i;
        }
    }
    return -1;
}

static void search_jump_to(int offset) {
    g_state.search_match_pos = offset;
    int line = line_for_offset(offset);
    /* 매칭 줄이 화면 밖이거나 가장자리면 중앙으로 가져옴 */
    if (line < g_state.top_line ||
        line >= g_state.top_line + g_state.visible_lines) {
        int target = line - g_state.visible_lines / 2;
        if (target < 0) target = 0;
        scroll_to_line(target);
    }
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

static void cmd_find(void) {
    if (g_state.text_len == 0) return;
    wchar_t buf[256];
    wcscpy_s(buf, 256, g_state.search_needle);  /* 직전 검색어 채우기 */
    if (!prompt_input(g_state.hwnd, L"검색", L"찾을 문자열:",
                      FALSE, buf, 256)) return;
    int len = (int)wcslen(buf);
    if (len == 0) return;
    wcsncpy_s(g_state.search_needle, 256, buf, _TRUNCATE);
    g_state.search_needle_len = len;

    /* 화면 윗줄부터 정방향 검색 → 못 찾으면 처음부터 wrap */
    int start = g_state.line_offsets[g_state.top_line];
    int pos = find_substr_offset(start, TRUE);
    if (pos < 0) pos = find_substr_offset(0, TRUE);
    if (pos < 0) {
        MessageBoxW(g_state.hwnd, L"찾는 문자열이 없습니다.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        g_state.search_match_pos = -1;
        InvalidateRect(g_state.hwnd, NULL, FALSE);
        return;
    }
    search_jump_to(pos);
}

static void cmd_find_again(BOOL forward) {
    if (g_state.search_needle_len == 0) { cmd_find(); return; }
    int start;
    if (forward) {
        start = g_state.search_match_pos < 0 ?
                0 : g_state.search_match_pos + 1;
    } else {
        start = g_state.search_match_pos < 0 ?
                (int)g_state.text_len - 1 :
                g_state.search_match_pos - 1;
    }
    int pos = find_substr_offset(start, forward);
    /* wrap-around */
    if (pos < 0) {
        pos = find_substr_offset(forward ? 0 : (int)g_state.text_len - 1,
                                 forward);
    }
    if (pos < 0) {
        MessageBoxW(g_state.hwnd, L"찾는 문자열이 없습니다.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    search_jump_to(pos);
}

/* ------------------------------------------------------------------
 * 줄 이동 (Ctrl+G)
 *
 * 1-기반 줄 번호로 입력 받음. 음수/0/초과는 클램프.
 * ------------------------------------------------------------------ */
static void cmd_goto_line(void) {
    if (g_state.line_count == 0) return;
    wchar_t buf[16] = {0};
    if (prompt_input(g_state.hwnd, L"줄 이동", L"줄 번호:",
                     TRUE, buf, 16)) {
        int line = _wtoi(buf);
        if (line < 1) line = 1;
        if (line > g_state.line_count) line = g_state.line_count;
        scroll_to_line(line - 1);
    }
}

/* ------------------------------------------------------------------
 * 책갈피 — 줄 번호 정렬 배열로 보관. 세션 한정 (영속화는 후속 phase).
 *
 * 토글 기준은 top_line (가장 위에 보이는 줄). 캐럿이 없으므로
 * "현재 위치 = 시작 위치"로 정의.
 * ------------------------------------------------------------------ */
static int bookmark_index_of(int line) {
    for (int i = 0; i < g_state.bookmark_count; i++) {
        if (g_state.bookmarks[i] == line) return i;
    }
    return -1;
}

static BOOL bookmark_has(int line) {
    return bookmark_index_of(line) >= 0;
}

static void cmd_bookmark_toggle(void) {
    if (g_state.line_count == 0) return;
    int line = g_state.top_line;
    int idx = bookmark_index_of(line);
    if (idx >= 0) {
        for (int j = idx; j < g_state.bookmark_count - 1; j++)
            g_state.bookmarks[j] = g_state.bookmarks[j + 1];
        g_state.bookmark_count--;
    } else if (g_state.bookmark_count < MAX_BOOKMARKS) {
        int pos = g_state.bookmark_count;
        while (pos > 0 && g_state.bookmarks[pos - 1] > line) {
            g_state.bookmarks[pos] = g_state.bookmarks[pos - 1];
            pos--;
        }
        g_state.bookmarks[pos] = line;
        g_state.bookmark_count++;
    }
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

static void cmd_bookmark_jump(BOOL forward) {
    if (g_state.bookmark_count == 0) return;
    int curr = g_state.top_line;
    int target = -1;
    if (forward) {
        for (int i = 0; i < g_state.bookmark_count; i++) {
            if (g_state.bookmarks[i] > curr) {
                target = g_state.bookmarks[i];
                break;
            }
        }
        if (target < 0) target = g_state.bookmarks[0];     /* wrap */
    } else {
        for (int i = g_state.bookmark_count - 1; i >= 0; i--) {
            if (g_state.bookmarks[i] < curr) {
                target = g_state.bookmarks[i];
                break;
            }
        }
        if (target < 0)
            target = g_state.bookmarks[g_state.bookmark_count - 1];
    }
    scroll_to_line(target);
}

static void cmd_bookmark_clear(void) {
    if (g_state.bookmark_count == 0) return;
    g_state.bookmark_count = 0;
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------
 * 다크 모드 토글
 * ------------------------------------------------------------------ */
static void cmd_toggle_dark_mode(void) {
    g_state.dark_mode = !g_state.dark_mode;
    HMENU menu = GetMenu(g_state.hwnd);
    if (!menu) menu = g_state.fs_menu;
    if (menu) {
        CheckMenuItem(menu, IDM_DARK_MODE,
                      MF_BYCOMMAND | (g_state.dark_mode ?
                                      MF_CHECKED : MF_UNCHECKED));
    }
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 줄 번호 거터 토글
 * ------------------------------------------------------------------ */
static void cmd_toggle_line_numbers(void) {
    g_state.show_line_numbers = !g_state.show_line_numbers;

    /* 메뉴 체크 표시 갱신 — 전체화면 중이면 저장된 메뉴에 적용 */
    HMENU menu = GetMenu(g_state.hwnd);
    if (!menu) menu = g_state.fs_menu;
    if (menu) {
        CheckMenuItem(menu, IDM_LINENO,
                      MF_BYCOMMAND | (g_state.show_line_numbers ?
                                      MF_CHECKED : MF_UNCHECKED));
    }

    /* 거터 폭이 바뀌면 가로 스크롤 가용폭도 변하므로 클램프 */
    scroll_h_to(g_state.h_scroll_px);
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 전체화면 토글 (F11)
 *
 * 메뉴/타이틀바/테두리 제거 후 모니터 전체로 확장.
 * 두 번째 호출 시 저장해둔 스타일/위치로 복원.
 * Esc로도 빠져나올 수 있게 wnd_proc에서 처리.
 * ------------------------------------------------------------------ */
static void cmd_toggle_fullscreen(void) {
    HWND hwnd = g_state.hwnd;
    if (!g_state.fs_active) {
        g_state.fs_style    = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        g_state.fs_ex_style = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        g_state.fs_menu     = GetMenu(hwnd);
        g_state.fs_was_maximized = IsZoomed(hwnd);
        if (g_state.fs_was_maximized) ShowWindow(hwnd, SW_RESTORE);
        GetWindowRect(hwnd, &g_state.fs_rect);

        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                        &mi);

        SetMenu(hwnd, NULL);
        SetWindowLongW(hwnd, GWL_STYLE,
                       (LONG)(g_state.fs_style & ~WS_OVERLAPPEDWINDOW));
        SetWindowLongW(hwnd, GWL_EXSTYLE,
                       (LONG)(g_state.fs_ex_style &
                              ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)));
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_state.fs_active = TRUE;
    } else {
        SetMenu(hwnd, g_state.fs_menu);
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)g_state.fs_style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (LONG)g_state.fs_ex_style);
        SetWindowPos(hwnd, NULL,
                     g_state.fs_rect.left, g_state.fs_rect.top,
                     g_state.fs_rect.right - g_state.fs_rect.left,
                     g_state.fs_rect.bottom - g_state.fs_rect.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        if (g_state.fs_was_maximized) ShowWindow(hwnd, SW_MAXIMIZE);
        g_state.fs_active = FALSE;
    }
}

/* ------------------------------------------------------------------
 * 최근 파일 — 레지스트리(HKCU)에 영속화.
 *
 * 인덱스 0이 가장 최근. 최대 RECENT_MAX개. 같은 경로는 중복 제거 후 재정렬.
 * 의도적으로 ini 대신 레지스트리 사용 (Win32 표준, 외부 의존성 없음).
 * ------------------------------------------------------------------ */
static void recent_load(void) {
    g_state.recent_count = 0;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RECENT_REG_PATH, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return;
    for (int i = 0; i < RECENT_MAX; i++) {
        wchar_t name[16];
        _snwprintf_s(name, 16, _TRUNCATE, L"%d", i);
        DWORD type;
        DWORD sz = sizeof(g_state.recent_paths[g_state.recent_count]);
        if (RegQueryValueExW(hk, name, NULL, &type,
                             (BYTE*)g_state.recent_paths[g_state.recent_count],
                             &sz) == ERROR_SUCCESS && type == REG_SZ &&
            g_state.recent_paths[g_state.recent_count][0]) {
            g_state.recent_count++;
        }
    }
    RegCloseKey(hk);
}

static void recent_save(void) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RECENT_REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;
    for (int i = 0; i < RECENT_MAX; i++) {
        wchar_t name[16];
        _snwprintf_s(name, 16, _TRUNCATE, L"%d", i);
        if (i < g_state.recent_count) {
            DWORD bytes = (DWORD)((wcslen(g_state.recent_paths[i]) + 1) *
                                  sizeof(wchar_t));
            RegSetValueExW(hk, name, 0, REG_SZ,
                           (const BYTE*)g_state.recent_paths[i], bytes);
        } else {
            RegDeleteValueW(hk, name);
        }
    }
    RegCloseKey(hk);
}

static void recent_rebuild_menu(void) {
    HMENU m = g_state.recent_menu;
    if (!m) return;
    /* 기존 항목 모두 제거 */
    while (DeleteMenu(m, 0, MF_BYPOSITION)) ;
    if (g_state.recent_count == 0) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(없음)");
        return;
    }
    for (int i = 0; i < g_state.recent_count; i++) {
        wchar_t label[MAX_PATH + 16];
        const wchar_t *fname = wcsrchr(g_state.recent_paths[i], L'\\');
        fname = fname ? fname + 1 : g_state.recent_paths[i];
        _snwprintf_s(label, MAX_PATH + 16, _TRUNCATE,
                     L"&%d  %s", (i + 1) % 10, fname);
        AppendMenuW(m, MF_STRING, IDM_RECENT_BASE + i, label);
    }
}

static void recent_add(const wchar_t *path) {
    /* 호출자가 recent_paths 배열을 가리킬 수도 있으니 로컬 복사 */
    wchar_t copy[MAX_PATH];
    wcsncpy_s(copy, MAX_PATH, path, _TRUNCATE);

    /* 중복 제거 */
    int found = -1;
    for (int i = 0; i < g_state.recent_count; i++) {
        if (_wcsicmp(g_state.recent_paths[i], copy) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (int i = found; i < g_state.recent_count - 1; i++) {
            wcscpy_s(g_state.recent_paths[i], MAX_PATH,
                     g_state.recent_paths[i + 1]);
        }
        g_state.recent_count--;
    }

    /* 앞쪽으로 한 칸씩 밀고 [0]에 삽입 */
    if (g_state.recent_count < RECENT_MAX) g_state.recent_count++;
    for (int i = g_state.recent_count - 1; i > 0; i--) {
        wcscpy_s(g_state.recent_paths[i], MAX_PATH,
                 g_state.recent_paths[i - 1]);
    }
    wcsncpy_s(g_state.recent_paths[0], MAX_PATH, copy, _TRUNCATE);

    recent_save();
    recent_rebuild_menu();
    if (!g_state.fs_active) DrawMenuBar(g_state.hwnd);
}

static void recent_remove(int idx) {
    if (idx < 0 || idx >= g_state.recent_count) return;
    for (int i = idx; i < g_state.recent_count - 1; i++) {
        wcscpy_s(g_state.recent_paths[i], MAX_PATH,
                 g_state.recent_paths[i + 1]);
    }
    g_state.recent_count--;
    recent_save();
    recent_rebuild_menu();
    if (!g_state.fs_active) DrawMenuBar(g_state.hwnd);
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
    g_state.recent_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_POPUP, (UINT_PTR)g_state.recent_menu,
                L"최근 파일(&R)");
    recent_rebuild_menu();
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"종료(&X)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file_menu, L"파일(&F)");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_INC, L"글자 크게\tCtrl+.");
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_DEC, L"글자 작게\tCtrl+,");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_LINENO,
                L"줄 번호 표시(&L)\tCtrl+L");
    AppendMenuW(view_menu, MF_STRING, IDM_DARK_MODE,
                L"다크 모드(&D)\tCtrl+D");
    AppendMenuW(view_menu, MF_STRING, IDM_FULLSCREEN,
                L"전체화면(&F)\tF11");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_FIND,
                L"찾기(&F)...\tCtrl+F");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_NEXT,
                L"다음 찾기\tF3");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_PREV,
                L"이전 찾기\tShift+F3");
    AppendMenuW(view_menu, MF_STRING, IDM_GOTO,
                L"줄 이동(&G)...\tCtrl+G");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_BM_TOGGLE,
                L"책갈피 추가/제거(&B)\tCtrl+B");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_NEXT,
                L"다음 책갈피\tF2");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_PREV,
                L"이전 책갈피\tShift+F2");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_CLEAR,
                L"책갈피 모두 지우기");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view_menu, L"보기(&V)");

    HMENU enc_menu = CreatePopupMenu();
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_AUTO,    L"자동 판별");
    AppendMenuW(enc_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF8,    L"UTF-8");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16LE, L"UTF-16 LE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16BE, L"UTF-16 BE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_CP949,   L"CP949 (EUC-KR)");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_JOHAB,   L"조합형 (Johab)");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_SJIS,    L"Shift-JIS (일본어)");
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
        g_state.search_match_pos = -1;
        create_font();
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE: {
        g_state.client_w = LOWORD(lp);
        g_state.client_h = HIWORD(lp);
        if (g_state.char_height > 0) {
            g_state.visible_lines = g_state.client_h / line_total_height();
        }
        update_scrollbars();
        return 0;
    }

    case WM_PAINT:
        on_paint(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == AUTOSCROLL_TIMER_ID) {
            int max_top = g_state.line_count - g_state.visible_lines;
            if (max_top < 0) max_top = 0;
            if (g_state.top_line >= max_top) autoscroll_stop();
            else scroll_to_line(g_state.top_line + 1);
        }
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
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        WORD keys = GET_KEYSTATE_WPARAM(wp);
        BOOL ctrl  = (keys & MK_CONTROL) != 0;
        BOOL shift = (keys & MK_SHIFT)   != 0;
        if (ctrl && shift) {
            char_spacing_change(notches);
        } else if (ctrl) {
            font_change(notches);
        } else if (shift) {
            line_spacing_change(notches);
        } else if (g_state.autoscroll_active) {
            /* 자동 스크롤 진행 중: 휠로 속도 조절 (위=빠름) */
            autoscroll_change_speed(notches * 25);
        } else {
            UINT lines_per_notch = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0,
                                  &lines_per_notch, 0);
            int lines = -notches * (int)lines_per_notch;
            scroll_to_line(g_state.top_line + lines);
        }
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
        case 'G':
            if (ctrl) cmd_goto_line();
            break;
        case 'L':
            if (ctrl) cmd_toggle_line_numbers();
            break;
        case 'F':
            if (ctrl) cmd_find();
            break;
        case 'D':
            if (ctrl) cmd_toggle_dark_mode();
            break;
        case 'B':
            if (ctrl) cmd_bookmark_toggle();
            break;
        case VK_F2: {
            int shift = GetKeyState(VK_SHIFT) & 0x8000;
            cmd_bookmark_jump(shift ? FALSE : TRUE);
            break;
        }
        case VK_F3: {
            int shift = GetKeyState(VK_SHIFT) & 0x8000;
            cmd_find_again(shift ? FALSE : TRUE);
            break;
        }
        case VK_F11:
            cmd_toggle_fullscreen();
            break;
        case VK_SPACE:
            autoscroll_toggle();
            break;
        case VK_ESCAPE:
            if (g_state.autoscroll_active) autoscroll_stop();
            else if (g_state.fs_active) cmd_toggle_fullscreen();
            break;
        case VK_OEM_PERIOD:  /* '.' 키 */
            if (ctrl) font_change(+1);
            break;
        case VK_OEM_COMMA:   /* ',' 키 */
            if (ctrl) font_change(-1);
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:        cmd_open_file(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        case IDM_GOTO:        cmd_goto_line(); break;
        case IDM_LINENO:      cmd_toggle_line_numbers(); break;
        case IDM_DARK_MODE:   cmd_toggle_dark_mode(); break;
        case IDM_FULLSCREEN:  cmd_toggle_fullscreen(); break;
        case IDM_FIND:        cmd_find(); break;
        case IDM_FIND_NEXT:   cmd_find_again(TRUE); break;
        case IDM_FIND_PREV:   cmd_find_again(FALSE); break;
        case IDM_BM_TOGGLE:   cmd_bookmark_toggle(); break;
        case IDM_BM_NEXT:     cmd_bookmark_jump(TRUE); break;
        case IDM_BM_PREV:     cmd_bookmark_jump(FALSE); break;
        case IDM_BM_CLEAR:    cmd_bookmark_clear(); break;
        case IDM_FONT_INC:     font_change(+1); break;
        case IDM_FONT_DEC:     font_change(-1); break;
        case IDM_ENC_AUTO:    reload_with_encoding(ENC_UNKNOWN); break;
        case IDM_ENC_UTF8:    reload_with_encoding(ENC_UTF8); break;
        case IDM_ENC_UTF16LE: reload_with_encoding(ENC_UTF16_LE); break;
        case IDM_ENC_UTF16BE: reload_with_encoding(ENC_UTF16_BE); break;
        case IDM_ENC_CP949:   reload_with_encoding(ENC_CP949); break;
        case IDM_ENC_JOHAB:   reload_with_encoding(ENC_JOHAB); break;
        case IDM_ENC_SJIS:    reload_with_encoding(ENC_SJIS); break;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"hview — Win32 한글 텍스트 뷰어\n"
                L"최대 64MB, 조합형 지원",
                APP_TITLE, MB_OK | MB_ICONINFORMATION);
            break;
        default: {
            int id = LOWORD(wp);
            if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + RECENT_MAX) {
                int idx = id - IDM_RECENT_BASE;
                if (idx < g_state.recent_count) {
                    /* 호출 중 recent_paths 배열이 재정렬되므로 복사본 사용 */
                    wchar_t local[MAX_PATH];
                    wcscpy_s(local, MAX_PATH, g_state.recent_paths[idx]);
                    if (!load_file(local, ENC_UNKNOWN)) {
                        /* 죽은 항목 정리 — 파일이 사라졌을 가능성 */
                        recent_remove(idx);
                    }
                }
            }
            break;
        }
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

    /* 최근 파일 — 메뉴 생성 전에 로드되어야 초기 메뉴에 반영됨 */
    recent_load();

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
