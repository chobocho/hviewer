/*
 * help_dialog.h — F1 단축키 도움말 다이얼로그 (헤더-온리)
 *
 * MessageBox는 가변폭 폰트 + 단일 컬럼이라 항목이 많아지면 화면 밖으로
 * 넘친다. prompt_input과 같은 코드 기반 모달 패턴으로 좌/우 두 컬럼 +
 * 모노스페이스 폰트(Consolas) 다이얼로그를 직접 띄운다. .rc 자원 의존성 0.
 *
 * 텍스트는 hview.c 상단 키 바인딩 주석과 동기 유지 (변경 시 양쪽 같이).
 *
 * 의존성: g_state(.hwnd) — hview.c 단일 TU에 #include되며 g_state 정의
 * 이후 위치에 두어야 한다.
 */

#ifndef HELP_DIALOG_H
#define HELP_DIALOG_H

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

#endif /* HELP_DIALOG_H */
