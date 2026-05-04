/*
 * search_history.h — 최근 검색어 (헤더-온리)
 *
 * HKCU\Software\hview\SearchHistory 아래 0..9 값에 REG_SZ로 저장.
 * 0 = 가장 최근. recent files와 동일 패턴.
 *
 * 의존성: g_state.search_history*, g_state.search_hist_menu —
 * hview.c 단일 TU에 #include되며 g_state 정의 이후 위치에 두어야 한다.
 * 매크로 SEARCH_HIST_MAX, SEARCH_HIST_REG_PATH, IDM_SEARCH_HIST_BASE는
 * hview.c 상단에 정의되어 있으므로 #include는 그보다 뒤에 와야 한다.
 */

#ifndef SEARCH_HISTORY_H
#define SEARCH_HISTORY_H

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

#endif /* SEARCH_HISTORY_H */
