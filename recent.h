/*
 * recent.h — 최근 파일 목록 (헤더-온리)
 *
 * HKCU\Software\hview\Recent 아래 0..9 값에 REG_SZ로 파일 경로 저장.
 * 인덱스 0 = 가장 최근. 최대 RECENT_MAX개. 같은 경로는 중복 제거 후 재정렬.
 * 의도적으로 ini 대신 레지스트리 사용 (Win32 표준, 외부 의존성 없음).
 *
 * 의존성: g_state.recent_*, g_state.recent_menu, g_state.fs_active,
 * g_state.hwnd — hview.c 단일 TU에 #include되며 g_state 정의 이후에
 * 위치해야 한다. 매크로 RECENT_MAX, RECENT_REG_PATH, IDM_RECENT_BASE,
 * IDM_RECENT_CLEAR가 hview.c 상단에 정의되어 있으므로 그보다 뒤.
 */

#ifndef RECENT_H
#define RECENT_H

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

#endif /* RECENT_H */
