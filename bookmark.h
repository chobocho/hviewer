/*
 * bookmark.h — 책갈피 (헤더-온리)
 *
 * 줄 번호 정렬 배열로 보관. 토글 기준은 top_line(가장 위에 보이는 줄).
 * 캐럿이 없으므로 "현재 위치 = 시작 위치"로 정의.
 *
 * 영속화: HKCU\Software\hview\Bookmarks 아래 파일 경로를 값 이름으로,
 * REG_BINARY로 int 줄 번호 배열을 저장. 키 경로(value name)에는 백슬래시
 * 가 들어가도 OK (분리자는 키 path 한정). 값 이름 길이 제한은 16383자.
 *
 * 의존성:
 *   - g_state.bookmarks/.bookmark_count, g_state.line_count,
 *     g_state.render_to_doc, g_state.doc_to_render, g_state.doc_line_count,
 *     g_state.top_line, g_state.filepath, g_state.hwnd
 *   - 외부 함수: scroll_to_line (forward 선언 필요)
 *   - 매크로: MAX_BOOKMARKS, BOOKMARKS_REG_PATH
 *
 * hview.c 단일 TU에 #include되며 g_state 정의 + 위 forward 선언 이후에
 * 위치해야 한다.
 */

#ifndef BOOKMARK_H
#define BOOKMARK_H

static int bookmark_index_of(int line) {
    for (int i = 0; i < g_state.bookmark_count; i++) {
        if (g_state.bookmarks[i] == line) return i;
    }
    return -1;
}

static BOOL bookmark_has(int line) {
    return bookmark_index_of(line) >= 0;
}

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

#endif /* BOOKMARK_H */
