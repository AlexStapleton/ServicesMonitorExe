// ServicesExeMonitor-modernized.cpp
// Pure C Win32 (Unicode) Windows Service + EXE Monitor with Auto-Stop Policy
//
// UI modernization + safe optimizations while preserving ALL existing functionality.
//
// What changed (high level):
// - Modern font (Segoe UI) applied consistently.
// - DPI awareness (crisper UI on high-DPI / scaling).
// - Explorer theme for Tab + ListView (loaded dynamically; no new link deps).
// - Added Status Bar (uses your UI refresh setting meaningfully).
// - Reduced UI churn: ListView autosize/fill runs on a UI timer instead of every status message.
// - Optimized batch service status mapping: O(M log N) vs O(M*N) matching (safe, no behavior change).
//
// Build (MinGW-w64, C++20):
//   g++ -std=c++20 -municode -mwindows -O2 -Wall -Wextra ServicesExeMonitor-modernized.cpp -o ServicesExeMonitor.exe ^
//       -lcomctl32 -ladvapi32 -lshell32 -lgdi32 -luser32 -luxtheme -ldwmapi -lshlwapi -lcomdlg32
//
// Notes:
// - No new libraries required (uxtheme/dwmapi are loaded dynamically if present).
// - Page containers still forward WM_COMMAND/WM_NOTIFY (fix preserved).
// - Config format unchanged and compatible.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <strsafe.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <new>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <chrono>

#include <cstdint>
#include <cstddef>// --------------------------------------------------
// Small RAII helpers (Win32 handles)
// --------------------------------------------------
struct unique_handle {
    HANDLE h = NULL;
    unique_handle() = default;
    explicit unique_handle(HANDLE hh) : h(hh) {}
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&& o) noexcept : h(o.h) { o.h = NULL; }
    unique_handle& operator=(unique_handle&& o) noexcept {
        if (this != &o) { reset(); h = o.h; o.h = NULL; }
        return *this;
    }
    ~unique_handle() { reset(); }
    void reset(HANDLE hh = NULL) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = hh;
    }
    HANDLE get() const { return h; }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

struct unique_sc_handle {
    SC_HANDLE h = NULL;
    unique_sc_handle() = default;
    explicit unique_sc_handle(SC_HANDLE hh) : h(hh) {}
    unique_sc_handle(const unique_sc_handle&) = delete;
    unique_sc_handle& operator=(const unique_sc_handle&) = delete;
    unique_sc_handle(unique_sc_handle&& o) noexcept : h(o.h) { o.h = NULL; }
    unique_sc_handle& operator=(unique_sc_handle&& o) noexcept {
        if (this != &o) { reset(); h = o.h; o.h = NULL; }
        return *this;
    }
    ~unique_sc_handle() { reset(); }
    void reset(SC_HANDLE hh = NULL) {
        if (h) CloseServiceHandle(h);
        h = hh;
    }
    SC_HANDLE get() const { return h; }
    explicit operator bool() const { return h != NULL; }
};


// <stdbool.h> not needed in C++
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <cstring>   // memcpy

// Forward declarations
struct App;
struct Item;

// --------------------------------------------------
// UIPI / Explorer tray callback compatibility (elevated apps)
// If this app runs elevated (admin), Explorer is lower integrity and its
// tray callback messages can be blocked. Allow the specific messages we use.
// --------------------------------------------------
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif
#ifndef MSGFLT_ADD
#define MSGFLT_ADD 1
#endif

static void allow_uipi_message(HWND hwnd, UINT msg) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    // Prefer ChangeWindowMessageFilterEx (per-window). If it fails, fall back to
    // ChangeWindowMessageFilter (process-wide, legacy).
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
            return; // success
        }
        // If it exists but fails (e.g., bad args / older compatibility quirk), try legacy below.
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



// ----------------------------
// Constants / IDs
// ----------------------------
enum { KIND_SVC = 0, KIND_EXE = 1 };

#define APP_TITLE L"Windows Service / EXE Monitor"

// --------------------------------------------------
// App title helpers (match Task Manager name to EXE filename)
// --------------------------------------------------
static std::wstring exe_stem_title() {
    wchar_t path[MAX_PATH];
    path[0] = L'\0';
    DWORD n = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) {
        return std::wstring(APP_TITLE);
    }

    const wchar_t* file = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') file = p + 1;
    }

    std::wstring name(file);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    if (name.empty()) return std::wstring(APP_TITLE);
    return name;
}


#ifndef IDI_APPICON
#define IDI_APPICON 101
#endif

#define WM_APP_LOG              (WM_APP + 1)
#define WM_APP_STATUS           (WM_APP + 2)
#define WM_APP_REQUEST_SAVE     (WM_APP + 3)
#define WM_APP_RESTART_UI_TIMER (WM_APP + 4)

#define WM_APP_STATUS_BULK      (WM_APP + 5)
#define WM_APP_PROFILE_SWITCH   (WM_APP + 6)
#define TIMER_DEBOUNCE_SAVE  102
#define TIMER_UI_REFRESH     103

#define MAX_LOG_LINES 1000

#define LOAD_PROC(module, procName, Type, outVar)            \
    do {                                                     \
        (outVar) = NULL;                                     \
        FARPROC __fp = GetProcAddress((module), (procName)); \
        if (__fp) memcpy(&(outVar), &__fp, sizeof(outVar));  \
    } while (0)

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

// Menu IDs
#define IDM_PREFS  40001
#define IDM_HELP   40002
#define IDM_PROFILES  40008
#define IDM_EXIT   40003

// Tray menu / commands
#define IDM_TRAY_SHOWHIDE      40004
#define IDM_TRAY_PREFS         40005
#define IDM_TRAY_PROFILES      40009
#define IDM_TRAY_CLOSE_TO_TRAY 40006
#define IDM_TRAY_EXIT          40007
// Control IDs
#define IDC_LEFT_TABS      5001
#define IDC_CENTER_TABS    5002
#define IDC_STATUSBAR      5003

// Left pages
#define IDC_SVC_ADD_EDIT     5101
#define IDC_SVC_ADD_BTN      5102
#define IDC_SVC_STOP_BTN     5103
#define IDC_SVC_START_BTN    5105
#define IDC_SVC_REMOVE_BTN   5104

#define IDC_EXE_ADD_EDIT     5201
#define IDC_EXE_ADD_BTN      5202
#define IDC_EXE_STOP_BTN     5203
#define IDC_EXE_START_BTN    5206
#define IDC_EXE_REMOVE_BTN   5204
#define IDC_EXE_BROWSE_BTN   5205

// Center listviews
#define IDC_SVC_SEARCH_EDIT  6101
#define IDC_EXE_SEARCH_EDIT  6102

// Context menu
#define IDM_CTX_STOP         40100
#define IDM_CTX_REMOVE       40101
#define IDM_CTX_TOGGLE_AUTO  40102
#define IDM_CTX_COPY_NAME    40103
#define IDM_CTX_OPEN_LOC     40104

// Center listviews
#define IDC_LV_SVC  6001
#define IDC_LV_EXE  6002

// Activity listbox
#define IDC_ACTIVITY 7001

// Tray icon
#define WM_APP_TRAYICON   (WM_APP + 10)
#define TRAY_UID          1
#define IDC_SPLITTER 7002

#define SPLITTER_WIDTH 6
#define ACTIVITY_MIN_W  200
#define CENTER_MIN_W    260
// Dialog control IDs (created programmatically)
#define IDC_PREF_UI_MS        8001
#define IDC_PREF_MON_S        8002
#define IDC_PREF_CD_S         8003
#define IDC_PREF_SW_S         8004
#define IDC_PREF_APPLY        8005
#define IDC_PREF_TRAY_ENABLE  8006
#define IDC_PREF_CLOSE_TO_TRAY 8007

#define IDC_PROF_LIST             8101
#define IDC_PROF_NAME_EDIT        8102
#define IDC_PROF_ADD              8103
#define IDC_PROF_RENAME           8104
#define IDC_PROF_DELETE           8105
#define IDC_PROF_MOVE_UP          8106
#define IDC_PROF_MOVE_DOWN        8107
#define IDC_PROF_ACTIVE_LABEL     8108
#define IDC_WATCH_LIST            8201
#define IDC_WATCH_EDIT            8202
#define IDC_WATCH_ADD             8203
#define IDC_WATCH_REMOVE          8204

// Profiles dialog: profile contents (services/exes) editor
#define IDC_PCONT_SVC_LIST        8301
#define IDC_PCONT_SVC_EDIT        8302
#define IDC_PCONT_SVC_AUTOSTOP    8303
#define IDC_PCONT_SVC_ADD         8304
#define IDC_PCONT_SVC_REMOVE      8305
#define IDC_PCONT_SVC_TOGGLE      8306

#define IDC_PCONT_EXE_LIST        8311
#define IDC_PCONT_EXE_EDIT        8312
#define IDC_PCONT_EXE_AUTOSTOP    8313
#define IDC_PCONT_EXE_ADD         8314
#define IDC_PCONT_EXE_REMOVE      8315
#define IDC_PCONT_EXE_TOGGLE      8316

// Profiles dialog: EXEs to start when profile activates
#define IDC_PSTART_EXE_LIST       8321
#define IDC_PSTART_EXE_EDIT       8322
#define IDC_PSTART_EXE_ADD        8323
#define IDC_PSTART_EXE_REMOVE     8324


// Tray forward declarations
static void tray_remove(App& self);
static void tray_add(App& self, HINSTANCE hInst);
static void tray_sync(App& self, HINSTANCE hInst);
static void show_main_window(App& self);
static void hide_main_window(App& self);
static void app_request_exit(App& self, HWND hwnd);
static void tray_show_menu(HWND hwnd);

// ----------------------------
// Small helpers
// ----------------------------
static uint64_t now_mono_ms(void) {
    static LARGE_INTEGER freq{};
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
}

static void fmt_time_local(wchar_t* buf, size_t cch, time_t t) {
    if (!t) { StringCchCopyW(buf, cch, L"\u2014"); return; }
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t tmp[64];
    wcsftime(tmp, 64, L"%Y-%m-%d %H:%M:%S", &lt);
    StringCchCopyW(buf, cch, tmp);
}

static void trim_ws_inplace(wchar_t* s) {
    if (!s) return;
    size_t n = wcslen(s);
    size_t start = 0;
    while (start < n && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    size_t end = n;
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
    if (start > 0) memmove(s, s + start, (end - start + 1) * sizeof(wchar_t));
    s[end - start] = 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


static bool ends_with_i(const wchar_t* s, const wchar_t* suf) {
    size_t a = wcslen(s), b = wcslen(suf);
    if (b > a) return false;
    return _wcsicmp(s + (a - b), suf) == 0;
}

static wchar_t* wcsdup_heap(const wchar_t* s) {
    if (!s) return NULL;
    size_t n = wcslen(s);
    wchar_t* p = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

static void msgbox_err(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONERROR | MB_OK);
}
static void msgbox_info(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONINFORMATION | MB_OK);
}
static void msgbox_warn(HWND parent, const wchar_t* title, const wchar_t* text) {
    MessageBoxW(parent, text, title, MB_ICONWARNING | MB_OK);
}

// ----------------------------
// Modern UI helpers (no extra link deps)
// ----------------------------
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(void * /*DPI_AWARENESS_CONTEXT*/);
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);

// DPI helpers (loaded dynamically so the binary still runs on older Windows)
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef int  (WINAPI *PFN_GetSystemMetricsForDpi)(int, UINT);
typedef BOOL (WINAPI *PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

template <typename T>
static T get_proc(HMODULE mod, const char* name) {
    if (!mod || !name) return (T)nullptr;
    FARPROC fp = GetProcAddress(mod, name);
    // Avoid -Wcast-function-type by casting FARPROC -> void* -> function pointer.
    return reinterpret_cast<T>(reinterpret_cast<void*>(fp));
}


static UINT get_dpi_for_hwnd(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_GetDpiForWindow pW = get_proc<PFN_GetDpiForWindow>(user32, "GetDpiForWindow");
        if (pW && hwnd) return pW(hwnd);
        PFN_GetDpiForSystem pS = get_proc<PFN_GetDpiForSystem>(user32, "GetDpiForSystem");
        if (pS) return pS();
    }
    return 96;
}

[[maybe_unused]] static int sm_for_dpi(int idx, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_GetSystemMetricsForDpi p = get_proc<PFN_GetSystemMetricsForDpi>(user32, "GetSystemMetricsForDpi");
        if (p) return p(idx, dpi);
    }
    return GetSystemMetrics(idx);
}

static int dpi_scale(UINT dpi, int v96) {
    return MulDiv(v96, (int)dpi, 96);
}

static void enable_dpi_awareness(void) {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;

    PFN_SetProcessDpiAwarenessContext p = NULL;
    LOAD_PROC(user32, "SetProcessDpiAwarenessContext", PFN_SetProcessDpiAwarenessContext, p);

    if (p) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        p((void*)-4);
    }

    FreeLibrary(user32);
}

static void try_enable_dark_titlebar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;

    PFN_DwmSetWindowAttribute p = NULL;
    LOAD_PROC(dwm, "DwmSetWindowAttribute", PFN_DwmSetWindowAttribute, p);

    if (p) {
        // Common attribute IDs used in the wild; you can keep your existing logic here.
        // Example: DWMWA_USE_IMMERSIVE_DARK_MODE often 19/20 depending on Windows build.
        BOOL on = TRUE;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_1 = 19;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_2 = 20;
        p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_1, &on, sizeof(on));
        p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_2, &on, sizeof(on));
    }

    FreeLibrary(dwm);
}

static void try_set_explorer_theme(HWND hwnd) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;

    PFN_SetWindowTheme p = NULL;
    LOAD_PROC(ux, "SetWindowTheme", PFN_SetWindowTheme, p);

    if (p) {
        p(hwnd, L"Explorer", NULL);
    }

    FreeLibrary(ux);
}

static HFONT create_ui_font_for_dpi(UINT dpi) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);

    // Prefer SystemParametersInfoForDpi when available (correct per-monitor fonts)
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
    // Prefer Segoe UI (modern). If system already uses it, this matches.
    StringCchCopyW(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;

    HFONT h = CreateFontIndirectW(&lf);
    return h ? h : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void apply_font_recursive(HWND root, HFONT font) {
    if (!root || !font) return;
    SendMessageW(root, WM_SETFONT, (WPARAM)font, TRUE);
    HWND child = GetWindow(root, GW_CHILD);
    while (child) {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        apply_font_recursive(child, font);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

// ----------------------------
// Child-window scaling helper (for modeless dialogs built with fixed 96-DPI coords)
// ----------------------------
struct ScaleChildrenCtx {
    HWND parent;
    UINT old_dpi;
    UINT new_dpi;
};

static BOOL CALLBACK enum_scale_children_proc(HWND child, LPARAM lp) {
    auto* ctx = reinterpret_cast<ScaleChildrenCtx*>(lp);
    if (!ctx || !IsWindow(child)) return TRUE;

    RECT r;
    if (!GetWindowRect(child, &r)) return TRUE;

    // Convert screen -> parent client coords
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

        // ListBox needs explicit item-height scaling.
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

static void scale_children(HWND parent, UINT old_dpi, UINT new_dpi) {
    if (!parent || old_dpi == 0 || new_dpi == 0 || old_dpi == new_dpi) return;
    ScaleChildrenCtx ctx{ parent, old_dpi, new_dpi };
    EnumChildWindows(parent, enum_scale_children_proc, (LPARAM)&ctx);
}



// ----------------------------
// Facelift helpers (cue banners, backdrop, clipboard, filtering)
// ----------------------------
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

static void set_cue_banner(HWND edit, const wchar_t* text) {
    if (!edit || !text) return;
    // Works on Vista+; harmless if ignored.
    SendMessageW(edit, EM_SETCUEBANNER, (WPARAM)TRUE, (LPARAM)text);
}

static void try_enable_mica_backdrop(HWND hwnd) {
    // Best-effort Windows 11 system backdrop. Safe no-op on older builds.
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;

    PFN_DwmSetWindowAttribute p = NULL;
    LOAD_PROC(dwm, "DwmSetWindowAttribute", PFN_DwmSetWindowAttribute, p);
    if (p) {
        // DWMWA_SYSTEMBACKDROP_TYPE == 38 (Win11)
        const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
        // 2 = Mica, 3 = Acrylic, 4 = Tabbed (varies by build; Tabbed looks good for utilities)
        int backdrop = 2;
        p(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }
    FreeLibrary(dwm);
}

static bool clipboard_set_text(HWND hwnd, const wchar_t* text) {
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

static std::wstring to_lower_ws(const wchar_t* s) {
    if (!s) return L"";
    std::wstring out(s);
    if (!out.empty()) CharLowerBuffW(out.data(), (DWORD)out.size());
    return out;
}


static bool looks_like_path(const wchar_t* s) {
    if (!s) return false;
    return (wcschr(s, L'\\') != NULL) || (wcschr(s, L'/') != NULL) || (wcschr(s, L':') != NULL);
}

// Facelift helpers that depend on Item/App are implemented later (after types are defined).
static bool item_matches_filter(const Item* it, const std::wstring& flt_lower);
static void rebuild_listview_filtered(App& self, int kind);
static Item* lv_get_selected_item_ptr(App& self, int kind);
static void toggle_autostop_selected(App& self, int kind);
static void show_item_details(App& self, int kind);
static void open_file_location_best_effort(App& self, const wchar_t* exeOrPath);

// ----------------------------
// Admin / UAC self-elevation
// ----------------------------
static BOOL is_running_as_admin(void) {
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

static BOOL relaunch_as_admin(void) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return FALSE;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return FALSE;

    wchar_t params[4096];
    params[0] = 0;

    for (int i = 1; i < argc; i++) {
        if (i > 1) StringCchCatW(params, 4096, L" ");
        StringCchCatW(params, 4096, L"\"");
        StringCchCatW(params, 4096, argv[i]);
        StringCchCatW(params, 4096, L"\"");
    }

    LocalFree(argv);

    HINSTANCE h = ShellExecuteW(NULL, L"runas", exePath, params[0] ? params : NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)h > 32);
}

static void ensure_admin_or_exit(void) {
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

// ----------------------------
// Status mapping
// ----------------------------
static const wchar_t* svc_state_to_str(DWORD st) {
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

// ----------------------------
// Data model (C++ containers / RAII)
// ----------------------------
struct Item {
    int kind = KIND_SVC;                 // KIND_SVC / KIND_EXE
    std::wstring name;                   // owning
    bool auto_stop = false;
    int img = 0;                         // small icon index for ListView

    std::wstring exe_path;              // optional launch path (EXE rows)

    wchar_t  last_status[32]{};          // small fixed buffer keeps LV code simple
    time_t   last_update_wall = 0;

    uint64_t last_autostop_mono_ms = 0;
    int      autostop_count = 0;
};

struct ItemList {
    std::vector<std::unique_ptr<Item>> v;
};

// NOTE: Keep the old helper names to minimize churn elsewhere in the file.
// The implementation is now C++-idiomatic (std::unique_ptr + std::vector).

static void list_init(ItemList* L) {
    if (!L) return;
    L->v.clear();
}

static void list_free(ItemList* L) {
    if (!L) return;
    L->v.clear();
}

static Item* list_find(ItemList* L, const wchar_t* name) {
    if (!L || !name || !name[0]) return NULL;
    for (auto& up : L->v) {
        Item* it = up.get();
        if (it && _wcsicmp(it->name.c_str(), name) == 0) return it;
    }
    return NULL;
}

static bool list_push(ItemList* L, Item* it) {
    if (!L || !it) return false;
    L->v.emplace_back(it); // takes ownership
    return true;
}

static void list_remove_at(ItemList* L, size_t idx) {
    if (!L) return;
    if (idx >= L->v.size()) return;
    L->v.erase(L->v.begin() + (ptrdiff_t)idx);
}

static bool list_remove_name(ItemList* L, const wchar_t* name) {
    if (!L || !name || !name[0]) return false;
    for (size_t i = 0; i < L->v.size(); i++) {
        Item* it = L->v[i].get();
        if (it && _wcsicmp(it->name.c_str(), name) == 0) {
            list_remove_at(L, i);
            return true;
        }
    }
    return false;
}

// ----------------------------
// Profile / config snapshots
// ----------------------------
struct ItemRow {
    std::wstring name;
    bool auto_stop = false;
    std::wstring exe_path; // optional: full path for launching (EXE rows)
};

struct ConfigSnapshot {
    int ui_refresh_ms = 1000;
    int monitor_interval_ms = 1000;
    int autostop_cooldown_ms = 15000;
    int stop_wait_ms = 10000;
    bool tray_enabled = true;
    bool close_to_tray = false;

    std::vector<ItemRow> items[2]; // [KIND_SVC], [KIND_EXE]
};

struct ProfileSnapshot {
    std::wstring name;
    std::vector<std::wstring> watch_exes; // normalized lower-case exe names (e.g. "game.exe")
    std::vector<ItemRow> start_exes; // EXEs to start on activation (name + optional exe_path)
    ConfigSnapshot cfg;
};

static void cfg_clear_items(ConfigSnapshot& c) {
    c.items[KIND_SVC].clear();
    c.items[KIND_EXE].clear();
}


// Display-name cache (display -> key name)
// Switch to unordered_map for O(1) lookup and automatic memory management.
struct DispCache {
    std::unordered_map<std::wstring, std::wstring> m; // key: lowercased display name
};

static void dispcache_free(DispCache* D) {
    if (!D) return;
    D->m.clear();
}

static bool dispcache_push(DispCache* D, const wchar_t* disp_lower, const wchar_t* key) {
    if (!D || !disp_lower || !key) return false;
    D->m[disp_lower] = key;
    return true;
}

static const wchar_t* dispcache_lookup(DispCache* D, const wchar_t* disp) {
    if (!D || !disp) return NULL;

    std::wstring tmp(disp);
    if (!tmp.empty()) {
        // CharLowerBuffW mutates in-place; data() is writable in C++20 for non-const strings.
        CharLowerBuffW(tmp.data(), (DWORD)tmp.size());
    }

    auto it = D->m.find(tmp);
    if (it == D->m.end()) return NULL;
    return it->second.c_str();
}

// ----------------------------
// Action queue types

// ----------------------------
enum { ACTION_STOP = 0, ACTION_START = 1 };

struct Action {
    int kind = 0;
    int op = ACTION_STOP;
    std::wstring name;
    std::wstring reason;
    int wait_ms = 0;
};

// ----------------------------
// App State
// ----------------------------
typedef struct App {
    HWND hwnd;

    // Per-monitor DPI (for correct scaling at non-100% Windows display scaling)
    UINT dpi;

    // Title shown in Task Manager / taskbar (derived from EXE filename)
    std::wstring app_title;


    HWND left_tabs;
    HWND center_tabs;

    HWND left_page_svc;
    HWND left_page_exe;

    HWND center_page_svc;
    HWND center_page_exe;

    HWND lv_svc;
    HWND lv_exe;

    // Center search boxes (per tab)
    HWND svc_search;
    HWND exe_search;

    // Left inputs
    HWND svc_add_label;
    HWND exe_add_label;
    HWND svc_add_edit;
    HWND exe_add_edit;
    HWND exe_browse_btn;

    // ListView small icon image lists
    HIMAGELIST il_svc;
    HIMAGELIST il_exe;

    // Active filter text (lowercased)
    std::wstring filter[2];

    HWND activity;
    HWND activity_label;
    HWND statusbar;


    HWND splitter;
    int  activity_panel_w;   // resizable right panel total width
    bool splitter_dragging;
    int  splitter_drag_start_x;
    int  splitter_drag_start_w;
    HFONT ui_font;

    std::mutex mtx;
    ItemList items[2];


    uint32_t items_gen[2]; // increments on add/remove; used to refresh monitor snapshots cheaply
    DispCache disp_cache;

    // settings
    int ui_refresh_ms;
    int monitor_interval_ms;
    int autostop_cooldown_ms;
    int stop_wait_ms;


    // profiles
    ConfigSnapshot default_cfg;            // stored default config (persisted even while profiles are active)
    std::vector<ProfileSnapshot> profiles; // additional configs activated by watched .exe processes
    int active_profile;                   // -1 = default, otherwise index into `profiles`
    bool have_default_cfg;

    // EXEs that were started automatically when a profile was activated (so we can stop them when leaving that profile)
    std::unordered_set<std::wstring> profile_started_exes;


    // tray
    bool tray_enabled;
    bool close_to_tray;
    bool tray_added;
    bool force_quit;
    NOTIFYICONDATAW nid;
    UINT taskbar_restart_msg;
    wchar_t cfg_path[MAX_PATH];

    std::jthread monitor_thread;
    std::jthread action_thread;

    std::mutex action_mtx;
    std::condition_variable action_cv;
    std::deque<Action> action_q;

    bool save_pending;

    // UI throttling
    bool suppress_lv_notify;
    bool lv_needs_layout[2];
    time_t last_any_status_wall;

} App;

// --------------------------------------------------
// Final-form structure note
// - We removed the global App* and the global app() accessor.
// - Legacy code still uses self.field; we map app() -> `self` via a macro.
//   Any function that uses app() must have an `App& self` in scope.
// - The main window proc is now instance-bound via GWLP_USERDATA.
// --------------------------------------------------
#define app() (self)
// ----------------------------
// Tray icon helpers
// ----------------------------
static HICON load_app_icon(HINSTANCE hInst, int cx, int cy) {
#ifdef IDI_APPICON
    return (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, 0);
#else
    (void)hInst; (void)cx; (void)cy;
    return LoadIconW(NULL, IDI_APPLICATION);
#endif
}

static void tray_remove(App& self) {
    if (self.tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &self.nid);
        self.tray_added = false;
        ZeroMemory(&self.nid, sizeof(self.nid));
    }
}

static void tray_add(App& self, HINSTANCE hInst) {
    if (!self.hwnd) return;
    if (!self.tray_enabled) return;

    ZeroMemory(&self.nid, sizeof(self.nid));
    self.nid.cbSize = sizeof(self.nid);
    self.nid.hWnd = self.hwnd;
    self.nid.uID = TRAY_UID;
    self.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    self.nid.uCallbackMessage = WM_APP_TRAYICON;

    HICON ico = load_app_icon(hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    self.nid.hIcon = ico ? ico : LoadIconW(NULL, IDI_APPLICATION);
    StringCchCopyW(self.nid.szTip, ARRAYSIZE(self.nid.szTip), self.app_title.c_str());

    if (Shell_NotifyIconW(NIM_ADD, &self.nid)) {
        self.tray_added = true;
        self.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &self.nid);
    }
}

static void tray_sync(App& self, HINSTANCE hInst) {
    if (!self.tray_enabled) {
        tray_remove(self);
        return;
    }
    if (!self.tray_added) tray_add(self, hInst);
}

static void show_main_window(App& self) {
    HWND hwnd = self.hwnd;
    if (!hwnd) return;

    // Ensure visible (close-to-tray uses SW_HIDE)
    ShowWindow(hwnd, SW_SHOW);

    // If minimized, restore. If not, bring to front.
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    // Bring to front (foreground restrictions can otherwise make it look like "nothing happened")
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
}

static void hide_main_window(App& self) {
    if (!self.hwnd) return;
    ShowWindow(self.hwnd, SW_HIDE);
}

static void app_request_exit(App& self, HWND hwnd) {
    self.force_quit = true;
    DestroyWindow(hwnd);
}

static void tray_show_menu(HWND hwnd) {
    App& self = *reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    BOOL vis = IsWindowVisible(hwnd);

    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOWHIDE, vis ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_PREFS, L"Preferences\u2026");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_PROFILES, L"Profiles\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    UINT ctt = self.close_to_tray ? MF_CHECKED : 0;
    AppendMenuW(menu, MF_STRING | ctt, IDM_TRAY_CLOSE_TO_TRAY, L"Close to tray");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    // Required so the menu dismisses correctly
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}


// ----------------------------
// FIX: Page container window class that forwards WM_COMMAND / WM_NOTIFY
// ----------------------------
static const wchar_t* PAGE_CLASS = L"PageContainer_C";

static LRESULT CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND || msg == WM_NOTIFY) {
        HWND parent = GetParent(page);
        if (parent) return SendMessageW(parent, msg, wParam, lParam);
        return 0;
    }
    return DefWindowProcW(page, msg, wParam, lParam);
}

static void register_page_class(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PageProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = PAGE_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
}

// ----------------------------
// UI logging (via message to main thread)
// ----------------------------
typedef struct LogMsg { wchar_t* text; } LogMsg;

static void post_log(App& self, const wchar_t* text) {
    LogMsg* m = (LogMsg*)malloc(sizeof(LogMsg));
    if (!m) return;
    m->text = wcsdup_heap(text ? text : L"");
    if (!m->text) { free(m); return; }

    if (!PostMessageW(self.hwnd, WM_APP_LOG, 0, (LPARAM)m)) {
        free(m->text);
        free(m);
    }
}


typedef struct StatusMsg {
    int kind;
    wchar_t* name;
    wchar_t status[32];
    time_t wall_ts;
} StatusMsg;

[[maybe_unused]] static void post_status(App& self, int kind, const wchar_t* name, const wchar_t* status, time_t wall_ts) {
    StatusMsg* m = (StatusMsg*)malloc(sizeof(StatusMsg));
    if (!m) return;
    m->kind = kind;
    m->name = wcsdup_heap(name ? name : L"");
    if (!m->name) { free(m); return; }
    StringCchCopyW(m->status, 32, status ? status : L"unknown");
    m->wall_ts = wall_ts;

    if (!PostMessageW(self.hwnd, WM_APP_STATUS, 0, (LPARAM)m)) {
        free(m->name);
        free(m);
    }
}



// Bulk status message (Option A):
// - One PostMessage per KIND per monitor tick instead of N messages.
// - Single allocation containing entries + name strings (no per-item malloc).
typedef struct StatusBulkEntry {
    uint32_t name_off;      // byte offset from start of StatusBulkMsg to wchar_t string
    wchar_t  status[32];
} StatusBulkEntry;

typedef struct StatusBulkMsg {
    int kind;
    uint32_t n;
    time_t wall_ts;
    // Followed by: StatusBulkEntry entries[n], then wchar_t string blob
} StatusBulkMsg;

static void post_status_bulk(App& self, int kind, wchar_t** names, wchar_t (*statuses)[32], size_t n, time_t wall_ts) {
    if (!names || !statuses || n == 0) return;

    size_t entries_bytes = n * sizeof(StatusBulkEntry);
    size_t names_bytes = 0;
    for (size_t i = 0; i < n; i++) {
        if (!names[i]) continue;
        size_t L = wcslen(names[i]) + 1;
        names_bytes += L * sizeof(wchar_t);
    }

    size_t total = sizeof(StatusBulkMsg) + entries_bytes + names_bytes;
    StatusBulkMsg* m = (StatusBulkMsg*)malloc(total);
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    m->n = (uint32_t)n;
    m->wall_ts = wall_ts;

    StatusBulkEntry* e = (StatusBulkEntry*)(m + 1);
    uint8_t* base = (uint8_t*)m;
    uint8_t* cur = base + sizeof(StatusBulkMsg) + entries_bytes;

    for (size_t i = 0; i < n; i++) {
        if (!names[i]) {
            e[i].name_off = 0;
            StringCchCopyW(e[i].status, 32, L"unknown");
            continue;
        }

        e[i].name_off = (uint32_t)(cur - base);
        StringCchCopyW(e[i].status, 32, statuses[i]);

        size_t L = wcslen(names[i]) + 1;
        memcpy(cur, names[i], L * sizeof(wchar_t));
        cur += L * sizeof(wchar_t);
    }

    if (!PostMessageW(self.hwnd, WM_APP_STATUS_BULK, 0, (LPARAM)m)) {
        free(m);
    }
}

static void request_save_debounced(App& self) {
    PostMessageW(self.hwnd, WM_APP_REQUEST_SAVE, 0, 0);
}

// ----------------------------
// Process helpers
// ----------------------------
static void normalize_exe_input(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[512];
    StringCchCopyW(buf, 512, raw);
    trim_ws_inplace(buf);

    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') { buf[n - 1] = 0; memmove(buf, buf + 1, n * sizeof(wchar_t)); }
    }
    if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') { buf[n - 1] = 0; memmove(buf, buf + 1, n * sizeof(wchar_t)); }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    wchar_t exp[1024];
    DWORD got = ExpandEnvironmentStringsW(buf, exp, 1024);
    const wchar_t* s = (got > 0 && got < 1024) ? exp : buf;

    const wchar_t* base = s;
    for (const wchar_t* p = s; *p; p++) {
        if (*p == L'\\' || *p == L'/' || *p == L':') base = p + 1;
    }

    wchar_t name[512];
    StringCchCopyW(name, 512, base);
    trim_ws_inplace(name);
    if (!name[0]) return;

    if (!ends_with_i(name, L".exe")) {
        StringCchCatW(name, 512, L".exe");
    }

    CharLowerBuffW(name, (DWORD)wcslen(name));
    StringCchCopyW(out, cch_out, name);
}

static void resolve_exe_launch_path(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[1024];
    StringCchCopyW(buf, 1024, raw);
    trim_ws_inplace(buf);

    // Strip simple quotes
    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') { buf[n - 1] = 0; memmove(buf, buf + 1, n * sizeof(wchar_t)); }
    } else if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') { buf[n - 1] = 0; memmove(buf, buf + 1, n * sizeof(wchar_t)); }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    // Expand environment vars (e.g. %SystemRoot%)
    wchar_t exp[1024];
    DWORD nExp = ExpandEnvironmentStringsW(buf, exp, 1024);
    if (nExp > 0 && nExp < 1024) {
        StringCchCopyW(buf, 1024, exp);
        trim_ws_inplace(buf);
    }

    // If input looks like a path, canonicalize + validate.
    bool looks_path = (wcschr(buf, L'\\') || wcschr(buf, L'/') || wcschr(buf, L':'));
    if (looks_path) {
        wchar_t full[MAX_PATH];
        DWORD nFull = GetFullPathNameW(buf, MAX_PATH, full, NULL);
        if (nFull > 0 && nFull < MAX_PATH) {
            // Ensure .exe suffix if missing
            if (!ends_with_i(full, L".exe")) {
                StringCchCatW(full, MAX_PATH, L".exe");
            }
            if (PathFileExistsW(full)) {
                StringCchCopyW(out, cch_out, full);
                return;
            }
        }
        // If it was a path and doesn't exist, fall through to SearchPath.
    }

    // Try SearchPath on PATH (also handles names without .exe)
    wchar_t found[MAX_PATH];
    DWORD r = SearchPathW(NULL, buf, NULL, MAX_PATH, found, NULL);
    if (r > 0 && r < MAX_PATH && PathFileExistsW(found)) {
        StringCchCopyW(out, cch_out, found);
        return;
    }

    // Try with ".exe" explicitly if missing.
    if (!ends_with_i(buf, L".exe")) {
        wchar_t buf2[1024];
        StringCchCopyW(buf2, 1024, buf);
        StringCchCatW(buf2, 1024, L".exe");
        r = SearchPathW(NULL, buf2, NULL, MAX_PATH, found, NULL);
        if (r > 0 && r < MAX_PATH && PathFileExistsW(found)) {
            StringCchCopyW(out, cch_out, found);
            return;
        }
    }
}

static int process_count_by_name_lower(const wchar_t* exe_lower) {
    if (!exe_lower || !exe_lower[0]) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int count = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[260];
            StringCchCopyW(name, 260, pe.szExeFile);
            CharLowerBuffW(name, (DWORD)wcslen(name));
            if (_wcsicmp(name, exe_lower) == 0) count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}


static bool terminate_all_by_name_lower(const wchar_t* exe_lower, int* terminated, int* failed) {
    if (terminated) *terminated = 0;
    if (failed) *failed = 0;

    unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return false;

    do {
        wchar_t tmp[MAX_PATH];
        StringCchCopyW(tmp, ARRAYSIZE(tmp), pe.szExeFile);
        CharLowerBuffW(tmp, (DWORD)wcslen(tmp));

        if (wcscmp(tmp, exe_lower) == 0) {
            unique_handle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID));
            if (!h) {
                if (failed) (*failed)++;
                continue;
            }
            BOOL ok = TerminateProcess(h.get(), 1);
            if (ok) {
                if (terminated) (*terminated)++;
            } else {
                if (failed) (*failed)++;
            }
        }
    } while (Process32NextW(snap.get(), &pe));

    return true;
}


// ----------------------------
// Win32 error formatting
// ----------------------------
static void format_winerr(DWORD e, wchar_t* out, size_t cch) {
    if (!out || cch == 0) return;
    out[0] = 0;

    wchar_t sys[256];
    sys[0] = 0;

    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, sys, 256, NULL
    );
    trim_ws_inplace(sys);

    if (sys[0]) {
        StringCchPrintfW(out, cch, L"winerror %lu: %s", e, sys);
    } else {
        StringCchPrintfW(out, cch, L"winerror %lu", e);
    }
}

// ----------------------------
// Service helpers
// ----------------------------

static bool query_service_status_fast(const wchar_t* svc_name, wchar_t* out_status, size_t cch) {
    StringCchCopyW(out_status, cch, L"unknown");

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) return false;

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_QUERY_STATUS));
    if (!h) return false;

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes = 0;
    BOOL ok = QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes);
    if (!ok) return false;

    StringCchCopyW(out_status, cch, svc_state_to_str(ssp.dwCurrentState));
    return true;
}



static bool stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        return false;
    }

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp0{};
    DWORD bytes0 = 0;
    if (QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp0, sizeof(ssp0), &bytes0)) {
        if (ssp0.dwCurrentState == SERVICE_STOPPED) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }
    }

    SERVICE_STATUS st{};
    BOOL ok = ControlService(h.get(), SERVICE_CONTROL_STOP, &st);
    if (!ok) {
        DWORD e = GetLastError();

        if (e == ERROR_SERVICE_NOT_ACTIVE) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: stop refused (%s)", ebuf);
        return false;
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, L"stop requested");
        return true;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes = 0;
    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;

    while (now_mono_ms() < deadline) {
        if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes)) {
            DWORD e = GetLastError();
            wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
            StringCchPrintfW(out_result, cch, L"stop requested (status query failed: %s)", ebuf);
            return true;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        Sleep(150);
    }

    StringCchCopyW(out_result, cch, L"stop requested (still stopping or refused)");
    return true;
}

static bool start_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        return false;
    }

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: status query failed (%s)", ebuf);
        return false;
    }

    if (ssp.dwCurrentState == SERVICE_RUNNING) {
        StringCchCopyW(out_result, cch, L"already running");
        return true;
    }

    if (!StartServiceW(h.get(), 0, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_ALREADY_RUNNING) {
            StringCchCopyW(out_result, cch, L"already running");
            return true;
        }
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: start failed (%s)", ebuf);
        return false;
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, L"start requested");
        return true;
    }

    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;
    for (;;) {
        if (now_mono_ms() >= deadline) break;
        if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) break;
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            StringCchCopyW(out_result, cch, L"running");
            return true;
        }
        if (ssp.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(150);
    }

    // Final status snapshot
    if (QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) StringCchCopyW(out_result, cch, L"running");
        else if (ssp.dwCurrentState == SERVICE_START_PENDING) StringCchCopyW(out_result, cch, L"start pending");
        else StringCchCopyW(out_result, cch, L"start requested");
    } else {
        StringCchCopyW(out_result, cch, L"start requested");
    }
    return true;
}

static bool start_exe_and_wait(const wchar_t* exe_lower_name, const wchar_t* launch_path_opt, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    if (!exe_lower_name || !exe_lower_name[0]) {
        StringCchCopyW(out_result, cch, L"error: invalid name");
        return false;
    }

    if (process_count_by_name_lower(exe_lower_name) > 0) {
        StringCchCopyW(out_result, cch, L"already running");
        return true;
    }

    wchar_t path[MAX_PATH] = {0};

    if (launch_path_opt && launch_path_opt[0] && PathFileExistsW(launch_path_opt)) {
        StringCchCopyW(path, MAX_PATH, launch_path_opt);
    } else {
        // Try SearchPath on PATH using the *original* exe name; best effort.
        wchar_t found[MAX_PATH] = {0};
        DWORD r = SearchPathW(NULL, exe_lower_name, NULL, MAX_PATH, found, NULL);
        if (r > 0 && r < MAX_PATH && PathFileExistsW(found)) {
            StringCchCopyW(path, MAX_PATH, found);
        } else {
            // Try with ".exe" suffix if not present
            if (!ends_with_i(exe_lower_name, L".exe")) {
                wchar_t buf2[260];
                StringCchCopyW(buf2, 260, exe_lower_name);
                StringCchCatW(buf2, 260, L".exe");
                r = SearchPathW(NULL, buf2, NULL, MAX_PATH, found, NULL);
                if (r > 0 && r < MAX_PATH && PathFileExistsW(found)) {
                    StringCchCopyW(path, MAX_PATH, found);
                }
            }
        }
    }

    // If we still don't have a path, fall back to ShellExecute using the name.
    bool used_shell_execute = false;
    if (!path[0]) {
        HINSTANCE h = ShellExecuteW(NULL, L"open", exe_lower_name, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) {
            StringCchCopyW(out_result, cch, L"error: not found on PATH (add with full path or use Browse\u2026)");
            return false;
        }
        used_shell_execute = true;
    } else {
        // CreateProcess
        wchar_t cmd[2 * MAX_PATH + 8];
        StringCchPrintfW(cmd, _countof(cmd), L"\"%s\"", path);

        // Working directory = folder of exe
        wchar_t workdir[MAX_PATH];
        StringCchCopyW(workdir, MAX_PATH, path);
        PathRemoveFileSpecW(workdir);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            NULL,
            cmd,
            NULL, NULL,
            FALSE,
            0,
            NULL,
            workdir[0] ? workdir : NULL,
            &si,
            &pi
        );
        if (!ok) {
            DWORD e = GetLastError();
            wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
            wchar_t msg[300];
            StringCchPrintfW(msg, 300, L"error: start failed (%s)", ebuf);
            StringCchCopyW(out_result, cch, msg);
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, used_shell_execute ? L"started (ShellExecute)" : L"started");
        return true;
    }

    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;
    while (now_mono_ms() < deadline) {
        if (process_count_by_name_lower(exe_lower_name) > 0) {
            StringCchCopyW(out_result, cch, L"running");
            return true;
        }
        Sleep(150);
    }

    StringCchCopyW(out_result, cch, used_shell_execute ? L"start requested" : L"started");
    return true;
}



static bool enum_services_build_disp_cache(DispCache* out_cache) {
    dispcache_free(out_cache);

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm) return false;

    DWORD bytes_needed = 0;
    DWORD count = 0;
    DWORD resume = 0;

    std::vector<BYTE> buf(128 * 1024);

    for (;;) {
        BOOL ok = EnumServicesStatusExW(
            scm.get(),
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32,
            SERVICE_STATE_ALL,
            buf.data(),
            (DWORD)buf.size(),
            &bytes_needed,
            &count,
            &resume,
            NULL
        );

        if (ok) break;

        DWORD e = GetLastError();
        if (e != ERROR_MORE_DATA || bytes_needed == 0) return false;

        buf.resize(bytes_needed + 16 * 1024);
    }

    auto* arr = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
    for (DWORD i = 0; i < count; i++) {
        const wchar_t* disp = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
        const wchar_t* key  = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
        if (!disp[0] || !key[0]) continue;

        std::wstring d(disp);
        CharLowerBuffW(d.data(), (DWORD)d.size());
        dispcache_push(out_cache, d.c_str(), key);
    }

    return true;
}


static void resolve_service_name(App& self, const wchar_t* raw, wchar_t* out, size_t cch) {
    StringCchCopyW(out, cch, raw ? raw : L"");
    trim_ws_inplace(out);
    if (!out[0]) return;

    wchar_t st[32];
    if (query_service_status_fast(out, st, 32)) return;

    const wchar_t* key = dispcache_lookup(&self.disp_cache, out);
    if (key) { StringCchCopyW(out, cch, key); return; }

    enum_services_build_disp_cache(&self.disp_cache);
    key = dispcache_lookup(&self.disp_cache, out);
    if (key) { StringCchCopyW(out, cch, key); return; }
}

// ----------------------------
// Batch status fetch (optimized name matching)
// ----------------------------
typedef struct NameIdx {
    const wchar_t* name;
    size_t idx;
} NameIdx;

static int cmp_nameidx(const void* a, const void* b) {
    const NameIdx* A = (const NameIdx*)a;
    const NameIdx* B = (const NameIdx*)b;
    if (!A->name && !B->name) return 0;
    if (!A->name) return -1;
    if (!B->name) return 1;
    return _wcsicmp(A->name, B->name);
}

static bool binsearch_nameidx(NameIdx* arr, size_t n, const wchar_t* key, size_t* out_idx) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = _wcsicmp(arr[mid].name, key);
        if (c == 0) { *out_idx = arr[mid].idx; return true; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

static bool batch_count_processes_by_name_lower(wchar_t** names, int* out_counts, size_t n) {
    if (out_counts) {
        for (size_t i = 0; i < n; i++) out_counts[i] = 0;
    }
    if (!names || !out_counts || n == 0) return true;

    NameIdx* idx = (NameIdx*)calloc(n, sizeof(NameIdx));
    if (!idx) return false;
    for (size_t i = 0; i < n; i++) {
        idx[i].name = names[i] ? names[i] : L"";
        idx[i].idx = i;
    }
    qsort(idx, n, sizeof(NameIdx), cmp_nameidx);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { free(idx); return false; }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[260];
            StringCchCopyW(name, 260, pe.szExeFile);
            CharLowerBuffW(name, (DWORD)wcslen(name));

            size_t orig = 0;
            if (binsearch_nameidx(idx, n, name, &orig)) {
                out_counts[orig] += 1;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    free(idx);
    return true;
}

static bool batch_query_service_states(wchar_t** names, wchar_t out_status[][32], size_t n) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    DWORD cap = 256 * 1024;
    BYTE* buf = (BYTE*)malloc(cap);
    if (!buf) { CloseServiceHandle(scm); return false; }

    for (size_t i = 0; i < n; i++) StringCchCopyW(out_status[i], 32, L"unknown");

    // Build sorted index for fast matching
    NameIdx* idx = NULL;
    if (n > 0) {
        idx = (NameIdx*)calloc(n, sizeof(NameIdx));
        if (idx) {
            for (size_t i = 0; i < n; i++) { idx[i].name = names[i] ? names[i] : L""; idx[i].idx = i; }
            qsort(idx, n, sizeof(NameIdx), cmp_nameidx);
        }
    }

    DWORD bytesNeeded = 0, returned = 0, resume = 0;
    bool ok_all = true;

    for (;;) {
        bytesNeeded = returned = 0;
        BOOL ok = EnumServicesStatusExW(
            scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf, cap, &bytesNeeded, &returned, &resume, NULL
        );
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_MORE_DATA) {
                cap = bytesNeeded + 4096;
                BYTE* nb = (BYTE*)realloc(buf, cap);
                if (!nb) { ok_all = false; break; }
                buf = nb;
                continue;
            }
            ok_all = false;
            break;
        }

        ENUM_SERVICE_STATUS_PROCESSW* rows = (ENUM_SERVICE_STATUS_PROCESSW*)buf;
        for (DWORD r = 0; r < returned; r++) {
            const wchar_t* key = rows[r].lpServiceName ? rows[r].lpServiceName : L"";
            if (!key[0] || !idx) continue;

            size_t orig = 0;
            if (binsearch_nameidx(idx, n, key, &orig)) {
                DWORD st = rows[r].ServiceStatusProcess.dwCurrentState;
                StringCchCopyW(out_status[orig], 32, svc_state_to_str(st));
            }
        }

        if (resume == 0) break;
    }

    free(idx);
    free(buf);
    CloseServiceHandle(scm);
    return ok_all;
}

// ----------------------------
// ListView helpers
// ----------------------------
static void lv_set_columns(HWND lv, const wchar_t* col0, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3) {
    ListView_DeleteAllItems(lv);
    while (ListView_DeleteColumn(lv, 0)) {}

    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    c.fmt = LVCFMT_CENTER;
    c.pszText = (LPWSTR)col0;
    c.cx = 90;
    ListView_InsertColumn(lv, 0, &c);

    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)col1;
    c.cx = 380;
    ListView_InsertColumn(lv, 1, &c);

    c.pszText = (LPWSTR)col2;
    c.cx = 140;
    ListView_InsertColumn(lv, 2, &c);

    c.pszText = (LPWSTR)col3;
    c.cx = 220;
    ListView_InsertColumn(lv, 3, &c);

    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
}

static int lv_find_item_by_name(HWND lv, const wchar_t* name) {
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++) {
        wchar_t buf[512];
        ListView_GetItemText(lv, i, 1, buf, 512);
        if (_wcsicmp(buf, name) == 0) return i;
    }
    return -1;
}

static int lv_find_item_by_ptr(HWND lv, const Item* it) {
    if (!lv || !it) return -1;
    LVFINDINFOW fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)it;
    return ListView_FindItem(lv, -1, &fi);
}

static void lv_autosize_status_last(HWND lv) {
    ListView_SetColumnWidth(lv, 2, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(lv, 3, LVSCW_AUTOSIZE_USEHEADER);
}

static void lv_apply_name_fill(HWND lv) {
    RECT rc;
    GetClientRect(lv, &rc);
    int total = rc.right - rc.left;

    int w0 = ListView_GetColumnWidth(lv, 0);
    int w2 = ListView_GetColumnWidth(lv, 2);
    int w3 = ListView_GetColumnWidth(lv, 3);

    int slack = 30;
    int avail = total - (w0 + w2 + w3 + slack);
    if (avail < 160) avail = 160;
    ListView_SetColumnWidth(lv, 1, avail);
}

static void lv_set_row(App& self, HWND lv, Item* it) {
    if (!lv || !it || it->name.empty()) return;

    int idx = lv_find_item_by_ptr(lv, it);
    if (idx < 0) idx = lv_find_item_by_name(lv, it->name.c_str());

    wchar_t last[64];
    fmt_time_local(last, 64, it->last_update_wall);

    if (idx < 0) {
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = ListView_GetItemCount(lv);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(L"");
        item.lParam = (LPARAM)it;
        item.iImage = it->img;
        int row = ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, row, 1, (LPWSTR)it->name.c_str());

        std::wstring status_disp = it->last_status;
        if (lv == self.lv_exe) {
            // Indicator: EXEs that were started automatically due to an active profile.
            if (self.profile_started_exes.find(it->name) != self.profile_started_exes.end()) {
                status_disp += L" (Profile)";
            }
        }

        ListView_SetItemText(lv, row, 2, (LPWSTR)status_disp.c_str());
        ListView_SetItemText(lv, row, 3, last);

        bool prev = self.suppress_lv_notify;
        self.suppress_lv_notify = true;
        ListView_SetCheckState(lv, row, it->auto_stop ? TRUE : FALSE);
        self.suppress_lv_notify = prev;
    } else {
        // Ensure the row is bound to this Item* for O(1) future lookup
        LVITEMW pi;
        memset(&pi, 0, sizeof(pi));
        pi.mask = LVIF_PARAM | LVIF_IMAGE;
        pi.iItem = idx;
        pi.lParam = (LPARAM)it;
        pi.iImage = it->img;
        ListView_SetItem(lv, &pi);

        {
        std::wstring status_disp = it->last_status;
        if (lv == self.lv_exe) {
            if (self.profile_started_exes.find(it->name) != self.profile_started_exes.end()) {
                status_disp += L" (Profile)";
            }
        }
        ListView_SetItemText(lv, idx, 2, (LPWSTR)status_disp.c_str());
    }
        ListView_SetItemText(lv, idx, 3, last);

        BOOL cur = ListView_GetCheckState(lv, idx);
        BOOL want = it->auto_stop ? TRUE : FALSE;
        if ((cur != 0) != (want != 0)) {
            bool prev = self.suppress_lv_notify;
            self.suppress_lv_notify = true;
            ListView_SetCheckState(lv, idx, want);
            self.suppress_lv_notify = prev;
        }
    }
}


static bool lv_get_selected_name(HWND lv, wchar_t* out, size_t cch) {
    if (!lv || !out || cch == 0) return false;
    out[0] = 0;

    // Prefer explicit selection, but fall back to focused/selection-mark
    // because clicking the checkbox column doesn't always set LVIS_SELECTED.
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) sel = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
    if (sel < 0) sel = ListView_GetSelectionMark(lv);

    if (sel < 0) return false;

    ListView_GetItemText(lv, sel, 1, out, (int)cch);
    return out[0] != 0;
}
// ----------------------------
// Activity log listbox
// ----------------------------
static void activity_append(App& self, HWND lb, const wchar_t* line) {
    if (!lb) return;

    int count = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    if (count >= MAX_LOG_LINES) {
        int drop = count - MAX_LOG_LINES + 1;
        for (int i = 0; i < drop; i++) SendMessageW(lb, LB_DELETESTRING, 0, 0);
    }

    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line);

    HDC hdc = GetDC(lb);
    HFONT hf = (HFONT)SendMessageW(lb, WM_GETFONT, 0, 0);
    HFONT old = (HFONT)SelectObject(hdc, hf);

    SIZE sz{};
    GetTextExtentPoint32W(hdc, line, (int)wcslen(line), &sz);

    SelectObject(hdc, old);
    ReleaseDC(lb, hdc);

    int extra = dpi_scale(self.dpi ? self.dpi : 96, 24);
    int cur = (int)SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0);
    if (sz.cx + extra > cur) SendMessageW(lb, LB_SETHORIZONTALEXTENT, (WPARAM)(sz.cx + extra), 0);

    int newCount = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    SendMessageW(lb, LB_SETTOPINDEX, (WPARAM)std::max(0, newCount - 1), 0);
}

static void log_linef(App& self, const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(msg, 1024, fmt, ap);
    va_end(ap);

    wchar_t ts[16];
    SYSTEMTIME st;
    GetLocalTime(&st);
    StringCchPrintfW(ts, 16, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);

    wchar_t line[1200];
    StringCchPrintfW(line, 1200, L"[%s] %s", ts, msg);
    activity_append(self, self.activity, line);
}

// ----------------------------
// Facelift helpers that depend on Item/App
// ----------------------------
static bool item_matches_filter(const Item* it, const std::wstring& flt_lower) {
    if (!it) return false;
    if (flt_lower.empty()) return true;

    // Match against name
    std::wstring name_lower = it->name;
    if (!name_lower.empty()) CharLowerBuffW(name_lower.data(), (DWORD)name_lower.size());
    if (name_lower.find(flt_lower) != std::wstring::npos) return true;

    // Match against status (stored as wchar_t[32])
    std::wstring st_lower = it->last_status;
    if (!st_lower.empty()) CharLowerBuffW(st_lower.data(), (DWORD)st_lower.size());
    if (st_lower.find(flt_lower) != std::wstring::npos) return true;

    return false;
}

static void rebuild_listview_filtered(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    // Rebuild from authoritative model under lock to avoid dangling Item*.
    std::lock_guard<std::mutex> lk(self.mtx);

    const std::wstring flt = self.filter[kind];

    bool prev_suppress = self.suppress_lv_notify;
    self.suppress_lv_notify = true;

    ListView_DeleteAllItems(lv);
    for (auto& up : self.items[kind].v) {
        Item* it = up.get();
        if (!it) continue;
        if (item_matches_filter(it, flt)) {
            lv_set_row(self, lv, it);
        }
    }

    self.suppress_lv_notify = prev_suppress;

    self.lv_needs_layout[kind] = true;
}

static Item* lv_get_selected_item_ptr(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return nullptr;

    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) sel = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
    if (sel < 0) sel = ListView_GetSelectionMark(lv);
    if (sel < 0) return nullptr;

    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(lv, &it)) return nullptr;
    return reinterpret_cast<Item*>(it.lParam);
}

static void toggle_autostop_selected(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    Item* sel = lv_get_selected_item_ptr(self, kind);
    if (!sel) { msgbox_warn(self.hwnd, L"Auto-stop", L"Select a row first."); return; }

    bool now = false;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        Item* real = list_find(&self.items[kind], sel->name.c_str());
        if (!real) return;
        real->auto_stop = !real->auto_stop;
        now = real->auto_stop;
    }

    int row = lv_find_item_by_ptr(lv, sel);
    if (row >= 0) {
        bool prev = self.suppress_lv_notify;
        self.suppress_lv_notify = true;
        ListView_SetCheckState(lv, row, now ? TRUE : FALSE);
        self.suppress_lv_notify = prev;
    }

    log_linef(self, L"%s: auto-stop %s", sel->name.c_str(), now ? L"enabled" : L"disabled");
    request_save_debounced(self);
}

static void show_item_details(App& self, int kind) {
    Item* it = lv_get_selected_item_ptr(self, kind);
    if (!it) return;

    wchar_t last[64];
    fmt_time_local(last, 64, it->last_update_wall);

    wchar_t buf[1200];
    StringCchPrintfW(buf, 1200,
        L"Name: %s\n"
        L"Type: %s\n"
        L"Status: %s\n"
        L"Last update: %s\n"
        L"Auto-stop: %s\n"
        L"Auto-stop count: %d",
        it->name.c_str(),
        (kind == KIND_SVC) ? L"Service" : L"EXE",
        it->last_status,
        last,
        it->auto_stop ? L"Enabled" : L"Disabled",
        it->autostop_count);

    msgbox_info(self.hwnd, L"Details", buf);
}

static void open_file_location_best_effort(App& self, const wchar_t* exeOrPath) {
    if (!exeOrPath || !exeOrPath[0]) return;
    if (!looks_like_path(exeOrPath)) {
        msgbox_info(self.hwnd, L"Open file location",
            L"That EXE entry is not a full path.\n\nTip: add a full path (e.g., C:\\Games\\Game\\game.exe) to enable this.");
        return;
    }
    wchar_t args[1200];
    StringCchPrintfW(args, 1200, L"/select,\"%s\"", exeOrPath);
    ShellExecuteW(self.hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}


// ----------------------------
// Config path
// ----------------------------
static void compute_default_cfg_path(wchar_t* out, size_t cch) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* last = wcsrchr(exe, L'\\');
    if (last) *last = 0;
    StringCchPrintfW(out, cch, L"%s\\windows_service_monitor.cfg", exe);
}

// ----------------------------
// Config load/save (unchanged)
// ----------------------------
static bool parse_setting(App& self, const wchar_t* key, const wchar_t* val) {
    if (!key || !val) return false;

    // NOTE: Settings are stored in milliseconds (ms). For backward compatibility,
    // we still accept legacy *_s keys (seconds) and convert to ms.
    auto parse_int = [&](int* out) -> bool {
        if (!out) return false;
        // _wtoi tolerates leading/trailing whitespace and stops on non-digit.
        *out = _wtoi(val);
        return true;
    };
    auto parse_seconds_to_ms = [&](int* out_ms) -> bool {
        if (!out_ms) return false;
        double s = _wtof(val);
        if (s < 0) return false;
        // round to nearest millisecond
        long long ms = (long long)(s * 1000.0 + 0.5);
        if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
        *out_ms = (int)ms;
        return true;
    };

    if (_wcsicmp(key, L"ui_refresh_ms") == 0) {
        int v = 0;
        parse_int(&v);
        if (v >= 200) { self.ui_refresh_ms = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"monitor_interval_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { self.monitor_interval_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"monitor_interval_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms) && ms >= 200) { self.monitor_interval_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"autostop_cooldown_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { self.autostop_cooldown_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { self.autostop_cooldown_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"stop_wait_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { self.stop_wait_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { self.stop_wait_ms = ms; return true; }
        return false;
    }

    // Tray settings (were written previously but not parsed; now supported)
    if (_wcsicmp(key, L"tray_enabled") == 0) {
        int v = 0; parse_int(&v);
        self.tray_enabled = (v != 0);
        if (!self.tray_enabled) self.close_to_tray = false;
        return true;
    }
    if (_wcsicmp(key, L"close_to_tray") == 0) {
        int v = 0; parse_int(&v);
        self.close_to_tray = (v != 0);
        if (!self.tray_enabled) self.close_to_tray = false;
        return true;
    }

    return false;
}




static bool parse_setting_to_cfg(ConfigSnapshot& cfg, const wchar_t* key, const wchar_t* val) {
    if (!key || !val) return false;

    auto parse_int = [&](int* out) -> bool {
        if (!out) return false;
        *out = _wtoi(val);
        return true;
    };
    auto parse_seconds_to_ms = [&](int* out_ms) -> bool {
        if (!out_ms) return false;
        double s = _wtof(val);
        if (s < 0) return false;
        long long ms = (long long)(s * 1000.0 + 0.5);
        if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
        *out_ms = (int)ms;
        return true;
    };

    if (_wcsicmp(key, L"ui_refresh_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { cfg.ui_refresh_ms = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"monitor_interval_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { cfg.monitor_interval_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"monitor_interval_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms) && ms >= 200) { cfg.monitor_interval_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"autostop_cooldown_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { cfg.autostop_cooldown_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { cfg.autostop_cooldown_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"stop_wait_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { cfg.stop_wait_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { cfg.stop_wait_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"tray_enabled") == 0) {
        int v = 0; parse_int(&v);
        cfg.tray_enabled = (v != 0);
        if (!cfg.tray_enabled) cfg.close_to_tray = false;
        return true;
    }
    if (_wcsicmp(key, L"close_to_tray") == 0) {
        int v = 0; parse_int(&v);
        cfg.close_to_tray = (v != 0);
        if (!cfg.tray_enabled) cfg.close_to_tray = false;
        return true;
    }

    return false;
}


static void snapshot_from_runtime_locked(App& self, ConfigSnapshot& out);
static void snapshot_active_from_runtime_locked(App& self);
[[maybe_unused]] static void snapshot_active_from_runtime(App& self);
static void apply_profile_index(App& self, int idx);
static void action_enqueue(App& self, int kind, int op, const wchar_t* name, const wchar_t* reason, int wait_ms);
static void update_statusbar(App& self);

static void load_config(App& self) {
    FILE* f = NULL;
    _wfopen_s(&f, self.cfg_path, L"rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 5 * 1024 * 1024) { fclose(f); return; }
    char* bytes = (char*)malloc((size_t)sz + 1);
    if (!bytes) { fclose(f); return; }
    fread(bytes, 1, (size_t)sz, f);
    bytes[sz] = 0;
    fclose(f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes, -1, NULL, 0);
    wchar_t* wtxt = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wtxt) { free(bytes); return; }
    MultiByteToWideChar(CP_UTF8, 0, bytes, -1, wtxt, wlen);
    free(bytes);

    int restored = 0;
    bool settings_loaded = false;

// Profiles: default config is loaded as before (settings + monitored rows outside PROFILE blocks).
// Additional profiles can be defined in the cfg file like:
//   PROFILE|Gaming
//   WATCH|game.exe
//   STARTEXE|discord.exe|C:\\Path\\discord.exe
//   SETTING|autostop_cooldown_ms|5000
//   EXE|discord.exe|1
//   SVC|wuauserv|0
//   ENDPROFILE
//
// A profile activates automatically when ANY WATCH exe is running.
// If multiple profiles match, the first match in the config wins.
// When no profiles match, the app falls back to Default.
bool profiles_loaded = false;
{
    std::lock_guard<std::mutex> lk(self.mtx);
    self.profiles.clear();
    self.active_profile = -1;
    self.have_default_cfg = false;
}

bool in_profile = false;
ProfileSnapshot cur_profile;
auto commit_profile = [&]() {
    if (!in_profile) return;
    // Ensure watch exes are unique-ish (best-effort)
    std::sort(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end());
    cur_profile.watch_exes.erase(std::unique(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end()),
                                 cur_profile.watch_exes.end());

    {
        std::lock_guard<std::mutex> lk(self.mtx);
        self.profiles.push_back(std::move(cur_profile));
    }
    cur_profile = ProfileSnapshot{};
    in_profile = false;
    profiles_loaded = true;
};
auto start_profile = [&](const wchar_t* name) {
    commit_profile();
    in_profile = true;
    cur_profile = ProfileSnapshot{};
    if (name) cur_profile.name = name;

    // Inherit current app settings as the base for this profile.
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        cur_profile.cfg.ui_refresh_ms = self.ui_refresh_ms;
        cur_profile.cfg.monitor_interval_ms = self.monitor_interval_ms;
        cur_profile.cfg.autostop_cooldown_ms = self.autostop_cooldown_ms;
        cur_profile.cfg.stop_wait_ms = self.stop_wait_ms;
        cur_profile.cfg.tray_enabled = self.tray_enabled;
        cur_profile.cfg.close_to_tray = self.close_to_tray;
    }
};


    wchar_t* ctx = NULL;
    wchar_t* line = wcstok_s(wtxt, L"\n", &ctx);
    while (line) {
        trim_ws_inplace(line);
        if (!line[0] || line[0] == L'#') { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        
const wchar_t* SETP = L"SETTING|";
if (_wcsnicmp(line, SETP, wcslen(SETP)) == 0) {
    wchar_t* p = line + wcslen(SETP);
    wchar_t* bar = wcschr(p, L'|');
    if (bar) {
        *bar = 0;
        if (in_profile) {
            if (parse_setting_to_cfg(cur_profile.cfg, p, bar + 1)) settings_loaded = true;
        } else {
            if (parse_setting(self, p, bar + 1)) settings_loaded = true;
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}



// Profile blocks
const wchar_t* PROFP = L"PROFILE|";
if (_wcsnicmp(line, PROFP, wcslen(PROFP)) == 0) {
    wchar_t* pname = line + wcslen(PROFP);
    trim_ws_inplace(pname);
    start_profile(pname);
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}
if (_wcsicmp(line, L"ENDPROFILE") == 0 || _wcsicmp(line, L"PROFILE_END") == 0) {
    commit_profile();
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}
const wchar_t* WATCHP = L"WATCH|";
if (_wcsnicmp(line, WATCHP, wcslen(WATCHP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(WATCHP);
        trim_ws_inplace(p);
        wchar_t norm[512];
        normalize_exe_input(p, norm, 512);
        if (norm[0]) cur_profile.watch_exes.push_back(norm);
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}


// STARTEXE lines: EXEs to launch on profile activation
const wchar_t* STARTP = L"STARTEXE|";
if (_wcsnicmp(line, STARTP, wcslen(STARTP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(STARTP);
        trim_ws_inplace(p);

        // Optional: STARTEXE|name_or_path|fullpath
        wchar_t name_raw[1024]; name_raw[0] = 0;
        wchar_t exe_path_raw2[1024]; exe_path_raw2[0] = 0;

        wchar_t* bar = wcschr(p, L'|');
        if (bar) {
            *bar = 0;
            StringCchCopyW(name_raw, 1024, p);
            StringCchCopyW(exe_path_raw2, 1024, bar + 1);
        } else {
            StringCchCopyW(name_raw, 1024, p);
        }
        trim_ws_inplace(name_raw);
        trim_ws_inplace(exe_path_raw2);

        wchar_t norm[512]; norm[0] = 0;
        normalize_exe_input(name_raw, norm, 512);
        if (norm[0]) {
            wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
            if (exe_path_raw2[0]) {
                resolve_exe_launch_path(exe_path_raw2, exe_path_full, MAX_PATH);
            } else if (name_raw[0]) {
                // If they pasted a path into the first field, try resolving it as a launch path too.
                resolve_exe_launch_path(name_raw, exe_path_full, MAX_PATH);
            }

            bool dup = false;
            for (const auto& r : cur_profile.start_exes) {
                if (_wcsicmp(r.name.c_str(), norm) == 0) { dup = true; break; }
            }
            if (!dup) {
                ItemRow r;
                r.name = norm;
                r.auto_stop = false;
                if (exe_path_full[0]) r.exe_path = exe_path_full;
                cur_profile.start_exes.push_back(std::move(r));
            }

            // Also ensure it's present in the profile EXE list (Auto-stop OFF by default) so it's visible/managed.
            bool exists_in_cfg = false;
            for (auto& r : cur_profile.cfg.items[KIND_EXE]) {
                if (_wcsicmp(r.name.c_str(), norm) == 0) {
                    exists_in_cfg = true;
                    if (r.exe_path.empty() && exe_path_full[0]) r.exe_path = exe_path_full;
                    break;
                }
            }
            if (!exists_in_cfg) {
                ItemRow rr;
                rr.name = norm;
                rr.auto_stop = false;
                if (exe_path_full[0]) rr.exe_path = exe_path_full;
                cur_profile.cfg.items[KIND_EXE].push_back(std::move(rr));
            }
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

        int kind = KIND_SVC;
        wchar_t name[512]; name[0] = 0;
        wchar_t exe_path_raw[1024]; exe_path_raw[0] = 0; // optional EXE launch path
        int auto_stop = 0;

        if (_wcsnicmp(line, L"SVC|", 4) == 0 || _wcsnicmp(line, L"EXE|", 4) == 0) {
            kind = (_wcsnicmp(line, L"EXE|", 4) == 0) ? KIND_EXE : KIND_SVC;
            wchar_t* p = line + 4;
            wchar_t* a = wcschr(p, L'|');
            if (a) {
                *a = 0;
                StringCchCopyW(name, 512, p);

                wchar_t* b = wcschr(a + 1, L'|');
                if (b) {
                    *b = 0;
                    auto_stop = _wtoi(a + 1);
                    StringCchCopyW(exe_path_raw, 1024, b + 1);
                } else {
                    auto_stop = _wtoi(a + 1);
                }
            } else {
                StringCchCopyW(name, 512, p);
                auto_stop = 0;
            }
        } else {
            wchar_t* a = wcschr(line, L'|');
            if (a) { *a = 0; StringCchCopyW(name, 512, line); auto_stop = _wtoi(a + 1); }
            else { StringCchCopyW(name, 512, line); auto_stop = 0; }
            kind = KIND_SVC;
        }

        trim_ws_inplace(name);
        trim_ws_inplace(exe_path_raw);
        if (!name[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        wchar_t norm[512];
        if (kind == KIND_EXE) {
            normalize_exe_input(name, norm, 512);
            if (!norm[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }
        } else {
            StringCchCopyW(norm, 512, name);
        }

        wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;

        if (kind == KIND_EXE && exe_path_raw[0]) {
            resolve_exe_launch_path(exe_path_raw, exe_path_full, MAX_PATH);
        }


if (in_profile) {
    // Store into the profile snapshot instead of the live runtime list.
    bool dup = false;
    for (const auto& r : cur_profile.cfg.items[kind]) {
        if (_wcsicmp(r.name.c_str(), norm) == 0) { dup = true; break; }
    }
    if (!dup) {
        ItemRow r;
        r.name = norm;
        r.auto_stop = (auto_stop == 1);
        if (kind == KIND_EXE && exe_path_full[0]) r.exe_path = exe_path_full;
        cur_profile.cfg.items[kind].push_back(std::move(r));
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

        Item* exist = NULL;
        {
            std::lock_guard<std::mutex> lk(self.mtx);
            exist = list_find(&self.items[kind], norm);
        }
        if (exist) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        Item* it = new (std::nothrow) Item();
        if (!it) break;
        it->kind = kind;
        it->name = norm;
        if (kind == KIND_EXE && exe_path_full[0]) it->exe_path = exe_path_full;
        it->auto_stop = (auto_stop == 1);
        it->img = (kind == KIND_SVC) ? 0 : 1;
        it->autostop_count = 0;
        it->last_autostop_mono_ms = 0;
        it->last_update_wall = time(NULL);

        if (kind == KIND_SVC) {
            wchar_t st[32];
            if (query_service_status_fast(norm, st, 32)) StringCchCopyW(it->last_status, 32, st);
            else StringCchCopyW(it->last_status, 32, L"not found");
        } else {
            int c = process_count_by_name_lower(norm);
            StringCchCopyW(it->last_status, 32, (c > 0) ? L"running" : L"stopped");
        }

        {
            std::lock_guard<std::mutex> lk(self.mtx);
            list_push(&self.items[kind], it);
            self.items_gen[kind]++;
        }

        lv_set_row(self, kind == KIND_SVC ? self.lv_svc : self.lv_exe, it);

        log_linef(self, L"Monitoring restored (%s): %s (status: %s)",
                  (kind == KIND_SVC) ? L"service" : L"exe", it->name.c_str(), it->last_status);
        restored++;

        line = wcstok_s(NULL, L"\n", &ctx);
    }


// If the file ended while inside a profile block, commit it.
commit_profile();

// Persist the loaded Default config snapshot.
{
    std::lock_guard<std::mutex> lk(self.mtx);
    snapshot_from_runtime_locked(self, self.default_cfg);
    self.have_default_cfg = true;
    self.active_profile = -1;
}

    free(wtxt);

    if (restored > 0 || settings_loaded || profiles_loaded) request_save_debounced(self);

    // Restart UI timer with any loaded ui_refresh_ms
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}

typedef struct SaveRow {
    wchar_t* name;
    int auto_stop;
} SaveRow;

[[maybe_unused]] static void free_saverows(SaveRow* r, size_t n) {
    if (!r) return;
    for (size_t i = 0; i < n; i++) free(r[i].name);
    free(r);
}


static void save_config_now(App& self) {
    wchar_t cfg_path[MAX_PATH] = {0};

    ConfigSnapshot def;
    std::vector<ProfileSnapshot> profs;

    // Snapshot active runtime back into its owning snapshot, then copy out everything needed for saving.
    {
        std::lock_guard<std::mutex> lk(self.mtx);

        // Ensure we have a default snapshot even if config was never loaded (fresh run).
        if (!self.have_default_cfg) {
            snapshot_from_runtime_locked(self, self.default_cfg);
            self.have_default_cfg = true;
            self.active_profile = -1;
        }

        snapshot_active_from_runtime_locked(self);

        def = self.default_cfg;
        profs = self.profiles;

        StringCchCopyW(cfg_path, MAX_PATH, self.cfg_path);
    }

    wchar_t tmp[MAX_PATH];
    StringCchPrintfW(tmp, MAX_PATH, L"%s.tmp", cfg_path);

    FILE* f = NULL;
    _wfopen_s(&f, tmp, L"wb");
    if (!f) return;

    // Header + default settings
    char header[4096];
    int n = snprintf(header, sizeof(header),
        "SETTING|ui_refresh_ms|%d\n"
        "SETTING|monitor_interval_ms|%d\n"
        "SETTING|autostop_cooldown_ms|%d\n"
        "SETTING|stop_wait_ms|%d\n"
        "SETTING|tray_enabled|%d\n"
        "SETTING|close_to_tray|%d\n"
        "# Lines:\n"
        "#   SVC|<service_key_name>|<auto_stop 0/1>\n"
        "#   EXE|<exe_name>|<auto_stop 0/1>\n"
        "# Profiles:\n"
        "#   PROFILE|<name>\n"
        "#   WATCH|<exe_name>            (any WATCH running activates the profile)\n"
        "#   SETTING|<key>|<val>         (optional overrides for this profile)\n"
        "#   SVC|...\n"
        "#   EXE|...\n"
        "#   ENDPROFILE\n",
        def.ui_refresh_ms, def.monitor_interval_ms, def.autostop_cooldown_ms, def.stop_wait_ms,
        def.tray_enabled ? 1 : 0, def.close_to_tray ? 1 : 0
    );
    fwrite(header, 1, (size_t)n, f);

    auto write_wline = [&](const wchar_t* wline) {
        int blen = WideCharToMultiByte(CP_UTF8, 0, wline, -1, NULL, 0, NULL, NULL);
        if (blen <= 0) return;
        char* b = (char*)malloc((size_t)blen);
        if (!b) return;
        WideCharToMultiByte(CP_UTF8, 0, wline, -1, b, blen, NULL, NULL);
        fwrite(b, 1, (size_t)(blen - 1), f);
        free(b);
    };

    // Default items
    for (const auto& r : def.items[KIND_SVC]) {
        if (r.name.empty()) continue;
        wchar_t wline[700];
        StringCchPrintfW(wline, 700, L"SVC|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
        write_wline(wline);
    }
    for (const auto& r : def.items[KIND_EXE]) {
        if (r.name.empty()) continue;
        wchar_t wline[700];
        if (!r.exe_path.empty()) {
            StringCchPrintfW(wline, 700, L"EXE|%s|%d|%s\n", r.name.c_str(), r.auto_stop ? 1 : 0, r.exe_path.c_str());
        } else {
            StringCchPrintfW(wline, 700, L"EXE|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
        }
        write_wline(wline);
    }

    // Profiles
    for (const auto& p : profs) {
        if (p.name.empty()) continue;

        wchar_t wline[700];
        StringCchPrintfW(wline, 700, L"\nPROFILE|%s\n", p.name.c_str());
        write_wline(wline);

        for (const auto& w : p.watch_exes) {
            if (w.empty()) continue;
            StringCchPrintfW(wline, 700, L"WATCH|%s\n", w.c_str());
            write_wline(wline);
        }

        // EXEs to start on profile activation
        for (const auto& r : p.start_exes) {
            if (r.name.empty()) continue;
            if (!r.exe_path.empty()) {
                StringCchPrintfW(wline, 700, L"STARTEXE|%s|%s\n", r.name.c_str(), r.exe_path.c_str());
            } else {
                StringCchPrintfW(wline, 700, L"STARTEXE|%s\n", r.name.c_str());
            }
            write_wline(wline);
        }



        // Emit profile settings explicitly (simple + predictable). Since profile cfg inherits defaults on load,
        // saving them makes the profile self-contained and resilient to future default changes.
        StringCchPrintfW(wline, 700, L"SETTING|ui_refresh_ms|%d\n", p.cfg.ui_refresh_ms); write_wline(wline);
        StringCchPrintfW(wline, 700, L"SETTING|monitor_interval_ms|%d\n", p.cfg.monitor_interval_ms); write_wline(wline);
        StringCchPrintfW(wline, 700, L"SETTING|autostop_cooldown_ms|%d\n", p.cfg.autostop_cooldown_ms); write_wline(wline);
        StringCchPrintfW(wline, 700, L"SETTING|stop_wait_ms|%d\n", p.cfg.stop_wait_ms); write_wline(wline);
        StringCchPrintfW(wline, 700, L"SETTING|tray_enabled|%d\n", p.cfg.tray_enabled ? 1 : 0); write_wline(wline);
        StringCchPrintfW(wline, 700, L"SETTING|close_to_tray|%d\n", p.cfg.close_to_tray ? 1 : 0); write_wline(wline);

        for (const auto& r : p.cfg.items[KIND_SVC]) {
            if (r.name.empty()) continue;
            StringCchPrintfW(wline, 700, L"SVC|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
            write_wline(wline);
        }
        for (const auto& r : p.cfg.items[KIND_EXE]) {
            if (r.name.empty()) continue;
            if (!r.exe_path.empty()) {
                StringCchPrintfW(wline, 700, L"EXE|%s|%d|%s\n", r.name.c_str(), r.auto_stop ? 1 : 0, r.exe_path.c_str());
            } else {
                StringCchPrintfW(wline, 700, L"EXE|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
            }
            write_wline(wline);
        }

        write_wline(L"ENDPROFILE\n");
    }

    fclose(f);
    MoveFileExW(tmp, cfg_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}


// ----------------------------
// Profile helpers
// ----------------------------
static void snapshot_from_runtime_locked(App& self, ConfigSnapshot& out) {
    out.ui_refresh_ms = self.ui_refresh_ms;
    out.monitor_interval_ms = self.monitor_interval_ms;
    out.autostop_cooldown_ms = self.autostop_cooldown_ms;
    out.stop_wait_ms = self.stop_wait_ms;
    out.tray_enabled = self.tray_enabled;
    out.close_to_tray = self.close_to_tray;

    cfg_clear_items(out);

    for (int kind = 0; kind < 2; kind++) {
        out.items[kind].reserve(self.items[kind].v.size());
        for (auto& up : self.items[kind].v) {
            Item* it = up.get();
            if (!it || it->name.empty()) continue;
            ItemRow r;
            r.name = it->name;
            r.auto_stop = it->auto_stop;
            if (kind == KIND_EXE && !it->exe_path.empty()) r.exe_path = it->exe_path;
            out.items[kind].push_back(std::move(r));
        }
    }
}

static void snapshot_active_from_runtime_locked(App& self) {
    if (self.active_profile >= 0 && self.active_profile < (int)self.profiles.size()) {
        snapshot_from_runtime_locked(self, self.profiles[(size_t)self.active_profile].cfg);
    } else {
        snapshot_from_runtime_locked(self, self.default_cfg);
        self.have_default_cfg = true;
    }
}

static void snapshot_active_from_runtime(App& self) {
    std::lock_guard<std::mutex> lk(self.mtx);
    snapshot_active_from_runtime_locked(self);
}

static void rebuild_runtime_items_locked(App& self, const ConfigSnapshot& cfg) {
    list_free(&self.items[KIND_SVC]);
    list_free(&self.items[KIND_EXE]);

    for (int kind = 0; kind < 2; kind++) {
        for (const auto& row : cfg.items[kind]) {
            if (row.name.empty()) continue;

            Item* it = new (std::nothrow) Item();
            if (!it) continue;

            it->kind = kind;
            it->name = row.name;
            if (kind == KIND_EXE && !row.exe_path.empty()) it->exe_path = row.exe_path;
            it->auto_stop = row.auto_stop;
            it->img = (kind == KIND_SVC) ? 0 : 1;
            it->autostop_count = 0;
            it->last_autostop_mono_ms = 0;
            it->last_update_wall = time(NULL);

            if (kind == KIND_SVC) {
                wchar_t st[32];
                if (query_service_status_fast(it->name.c_str(), st, 32)) StringCchCopyW(it->last_status, 32, st);
                else StringCchCopyW(it->last_status, 32, L"not found");
            } else {
                int c = process_count_by_name_lower(it->name.c_str());
                StringCchCopyW(it->last_status, 32, (c > 0) ? L"running" : L"stopped");
            }

            list_push(&self.items[kind], it);
        }
    }

    self.items_gen[KIND_SVC]++;
    self.items_gen[KIND_EXE]++;
}

static const wchar_t* active_profile_name_locked(const App& self) {
    if (self.active_profile >= 0 && self.active_profile < (int)self.profiles.size()) {
        const auto& p = self.profiles[(size_t)self.active_profile].name;
        return p.empty() ? L"(unnamed)" : p.c_str();
    }
    return L"Default";
}

// UI thread: apply config snapshot to runtime + listviews
static void apply_profile_index(App& self, int idx) {
    // idx: -1 = default, 0..N-1 = profiles
    std::vector<Item*> svc_ptrs;
    std::vector<Item*> exe_ptrs;
    std::wstring pname;

    // Exes that were started automatically by the *previous* active profile (stop them when leaving)
    std::vector<std::wstring> to_stop_started;

    // Exes to start for the *new* profile activation (only EXE rows with Auto-stop OFF)
    std::vector<std::wstring> to_start;

    {
        std::lock_guard<std::mutex> lk(self.mtx);

        // Before switching away, persist any runtime edits into the currently active snapshot.
        // (So your adds/removes/checkboxes/preferences edits are stored under the profile you edited.)
        snapshot_active_from_runtime_locked(self);

        // Leaving any profile (or default): stop anything we previously started due to profile activation.
        if (!self.profile_started_exes.empty()) {
            to_stop_started.reserve(self.profile_started_exes.size());
            for (const auto& n : self.profile_started_exes) to_stop_started.push_back(n);
            self.profile_started_exes.clear();
        }

        self.active_profile = idx;
        pname = active_profile_name_locked(self);

        const ConfigSnapshot* cfg = &self.default_cfg;
        if (idx >= 0 && idx < (int)self.profiles.size()) cfg = &self.profiles[(size_t)idx].cfg;

        // Determine which EXEs should be started when a profile is activated.
// Policy: Only EXEs listed in the profile's "Start EXEs on activation" section are launched.
// We only record EXEs that we actually started (if already running, we don't mark it as profile-started).
if (idx >= 0 && idx < (int)self.profiles.size()) {
    for (const auto& r : self.profiles[(size_t)idx].start_exes) {
        if (r.name.empty()) continue;

        // If the EXE is currently set to auto-stop in the active cfg, don't start it (it would be stopped immediately).
        bool auto_stop_now = false;
        for (const auto& rr : cfg->items[KIND_EXE]) {
            if (_wcsicmp(rr.name.c_str(), r.name.c_str()) == 0) { auto_stop_now = rr.auto_stop; break; }
        }
        if (auto_stop_now) continue;

        if (process_count_by_name_lower(r.name.c_str()) > 0) continue;

        // Ensure the runtime EXE item carries a launch path if we have one.
        // (ACTION_START will look up exe_path from the runtime item list.)
        if (!r.exe_path.empty()) {
            Item* it = list_find(&self.items[KIND_EXE], r.name.c_str());
            if (it && it->exe_path.empty()) it->exe_path = r.exe_path;
        }

        to_start.push_back(r.name);
    }
    // We'll record these after the switch so we can stop them when leaving the profile.
}
We'll record these after the switch so we can stop them when leaving the profile.
            for (const auto& n : to_start) self.profile_started_exes.insert(n);
        }

        // Apply settings
        self.ui_refresh_ms = cfg->ui_refresh_ms;
        self.monitor_interval_ms = cfg->monitor_interval_ms;
        self.autostop_cooldown_ms = cfg->autostop_cooldown_ms;
        self.stop_wait_ms = cfg->stop_wait_ms;
        self.tray_enabled = cfg->tray_enabled;
        self.close_to_tray = cfg->tray_enabled && cfg->close_to_tray;

        rebuild_runtime_items_locked(self, *cfg);

        // Snapshot Item* for list view updates outside the lock
        svc_ptrs.reserve(self.items[KIND_SVC].v.size());
        for (auto& up : self.items[KIND_SVC].v) if (up) svc_ptrs.push_back(up.get());

        exe_ptrs.reserve(self.items[KIND_EXE].v.size());
        for (auto& up : self.items[KIND_EXE].v) if (up) exe_ptrs.push_back(up.get());
    }

    // Stop any EXEs that were started by the previously active profile.
    // Do this outside the lock so we don't block UI / monitor thread.
    for (const auto& n : to_stop_started) {
        action_enqueue(self, KIND_EXE, ACTION_STOP, n.c_str(), L"profile deactivated (stop started exe)", self.stop_wait_ms);
    }

    // Start EXEs for the newly activated profile.
    // Start happens on the worker thread; we don't block UI.
    if (idx >= 0) {
        for (const auto& n : to_start) {
            action_enqueue(self, KIND_EXE, ACTION_START, n.c_str(), L"profile activated (start exe)", 0);
        }
    }

    // Update tray immediately if needed
    tray_sync(self, GetModuleHandleW(NULL));

    // Refresh listviews
    rebuild_listview_filtered(self, KIND_SVC);
    rebuild_listview_filtered(self, KIND_EXE);

    self.lv_needs_layout[KIND_SVC] = true;
    self.lv_needs_layout[KIND_EXE] = true;
    update_statusbar(self);

    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
    request_save_debounced(self);

    if (idx >= 0) {
        log_linef(self, L"Profile activated: %s", pname.c_str());
    } else {
        log_linef(self, L"Profile deactivated: back to Default");
    }
}


// ----------------------------
// Action worker queue
// ----------------------------
static void action_enqueue(App& self, int kind, int op, const wchar_t* name, const wchar_t* reason, int wait_ms) {
    if (!name || !name[0]) return;

    Action a;
    a.kind = kind;
    a.op = op;
    a.name = name;
    a.reason = reason ? reason : L"";
    a.wait_ms = wait_ms;

    {
        std::lock_guard<std::mutex> lk(self.action_mtx);
        self.action_q.emplace_back(std::move(a));
    }
    self.action_cv.notify_one();
}

static void action_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    for (;;) {
        Action a;
        {
            std::unique_lock<std::mutex> lk(self.action_mtx);
            self.action_cv.wait(lk, [&] { return st.stop_requested() || !self.action_q.empty(); });
            if (st.stop_requested()) break;

            a = std::move(self.action_q.front());
            self.action_q.pop_front();
        }

        if (a.kind == KIND_SVC) {
            wchar_t result[256];
            if (a.op == ACTION_START) {
                start_service_and_wait(a.name.c_str(), a.wait_ms, result, 256);
            } else {
                stop_service_and_wait(a.name.c_str(), a.wait_ms, result, 256);
            }

            wchar_t line[700];
            StringCchPrintfW(line, 700, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(self, line);
        } else {
            if (a.op == ACTION_START) {
                std::wstring launch;
                {
                    std::lock_guard<std::mutex> lk(self.mtx);
                    Item* it = list_find(&self.items[KIND_EXE], a.name.c_str());
                    if (it) launch = it->exe_path;
                }
                wchar_t result[256];
                start_exe_and_wait(a.name.c_str(), launch.c_str(), a.wait_ms, result, 256);

                wchar_t line[700];
                StringCchPrintfW(line, 700, L"%s: %s → %s",
                                 a.name.c_str(), a.reason.c_str(), result);
                post_log(self, line);
                continue;
            }

            int terminated = 0, failed = 0;
            bool ok = terminate_all_by_name_lower(a.name.c_str(), &terminated, &failed);

            wchar_t result[256];
            if (!ok) {
                StringCchCopyW(result, 256, L"error: terminate failed");
            } else {
                if (terminated == 0 && failed == 0) {
                    wchar_t cmd[512];
                    StringCchPrintfW(cmd, 512, L"taskkill /IM \"%s\" /F >NUL 2>NUL", a.name.c_str());
                    _wsystem(cmd);
                    terminated = 1;
                }
                if (a.wait_ms > 0) {
                    uint64_t deadline = now_mono_ms() + (uint64_t)a.wait_ms;
                    while (!st.stop_requested() && now_mono_ms() < deadline) {
                        if (process_count_by_name_lower(a.name.c_str()) == 0) break;
                        Sleep(150);
                    }
                }
                if (failed && !terminated) {
                    StringCchPrintfW(result, 256, L"error: failed to terminate (%d)", failed);
                } else if (failed) {
                    StringCchPrintfW(result, 256, L"terminated (%d), failed (%d)", terminated, failed);
                } else {
                    StringCchPrintfW(result, 256, L"terminated (%d)", std::max(1, terminated));
                }
            }

            wchar_t line[800];
            StringCchPrintfW(line, 800, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(self, line);
        }
    }

    // Best-effort drop any remaining queued work on shutdown (old behavior: pending actions were freed/dropped).
    {
        std::lock_guard<std::mutex> lk(self.action_mtx);
        self.action_q.clear();
    }
}

// ----------------------------
// Monitor thread
// ----------------------------
static bool should_skip_autostop_for_service_status(const wchar_t* status) {
    if (!status) return true;
    return (_wcsicmp(status, L"unknown") == 0) ||
           (_wcsicmp(status, L"not found") == 0) ||
           (_wcsicmp(status, L"stopping") == 0) ||
           (_wcsicmp(status, L"starting") == 0) ||
           (_wcsicmp(status, L"resuming") == 0) ||
           (_wcsicmp(status, L"pausing") == 0);
}


static void free_name_array(wchar_t** a, size_t n) {
    if (!a) return;
    for (size_t i = 0; i < n; i++) free(a[i]);
}

typedef struct MonSnap {
    size_t cap_s, cap_e;
    size_t ns, ne;
    uint32_t gen_s, gen_e;

    wchar_t** svc_names;
    wchar_t** exe_names;

    bool* svc_auto;
    bool* exe_auto;

    uint64_t* svc_lastreq;
    uint64_t* exe_lastreq;
} MonSnap;

typedef struct MonScratch {
    size_t cap_s, cap_e;
    wchar_t (*svc_status)[32];
    wchar_t (*exe_status)[32];
    int* exe_counts;
} MonScratch;

static void monsnap_free(MonSnap* s) {
    if (!s) return;
    free_name_array(s->svc_names, s->ns);
    free_name_array(s->exe_names, s->ne);
    free(s->svc_names); free(s->exe_names);
    free(s->svc_auto);  free(s->exe_auto);
    free(s->svc_lastreq); free(s->exe_lastreq);
    memset(s, 0, sizeof(*s));
}


static bool mons_ensure_snap(MonSnap* s, size_t ns, size_t ne) {
    if (ns > s->cap_s) {
        size_t old = s->cap_s;
        size_t nc = (s->cap_s == 0) ? 16 : (s->cap_s * 2);
        while (nc < ns) nc *= 2;

        wchar_t** new_names   = (wchar_t**)realloc(s->svc_names,   nc * sizeof(wchar_t*));
        bool*     new_auto    = (bool*)realloc(s->svc_auto,        nc * sizeof(bool));
        uint64_t* new_lastreq = (uint64_t*)realloc(s->svc_lastreq, nc * sizeof(uint64_t));

        if (!new_names || !new_auto || !new_lastreq) {
            if (new_names)   s->svc_names = new_names;
            if (new_auto)    s->svc_auto = new_auto;
            if (new_lastreq) s->svc_lastreq = new_lastreq;
            return false;
        }

        s->svc_names = new_names;
        s->svc_auto = new_auto;
        s->svc_lastreq = new_lastreq;
        for (size_t i = old; i < nc; i++) s->svc_names[i] = NULL;
        s->cap_s = nc;
    }

    if (ne > s->cap_e) {
        size_t old = s->cap_e;
        size_t nc = (s->cap_e == 0) ? 16 : (s->cap_e * 2);
        while (nc < ne) nc *= 2;

        wchar_t** new_names   = (wchar_t**)realloc(s->exe_names,   nc * sizeof(wchar_t*));
        bool*     new_auto    = (bool*)realloc(s->exe_auto,        nc * sizeof(bool));
        uint64_t* new_lastreq = (uint64_t*)realloc(s->exe_lastreq, nc * sizeof(uint64_t));

        if (!new_names || !new_auto || !new_lastreq) {
            if (new_names)   s->exe_names = new_names;
            if (new_auto)    s->exe_auto = new_auto;
            if (new_lastreq) s->exe_lastreq = new_lastreq;
            return false;
        }

        s->exe_names = new_names;
        s->exe_auto = new_auto;
        s->exe_lastreq = new_lastreq;
        for (size_t i = old; i < nc; i++) s->exe_names[i] = NULL;
        s->cap_e = nc;
    }

    return true;
}



static bool mons_ensure_scratch(MonScratch* sc, size_t ns, size_t ne) {
    if (ns > sc->cap_s) {
        size_t nc = (sc->cap_s == 0) ? 16 : (sc->cap_s * 2);
        while (nc < ns) nc *= 2;
        wchar_t (*p)[32] = (wchar_t (*)[32])realloc(sc->svc_status, nc * sizeof(wchar_t[32]));
        if (!p) return false;
        sc->svc_status = p;
        sc->cap_s = nc;
    }
    if (ne > sc->cap_e) {
        size_t nc = (sc->cap_e == 0) ? 16 : (sc->cap_e * 2);
        while (nc < ne) nc *= 2;

        wchar_t (*st)[32] = (wchar_t (*)[32])realloc(sc->exe_status, nc * sizeof(wchar_t[32]));
        int* cnt = (int*)realloc(sc->exe_counts, nc * sizeof(int));
        if (!st || !cnt) {
            if (st) sc->exe_status = st;
            if (cnt) sc->exe_counts = cnt;
            return false;
        }
        sc->exe_status = st;
        sc->exe_counts = cnt;
        sc->cap_e = nc;
    }
    return true;
}


static bool sleep_ms_cooperative(std::stop_token st, DWORD total_ms) {
    const DWORD step = 50;
    DWORD slept = 0;
    while (slept < total_ms && !st.stop_requested()) {
        DWORD chunk = (total_ms - slept < step) ? (total_ms - slept) : step;
        Sleep(chunk);
        slept += chunk;
    }
    return st.stop_requested();
}

static void monitor_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    MonSnap snap;
    MonScratch sc;
    memset(&snap, 0, sizeof(snap));
    memset(&sc, 0, sizeof(sc));

    for (;;) {
        if (st.stop_requested()) break;


// Profile auto-activation based on watched .exe processes.
// If multiple profiles match, the first match (config order) wins.
{
    int cur = -1;
    std::vector<std::vector<std::wstring>> watches;

    {
        std::lock_guard<std::mutex> lk(self.mtx);
        cur = self.active_profile;
        watches.reserve(self.profiles.size());
        for (const auto& p : self.profiles) watches.push_back(p.watch_exes);
    }

    int desired = -1;
    for (size_t i = 0; i < watches.size(); i++) {
        const auto& ws = watches[i];
        for (const auto& exe : ws) {
            if (exe.empty()) continue;
            if (process_count_by_name_lower(exe.c_str()) > 0) { desired = (int)i; break; }
        }
        if (desired >= 0) break;
    }

    if (desired != cur) {
        PostMessageW(self.hwnd, WM_APP_PROFILE_SWITCH, (WPARAM)desired, 0);
    }
}


        int monitor_interval_ms = 1000;
        int cooldown_ms = 0;
        int stop_wait_ms = 0;

        size_t ns = 0, ne = 0;
        uint32_t gen_s = 0, gen_e = 0;

        bool ok_alloc = false;

        // Snapshot pointers + cheap fields under lock.
        {
            std::lock_guard<std::mutex> lk(self.mtx);

            monitor_interval_ms = self.monitor_interval_ms;
            cooldown_ms = self.autostop_cooldown_ms;
            stop_wait_ms = self.stop_wait_ms;

            ns = self.items[KIND_SVC].v.size();
            ne = self.items[KIND_EXE].v.size();
            gen_s = self.items_gen[KIND_SVC];
            gen_e = self.items_gen[KIND_EXE];

            ok_alloc = mons_ensure_snap(&snap, ns, ne) && mons_ensure_scratch(&sc, ns, ne);
            if (ok_alloc) {
                // Refresh name copies ONLY if list changed (add/remove), but always refresh auto/lastreq.
                if (gen_s != snap.gen_s || ns != snap.ns) {
                    // Free old names in range [0, snap.ns)
                    free_name_array(snap.svc_names, snap.ns);
                    for (size_t i = 0; i < ns; i++) {
                        Item* it = self.items[KIND_SVC].v[i].get();
                        snap.svc_names[i] = (it && !it->name.empty()) ? wcsdup_heap(it->name.c_str()) : NULL;
                    }
                    // Null out any stale pointers if we shrank
                    for (size_t i = ns; i < snap.ns; i++) snap.svc_names[i] = NULL;

                    snap.ns = ns;
                    snap.gen_s = gen_s;
                } else {
                    snap.ns = ns;
                }

                if (gen_e != snap.gen_e || ne != snap.ne) {
                    free_name_array(snap.exe_names, snap.ne);
                    for (size_t i = 0; i < ne; i++) {
                        Item* it = self.items[KIND_EXE].v[i].get();
                        snap.exe_names[i] = (it && !it->name.empty()) ? wcsdup_heap(it->name.c_str()) : NULL;
                    }
                    // Null out stale pointers if we shrank
                    for (size_t i = ne; i < snap.ne; i++) snap.exe_names[i] = NULL;

                    snap.ne = ne;
                    snap.gen_e = gen_e;
                } else {
                    snap.ne = ne;
                }

                for (size_t i = 0; i < ns; i++) {
                    Item* it = self.items[KIND_SVC].v[i].get();
                    snap.svc_auto[i] = it ? it->auto_stop : false;
                    snap.svc_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                }
                for (size_t i = 0; i < ne; i++) {
                    Item* it = self.items[KIND_EXE].v[i].get();
                    snap.exe_auto[i] = it ? it->auto_stop : false;
                    snap.exe_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                }
            }
        }

        if (!ok_alloc) {
            // If we can't allocate snapshot/scratch buffers, skip this cycle.
            post_log(self, L"Monitor: out of memory (skipping cycle)");
            DWORD wait_ms = (DWORD)std::max(50, monitor_interval_ms);
            if (sleep_ms_cooperative(st, wait_ms)) break;
            continue;
        }

        time_t wall_now = time(NULL);
        uint64_t mono_now = now_mono_ms();
        uint64_t cd_ms = (uint64_t)cooldown_ms;

        // --- Services: batch query via EnumServicesStatusEx + per-item fallback ---
        if (ns && snap.svc_names && sc.svc_status) {
            // initialize to unknown so post_status_bulk has something even on failure
            for (size_t i = 0; i < ns; i++) StringCchCopyW(sc.svc_status[i], 32, L"unknown");

            bool ok = batch_query_service_states(snap.svc_names, sc.svc_status, ns);
            if (!ok) post_log(self, L"Monitor: batch service enum failed");

            for (size_t i = 0; i < ns; i++) {
                if (!snap.svc_names[i]) continue;

                if (_wcsicmp(sc.svc_status[i], L"unknown") == 0) {
                    wchar_t stbuf[32];
                    if (query_service_status_fast(snap.svc_names[i], stbuf, 32)) StringCchCopyW(sc.svc_status[i], 32, stbuf);
                    else StringCchCopyW(sc.svc_status[i], 32, L"not found");
                }

                if (snap.svc_auto[i] && !should_skip_autostop_for_service_status(sc.svc_status[i])) {
                    if (_wcsicmp(sc.svc_status[i], L"stopped") != 0) {
                        if ((mono_now - snap.svc_lastreq[i]) >= cd_ms) {
                            int c = 0;
                            bool found = false;

                            {
                                std::lock_guard<std::mutex> lk(self.mtx);
                                Item* it = list_find(&self.items[KIND_SVC], snap.svc_names[i]);
                                if (it) {
                                    it->last_autostop_mono_ms = mono_now;
                                    it->autostop_count++;
                                    c = it->autostop_count;
                                    found = true;
                                }
                            }

                            if (found) {
                                // keep local snapshot consistent for the next check
                                snap.svc_lastreq[i] = mono_now;

                                wchar_t msg[600];
                                StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was %s) requested\u2026",
                                                 snap.svc_names[i], c, sc.svc_status[i]);
                                post_log(self, msg);

                                wchar_t reason[256];
                                StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", c);
                                action_enqueue(self, KIND_SVC, ACTION_STOP, snap.svc_names[i], reason, stop_wait_ms);
                            }
                        }
                    }
                }
            }

            // Option A: one message per kind per tick
            post_status_bulk(self, KIND_SVC, snap.svc_names, sc.svc_status, ns, wall_now);
        }

        // --- EXEs: batch count processes; derive running/stopped ---
        if (ne && snap.exe_names && sc.exe_counts && sc.exe_status) {
            memset(sc.exe_counts, 0, ne * sizeof(int));
            batch_count_processes_by_name_lower(snap.exe_names, sc.exe_counts, ne);

            for (size_t i = 0; i < ne; i++) {
                if (!snap.exe_names[i]) {
                    StringCchCopyW(sc.exe_status[i], 32, L"unknown");
                    continue;
                }

                int c = sc.exe_counts[i];
                const wchar_t* status = (c > 0) ? L"running" : L"stopped";
                StringCchCopyW(sc.exe_status[i], 32, status);

                if (snap.exe_auto[i] && _wcsicmp(status, L"stopped") != 0) {
                    if ((mono_now - snap.exe_lastreq[i]) >= cd_ms) {
                        int k = 0;
                        bool found = false;

                        {
                            std::lock_guard<std::mutex> lk(self.mtx);
                            Item* it = list_find(&self.items[KIND_EXE], snap.exe_names[i]);
                            if (it) {
                                it->last_autostop_mono_ms = mono_now;
                                it->autostop_count++;
                                k = it->autostop_count;
                                found = true;
                            }
                        }

                        if (found) {
                            snap.exe_lastreq[i] = mono_now;

                            wchar_t msg[600];
                            StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was running) requested\u2026",
                                             snap.exe_names[i], k);
                            post_log(self, msg);

                            wchar_t reason[256];
                            StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", k);
                            action_enqueue(self, KIND_EXE, ACTION_STOP, snap.exe_names[i], reason, stop_wait_ms);
                        }
                    }
                }
            }

            post_status_bulk(self, KIND_EXE, snap.exe_names, sc.exe_status, ne, wall_now);
        }

        DWORD wait_ms = (DWORD)std::max(50, monitor_interval_ms);
        if (sleep_ms_cooperative(st, wait_ms)) break;
    }

    monsnap_free(&snap);
    free(sc.svc_status);
    free(sc.exe_status);
    free(sc.exe_counts);
}

// ----------------------------
// Preferences + Help dialogs (unchanged behavior; tiny addition: restart UI timer)
// ----------------------------
static bool prefs_apply(App& self, HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close);
static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_prefs_dialog(App& self, HWND parent);
static LRESULT CALLBACK HelpWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_help_dialog(App& self, HWND parent);

// Profiles dialog
static LRESULT CALLBACK ProfilesWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_profiles_dialog(App& self, HWND parent);

static bool prefs_apply(App& self, HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close) {
    wchar_t b1[64], b2[64], b3[64], b4[64];
    GetWindowTextW(e_ui, b1, 64);
    GetWindowTextW(e_mon, b2, 64);
    GetWindowTextW(e_cd, b3, 64);
    GetWindowTextW(e_sw, b4, 64);

    int ui = _wtoi(b1);
    int mon = _wtoi(b2);
    int cd = _wtoi(b3);
    int sw = _wtoi(b4);

    if (ui < 200) { msgbox_err(dlg, L"Preferences", L"UI Refresh must be at least 200 ms."); return false; }
    if (mon < 200) { msgbox_err(dlg, L"Preferences", L"Monitor Interval must be at least 200 ms."); return false; }
    if (cd < 0) { msgbox_err(dlg, L"Preferences", L"Autostop Cooldown (ms) cannot be negative."); return false; }
    if (sw < 0) { msgbox_err(dlg, L"Preferences", L"Stop Wait (ms) cannot be negative."); return false; }

    {
        std::lock_guard<std::mutex> lk(self.mtx);

        self.ui_refresh_ms = ui;
        self.monitor_interval_ms = mon;
        self.autostop_cooldown_ms = cd;
        self.stop_wait_ms = sw;

        // Tray settings
        BOOL tray_checked = (SendMessageW(c_tray, BM_GETCHECK, 0, 0) == BST_CHECKED);
        BOOL close_checked = (SendMessageW(c_close, BM_GETCHECK, 0, 0) == BST_CHECKED);

        bool tray_en = tray_checked ? true : false;
        bool close_en = close_checked ? true : false;

        if (close_en) tray_en = true; // close-to-tray requires tray icon

        self.tray_enabled = tray_en;
        self.close_to_tray = tray_en && close_en;

        if (!self.tray_enabled) self.close_to_tray = false;
    }

// Apply tray icon add/remove immediately
tray_sync(self, GetModuleHandleW(NULL));

    log_linef(self, L"Settings updated: UI %dms, Monitor %dms, Cooldown %dms, StopWait %dms, Tray %d, CloseToTray %d",
              self.ui_refresh_ms, self.monitor_interval_ms, self.autostop_cooldown_ms, self.stop_wait_ms, self.tray_enabled ? 1 : 0, self.close_to_tray ? 1 : 0);

    request_save_debounced(self);
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
    return true;
}

static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self_ptr));
        return TRUE;
    }
    auto* self_ptr = reinterpret_cast<App*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!self_ptr) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *self_ptr;

    static HWND e_ui, e_mon, e_cd, e_sw;
    static HWND c_tray, c_close;
    // Per-dialog DPI state for the Preferences window
    static UINT s_prefs_dpi = 96;
    static HFONT s_prefs_font = NULL;
    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        UINT dpi = get_dpi_for_hwnd(dlg);
        auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

        CreateWindowW(L"STATIC", L"UI Refresh (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(14), S(160), S(20), dlg, NULL, NULL, NULL);
        e_ui = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(12), S(140), S(24), dlg, (HMENU)IDC_PREF_UI_MS, NULL, NULL);

        CreateWindowW(L"STATIC", L"Monitor Interval (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(48), S(160), S(20), dlg, NULL, NULL, NULL);
        e_mon = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(46), S(140), S(24), dlg, (HMENU)IDC_PREF_MON_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Autostop Cooldown (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(82), S(160), S(20), dlg, NULL, NULL, NULL);
        e_cd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(80), S(140), S(24), dlg, (HMENU)IDC_PREF_CD_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Stop Wait (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(116), S(160), S(20), dlg, NULL, NULL, NULL);
        e_sw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(114), S(140), S(24), dlg, (HMENU)IDC_PREF_SW_S, NULL, NULL);


// Tray options
c_tray = CreateWindowW(L"BUTTON", L"Enable tray icon", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    S(12), S(152), S(200), S(22), dlg, (HMENU)IDC_PREF_TRAY_ENABLE, NULL, NULL);

c_close = CreateWindowW(L"BUTTON", L"Close to tray (hide on X)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    S(12), S(176), S(240), S(22), dlg, (HMENU)IDC_PREF_CLOSE_TO_TRAY, NULL, NULL);
        wchar_t buf[64];
        StringCchPrintfW(buf, 64, L"%d", self.ui_refresh_ms);
        SetWindowTextW(e_ui, buf);
        StringCchPrintfW(buf, 64, L"%d", self.monitor_interval_ms);
        SetWindowTextW(e_mon, buf);
        StringCchPrintfW(buf, 64, L"%d", self.autostop_cooldown_ms);
        SetWindowTextW(e_cd, buf);
        StringCchPrintfW(buf, 64, L"%d", self.stop_wait_ms);
        SetWindowTextW(e_sw, buf);

SendMessageW(c_tray, BM_SETCHECK, self.tray_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
SendMessageW(c_close, BM_SETCHECK, self.close_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);


        wchar_t cfgline[MAX_PATH + 64];
        StringCchPrintfW(cfgline, MAX_PATH + 64, L"Config file: %s", self.cfg_path);
        CreateWindowW(L"STATIC", cfgline, WS_CHILD | WS_VISIBLE,
            S(12), S(210), S(470), S(20), dlg, NULL, NULL, NULL);

        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            S(230), S(244), S(80), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE,
            S(318), S(244), S(80), S(28), dlg, (HMENU)IDC_PREF_APPLY, NULL, NULL);
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            S(406), S(244), S(80), S(28), dlg, (HMENU)IDOK, NULL, NULL);

        // Per-window DPI scaling (Profiles window is modeless; must scale its fixed 96-DPI layout)
        s_prefs_dpi = dpi;
        if (s_prefs_font) { DeleteObject(s_prefs_font); s_prefs_font = NULL; }
        s_prefs_font = create_ui_font_for_dpi(s_prefs_dpi);
        if (s_prefs_font) apply_font_recursive(dlg, s_prefs_font);

        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Live filtering (search boxes)
        if ((id == IDC_SVC_SEARCH_EDIT || id == IDC_EXE_SEARCH_EDIT) && code == EN_CHANGE) {
            wchar_t buf[512];
            buf[0] = 0;
            if (id == IDC_SVC_SEARCH_EDIT && self.svc_search) GetWindowTextW(self.svc_search, buf, 512);
            if (id == IDC_EXE_SEARCH_EDIT && self.exe_search) GetWindowTextW(self.exe_search, buf, 512);
            trim_ws_inplace(buf);
            int kind = (id == IDC_SVC_SEARCH_EDIT) ? KIND_SVC : KIND_EXE;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                self.filter[kind] = to_lower_ws(buf);
            }
            rebuild_listview_filtered(self, kind);
            update_statusbar(self);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        if (id == IDC_PREF_APPLY) { prefs_apply(self, dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close); return 0; }
        if (id == IDOK) {
            if (prefs_apply(self, dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close)) DestroyWindow(dlg);
            return 0;
        }
        return 0;
    }

    case WM_DPICHANGED: {
        UINT new_dpi = LOWORD(wParam);
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        if (rc) {
            SetWindowPos(dlg, NULL, rc->left, rc->top,
                rc->right - rc->left, rc->bottom - rc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (new_dpi && new_dpi != s_prefs_dpi) {
            scale_children(dlg, s_prefs_dpi, new_dpi);
            s_prefs_dpi = new_dpi;

            if (s_prefs_font) { DeleteObject(s_prefs_font); s_prefs_font = NULL; }
            s_prefs_font = create_ui_font_for_dpi(s_prefs_dpi);
            if (s_prefs_font) apply_font_recursive(dlg, s_prefs_font);
        }
        return 0;
    }

    case WM_NCDESTROY:
        if (s_prefs_font) { DeleteObject(s_prefs_font); s_prefs_font = NULL; }
        s_prefs_dpi = 96;
        return 0;

    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void open_prefs_dialog(App& self, HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PrefsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PrefsDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    UINT dpi = get_dpi_for_hwnd(parent);
    int w = dpi_scale(dpi, 510), h = dpi_scale(dpi, 310);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Preferences",
        WS_CAPTION | WS_POPUP | WS_SYSMENU,
        x, y, w, h, parent, NULL, wc.hInstance, &self);

    ShowWindow(dlg, SW_SHOW);
    EnableWindow(parent, FALSE);

    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);
}

static LRESULT CALLBACK HelpWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self_ptr));
        return TRUE;
    }
    auto* self_ptr = reinterpret_cast<App*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!self_ptr) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *self_ptr;

    static HWND edit;
    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        UINT dpi = get_dpi_for_hwnd(dlg);
        auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

        CreateWindowW(L"STATIC", L"What the settings do", WS_CHILD | WS_VISIBLE,
            S(12), S(10), S(400), S(20), dlg, NULL, NULL, NULL);

        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
            WS_VSCROLL | WS_HSCROLL,
            S(12), S(34), S(560), S(320), dlg, NULL, NULL, NULL);

        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            S(492), S(362), S(80), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);

        const wchar_t* help =
            L"All timing settings are in milliseconds (ms). 1000 ms = 1 second.\r\n\r\n"
            L"UI Refresh (ms)\r\n"
            L"- How often the UI does maintenance work (status bar updates, column autosize/fill).\r\n"
            L"- Lower = more responsive layout updates, but slightly more UI work.\r\n\r\n"
            L"Monitor Interval (ms)\r\n"
            L"- How often the background monitor polls service states and process counts.\r\n"
            L"- Lower = faster detection, higher CPU usage.\r\n\r\n"
            L"Autostop Cooldown (ms)\r\n"
            L"- Auto-stop enforcement means: if it’s running, request stop/terminate.\r\n"
            L"- Cooldown is the minimum time between auto-stop attempts for the same item.\r\n"
            L"- Prevents spam when services/processes restart quickly.\r\n\r\n"
            L"Stop Wait (ms)\r\n"
            L"- After requesting stop/terminate, wait up to this many milliseconds to confirm it stopped.\r\n"
            L"- 0 = fire-and-forget.\r\n\r\n"
            L"Auto Stop checkbox (per row)\r\n"
            L"- When enabled, the app enforces that the service/EXE should remain stopped.\r\n"
            L"- Transitional service states (starting/stopping/etc.) are skipped.\r\n";

        SetWindowTextW(edit, help);
        if (self.ui_font) apply_font_recursive(dlg, self.ui_font);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(dlg); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void open_help_dialog(App& self, HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HelpWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HelpDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    UINT dpi = get_dpi_for_hwnd(parent);
    int w = dpi_scale(dpi, 600), h = dpi_scale(dpi, 430);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Settings Help",
        WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, parent, NULL, wc.hInstance, &self);

    ShowWindow(dlg, SW_SHOW);
    EnableWindow(parent, FALSE);

    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);
}


// ----------------------------
// Profiles dialog (manage automatic profile activation WATCH EXEs)
// ----------------------------
static std::wstring make_unique_profile_name_locked(const App& self, const wchar_t* base) {
    std::wstring b = base && base[0] ? base : L"Profile";
    auto exists_ci = [&](const std::wstring& name) -> bool {
        for (const auto& p : self.profiles) {
            if (_wcsicmp(p.name.c_str(), name.c_str()) == 0) return true;
        }
        return false;
    };

    if (!exists_ci(b)) return b;

    for (int i = 2; i < 10000; i++) {
        wchar_t tmp[128];
        StringCchPrintfW(tmp, 128, L"%s %d", b.c_str(), i);
        if (!exists_ci(tmp)) return std::wstring(tmp);
    }
    return b + L" (copy)";
}

static void prof_fill_profile_list(App& self, HWND lb_profiles, int sel) {
    if (!lb_profiles) return;

    SendMessageW(lb_profiles, LB_RESETCONTENT, 0, 0);

    std::vector<std::wstring> names;
    int active = -1;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        active = self.active_profile;
        names.reserve(self.profiles.size());
        for (const auto& p : self.profiles) names.push_back(p.name.empty() ? L"(unnamed)" : p.name);
    }

    for (size_t i = 0; i < names.size(); i++) {
        SendMessageW(lb_profiles, LB_ADDSTRING, 0, (LPARAM)names[i].c_str());
    }

    int count = (int)names.size();
    if (count <= 0) {
        SendMessageW(lb_profiles, LB_SETCURSEL, (WPARAM)-1, 0);
        return;
    }

    int want = sel;
    if (want < 0 || want >= count) {
        // Prefer active profile if present
        if (active >= 0 && active < count) want = active;
        else want = 0;
    }
    SendMessageW(lb_profiles, LB_SETCURSEL, want, 0);
}

static void prof_fill_watch_list(App& self, HWND lb_watch, int prof_idx) {
    if (!lb_watch) return;
    SendMessageW(lb_watch, LB_RESETCONTENT, 0, 0);

    std::vector<std::wstring> ws;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        if (prof_idx >= 0 && prof_idx < (int)self.profiles.size()) {
            ws = self.profiles[(size_t)prof_idx].watch_exes;
        }
    }
    for (const auto& w : ws) {
        if (w.empty()) continue;
        SendMessageW(lb_watch, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
    if (!ws.empty()) SendMessageW(lb_watch, LB_SETCURSEL, 0, 0);
}

// Format: "[✓] name" or "[ ] name"
static std::wstring prof_fmt_itemrow(const ItemRow& r) {
    std::wstring s = r.auto_stop ? L"[✓] " : L"[ ] ";
    s += r.name;
    return s;
}

static void prof_fill_content_list(App& self, HWND lb, int prof_idx, int kind) {
    if (!lb) return;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    std::vector<ItemRow> rows;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        if (prof_idx >= 0 && prof_idx < (int)self.profiles.size()) {
            rows = self.profiles[(size_t)prof_idx].cfg.items[kind];
        }
    }

    for (const auto& r : rows) {
        if (r.name.empty()) continue;
        std::wstring line = prof_fmt_itemrow(r);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (!rows.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
}


static std::wstring prof_fmt_startexe(const ItemRow& r) {
    if (!r.exe_path.empty()) {
        std::wstring s = r.name;
        s += L" | ";
        s += r.exe_path;
        return s;
    }
    return r.name;
}

static void prof_fill_start_exe_list(App& self, HWND lb, int prof_idx) {
    if (!lb) return;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    std::vector<ItemRow> rows;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        if (prof_idx >= 0 && prof_idx < (int)self.profiles.size()) {
            rows = self.profiles[(size_t)prof_idx].start_exes;
        }
    }

    for (const auto& r : rows) {
        if (r.name.empty()) continue;
        std::wstring line = prof_fmt_startexe(r);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (!rows.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
}

[[maybe_unused]] static bool prof_row_name_from_listbox_line(const std::wstring& line, std::wstring& out_name) {
    // Accept either "[✓] name" or "[ ] name"
    out_name = line;
    if (out_name.rfind(L"[✓] ", 0) == 0 || out_name.rfind(L"[ ] ", 0) == 0) {
        out_name.erase(0, 4);
    }
    // Trim
    while (!out_name.empty() && iswspace(out_name.back())) out_name.pop_back();
    size_t start = 0;
    while (start < out_name.size() && iswspace(out_name[start])) start++;
    if (start) out_name.erase(0, start);
    return !out_name.empty();
}

[[maybe_unused]] static std::wstring prof_get_lb_text(HWND lb, int idx) {
    if (!lb || idx < 0) return L"";
    LRESULT len = SendMessageW(lb, LB_GETTEXTLEN, (WPARAM)idx, 0);
    if (len == LB_ERR || len <= 0) return L"";
    std::wstring s;
    s.resize((size_t)len);
    SendMessageW(lb, LB_GETTEXT, (WPARAM)idx, (LPARAM)s.data());
    return s;
}

// Apply the currently active profile's snapshot to runtime WITHOUT snapshotting runtime back into the snapshot.
// (Used when editing the active profile in the Profiles dialog.)
static void apply_active_profile_cfg_inplace(App& self) {
    std::vector<Item*> svc_ptrs;
    std::vector<Item*> exe_ptrs;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        idx = self.active_profile;
        const ConfigSnapshot* cfg = &self.default_cfg;
        if (idx >= 0 && idx < (int)self.profiles.size()) cfg = &self.profiles[(size_t)idx].cfg;

        self.ui_refresh_ms = cfg->ui_refresh_ms;
        self.monitor_interval_ms = cfg->monitor_interval_ms;
        self.autostop_cooldown_ms = cfg->autostop_cooldown_ms;
        self.stop_wait_ms = cfg->stop_wait_ms;
        self.tray_enabled = cfg->tray_enabled;
        self.close_to_tray = cfg->tray_enabled && cfg->close_to_tray;

        rebuild_runtime_items_locked(self, *cfg);

        svc_ptrs.reserve(self.items[KIND_SVC].v.size());
        for (auto& up : self.items[KIND_SVC].v) if (up) svc_ptrs.push_back(up.get());

        exe_ptrs.reserve(self.items[KIND_EXE].v.size());
        for (auto& up : self.items[KIND_EXE].v) if (up) exe_ptrs.push_back(up.get());
    }

    tray_sync(self, GetModuleHandleW(NULL));

    rebuild_listview_filtered(self, KIND_SVC);
    rebuild_listview_filtered(self, KIND_EXE);

    self.lv_needs_layout[KIND_SVC] = true;
    self.lv_needs_layout[KIND_EXE] = true;
    update_statusbar(self);
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}


static void prof_update_active_label(App& self, HWND st_active) {
    if (!st_active) return;
    std::wstring name;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        name = active_profile_name_locked(self);
    }
    if (name.empty()) name = L"Default";
    wchar_t buf[300];
    StringCchPrintfW(buf, 300, L"Currently active: %s", name.c_str());
    SetWindowTextW(st_active, buf);
}

static int prof_get_sel(HWND lb) {
    if (!lb) return -1;
    LRESULT s = SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (s == LB_ERR) return -1;
    return (int)s;
}

static void prof_set_edit_text(HWND hEdit, const std::wstring& s) {
    if (!hEdit) return;
    SetWindowTextW(hEdit, s.c_str());
}

static std::wstring prof_get_edit_text(HWND hEdit) {
    if (!hEdit) return L"";
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return L"";
    std::wstring s;
    s.resize((size_t)len);
    GetWindowTextW(hEdit, s.data(), len + 1);
    // Trim
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    if (start > 0) s.erase(0, start);
    return s;
}

static LRESULT CALLBACK ProfilesWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self_ptr));
        return TRUE;
    }
    auto* self_ptr = reinterpret_cast<App*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!self_ptr) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *self_ptr;

    static HWND lb_profiles, e_name, b_add, b_rename, b_del, b_up, b_down, st_active;
    static HWND lb_watch, e_watch, b_watch_add, b_watch_del;

    static UINT s_prof_dpi = 96;
    static HFONT s_prof_font = NULL;

    // Profile contents editors (services / exes) — editable even when profile is not active
    static HWND lb_svc, e_svc, chk_svc, b_svc_add, b_svc_del, b_svc_toggle;
    static HWND lb_exe, e_exe, chk_exe, b_exe_add, b_exe_del, b_exe_toggle;
    static HWND lb_start_exe, e_start_exe, b_start_exe_add, b_start_exe_del;

    auto refresh_for_selection = [&]() {
        int sel = prof_get_sel(lb_profiles);
        // Button enable/disable so stored HWNDs are actually used
        EnableWindow(b_add, TRUE);
        EnableWindow(b_rename, sel >= 0);
        EnableWindow(b_del, sel >= 0);
        EnableWindow(b_up, sel > 0);
        int profN = (int)SendMessageW(lb_profiles, LB_GETCOUNT, 0, 0);
        EnableWindow(b_down, sel >= 0 && sel < profN - 1);
        EnableWindow(b_watch_add, sel >= 0);
        EnableWindow(b_watch_del, sel >= 0);
        std::wstring pname;
        {
            std::lock_guard<std::mutex> lk(self.mtx);
            if (sel >= 0 && sel < (int)self.profiles.size()) pname = self.profiles[(size_t)sel].name;
        }
        if (pname.empty()) pname = L"(unnamed)";
        prof_set_edit_text(e_name, pname);
        prof_fill_watch_list(self, lb_watch, sel);
        prof_fill_content_list(self, lb_svc, sel, KIND_SVC);
        prof_fill_content_list(self, lb_exe, sel, KIND_EXE);
        prof_fill_start_exe_list(self, lb_start_exe, sel);

        int svcsel = prof_get_sel(lb_svc);
        int exesel = prof_get_sel(lb_exe);
        EnableWindow(b_svc_add, sel >= 0);
        EnableWindow(b_svc_del, sel >= 0 && svcsel >= 0);
        EnableWindow(b_svc_toggle, sel >= 0 && svcsel >= 0);
        EnableWindow(b_exe_add, sel >= 0);
        EnableWindow(b_exe_del, sel >= 0 && exesel >= 0);
        EnableWindow(b_exe_toggle, sel >= 0 && exesel >= 0);
        int starts = prof_get_sel(lb_start_exe);
        EnableWindow(b_start_exe_add, sel >= 0);
        EnableWindow(b_start_exe_del, sel >= 0 && starts >= 0);

        prof_update_active_label(self, st_active);
    };

    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        CreateWindowW(L"STATIC", L"Profiles", WS_CHILD | WS_VISIBLE,
            12, 10, 240, 20, dlg, NULL, NULL, NULL);

        lb_profiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            12, 34, 250, 270, dlg, (HMENU)IDC_PROF_LIST, NULL, NULL);

        CreateWindowW(L"STATIC", L"Profile name:", WS_CHILD | WS_VISIBLE,
            12, 310, 100, 18, dlg, NULL, NULL, NULL);

        e_name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            12, 330, 250, 24, dlg, (HMENU)IDC_PROF_NAME_EDIT, NULL, NULL);

        b_add = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
            12, 362, 60, 26, dlg, (HMENU)IDC_PROF_ADD, NULL, NULL);
        b_rename = CreateWindowW(L"BUTTON", L"Rename", WS_CHILD | WS_VISIBLE,
            78, 362, 70, 26, dlg, (HMENU)IDC_PROF_RENAME, NULL, NULL);
        b_del = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE,
            154, 362, 70, 26, dlg, (HMENU)IDC_PROF_DELETE, NULL, NULL);

        b_up = CreateWindowW(L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE,
            230, 362, 32, 26, dlg, (HMENU)IDC_PROF_MOVE_UP, NULL, NULL);
        b_down = CreateWindowW(L"BUTTON", L"Down", WS_CHILD | WS_VISIBLE,
            230, 392, 32, 26, dlg, (HMENU)IDC_PROF_MOVE_DOWN, NULL, NULL);

        CreateWindowW(L"STATIC", L"Activation (WATCH EXEs) — profile activates if ANY are running", WS_CHILD | WS_VISIBLE,
            280, 10, 460, 20, dlg, NULL, NULL, NULL);

        lb_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 34, 460, 230, dlg, (HMENU)IDC_WATCH_LIST, NULL, NULL);

        CreateWindowW(L"STATIC", L"Add WATCH exe (name or full path):", WS_CHILD | WS_VISIBLE,
            280, 270, 300, 18, dlg, NULL, NULL, NULL);

        e_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 290, 340, 24, dlg, (HMENU)IDC_WATCH_EDIT, NULL, NULL);

        b_watch_add = CreateWindowW(L"BUTTON", L"Add WATCH", WS_CHILD | WS_VISIBLE,
            628, 290, 112, 24, dlg, (HMENU)IDC_WATCH_ADD, NULL, NULL);

        b_watch_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            628, 320, 112, 24, dlg, (HMENU)IDC_WATCH_REMOVE, NULL, NULL);

        // ---- Profile contents editor (services / exes) ----
        CreateWindowW(L"STATIC", L"Profile contents (editable even when profile is NOT active)", WS_CHILD | WS_VISIBLE,
            280, 352, 460, 18, dlg, NULL, NULL, NULL);

        // Services
        CreateWindowW(L"STATIC", L"Services in this profile:", WS_CHILD | WS_VISIBLE,
            280, 374, 220, 18, dlg, NULL, NULL, NULL);

        lb_svc = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 396, 220, 190, dlg, (HMENU)IDC_PCONT_SVC_LIST, NULL, NULL);

        e_svc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 592, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_EDIT, NULL, NULL);

        chk_svc = CreateWindowW(L"BUTTON", L"Auto-stop", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            280, 620, 220, 22, dlg, (HMENU)IDC_PCONT_SVC_AUTOSTOP, NULL, NULL);

        b_svc_add = CreateWindowW(L"BUTTON", L"Add service", WS_CHILD | WS_VISIBLE,
            280, 646, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_ADD, NULL, NULL);

        b_svc_toggle = CreateWindowW(L"BUTTON", L"Toggle auto-stop", WS_CHILD | WS_VISIBLE,
            280, 674, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_TOGGLE, NULL, NULL);

        b_svc_del = CreateWindowW(L"BUTTON", L"Remove selected service", WS_CHILD | WS_VISIBLE,
            280, 702, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_REMOVE, NULL, NULL);

        // EXEs
        CreateWindowW(L"STATIC", L"EXEs in this profile:", WS_CHILD | WS_VISIBLE,
            520, 374, 220, 18, dlg, NULL, NULL, NULL);

        lb_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            520, 396, 220, 190, dlg, (HMENU)IDC_PCONT_EXE_LIST, NULL, NULL);

        e_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            520, 592, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_EDIT, NULL, NULL);

        chk_exe = CreateWindowW(L"BUTTON", L"Auto-stop", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            520, 620, 220, 22, dlg, (HMENU)IDC_PCONT_EXE_AUTOSTOP, NULL, NULL);

        b_exe_add = CreateWindowW(L"BUTTON", L"Add exe", WS_CHILD | WS_VISIBLE,
            520, 646, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_ADD, NULL, NULL);

        b_exe_toggle = CreateWindowW(L"BUTTON", L"Toggle auto-stop", WS_CHILD | WS_VISIBLE,
            520, 674, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_TOGGLE, NULL, NULL);

        b_exe_del = CreateWindowW(L"BUTTON", L"Remove selected exe", WS_CHILD | WS_VISIBLE,
            520, 702, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_REMOVE, NULL, NULL);

        st_active = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            280, 734, 460, 18, dlg, (HMENU)IDC_PROF_ACTIVE_LABEL, NULL, NULL);

        CreateWindowW(L"STATIC",
            L"Note: If multiple profiles match, the first one in this list wins.\r\n"
            L"Profiles deactivate automatically when none of their WATCH EXEs are running.",
            WS_CHILD | WS_VISIBLE,
            280, 756, 460, 40, dlg, NULL, NULL, NULL);


        // Start EXEs on activation (separate from Auto-stop lists)
        CreateWindowW(L"STATIC",
            L"Start EXEs when this profile activates (stopped on deactivation):",
            WS_CHILD | WS_VISIBLE,
            280, 804, 460, 18, dlg, NULL, NULL, NULL);

        lb_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 826, 460, 110, dlg, (HMENU)IDC_PSTART_EXE_LIST, NULL, NULL);

        CreateWindowW(L"STATIC", L"Add start exe (name or full path):", WS_CHILD | WS_VISIBLE,
            280, 944, 300, 18, dlg, NULL, NULL, NULL);

        e_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 962, 340, 24, dlg, (HMENU)IDC_PSTART_EXE_EDIT, NULL, NULL);

        b_start_exe_add = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
            630, 960, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_ADD, NULL, NULL);

        b_start_exe_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            630, 988, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_REMOVE, NULL, NULL);

        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            660, 1040, 80, 28, dlg, (HMENU)IDCANCEL, NULL, NULL);

        if (self.ui_font) apply_font_recursive(dlg, self.ui_font);

        // DPI-aware: scale child controls + apply per-dialog font
        s_prof_dpi = get_dpi_for_hwnd(dlg);
        if (s_prof_font) { DeleteObject(s_prof_font); s_prof_font = NULL; }
        s_prof_font = create_ui_font_for_dpi(s_prof_dpi);
        if (s_prof_dpi != 96) {
            scale_children(dlg, 96, s_prof_dpi);
        }
        if (s_prof_font) apply_font_recursive(dlg, s_prof_font);

        prof_fill_profile_list(self, lb_profiles, 0);
        refresh_for_selection();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }

        if (id == IDC_PROF_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PROF_ADD) {
            int new_sel = -1;
            {
                std::lock_guard<std::mutex> lk(self.mtx);

                // Ensure default snapshot exists
                if (!self.have_default_cfg) {
                    snapshot_from_runtime_locked(self, self.default_cfg);
                    self.have_default_cfg = true;
                }

                ProfileSnapshot p;
                p.name = make_unique_profile_name_locked(self, L"Profile");
                p.cfg = self.default_cfg; // start from default
                self.profiles.push_back(std::move(p));
                new_sel = (int)self.profiles.size() - 1;
            }
            request_save_debounced(self);
            prof_fill_profile_list(self, lb_profiles, new_sel);
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PROF_RENAME) {
            int sel = prof_get_sel(lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring name = prof_get_edit_text(e_name);
            if (name.empty()) { msgbox_warn(dlg, L"Profiles", L"Profile name cannot be empty."); return 0; }

            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);

                // Uniqueness check
                for (size_t i = 0; i < self.profiles.size(); i++) {
                    if ((int)i == sel) continue;
                    if (_wcsicmp(self.profiles[i].name.c_str(), name.c_str()) == 0) {
                        msgbox_warn(dlg, L"Profiles", L"A profile with that name already exists.");
                        return 0;
                    }
                }

                if (sel >= 0 && sel < (int)self.profiles.size()) {
                    self.profiles[(size_t)sel].name = name;
                    ok = true;
                }
            }
            if (ok) {
                request_save_debounced(self);
                prof_fill_profile_list(self, lb_profiles, sel);
                refresh_for_selection();
                update_statusbar(self);
            }
            return 0;
        }

        if (id == IDC_PROF_DELETE) {
            int sel = prof_get_sel(lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            if (MessageBoxW(dlg, L"Delete selected profile?", L"Profiles", MB_ICONWARNING | MB_OKCANCEL) != IDOK) {
                return 0;
            }

            bool need_switch_default = false;
            int new_sel = -1;

            {
                std::lock_guard<std::mutex> lk(self.mtx);

                // Persist runtime edits to the active snapshot before changing profile list
                snapshot_active_from_runtime_locked(self);

                if (sel >= 0 && sel < (int)self.profiles.size()) {
                    // Fix active_profile index
                    if (self.active_profile == sel) {
                        need_switch_default = true;
                    } else if (self.active_profile > sel) {
                        self.active_profile--;
                    }

                    self.profiles.erase(self.profiles.begin() + sel);

                    int count = (int)self.profiles.size();
                    if (count > 0) {
                        new_sel = std::min(sel, count - 1);
                    } else {
                        new_sel = -1;
                    }
                }
            }

            if (need_switch_default) {
                // Apply default immediately (avoids dangling index)
                apply_profile_index(self, -1);
            }

            request_save_debounced(self);
            prof_fill_profile_list(self, lb_profiles, new_sel);
            refresh_for_selection();
            update_statusbar(self);
            return 0;
        }

        if (id == IDC_PROF_MOVE_UP || id == IDC_PROF_MOVE_DOWN) {
            int sel = prof_get_sel(lb_profiles);
            if (sel < 0) return 0;

            int dir = (id == IDC_PROF_MOVE_UP) ? -1 : +1;
            int other = sel + dir;

            bool changed = false;
            int new_sel = sel;

            {
                std::lock_guard<std::mutex> lk(self.mtx);

                if (other >= 0 && other < (int)self.profiles.size()) {
                    std::swap(self.profiles[(size_t)sel], self.profiles[(size_t)other]);

                    // keep active_profile tracking correct
                    if (self.active_profile == sel) self.active_profile = other;
                    else if (self.active_profile == other) self.active_profile = sel;

                    changed = true;
                    new_sel = other;
                }
            }

            if (changed) {
                request_save_debounced(self);
                prof_fill_profile_list(self, lb_profiles, new_sel);
                refresh_for_selection();
                update_statusbar(self);
            }
            return 0;
        }

        if (id == IDC_WATCH_ADD) {
            int sel = prof_get_sel(lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(e_watch);
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter an .exe name or path first."); return 0; }

            wchar_t norm[512]; norm[0] = 0;
            normalize_exe_input(raw.c_str(), norm, 512);
            if (!norm[0]) { msgbox_warn(dlg, L"Profiles", L"That doesn't look like a valid .exe name or path."); return 0; }

            bool added = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (sel >= 0 && sel < (int)self.profiles.size()) {
                    auto& ws = self.profiles[(size_t)sel].watch_exes;
                    bool dup = false;
                    for (const auto& w : ws) {
                        if (_wcsicmp(w.c_str(), norm) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        ws.push_back(norm);
                        std::sort(ws.begin(), ws.end());
                        ws.erase(std::unique(ws.begin(), ws.end(),
                            [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }),
                            ws.end());
                        added = true;
                    }
                }
            }

            if (added) {
                SetWindowTextW(e_watch, L"");
                request_save_debounced(self);
                prof_fill_watch_list(self, lb_watch, sel);
                refresh_for_selection();
            } else {
                msgbox_info(dlg, L"Profiles", L"That WATCH exe is already in the list.");
            }
            return 0;
        }

        if (id == IDC_WATCH_REMOVE) {
            int sel = prof_get_sel(lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            int wsel = prof_get_sel(lb_watch);
            if (wsel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a WATCH exe first."); return 0; }

            bool removed = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (sel >= 0 && sel < (int)self.profiles.size()) {
                    auto& ws = self.profiles[(size_t)sel].watch_exes;
                    if (wsel >= 0 && wsel < (int)ws.size()) {
                        ws.erase(ws.begin() + wsel);
                        removed = true;
                    }
                }
            }

            if (removed) {
                request_save_debounced(self);
                prof_fill_watch_list(self, lb_watch, sel);
                refresh_for_selection();
            }
            return 0;
        }

        // ---- Profile contents editors (services / exes) ----
        if (id == IDC_PCONT_SVC_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            int psel = prof_get_sel(lb_profiles);
            int sel2 = prof_get_sel(lb_svc);
            EnableWindow(b_svc_del, psel >= 0 && sel2 >= 0);
            EnableWindow(b_svc_toggle, psel >= 0 && sel2 >= 0);
            return 0;
        }
        if (id == IDC_PCONT_EXE_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            int psel = prof_get_sel(lb_profiles);
            int sel2 = prof_get_sel(lb_exe);
            EnableWindow(b_exe_del, psel >= 0 && sel2 >= 0);
            EnableWindow(b_exe_toggle, psel >= 0 && sel2 >= 0);
            return 0;
        }

        if (id == IDC_PCONT_SVC_ADD) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring name = prof_get_edit_text(e_svc);
            if (name.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter a service name first."); return 0; }

            bool autostop = (SendMessageW(chk_svc, BM_GETCHECK, 0, 0) == BST_CHECKED);

            bool added = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_SVC];
                    bool dup = false;
                    for (const auto& r : rows) {
                        if (_wcsicmp(r.name.c_str(), name.c_str()) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        rows.push_back(ItemRow{ name, autostop, L"" });
                        std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) {
                            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                        });
                        added = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (!added) { msgbox_info(dlg, L"Profiles", L"That service is already in this profile."); return 0; }

            SetWindowTextW(e_svc, L"");
            SendMessageW(chk_svc, BM_SETCHECK, BST_UNCHECKED, 0);

            request_save_debounced(self);
            prof_fill_content_list(self, lb_svc, psel, KIND_SVC);
            refresh_for_selection();

            if (apply_now) apply_active_profile_cfg_inplace(self);
            return 0;
        }

        if (id == IDC_PCONT_SVC_TOGGLE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_svc);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a service first."); return 0; }

            bool changed = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_SVC];
                    if (isel >= 0 && isel < (int)rows.size()) {
                        rows[(size_t)isel].auto_stop = !rows[(size_t)isel].auto_stop;
                        changed = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (changed) {
                request_save_debounced(self);
                prof_fill_content_list(self, lb_svc, psel, KIND_SVC);
                if (isel >= 0) SendMessageW(lb_svc, LB_SETCURSEL, (WPARAM)isel, 0);
                refresh_for_selection();
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_SVC_REMOVE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_svc);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a service first."); return 0; }

            bool removed = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_SVC];
                    if (isel >= 0 && isel < (int)rows.size()) {
                        rows.erase(rows.begin() + isel);
                        removed = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (removed) {
                request_save_debounced(self);
                prof_fill_content_list(self, lb_svc, psel, KIND_SVC);
                refresh_for_selection();
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_EXE_ADD) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(e_exe);
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter an .exe name or path first."); return 0; }

            wchar_t norm[512]; norm[0] = 0;
            normalize_exe_input(raw.c_str(), norm, 512);
            if (!norm[0]) { msgbox_warn(dlg, L"Profiles", L"That doesn't look like a valid .exe name or path."); return 0; }

            bool autostop = (SendMessageW(chk_exe, BM_GETCHECK, 0, 0) == BST_CHECKED);

            bool added = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_EXE];
                    bool dup = false;
                    for (const auto& r : rows) {
                        if (_wcsicmp(r.name.c_str(), norm) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        rows.push_back(ItemRow{ std::wstring(norm), autostop, L"" });
                        std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) {
                            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                        });
                        added = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (!added) { msgbox_info(dlg, L"Profiles", L"That .exe is already in this profile."); return 0; }

            SetWindowTextW(e_exe, L"");
            SendMessageW(chk_exe, BM_SETCHECK, BST_UNCHECKED, 0);

            request_save_debounced(self);
            prof_fill_content_list(self, lb_exe, psel, KIND_EXE);
            refresh_for_selection();
            if (apply_now) apply_active_profile_cfg_inplace(self);
            return 0;
        }

        if (id == IDC_PCONT_EXE_TOGGLE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select an exe first."); return 0; }

            bool changed = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_EXE];
                    if (isel >= 0 && isel < (int)rows.size()) {
                        rows[(size_t)isel].auto_stop = !rows[(size_t)isel].auto_stop;
                        changed = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (changed) {
                request_save_debounced(self);
                prof_fill_content_list(self, lb_exe, psel, KIND_EXE);
                if (isel >= 0) SendMessageW(lb_exe, LB_SETCURSEL, (WPARAM)isel, 0);
                refresh_for_selection();
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_EXE_REMOVE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select an exe first."); return 0; }

            bool removed = false;
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    auto& rows = self.profiles[(size_t)psel].cfg.items[KIND_EXE];
                    if (isel >= 0 && isel < (int)rows.size()) {
                        rows.erase(rows.begin() + isel);
                        removed = true;
                        apply_now = (self.active_profile == psel);
                    }
                }
            }
            if (removed) {
                request_save_debounced(self);
                prof_fill_content_list(self, lb_exe, psel, KIND_EXE);
                refresh_for_selection();
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }


        if (id == IDC_PSTART_EXE_LIST && code == LBN_SELCHANGE) {
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PSTART_EXE_ADD) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(e_start_exe);
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter an .exe name or path first."); return 0; }

            wchar_t norm[512]; norm[0] = 0;
            normalize_exe_input(raw.c_str(), norm, 512);
            if (!norm[0]) { msgbox_warn(dlg, L"Profiles", L"That doesn't look like a valid .exe name or path."); return 0; }

            wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
            resolve_exe_launch_path(raw.c_str(), exe_path_full, MAX_PATH);

            bool added = false;
            bool start_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    ProfileSnapshot& p = self.profiles[(size_t)psel];

                    bool dup = false;
                    for (const auto& r : p.start_exes) {
                        if (_wcsicmp(r.name.c_str(), norm) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        ItemRow r;
                        r.name = norm;
                        r.auto_stop = false;
                        if (exe_path_full[0]) r.exe_path = exe_path_full;
                        p.start_exes.push_back(std::move(r));
                        added = true;
                    }

                    // Ensure visible/managed in the profile EXE list too.
                    bool exists_in_cfg = false;
                    for (auto& rr : p.cfg.items[KIND_EXE]) {
                        if (_wcsicmp(rr.name.c_str(), norm) == 0) {
                            exists_in_cfg = true;
                            if (rr.exe_path.empty() && exe_path_full[0]) rr.exe_path = exe_path_full;
                            break;
                        }
                    }
                    if (!exists_in_cfg) {
                        ItemRow rr;
                        rr.name = norm;
                        rr.auto_stop = false;
                        if (exe_path_full[0]) rr.exe_path = exe_path_full;
                        p.cfg.items[KIND_EXE].push_back(std::move(rr));
                    }

                    if (self.active_profile == psel) start_now = true;
                }
            }

            if (added) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, lb_start_exe, psel);
                prof_fill_content_list(self, lb_exe, psel, KIND_EXE);
                refresh_for_selection();
            }

            if (start_now && process_count_by_name_lower(norm) == 0) {
                {
                    std::lock_guard<std::mutex> lk(self.mtx);
                    self.profile_started_exes.insert(norm);
                    Item* it = list_find(&self.items[KIND_EXE], norm);
                    if (it && it->exe_path.empty() && exe_path_full[0]) it->exe_path = exe_path_full;
                }
                action_enqueue(self, KIND_EXE, ACTION_START, norm, L"profile active (start exe)", 0);
                rebuild_listview_filtered(self, KIND_EXE);
            }
            return 0;
        }

        if (id == IDC_PSTART_EXE_REMOVE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_start_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a start exe first."); return 0; }

            std::wstring line = prof_get_lb_text(lb_start_exe, isel);
            std::wstring name = line;
            size_t bar = name.find(L'|');
            if (bar != std::wstring::npos) name = name.substr(0, bar);
            if (!name.empty()) {
                std::vector<wchar_t> tmp(name.begin(), name.end());
                tmp.push_back(0);
                trim_ws_inplace(tmp.data());
                name = tmp.data();
            }

            bool removed = false;
            bool stop_now = false;
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    ProfileSnapshot& p = self.profiles[(size_t)psel];
                    for (size_t i = 0; i < p.start_exes.size(); i++) {
                        if (_wcsicmp(p.start_exes[i].name.c_str(), name.c_str()) == 0) {
                            p.start_exes.erase(p.start_exes.begin() + (ptrdiff_t)i);
                            removed = true;
                            break;
                        }
                    }
                    if (removed && self.active_profile == psel) {
                        auto it = self.profile_started_exes.find(name);
                        if (it != self.profile_started_exes.end()) {
                            self.profile_started_exes.erase(it);
                            stop_now = true;
                        }
                    }
                }
            }

            if (removed) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, lb_start_exe, psel);
                refresh_for_selection();
            }

            if (stop_now) {
                action_enqueue(self, KIND_EXE, ACTION_STOP, name.c_str(), L"profile active (stop start-exe)", self.stop_wait_ms);
                rebuild_listview_filtered(self, KIND_EXE);
            }
            return 0;
        }


        return 0;
    }

    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(wParam);
        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        if (prc) {
            SetWindowPos(dlg, NULL, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (new_dpi && new_dpi != s_prof_dpi) {
            scale_children(dlg, s_prof_dpi, new_dpi);
            s_prof_dpi = new_dpi;
            if (s_prof_font) { DeleteObject(s_prof_font); s_prof_font = NULL; }
            s_prof_font = create_ui_font_for_dpi(s_prof_dpi);
            if (s_prof_font) apply_font_recursive(dlg, s_prof_font);
        }
        return 0;
    }
    case WM_DESTROY:
        if (s_prof_font) { DeleteObject(s_prof_font); s_prof_font = NULL; }
        s_prof_dpi = 96;
        return 0;

    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void open_profiles_dialog(App& self, HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ProfilesWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ProfilesDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    UINT dpi = get_dpi_for_hwnd(parent);
    int w = dpi_scale(dpi, 770), h = dpi_scale(dpi, 1110);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Profiles",
        WS_CAPTION | WS_POPUP | WS_SYSMENU,
        x, y, w, h, parent, NULL, wc.hInstance, &self);

    ShowWindow(dlg, SW_SHOW);
    EnableWindow(parent, FALSE);

    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);
}

// ----------------------------
// UI construction
// ----------------------------

static void layout(App& self, HWND hwnd);

static const wchar_t* SPLITTER_CLASS = L"SplitterBar_C";

static LRESULT CALLBACK SplitterProc(HWND sw, UINT msg, WPARAM wParam, LPARAM lParam) {
    App& self = *reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(sw), GWLP_USERDATA));
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN: {
        SetCapture(sw);
        self.splitter_dragging = true;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(self.hwnd, &pt);

        self.splitter_drag_start_x = pt.x;
        self.splitter_drag_start_w = (self.activity_panel_w > 0) ? self.activity_panel_w : 360;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (self.splitter_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(self.hwnd, &pt);

            int dx = pt.x - self.splitter_drag_start_x;
            int desired = self.splitter_drag_start_w - dx; // drag right => narrower Activity panel
            self.activity_panel_w = desired;

            // Layout clamps sizes; keep it snappy during drags.
            layout(self, self.hwnd);
            return 0;
        }
        return 0;

    case WM_LBUTTONUP:
        if (self.splitter_dragging) {
            self.splitter_dragging = false;
            ReleaseCapture();
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        self.splitter_dragging = false;
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(sw, &ps);
        RECT rc;
        GetClientRect(sw, &rc);

        FillRect(hdc, &rc, GetSysColorBrush(COLOR_3DFACE));

        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int cx = w / 2;

        HPEN penShadow = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
        HPEN penHilite = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DHILIGHT));

        // Simple etched grip: highlight/shadow pair
        HPEN old = (HPEN)SelectObject(hdc, penHilite);
        MoveToEx(hdc, cx - 1, 4, NULL); LineTo(hdc, cx - 1, h - 4);
        SelectObject(hdc, penShadow);
        MoveToEx(hdc, cx, 4, NULL); LineTo(hdc, cx, h - 4);

        SelectObject(hdc, old);
        DeleteObject(penShadow);
        DeleteObject(penHilite);

        EndPaint(sw, &ps);
        return 0;
    }
    }
    return DefWindowProcW(sw, msg, wParam, lParam);
}

static void register_splitter_class(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = SplitterProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SPLITTER_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_SIZEWE);
    RegisterClassW(&wc);
}

static HWND make_tab(HWND parent, int id) {
    // Slightly more modern: focus never (keeps focus rectangle off), clip siblings.
    return CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 100, 100, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static HWND make_page(HWND parent) {
    register_page_class();
    return CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, parent, NULL, GetModuleHandleW(NULL), NULL);
}

static void tab_add(HWND tab, int idx, const wchar_t* text) {
    TCITEMW ti;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)text;
    TabCtrl_InsertItem(tab, idx, &ti);
}

static void show_only(HWND a, HWND b, bool show_a) {
    ShowWindow(a, show_a ? SW_SHOW : SW_HIDE);
    ShowWindow(b, show_a ? SW_HIDE : SW_SHOW);
}

static void build_ui(App& self, HWND hwnd) {
    register_splitter_class();
    self.left_tabs = make_tab(hwnd, IDC_LEFT_TABS);
    self.center_tabs = make_tab(hwnd, IDC_CENTER_TABS);

    tab_add(self.left_tabs, 0, L"Services");
    tab_add(self.left_tabs, 1, L"EXEs");
    tab_add(self.center_tabs, 0, L"Services");
    tab_add(self.center_tabs, 1, L"EXEs");

    self.left_page_svc = make_page(hwnd);
    self.left_page_exe = make_page(hwnd);
    self.center_page_svc = make_page(hwnd);
    self.center_page_exe = make_page(hwnd);

    // Left: Services controls (same functionality, cleaner labels)
    self.svc_add_label = CreateWindowW(L"STATIC", L"Add Windows Service", WS_CHILD | WS_VISIBLE,
        10, 10, 220, 18, self.left_page_svc, NULL, NULL, NULL);
    self.svc_add_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 34, 220, 24, self.left_page_svc, (HMENU)IDC_SVC_ADD_EDIT, NULL, NULL);
    set_cue_banner(self.svc_add_edit, L"Service name (or display name)");
    CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
        10, 64, 220, 28, self.left_page_svc, (HMENU)IDC_SVC_ADD_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Stop selected", WS_CHILD | WS_VISIBLE,
        10, 110, 220, 28, self.left_page_svc, (HMENU)IDC_SVC_STOP_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Start selected", WS_CHILD | WS_VISIBLE,
        10, 144, 220, 28, self.left_page_svc, (HMENU)IDC_SVC_START_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
        10, 178, 220, 28, self.left_page_svc, (HMENU)IDC_SVC_REMOVE_BTN, NULL, NULL);

    // Left: EXE controls
    self.exe_add_label = CreateWindowW(L"STATIC", L"Add Executable (.exe)", WS_CHILD | WS_VISIBLE,
        10, 10, 220, 18, self.left_page_exe, NULL, NULL, NULL);
    self.exe_add_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 34, 220, 24, self.left_page_exe, (HMENU)IDC_EXE_ADD_EDIT, NULL, NULL);
    set_cue_banner(self.exe_add_edit, L"Exe name (game.exe) or full path");
    self.exe_browse_btn = CreateWindowW(L"BUTTON", L"Browse…", WS_CHILD | WS_VISIBLE,
        10, 64, 220, 28, self.left_page_exe, (HMENU)IDC_EXE_BROWSE_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
        10, 64, 220, 28, self.left_page_exe, (HMENU)IDC_EXE_ADD_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Stop selected", WS_CHILD | WS_VISIBLE,
        10, 110, 220, 28, self.left_page_exe, (HMENU)IDC_EXE_STOP_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Start selected", WS_CHILD | WS_VISIBLE,
        10, 144, 220, 28, self.left_page_exe, (HMENU)IDC_EXE_START_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
        10, 178, 220, 28, self.left_page_exe, (HMENU)IDC_EXE_REMOVE_BTN, NULL, NULL);

    // Center: listviews + search
    self.svc_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, self.center_page_svc, (HMENU)IDC_SVC_SEARCH_EDIT, GetModuleHandleW(NULL), NULL);
    self.exe_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, self.center_page_exe, (HMENU)IDC_EXE_SEARCH_EDIT, GetModuleHandleW(NULL), NULL);
    set_cue_banner(self.svc_search, L"Search services (name/status)");
    set_cue_banner(self.exe_search, L"Search EXEs (name/status)");
    

    self.lv_svc = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, self.center_page_svc, (HMENU)IDC_LV_SVC, GetModuleHandleW(NULL), NULL);

    self.lv_exe = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, self.center_page_exe, (HMENU)IDC_LV_EXE, GetModuleHandleW(NULL), NULL);

    lv_set_columns(self.lv_svc, L"Auto Stop", L"Service Name", L"Status", L"Last Update");
    lv_set_columns(self.lv_exe, L"Auto Stop", L"Executable", L"Status", L"Last Update");

    // Explorer theming for modern look (safe if unavailable)
    try_set_explorer_theme(self.lv_svc);
    try_set_explorer_theme(self.lv_exe);
    // Small icons for a cleaner, modern report view
    const int ilSz = std::max(16, dpi_scale(self.dpi ? self.dpi : 96, 16));
    self.il_svc = ImageList_Create(ilSz, ilSz, ILC_COLOR32 | ILC_MASK, 4, 4);
    self.il_exe = ImageList_Create(ilSz, ilSz, ILC_COLOR32 | ILC_MASK, 4, 4);
    if (self.il_svc) {
        HICON hSvc = (HICON)LoadImageW(NULL, IDI_INFORMATION, IMAGE_ICON, ilSz, ilSz, LR_SHARED);
        HICON hExe = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, ilSz, ilSz, LR_SHARED);
        if (hSvc) ImageList_AddIcon(self.il_svc, hSvc);
        if (hExe) ImageList_AddIcon(self.il_svc, hExe);
        // Keep indices aligned between lists
        if (hSvc) ImageList_AddIcon(self.il_exe, hSvc);
        if (hExe) ImageList_AddIcon(self.il_exe, hExe);

        ListView_SetImageList(self.lv_svc, self.il_svc, LVSIL_SMALL);
        ListView_SetImageList(self.lv_exe, self.il_exe, LVSIL_SMALL);
    }

    try_set_explorer_theme(self.left_tabs);
    try_set_explorer_theme(self.center_tabs);

    // Right: activity
    // Draggable splitter between center and Activity
    self.splitter = CreateWindowExW(0, SPLITTER_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, SPLITTER_WIDTH, 100, hwnd, (HMENU)IDC_SPLITTER, GetModuleHandleW(NULL), NULL);

    self.activity_label = CreateWindowW(L"STATIC", L"Activity", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 18, hwnd, NULL, NULL, NULL);

    self.activity = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, hwnd, (HMENU)IDC_ACTIVITY, NULL, NULL);

    // Status bar (new; uses your UI refresh setting)
    self.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, GetModuleHandleW(NULL), NULL);

    TabCtrl_SetCurSel(self.left_tabs, 0);
    TabCtrl_SetCurSel(self.center_tabs, 0);
    show_only(self.left_page_svc, self.left_page_exe, true);
    show_only(self.center_page_svc, self.center_page_exe, true);

    // Apply modern font once everything exists
    if (self.ui_font) apply_font_recursive(hwnd, self.ui_font);
}

static const wchar_t* active_profile_name_locked(const App& self);

// Status bar update (UI thread)
static void update_statusbar(App& self) {
    if (!self.statusbar) return;

    size_t ns = 0, ne = 0;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        ns = self.items[KIND_SVC].v.size();
        ne = self.items[KIND_EXE].v.size();
    }
    std::wstring prof;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        prof = active_profile_name_locked(self);
    }
    if (prof.empty()) prof = L"Default";


    wchar_t ts[64];
    fmt_time_local(ts, 64, self.last_any_status_wall);

    wchar_t text[256];
    StringCchPrintfW(text, 256, L"Profile: %s   Services: %zu   EXEs: %zu   Last update: %s", prof.c_str(), ns, ne, ts);
    SendMessageW(self.statusbar, SB_SETTEXTW, 0, (LPARAM)text);
}

static void layout(App& self, HWND hwnd) {
    UINT dpi = self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd);
    auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    // Status bar takes bottom space
    int sbH = 0;
    if (self.statusbar) {
        SendMessageW(self.statusbar, WM_SIZE, 0, 0);
        RECT sr; GetWindowRect(self.statusbar, &sr);
        sbH = (sr.bottom - sr.top);
        SetWindowPos(self.statusbar, NULL, 0, H - sbH, W, sbH, SWP_NOZORDER);
        H -= sbH;
    }

    int leftW = S(270);
    if (W < leftW + S(360) + S(300)) {
        leftW = std::max(S(220), W / 5);
    }

    int rightW = (self.activity_panel_w > 0) ? self.activity_panel_w : S(360);

// Clamp to sane bounds while always fitting in the client area.
// (If the window is extremely narrow, center/right may get small, but we avoid negative sizes.)
    int minRightW = S(ACTIVITY_MIN_W);
    int minCenterW = S(CENTER_MIN_W);

    int maxRightW = W - leftW - minCenterW;
    if (maxRightW < minRightW) maxRightW = W - leftW - S(100);
    if (maxRightW < S(80)) maxRightW = S(80);

    rightW = clamp_int(rightW, S(80), maxRightW);

    int centerW = W - leftW - rightW;
    if (centerW < S(80)) {
        centerW = S(80);
        rightW = W - leftW - centerW;
        if (rightW < S(80)) rightW = S(80);
    }

// If there's enough room to honor minimum center width, do it by shrinking the Activity panel.
if (centerW < minCenterW) {
    int wantRight = W - leftW - minCenterW;
    if (wantRight >= minRightW) {
        rightW = wantRight;
        centerW = minCenterW;
    }
}

self.activity_panel_w = rightW;

    int pad = S(8);
    int tabH = S(28);

    SetWindowPos(self.left_tabs, NULL, pad, pad, leftW - 2*pad, tabH, SWP_NOZORDER);
    int leftX = pad;
    int leftY = pad + tabH;
    int leftH = H - leftY - pad;
    SetWindowPos(self.left_page_svc, NULL, leftX, leftY, leftW - 2*pad, leftH, SWP_NOZORDER);
    SetWindowPos(self.left_page_exe, NULL, leftX, leftY, leftW - 2*pad, leftH, SWP_NOZORDER);

    // Inner layout: left pages (responsive)
    const int inPad = S(12);
    const int rowH = S(28);
    const int btnH = S(30);
    int lw = (leftW - 2*pad);
    int labelY = S(10);
    int labelH = S(18);
    int y1 = S(34);

    // Services page
    {
        HWND bAdd = GetDlgItem(self.left_page_svc, IDC_SVC_ADD_BTN);
        HWND bStop  = GetDlgItem(self.left_page_svc, IDC_SVC_STOP_BTN);
        HWND bStart = GetDlgItem(self.left_page_svc, IDC_SVC_START_BTN);
        HWND bRem   = GetDlgItem(self.left_page_svc, IDC_SVC_REMOVE_BTN);

        int addW = S(84);
        int gap = S(8);
        int editW = std::max(S(60), lw - 2*inPad - addW - gap);

        if (self.svc_add_label) SetWindowPos(self.svc_add_label, NULL, inPad, labelY, std::max(S(80), lw - 2*inPad), labelH, SWP_NOZORDER);
        if (self.svc_add_edit) SetWindowPos(self.svc_add_edit, NULL, inPad, y1, editW, rowH, SWP_NOZORDER);
        if (bAdd) SetWindowPos(bAdd, NULL, inPad + editW + gap, y1, addW, rowH, SWP_NOZORDER);

        int y2 = y1 + rowH + S(14);
        int fullW = std::max(S(80), lw - 2*inPad);

        int btnGap = S(10);
        if (bStop)  SetWindowPos(bStop,  NULL, inPad, y2, fullW, btnH, SWP_NOZORDER);
        if (bStart) SetWindowPos(bStart, NULL, inPad, y2 + btnH + btnGap, fullW, btnH, SWP_NOZORDER);
        if (bRem)   SetWindowPos(bRem,   NULL, inPad, y2 + 2*(btnH + btnGap), fullW, btnH, SWP_NOZORDER);
    }

    // EXEs page
    {
        HWND bAdd = GetDlgItem(self.left_page_exe, IDC_EXE_ADD_BTN);
        HWND bStop  = GetDlgItem(self.left_page_exe, IDC_EXE_STOP_BTN);
        HWND bStart = GetDlgItem(self.left_page_exe, IDC_EXE_START_BTN);
        HWND bRem   = GetDlgItem(self.left_page_exe, IDC_EXE_REMOVE_BTN);
        HWND bBrowse = self.exe_browse_btn ? self.exe_browse_btn : GetDlgItem(self.left_page_exe, IDC_EXE_BROWSE_BTN);

        int browseW = S(92);
        int addW = S(72);
        int gap = S(8);
        int editW = std::max(S(60), lw - 2*inPad - browseW - addW - gap*2);

        if (self.exe_add_label) SetWindowPos(self.exe_add_label, NULL, inPad, labelY, std::max(S(80), lw - 2*inPad), labelH, SWP_NOZORDER);
        if (self.exe_add_edit) SetWindowPos(self.exe_add_edit, NULL, inPad, y1, editW, rowH, SWP_NOZORDER);
        if (bBrowse) SetWindowPos(bBrowse, NULL, inPad + editW + gap, y1, browseW, rowH, SWP_NOZORDER);
        if (bAdd) SetWindowPos(bAdd, NULL, inPad + editW + gap + browseW + gap, y1, addW, rowH, SWP_NOZORDER);

        int y2 = y1 + rowH + S(14);
        int fullW = std::max(S(80), lw - 2*inPad);

        int btnGap = S(10);
        if (bStop)  SetWindowPos(bStop,  NULL, inPad, y2, fullW, btnH, SWP_NOZORDER);
        if (bStart) SetWindowPos(bStart, NULL, inPad, y2 + btnH + btnGap, fullW, btnH, SWP_NOZORDER);
        if (bRem)   SetWindowPos(bRem,   NULL, inPad, y2 + 2*(btnH + btnGap), fullW, btnH, SWP_NOZORDER);
    }

    int cx = leftW + pad;
    SetWindowPos(self.center_tabs, NULL, cx, pad, centerW - 2*pad, tabH, SWP_NOZORDER);
    int cy = pad + tabH;
    int ch = H - cy - pad;
    SetWindowPos(self.center_page_svc, NULL, cx, cy, centerW - 2*pad, ch, SWP_NOZORDER);
    SetWindowPos(self.center_page_exe, NULL, cx, cy, centerW - 2*pad, ch, SWP_NOZORDER);

    // Inner layout: center pages (search bar + list)
    const int cPad = S(10);
    const int searchH = S(28);
    const int gap = S(8);
    int cw = (centerW - 2*pad);

    if (self.svc_search) SetWindowPos(self.svc_search, NULL, cPad, cPad, std::max(S(80), cw - 2*cPad), searchH, SWP_NOZORDER);
    if (self.exe_search) SetWindowPos(self.exe_search, NULL, cPad, cPad, std::max(S(80), cw - 2*cPad), searchH, SWP_NOZORDER);

    int lvY = cPad + searchH + gap;
    int lvH = std::max(S(60), ch - lvY - cPad);

    SetWindowPos(self.lv_svc, NULL, cPad, lvY, std::max(S(80), cw - 2*cPad), lvH, SWP_NOZORDER);
    SetWindowPos(self.lv_exe, NULL, cPad, lvY, std::max(S(80), cw - 2*cPad), lvH, SWP_NOZORDER);

    int rx = leftW + centerW + pad;

    int splitterW = std::max(4, S(SPLITTER_WIDTH));
    if (self.splitter) {
        int sx = leftW + centerW - (splitterW / 2);
        SetWindowPos(self.splitter, NULL, sx, pad, splitterW, H - 2*pad, SWP_NOZORDER);
    }
    int actLabelH = S(18);
    HWND label = self.activity_label;
    if (label) SetWindowPos(label, NULL, rx, pad, rightW - 2*pad, actLabelH, SWP_NOZORDER);

    int actGap = S(6);
    SetWindowPos(self.activity, NULL, rx, pad + actLabelH + actGap, rightW - 2*pad, H - (pad + actLabelH + actGap) - pad, SWP_NOZORDER);

    // Mark for autosize on next UI tick (less churn during resize drags)
    self.lv_needs_layout[KIND_SVC] = true;
    self.lv_needs_layout[KIND_EXE] = true;
}

// ----------------------------
// Add/Remove/Stop handlers
// ----------------------------
static void add_item(App& self, int kind) {
    HWND page = (kind == KIND_SVC) ? self.left_page_svc : self.left_page_exe;
    int editId = (kind == KIND_SVC) ? IDC_SVC_ADD_EDIT : IDC_EXE_ADD_EDIT;
    HWND edit = GetDlgItem(page, editId);

    wchar_t raw[512];
    GetWindowTextW(edit, raw, 512);
    trim_ws_inplace(raw);
    if (!raw[0]) {
        msgbox_warn(self.hwnd, L"Add", L"Name is required.");
        return;
    }

    wchar_t name[512];
    wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
    if (kind == KIND_SVC) {
        resolve_service_name(self, raw, name, 512);
        wchar_t st[32];
        if (!query_service_status_fast(name, st, 32)) {
            wchar_t err[900];
            StringCchPrintfW(err, 900,
                L"Service not found.\n\nInput: %s\nResolved as: %s\n\nTip: try the service key name (e.g., 'wuauserv').",
                raw, name);
            msgbox_err(self.hwnd, L"Add service", err);
            return;
        }
    } else {
        normalize_exe_input(raw, name, 512);
        if (!name[0]) {
            msgbox_err(self.hwnd, L"Add EXE", L"Executable name is invalid.");
            return;
        }
        resolve_exe_launch_path(raw, exe_path_full, MAX_PATH);
    }

    Item* exist = NULL;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        exist = list_find(&self.items[kind], name);
    }
    if (exist) {
        msgbox_info(self.hwnd, L"Add", L"Already monitored.");
        return;
    }

    Item* it = new (std::nothrow) Item();
    if (!it) return;
    it->kind = kind;
    it->name = name;
    if (kind == KIND_EXE && exe_path_full[0]) it->exe_path = exe_path_full;
    it->auto_stop = false;
    it->img = (kind == KIND_SVC) ? 0 : 1;
    it->autostop_count = 0;
    it->last_autostop_mono_ms = 0;
    it->last_update_wall = time(NULL);

    if (kind == KIND_SVC) {
        wchar_t st[32];
        query_service_status_fast(name, st, 32);
        StringCchCopyW(it->last_status, 32, st);
    } else {
        int c = process_count_by_name_lower(name);
        StringCchCopyW(it->last_status, 32, (c > 0) ? L"running" : L"stopped");
    }

    {
        std::lock_guard<std::mutex> lk(self.mtx);
        list_push(&self.items[kind], it);
        self.items_gen[kind]++;
    }

    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    {
        std::wstring flt;
        {
            std::lock_guard<std::mutex> lk(self.mtx);
            flt = self.filter[kind];
        }
        if (item_matches_filter(it, flt)) lv_set_row(self, lv, it);
    }

    log_linef(self, L"Monitoring started (%s): %s (status: %s)",
              (kind == KIND_SVC) ? L"service" : L"exe", it->name.c_str(), it->last_status);
    request_save_debounced(self);

    self.lv_needs_layout[kind] = true;
    update_statusbar(self);

    SetWindowTextW(edit, L"");
}

static void remove_selected(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) return;

    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        removed = list_remove_name(&self.items[kind], name);
        if (removed) self.items_gen[kind]++;
    }
    if (!removed) return;

    int idx = lv_find_item_by_name(lv, name);
    if (idx >= 0) ListView_DeleteItem(lv, idx);

    log_linef(self, L"Monitoring removed (%s): %s", (kind == KIND_SVC) ? L"service" : L"exe", name);
    request_save_debounced(self);

    self.lv_needs_layout[kind] = true;
    update_statusbar(self);
}

static void stop_selected(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) {
        msgbox_warn(self.hwnd, L"Stop", L"Select a row first.");
        return;
    }

    log_linef(self, L"%s: %s\u2026", name, (kind == KIND_SVC) ? L"stop requested" : L"terminate requested");
    int wait_ms = 0;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        wait_ms = self.stop_wait_ms;
    }
    action_enqueue(self, kind, ACTION_STOP, name, L"manual stop", wait_ms);
}

static void start_selected(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) {
        msgbox_warn(self.hwnd, L"Start", L"Select a row first.");
        return;
    }

    log_linef(self, L"%s: %s\u2026", name, (kind == KIND_SVC) ? L"start requested" : L"launch requested");
    int wait_ms = 0;
    {
        std::lock_guard<std::mutex> lk(self.mtx);
        wait_ms = self.stop_wait_ms; // reuse existing wait window
    }
    action_enqueue(self, kind, ACTION_START, name, L"manual start", wait_ms);
}

// Row checkbox toggle
static void toggle_autostop_by_row(App& self, int kind, int row) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;

    wchar_t name[512];
    ListView_GetItemText(lv, row, 1, name, 512);
    if (!name[0]) return;

    bool enabled = (ListView_GetCheckState(lv, row) != 0);

    {
        std::lock_guard<std::mutex> lk(self.mtx);
        Item* it = list_find(&self.items[kind], name);
        if (it) it->auto_stop = enabled;
    }

    log_linef(self, L"%s: auto-stop %s", name, enabled ? L"enabled" : L"disabled");
    request_save_debounced(self);
}

// ----------------------------
// Main window proc
// ----------------------------
static void set_tab_sel_if(HWND tab, int sel) {
    if (!tab) return;
    int cur = TabCtrl_GetCurSel(tab);
    if (cur != sel) TabCtrl_SetCurSel(tab, sel);
}

static void sync_tabs_to(App& self, int sel) {
    if (sel < 0) sel = 0;
    if (sel > 1) sel = 1;

    set_tab_sel_if(self.left_tabs, sel);
    set_tab_sel_if(self.center_tabs, sel);

    // Keep both sides in lock-step so the left actions always correspond
    // to the visible center list.
    show_only(self.left_page_svc, self.left_page_exe, (sel == 0));
    show_only(self.center_page_svc, self.center_page_exe, (sel == 0));
}
static void restart_ui_timer(App& self, HWND hwnd) {
    KillTimer(hwnd, TIMER_UI_REFRESH);
    int ms = std::max(200, self.ui_refresh_ms);
    SetTimer(hwnd, TIMER_UI_REFRESH, (UINT)ms, NULL);
}


// --------------------------------------------------
// Main window proc (final-form instance binding)
// Stores `App*` in GWLP_USERDATA and forwards to AppWndProc(self,...).
// --------------------------------------------------
static LRESULT AppWndProc(App& self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self_ptr));
            self_ptr->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self_ptr = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self_ptr) return DefWindowProcW(hwnd, msg, wParam, lParam);

    App& self = *self_ptr;
    return AppWndProc(self, hwnd, msg, wParam, lParam);
}

static LRESULT AppWndProc(App& self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    
if (self.taskbar_restart_msg && msg == self.taskbar_restart_msg) {
    // Explorer restarted; re-add tray icon
    self.tray_added = false;
    tray_sync(self, GetModuleHandleW(NULL));
    return 0;
}

switch (msg) {
    case WM_CREATE: {
        self.hwnd = hwnd;

        // Capture initial DPI for this window and create a DPI-correct UI font.
        self.dpi = get_dpi_for_hwnd(hwnd);
        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
            self.ui_font = NULL;
        }
        self.ui_font = create_ui_font_for_dpi(self.dpi);

        // Optional: modern titlebar/backdrop (harmless if unsupported)
        try_enable_dark_titlebar(hwnd);
        try_enable_mica_backdrop(hwnd);

        build_ui(self, hwnd);

        HMENU menubar = CreateMenu();
        HMENU settings = CreatePopupMenu();
        AppendMenuW(settings, MF_STRING, IDM_PREFS, L"Preferences\u2026");
        AppendMenuW(settings, MF_STRING, IDM_PROFILES, L"Profiles\u2026");
        AppendMenuW(settings, MF_SEPARATOR, 0, NULL);
        AppendMenuW(settings, MF_STRING, IDM_HELP, L"Help\u2026");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)settings, L"Settings");
        AppendMenuW(menubar, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menubar, MF_STRING, IDM_EXIT, L"Exit");
        SetMenu(hwnd, menubar);

// Register for Explorer restart message so we can re-add the tray icon
self.taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");



        // Allow Explorer to deliver tray callback messages to an elevated window
        allow_uipi_message(hwnd, WM_APP_TRAYICON);
        if (self.taskbar_restart_msg) allow_uipi_message(hwnd, self.taskbar_restart_msg);

        if (!enum_services_build_disp_cache(&self.disp_cache)) {
            log_linef(self, L"Display cache: enumerate failed");
        }

        // Start UI timer early (may be restarted after config load)
        restart_ui_timer(self, hwnd);

        load_config(self);

// Apply tray icon setting from config
tray_sync(self, GetModuleHandleW(NULL));


        // Start background workers (C++20 std::jthread: auto-join + cooperative stop).
        self.action_thread = std::jthread(action_thread_main, &self);
        self.monitor_thread = std::jthread(monitor_thread_main, &self);

        update_statusbar(self);
        return 0;
    }

    case WM_DPICHANGED: {
        // Per-monitor DPI change (move between monitors / change scaling)
        UINT oldDpi = self.dpi ? self.dpi : 96;
        UINT newDpi = HIWORD(wParam);
        if (newDpi == 0) newDpi = get_dpi_for_hwnd(hwnd);
        self.dpi = newDpi;

        // Keep the Activity panel width feeling consistent across DPI changes
        if (self.activity_panel_w > 0 && oldDpi && newDpi && oldDpi != newDpi) {
            self.activity_panel_w = MulDiv(self.activity_panel_w, (int)newDpi, (int)oldDpi);
        }

        // Resize to suggested bounds
        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        if (prc) {
            SetWindowPos(hwnd, NULL, prc->left, prc->top,
                prc->right - prc->left, prc->bottom - prc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // Recreate DPI-correct font and reapply
        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
            self.ui_font = NULL;
        }
        self.ui_font = create_ui_font_for_dpi(self.dpi);
        if (self.ui_font) apply_font_recursive(hwnd, self.ui_font);

        // Recreate small imagelists at the new DPI for crisp ListView icons
        int iconPx = dpi_scale(self.dpi, 16);
        if (iconPx < 12) iconPx = 12;
        if (self.il_svc) { ImageList_Destroy(self.il_svc); self.il_svc = NULL; }
        if (self.il_exe) { ImageList_Destroy(self.il_exe); self.il_exe = NULL; }
        self.il_svc = ImageList_Create(iconPx, iconPx, ILC_COLOR32 | ILC_MASK, 4, 4);
        self.il_exe = ImageList_Create(iconPx, iconPx, ILC_COLOR32 | ILC_MASK, 4, 4);
        if (self.il_svc) {
            HICON hSvc = (HICON)LoadImageW(NULL, IDI_INFORMATION, IMAGE_ICON, iconPx, iconPx, LR_SHARED);
            HICON hExe = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, iconPx, iconPx, LR_SHARED);
            if (hSvc) ImageList_AddIcon(self.il_svc, hSvc);
            if (hExe) ImageList_AddIcon(self.il_svc, hExe);
            if (hSvc) ImageList_AddIcon(self.il_exe, hSvc);
            if (hExe) ImageList_AddIcon(self.il_exe, hExe);
            if (self.lv_svc) ListView_SetImageList(self.lv_svc, self.il_svc, LVSIL_SMALL);
            if (self.lv_exe) ListView_SetImageList(self.lv_exe, self.il_exe, LVSIL_SMALL);
        }

        // Layout with the new scaling
        layout(self, hwnd);
        return 0;
    }

    case WM_SIZE:
        // IMPORTANT: When minimized, Windows sends WM_SIZE with a 0x0 client area.
        // If we run our layout logic here, we clamp the right "Activity" panel
        // to a tiny width and permanently overwrite the user's splitter size.
        // That shows up as the internal panels "reset" when you restore the window.
        if (wParam == SIZE_MINIMIZED) return 0;
        layout(self, hwnd);
        return 0;

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lParam;
        
if (nh->idFrom == IDC_LEFT_TABS && nh->code == TCN_SELCHANGE) {
    int sel = TabCtrl_GetCurSel(self.left_tabs);
    sync_tabs_to(self, sel);
    return 0;
}
if (nh->idFrom == IDC_CENTER_TABS && nh->code == TCN_SELCHANGE) {
    int sel = TabCtrl_GetCurSel(self.center_tabs);
    sync_tabs_to(self, sel);
    return 0;
}
        if ((nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE) && nh->code == LVN_ITEMCHANGED) {
            if (self.suppress_lv_notify) return 0;
            NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
            if (lv->uChanged & LVIF_STATE) {
                int kind = (nh->idFrom == IDC_LV_SVC) ? KIND_SVC : KIND_EXE;
                if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) {
                    toggle_autostop_by_row(self, kind, lv->iItem);
                }
            }
        }

// Double-click / Enter opens a quick details dialog
if ((nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE) &&
    (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE)) {
    int kind = (nh->idFrom == IDC_LV_SVC) ? KIND_SVC : KIND_EXE;
    show_item_details(self, kind);
    return 0;
}

// Ctrl+C copies selected name (works even if only focused)
if ((nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE) && nh->code == LVN_KEYDOWN) {
    NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
    if (kd) {
        int kind = (nh->idFrom == IDC_LV_SVC) ? KIND_SVC : KIND_EXE;
        if ((GetKeyState(VK_CONTROL) & 0x8000) && kd->wVKey == 'C') {
            Item* it = lv_get_selected_item_ptr(self, kind);
            if (it) clipboard_set_text(hwnd, it->name.c_str());
            return 0;
        }
        if (kd->wVKey == VK_RETURN) {
            show_item_details(self, kind);
            return 0;
        }
    }
}

        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Live filtering (search boxes)
        if ((id == IDC_SVC_SEARCH_EDIT || id == IDC_EXE_SEARCH_EDIT) && code == EN_CHANGE) {
            wchar_t buf[512]; buf[0] = 0;
            if (id == IDC_SVC_SEARCH_EDIT && self.svc_search) GetWindowTextW(self.svc_search, buf, 512);
            if (id == IDC_EXE_SEARCH_EDIT && self.exe_search) GetWindowTextW(self.exe_search, buf, 512);
            trim_ws_inplace(buf);
            int kind = (id == IDC_SVC_SEARCH_EDIT) ? KIND_SVC : KIND_EXE;
            self.filter[kind] = to_lower_ws(buf);
            rebuild_listview_filtered(self, kind);
            update_statusbar(self);
            return 0;
        }

        // Browse… (EXE tab)
        if (id == IDC_EXE_BROWSE_BTN && code == BN_CLICKED) {
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                if (self.exe_add_edit) SetWindowTextW(self.exe_add_edit, file);
            }
            return 0;
        }

        if (id == IDM_PREFS) { open_prefs_dialog(self, hwnd); return 0; }
        if (id == IDM_PROFILES) { open_profiles_dialog(self, hwnd); return 0; }
        if (id == IDM_HELP)  { open_help_dialog(self, hwnd);  return 0; }
        if (id == IDM_EXIT)  { app_request_exit(self, hwnd); return 0; }

        
if (id == IDM_TRAY_SHOWHIDE) {
    if (IsWindowVisible(hwnd)) hide_main_window(self); else show_main_window(self);
    return 0;
}
if (id == IDM_TRAY_PREFS) { open_prefs_dialog(self, hwnd); return 0; }
if (id == IDM_TRAY_PROFILES) { open_profiles_dialog(self, hwnd); return 0; }
if (id == IDM_TRAY_CLOSE_TO_TRAY) {
    self.close_to_tray = !self.close_to_tray;
    if (self.close_to_tray) self.tray_enabled = true;
    request_save_debounced(self);
    return 0;
}
if (id == IDM_TRAY_EXIT) { app_request_exit(self, hwnd); return 0; }
switch (id) {
        case IDC_SVC_ADD_BTN: add_item(self, KIND_SVC); return 0;
        case IDC_EXE_ADD_BTN: add_item(self, KIND_EXE); return 0;
        case IDC_SVC_REMOVE_BTN: remove_selected(self, KIND_SVC); return 0;
        case IDC_EXE_REMOVE_BTN: remove_selected(self, KIND_EXE); return 0;
        case IDC_SVC_STOP_BTN:  stop_selected(self, KIND_SVC); return 0;
        case IDC_SVC_START_BTN: start_selected(self, KIND_SVC); return 0;
        case IDC_EXE_STOP_BTN:  stop_selected(self, KIND_EXE); return 0;
        case IDC_EXE_START_BTN: start_selected(self, KIND_EXE); return 0;
        }
        return 0;
    }

    case WM_APP_PROFILE_SWITCH: {
        int idx = (int)(INT_PTR)wParam;
        apply_profile_index(self, idx);
        return 0;
    }

    case WM_APP_RESTART_UI_TIMER:
        restart_ui_timer(self, hwnd);
        return 0;

    case WM_APP_LOG: {
        LogMsg* m = (LogMsg*)lParam;
        if (m) {
            log_linef(self, L"%s", m->text ? m->text : L"");
            free(m->text);
            free(m);
        }
        return 0;
    }


    case WM_APP_STATUS_BULK: {
        StatusBulkMsg* m = (StatusBulkMsg*)lParam;
        if (m) {
            uint8_t* base = (uint8_t*)m;
            StatusBulkEntry* e = (StatusBulkEntry*)(m + 1);

            {
                std::lock_guard<std::mutex> lk(self.mtx);
                for (uint32_t i = 0; i < m->n; i++) {
                if (e[i].name_off == 0) continue;
                wchar_t* name = (wchar_t*)(base + e[i].name_off);
                if (!name || !name[0]) continue;

                Item* it = list_find(&self.items[m->kind], name);
                if (it) {
                    StringCchCopyW(it->last_status, 32, e[i].status);
                    it->last_update_wall = m->wall_ts;
                    lv_set_row(self, m->kind == KIND_SVC ? self.lv_svc : self.lv_exe, it);
                }
                }
            }

            self.last_any_status_wall = m->wall_ts;
            self.lv_needs_layout[m->kind] = true;

            free(m);
        }
        return 0;
    }



    case WM_APP_STATUS: {
        StatusMsg* m = (StatusMsg*)lParam;
        if (m && m->name) {
            {
                std::lock_guard<std::mutex> lk(self.mtx);
                Item* it = list_find(&self.items[m->kind], m->name);
                if (it) {
                StringCchCopyW(it->last_status, 32, m->status);
                it->last_update_wall = m->wall_ts;
                lv_set_row(self, m->kind == KIND_SVC ? self.lv_svc : self.lv_exe, it);
                }
            }

            self.last_any_status_wall = m->wall_ts;
            self.lv_needs_layout[m->kind] = true;

            free(m->name);
            free(m);
        }
        return 0;
    }

    case WM_APP_REQUEST_SAVE:
        if (!self.save_pending) {
            self.save_pending = true;
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        } else {
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DEBOUNCE_SAVE) {
            KillTimer(hwnd, TIMER_DEBOUNCE_SAVE);
            self.save_pending = false;
            save_config_now(self);
            return 0;
        }
        if (wParam == TIMER_UI_REFRESH) {
            // Throttled UI maintenance
            if (self.lv_needs_layout[KIND_SVC]) {
                lv_autosize_status_last(self.lv_svc);
                lv_apply_name_fill(self.lv_svc);
                self.lv_needs_layout[KIND_SVC] = false;
            }
            if (self.lv_needs_layout[KIND_EXE]) {
                lv_autosize_status_last(self.lv_exe);
                lv_apply_name_fill(self.lv_exe);
                self.lv_needs_layout[KIND_EXE] = false;
            }
            update_statusbar(self);
            return 0;
        }
        return 0;

    

    case WM_CONTEXTMENU: {
        HWND target = (HWND)wParam;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (target == self.lv_svc || target == self.lv_exe) {
            int kind = (target == self.lv_svc) ? KIND_SVC : KIND_EXE;
            HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;

            // If invoked via keyboard, place the menu near selection.
            if (pt.x == -1 && pt.y == -1) {
                int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
                if (sel < 0) sel = ListView_GetSelectionMark(lv);
                RECT ir{};
                if (sel >= 0 && ListView_GetItemRect(lv, sel, &ir, LVIR_BOUNDS)) {
                    pt.x = ir.left + 20;
                    pt.y = ir.top + 20;
                    ClientToScreen(lv, &pt);
                } else {
                    GetCursorPos(&pt);
                }
            } else {
                // Hit-test and select the row under the cursor
                POINT cpt = pt;
                ScreenToClient(lv, &cpt);
                LVHITTESTINFO ht{};
                ht.pt = cpt;
                int hit = ListView_HitTest(lv, &ht);
                if (hit >= 0) {
                    ListView_SetItemState(lv, hit, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(lv, hit, FALSE);
                }
            }

            HMENU m = CreatePopupMenu();
            if (!m) return 0;

            AppendMenuW(m, MF_STRING, IDM_CTX_STOP, L"Stop");
            AppendMenuW(m, MF_STRING, IDM_CTX_REMOVE, L"Remove");
            AppendMenuW(m, MF_STRING, IDM_CTX_TOGGLE_AUTO, L"Toggle auto-stop");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_CTX_COPY_NAME, L"Copy name");
            if (kind == KIND_EXE) AppendMenuW(m, MF_STRING, IDM_CTX_OPEN_LOC, L"Open file location");

            SetForegroundWindow(hwnd); // menu dismiss behavior
            UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);

            switch (cmd) {
            case IDM_CTX_STOP:        stop_selected(self, kind); break;
            case IDM_CTX_REMOVE:      remove_selected(self, kind); break;
            case IDM_CTX_TOGGLE_AUTO: toggle_autostop_selected(self, kind); break;
            case IDM_CTX_COPY_NAME: {
                Item* it = lv_get_selected_item_ptr(self, kind);
                if (it) clipboard_set_text(hwnd, it->name.c_str());
            } break;
            case IDM_CTX_OPEN_LOC: {
                Item* it = lv_get_selected_item_ptr(self, kind);
                if (it) open_file_location_best_effort(self, it->name.c_str());
            } break;
            default: break;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

case WM_APP_TRAYICON: {
        // Tray callback:
        //   wParam = icon ID (TRAY_UID)
        //   lParam = mouse/key notification message (WM_* / NIN_*)
        // NOTE: lParam is NOT a packed POINT.
        switch ((UINT)lParam) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case NIN_BALLOONUSERCLICK:
            show_main_window(self);
            return 0;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_show_menu(hwnd);
            return 0;

        default:
            return 0;
        }
    }
    case WM_CLOSE:
        if (!self.force_quit && self.close_to_tray && self.tray_enabled) {
            hide_main_window(self);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        tray_remove(self);

        if (self.il_svc) { ImageList_Destroy(self.il_svc); self.il_svc = NULL; }
        if (self.il_exe) { ImageList_Destroy(self.il_exe); self.il_exe = NULL; }

        // Stop background threads before freeing shared state.
        if (self.monitor_thread.joinable()) self.monitor_thread.request_stop();
        if (self.action_thread.joinable()) self.action_thread.request_stop();
        self.action_cv.notify_all();

        if (self.monitor_thread.joinable()) self.monitor_thread.join();
        if (self.action_thread.joinable()) self.action_thread.join();

        save_config_now(self);

        {
            std::lock_guard<std::mutex> lk(self.action_mtx);
            self.action_q.clear();
        }

        dispcache_free(&self.disp_cache);
        list_free(&self.items[KIND_SVC]);
        list_free(&self.items[KIND_EXE]);

        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
        }

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----------------------------
// Entry
// ----------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    App app_instance{};
    
    App& self = app_instance;

    enable_dpi_awareness();
    ensure_admin_or_exit();

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);

    list_init(&self.items[KIND_SVC]);
    list_init(&self.items[KIND_EXE]);


    self.items_gen[KIND_SVC] = 1;
    self.items_gen[KIND_EXE] = 1;
    self.ui_refresh_ms = 1000;
    self.monitor_interval_ms = 1000;
    self.autostop_cooldown_ms = 15000;
    self.stop_wait_ms = 10000;

    self.active_profile = -1;
    self.have_default_cfg = false;

self.tray_enabled = true;   // show tray icon by default
self.close_to_tray = false; // X closes normally unless enabled
self.tray_added = false;
self.force_quit = false;
self.taskbar_restart_msg = 0;

    self.activity_panel_w = 360;
    self.last_any_status_wall = 0;

    compute_default_cfg_path(self.cfg_path, MAX_PATH);

    // Font is created once the main window exists (needs window DPI)
    self.ui_font = NULL;
    self.app_title = exe_stem_title();
	
		WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = MainWndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = L"SvcExeMonitorMain_C";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

	wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
								 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

	wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
								   IMAGE_ICON,
								   GetSystemMetrics(SM_CXSMICON),
								   GetSystemMetrics(SM_CYSMICON),
								   0);
	RegisterClassExW(&wc);
	
    HWND hwnd = CreateWindowW(
        wc.lpszClassName, self.app_title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 600,
        NULL, NULL, hInst, &app_instance
    );
    if (!hwnd) return 0;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}