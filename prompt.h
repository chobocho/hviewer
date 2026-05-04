/*
 * prompt.h — 입력 프롬프트 모달 다이얼로그 (헤더-온리, hview 단일 TU 전용)
 *
 * 줄 이동(Ctrl+G), 검색(Ctrl+F)에서 공용 사용. .rc 자원 없이 코드만으로
 * 모달 다이얼로그 구성 (외부 의존성 0 정책). 부모 윈도우 비활성화 +
 * 자체 메시지 펌프로 모달 효과 구현.
 *
 * hview.c 단일 TU에 #include 되어 사용된다 (windows.h 이후, g_state 정의는
 * 이 모듈이 직접 참조하지 않으므로 선후 무관).
 */

#ifndef PROMPT_H
#define PROMPT_H

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

#endif /* PROMPT_H */
