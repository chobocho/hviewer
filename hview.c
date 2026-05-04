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
 *   Ctrl+C          선택 영역 복사
 *   Ctrl+A          모두 선택
 *   마우스 드래그   텍스트 선택
 *   Ctrl+F          찾기
 *   F3 / Shift+F3   다음/이전 찾기
 *   Ctrl+G          줄 이동
 *   Ctrl+L          줄 번호 표시 토글
 *   Ctrl+D          다크 모드 토글
 *   Ctrl+W          자동 줄바꿈 토글
 *   Ctrl+B          책갈피 추가/제거 (현재 위치)
 *   Ctrl+, / Ctrl+. 이전/다음 책갈피
 *   Ctrl+Shift+, / Ctrl+Shift+. 폰트 크기 -/+
 *   F2 / Shift+F2   인코딩 순환 (CP949↔UTF-8↔Johab; 그 외는 메뉴)
 *   Ctrl+T          목차 (마크다운 # 헤딩)
 *   F11             전체화면 토글 (Esc로도 빠져나옴)
 *   Space           자동 스크롤 토글 (진행 중 휠로 속도 조절)
 *   ↑/↓             한 줄 스크롤
 *   PageUp/PageDown 한 화면 스크롤
 *   Home/End        문서 처음/끝
 *   Ctrl+Home/End   동일
 *   ←/→             가로 스크롤
 *   Alt+←/→/↑/↓     여백 조절
 *   Ctrl+휠         폰트 크기 -/+
 *   Shift+휠        줄 간격 조절
 *   Ctrl+Shift+휠   자간 조절
 *   Alt+1           두 쪽 보기(분할) 토글
 *   F1              단축키 도움말
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
#include <limits.h>
#include <wctype.h>
#include <commctrl.h>

#include "encoding.h"
#include "hanja.h"
#include "hview_resources.h"

/* ------------------------------------------------------------------
 * 상수
 * ------------------------------------------------------------------ */
#define MAX_FILE_SIZE       (64 * 1024 * 1024)   /* 64MB */
#define INITIAL_LINE_CAP    1024
#define APP_TITLE           L"hViewer V0.1b"

/* 자동 스크롤 타이머 */
#define AUTOSCROLL_TIMER_ID 1
#define AUTOSCROLL_DEFAULT_MS 250
#define AUTOSCROLL_MIN_MS     30
#define AUTOSCROLL_MAX_MS     2000

#define MAX_BOOKMARKS       32

/* 분할 보기 — 가운데 디바이더 폭 */
#define SPLIT_DIVIDER_PX    6

/* 사용자 색상 기본값 — 흰 배경 + 검은 글자 (light theme와 동일).
 * theme.txt 파일이 손상되거나 없으면 이 값으로 복구. */
#define HVIEW_THEME_DEFAULT_BG  RGB(255, 255, 255)
#define HVIEW_THEME_DEFAULT_FG  RGB(  0,   0,   0)

/* 메뉴 ID */
#define IDM_OPEN            1001
#define IDM_EXIT            1002
#define IDM_GOTO            1003
#define IDM_SAVE_AS         1004
#define IDM_FONT_INC        1010
#define IDM_FONT_DEC        1011
#define IDM_LINENO          1012
#define IDM_FULLSCREEN      1013
#define IDM_FIND            1014
#define IDM_FIND_NEXT       1015
#define IDM_FIND_PREV       1016
#define IDM_DARK_MODE       1017
#define IDM_CHOOSE_FONT     1018
#define IDM_SHOW_TOC        1019
#define IDM_WRAP            1027
#define IDM_SPLIT           1028
#define IDM_HANJA           1029
#define IDM_COPY            1030
#define IDM_THEME_BG        1050
#define IDM_THEME_FG        1051
#define IDM_THEME_RESET     1052
#define IDM_THEME_HL_BG     1053
#define IDM_THEME_HL_FG     1054
#define IDM_THEME_HL_RESET  1055
#define IDM_STATUSBAR       1056
#define IDM_SELECT_ALL      1031
#define IDM_BM_TOGGLE       1040
#define IDM_BM_NEXT         1041
#define IDM_BM_PREV         1042
#define IDM_BM_CLEAR        1043
#define IDM_ENC_AUTO        1020
#define IDM_RECENT_CLEAR    1099
#define IDM_RECENT_BASE     1100        /* 1100..1109 */
#define IDM_SEARCH_HIST_BASE 1110       /* 1110..1119 */
#define IDM_TOC_BASE        2000        /* 2000..2000+toc_count-1 */
#define RECENT_MAX          10
#define SEARCH_HIST_MAX     10
#define RECENT_REG_PATH     L"Software\\hview\\Recent"
#define SETTINGS_REG_PATH   L"Software\\hview\\Settings"
#define BOOKMARKS_REG_PATH  L"Software\\hview\\Bookmarks"
#define SEARCH_HIST_REG_PATH L"Software\\hview\\SearchHistory"
#define IDM_ENC_UTF8        1021
#define IDM_ENC_UTF16LE     1022
#define IDM_ENC_UTF16BE     1023
#define IDM_ENC_CP949       1024
#define IDM_ENC_JOHAB       1025
#define IDM_ENC_SJIS        1026
#define IDM_HELP            1089
#define IDM_ABOUT           1090
#define IDM_FIND_CASE       1060
#define IDM_FIND_WORD       1061

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

    /* 렌더 라인 — wrap_mode가 OFF면 doc 라인과 동일.
     * ON이면 소프트 wrap이 추가되어 더 많은 라인을 가짐. */
    int           *line_offsets;
    int            line_count;
    int            line_cap;

    /* 문서 라인 (실제 줄바꿈) — goto, 책갈피, 줄 번호 가터 표시에 사용 */
    int           *doc_line_offsets;
    int            doc_line_count;
    int            doc_line_cap;

    /* 매핑: render → doc, doc → 첫 render. line_count / doc_line_count 크기. */
    int           *render_to_doc;
    int           *doc_to_render;

    /* 자동 줄바꿈 모드 */
    BOOL           wrap_mode;

    /* 뷰포트 — 분할 보기 시 두 페인이 독립 스크롤.
     * 비분할 시 pane 1 필드는 사용하지 않음. */
    int            top_line;
    int            h_scroll_px;     /* 가로 스크롤 (픽셀) */
    int            top_line2;       /* pane 1의 top_line (분할 시) */
    int            h_scroll_px2;    /* pane 1의 h_scroll_px */
    int            active_pane;     /* 0 또는 1 — 키/스크롤바가 영향을 미치는 페인 */
    BOOL           split_active;    /* 분할 보기 토글 (Alt+1) */
    int            max_line_px;     /* 가장 긴 줄의 픽셀 폭 */

    /* 폰트/렌더링 */
    HFONT          font;
    int            font_size;          /* 포인트 */
    wchar_t        font_face[LF_FACESIZE]; /* 빈 문자열이면 기본(D2Coding) */
    LONG           font_weight;        /* FW_NORMAL=400, 0이면 NORMAL 처리 */
    BYTE           font_italic;
    int            char_height;        /* 한 줄 픽셀 높이 */
    int            avg_char_width;     /* 영문 평균 폭 (스크롤 단위) */
    int            line_spacing_extra; /* 줄 간격 추가 픽셀 (Shift+휠) */
    int            char_spacing_extra; /* 자간 추가 픽셀 (Ctrl+Shift+휠) */
    int            margin_left_px;     /* 좌측 여백 (Alt+←/→) */
    int            margin_top_px;      /* 상단 여백 (Alt+↑/↓) */

    /* 윈도우 */
    HWND           hwnd;
    HWND           status_hwnd;        /* 상태 표시줄 (msctls_statusbar32) */
    BOOL           status_visible;     /* 메뉴 토글로 변경 (영속화) */
    int            status_height;      /* 0이면 미생성/숨김 */
    int            client_w;
    int            client_h;            /* 상태 표시줄을 제외한 본문 높이 */
    int            visible_lines;

    /* 줄 번호 거터 */
    BOOL           show_line_numbers;

    /* 검색 (Ctrl+F / F3 / Shift+F3) */
    wchar_t        search_needle[256];
    int            search_needle_len;
    int            search_match_pos;    /* text 내 매칭 시작 오프셋, -1=없음 */
    BOOL           search_canceled;     /* Esc로 검색 중단 요청 (find_substr_offset가 폴링) */
    /* 검색어 히스토리 — 인덱스 0이 가장 최근. 메뉴에서 클릭하거나
     * cmd_find가 새 needle을 입력받을 때 갱신. */
    wchar_t        search_history[SEARCH_HIST_MAX][256];
    int            search_history_count;
    HMENU          search_hist_menu;

    /* 자동 스크롤 (Space) */
    BOOL           autoscroll_active;
    int            autoscroll_delay_ms;

    /* 다크 모드 */
    BOOL           dark_mode;

    /* 사용자 색 — 메뉴(보기 → 배경색/글자색)로 변경, theme.txt에 저장.
     * theme()이 light/dark 프리셋의 bg/fg를 이 값으로 덮어쓴다.
     * 다크 모드 토글 시 프리셋의 bg/fg로 재설정 (예측 가능성). */
    COLORREF       user_bg;
    COLORREF       user_fg;
    /* 검색 하이라이트 색 — _set 플래그가 0이면 프리셋(THEME_LIGHT/DARK)
     * 그대로 사용. 1이면 user_hl_bg/fg가 프리셋을 덮어씀. */
    COLORREF       user_hl_bg;
    COLORREF       user_hl_fg;
    int            user_hl_set;

    /* 한자 음 표시 (보기 메뉴 토글) — 렌더 직전 1:1 한자→한글 치환.
     * 원본 텍스트(g_state.text)는 그대로, 검색/선택은 원본 기준. */
    BOOL           hanja_show;

    /* 책갈피 (Ctrl+B / F2 / Shift+F2) — 줄 번호 정렬 보관 */
    int            bookmarks[MAX_BOOKMARKS];
    int            bookmark_count;

    /* 텍스트 선택 (마우스 드래그) — text 오프셋, -1=선택 없음 */
    int            sel_anchor;
    int            sel_caret;
    BOOL           sel_dragging;

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

    /* 검색 옵션 — 메뉴 토글로 변경, 레지스트리에 영속화 */
    BOOL           search_case_sensitive;
    BOOL           search_whole_word;

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
    COLORREF sel_bg;      /* 선택 영역 배경 */
    COLORREF sel_fg;
} Theme;

static const Theme THEME_LIGHT = {
    RGB(255, 255, 255), RGB(0, 0, 0),
    RGB(240, 240, 240), RGB(128, 128, 128),
    RGB(255, 230, 80),  RGB(0, 0, 0),
    RGB(128, 128, 128),
    RGB(220, 100,   0),
    RGB(180, 210, 255), RGB(0, 0, 0)
};

static const Theme THEME_DARK = {
    RGB( 30,  30,  30), RGB(220, 220, 220),
    RGB( 45,  45,  45), RGB(140, 140, 140),
    RGB(180, 130,   0), RGB(  0,   0,   0),
    RGB(140, 140, 140),
    RGB(255, 180, 100),
    RGB( 60,  90, 160), RGB(255, 255, 255)
};

/* 두 색 사이 RGB 채널 블렌드. pct_a = a의 비율(0~100). */
static COLORREF blend_color(COLORREF a, COLORREF b, int pct_a) {
    int r  = (GetRValue(a) * pct_a + GetRValue(b) * (100 - pct_a)) / 100;
    int g  = (GetGValue(a) * pct_a + GetGValue(b) * (100 - pct_a)) / 100;
    int bl = (GetBValue(a) * pct_a + GetBValue(b) * (100 - pct_a)) / 100;
    return RGB(r, g, bl);
}

/* 다크/라이트 프리셋 + 사용자 색(bg/fg) 오버라이드.
 * dim_fg("파일을 드래그..." 빈 화면 메시지)는 사용자 bg/fg의 중간색으로
 * 자동 계산 — 어떤 사용자 색 조합에서도 자연스러운 가독성 유지.
 * 다른 색(거터/하이라이트/선택)은 프리셋 그대로. */
static Theme g_theme_buf;
static const Theme *theme(void) {
    g_theme_buf = g_state.dark_mode ? THEME_DARK : THEME_LIGHT;
    g_theme_buf.bg     = g_state.user_bg;
    g_theme_buf.fg     = g_state.user_fg;
    g_theme_buf.dim_fg = blend_color(g_state.user_bg, g_state.user_fg, 50);
    if (g_state.user_hl_set) {
        g_theme_buf.hl_bg = g_state.user_hl_bg;
        g_theme_buf.hl_fg = g_state.user_hl_fg;
    }
    return &g_theme_buf;
}

/* 전방 선언 — 정의 순서가 어긋나는 경우만 */
static void recent_add(const wchar_t *path);
static void bookmarks_load(const wchar_t *path);
static void bookmarks_save(const wchar_t *path);
static void rebuild_render_lines_preserve(void);
static void autoscroll_stop(void);
static int  selection_start(void);
static int  selection_end(void);
static BOOL has_selection(void);
static void selection_clear(void);
static int  line_for_offset(int offset);
static BOOL bookmark_has(int line);
static int  gutter_pixel_width(void);
static void theme_save(void);
static int  theme_load_or_default(void);

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
    int            dpi;             /* 부모 윈도우 DPI (스케일 기준 96) */
    HFONT          font;            /* 다이얼로그 전용 — WM_DESTROY에서 해제 */
} PromptCtx;

static PromptCtx *g_prompt;

#define PROMPT_SCALE(v) MulDiv((v), g_prompt->dpi, 96)

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

        /* DPI 스케일된 UI 폰트 — DEFAULT_GUI_FONT는 96 DPI 고정이라
         * 고해상도 화면에서 너무 작게 나옴. 9pt Segoe UI를 현재 DPI로. */
        LOGFONTW lf = { 0 };
        lf.lfHeight  = -MulDiv(9, g_prompt->dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
        g_prompt->font = CreateFontIndirectW(&lf);

        CreateWindowW(L"STATIC", g_prompt->prompt,
                      WS_CHILD | WS_VISIBLE,
                      PROMPT_SCALE(12), PROMPT_SCALE(12),
                      PROMPT_SCALE(280), PROMPT_SCALE(18),
                      hwnd, NULL, hi, NULL);
        g_prompt->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", g_prompt->out_buf,
            edit_style,
            PROMPT_SCALE(12), PROMPT_SCALE(34),
            PROMPT_SCALE(280), PROMPT_SCALE(24),
            hwnd, (HMENU)(UINT_PTR)100, hi, NULL);
        CreateWindowW(L"BUTTON", L"확인",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      PROMPT_SCALE(138), PROMPT_SCALE(70),
                      PROMPT_SCALE(76), PROMPT_SCALE(26),
                      hwnd, (HMENU)(UINT_PTR)IDOK, hi, NULL);
        CreateWindowW(L"BUTTON", L"취소",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      PROMPT_SCALE(218), PROMPT_SCALE(70),
                      PROMPT_SCALE(76), PROMPT_SCALE(26),
                      hwnd, (HMENU)(UINT_PTR)IDCANCEL, hi, NULL);
        EnumChildWindows(hwnd, prompt_set_font,
                         (LPARAM)(g_prompt->font ? g_prompt->font
                                                 : (HFONT)GetStockObject(DEFAULT_GUI_FONT)));
        SendMessageW(g_prompt->edit, EM_SETSEL, 0, -1);
        SetFocus(g_prompt->edit);
        return 0;
    }
    case WM_DESTROY:
        if (g_prompt && g_prompt->font) {
            DeleteObject(g_prompt->font);
            g_prompt->font = NULL;
        }
        break;
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

    /* 부모 윈도우의 DPI — HiDPI 디스플레이에서 다이얼로그가 작아지지 않도록 */
    int dpi;
    {
        HDC hdc = GetDC(parent);
        dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(parent, hdc);
        if (dpi <= 0) dpi = 96;
    }

    PromptCtx ctx;
    ctx.prompt       = prompt;
    ctx.numeric_only = numeric_only;
    ctx.out_buf      = buf;
    ctx.out_buf_len  = buflen;
    ctx.done         = 0;
    ctx.edit         = NULL;
    ctx.dpi          = dpi;
    ctx.font         = NULL;
    g_prompt = &ctx;

    /* 부모 중앙 배치 — 96 DPI 기준 320×140을 현재 DPI로 스케일 */
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = MulDiv(320, dpi, 96);
    int h = MulDiv(140, dpi, 96);
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

    LONG weight = g_state.font_weight ? g_state.font_weight : FW_NORMAL;
    const wchar_t *face = g_state.font_face[0] ? g_state.font_face : L"D2Coding";

    /* 한글 charset 강제 — 폰트 변경 시에도 한글 글리프 보장 */
    g_state.font = CreateFontW(
        height, 0, 0, 0,
        weight, g_state.font_italic, FALSE, FALSE,
        HANGUL_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        face
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
 * 문자 너비 셀 — wrap 계산 시 시각적 폭을 cell 단위로 근사.
 *
 * 정확한 GDI 측정은 수십만 줄 처리에 너무 느려서 BMP 영역 휴리스틱:
 *   ASCII / 라틴 → 1 cell, CJK → 2 cells.
 * 변폭 폰트일 경우 약간 어긋나지만 wrap이 되긴 됨.
 * ------------------------------------------------------------------ */
static int char_width_cells(wchar_t c) {
    if (c < 0x80) return 1;
    if (c >= 0x1100 && c <= 0x115F) return 2;     /* Hangul Jamo */
    if (c >= 0x2E80 && c <= 0x9FFF) return 2;     /* CJK */
    if (c >= 0xAC00 && c <= 0xD7A3) return 2;     /* Hangul Syllables */
    if (c >= 0xF900 && c <= 0xFAFF) return 2;     /* CJK Compat */
    if (c >= 0xFF00 && c <= 0xFF60) return 2;     /* Fullwidth */
    if (c >= 0xFFE0 && c <= 0xFFE6) return 2;
    return 1;
}

/* ------------------------------------------------------------------
 * 문서 줄 인덱스 — \r, \n, \r\n으로 구분된 실제 줄.
 *
 * 64MB 텍스트(최대 ~3천만 줄)도 ~100ms. 동기 처리로 충분.
 * 알고리즘: 줄 종결자에서 다음 줄 시작 위치 등록. \r\n은 한 번만.
 * 마지막에 sentinel(text_len)로 closure.
 * ------------------------------------------------------------------ */
static int build_doc_line_index(void) {
    free(g_state.doc_line_offsets);
    g_state.doc_line_cap = INITIAL_LINE_CAP;
    g_state.doc_line_offsets = (int*)malloc(g_state.doc_line_cap * sizeof(int));
    if (!g_state.doc_line_offsets) return 0;

    g_state.doc_line_count = 0;
    g_state.doc_line_offsets[g_state.doc_line_count++] = 0;

    const wchar_t *t = g_state.text;
    size_t n = g_state.text_len;

    for (size_t i = 0; i < n; i++) {
        wchar_t c = t[i];
        if (c == L'\r' || c == L'\n') {
            size_t next = i + 1;
            if (c == L'\r' && next < n && t[next] == L'\n') {
                i = next;
            }
            if (g_state.doc_line_count >= g_state.doc_line_cap) {
                /* realloc 실패 시 g_state 포인터를 NULL로 덮어쓰지 않도록
                 * 임시 변수에 받아 NULL 체크 후 교체 (dangling 방지) */
                int new_cap = g_state.doc_line_cap * 2;
                int *bigger = (int*)realloc(g_state.doc_line_offsets,
                                            (size_t)new_cap * sizeof(int));
                if (!bigger) return 0;
                g_state.doc_line_offsets = bigger;
                g_state.doc_line_cap = new_cap;
            }
            g_state.doc_line_offsets[g_state.doc_line_count++] = (int)(i + 1);
        }
    }

    if (g_state.doc_line_count >= g_state.doc_line_cap) {
        int new_cap = g_state.doc_line_cap + 1;
        int *bigger = (int*)realloc(g_state.doc_line_offsets,
                                    (size_t)new_cap * sizeof(int));
        if (!bigger) return 0;
        g_state.doc_line_offsets = bigger;
        g_state.doc_line_cap = new_cap;
    }
    g_state.doc_line_offsets[g_state.doc_line_count] = (int)n;
    return 1;
}

/* 텍스트 영역 폭 forward 선언 — render 빌더가 사용 */
static int text_area_width(void);

/* ------------------------------------------------------------------
 * 렌더 라인 빌드 — wrap_mode에 따라 doc 라인을 그대로 쓰거나 wrap.
 *
 * 결과:
 *   line_offsets[k]    = render 라인 k의 텍스트 시작
 *   line_count         = render 라인 수
 *   render_to_doc[k]   = render k가 속한 doc 라인
 *   doc_to_render[d]   = doc d의 첫 render 라인
 * ------------------------------------------------------------------ */
static int build_render_lines(void) {
    free(g_state.line_offsets);
    free(g_state.render_to_doc);
    free(g_state.doc_to_render);
    g_state.line_offsets   = NULL;
    g_state.render_to_doc  = NULL;
    g_state.doc_to_render  = NULL;
    g_state.line_count     = 0;
    g_state.line_cap       = 0;

    int dn = g_state.doc_line_count;
    if (dn <= 0) return 0;

    g_state.doc_to_render = (int*)malloc((size_t)dn * sizeof(int));
    if (!g_state.doc_to_render) return 0;

    /* avg_char_width 가 0이면 wrap 정확도 떨어짐 — wrap 강제 OFF 처리 */
    int avail_w = text_area_width();
    int unit = (g_state.avg_char_width > 0 ? g_state.avg_char_width : 8)
               + g_state.char_spacing_extra;
    int max_cells = (g_state.wrap_mode && avail_w > unit) ? avail_w / unit : 0;

    if (max_cells <= 1) {
        /* wrap OFF or 너비가 너무 작음 → doc 라인을 그대로 복제 */
        g_state.line_cap = dn + 1;
        g_state.line_offsets = (int*)malloc((size_t)(dn + 1) * sizeof(int));
        g_state.render_to_doc = (int*)malloc((size_t)dn * sizeof(int));
        if (!g_state.line_offsets || !g_state.render_to_doc) return 0;
        memcpy(g_state.line_offsets, g_state.doc_line_offsets,
               (size_t)(dn + 1) * sizeof(int));
        for (int i = 0; i < dn; i++) {
            g_state.render_to_doc[i] = i;
            g_state.doc_to_render[i] = i;
        }
        g_state.line_count = dn;
        return 1;
    }

    /* wrap ON — 각 doc 라인을 cell 폭 기준으로 분할.
     * cap 계산 정수 오버플로 방어: dn은 int, dn/4도 int,
     * dn + dn/4 + 16이 INT_MAX를 넘지 않도록 검사. */
    if (dn > INT_MAX - dn / 4 - 16) return 0;
    int cap = dn + dn / 4 + 16;
    if ((size_t)cap > SIZE_MAX / sizeof(int) - 1) return 0;
    g_state.line_offsets = (int*)malloc((size_t)(cap + 1) * sizeof(int));
    g_state.render_to_doc = (int*)malloc((size_t)cap * sizeof(int));
    if (!g_state.line_offsets || !g_state.render_to_doc) return 0;
    g_state.line_cap = cap;

    int rcount = 0;

    for (int d = 0; d < dn; d++) {
        int start = g_state.doc_line_offsets[d];
        int end   = g_state.doc_line_offsets[d + 1];
        /* 줄 종결자 제외한 본문 끝 */
        int text_end = end;
        while (text_end > start) {
            wchar_t c = g_state.text[text_end - 1];
            if (c == L'\r' || c == L'\n') text_end--;
            else break;
        }

        g_state.doc_to_render[d] = rcount;

        int seg_start = start;
        int cells = 0;
        int last_break = -1;   /* 단어 경계 후보 (공백/탭 위치) */
        int i = start;

        while (i < text_end) {
            wchar_t c = g_state.text[i];
            int w = char_width_cells(c);

            if (cells + w > max_cells && i > seg_start) {
                int wrap_at = (last_break > seg_start) ?
                              (last_break + 1) : i;
                /* render 라인 emit */
                if (rcount + 2 > g_state.line_cap) {
                    /* realloc 두 번 — 둘 중 하나라도 실패하면 기존 포인터
                     * 보존 채로 빠져나감 (NULL 덮어쓰기 방지) */
                    int new_cap = g_state.line_cap * 2;
                    if (new_cap <= g_state.line_cap) return 0; /* 오버플로 */
                    int *a = (int*)realloc(g_state.line_offsets,
                                           (size_t)(new_cap + 1) * sizeof(int));
                    if (!a) return 0;
                    g_state.line_offsets = a;
                    int *b = (int*)realloc(g_state.render_to_doc,
                                           (size_t)new_cap * sizeof(int));
                    if (!b) return 0;
                    g_state.render_to_doc = b;
                    g_state.line_cap = new_cap;
                }
                g_state.line_offsets[rcount]  = seg_start;
                g_state.render_to_doc[rcount] = d;
                rcount++;

                seg_start = wrap_at;
                cells = 0;
                last_break = -1;
                i = wrap_at;
                continue;
            }
            cells += w;
            if (c == L' ' || c == L'\t') last_break = i;
            i++;
        }

        /* doc 라인의 마지막 segment (본문이 비어도 한 render 라인 emit) */
        if (rcount + 2 > g_state.line_cap) {
            g_state.line_cap *= 2;
            int *a = (int*)realloc(g_state.line_offsets,
                                   (size_t)(g_state.line_cap + 1) * sizeof(int));
            int *b = (int*)realloc(g_state.render_to_doc,
                                   (size_t)g_state.line_cap * sizeof(int));
            if (!a || !b) return 0;
            g_state.line_offsets  = a;
            g_state.render_to_doc = b;
        }
        g_state.line_offsets[rcount]  = seg_start;
        g_state.render_to_doc[rcount] = d;
        rcount++;
    }

    g_state.line_offsets[rcount] = (int)g_state.text_len;
    g_state.line_count = rcount;
    return 1;
}

/* ------------------------------------------------------------------
 * 가장 긴 줄의 픽셀 폭 계산 — 가로 스크롤 범위용.
 *
 * 64MB에 줄이 많으면 비싸므로, 가시 영역에서만 lazy하게 갱신해도 됨.
 * 일단 단순 구현: 전체 훑기. 필요하면 최적화.
 * ------------------------------------------------------------------ */
static void measure_max_line_width(void) {
    /* wrap 모드면 가로 스크롤이 의미 없음 (모든 줄이 viewport 폭에 맞음) */
    if (g_state.wrap_mode) {
        g_state.max_line_px = 0;
        return;
    }

    HDC hdc = GetDC(g_state.hwnd);
    HFONT old = (HFONT)SelectObject(hdc, g_state.font);

    int max_w = 0;
    SIZE sz;
    int cextra = g_state.char_spacing_extra;
    /* doc 줄 전체 측정. 10000줄 초과 시 등간격 stride 샘플링으로 전체에서
     * 약 10000개 후보를 측정 (앞쪽만 보면 후반부 긴 줄을 놓침). */
    int dn = g_state.doc_line_count;
    int stride = dn > 10000 ? (dn + 9999) / 10000 : 1;
    for (int i = 0; i < dn; i += stride) {
        int start = g_state.doc_line_offsets[i];
        int end   = g_state.doc_line_offsets[i + 1];
        int len = end - start;
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
    if (!g_state.show_line_numbers || g_state.doc_line_count == 0) return 0;
    int digits = 1, n = g_state.doc_line_count;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 4) digits = 4;
    int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
    return (digits + 2) * unit;
}

/* ------------------------------------------------------------------
 * 분할 보기 페인 좌표.
 *
 * 비분할 시 페인 0이 클라이언트 영역 전체를 차지. 분할 시 가운데
 * 디바이더(SPLIT_DIVIDER_PX)를 두고 좌/우로 균등 분할.
 * 거터/여백/wrap 계산은 모두 페인 단위로 수행.
 * ------------------------------------------------------------------ */
static int pane_x_left(int idx) {
    if (!g_state.split_active) return 0;
    int mid = g_state.client_w / 2;
    int half = SPLIT_DIVIDER_PX / 2;
    return (idx == 0) ? 0 : (mid + half);
}

static int pane_x_right(int idx) {
    if (!g_state.split_active) return g_state.client_w;
    int mid = g_state.client_w / 2;
    int half = SPLIT_DIVIDER_PX / 2;
    return (idx == 0) ? (mid - half) : g_state.client_w;
}

static int pane_width(int idx) {
    int w = pane_x_right(idx) - pane_x_left(idx);
    return w > 0 ? w : 0;
}

static int pane_top_line(int idx) {
    return idx == 1 ? g_state.top_line2 : g_state.top_line;
}

static void set_pane_top_line(int idx, int v) {
    if (idx == 1) g_state.top_line2 = v;
    else          g_state.top_line  = v;
}

static int pane_h_scroll(int idx) {
    return idx == 1 ? g_state.h_scroll_px2 : g_state.h_scroll_px;
}

static void set_pane_h_scroll(int idx, int v) {
    if (idx == 1) g_state.h_scroll_px2 = v;
    else          g_state.h_scroll_px  = v;
}

static int pane_text_area_width(int idx) {
    int w = pane_width(idx) - gutter_pixel_width() - g_state.margin_left_px;
    return w > 0 ? w : 0;
}

/* 클라이언트 X 좌표 → 페인 인덱스. 디바이더 위면 -1. */
static int pane_at_x(int mx) {
    if (!g_state.split_active) return 0;
    int mid = g_state.client_w / 2;
    int half = SPLIT_DIVIDER_PX / 2;
    if (mx <  mid - half) return 0;
    if (mx >= mid + half) return 1;
    return -1;
}

/* 텍스트 영역 가용 폭 — wrap 계산은 active 페인 기준.
 * (분할 시 두 페인이 동일 폭이라 양쪽 모두 같은 wrap 적용 가능) */
static int text_area_width(void) {
    return pane_text_area_width(g_state.active_pane);
}

/* ------------------------------------------------------------------
 * 상태 표시줄 — 4 파트:
 *   [0] 줄 X / Y
 *   [1] 위치 N%
 *   [2] 인코딩
 *   [3] 파일 크기 + 글자 수
 *
 * 호출 시점: 파일 로드, 스크롤, 인코딩 변경, wrap/split 토글.
 * 가시성 토글은 IDM_STATUSBAR + StatusBar 레지스트리 키.
 * ------------------------------------------------------------------ */
static void status_update(void) {
    if (!g_state.status_hwnd || !g_state.status_visible) return;
    HWND s = g_state.status_hwnd;

    int top_doc = (g_state.line_count > 0 && g_state.render_to_doc) ?
                  g_state.render_to_doc[g_state.top_line] + 1 : 0;
    int total_doc = g_state.doc_line_count;
    int pct = (total_doc > 0) ? (top_doc * 100) / total_doc : 0;

    wchar_t b0[64], b1[64], b2[64], b3[96];
    _snwprintf_s(b0, 64, _TRUNCATE, L" 줄 %d / %d", top_doc, total_doc);
    _snwprintf_s(b1, 64, _TRUNCATE, L" %d%%", pct);
    _snwprintf_s(b2, 64, _TRUNCATE, L" %s", encoding_name(g_state.encoding));
    /* raw_len은 원본 바이트, text_len은 wchar 수 */
    _snwprintf_s(b3, 96, _TRUNCATE, L" %zu바이트  %zu자",
                 g_state.raw_len, g_state.text_len);

    SendMessageW(s, SB_SETTEXTW, 0, (LPARAM)b0);
    SendMessageW(s, SB_SETTEXTW, 1, (LPARAM)b1);
    SendMessageW(s, SB_SETTEXTW, 2, (LPARAM)b2);
    SendMessageW(s, SB_SETTEXTW, 3, (LPARAM)b3);
}

static void status_set_parts(void) {
    if (!g_state.status_hwnd || !g_state.status_visible) return;
    /* 클라이언트 폭 기준으로 4파트 너비 계산. 우측에서 역으로 잡고 0번이 늘어남. */
    RECT rc;
    GetClientRect(g_state.hwnd, &rc);
    int w = rc.right - rc.left;
    if (w < 200) w = 200;
    int p3 = w;             /* 우측 끝 */
    int p2 = w - 180;
    int p1 = w - 280;
    int p0 = w - 360;
    if (p0 < 100) p0 = 100;
    if (p1 < p0 + 60) p1 = p0 + 60;
    if (p2 < p1 + 60) p2 = p1 + 60;
    if (p3 < p2 + 60) p3 = p2 + 60;
    int parts[4] = { p0, p1, p2, p3 };
    SendMessageW(g_state.status_hwnd, SB_SETPARTS, 4, (LPARAM)parts);
    status_update();
}

static void status_recompute_height(void) {
    g_state.status_height = 0;
    if (g_state.status_hwnd && g_state.status_visible) {
        RECT sr;
        GetWindowRect(g_state.status_hwnd, &sr);
        g_state.status_height = sr.bottom - sr.top;
    }
}

static int line_total_height(void) {
    int h = g_state.char_height + g_state.line_spacing_extra;
    return h > 0 ? h : 1;
}

/* ------------------------------------------------------------------
 * 스크롤바 업데이트
 * ------------------------------------------------------------------ */
static void update_scrollbars(void) {
    int idx = g_state.active_pane;
    /* 세로: 줄 단위 — active 페인 기준 */
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0;
    si.nMax = g_state.line_count > 0 ? g_state.line_count - 1 : 0;
    si.nPage = g_state.visible_lines > 0 ? g_state.visible_lines : 1;
    si.nPos = pane_top_line(idx);
    SetScrollInfo(g_state.hwnd, SB_VERT, &si, TRUE);

    /* 가로: 평균 글자 폭 단위, 거터 제외 — active 페인 기준 */
    int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
    int avail = pane_text_area_width(idx);
    SCROLLINFO sh = { sizeof(sh), SIF_RANGE | SIF_PAGE | SIF_POS };
    sh.nMin = 0;
    sh.nMax = g_state.max_line_px / unit;
    sh.nPage = avail / unit;
    sh.nPos = pane_h_scroll(idx) / unit;
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
    g_state.bookmark_count = 0;          /* 새 파일 — 일단 비우고 아래에서 영속 책갈피 로드 */
    selection_clear();
    g_state.sel_dragging = FALSE;
    autoscroll_stop();                   /* 자동 스크롤 중지 */
    wcsncpy_s(g_state.filepath, MAX_PATH, path, _TRUNCATE);
    bookmarks_load(path);

    if (!build_doc_line_index()) {
        show_error(g_state.hwnd, L"줄 인덱스 구축 실패.");
        return 0;
    }
    if (!build_render_lines()) {
        show_error(g_state.hwnd, L"렌더 라인 구축 실패.");
        return 0;
    }

    measure_max_line_width();
    update_title();
    update_scrollbars();
    status_update();
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

/* 인코딩 순환 — F2(다음) / Shift+F2(이전).
 *
 * 한국어 텍스트에서 거의 모든 케이스를 커버하는 CP949 / UTF-8 / Johab
 * 3종만 순환. UTF-16 LE/BE 와 Shift-JIS 는 빈도가 낮아 메뉴에서만 선택.
 * 현재 인코딩이 cycle 배열 밖이면 첫 항목(CP949)을 시작점으로 잡는다. */
static void cmd_cycle_encoding(BOOL forward) {
    if (!g_state.filepath[0]) return;
    static const Encoding cycle[] = {
        ENC_CP949, ENC_UTF8, ENC_JOHAB
    };
    const int n = (int)(sizeof(cycle) / sizeof(cycle[0]));
    Encoding cur = g_state.encoding;
    if (cur == ENC_UTF8_BOM) cur = ENC_UTF8;   /* BOM은 UTF-8 슬롯으로 취급 */
    int idx = 0;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (cycle[i] == cur) { idx = i; found = 1; break; }
    }
    if (found) {
        idx = forward ? (idx + 1) % n : (idx - 1 + n) % n;
    }
    /* found=0 이면 cycle 밖 인코딩(UTF-16/SJIS) — idx=0(CP949)로 진입.
     * forward/backward 동일하게 첫 항목으로 들어와야 사용자가 F2 한 번에
     * "한글 3종으로 복귀"한다는 멘탈 모델을 만족한다. */
    reload_with_encoding(cycle[idx]);
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
/* ------------------------------------------------------------------
 * 한자 음 표시 — 라인 사본에 CJK 한자만 한글 음으로 치환.
 *
 * 1글자 → 1글자 치환이라 길이/wrap/측정 모두 영향 없음.
 * 검색·선택 위치는 원본 오프셋을 그대로 쓰면 됨 (사본도 같은 인덱스).
 *
 * 반환: 사용할 wchar_t 버퍼 포인터.
 *   hanja_show=OFF면 원본 포인터 그대로 (복사 없음).
 *   hanja_show=ON이고 짧은 라인이면 stack_buf 사용, 길면 *out_alloc에 malloc 할당.
 *   호출자는 *out_alloc != NULL이면 free() 책임.
 * ------------------------------------------------------------------ */
static const wchar_t *line_render_buf(int ls, int len,
                                       wchar_t *stack_buf, int stack_cap,
                                       wchar_t **out_alloc) {
    *out_alloc = NULL;
    if (!g_state.hanja_show) return &g_state.text[ls];

    wchar_t *buf;
    if (len <= stack_cap) {
        buf = stack_buf;
    } else {
        buf = (wchar_t*)malloc((size_t)len * sizeof(wchar_t));
        if (!buf) return &g_state.text[ls];   /* 메모리 부족 시 원본 폴백 */
        *out_alloc = buf;
    }

    for (int i = 0; i < len; i++) {
        wchar_t c = g_state.text[ls + i];
        if (is_cjk(c)) {
            wchar_t h = hanja_to_hangul(c);
            buf[i] = h ? h : c;
        } else {
            buf[i] = c;
        }
    }
    return buf;
}

/* ------------------------------------------------------------------
 * 한 페인 그리기 — 분할 보기 시 두 번 호출됨.
 *
 * 좌표 기준은 클라이언트 영역 전체. 페인 인덱스로 x 좌표 오프셋과
 * top_line/h_scroll을 결정. 선택/검색 하이라이트는 전역(텍스트 오프셋
 * 기준)이라 두 페인 모두에서 동일하게 그려짐.
 * ------------------------------------------------------------------ */
static void paint_pane(HDC hdc, int pane_idx, const RECT *rcPaint) {
    const Theme *th = theme();
    int px0 = pane_x_left(pane_idx);
    int px1 = pane_x_right(pane_idx);

    RECT pr = { px0, 0, px1, g_state.client_h };
    RECT clip;
    if (!IntersectRect(&clip, &pr, rcPaint)) return;

    /* 페인 배경 */
    HBRUSH bg_brush = CreateSolidBrush(th->bg);
    FillRect(hdc, &clip, bg_brush);
    DeleteObject(bg_brush);

    if (!g_state.text || g_state.line_count == 0) {
        if (pane_idx == 0) {
            const wchar_t *msg = L"파일을 드래그하거나 Ctrl+O로 여세요.";
            HFONT old = (HFONT)SelectObject(hdc, g_state.font);
            SetTextColor(hdc, th->dim_fg);
            SetBkMode(hdc, TRANSPARENT);
            TextOutW(hdc, px0 + 20, 20, msg, (int)wcslen(msg));
            SelectObject(hdc, old);
        }
        return;
    }

    int top_line = pane_top_line(pane_idx);
    int h_scroll = pane_h_scroll(pane_idx);
    int gutter   = gutter_pixel_width();
    int lh       = line_total_height();
    int cextra   = g_state.char_spacing_extra;
    int mtop     = g_state.margin_top_px;

    HFONT old = (HFONT)SelectObject(hdc, g_state.font);
    SetBkMode(hdc, TRANSPARENT);

    /* 가시 영역 줄 범위 — 페인의 클립 기준 */
    int rel_top    = clip.top    - mtop;
    int rel_bottom = clip.bottom - mtop;
    int first = (rel_top    < 0) ? top_line : (rel_top    / lh + top_line);
    int last  = (rel_bottom < 0) ? top_line : (rel_bottom / lh + top_line + 1);
    if (first < 0) first = 0;
    if (last > g_state.line_count) last = g_state.line_count;

    /* 본문 — 페인 텍스트 영역으로 클리핑 */
    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, px0 + gutter + g_state.margin_left_px, mtop,
                      px1, g_state.client_h);
    SetTextColor(hdc, th->fg);
    SetTextCharacterExtra(hdc, cextra);
    int x0 = px0 + gutter + g_state.margin_left_px - h_scroll;

    for (int i = first; i < last; i++) {
        int y = mtop + (i - top_line) * lh;
        int start = g_state.line_offsets[i];
        int end   = g_state.line_offsets[i + 1];
        int len = end - start;

        while (len > 0) {
            wchar_t c = g_state.text[start + len - 1];
            if (c == L'\r' || c == L'\n') len--;
            else break;
        }

        if (len > 0) {
            wchar_t sbuf[256];
            wchar_t *alloc = NULL;
            const wchar_t *r = line_render_buf(start, len, sbuf, 256, &alloc);
            ExtTextOutW(hdc, x0, y, 0, NULL, r, len, NULL);
            free(alloc);
        }
    }

    /* 선택 영역 하이라이트 */
    if (has_selection()) {
        int sel_s = selection_start();
        int sel_e = selection_end();
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, th->sel_bg);
        SetTextColor(hdc, th->sel_fg);
        for (int i = first; i < last; i++) {
            int ls = g_state.line_offsets[i];
            int le = g_state.line_offsets[i + 1];
            int len_text = le - ls;
            while (len_text > 0) {
                wchar_t c = g_state.text[ls + len_text - 1];
                if (c == L'\r' || c == L'\n') len_text--;
                else break;
            }
            if (sel_e <= ls || sel_s >= ls + len_text) continue;
            int local_s = (sel_s > ls) ? (sel_s - ls) : 0;
            int local_e = (sel_e < ls + len_text) ?
                          (sel_e - ls) : len_text;
            if (local_e <= local_s) continue;

            wchar_t sbuf[256];
            wchar_t *alloc = NULL;
            const wchar_t *r = line_render_buf(ls, len_text, sbuf, 256, &alloc);

            SIZE pre_sz, sel_sz;
            GetTextExtentPoint32W(hdc, r, local_s, &pre_sz);
            GetTextExtentPoint32W(hdc, &r[local_s],
                                  local_e - local_s, &sel_sz);
            int pre_w = pre_sz.cx + local_s * cextra;
            int sel_w = sel_sz.cx + (local_e - local_s) * cextra;
            int sx = x0 + pre_w;
            int sy = mtop + (i - top_line) * lh;
            RECT rs;
            rs.left   = sx;
            rs.top    = sy;
            rs.right  = sx + sel_w;
            rs.bottom = sy + lh;
            ExtTextOutW(hdc, sx, sy, ETO_OPAQUE, &rs,
                        &r[local_s], local_e - local_s, NULL);
            free(alloc);
        }
        SetBkMode(hdc, TRANSPARENT);
    }

    /* 검색 매칭 하이라이트 */
    if (g_state.search_match_pos >= 0 && g_state.search_needle_len > 0) {
        int mp = g_state.search_match_pos;
        int mline = line_for_offset(mp);
        if (mline >= first && mline < last) {
            int ls = g_state.line_offsets[mline];
            int le = g_state.line_offsets[mline + 1];
            int mlen = g_state.search_needle_len;
            if (mp >= ls && mp + mlen <= le) {
                int line_len = le - ls;
                wchar_t sbuf[256];
                wchar_t *alloc = NULL;
                const wchar_t *r = line_render_buf(ls, line_len,
                                                    sbuf, 256, &alloc);
                SIZE pre, mw;
                GetTextExtentPoint32W(hdc, r, mp - ls, &pre);
                GetTextExtentPoint32W(hdc, &r[mp - ls], mlen, &mw);
                int pre_cx = pre.cx + (mp - ls) * cextra;
                int mw_cx  = mw.cx  + mlen * cextra;
                int my = mtop + (mline - top_line) * lh;
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
                            &r[mp - ls], mlen, NULL);
                SetBkMode(hdc, TRANSPARENT);
                free(alloc);
            }
        }
    }

    RestoreDC(hdc, saved);

    /* 거터 — 페인 좌측에 덮어 그림 */
    if (gutter > 0) {
        RECT gr;
        gr.left   = px0;
        gr.top    = clip.top;
        gr.right  = px0 + gutter;
        gr.bottom = clip.bottom;
        HBRUSH gb = CreateSolidBrush(th->gutter_bg);
        FillRect(hdc, &gr, gb);
        DeleteObject(gb);

        SetTextCharacterExtra(hdc, 0);
        int unit = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
        for (int i = first; i < last; i++) {
            int doc = g_state.render_to_doc[i];
            BOOL first_render =
                (i == 0) || (g_state.render_to_doc[i - 1] != doc);
            int y = mtop + (i - top_line) * lh;
            if (!first_render) continue;

            wchar_t numbuf[16];
            int len = _snwprintf_s(numbuf, 16, _TRUNCATE, L"%d", doc + 1);
            SIZE sz;
            GetTextExtentPoint32W(hdc, numbuf, len, &sz);
            int x = px0 + gutter - sz.cx - unit;
            if (x < px0) x = px0;
            SetTextColor(hdc, bookmark_has(doc) ? th->bm_fg : th->gutter_fg);
            ExtTextOutW(hdc, x, y, 0, NULL, numbuf, len, NULL);
        }
    }

    SelectObject(hdc, old);
}

static void on_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    paint_pane(hdc, 0, &ps.rcPaint);

    if (g_state.split_active) {
        const Theme *th = theme();
        int mid  = g_state.client_w / 2;
        int half = SPLIT_DIVIDER_PX / 2;

        /* 디바이더 — active 페인 쪽 절반을 강조색으로 칠해 표시 */
        RECT dr_left  = { mid - half, 0, mid,         g_state.client_h };
        RECT dr_right = { mid,        0, mid + half,  g_state.client_h };
        RECT clip;
        HBRUSH base = CreateSolidBrush(th->gutter_bg);
        HBRUSH accent = CreateSolidBrush(th->bm_fg);
        if (IntersectRect(&clip, &dr_left, &ps.rcPaint))
            FillRect(hdc, &clip, g_state.active_pane == 0 ? accent : base);
        if (IntersectRect(&clip, &dr_right, &ps.rcPaint))
            FillRect(hdc, &clip, g_state.active_pane == 1 ? accent : base);
        DeleteObject(base);
        DeleteObject(accent);

        paint_pane(hdc, 1, &ps.rcPaint);
    }

    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------
 * 스크롤 처리
 * ------------------------------------------------------------------ */
static void scroll_to_line(int line) {
    int idx = g_state.active_pane;
    if (line < 0) line = 0;
    int max_top = g_state.line_count - g_state.visible_lines;
    if (max_top < 0) max_top = 0;
    if (line > max_top) line = max_top;

    int cur = pane_top_line(idx);
    int delta = line - cur;
    if (delta == 0) return;

    set_pane_top_line(idx, line);
    /* ScrollWindowEx로 부드럽게 — 활성 페인 영역만 다시 그림.
     * 상단 여백은 고정이어야 하므로 mtop 아래만 스크롤. */
    RECT rc;
    rc.left   = pane_x_left(idx);
    rc.top    = g_state.margin_top_px;
    rc.right  = pane_x_right(idx);
    rc.bottom = g_state.client_h;
    ScrollWindowEx(g_state.hwnd, 0, -delta * line_total_height(),
                   &rc, &rc, NULL, NULL,
                   SW_INVALIDATE | SW_ERASE);
    update_scrollbars();
    status_update();
}

static void scroll_h_to(int px) {
    int idx = g_state.active_pane;
    if (px < 0) px = 0;
    int max_h = g_state.max_line_px - pane_text_area_width(idx);
    if (max_h < 0) max_h = 0;
    if (px > max_h) px = max_h;

    int cur = pane_h_scroll(idx);
    int delta = px - cur;
    if (delta == 0) return;

    set_pane_h_scroll(idx, px);
    /* 거터/좌측 여백은 가로 스크롤 시 고정. 텍스트 영역만 ScrollWindowEx. */
    int gutter = gutter_pixel_width();
    RECT rc;
    rc.left   = pane_x_left(idx) + gutter + g_state.margin_left_px;
    rc.top    = 0;
    rc.right  = pane_x_right(idx);
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
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        g_state.visible_lines = g_state.client_h / line_total_height();
        measure_max_line_width();
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
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
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        measure_max_line_width();
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
}

/* 여백 — Alt+방향키. 최대 client 크기의 1/4까지 허용. */
static void margin_change_left(int delta_px) {
    int v = g_state.margin_left_px + delta_px;
    int maxv = g_state.client_w / 4;
    if (v < 0) v = 0;
    if (v > maxv) v = maxv;
    if (v == g_state.margin_left_px) return;
    g_state.margin_left_px = v;
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        scroll_h_to(g_state.h_scroll_px);
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
}

static void margin_change_top(int delta_px) {
    int v = g_state.margin_top_px + delta_px;
    int maxv = g_state.client_h / 4;
    if (v < 0) v = 0;
    if (v > maxv) v = maxv;
    if (v == g_state.margin_top_px) return;
    g_state.margin_top_px = v;
    int avail_h = g_state.client_h - v;
    g_state.visible_lines = avail_h > 0 ? avail_h / line_total_height() : 1;
    if (g_state.visible_lines < 1) g_state.visible_lines = 1;
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 텍스트 선택 — 마우스 드래그 기반.
 *
 * sel_anchor/sel_caret 모두 text 오프셋. anchor==caret이면 선택 없음.
 * 화면 표시: 두 번째 패스로 선택 영역만 hl 색으로 덮어 그림.
 * ------------------------------------------------------------------ */
static int  selection_start(void) {
    int a = g_state.sel_anchor, c = g_state.sel_caret;
    return a < c ? a : c;
}
static int  selection_end(void) {
    int a = g_state.sel_anchor, c = g_state.sel_caret;
    return a > c ? a : c;
}
static BOOL has_selection(void) {
    return g_state.sel_anchor >= 0 && g_state.sel_caret >= 0 &&
           g_state.sel_anchor != g_state.sel_caret;
}
static void selection_clear(void) {
    g_state.sel_anchor = -1;
    g_state.sel_caret  = -1;
}

/* ------------------------------------------------------------------
 * 키보드 텍스트 선택 — Shift+방향키, Shift+Home/End, Shift+Ctrl+Home/End.
 *
 * 모델: sel_caret이 캐럿(이동하는 끝). sel_anchor가 고정점.
 * 처음 Shift+방향 누를 때 anchor=caret=현재 보이는 첫 줄 시작 으로 초기화.
 * 이후 caret만 이동. anchor==caret이면 selection_clear() 호출 효과.
 *
 * Up/Down은 column-preserving 없이 단순히 같은 offset(라인 내 위치)을
 * 새 라인에 적용 — wrap/varying-length에서 살짝 어긋날 수 있지만
 * 한국어/CJK 위주 텍스트에서는 받아들일 만한 절충.
 * ------------------------------------------------------------------ */
static void kb_sel_ensure_init(void) {
    if (g_state.sel_anchor >= 0 && g_state.sel_caret >= 0) return;
    /* 보이는 첫 줄의 시작 — viewport top */
    int line = pane_top_line(g_state.active_pane);
    if (line < 0) line = 0;
    if (line > g_state.line_count) line = g_state.line_count;
    int off = (g_state.line_offsets && g_state.line_count > 0) ?
              g_state.line_offsets[line] : 0;
    g_state.sel_anchor = off;
    g_state.sel_caret  = off;
}

/* caret 위치가 화면에 보이도록 viewport 스크롤 — 분할 시 활성 페인. */
static void kb_sel_scroll_to_caret(void) {
    if (g_state.sel_caret < 0 || g_state.line_count == 0) return;
    int line = line_for_offset(g_state.sel_caret);
    int top = pane_top_line(g_state.active_pane);
    if (line < top) {
        scroll_to_line(line);
    } else if (line >= top + g_state.visible_lines) {
        scroll_to_line(line - g_state.visible_lines + 1);
    }
}

/* 라인 내 컨텐츠 끝(줄 종결자 직전) — Shift+End용 */
static int line_content_end(int line) {
    if (line < 0 || line >= g_state.line_count) return 0;
    int s = g_state.line_offsets[line];
    int e = g_state.line_offsets[line + 1];
    while (e > s) {
        wchar_t c = g_state.text[e - 1];
        if (c == L'\r' || c == L'\n') e--;
        else break;
    }
    return e;
}

/* dir: -1 한 글자 왼쪽, +1 한 글자 오른쪽,
 *      -2 위 라인, +2 아래 라인,
 *      -3 라인 시작, +3 라인 끝(컨텐츠),
 *      -4 문서 시작, +4 문서 끝 */
static void kb_sel_extend(int dir) {
    if (g_state.text_len == 0 || g_state.line_count == 0) return;
    kb_sel_ensure_init();

    int n = (int)g_state.text_len;
    int caret = g_state.sel_caret;
    int line  = line_for_offset(caret);
    int ls    = g_state.line_offsets[line];

    int new_caret = caret;
    switch (dir) {
    case -1: new_caret = caret > 0 ? caret - 1 : 0; break;
    case +1: new_caret = caret < n ? caret + 1 : n; break;
    case -2:
        if (line > 0) {
            int col = caret - ls;
            int prev_s = g_state.line_offsets[line - 1];
            int prev_end = line_content_end(line - 1);
            int target = prev_s + col;
            if (target > prev_end) target = prev_end;
            new_caret = target;
        }
        break;
    case +2:
        if (line + 1 < g_state.line_count) {
            int col = caret - ls;
            int next_s = g_state.line_offsets[line + 1];
            int next_end = line_content_end(line + 1);
            int target = next_s + col;
            if (target > next_end) target = next_end;
            new_caret = target;
        } else {
            new_caret = n;
        }
        break;
    case -3: new_caret = ls; break;
    case +3: new_caret = line_content_end(line); break;
    case -4: new_caret = 0; break;
    case +4: new_caret = n; break;
    }

    g_state.sel_caret = new_caret;
    /* anchor == caret이면 has_selection()이 자연히 FALSE 반환 — 클리어
     * 안 하고 둠으로써 다음 Shift+방향키가 동일 지점을 anchor로 유지. */
    kb_sel_scroll_to_caret();
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

/* (line, x_target) → text 오프셋 — GetTextExtentExPointW로 누적 폭 산출 후
 * 가장 가까운 글자 경계 선택. char_spacing_extra는 수동 보정. */
static int line_offset_at_x(int line, int x_target) {
    if (line < 0) line = 0;
    if (line >= g_state.line_count) line = g_state.line_count - 1;
    int ls = g_state.line_offsets[line];
    int le = g_state.line_offsets[line + 1];
    int len = le - ls;
    while (len > 0) {
        wchar_t c = g_state.text[ls + len - 1];
        if (c == L'\r' || c == L'\n') len--;
        else break;
    }
    if (x_target <= 0 || len == 0) return ls;

    HDC hdc = GetDC(g_state.hwnd);
    HFONT old = (HFONT)SelectObject(hdc, g_state.font);
    int cextra = g_state.char_spacing_extra;

    INT *widths = (INT*)malloc((size_t)len * sizeof(INT));
    if (!widths) {
        SelectObject(hdc, old);
        ReleaseDC(g_state.hwnd, hdc);
        return ls + len;
    }
    int fit = 0;
    SIZE sz;
    /* hanja_show 모드면 화면에 그려진 한글 사본 기준으로 측정해야
     * 클릭 위치가 시각적으로 일치 */
    wchar_t sbuf[256];
    wchar_t *alloc = NULL;
    const wchar_t *measure = line_render_buf(ls, len, sbuf, 256, &alloc);
    GetTextExtentExPointW(hdc, measure, len, INT_MAX,
                          &fit, widths, &sz);
    free(alloc);

    int col = len;  /* 끝까지 다 지나면 줄 끝 */
    for (int i = 0; i < len; i++) {
        int x_after  = widths[i] + (i + 1) * cextra;
        int x_before = (i == 0) ? 0 : (widths[i - 1] + i * cextra);
        if (x_after > x_target) {
            int mid = (x_before + x_after) / 2;
            col = (x_target < mid) ? i : (i + 1);
            break;
        }
    }
    free(widths);
    SelectObject(hdc, old);
    ReleaseDC(g_state.hwnd, hdc);
    return ls + col;
}

static int offset_at_mouse(int mx, int my) {
    if (g_state.line_count == 0) return 0;
    int pane = pane_at_x(mx);
    if (pane < 0) pane = g_state.active_pane;   /* 디바이더 위 — 현재 페인 */

    int rel_y = my - g_state.margin_top_px;
    if (rel_y < 0) rel_y = 0;
    int line = rel_y / line_total_height() + pane_top_line(pane);
    if (line < 0) line = 0;
    if (line >= g_state.line_count) line = g_state.line_count - 1;

    int x_target = mx - pane_x_left(pane) - gutter_pixel_width()
                 - g_state.margin_left_px + pane_h_scroll(pane);
    return line_offset_at_x(line, x_target);
}

static void cmd_select_all(void) {
    if (g_state.text_len == 0) return;
    g_state.sel_anchor = 0;
    g_state.sel_caret  = (int)g_state.text_len;
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

static void cmd_copy(void) {
    if (!has_selection()) return;
    int s = selection_start();
    int e = selection_end();
    size_t n = (size_t)(e - s);
    if (!OpenClipboard(g_state.hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t *p = (wchar_t*)GlobalLock(h);
        if (p) {
            memcpy(p, &g_state.text[s], n * sizeof(wchar_t));
            p[n] = 0;
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
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
 * 옵션:
 *   - search_case_sensitive: ASCII는 빠른 경로, 그 외엔 towlower 정규화
 *   - search_whole_word: 매칭 직전/직후 글자가 단어 문자가 아니어야 함
 *
 * 알고리즘: m≥2면 Boyer-Moore-Horspool, m==1은 단순 루프.
 * 텍스트 끝에 도달하면 호출자가 처음부터 wrap-around 재시도.
 * BMP 외 surrogate pair 정확도 부족 (한글 영역엔 영향 없음).
 * ------------------------------------------------------------------ */
static inline wchar_t search_fold(wchar_t c) {
    if (g_state.search_case_sensitive) return c;
    if (c < 0x80) return (wchar_t)((c >= L'A' && c <= L'Z') ? c + 32 : c);
    return (wchar_t)towlower(c);
}

/* 단어 문자 — ASCII 영숫자/언더스코어 + CJK 범위 + 한글.
 * 단어 단위 검색의 경계 판정에만 사용. */
static int is_word_char(wchar_t c) {
    if ((c >= L'0' && c <= L'9') ||
        (c >= L'A' && c <= L'Z') ||
        (c >= L'a' && c <= L'z') ||
        c == L'_') return 1;
    if (c >= 0xAC00 && c <= 0xD7A3) return 1;        /* Hangul Syllables */
    if (c >= 0x4E00 && c <= 0x9FFF) return 1;        /* CJK */
    if (c >= 0x3400 && c <= 0x4DBF) return 1;        /* CJK Ext A */
    if (c >= 0x3040 && c <= 0x30FF) return 1;        /* Kana */
    return 0;
}

static int word_boundary_ok(int pos, int m) {
    int n = (int)g_state.text_len;
    if (pos > 0 && is_word_char(g_state.text[pos - 1]) &&
                   is_word_char(g_state.text[pos])) return 0;
    if (pos + m < n && is_word_char(g_state.text[pos + m - 1]) &&
                       is_word_char(g_state.text[pos + m])) return 0;
    return 1;
}

/* 검색 도중 Esc로 취소 요청이 있는지 폴링.
 * 64MB 선형 스캔 중 UI가 굳어 보이지 않게, 일정 간격으로 메시지 펌프를
 * 돌리고 VK_ESCAPE 키 다운만 골라 소비. 한번 취소 요청이 들어오면
 * search_canceled=TRUE로 끊어 호출자가 cleanup 메시지를 띄우게 한다. */
static void search_cancel_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            g_state.search_canceled = TRUE;
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

static int find_substr_offset(int start, BOOL forward) {
    if (g_state.search_needle_len == 0 || g_state.text_len == 0) return -1;
    int n = (int)g_state.text_len;
    int m = g_state.search_needle_len;
    if (m > n) return -1;
    const wchar_t *text = g_state.text;

    /* needle도 옵션에 맞게 한 번 정규화해 둠 */
    wchar_t fneedle[256];
    for (int i = 0; i < m; i++) fneedle[i] = search_fold(g_state.search_needle[i]);
    BOOL want_word = g_state.search_whole_word;
    /* 1M 글자마다 메시지 펌프/취소 검사. 64MB 파일도 ~64회면 충분하고
     * 폴링 비용은 무시할 수준 (~µs). */
    const int POLL_EVERY = 1 << 20;
    int next_poll = (forward ? start : start) + (forward ? POLL_EVERY : -POLL_EVERY);

    if (m == 1) {
        wchar_t c = fneedle[0];
        if (forward) {
            if (start < 0) start = 0;
            for (int i = start; i < n; i++) {
                if (i >= next_poll) {
                    search_cancel_pump();
                    if (g_state.search_canceled) return -1;
                    next_poll = i + POLL_EVERY;
                }
                if (search_fold(text[i]) == c &&
                    (!want_word || word_boundary_ok(i, m))) return i;
            }
        } else {
            if (start > n - 1) start = n - 1;
            for (int i = start; i >= 0; i--) {
                if (i <= next_poll) {
                    search_cancel_pump();
                    if (g_state.search_canceled) return -1;
                    next_poll = i - POLL_EVERY;
                }
                if (search_fold(text[i]) == c &&
                    (!want_word || word_boundary_ok(i, m))) return i;
            }
        }
        return -1;
    }

    /* BMH 시프트 테이블 — fold된 needle 기준으로 빌드. 충돌은 안전. */
    int shift[256];
    for (int i = 0; i < 256; i++) shift[i] = m;

    if (forward) {
        if (start < 0) start = 0;
        for (int i = 0; i < m - 1; i++)
            shift[(unsigned)fneedle[i] & 0xFF] = m - 1 - i;
        int i = start;
        while (i <= n - m) {
            if (i >= next_poll) {
                search_cancel_pump();
                if (g_state.search_canceled) return -1;
                next_poll = i + POLL_EVERY;
            }
            wchar_t c = search_fold(text[i + m - 1]);
            if (c == fneedle[m - 1]) {
                int k = m - 2;
                while (k >= 0 && search_fold(text[i + k]) == fneedle[k]) k--;
                if (k < 0 && (!want_word || word_boundary_ok(i, m))) return i;
            }
            i += shift[(unsigned)c & 0xFF];
        }
    } else {
        if (start > n - m) start = n - m;
        for (int i = 1; i < m; i++)
            shift[(unsigned)fneedle[i] & 0xFF] = i;
        int i = start;
        while (i >= 0) {
            if (i <= next_poll) {
                search_cancel_pump();
                if (g_state.search_canceled) return -1;
                next_poll = i - POLL_EVERY;
            }
            wchar_t c = search_fold(text[i]);
            if (c == fneedle[0]) {
                int k = 1;
                while (k < m && search_fold(text[i + k]) == fneedle[k]) k++;
                if (k == m && (!want_word || word_boundary_ok(i, m))) return i;
            }
            i -= shift[(unsigned)c & 0xFF];
        }
    }
    return -1;
}

static void search_jump_to(int offset) {
    g_state.search_match_pos = offset;
    int line = line_for_offset(offset);
    /* 매칭 줄이 화면 밖이거나 가장자리면 중앙으로 가져옴.
     * 분할 보기에서는 활성 페인 기준으로 가시성 판정/스크롤. */
    int top = pane_top_line(g_state.active_pane);
    if (line < top || line >= top + g_state.visible_lines) {
        int target = line - g_state.visible_lines / 2;
        if (target < 0) target = 0;
        scroll_to_line(target);
    }
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

/* search_history_*는 cmd_find 등에서 호출되지만 정의는 한참 아래. */
static void search_history_add(const wchar_t *needle);
static void search_history_rebuild_menu(void);

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
    search_history_add(buf);
    search_history_rebuild_menu();

    /* 화면 윗줄부터 정방향 검색 → 못 찾으면 처음부터 wrap.
     * 분할 보기에서는 활성 페인의 top_line을 기준으로. */
    g_state.search_canceled = FALSE;
    int start = g_state.line_offsets[pane_top_line(g_state.active_pane)];
    int pos = find_substr_offset(start, TRUE);
    if (pos < 0 && !g_state.search_canceled) pos = find_substr_offset(0, TRUE);
    if (pos < 0) {
        const wchar_t *msg = g_state.search_canceled ?
            L"검색이 취소되었습니다." : L"찾는 문자열이 없습니다.";
        MessageBoxW(g_state.hwnd, msg,
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        g_state.search_match_pos = -1;
        InvalidateRect(g_state.hwnd, NULL, FALSE);
        return;
    }
    search_jump_to(pos);
}

/* 히스토리 항목으로 즉시 재검색 (메뉴에서 호출) */
static void cmd_find_from_history(int idx) {
    if (idx < 0 || idx >= g_state.search_history_count) return;
    if (g_state.text_len == 0) return;
    wcsncpy_s(g_state.search_needle, 256,
              g_state.search_history[idx], _TRUNCATE);
    g_state.search_needle_len = (int)wcslen(g_state.search_needle);
    /* 선택한 항목을 맨 앞으로 — 사용자 의도에 맞게 */
    search_history_add(g_state.search_history[idx]);
    search_history_rebuild_menu();

    g_state.search_canceled = FALSE;
    int start = g_state.line_offsets[pane_top_line(g_state.active_pane)];
    int pos = find_substr_offset(start, TRUE);
    if (pos < 0 && !g_state.search_canceled) pos = find_substr_offset(0, TRUE);
    if (pos < 0) {
        const wchar_t *msg = g_state.search_canceled ?
            L"검색이 취소되었습니다." : L"찾는 문자열이 없습니다.";
        MessageBoxW(g_state.hwnd, msg,
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
    g_state.search_canceled = FALSE;
    int pos = find_substr_offset(start, forward);
    /* wrap-around — 취소된 경우는 wrap 안 함 */
    if (pos < 0 && !g_state.search_canceled) {
        pos = find_substr_offset(forward ? 0 : (int)g_state.text_len - 1,
                                 forward);
    }
    if (pos < 0) {
        const wchar_t *msg = g_state.search_canceled ?
            L"검색이 취소되었습니다." : L"찾는 문자열이 없습니다.";
        MessageBoxW(g_state.hwnd, msg,
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    search_jump_to(pos);
}

/* ------------------------------------------------------------------
 * 마크다운 목차 (Ctrl+T) — `# ... ######` 헤딩을 추출하여 팝업 메뉴.
 *
 * 일반 텍스트의 휴리스틱(빈 줄+짧은 줄)은 후속 phase. 일단 마크다운만.
 * 메뉴 항목 ID는 IDM_TOC_BASE + index로 일회성 사용.
 * ------------------------------------------------------------------ */
static void cmd_show_toc(void) {
    if (g_state.line_count == 0 || g_state.text_len == 0) return;

    HMENU menu = CreatePopupMenu();
    int count = 0;

    /* doc 라인 순회 — wrap 모드에서도 헤딩이 한 번만 잡힘 */
    for (int i = 0; i < g_state.doc_line_count && count < 1000; i++) {
        int ls = g_state.doc_line_offsets[i];
        int le = g_state.doc_line_offsets[i + 1];
        if (ls >= le) continue;
        if (g_state.text[ls] != L'#') continue;

        /* 연속 # 카운트 — 1~6 */
        int level = 0, pos = ls;
        while (pos < le && g_state.text[pos] == L'#' && level < 6) {
            level++;
            pos++;
        }
        if (level == 0) continue;

        /* # 뒤에는 공백/줄끝이어야 헤딩 — `#word`는 헤딩 아님 */
        if (pos < le) {
            wchar_t c = g_state.text[pos];
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') continue;
        }
        while (pos < le &&
               (g_state.text[pos] == L' ' || g_state.text[pos] == L'\t')) {
            pos++;
        }

        int text_end = le;
        while (text_end > pos) {
            wchar_t c = g_state.text[text_end - 1];
            if (c == L'\r' || c == L'\n') text_end--;
            else break;
        }

        /* 라벨: 들여쓰기(레벨-1)*2 공백 + 본문(최대 200자) */
        wchar_t label[256];
        int indent = (level - 1) * 2;
        if (indent > 16) indent = 16;
        for (int j = 0; j < indent; j++) label[j] = L' ';
        int max_text = 255 - indent - 1;
        int text_len = text_end - pos;
        if (text_len > max_text) text_len = max_text;
        if (text_len < 0) text_len = 0;
        memcpy(&label[indent], &g_state.text[pos],
               (size_t)text_len * sizeof(wchar_t));
        label[indent + text_len] = 0;

        /* 빈 헤딩이면 placeholder */
        if (text_len == 0) {
            wcscpy_s(&label[indent], 256 - indent, L"(제목 없음)");
        }

        AppendMenuW(menu, MF_STRING,
                    (UINT_PTR)(IDM_TOC_BASE + count), label);
        /* 메뉴 항목과 줄 번호 매핑 — itemData에 line 저장 */
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_DATA;
        mii.dwItemData = (ULONG_PTR)i;
        SetMenuItemInfoW(menu, (UINT)(IDM_TOC_BASE + count), FALSE, &mii);
        count++;
    }

    if (count == 0) {
        DestroyMenu(menu);
        MessageBoxW(g_state.hwnd,
            L"마크다운 헤딩(# ...)을 찾을 수 없습니다.",
            APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* 팝업 위치 — 키보드 호출이면 클라이언트 좌상단 근처로 */
    POINT pt = { 50, 50 };
    ClientToScreen(g_state.hwnd, &pt);

    UINT cmd = (UINT)TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, g_state.hwnd, NULL);

    if (cmd >= (UINT)IDM_TOC_BASE) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_DATA;
        if (GetMenuItemInfoW(menu, cmd, FALSE, &mii)) {
            int doc = (int)mii.dwItemData;
            if (doc >= 0 && doc < g_state.doc_line_count) {
                scroll_to_line(g_state.doc_to_render[doc]);
            }
        }
    }
    DestroyMenu(menu);
}

/* ------------------------------------------------------------------
 * 줄 이동 (Ctrl+G)
 *
 * 1-기반 줄 번호로 입력 받음. 음수/0/초과는 클램프.
 * ------------------------------------------------------------------ */
static void cmd_goto_line(void) {
    if (g_state.doc_line_count == 0) return;
    wchar_t buf[16] = {0};
    if (prompt_input(g_state.hwnd, L"줄 이동", L"줄 번호:",
                     TRUE, buf, 16)) {
        int line = _wtoi(buf);   /* doc 라인 1-기반 */
        if (line < 1) line = 1;
        if (line > g_state.doc_line_count) line = g_state.doc_line_count;
        scroll_to_line(g_state.doc_to_render[line - 1]);
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
    /* 책갈피는 doc 라인 기준 — wrap 토글에도 의미 유지 */
    int line = g_state.render_to_doc[g_state.top_line];
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
    bookmarks_save(g_state.filepath);
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

static void cmd_bookmark_jump(BOOL forward) {
    if (g_state.bookmark_count == 0 || g_state.line_count == 0) return;
    int curr_doc = g_state.render_to_doc[g_state.top_line];
    int target_doc = -1;
    if (forward) {
        for (int i = 0; i < g_state.bookmark_count; i++) {
            if (g_state.bookmarks[i] > curr_doc) {
                target_doc = g_state.bookmarks[i];
                break;
            }
        }
        if (target_doc < 0) target_doc = g_state.bookmarks[0];
    } else {
        for (int i = g_state.bookmark_count - 1; i >= 0; i--) {
            if (g_state.bookmarks[i] < curr_doc) {
                target_doc = g_state.bookmarks[i];
                break;
            }
        }
        if (target_doc < 0)
            target_doc = g_state.bookmarks[g_state.bookmark_count - 1];
    }
    if (target_doc >= 0 && target_doc < g_state.doc_line_count) {
        scroll_to_line(g_state.doc_to_render[target_doc]);
    }
}

static void cmd_bookmark_clear(void) {
    if (g_state.bookmark_count == 0) return;
    g_state.bookmark_count = 0;
    bookmarks_save(g_state.filepath);
    InvalidateRect(g_state.hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------
 * 폰트 종류 변경 — Win32 표준 ChooseFontW 다이얼로그.
 *
 * charset은 항상 HANGUL_CHARSET로 강제하므로 사용자는 face/weight/
 * italic/size만 선택. 결과는 g_state.font_face/weight/italic/size에
 * 반영하고 create_font 재호출.
 * ------------------------------------------------------------------ */
static void cmd_choose_font(void) {
    HDC hdc = GetDC(g_state.hwnd);
    int dpi_y = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(g_state.hwnd, hdc);

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight  = -MulDiv(g_state.font_size, dpi_y, 72);
    lf.lfWeight  = g_state.font_weight ? g_state.font_weight : FW_NORMAL;
    lf.lfItalic  = g_state.font_italic;
    lf.lfCharSet = HANGUL_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE,
             g_state.font_face[0] ? g_state.font_face : L"D2Coding");

    CHOOSEFONTW cf;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = g_state.hwnd;
    cf.lpLogFont   = &lf;
    cf.iPointSize  = g_state.font_size * 10;
    cf.Flags       = CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST |
                     CF_NOSCRIPTSEL | CF_TTONLY | CF_SCREENFONTS;

    if (!ChooseFontW(&cf)) return;

    wcscpy_s(g_state.font_face, LF_FACESIZE, lf.lfFaceName);
    g_state.font_weight = lf.lfWeight;
    g_state.font_italic = lf.lfItalic;
    int pt = cf.iPointSize / 10;
    if (pt < 6)  pt = 6;
    if (pt > 72) pt = 72;
    g_state.font_size = pt;

    create_font();
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        int avail = g_state.client_h - g_state.margin_top_px;
        if (avail < line_total_height()) avail = line_total_height();
        g_state.visible_lines = avail / line_total_height();
        measure_max_line_width();
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
}

/* ------------------------------------------------------------------
 * 다크 모드 토글
 *
 * 사용자 bg/fg는 토글마다 프리셋 값으로 재설정 — 라이트↔다크가 직관적.
 * (사용자가 그 후 메뉴로 다시 색을 바꾸면 그 값이 유지됨.)
 * ------------------------------------------------------------------ */
static void cmd_toggle_dark_mode(void) {
    g_state.dark_mode = !g_state.dark_mode;
    g_state.user_bg = g_state.dark_mode ? THEME_DARK.bg : THEME_LIGHT.bg;
    g_state.user_fg = g_state.dark_mode ? THEME_DARK.fg : THEME_LIGHT.fg;
    theme_save();

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
 * 사용자 색 선택 (보기 → 배경색/글자색).
 *
 * COMDLG32의 ChooseColorW로 표준 색 선택 다이얼로그 표시.
 * 사용자 정의 색 16개는 정적 배열에 보관 (다이얼로그 간 유지).
 * 확정 시 즉시 theme.txt에 저장 후 화면 갱신.
 * ------------------------------------------------------------------ */
/* kind: 0=user_bg, 1=user_fg, 2=user_hl_bg, 3=user_hl_fg */
static void cmd_choose_color(int kind) {
    static COLORREF custom_colors[16] = {
        RGB(255,255,255), RGB(  0,  0,  0), RGB(240,240,240), RGB( 30, 30, 30),
        RGB(220,220,220), RGB(180,210,255), RGB(255,230, 80), RGB(255,255,255),
        RGB(255,255,255), RGB(255,255,255), RGB(255,255,255), RGB(255,255,255),
        RGB(255,255,255), RGB(255,255,255), RGB(255,255,255), RGB(255,255,255)
    };
    /* hl 색이 아직 미설정이면 현재 프리셋 값을 초기치로 — 다이얼로그가
     * 의미 있는 색에서 시작하도록 (검정 시작 방지). */
    COLORREF init;
    if      (kind == 0) init = g_state.user_bg;
    else if (kind == 1) init = g_state.user_fg;
    else if (kind == 2) init = g_state.user_hl_set ? g_state.user_hl_bg : theme()->hl_bg;
    else                init = g_state.user_hl_set ? g_state.user_hl_fg : theme()->hl_fg;

    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner    = g_state.hwnd;
    cc.lpCustColors = custom_colors;
    cc.rgbResult    = init;
    cc.Flags        = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;

    if (!ChooseColorW(&cc)) return;

    if      (kind == 0) g_state.user_bg = cc.rgbResult;
    else if (kind == 1) g_state.user_fg = cc.rgbResult;
    else {
        /* hl_bg/hl_fg는 한 번이라도 설정되면 둘 다 user 값으로 고정.
         * 미설정인 다른 한쪽은 현재 프리셋 색으로 채워둠. */
        if (!g_state.user_hl_set) {
            const Theme *t = theme();
            g_state.user_hl_bg = t->hl_bg;
            g_state.user_hl_fg = t->hl_fg;
        }
        if (kind == 2) g_state.user_hl_bg = cc.rgbResult;
        else           g_state.user_hl_fg = cc.rgbResult;
        g_state.user_hl_set = 1;
    }
    theme_save();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

static void cmd_theme_reset(void) {
    g_state.user_bg = HVIEW_THEME_DEFAULT_BG;
    g_state.user_fg = HVIEW_THEME_DEFAULT_FG;
    theme_save();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

static void cmd_theme_hl_reset(void) {
    g_state.user_hl_set = 0;
    g_state.user_hl_bg  = 0;
    g_state.user_hl_fg  = 0;
    theme_save();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 자동 줄바꿈 토글 — top 위치를 doc 기준으로 보존하며 재구축.
 *
 * 윈도우 크기 변경, 폰트/자간/여백 변경 시에도 재호출됨
 * (wrap 모드일 때만 재구축이 필요하므로 wrap_mode 체크는 호출자에서).
 * ------------------------------------------------------------------ */
static void rebuild_render_lines_preserve(void) {
    /* 두 페인의 top_line을 doc 좌표로 변환해 보존 — render line index는
     * wrap 재계산으로 바뀌지만 doc line index는 안정적이라 안전. */
    int top_doc1 = 0, top_doc2 = 0;
    if (g_state.line_count > 0 && g_state.render_to_doc) {
        int t1 = g_state.top_line;
        int t2 = g_state.top_line2;
        if (t1 < 0) t1 = 0;
        if (t1 >= g_state.line_count) t1 = g_state.line_count - 1;
        if (t2 < 0) t2 = 0;
        if (t2 >= g_state.line_count) t2 = g_state.line_count - 1;
        top_doc1 = g_state.render_to_doc[t1];
        top_doc2 = g_state.render_to_doc[t2];
    }

    if (!build_render_lines()) return;

    if (g_state.line_count > 0) {
        if (top_doc1 < 0) top_doc1 = 0;
        if (top_doc1 >= g_state.doc_line_count)
            top_doc1 = g_state.doc_line_count - 1;
        if (top_doc2 < 0) top_doc2 = 0;
        if (top_doc2 >= g_state.doc_line_count)
            top_doc2 = g_state.doc_line_count - 1;
        g_state.top_line  = g_state.doc_to_render[top_doc1];
        g_state.top_line2 = g_state.doc_to_render[top_doc2];
    } else {
        g_state.top_line  = 0;
        g_state.top_line2 = 0;
    }

    /* visible_lines, scrollbar 갱신 */
    int avail = g_state.client_h - g_state.margin_top_px;
    if (avail < line_total_height()) avail = line_total_height();
    g_state.visible_lines = avail / line_total_height();

    measure_max_line_width();
    /* h_scroll 클램프 (wrap on 시 max_line_px=0) */
    if (g_state.h_scroll_px  > 0) g_state.h_scroll_px  = 0;
    if (g_state.h_scroll_px2 > 0) g_state.h_scroll_px2 = 0;
    update_scrollbars();
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

static void cmd_toggle_wrap(void) {
    g_state.wrap_mode = !g_state.wrap_mode;
    HMENU menu = GetMenu(g_state.hwnd);
    if (!menu) menu = g_state.fs_menu;
    if (menu) {
        CheckMenuItem(menu, IDM_WRAP,
                      MF_BYCOMMAND | (g_state.wrap_mode ?
                                      MF_CHECKED : MF_UNCHECKED));
    }
    rebuild_render_lines_preserve();
}

/* ------------------------------------------------------------------
 * 한자 음 표시 토글 (보기 메뉴).
 *
 * 원본 텍스트는 그대로, 렌더 직전에만 1:1로 한자→한글 치환.
 * 검색/선택은 원본 기준이라 영향 없음. wrap도 cell width 동일하므로
 * 재구성 불필요. 그리기만 다시 요청.
 * ------------------------------------------------------------------ */
static void cmd_toggle_hanja(void) {
    g_state.hanja_show = !g_state.hanja_show;
    HMENU menu = GetMenu(g_state.hwnd);
    if (!menu) menu = g_state.fs_menu;
    if (menu) {
        CheckMenuItem(menu, IDM_HANJA,
                      MF_BYCOMMAND | (g_state.hanja_show ?
                                      MF_CHECKED : MF_UNCHECKED));
    }
    InvalidateRect(g_state.hwnd, NULL, TRUE);
}

/* ------------------------------------------------------------------
 * 두 쪽 보기 (분할) 토글 — Alt+1.
 *
 * 활성화: pane 1을 pane 0과 같은 위치로 동기화. 두 페인 모두 같은
 * 문서를 보지만 독립 스크롤. wrap 모드에서는 페인 폭이 절반으로
 * 줄어드므로 render line을 재구성.
 * 비활성화: active_pane을 0으로 리셋, wrap 시 전폭으로 다시 구성.
 * ------------------------------------------------------------------ */
static void cmd_toggle_split(void) {
    g_state.split_active = !g_state.split_active;
    if (g_state.split_active) {
        g_state.top_line2    = g_state.top_line;
        g_state.h_scroll_px2 = g_state.h_scroll_px;
    } else {
        g_state.active_pane = 0;
    }

    HMENU menu = GetMenu(g_state.hwnd);
    if (!menu) menu = g_state.fs_menu;
    if (menu) {
        CheckMenuItem(menu, IDM_SPLIT,
                      MF_BYCOMMAND | (g_state.split_active ?
                                      MF_CHECKED : MF_UNCHECKED));
    }

    /* wrap 모드면 페인 폭 변경에 맞춰 render line 재구성 */
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        /* 비-wrap에서는 max_line_px가 doc 기준이라 그대로 유효.
         * h_scroll만 새 페인 폭에 맞춰 클램프. */
        scroll_h_to(pane_h_scroll(g_state.active_pane));
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
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
    if (g_state.wrap_mode && g_state.text_len > 0) {
        rebuild_render_lines_preserve();
    } else {
        scroll_h_to(g_state.h_scroll_px);
        update_scrollbars();
        InvalidateRect(g_state.hwnd, NULL, TRUE);
    }
}

/* ------------------------------------------------------------------
 * 전체화면 토글 (F11)
 *
 * 메뉴/타이틀바/테두리 제거 후 모니터 전체로 확장.
 * 두 번째 호출 시 저장해둔 스타일/위치로 복원.
 * Esc로도 빠져나올 수 있게 wnd_proc에서 처리.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------
 * 단축키 도움말 — F1 또는 메뉴에서 호출.
 *
 * MessageBox는 가변폭 폰트 + 단일 컬럼이라 길어지면 화면 밖으로 넘침.
 * 그래서 prompt_input과 같은 코드 기반 모달 다이얼로그 패턴으로
 * 좌/우 두 컬럼 + 모노스페이스 폰트(Consolas) 다이얼로그를 직접 띄움.
 * 외부 .rc 자원 의존성 0 정책 유지.
 *
 * 텍스트는 파일 상단 키 바인딩 주석과 동기 유지 (변경 시 양쪽 같이).
 * ------------------------------------------------------------------ */
typedef struct {
    HFONT font;
    int   dpi;
    int   done;
} HelpCtx;
static HelpCtx *g_help;

#define HELP_SCALE(v) MulDiv((v), g_help->dpi, 96)

static BOOL CALLBACK help_set_font(HWND hwnd, LPARAM font) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static const wchar_t *help_text_left(void) {
    return
        L"[파일]\r\n"
        L"  Ctrl+O               열기\r\n"
        L"  Ctrl+S               다른 이름으로 저장\r\n"
        L"\r\n"
        L"[편집]\r\n"
        L"  Ctrl+C               선택 복사\r\n"
        L"  Ctrl+A               모두 선택\r\n"
        L"  마우스 드래그        텍스트 선택\r\n"
        L"  Shift+← / →          한 글자 선택\r\n"
        L"  Shift+↑ / ↓          한 줄 선택\r\n"
        L"  Shift+Home / End     줄 시작/끝 선택\r\n"
        L"  Shift+Ctrl+Home/End  문서 처음/끝 선택\r\n"
        L"\r\n"
        L"[찾기 / 이동]\r\n"
        L"  Ctrl+F               찾기 (Esc 취소)\r\n"
        L"  F3 / Shift+F3        다음 / 이전\r\n"
        L"  Ctrl+G               줄 이동\r\n"
        L"  Ctrl+T               목차 (마크다운 #)\r\n"
        L"\r\n"
        L"[책갈피]\r\n"
        L"  Ctrl+B               책갈피 토글\r\n"
        L"  Ctrl+, / Ctrl+.      이전 / 다음";
}

static const wchar_t *help_text_right(void) {
    return
        L"[보기]\r\n"
        L"  Ctrl+L               줄 번호 토글\r\n"
        L"  Ctrl+D               다크 모드\r\n"
        L"  Ctrl+W               자동 줄바꿈\r\n"
        L"  Alt+1                두 쪽 보기\r\n"
        L"  F11                  전체화면 (Esc 해제)\r\n"
        L"\r\n"
        L"[인코딩]\r\n"
        L"  F2 / Shift+F2        CP949 ↔ UTF-8 ↔ Johab\r\n"
        L"  (UTF-16, Shift-JIS는 메뉴에서 선택)\r\n"
        L"\r\n"
        L"[폰트]\r\n"
        L"  Ctrl+Shift+, / .     크기 - / +\r\n"
        L"  Ctrl+휠              크기 - / +\r\n"
        L"  Shift+휠             줄 간격\r\n"
        L"  Ctrl+Shift+휠        자간\r\n"
        L"\r\n"
        L"[스크롤]\r\n"
        L"  ↑ / ↓                한 줄\r\n"
        L"  PageUp / PageDown    한 화면\r\n"
        L"  Home / End           처음 / 끝\r\n"
        L"  Ctrl+Home / End      처음 / 끝\r\n"
        L"  ← / →                가로\r\n"
        L"  Alt+화살표           여백 조절\r\n"
        L"  Space                자동 스크롤\r\n"
        L"\r\n"
        L"[도움말]\r\n"
        L"  F1                   이 도움말";
}

static LRESULT CALLBACK help_wnd_proc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;

        /* 모노스페이스 — 단축키 컬럼 정렬용. Consolas는 Windows 기본 포함,
         * lfCharSet=DEFAULT_CHARSET 으로 한글 글리프는 시스템 fallback. */
        LOGFONTW lf = { 0 };
        lf.lfHeight  = -MulDiv(10, g_help->dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Consolas");
        g_help->font = CreateFontIndirectW(&lf);

        CreateWindowW(L"STATIC", help_text_left(),
                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                      HELP_SCALE(16), HELP_SCALE(12),
                      HELP_SCALE(360), HELP_SCALE(450),
                      hwnd, NULL, hi, NULL);
        CreateWindowW(L"STATIC", help_text_right(),
                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                      HELP_SCALE(396), HELP_SCALE(12),
                      HELP_SCALE(360), HELP_SCALE(450),
                      hwnd, NULL, hi, NULL);
        CreateWindowW(L"BUTTON", L"닫기",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      HELP_SCALE(660), HELP_SCALE(474),
                      HELP_SCALE(96), HELP_SCALE(28),
                      hwnd, (HMENU)(UINT_PTR)IDOK, hi, NULL);

        EnumChildWindows(hwnd, help_set_font,
                         (LPARAM)(g_help->font ? g_help->font
                                              : (HFONT)GetStockObject(DEFAULT_GUI_FONT)));
        return 0;
    }
    case WM_DESTROY:
        if (g_help && g_help->font) {
            DeleteObject(g_help->font);
            g_help->font = NULL;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            g_help->done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_help->done = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void cmd_show_help(void) {
    static int registered = 0;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = help_wnd_proc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"hview_help";
        RegisterClassExW(&wc);
        registered = 1;
    }

    HWND parent = g_state.hwnd;
    int dpi;
    {
        HDC hdc = GetDC(parent);
        dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(parent, hdc);
        if (dpi <= 0) dpi = 96;
    }

    HelpCtx ctx;
    ctx.dpi  = dpi;
    ctx.done = 0;
    ctx.font = NULL;
    g_help = &ctx;

    /* 96 DPI 기준 780×560 — 두 컬럼 + 닫기 버튼 + 하단 여백.
     * 외곽 크기에서 타이틀바(~31px)와 아래 테두리를 빼고도 버튼이
     * 가장자리와 겹치지 않도록 약 22px 여백 확보. */
    int w = MulDiv(780, dpi, 96);
    int h = MulDiv(560, dpi, 96);
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top  + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"hview_help", L"hViewer 단축키 & 도움말",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, parent, NULL, hi, NULL);
    if (!dlg) { g_help = NULL; return; }

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
    g_help = NULL;
}

#undef HELP_SCALE

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

/* ------------------------------------------------------------------
 * 검색 히스토리 — HKCU\Software\hview\SearchHistory 아래 0..9 값에
 * 최근 검색어 저장 (REG_SZ). 0 = 가장 최근. recent files와 동일 패턴.
 * ------------------------------------------------------------------ */
static void search_history_load(void) {
    g_state.search_history_count = 0;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SEARCH_HIST_REG_PATH, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return;
    for (int i = 0; i < SEARCH_HIST_MAX; i++) {
        wchar_t name[16];
        _snwprintf_s(name, 16, _TRUNCATE, L"%d", i);
        DWORD type;
        DWORD sz = sizeof(g_state.search_history[g_state.search_history_count]);
        if (RegQueryValueExW(hk, name, NULL, &type,
                             (BYTE*)g_state.search_history[g_state.search_history_count],
                             &sz) == ERROR_SUCCESS && type == REG_SZ &&
            g_state.search_history[g_state.search_history_count][0]) {
            g_state.search_history_count++;
        }
    }
    RegCloseKey(hk);
}

static void search_history_save(void) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SEARCH_HIST_REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;
    for (int i = 0; i < SEARCH_HIST_MAX; i++) {
        wchar_t name[16];
        _snwprintf_s(name, 16, _TRUNCATE, L"%d", i);
        if (i < g_state.search_history_count) {
            DWORD bytes = (DWORD)((wcslen(g_state.search_history[i]) + 1) *
                                  sizeof(wchar_t));
            RegSetValueExW(hk, name, 0, REG_SZ,
                           (const BYTE*)g_state.search_history[i], bytes);
        } else {
            RegDeleteValueW(hk, name);
        }
    }
    RegCloseKey(hk);
}

static void search_history_add(const wchar_t *needle) {
    if (!needle || !needle[0]) return;
    /* 중복 제거 후 맨 앞 삽입 (recent files와 동일 패턴) */
    int found = -1;
    for (int i = 0; i < g_state.search_history_count; i++) {
        if (wcscmp(g_state.search_history[i], needle) == 0) { found = i; break; }
    }
    if (found > 0) {
        for (int i = found; i > 0; i--) {
            wcscpy_s(g_state.search_history[i], 256,
                     g_state.search_history[i - 1]);
        }
        wcsncpy_s(g_state.search_history[0], 256, needle, _TRUNCATE);
    } else if (found < 0) {
        int n = g_state.search_history_count;
        if (n > SEARCH_HIST_MAX - 1) n = SEARCH_HIST_MAX - 1;
        for (int i = n; i > 0; i--) {
            wcscpy_s(g_state.search_history[i], 256,
                     g_state.search_history[i - 1]);
        }
        wcsncpy_s(g_state.search_history[0], 256, needle, _TRUNCATE);
        if (g_state.search_history_count < SEARCH_HIST_MAX)
            g_state.search_history_count++;
    }
    search_history_save();
}

static void search_history_rebuild_menu(void) {
    HMENU m = g_state.search_hist_menu;
    if (!m) return;
    while (DeleteMenu(m, 0, MF_BYPOSITION)) ;
    if (g_state.search_history_count == 0) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(없음)");
        return;
    }
    for (int i = 0; i < g_state.search_history_count; i++) {
        wchar_t label[256 + 16];
        _snwprintf_s(label, 256 + 16, _TRUNCATE,
                     L"&%d  %s", (i + 1) % 10, g_state.search_history[i]);
        AppendMenuW(m, MF_STRING, IDM_SEARCH_HIST_BASE + i, label);
    }
}

/* ------------------------------------------------------------------
 * 책갈피 영속화 — HKCU\Software\hview\Bookmarks 아래 파일 경로를
 * 값 이름으로, REG_BINARY로 int 줄 번호 배열을 저장.
 *
 * 키 경로(value name)에는 백슬래시가 들어가도 OK (분리자는 키 path 한정).
 * 값 이름 길이 제한은 16383자 — MAX_PATH로 충분.
 * ------------------------------------------------------------------ */
static void bookmarks_load(const wchar_t *path) {
    g_state.bookmark_count = 0;
    if (!path || !path[0]) return;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, BOOKMARKS_REG_PATH, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return;
    DWORD type = 0, sz = sizeof(g_state.bookmarks);
    if (RegQueryValueExW(hk, path, NULL, &type,
                         (BYTE*)g_state.bookmarks, &sz) == ERROR_SUCCESS &&
        type == REG_BINARY) {
        int n = (int)(sz / sizeof(int));
        if (n > MAX_BOOKMARKS) n = MAX_BOOKMARKS;
        /* 정렬 가정 — 손상 시 재정렬 (insertion sort, n ≤ MAX_BOOKMARKS) */
        for (int i = 1; i < n; i++) {
            int v = g_state.bookmarks[i];
            int j = i - 1;
            while (j >= 0 && g_state.bookmarks[j] > v) {
                g_state.bookmarks[j + 1] = g_state.bookmarks[j];
                j--;
            }
            g_state.bookmarks[j + 1] = v;
        }
        /* 음수/중복 제거 */
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (g_state.bookmarks[i] < 0) continue;
            if (w > 0 && g_state.bookmarks[w - 1] == g_state.bookmarks[i]) continue;
            g_state.bookmarks[w++] = g_state.bookmarks[i];
        }
        g_state.bookmark_count = w;
    }
    RegCloseKey(hk);
}

static void bookmarks_save(const wchar_t *path) {
    if (!path || !path[0]) return;
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, BOOKMARKS_REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;
    if (g_state.bookmark_count > 0) {
        RegSetValueExW(hk, path, 0, REG_BINARY,
                       (const BYTE*)g_state.bookmarks,
                       (DWORD)(g_state.bookmark_count * sizeof(int)));
    } else {
        /* 모두 지웠으면 레지스트리도 정리 — 최근 파일 목록과 일치하는 정신적 모델 */
        RegDeleteValueW(hk, path);
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
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RECENT_CLEAR, L"목록 지우기(&C)");
}

static void cmd_clear_recent(void) {
    g_state.recent_count = 0;
    recent_save();              /* 빈 상태 — 모든 슬롯이 RegDeleteValueW로 정리됨 */
    recent_rebuild_menu();
    if (!g_state.fs_active) DrawMenuBar(g_state.hwnd);
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

/* ------------------------------------------------------------------
 * 사용자 설정 영속화 — HKCU\Software\hview\Settings.
 *
 * 메뉴/단축키로 바뀐 표시 옵션을 세션 간 유지. 종료 시점(WM_DESTROY)에
 * 한 번 저장하므로 비정상 종료 시 잃을 수 있음. 진짜 결정적 이슈는
 * 책갈피처럼 사용자 데이터인데, 이건 후속 phase로.
 * ------------------------------------------------------------------ */
static void reg_set_dword(HKEY hk, const wchar_t *name, DWORD v) {
    RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}
static DWORD reg_get_dword(HKEY hk, const wchar_t *name, DWORD def) {
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(hk, name, NULL, &type, (BYTE*)&v, &sz) ==
        ERROR_SUCCESS && type == REG_DWORD) return v;
    return def;
}

/* 시작 시 사용할 윈도우 위치/크기를 레지스트리에서 읽음.
 * 저장된 값이 없거나 화면 밖이면 0(use defaults) 반환. */
static int settings_load_window_rect(int *x, int *y, int *w, int *h, int *maximized) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return 0;
    DWORD has = 0, sz = sizeof(has), type = 0;
    if (RegQueryValueExW(hk, L"WinW", NULL, &type, (BYTE*)&has, &sz) != ERROR_SUCCESS) {
        RegCloseKey(hk);
        return 0;
    }
    *x = (int)reg_get_dword(hk, L"WinX", 0);
    *y = (int)reg_get_dword(hk, L"WinY", 0);
    *w = (int)reg_get_dword(hk, L"WinW", 900);
    *h = (int)reg_get_dword(hk, L"WinH", 700);
    *maximized = reg_get_dword(hk, L"WinMax", 0) ? 1 : 0;
    RegCloseKey(hk);

    /* 화면 밖이면 무효 — 모니터 구성이 바뀌었을 수 있음. SM_*를 통한
     * 가상 화면 좌표로 클램프 (다중 모니터 환경 대응). */
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w < 200) *w = 200;
    if (*h < 150) *h = 150;
    if (*x < vx || *x > vx + vw - 100) *x = CW_USEDEFAULT;
    if (*y < vy || *y > vy + vh - 100) *y = CW_USEDEFAULT;
    return 1;
}

static void settings_load(void) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return;

    g_state.dark_mode          = reg_get_dword(hk, L"DarkMode", 0)    ? TRUE : FALSE;
    g_state.show_line_numbers  = reg_get_dword(hk, L"LineNumbers", 0) ? TRUE : FALSE;
    g_state.wrap_mode          = reg_get_dword(hk, L"WrapMode", 0)    ? TRUE : FALSE;
    g_state.split_active       = reg_get_dword(hk, L"SplitMode", 0)   ? TRUE : FALSE;
    g_state.hanja_show         = reg_get_dword(hk, L"HanjaShow", 0)   ? TRUE : FALSE;
    g_state.font_size          = (int)reg_get_dword(hk, L"FontSize", 11);
    g_state.font_weight        = (LONG)reg_get_dword(hk, L"FontWeight", 0);
    g_state.font_italic        = reg_get_dword(hk, L"FontItalic", 0)  ? 1 : 0;
    g_state.line_spacing_extra = (int)reg_get_dword(hk, L"LineSpacing", 0);
    g_state.char_spacing_extra = (int)reg_get_dword(hk, L"CharSpacing", 0);
    g_state.margin_left_px     = (int)reg_get_dword(hk, L"MarginLeft", 0);
    g_state.margin_top_px      = (int)reg_get_dword(hk, L"MarginTop", 0);
    g_state.autoscroll_delay_ms = (int)reg_get_dword(hk, L"AutoscrollMs",
                                                     AUTOSCROLL_DEFAULT_MS);
    g_state.search_case_sensitive = reg_get_dword(hk, L"FindCase", 0) ? TRUE : FALSE;
    g_state.search_whole_word     = reg_get_dword(hk, L"FindWord", 0) ? TRUE : FALSE;
    g_state.status_visible        = reg_get_dword(hk, L"StatusBar", 1) ? TRUE : FALSE;

    DWORD type = 0;
    DWORD sz = sizeof(g_state.font_face);
    if (RegQueryValueExW(hk, L"FontFace", NULL, &type,
                         (BYTE*)g_state.font_face, &sz) != ERROR_SUCCESS ||
        type != REG_SZ) {
        g_state.font_face[0] = 0;
    }

    /* 범위 클램프 — 레지스트리가 망가져도 안전한 값으로 */
    if (g_state.font_size < 6 || g_state.font_size > 72)
        g_state.font_size = 11;
    if (g_state.line_spacing_extra < 0 || g_state.line_spacing_extra > 32)
        g_state.line_spacing_extra = 0;
    if (g_state.char_spacing_extra < 0 || g_state.char_spacing_extra > 16)
        g_state.char_spacing_extra = 0;
    if (g_state.margin_left_px < 0)  g_state.margin_left_px = 0;
    if (g_state.margin_top_px  < 0)  g_state.margin_top_px  = 0;

    RegCloseKey(hk);
}

static void settings_save(void) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) return;

    reg_set_dword(hk, L"DarkMode",     g_state.dark_mode ? 1 : 0);
    reg_set_dword(hk, L"LineNumbers",  g_state.show_line_numbers ? 1 : 0);
    reg_set_dword(hk, L"WrapMode",     g_state.wrap_mode ? 1 : 0);
    reg_set_dword(hk, L"SplitMode",    g_state.split_active ? 1 : 0);
    reg_set_dword(hk, L"HanjaShow",    g_state.hanja_show ? 1 : 0);
    reg_set_dword(hk, L"FontSize",     (DWORD)g_state.font_size);
    reg_set_dword(hk, L"FontWeight",   (DWORD)g_state.font_weight);
    reg_set_dword(hk, L"FontItalic",   g_state.font_italic ? 1 : 0);
    reg_set_dword(hk, L"LineSpacing",  (DWORD)g_state.line_spacing_extra);
    reg_set_dword(hk, L"CharSpacing",  (DWORD)g_state.char_spacing_extra);
    reg_set_dword(hk, L"MarginLeft",   (DWORD)g_state.margin_left_px);
    reg_set_dword(hk, L"MarginTop",    (DWORD)g_state.margin_top_px);
    reg_set_dword(hk, L"AutoscrollMs", (DWORD)g_state.autoscroll_delay_ms);
    reg_set_dword(hk, L"FindCase",     g_state.search_case_sensitive ? 1 : 0);
    reg_set_dword(hk, L"FindWord",     g_state.search_whole_word ? 1 : 0);
    reg_set_dword(hk, L"StatusBar",    g_state.status_visible ? 1 : 0);

    /* 윈도우 위치/크기 — 최대화 상태이거나 최소화 상태면 normal 좌표 보존,
     * 정상 상태면 현재 RECT. WinMain이 기동 시 복원함. */
    if (g_state.hwnd) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(g_state.hwnd, &wp)) {
            RECT r = wp.rcNormalPosition;
            reg_set_dword(hk, L"WinX",   (DWORD)r.left);
            reg_set_dword(hk, L"WinY",   (DWORD)r.top);
            reg_set_dword(hk, L"WinW",   (DWORD)(r.right  - r.left));
            reg_set_dword(hk, L"WinH",   (DWORD)(r.bottom - r.top));
            reg_set_dword(hk, L"WinMax", (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0);
        }
    }

    if (g_state.font_face[0]) {
        DWORD bytes = (DWORD)((wcslen(g_state.font_face) + 1) *
                              sizeof(wchar_t));
        RegSetValueExW(hk, L"FontFace", 0, REG_SZ,
                       (const BYTE*)g_state.font_face, bytes);
    }

    RegCloseKey(hk);
}

/* ------------------------------------------------------------------
 * 사용자 색 영속화 — %APPDATA%\hview\theme.txt
 *
 * 형식 (ASCII, 줄당 key=R,G,B):
 *   hview-theme v2          (v1도 호환 — bg, fg만 있는 옛 파일)
 *   bg=255,255,255
 *   fg=0,0,0
 *   hl_bg=255,230,80        (선택 사항)
 *   hl_fg=0,0,0             (선택 사항)
 *
 * 검증 규칙:
 *   - 첫 번째 비-주석 라인은 "hview-theme v1" 또는 "hview-theme v2"
 *   - bg, fg 둘 다 필수, R/G/B는 0~255 정수
 *   - hl_bg/hl_fg는 v2에서 선택 — 둘 다 있으면 user_hl_set=1
 *   - 라인은 "key=R,G,B" 형식
 *   - '#' 시작 라인과 빈 라인은 주석/스킵
 *
 * 위 규칙 어느 하나라도 어긋나면 즉시 기본값으로 복구하고 파일 재작성.
 * ------------------------------------------------------------------ */
static int theme_file_path(wchar_t *out, int cap) {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    int w = _snwprintf_s(out, cap, _TRUNCATE,
                         L"%s\\hview\\theme.txt", appdata);
    return w > 0;
}

static void theme_dir_ensure(void) {
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\hview", appdata);
    /* 이미 존재하면 FALSE 반환 — 무시 */
    CreateDirectoryW(dir, NULL);
}

static void theme_save(void) {
    theme_dir_ensure();
    wchar_t path[MAX_PATH];
    if (!theme_file_path(path, MAX_PATH)) return;

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[512];
    int len;
    if (g_state.user_hl_set) {
        len = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "hview-theme v2\r\n"
            "# Background, foreground, and search-highlight colors\r\n"
            "# (R,G,B per channel, 0-255). Edit manually or use View menu.\r\n"
            "bg=%u,%u,%u\r\n"
            "fg=%u,%u,%u\r\n"
            "hl_bg=%u,%u,%u\r\n"
            "hl_fg=%u,%u,%u\r\n",
            GetRValue(g_state.user_bg), GetGValue(g_state.user_bg),
            GetBValue(g_state.user_bg),
            GetRValue(g_state.user_fg), GetGValue(g_state.user_fg),
            GetBValue(g_state.user_fg),
            GetRValue(g_state.user_hl_bg), GetGValue(g_state.user_hl_bg),
            GetBValue(g_state.user_hl_bg),
            GetRValue(g_state.user_hl_fg), GetGValue(g_state.user_hl_fg),
            GetBValue(g_state.user_hl_fg));
    } else {
        len = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "hview-theme v2\r\n"
            "# Background and foreground color (R,G,B per channel, 0-255).\r\n"
            "# Edit manually or use menu: View > Background / Foreground.\r\n"
            "bg=%u,%u,%u\r\n"
            "fg=%u,%u,%u\r\n",
            GetRValue(g_state.user_bg), GetGValue(g_state.user_bg),
            GetBValue(g_state.user_bg),
            GetRValue(g_state.user_fg), GetGValue(g_state.user_fg),
            GetBValue(g_state.user_fg));
    }
    if (len > 0) {
        DWORD wrote = 0;
        WriteFile(h, buf, (DWORD)len, &wrote, NULL);
    }
    CloseHandle(h);
}

/* 현재 라인 1개를 파싱. 성공 시 1, 알 수 없는 키면 1(무시), 형식 오류는 0.
 * 알 수 없는 키를 0으로 떨어뜨리면 v2 추가 키가 있는 파일을 v1로 읽을 때
 * 손상으로 오인 — 무시 정책이 안전. */
static int theme_parse_kv_line(char *line,
                                COLORREF *bg, int *bg_set,
                                COLORREF *fg, int *fg_set,
                                COLORREF *hl_bg, int *hl_bg_set,
                                COLORREF *hl_fg, int *hl_fg_set) {
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = 0;
    const char *key = line;
    const char *val = eq + 1;

    unsigned r, g, b;
    if (sscanf_s(val, "%u,%u,%u", &r, &g, &b) != 3) return 0;
    if (r > 255 || g > 255 || b > 255) return 0;

    COLORREF c = RGB(r, g, b);
    if      (strcmp(key, "bg")    == 0) { *bg = c;    *bg_set = 1; }
    else if (strcmp(key, "fg")    == 0) { *fg = c;    *fg_set = 1; }
    else if (strcmp(key, "hl_bg") == 0) { *hl_bg = c; *hl_bg_set = 1; }
    else if (strcmp(key, "hl_fg") == 0) { *hl_fg = c; *hl_fg_set = 1; }
    else { /* 모르는 키는 무시 — 미래 확장 호환 */ }
    return 1;
}

/* 항상 g_state.user_bg/fg를 채움. 파일이 없거나 손상이면 기본값으로
 * 복구하고 0 반환, 정상 로드면 1. 손상 검출 시 즉시 기본값으로 재작성. */
static int theme_load_or_default(void) {
    g_state.user_bg = HVIEW_THEME_DEFAULT_BG;
    g_state.user_fg = HVIEW_THEME_DEFAULT_FG;
    g_state.user_hl_set = 0;

    wchar_t path[MAX_PATH];
    if (!theme_file_path(path, MAX_PATH)) return 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* 파일 없음 — 기본값으로 새로 생성 */
        theme_save();
        return 0;
    }

    char buf[2048];
    DWORD read_n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read_n, NULL);
    CloseHandle(h);
    if (!ok || read_n == 0) goto corrupt;
    buf[read_n] = 0;

    /* 라인 단위 strict 파싱. 각 비-주석 라인은 의미가 있어야 함. */
    int header_ok = 0;
    int bg_set = 0, fg_set = 0, hl_bg_set = 0, hl_fg_set = 0;
    COLORREF bg_v = 0, fg_v = 0, hl_bg_v = 0, hl_fg_v = 0;

    char *p = buf;
    while (*p) {
        char *eol = strpbrk(p, "\r\n");
        char saved = 0;
        if (eol) { saved = *eol; *eol = 0; }

        /* 빈 줄/주석 스킵 */
        if (*p && *p != '#') {
            if (!header_ok) {
                /* v1과 v2 둘 다 허용 — v1 파일은 hl_* 없이 그대로 통과 */
                if (strcmp(p, "hview-theme v1") != 0 &&
                    strcmp(p, "hview-theme v2") != 0) goto corrupt;
                header_ok = 1;
            } else {
                if (!theme_parse_kv_line(p, &bg_v, &bg_set, &fg_v, &fg_set,
                                         &hl_bg_v, &hl_bg_set,
                                         &hl_fg_v, &hl_fg_set))
                    goto corrupt;
            }
        }

        if (eol) {
            *eol = saved;
            p = eol;
            while (*p == '\r' || *p == '\n') p++;
        } else break;
    }

    if (!header_ok || !bg_set || !fg_set) goto corrupt;

    g_state.user_bg = bg_v;
    g_state.user_fg = fg_v;
    /* hl_bg/hl_fg는 둘 다 있을 때만 활성 — 한쪽만 있으면 미설정으로 취급 */
    if (hl_bg_set && hl_fg_set) {
        g_state.user_hl_bg  = hl_bg_v;
        g_state.user_hl_fg  = hl_fg_v;
        g_state.user_hl_set = 1;
    }
    return 1;

corrupt:
    g_state.user_bg = HVIEW_THEME_DEFAULT_BG;
    g_state.user_fg = HVIEW_THEME_DEFAULT_FG;
    g_state.user_hl_set = 0;
    theme_save();   /* 손상된 파일을 기본값으로 덮어씀 */
    return 0;
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
 * 다른 이름으로 저장 — 인코딩 변환 후 새 파일에 기록.
 *
 * UTF-16 본문(g_state.text)을 사용자가 선택한 인코딩으로 변환.
 * 필터 인덱스로 인코딩 식별 (별도 다이얼로그 회피).
 * 표현 불가능한 문자가 있으면 사용자에게 확인 후 '?'로 대체.
 *
 * Johab은 인코더 미구현 — 메뉴에서 제외.
 * ------------------------------------------------------------------ */
static void cmd_save_as(void) {
    if (g_state.text_len == 0) {
        show_error(g_state.hwnd, L"저장할 내용이 없습니다.");
        return;
    }

    wchar_t path[MAX_PATH] = {0};
    if (g_state.filepath[0]) wcscpy_s(path, MAX_PATH, g_state.filepath);

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_state.hwnd;
    ofn.lpstrFilter =
        L"UTF-8 (*.txt)\0*.txt\0"
        L"UTF-8 BOM (*.txt)\0*.txt\0"
        L"UTF-16 LE (*.txt)\0*.txt\0"
        L"UTF-16 BE (*.txt)\0*.txt\0"
        L"CP949/EUC-KR (*.txt)\0*.txt\0"
        L"Shift-JIS (*.txt)\0*.txt\0"
        L"\0";
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrDefExt  = L"txt";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    Encoding enc;
    switch (ofn.nFilterIndex) {
    case 1: enc = ENC_UTF8;     break;
    case 2: enc = ENC_UTF8_BOM; break;
    case 3: enc = ENC_UTF16_LE; break;
    case 4: enc = ENC_UTF16_BE; break;
    case 5: enc = ENC_CP949;    break;
    case 6: enc = ENC_SJIS;     break;
    default: enc = ENC_UTF8;    break;
    }

    BYTE  *out = NULL;
    size_t out_len = 0;
    int    in_len  = (int)g_state.text_len;

    if (enc == ENC_UTF8 || enc == ENC_UTF8_BOM) {
        int n = WideCharToMultiByte(CP_UTF8, 0, g_state.text, in_len,
                                    NULL, 0, NULL, NULL);
        if (n <= 0) { show_error(g_state.hwnd, L"인코딩 변환 실패."); return; }
        size_t prefix = (enc == ENC_UTF8_BOM) ? 3 : 0;
        out = (BYTE*)malloc(prefix + (size_t)n);
        if (!out) { show_error(g_state.hwnd, L"메모리 할당 실패."); return; }
        if (prefix) { out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF; }
        WideCharToMultiByte(CP_UTF8, 0, g_state.text, in_len,
                            (char*)(out + prefix), n, NULL, NULL);
        out_len = prefix + (size_t)n;
    } else if (enc == ENC_UTF16_LE) {
        out_len = 2 + (size_t)in_len * 2;
        out = (BYTE*)malloc(out_len);
        if (!out) { show_error(g_state.hwnd, L"메모리 할당 실패."); return; }
        out[0] = 0xFF; out[1] = 0xFE;
        memcpy(out + 2, g_state.text, (size_t)in_len * 2);
    } else if (enc == ENC_UTF16_BE) {
        out_len = 2 + (size_t)in_len * 2;
        out = (BYTE*)malloc(out_len);
        if (!out) { show_error(g_state.hwnd, L"메모리 할당 실패."); return; }
        out[0] = 0xFE; out[1] = 0xFF;
        for (int i = 0; i < in_len; i++) {
            wchar_t c = g_state.text[i];
            out[2 + i * 2]     = (BYTE)((c >> 8) & 0xFF);
            out[2 + i * 2 + 1] = (BYTE)(c & 0xFF);
        }
    } else {
        UINT cp = (enc == ENC_CP949) ? 949 : 932;
        BOOL used_default = FALSE;
        /* WC_NO_BEST_FIT_CHARS: 전각/유사 문자 best-fit 매핑을 막아
         * 변환 불가 문자가 '?'로 떨어질 때 used_default가 정확히 TRUE가 되도록. */
        int n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, g_state.text, in_len,
                                    NULL, 0, NULL, NULL);
        if (n <= 0) { show_error(g_state.hwnd, L"인코딩 변환 실패."); return; }
        out = (BYTE*)malloc((size_t)n);
        if (!out) { show_error(g_state.hwnd, L"메모리 할당 실패."); return; }
        WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, g_state.text, in_len,
                            (char*)out, n, "?", &used_default);
        out_len = (size_t)n;
        if (used_default) {
            int r = MessageBoxW(g_state.hwnd,
                L"일부 문자를 선택한 인코딩으로 변환할 수 없어 '?'로 대체됩니다.\n"
                L"계속 저장하시겠습니까?",
                APP_TITLE, MB_YESNO | MB_ICONWARNING);
            if (r != IDYES) { free(out); return; }
        }
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(out);
        show_error(g_state.hwnd, L"파일을 만들 수 없습니다.");
        return;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, out, (DWORD)out_len, &written, NULL) &&
              written == (DWORD)out_len;
    CloseHandle(h);
    free(out);

    if (!ok) show_error(g_state.hwnd, L"파일 쓰기 실패.");
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
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN, L"열기(&O)\t`");
    AppendMenuW(file_menu, MF_STRING, IDM_SAVE_AS,
                L"다른 이름으로 저장(&S)...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    g_state.recent_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_POPUP, (UINT_PTR)g_state.recent_menu,
                L"최근 파일(&R)");
    recent_rebuild_menu();
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"종료(&X)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file_menu, L"파일(&F)");

    HMENU edit_menu = CreatePopupMenu();
    AppendMenuW(edit_menu, MF_STRING, IDM_COPY,
                L"복사(&C)\tCtrl+C");
    AppendMenuW(edit_menu, MF_STRING, IDM_SELECT_ALL,
                L"모두 선택(&A)\tCtrl+A");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit_menu, L"편집(&E)");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_INC, L"글자 크게\tCtrl+.");
    AppendMenuW(view_menu, MF_STRING, IDM_FONT_DEC, L"글자 작게\tCtrl+,");
    AppendMenuW(view_menu, MF_STRING, IDM_CHOOSE_FONT, L"글꼴 선택...");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_LINENO,
                L"줄 번호 표시(&L)\tCtrl+L");
    AppendMenuW(view_menu, MF_STRING, IDM_DARK_MODE,
                L"다크 모드(&D)\tCtrl+D");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_BG,
                L"배경색(&K)...");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_FG,
                L"글자색(&G)...");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_RESET,
                L"색상 초기화");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_HL_BG,
                L"검색 하이라이트 배경색...");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_HL_FG,
                L"검색 하이라이트 글자색...");
    AppendMenuW(view_menu, MF_STRING, IDM_THEME_HL_RESET,
                L"하이라이트 색상 초기화");
    AppendMenuW(view_menu, MF_STRING, IDM_WRAP,
                L"자동 줄바꿈(&W)\tCtrl+W");
    AppendMenuW(view_menu, MF_STRING, IDM_SPLIT,
                L"두 쪽 보기(&2)\tAlt+1");
    AppendMenuW(view_menu, MF_STRING, IDM_HANJA,
                L"한자 음 표시(&H)");
    AppendMenuW(view_menu, MF_STRING, IDM_STATUSBAR,
                L"상태 표시줄(&S)");
    AppendMenuW(view_menu, MF_STRING, IDM_FULLSCREEN,
                L"전체화면(&F)\tF11");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_FIND,
                L"찾기(&F)...\tCtrl+F");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_NEXT,
                L"다음 찾기\tF3");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_PREV,
                L"이전 찾기\tShift+F3");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_CASE,
                L"대/소문자 구분(&C)");
    AppendMenuW(view_menu, MF_STRING, IDM_FIND_WORD,
                L"단어 단위 찾기(&W)");
    g_state.search_hist_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_POPUP, (UINT_PTR)g_state.search_hist_menu,
                L"최근 검색어");
    search_history_rebuild_menu();
    AppendMenuW(view_menu, MF_STRING, IDM_GOTO,
                L"줄 이동(&G)...\tCtrl+G");
    AppendMenuW(view_menu, MF_STRING, IDM_SHOW_TOC,
                L"목차(&T)\tCtrl+T");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view_menu, MF_STRING, IDM_BM_TOGGLE,
                L"책갈피 추가/제거(&B)\tCtrl+B");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_NEXT,
                L"다음 책갈피\tCtrl+.");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_PREV,
                L"이전 책갈피\tCtrl+,");
    AppendMenuW(view_menu, MF_STRING, IDM_BM_CLEAR,
                L"책갈피 모두 지우기");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view_menu, L"보기(&V)");

    HMENU enc_menu = CreatePopupMenu();
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_AUTO,    L"자동 판별");
    AppendMenuW(enc_menu, MF_SEPARATOR, 0, NULL);
    /* F2 순환 그룹 — 한국어 텍스트의 거의 모든 케이스. 라벨에 \tF2 표시. */
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_CP949,   L"CP949 (EUC-KR)\tF2");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF8,    L"UTF-8\tF2");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_JOHAB,   L"조합형 (Johab)\tF2");
    AppendMenuW(enc_menu, MF_SEPARATOR, 0, NULL);
    /* 메뉴 전용 — 빈도가 낮아 F2 순환에서 제외 */
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16LE, L"UTF-16 LE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_UTF16BE, L"UTF-16 BE");
    AppendMenuW(enc_menu, MF_STRING, IDM_ENC_SJIS,    L"Shift-JIS (일본어)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)enc_menu, L"인코딩(&E)");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_HELP, L"단축키 도움말(&K)\tF1");
    AppendMenuW(help_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"정보(&A)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help_menu, L"도움말(&H)");

    return menu;
}

/* ------------------------------------------------------------------
 * 윈도우 프로시저
 * ------------------------------------------------------------------ */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_state.hwnd = hwnd;
        g_state.font_size = 11;            /* 기본값 (settings_load가 덮음) */
        g_state.autoscroll_delay_ms = AUTOSCROLL_DEFAULT_MS;
        settings_load();
        /* 사용자 색은 별도 파일(%APPDATA%\hview\theme.txt) — 손상 시 자동 복구. */
        theme_load_or_default();
        g_state.search_match_pos = -1;
        g_state.sel_anchor = -1;
        g_state.sel_caret  = -1;
        create_font();
        DragAcceptFiles(hwnd, TRUE);
        /* 로드된 토글 상태를 메뉴 체크 표시에 반영 */
        HMENU m = GetMenu(hwnd);
        if (m) {
            CheckMenuItem(m, IDM_DARK_MODE,
                          MF_BYCOMMAND | (g_state.dark_mode ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_LINENO,
                          MF_BYCOMMAND | (g_state.show_line_numbers ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_WRAP,
                          MF_BYCOMMAND | (g_state.wrap_mode ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_SPLIT,
                          MF_BYCOMMAND | (g_state.split_active ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_HANJA,
                          MF_BYCOMMAND | (g_state.hanja_show ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_FIND_CASE,
                          MF_BYCOMMAND | (g_state.search_case_sensitive ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_FIND_WORD,
                          MF_BYCOMMAND | (g_state.search_whole_word ?
                                          MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m, IDM_STATUSBAR,
                          MF_BYCOMMAND | (g_state.status_visible ?
                                          MF_CHECKED : MF_UNCHECKED));
        }
        /* 상태 표시줄 — 기본은 표시. 메뉴 토글로 숨김. */
        g_state.status_hwnd = CreateWindowExW(
            0, STATUSCLASSNAMEW, NULL,
            WS_CHILD | SBARS_SIZEGRIP | (g_state.status_visible ? WS_VISIBLE : 0),
            0, 0, 0, 0,
            hwnd, NULL, GetModuleHandleW(NULL), NULL);
        return 0;
    }

    case WM_SIZE: {
        /* 상태 표시줄 자동 위치 조정 — 부모 WM_SIZE를 그대로 전달 */
        if (g_state.status_hwnd) {
            SendMessageW(g_state.status_hwnd, WM_SIZE, 0, 0);
            status_recompute_height();
            status_set_parts();
        }
        g_state.client_w = LOWORD(lp);
        g_state.client_h = HIWORD(lp) - g_state.status_height;
        if (g_state.client_h < 0) g_state.client_h = 0;
        if (g_state.char_height > 0) {
            int avail = g_state.client_h - g_state.margin_top_px;
            if (avail < line_total_height()) avail = line_total_height();
            g_state.visible_lines = avail / line_total_height();
        }
        if (g_state.wrap_mode && g_state.text_len > 0) {
            rebuild_render_lines_preserve();
        } else {
            update_scrollbars();
        }
        status_update();
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
        /* HIWORD(wp)는 16비트 한정이라 65535줄 초과 파일에서 잘림.
         * SCROLLINFO의 nTrackPos는 32비트라 SIF_ALL/SIF_TRACKPOS로 받음. */
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: new_pos = si.nTrackPos; break;
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
        case SB_THUMBPOSITION: new_pos = si.nTrackPos; break;
        }
        scroll_h_to(new_pos * unit);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        /* 분할 시 클릭한 페인을 활성화. 디바이더 위 클릭은 무시. */
        if (g_state.split_active) {
            int p = pane_at_x(mx);
            if (p < 0) return 0;
            if (p != g_state.active_pane) {
                g_state.active_pane = p;
                update_scrollbars();
            }
        }
        int off = offset_at_mouse(mx, my);
        g_state.sel_anchor = off;
        g_state.sel_caret  = off;
        g_state.sel_dragging = TRUE;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_state.sel_dragging && (wp & MK_LBUTTON)) {
            int mx = (int)(short)LOWORD(lp);
            int my = (int)(short)HIWORD(lp);
            int off = offset_at_mouse(mx, my);
            if (off != g_state.sel_caret) {
                g_state.sel_caret = off;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_state.sel_dragging) {
            g_state.sel_dragging = FALSE;
            ReleaseCapture();
            /* 클릭만 하고 드래그 안 했으면 선택 없음으로 정리 */
            if (g_state.sel_anchor == g_state.sel_caret) {
                selection_clear();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        WORD keys = GET_KEYSTATE_WPARAM(wp);
        BOOL ctrl  = (keys & MK_CONTROL) != 0;
        BOOL shift = (keys & MK_SHIFT)   != 0;
        /* 분할 시 휠 위치의 페인을 활성화하여 그쪽이 스크롤되게 함 */
        if (g_state.split_active) {
            POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            int p = pane_at_x(pt.x);
            if (p >= 0 && p != g_state.active_pane) {
                g_state.active_pane = p;
                update_scrollbars();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
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

    case WM_SYSKEYDOWN: {
        /* Alt+방향키 — 여백 조절. Alt+1 — 분할 보기 토글.
         * 그 외 Alt 키는 시스템 처리 위임. */
        BOOL alt = (lp & (1 << 29)) != 0;
        if (alt) {
            int hstep = g_state.avg_char_width > 0 ? g_state.avg_char_width : 8;
            int vstep = g_state.char_height > 1 ? g_state.char_height / 2 : 4;
            switch (wp) {
            case VK_LEFT:  margin_change_left(-hstep); return 0;
            case VK_RIGHT: margin_change_left(+hstep); return 0;
            case VK_UP:    margin_change_top(-vstep);  return 0;
            case VK_DOWN:  margin_change_top(+vstep);  return 0;
            case '1':      cmd_toggle_split();         return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_KEYDOWN: {
        int ctrl  = GetKeyState(VK_CONTROL) & 0x8000;
        int shift = GetKeyState(VK_SHIFT)   & 0x8000;
        switch (wp) {
        case VK_UP:
            if (shift) kb_sel_extend(-2);
            else       scroll_to_line(g_state.top_line - 1);
            break;
        case VK_DOWN:
            if (shift) kb_sel_extend(+2);
            else       scroll_to_line(g_state.top_line + 1);
            break;
        case VK_PRIOR:
            scroll_to_line(g_state.top_line - g_state.visible_lines);
            break;
        case VK_NEXT:
            scroll_to_line(g_state.top_line + g_state.visible_lines);
            break;
        case VK_HOME:
            if (shift && ctrl) kb_sel_extend(-4);
            else if (shift)    kb_sel_extend(-3);
            else               scroll_to_line(0);
            break;
        case VK_END:
            if (shift && ctrl) kb_sel_extend(+4);
            else if (shift)    kb_sel_extend(+3);
            else               scroll_to_line(g_state.line_count);
            break;
        case VK_LEFT:
            if (shift) kb_sel_extend(-1);
            else       scroll_h_to(g_state.h_scroll_px -
                                   g_state.avg_char_width * 4);
            break;
        case VK_RIGHT:
            if (shift) kb_sel_extend(+1);
            else       scroll_h_to(g_state.h_scroll_px +
                                   g_state.avg_char_width * 4);
            break;
        case 'O':
            if (ctrl) cmd_open_file();
            break;
        case 'S':
            if (ctrl) cmd_save_as();
            break;
        case 'C':
            if (ctrl) cmd_copy();
            break;
        case 'A':
            if (ctrl) cmd_select_all();
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
        case 'W':
            if (ctrl) cmd_toggle_wrap();
            break;
        case 'B':
            if (ctrl) cmd_bookmark_toggle();
            break;
        case 'T':
            if (ctrl) cmd_show_toc();
            break;
        case VK_F2:
            cmd_cycle_encoding(shift ? FALSE : TRUE);
            break;
        case VK_F3:
            cmd_find_again(shift ? FALSE : TRUE);
            break;
        case VK_F1:
            cmd_show_help();
            break;
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
        case VK_OEM_PERIOD: /* '.' 키 — Ctrl+. : 다음 책갈피, Ctrl+Shift+. : 폰트 크기 + */
            if (ctrl && shift)      font_change(+1);
            else if (ctrl)          cmd_bookmark_jump(TRUE);
            break;
        case VK_OEM_COMMA:  /* ',' 키 — Ctrl+, : 이전 책갈피, Ctrl+Shift+, : 폰트 크기 - */
            if (ctrl && shift)      font_change(-1);
            else if (ctrl)          cmd_bookmark_jump(FALSE);
            break;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:        cmd_open_file(); break;
        case IDM_SAVE_AS:     cmd_save_as(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        case IDM_COPY:        cmd_copy(); break;
        case IDM_SELECT_ALL:  cmd_select_all(); break;
        case IDM_GOTO:        cmd_goto_line(); break;
        case IDM_LINENO:      cmd_toggle_line_numbers(); break;
        case IDM_DARK_MODE:   cmd_toggle_dark_mode(); break;
        case IDM_THEME_BG:       cmd_choose_color(0); break;
        case IDM_THEME_FG:       cmd_choose_color(1); break;
        case IDM_THEME_HL_BG:    cmd_choose_color(2); break;
        case IDM_THEME_HL_FG:    cmd_choose_color(3); break;
        case IDM_THEME_RESET:    cmd_theme_reset(); break;
        case IDM_THEME_HL_RESET: cmd_theme_hl_reset(); break;
        case IDM_STATUSBAR: {
            g_state.status_visible = !g_state.status_visible;
            if (g_state.status_hwnd) {
                ShowWindow(g_state.status_hwnd,
                           g_state.status_visible ? SW_SHOW : SW_HIDE);
            }
            CheckMenuItem(GetMenu(g_state.hwnd), IDM_STATUSBAR,
                          MF_BYCOMMAND | (g_state.status_visible ?
                                          MF_CHECKED : MF_UNCHECKED));
            /* 본문 영역 재계산 */
            RECT cr;
            GetClientRect(g_state.hwnd, &cr);
            SendMessageW(g_state.hwnd, WM_SIZE, 0,
                         MAKELPARAM(cr.right - cr.left, cr.bottom - cr.top));
            InvalidateRect(g_state.hwnd, NULL, TRUE);
            break;
        }
        case IDM_WRAP:        cmd_toggle_wrap(); break;
        case IDM_SPLIT:       cmd_toggle_split(); break;
        case IDM_HANJA:       cmd_toggle_hanja(); break;
        case IDM_FULLSCREEN:  cmd_toggle_fullscreen(); break;
        case IDM_FIND:        cmd_find(); break;
        case IDM_FIND_NEXT:   cmd_find_again(TRUE); break;
        case IDM_FIND_PREV:   cmd_find_again(FALSE); break;
        case IDM_FIND_CASE:
            g_state.search_case_sensitive = !g_state.search_case_sensitive;
            CheckMenuItem(GetMenu(g_state.hwnd), IDM_FIND_CASE,
                          MF_BYCOMMAND | (g_state.search_case_sensitive ?
                                          MF_CHECKED : MF_UNCHECKED));
            break;
        case IDM_FIND_WORD:
            g_state.search_whole_word = !g_state.search_whole_word;
            CheckMenuItem(GetMenu(g_state.hwnd), IDM_FIND_WORD,
                          MF_BYCOMMAND | (g_state.search_whole_word ?
                                          MF_CHECKED : MF_UNCHECKED));
            break;
        case IDM_BM_TOGGLE:   cmd_bookmark_toggle(); break;
        case IDM_BM_NEXT:     cmd_bookmark_jump(TRUE); break;
        case IDM_BM_PREV:     cmd_bookmark_jump(FALSE); break;
        case IDM_BM_CLEAR:    cmd_bookmark_clear(); break;
        case IDM_SHOW_TOC:    cmd_show_toc(); break;
        case IDM_FONT_INC:     font_change(+1); break;
        case IDM_FONT_DEC:     font_change(-1); break;
        case IDM_CHOOSE_FONT:  cmd_choose_font(); break;
        case IDM_ENC_AUTO:    reload_with_encoding(ENC_UNKNOWN); break;
        case IDM_ENC_UTF8:    reload_with_encoding(ENC_UTF8); break;
        case IDM_ENC_UTF16LE: reload_with_encoding(ENC_UTF16_LE); break;
        case IDM_ENC_UTF16BE: reload_with_encoding(ENC_UTF16_BE); break;
        case IDM_ENC_CP949:   reload_with_encoding(ENC_CP949); break;
        case IDM_ENC_JOHAB:   reload_with_encoding(ENC_JOHAB); break;
        case IDM_ENC_SJIS:    reload_with_encoding(ENC_SJIS); break;
        case IDM_HELP:
            cmd_show_help();
            break;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"hViewer — Win32 무료 한글 텍스트 뷰어\n"
                L"최대 64MB, 조합형/완성형 지원\n\n"
                L"https://github.com/chobocho/hviewer",
                APP_TITLE, MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_RECENT_CLEAR:
            cmd_clear_recent();
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
            } else if (id >= IDM_SEARCH_HIST_BASE &&
                       id <  IDM_SEARCH_HIST_BASE + SEARCH_HIST_MAX) {
                cmd_find_from_history(id - IDM_SEARCH_HIST_BASE);
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
        settings_save();
        if (g_state.font) DeleteObject(g_state.font);
        free(g_state.raw_data);
        free(g_state.text);
        free(g_state.line_offsets);
        free(g_state.doc_line_offsets);
        free(g_state.render_to_doc);
        free(g_state.doc_to_render);
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

    /* 상태 표시줄용 common controls 초기화 (status bar 클래스 등록) */
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
    }

    /* 최근 파일/검색 히스토리 — 메뉴 생성 전에 로드되어야 초기 메뉴에 반영됨 */
    recent_load();
    search_history_load();

    /* 큰/작은 아이콘 양쪽 로드 — 작업 표시줄과 타이틀 바에 모두 사용 */
    HICON h_big   = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON),
                                       GetSystemMetrics(SM_CYICON),
                                       LR_DEFAULTCOLOR);
    HICON h_small = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON),
                                       LR_DEFAULTCOLOR);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = h_big;
    wc.hIconSm       = h_small;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"hview_main";
    RegisterClassExW(&wc);

    /* 종료 시 저장한 윈도우 좌표/크기 복원 — 없거나 모니터 구성이 바뀌어
     * 무효한 위치면 CW_USEDEFAULT로 떨어짐 (안전한 폴백). */
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = 900, wh = 700;
    int wmax = 0;
    settings_load_window_rect(&wx, &wy, &ww, &wh, &wmax);

    HMENU menu = create_menu();
    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"hview_main", APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL,
        wx, wy, ww, wh,
        NULL, menu, hInst, NULL
    );
    if (!hwnd) return 1;

    /* 명령행 인자로 파일 받기 — `hview.exe foo.txt` 또는 Explorer 연결 프로그램.
     * CommandLineToArgvW로 따옴표/공백을 표준대로 처리하고, 상대 경로는
     * 절대 경로로 변환해 저장한다 (이후 SaveAs 등으로 cwd가 바뀌어도 안전). */
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc >= 2 && argv[1][0]) {
                wchar_t path[MAX_PATH];
                if (GetFullPathNameW(argv[1], MAX_PATH, path, NULL) > 0)
                    load_file(path, ENC_UNKNOWN);
                else
                    load_file(argv[1], ENC_UNKNOWN);
            }
            LocalFree(argv);
        }
    }

    /* 저장된 상태가 최대화면 그렇게 띄움. nCmdShow가 강제 minimized 등인 경우
     * 그쪽이 우선 (Windows shell이 주는 힌트는 존중). */
    if (wmax && nCmdShow == SW_SHOWNORMAL) nCmdShow = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
