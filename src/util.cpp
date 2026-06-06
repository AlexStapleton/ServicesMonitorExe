// util.cpp — RAII wrappers, string helpers, DPI, admin, misc utilities
#include "util.h"

// --------------------------------------------------
// DLL cache (global)
// --------------------------------------------------
DllCache g_dll_cache;

void dll_cache_init() {
    if (g_dll_cache.inited) return;
    g_dll_cache.inited = true;
    // Prefer GetModuleHandleW (no ref-count bump) since these DLLs are already loaded
    // on DWM-enabled systems. Fall back to LoadLibraryW if not yet loaded.
    g_dll_cache.dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!g_dll_cache.dwm) g_dll_cache.dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g_dll_cache.ux = GetModuleHandleW(L"uxtheme.dll");
    if (!g_dll_cache.ux) g_dll_cache.ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g_dll_cache.user32 = GetModuleHandleW(L"user32.dll");
    if (g_dll_cache.dwm) {
        LOAD_PROC(g_dll_cache.dwm, "DwmSetWindowAttribute", PFN_DwmSetWindowAttribute, g_dll_cache.DwmSetWindowAttribute);
    }
    if (g_dll_cache.ux) {
        LOAD_PROC(g_dll_cache.ux, "SetWindowTheme", PFN_SetWindowTheme, g_dll_cache.SetWindowTheme);
    }
    if (g_dll_cache.user32) {
        g_dll_cache.GetDpiForWindow = get_proc<PFN_GetDpiForWindow>(g_dll_cache.user32, "GetDpiForWindow");
        g_dll_cache.GetDpiForSystem = get_proc<PFN_GetDpiForSystem>(g_dll_cache.user32, "GetDpiForSystem");
    }
}

// --------------------------------------------------
// Small helpers
// --------------------------------------------------
uint64_t now_mono_ms() {
    static LARGE_INTEGER freq{};
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
}

void fmt_time_local(wchar_t* buf, size_t cch, time_t t) {
    if (!t) { StringCchCopyW(buf, cch, L"\u2014"); return; }
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t tmp[64];
    wcsftime(tmp, 64, L"%Y-%m-%d %H:%M:%S", &lt);
    StringCchCopyW(buf, cch, tmp);
}

void trim_ws_inplace(wchar_t* s) {
    if (!s) return;
    size_t n = wcslen(s);
    size_t start = 0;
    while (start < n && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    size_t end = n;
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
    if (start > 0) memmove(s, s + start, (end - start + 1) * sizeof(wchar_t));
    s[end - start] = 0;
}

void str_lower_inplace(std::wstring& s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
}

void str_lower_inplace(wchar_t* s) {
    if (s && s[0]) CharLowerBuffW(s, (DWORD)wcslen(s));
}

int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool ends_with_i(const wchar_t* s, const wchar_t* suf) {
    size_t a = wcslen(s), b = wcslen(suf);
    if (b > a) return false;
    return _wcsicmp(s + (a - b), suf) == 0;
}

void msgbox_err(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONERROR | MB_OK);
}
void msgbox_info(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONINFORMATION | MB_OK);
}
void msgbox_warn(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONWARNING | MB_OK);
}

// --------------------------------------------------
// DPI helpers
// --------------------------------------------------
UINT get_dpi_for_hwnd(HWND hwnd) {
    // dll_cache_init() called once at startup; no per-call overhead here.
    if (g_dll_cache.GetDpiForWindow && hwnd) return g_dll_cache.GetDpiForWindow(hwnd);
    if (g_dll_cache.GetDpiForSystem) return g_dll_cache.GetDpiForSystem();
    return 96;
}

int dpi_scale(UINT dpi, int v96) {
    return MulDiv(v96, (int)dpi, 96);
}

void listbox_adjust_item_height(HWND lb, UINT dpi) {
    if (!lb) return;
    HDC hdc = GetDC(lb);
    if (!hdc) return;

    HFONT f = (HFONT)SendMessageW(lb, WM_GETFONT, 0, 0);
    HGDIOBJ old = NULL;
    if (f) old = SelectObject(hdc, f);

    TEXTMETRICW tm{};
    if (GetTextMetricsW(hdc, &tm)) {
        int pad = dpi_scale(dpi, 6);
        int h = tm.tmHeight + tm.tmExternalLeading + pad;
        if (h < dpi_scale(dpi, 14)) h = dpi_scale(dpi, 14);
        SendMessageW(lb, LB_SETITEMHEIGHT, 0, (LPARAM)h);
    }

    if (old) SelectObject(hdc, old);
    ReleaseDC(lb, hdc);
}

void enable_dpi_awareness() {
    // user32.dll is guaranteed to already be loaded, so GetModuleHandleW avoids
    // any LoadLibrary path entirely.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    PFN_SetProcessDpiAwarenessContext p = NULL;
    LOAD_PROC(user32, "SetProcessDpiAwarenessContext", PFN_SetProcessDpiAwarenessContext, p);

    if (p) {
        p((void*)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    FreeLibrary(user32);
}

HFONT create_ui_font_for_dpi(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    bool ok = false;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_SystemParametersInfoForDpi p = get_proc<PFN_SystemParametersInfoForDpi>(user32, "SystemParametersInfoForDpi");
        if (p) {
            ok = p(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi) ? true : false;
        }
    }
    if (!ok) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }

    LOGFONTW lf = ncm.lfMessageFont;
    StringCchCopyW(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;

    HFONT h = CreateFontIndirectW(&lf);
    return h ? h : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

void apply_font_recursive(HWND root, HFONT font) {
    if (!root || !font) return;
    SendMessageW(root, WM_SETFONT, (WPARAM)font, TRUE);
    HWND child = GetWindow(root, GW_CHILD);
    while (child) {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        apply_font_recursive(child, font);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

// --------------------------------------------------
// Child-window scaling
// --------------------------------------------------
BOOL CALLBACK enum_scale_children_proc(HWND child, LPARAM lp) {
    auto* ctx = reinterpret_cast<ScaleChildrenCtx*>(lp);
    if (!ctx || !IsWindow(child)) return TRUE;

    RECT r;
    if (!GetWindowRect(child, &r)) return TRUE;

    POINT p1{ r.left, r.top };
    POINT p2{ r.right, r.bottom };
    MapWindowPoints(NULL, ctx->parent, &p1, 1);
    MapWindowPoints(NULL, ctx->parent, &p2, 1);

    int x = p1.x;
    int y = p1.y;
    int w = p2.x - p1.x;
    int h = p2.y - p1.y;

    if (ctx->old_dpi != 0 && ctx->new_dpi != 0 && ctx->old_dpi != ctx->new_dpi) {
        x = MulDiv(x, (int)ctx->new_dpi, (int)ctx->old_dpi);
        y = MulDiv(y, (int)ctx->new_dpi, (int)ctx->old_dpi);
        w = MulDiv(w, (int)ctx->new_dpi, (int)ctx->old_dpi);
        h = MulDiv(h, (int)ctx->new_dpi, (int)ctx->old_dpi);

        SetWindowPos(child, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

        wchar_t cls[32]{0};
        GetClassNameW(child, cls, 31);
        if (lstrcmpiW(cls, L"LISTBOX") == 0) {
            LRESULT ih = SendMessageW(child, LB_GETITEMHEIGHT, 0, 0);
            if (ih > 0) {
                int new_ih = MulDiv((int)ih, (int)ctx->new_dpi, (int)ctx->old_dpi);
                if (new_ih < 8) new_ih = 8;
                SendMessageW(child, LB_SETITEMHEIGHT, 0, (LPARAM)new_ih);
            }
        }
    }
    return TRUE;
}

void scale_children(HWND parent, UINT old_dpi, UINT new_dpi) {
    if (!parent || old_dpi == 0 || new_dpi == 0 || old_dpi == new_dpi) return;
    ScaleChildrenCtx ctx{ parent, old_dpi, new_dpi };
    EnumChildWindows(parent, enum_scale_children_proc, (LPARAM)&ctx);
}

// --------------------------------------------------
// Facelift helpers
// --------------------------------------------------
void set_cue_banner(HWND edit, const wchar_t* text) {
    if (!edit || !text) return;
    SendMessageW(edit, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)text);
}

bool try_enable_mica_backdrop(HWND hwnd) {
    if (!hwnd) return false;
    dll_cache_init();
    if (!g_dll_cache.DwmSetWindowAttribute) return false;

    const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
    int backdrop = 2; // Mica
    g_dll_cache.DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    return true;
}

bool clipboard_set_text(HWND hwnd, const wchar_t* text) {
    if (!text) return false;
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();

    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) { CloseClipboard(); return false; }

    void* p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); CloseClipboard(); return false; }
    memcpy(p, text, bytes);
    GlobalUnlock(hg);

    SetClipboardData(CF_UNICODETEXT, hg);
    CloseClipboard();
    return true;
}

std::wstring to_lower_ws(const wchar_t* s) {
    if (!s) return L"";
    std::wstring out(s);
    str_lower_inplace(out);
    return out;
}

bool looks_like_path(const wchar_t* s) {
    if (!s) return false;
    return (wcschr(s, L'\\') != NULL) || (wcschr(s, L'/') != NULL) || (wcschr(s, L':') != NULL);
}

// --------------------------------------------------
// Dark titlebar / explorer theme
// --------------------------------------------------
bool try_enable_dark_titlebar(HWND hwnd, bool enable) {
    if (!hwnd) return false;
    dll_cache_init();
    if (!g_dll_cache.DwmSetWindowAttribute) return false;

    BOOL on = enable ? TRUE : FALSE;
    const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_1 = 19;
    const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_2 = 20;
    g_dll_cache.DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_1, &on, sizeof(on));
    g_dll_cache.DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_2, &on, sizeof(on));
    return true;
}

bool try_set_explorer_theme(HWND hwnd) {
    if (!hwnd) return false;
    dll_cache_init();
    if (!g_dll_cache.SetWindowTheme) return false;
    g_dll_cache.SetWindowTheme(hwnd, L"Explorer", nullptr);
    return true;
}

// --------------------------------------------------
// UIPI message filter
// --------------------------------------------------
void allow_uipi_message(HWND hwnd, UINT msg) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    FARPROC fpEx = GetProcAddress(user32, "ChangeWindowMessageFilterEx");
    if (fpEx) {
        typedef BOOL (WINAPI *PFN_CWMFE)(HWND, UINT, DWORD, PCHANGEFILTERSTRUCT);

#if defined(__clang__)
#   if __has_warning("-Wcast-function-type")
#       pragma clang diagnostic push
#       pragma clang diagnostic ignored "-Wcast-function-type"
#   endif
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        PFN_CWMFE pEx = (PFN_CWMFE)fpEx;
#if defined(__clang__)
#   if __has_warning("-Wcast-function-type")
#       pragma clang diagnostic pop
#   endif
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#   pragma GCC diagnostic pop
#endif

        CHANGEFILTERSTRUCT cfs;
        ZeroMemory(&cfs, sizeof(cfs));
        cfs.cbSize = sizeof(cfs);

        if (pEx(hwnd, msg, MSGFLT_ALLOW, &cfs)) {
            return;
        }
    }

    FARPROC fp = GetProcAddress(user32, "ChangeWindowMessageFilter");
    if (fp) {
        typedef BOOL (WINAPI *PFN_CWMF)(UINT, DWORD);

#if defined(__clang__)
#   if __has_warning("-Wcast-function-type")
#       pragma clang diagnostic push
#       pragma clang diagnostic ignored "-Wcast-function-type"
#   endif
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        PFN_CWMF p = (PFN_CWMF)fp;
#if defined(__clang__)
#   if __has_warning("-Wcast-function-type")
#       pragma clang diagnostic pop
#   endif
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#   pragma GCC diagnostic pop
#endif

        (void)p(msg, MSGFLT_ADD);
    }
}

// --------------------------------------------------
// Admin / UAC
// --------------------------------------------------
BOOL is_running_as_admin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&NtAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0,0,0,0,0,0, &adminGroup))
    {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

std::wstring escape_argv(const wchar_t* arg) {
    std::wstring out;
    out.push_back(L'"');
    for (const wchar_t* p = arg; *p; ++p) {
        size_t n_bs = 0;
        while (*p == L'\\') { ++n_bs; ++p; }
        if (*p == L'\0') {
            out.append(n_bs * 2, L'\\');
            break;
        } else if (*p == L'"') {
            out.append(n_bs * 2, L'\\');
            out.push_back(L'\\');
            out.push_back(L'"');
        } else {
            out.append(n_bs, L'\\');
            out.push_back(*p);
        }
    }
    out.push_back(L'"');
    return out;
}

BOOL relaunch_as_admin() {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return FALSE;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return FALSE;

    std::wstring params;
    for (int i = 1; i < argc; i++) {
        if (i > 1) params.push_back(L' ');
        params.append(escape_argv(argv[i]));
    }

    LocalFree(argv);

    HINSTANCE h = ShellExecuteW(NULL, L"runas", exePath, params.empty() ? NULL : params.c_str(), NULL, SW_SHOWNORMAL);
    return ((uintptr_t)h > 32);
}

void ensure_admin_or_exit() {
    if (is_running_as_admin()) return;

    if (relaunch_as_admin()) ExitProcess(0);

    MessageBoxW(NULL,
        L"This app needs Administrator privileges to stop most Windows services.\n\n"
        L"Please re-run it as Administrator (UAC prompt).\n"
        L"Tip: Right-click the EXE and choose 'Run as administrator'.",
        L"Administrator required",
        MB_ICONWARNING | MB_OK);
    ExitProcess(0);
}

// --------------------------------------------------
// Service status string
// --------------------------------------------------
const wchar_t* svc_state_to_str(DWORD st) {
    switch (st) {
    case SERVICE_RUNNING: return L"running";
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"starting";
    case SERVICE_STOP_PENDING: return L"stopping";
    case SERVICE_PAUSED: return L"paused";
    case SERVICE_PAUSE_PENDING: return L"pausing";
    case SERVICE_CONTINUE_PENDING: return L"resuming";
    default: return L"unknown";
    }
}
