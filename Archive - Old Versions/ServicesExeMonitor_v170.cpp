// ServicesExeMonitor
// C++20 Win32 (Unicode) Windows Service + EXE Monitor with Auto-Stop Policy
//
// Build (MinGW-w64, C++20):
//   g++ -std=c++20 -municode -mwindows -O2 -Wall -Wextra ServicesExeMonitor_v95.cpp app_res.o -o ServicesExeMonitor.exe ^
//   -lcomctl32 -ladvapi32 -lshell32 -lgdi32 -luser32 -luxtheme -ldwmapi -lshlwapi -lcomdlg32 -static -static-libgcc -static-libstdc++
//
// Notes:
// - Config file will be created next to the .exe

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>

#ifndef SERVICE_CONFIG_DESCRIPTION
#define SERVICE_CONFIG_DESCRIPTION 1
#endif


#ifndef ERROR_SERVICE_BUSY
#define ERROR_SERVICE_BUSY 1052
#endif
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
#include <string_view>
#include <chrono>

#include <cstdint>
#include <cstddef>
// --------------------------------------------------
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

struct StartedProc {
    // The exact target we attempted to shell-open (normalized best-effort).
    std::wstring started_target;

    // If the target was an .exe but ShellExecuteEx did not return a distinct process handle
    // (common when an app reuses an existing instance), we still keep the lowercased basename
    // so we can optionally stop by name on profile deactivation.
    std::wstring exe_lower; // e.g. "notepad.exe"

    DWORD pid = 0;
    unique_handle hproc; // may be empty if ShellExecuteEx didn't return a process handle
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
#include <cwctype>   // towlower

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

// Strongly-typed kind to avoid accidental mixing of ints.
enum class ItemKind : uint8_t { Svc = 0, Exe = 1 };
static constexpr ItemKind kind_from_int(int k) {
    return (k == KIND_EXE) ? ItemKind::Exe : ItemKind::Svc;
}
static constexpr int kind_index(ItemKind k) {
    return (k == ItemKind::Exe) ? KIND_EXE : KIND_SVC;
}
static constexpr const wchar_t* kind_name(ItemKind k) {
    return (k == ItemKind::Exe) ? L"exe" : L"svc";
}

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
#define WM_APP_STATUS_GEN       (WM_APP + 7)
#define WM_APP_MODEL_DIRTY      (WM_APP + 8)
#define TIMER_DEBOUNCE_SAVE  102
#define TIMER_UI_REFRESH     103

#define TIMER_LIVE_RESIZE    104
#define TIMER_SEARCH_DEBOUNCE 105
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
#define IDM_DARKMODE  40010
#define IDM_EXIT   40003

// Tray menu / commands
#define IDM_TRAY_SHOWHIDE      40004
#define IDM_TRAY_PREFS         40005
#define IDM_TRAY_PROFILES      40009
#define IDM_TRAY_CLOSE_TO_TRAY 40006
#define IDM_TRAY_EXIT          40007
#define IDM_TRAY_DARKMODE      40011
#define IDM_RESET_COLUMNS     40012
#define IDM_HOTKEYS          40013
#define IDM_DIAGNOSTICS     40015
#define IDM_EXIT_TO_TRAY      40014
#define IDM_FOCUS_SEARCH 40025
#define IDM_TAB_SVC           40016
#define IDM_TAB_EXE           40017
#define IDM_SAVE_CONFIG_NOW   40018
#define IDM_FOCUS_MAIN_LIST   40019
#define IDM_REFRESH_UI        40020
// Control IDs
#define IDC_HOTKEYS_LIST  7201
#define IDC_HOTKEYS_COPY  7202

#define IDC_LEFT_TABS      5001
#define IDC_CENTER_TABS    5002
#define IDC_TOP_SETTINGS   5003
#define IDC_TOP_QUIT       5004
#define IDC_TOP_EXIT_TO_TRAY 5005

#define IDC_STATUSBAR      5900

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
#define IDM_CTX_SET_START_AUTO 40105
#define IDM_CTX_SET_START_MANUAL 40106
#define IDM_CTX_SET_START_DISABLED 40107

// Center listviews
#define IDC_LV_SVC  6001
#define IDC_LV_EXE  6002

// ---- ID range guards (maintenance) -----------------------------------------
// Keep main-window controls and dialogs in separate numeric bands to avoid
// collisions when refactoring modeless dialogs into child panes.
// Main window controls: 6000-6999
// Dialog/tools controls: 7200-7999
#define IDC_ID_GUARDS 1
static_assert(IDC_LV_SVC != IDC_LV_EXE, "Listview IDs must be distinct");
static_assert(IDC_HOTKEYS_LIST >= 7200 && IDC_HOTKEYS_LIST < 8000, "Hotkeys dialog IDs must live in dialog ID band");
static_assert(IDC_HOTKEYS_COPY >= 7200 && IDC_HOTKEYS_COPY < 8000, "Hotkeys dialog IDs must live in dialog ID band");
#define IDC_SPLITTER 7002

// Splitter is a main-window control; keep it out of dialog range.
static_assert(IDC_SPLITTER < 7200, "Main window IDs should stay below dialog ID band");

// Activity listbox
#define IDC_ACTIVITY 7001

// Tray icon
#define WM_APP_TRAYICON   (WM_APP + 10)
// One-time deferred layout after startup / show / position changes.
// Helps avoid the "tiny box until resize" issue on some systems.
#define WM_APP_INITIAL_LAYOUT (WM_APP + 42)
#define TRAY_UID          1
#define IDC_LEFT_HSPLIT_SVC 7003
#define IDC_LEFT_HSPLIT_EXE 7004


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
#define IDC_PREF_DARKMODE     8008

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


#define IDC_PROFILES_TAB       8325
// Tray forward declarations
static void tray_remove(App& self);
static void tray_add(App& self, HINSTANCE hInst);
static void tray_sync(App& self, HINSTANCE hInst);
static void show_main_window(App& self);
static void hide_main_window(App& self);
static void app_request_exit(App& self, HWND hwnd);
static void tray_show_menu(HWND hwnd);
static void darkmode_set_app(bool want_dark);
// --- forward declarations needed before first use (kept minimal) ---
static void theme_compute(App& self);
static void theme_apply_all_controls(App& self);
static void request_save_debounced(App& self);
static int process_count_by_name_lower(const wchar_t* exe_lower_name);
static void rebuild_profile_watch_keys_locked(App& self);
static std::wstring normalize_start_target_best_effort(const std::wstring& raw);
static std::wstring normalize_start_target_best_effort(const wchar_t* raw);
static void lv_set_columns(HWND lv, const wchar_t* c0, const wchar_t* c1, const wchar_t* c2, const wchar_t* c3);
static bool lv_set_row_with(App& self, HWND lv, Item* it, const wchar_t* status_raw, time_t wall_ts);
static bool lv_update_row_existing_with(App& self, HWND lv, int kind, Item* it, const wchar_t* status_raw, time_t wall_ts, int* out_row_idx);
static bool lv_update_row_existing_with(App& self, HWND lv, ItemKind kind, Item* it, const wchar_t* status_raw, time_t wall_ts, int* out_row_idx);
static int lv_find_item_by_uid(HWND lv, uint32_t uid);
static void post_model_dirty(App& self);
static bool lv_set_row(App& self, HWND lv, Item* it);
static void log_linef(App& self, const wchar_t* fmt, ...);
static void post_log(App& self, const wchar_t* text);
static void post_status_bulk(App& self, int kind, wchar_t** names, wchar_t (*statuses)[32], size_t n, time_t wall_ts);
static bool shell_open_and_track_process(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch);
static void close_process_best_effort(DWORD pid, HANDLE hproc_in, int wait_ms, wchar_t* out_result, size_t cch);


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
    if (!s) return nullptr;
    size_t n = wcslen(s);
    wchar_t* p = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!p) return nullptr;
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


static int dpi_scale(UINT dpi, int v96) {
    return MulDiv(v96, (int)dpi, 96);
}

static void listbox_adjust_item_height(HWND lb, UINT dpi) {
    if (!lb) return;

    // Force a DPI-appropriate item height so text doesn't look cramped/clipped
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

static bool try_enable_dark_titlebar(HWND hwnd, bool enable) {
    if (!hwnd) return false;

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return false;

    PFN_DwmSetWindowAttribute p = nullptr;
    LOAD_PROC(dwm, "DwmSetWindowAttribute", PFN_DwmSetWindowAttribute, p);

    bool ok = false;
    if (p) {
        // Common attribute IDs used in the wild; you can keep your existing logic here.
        // Example: DWMWA_USE_IMMERSIVE_DARK_MODE often 19/20 depending on Windows build.
        BOOL on = enable ? TRUE : FALSE;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_1 = 19;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_2 = 20;
        p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_1, &on, sizeof(on));
        p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_2, &on, sizeof(on));
        ok = true;
    }

    FreeLibrary(dwm);
    return ok;
}

static bool try_set_explorer_theme(HWND hwnd) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return false;

    PFN_SetWindowTheme p = nullptr;
    LOAD_PROC(ux, "SetWindowTheme", PFN_SetWindowTheme, p);

    bool ok = false;
    if (p) {
        p(hwnd, L"Explorer", nullptr);
        ok = true;
    }

    FreeLibrary(ux);
    return ok;
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

static bool try_enable_mica_backdrop(HWND hwnd) {
    // Best-effort Windows 11 system backdrop. Safe no-op on older builds.
    if (!hwnd) return false;

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return false;

    PFN_DwmSetWindowAttribute p = nullptr;
    LOAD_PROC(dwm, "DwmSetWindowAttribute", PFN_DwmSetWindowAttribute, p);

    bool ok = false;
    if (p) {
        // DWMWA_SYSTEMBACKDROP_TYPE == 38 (Win11)
        const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
        // 2 = Mica, 3 = Acrylic, 4 = Tabbed (varies by build; Tabbed looks good for utilities)
        int backdrop = 2;
        p(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        ok = true;
    }

    FreeLibrary(dwm);
    return ok;
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
static Item* lv_get_selected_item_ptr(App& self, ItemKind kind);
static inline Item* lv_get_selected_item_ptr(App& self, int kind) {
    return lv_get_selected_item_ptr(self, kind_from_int(kind));
}
static void toggle_autostop_selected(App& self, int kind);
static bool set_service_start_type(const wchar_t* svc_name, DWORD start_type, DWORD* out_err);
static void set_service_start_type_selected(App& self, DWORD start_type);
static void show_item_details(App& self, int kind);

static void update_left_info_pane(App& self, ItemKind kind, Item* it);
static inline void update_left_info_pane(App& self, int kind, Item* it) {
    update_left_info_pane(self, kind_from_int(kind), it);
}
static std::wstring query_service_description_best_effort(const std::wstring& svc_name, DWORD& out_err);

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
    uint32_t uid = 0;                   // stable ID for UI lParam mapping
    std::wstring name;                   // owning
    bool auto_stop = false;
    int img = 0;                         // small icon index for ListView

    std::wstring exe_path;              // optional launch path (EXE rows)


    // Service-only: cached description (queried on demand for info pane)
    bool svc_desc_loaded = false;
    DWORD svc_desc_last_err = 0;
    std::wstring svc_desc;

    wchar_t  last_status[32]{};          // small fixed buffer keeps LV code simple
    time_t   last_update_wall = 0;


    // UI-side cache to avoid repainting unchanged rows
    wchar_t  ui_last_status[32]{};
    time_t   ui_last_update_wall = 0;
    // Cached ListView display strings (avoid repeated formatting / allocations)
    // These are for the *displayed* subitems, not the raw model fields.
    wchar_t  ui_cache_status_disp[64]{};   // e.g. "* running" or "running (Profile)"
    wchar_t  ui_cache_last_text[64]{};     // formatted local time string

    uint32_t status_gen = 0;           // increments when last_status changes (model)
    uint32_t ui_applied_gen = 0;       // last applied to ListView (UI thread)
    uint64_t last_autostop_mono_ms = 0;
    int      autostop_count = 0;

    // Service flapping detection (restarts within a short window)
    time_t   flap_window_start_wall = 0;
    time_t   flap_last_change_wall = 0;
    uint16_t flap_changes = 0;
    uint16_t flap_restarts = 0; // STOPPED->RUNNING count
    bool     flap_flagged = false;

    // Service stop error backoff (prevents infinite retry loops on repeated Win32 errors)
    uint32_t svc_stop_fail_streak = 0;    // consecutive stop failures
    uint64_t svc_stop_suppress_until_ms = 0; // mono-ms until which AUTO-STOP won't enqueue
    DWORD    svc_last_stop_err = 0;       // last stop Win32 error (0 if none)
};

struct ItemList {
    std::vector<std::unique_ptr<Item>> v;

    // Fast name -> Item* lookup (case-insensitive, allocation-free lookup via heterogeneous find()).
    struct WStrCIHash {
        using is_transparent = void;
        static inline uint64_t fnv1a64_ci(std::wstring_view s) noexcept {
            uint64_t h = 1469598103934665603ULL;
            for (wchar_t ch : s) {
                wchar_t c = (wchar_t)towlower(ch);
                h ^= (uint64_t)(uint16_t)c;
                h *= 1099511628211ULL;
            }
            return h;
        }
        size_t operator()(std::wstring_view s) const noexcept { return (size_t)fnv1a64_ci(s); }
        size_t operator()(const std::wstring& s) const noexcept { return (size_t)fnv1a64_ci(s); }
        size_t operator()(const wchar_t* s) const noexcept {
            if (!s) return 0;
            return (size_t)fnv1a64_ci(std::wstring_view(s));
        }
    };
    struct WStrCIEq {
        using is_transparent = void;
        bool operator()(std::wstring_view a, std::wstring_view b) const noexcept {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (towlower(a[i]) != towlower(b[i])) return false;
            }
            return true;
        }
        bool operator()(const std::wstring& a, const std::wstring& b) const noexcept { return (*this)(std::wstring_view(a), std::wstring_view(b)); }
        bool operator()(const std::wstring& a, const wchar_t* b) const noexcept { return b ? (*this)(std::wstring_view(a), std::wstring_view(b)) : false; }
        bool operator()(const wchar_t* a, const std::wstring& b) const noexcept { return a ? (*this)(std::wstring_view(a), std::wstring_view(b)) : false; }
    };

    std::unordered_map<std::wstring, Item*, WStrCIHash, WStrCIEq> by_name;
};

// NOTE: Keep the old helper names to minimize churn elsewhere in the file.
// The implementation keeps behavior identical but removes O(N) scans from hot paths.

static void list_init(ItemList* L) {
    if (!L) return;
    L->v.clear();
    L->by_name.clear();
}

[[maybe_unused]] static void list_free(ItemList* L) {
    if (!L) return;
    L->v.clear();
    L->by_name.clear();
}

static Item* list_find(ItemList* L, const wchar_t* name) {
    if (!L || !name || !name[0]) return nullptr;
    auto it = L->by_name.find(name); // heterogeneous, no allocation
    return (it == L->by_name.end()) ? NULL : it->second;
}

[[maybe_unused]] static bool list_push(ItemList* L, Item* it) {
    if (!L || !it) return false;
    // Prevent duplicates by name (case-insensitive).
    if (!it->name.empty()) {
        if (L->by_name.find(it->name) != L->by_name.end()) return false;
    }
    L->v.emplace_back(it); // takes ownership
    if (!it->name.empty()) L->by_name.emplace(it->name, it);
    return true;
}

static void list_remove_at(ItemList* L, size_t idx) {
    if (!L) return;
    if (idx >= L->v.size()) return;
    Item* it = L->v[idx].get();
    if (it && !it->name.empty()) L->by_name.erase(it->name);
    L->v.erase(L->v.begin() + (ptrdiff_t)idx);
}

[[maybe_unused]] static bool list_remove_name(ItemList* L, const wchar_t* name) {
    if (!L || !name || !name[0]) return false;

    // Lookup pointer quickly, then remove from the vector (order-preserving).
    Item* target = nullptr;
    auto it = L->by_name.find(name);
    if (it != L->by_name.end()) target = it->second;

    if (!target) return false;

    for (size_t i = 0; i < L->v.size(); i++) {
        if (L->v[i].get() == target) {
            list_remove_at(L, i);
            return true;
        }
    }
    // Fallback if vector/map got out of sync (shouldn't happen):
    L->by_name.erase(name);
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

// Profile Start Items: any file/path/target to shell-open when a profile activates.
// Examples: .exe, .lnk, .url, documents, folders.
struct StartItem {
    std::wstring target;
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
    std::vector<std::wstring> watch_keys_lower; // cached normalized watch keys (basename + lower); kept in sync with watch_exes
    std::vector<StartItem> start_items; // targets to shell-open on activation (any file type)
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
    if (!D || !disp) return nullptr;

    std::wstring tmp(disp);
    if (!tmp.empty()) {
        // CharLowerBuffW mutates in-place; data() is writable in C++20 for non-const strings.
        CharLowerBuffW(tmp.data(), (DWORD)tmp.size());
    }

    auto it = D->m.find(tmp);
    if (it == D->m.end()) return nullptr;
    return it->second.c_str();
}

// ----------------------------
// Action queue types

// ----------------------------
enum { ACTION_STOP = 0, ACTION_START = 1, ACTION_LAUNCH_ITEM = 2, ACTION_CLOSE_STARTED = 3 };

struct Action {
    int kind = 0;
    int op = ACTION_STOP;
    std::wstring name;
    std::wstring reason;
    int wait_ms = 0;

    // ACTION_LAUNCH_ITEM: the target to shell-open
    std::wstring target;

    // ACTION_CLOSE_STARTED: process identity we launched
    DWORD pid = 0;
    HANDLE hproc = NULL; // duplicated handle; action worker closes it
};

// ----------------------------
// App State
// ----------------------------
typedef struct App {
    HWND hwnd;
    HACCEL accel;

    // Custom top bar (replaces classic menu bar for reliable dark mode)
    HWND btn_settings;
    HWND btn_quit;
    HWND btn_exit_to_tray;
    HMENU menu_settings; // popup menu for Settings
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

    // View index (filtered/sorted): rows map to stable uids
    // ListView row i -> uid = view_uids[kind][i]
    std::vector<uint32_t> view_uids[2];


// Debounced search
bool filter_pending[2]{};
DWORD filter_debounce_last_ms = 0;

// Column persistence (4 columns: AutoStop checkbox, Name, Status, Last)
bool cols_have[2]{};
int  cols_w[2][4]{};      // pixel widths
int  cols_order[2][4]{};  // display order

// Empty-state labels (center pages)
HWND svc_empty;
HWND exe_empty;



    // Left info panes (bottom half of left panel)
    HWND svc_info_label;
    HWND svc_info_edit;
    HWND exe_info_label;
    HWND exe_info_edit;
    // Left-panel horizontal splitters (between controls and info pane)
    HWND svc_hsplit;
    HWND exe_hsplit;
    int left_split_y_svc; // in client coords of left page
    int left_split_y_exe;


    HWND activity;
    HWND activity_label;
    HWND statusbar;


    HWND splitter;
    int  activity_panel_w;   // resizable right panel total width
    bool splitter_dragging;
    int  splitter_drag_start_x;
    int  splitter_drag_start_w;
    HFONT ui_font;
    // Additional cached UI fonts for dialogs on other DPIs (per-monitor).
    // Note: self.ui_font is the main window font (for self.dpi). Cache stores other DPIs only.
    std::vector<std::pair<UINT, HFONT>> ui_font_cache;

    std::mutex mtx;
    ItemList items[2];

    uint32_t next_uid = 1;
    std::unordered_map<uint32_t, Item*> uid_map; // uid -> Item* (guarded by mtx)

    uint32_t items_gen[2]; // increments on add/remove; used to refresh monitor snapshots cheaply

    uint32_t status_gen[2]{}; // increments when any row's status changes (per kind)
    std::vector<Item*> dirty_status[2]; // pointers to Items with changed status since last UI apply
    std::unordered_map<const Item*, int> lv_row_of_item[2]; // UI: Item* -> ListView row (valid for current filter/view)

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

    // Cached profile WATCH exe keys (shared_ptr so monitor thread can read without allocations)
    std::shared_ptr<const std::vector<std::vector<std::wstring>>> profile_watch_keys_sp;

    std::shared_ptr<const std::vector<std::vector<uint64_t>>> profile_watch_hashes_sp; // hashed watch keys (lower-case)

    // Processes started automatically when a profile was activated (so we can close them when leaving that profile)
    std::vector<StartedProc> profile_started_procs;

    // For main-list UI indicator (only meaningful for monitored EXE rows)
    std::unordered_set<std::wstring> profile_started_exes;


    // tray
    bool tray_enabled;
    bool close_to_tray;
    bool tray_added;
    bool force_quit;
    NOTIFYICONDATAW nid;
    UINT taskbar_restart_msg;

    // Remember last window placement when hiding to tray (restores to prior position)
    WINDOWPLACEMENT last_wp;
    bool have_last_wp;
    // Persisted main window placement (saved on exit, restored on startup)
    int win_x, win_y, win_w, win_h;
    bool win_maximized;
    bool have_win_rect;



    // theme
    bool dark_mode;
    bool theme_owns_brushes;
    // Track which inputs the currently-built theme resources correspond to.
    // This makes theme_compute() idempotent and prevents unnecessary churn.
    UINT theme_built_dpi = 0;
    bool theme_built_dark = false;

    COLORREF col_bg;

    // --- Startup layout stabilization ---
    // On some launch paths, the window is created/shown without a reliable "final" WM_SIZE
    // after all children (tabs/listviews/status/activity) exist and DPI/font/theme are settled.
    // Symptoms: everything renders in a tiny box until the user resizes.
    // Fix: schedule a deferred one-time layout pass after the window has a non-trivial client
    // area (and again after show/pos-changed if needed).
    bool did_initial_layout;
    bool initial_layout_scheduled;
    UINT initial_layout_tries;
    COLORREF col_panel;
    COLORREF col_edit_bg;
    COLORREF col_text;
    COLORREF col_text_dim;
    HBRUSH br_bg;
    HBRUSH br_panel;
    HBRUSH br_edit;
    HBRUSH br_btn;
    // Theme change generation (decoupled from status/UI refresh)
    std::atomic<uint32_t> theme_gen{1};
    uint32_t ui_seen_theme_gen = 0;

    // Model/UI generation tracking (event-driven UI refresh)
    uint32_t ui_seen_status_gen[2]{};   // last applied status gen per kind
    bool ui_dirty_posted = false;       // coalesce WM_APP_MODEL_DIRTY
    bool ui_timer_suspended = false;    // pause TIMER_UI_REFRESH when minimized/tray
    bool in_size_move = false;        // live-resize drag in progress

    wchar_t cfg_path[MAX_PATH];

    std::jthread monitor_thread;
    std::jthread action_thread;


    // ------------------------------------------------------------
    // ACTION QUEUE THREADING RULE (deadlock prevention)
    //
    // Lock order is STRICT:
    //   - Never wait on action_cv / lock action_mtx while holding self.mtx.
    //   - All enqueue operations MUST happen without holding self.mtx.
    //
    // Enforce this by NEVER touching action_q/action_mtx/action_cv directly
    // outside of the helper APIs (see action_enqueue/action_qdepth/action_clear
    // and action_pop_wait).
    // ------------------------------------------------------------


    std::mutex action_mtx;
    std::condition_variable action_cv;
    std::deque<Action> action_q;

    bool save_pending;

    // UI throttling
    bool suppress_lv_notify;
    bool lv_needs_layout[2];
    time_t last_any_status_wall;

} App;



// ------------------------------------------------------------
// DEBUG-ONLY LOCK ORDER ENFORCEMENT
//
// Goal: prevent future deadlocks caused by lock inversion between:
//   - self.mtx (model/UI mutex)
//   - self.action_mtx + self.action_cv (action queue)
//
// Rule (enforced in debug builds):
//   - NEVER call action_enqueue/action_qdepth/action_clear/action_pop_wait
//     while holding self.mtx.
//   - In other words: do not hold the model lock when touching the action queue.
//
// Implementation:
//   - Track "model lock depth" per-thread whenever self.mtx is held.
//   - Assert in action-queue helper APIs that model lock depth == 0.
// ------------------------------------------------------------
#if !defined(NDEBUG)
static thread_local int g_model_lock_depth = 0;

static void dbg_model_lock_inc() { ++g_model_lock_depth; }
static void dbg_model_lock_dec() { --g_model_lock_depth; }

static void dbg_assert_model_unlocked(const char* where) {
    if (g_model_lock_depth != 0) {
        OutputDebugStringA("LOCK ORDER VIOLATION: action-queue API called while holding self.mtx at ");
        OutputDebugStringA(where);
        OutputDebugStringA("\n");
        DebugBreak();
    }
}
#else
static void dbg_model_lock_inc() {}
static void dbg_model_lock_dec() {}
static void dbg_assert_model_unlocked(const char*) {}
#endif

// Use this for locking self.mtx so debug builds can track model-lock ownership.
// Replace direct std::lock_guard<std::mutex>(self.mtx) with ModelLockGuard(self).
struct ModelLockGuard {
    std::lock_guard<std::mutex> lk;
    explicit ModelLockGuard(App& self_) : lk(self_.mtx) { dbg_model_lock_inc(); }
    ~ModelLockGuard() { dbg_model_lock_dec(); }
};

// UID helpers (must be called with self.mtx held)
static Item* uid_to_item_ptr_unlocked(App& self, uint32_t uid) {
    auto it = self.uid_map.find(uid);
    return (it == self.uid_map.end()) ? nullptr : it->second;
}





// --------------------------------------------------
// Centralized item registration / unregistration
//
// Invariants:
//  - All changes to uid_map, ItemList::by_name, dirty_status[], and lv_row_of_item[]
//    must go through these helpers.
//  - Call only while holding the model lock (self.mtx) via ModelLockGuard.
// --------------------------------------------------

static bool register_item_locked(App& self, ItemKind kind, Item* it) {
    if (!it) return false;
    const int k = kind_index(kind);

    // Assign uid if caller hasn't.
    if (it->uid == 0) it->uid = self.next_uid++;

    // Enforce unique name per kind (case-insensitive via by_name hash/eq).
    if (!it->name.empty()) {
        auto f = self.items[k].by_name.find(it->name);
        if (f != self.items[k].by_name.end()) return false;
    }

    // Take ownership and index.
    self.items[k].v.emplace_back(it);
    if (!it->name.empty()) self.items[k].by_name.emplace(it->name, it);
    self.uid_map[it->uid] = it;

    // Clean any stale UI caches for this pointer just in case (defensive).
    self.lv_row_of_item[k].erase(it);

    return true;
}

static void unregister_item_locked(App& self, ItemKind kind, Item* it) {
    if (!it) return;
    const int k = kind_index(kind);

    // uid_map
    if (it->uid) self.uid_map.erase(it->uid);

    // by_name (robust erase by pointer; name may have changed)
    {
        auto& bn = self.items[k].by_name;
        bool erased = false;
        if (!it->name.empty()) {
            auto f = bn.find(it->name);
            if (f != bn.end() && f->second == it) {
                bn.erase(f);
                erased = true;
            }
        }
        if (!erased) {
            for (auto i = bn.begin(); i != bn.end(); ) {
                if (i->second == it) i = bn.erase(i);
                else ++i;
            }
        }
    }

    // dirty_status (remove any queued pointers)
    {
        auto& ds = self.dirty_status[k];
        ds.erase(std::remove(ds.begin(), ds.end(), it), ds.end());
    }

    // UI row cache map
    self.lv_row_of_item[k].erase(it);

    // Remove ownership from vector (linear; sizes are typically modest)
    {
        auto& vec = self.items[k].v;
        for (size_t i = 0; i < vec.size(); i++) {
            if (vec[i].get() == it) {
                vec.erase(vec.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
}

static void unregister_all_items_locked(App& self, ItemKind kind) {
    const int k = kind_index(kind);
    // Keep popping from the back to avoid O(N^2) from repeated erase at front.
    while (!self.items[k].v.empty()) {
        Item* it = self.items[k].v.back().get();
        unregister_item_locked(self, kind, it);
    }
    // Defensive: clear per-kind caches fully
    self.dirty_status[k].clear();
    self.lv_row_of_item[k].clear();
    self.items[k].by_name.clear();
}

// Action-queue helpers (MUST be called without holding self.mtx)
// These centralize the lock ordering rule: model lock (self.mtx) is never held while
// waiting for / holding the action queue lock (self.action_mtx).
static void action_enqueue(App& self, Action&& a) {
    dbg_assert_model_unlocked(__FUNCTION__);

    {
        std::lock_guard<std::mutex> lk(self.action_mtx);
        self.action_q.emplace_back(std::move(a));
    }
    self.action_cv.notify_one();
}

static size_t action_qdepth(App& self) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::lock_guard<std::mutex> lk(self.action_mtx);
    return self.action_q.size();
}

static void action_clear(App& self) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::lock_guard<std::mutex> lk(self.action_mtx);
    self.action_q.clear();
}

// Only the action thread should call this.
static bool action_pop_wait(std::stop_token st, App& self, Action& out) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::unique_lock<std::mutex> lk(self.action_mtx);
    self.action_cv.wait(lk, [&] { return st.stop_requested() || !self.action_q.empty(); });
    if (st.stop_requested()) return false;
    out = std::move(self.action_q.front());
    self.action_q.pop_front();
    return true;
}



// --------------------------------------------------
// Forward declarations for cross-module calls (single-TU module layout)
// (Reordering modules means some helpers are defined later in the file.)
// --------------------------------------------------
static bool parse_setting_to_cfg(ConfigSnapshot& cfg, const wchar_t* key, const wchar_t* val);
static void snapshot_from_runtime_locked(App& self, ConfigSnapshot& out);
static void snapshot_active_from_runtime_locked(App& self);
static void snapshot_active_from_runtime(App& self);
static void apply_profile_index(App& self, int idx);
static void action_enqueue(App& self, int kind, int op, const wchar_t* name, const wchar_t* reason, int wait_ms);
static void update_statusbar(App& self);
static void apply_filter_now(App& self, int kind);
static void capture_columns_now(App& self, int kind);
static void apply_saved_columns(App& self, int kind);
static LRESULT CALLBACK SearchEditSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);


static bool parse_setting(App& self, const wchar_t* key, const wchar_t* val) {
    if (!key || !val) return false;

    // NOTE: Settings are stored in milliseconds (ms).
    // For backward compatibility, we still accept legacy *_s keys (seconds) and convert to ms.
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

    // Tray settings
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

    if (_wcsicmp(key, L"dark_mode") == 0) {
        int v = 0; parse_int(&v);
        self.dark_mode = (v != 0);
        return true;
    }
    if (_wcsicmp(key, L"win_rect") == 0) {
        // x,y,w,h,max (max: 0/1)
        int x=0,y=0,w=0,h=0,mx=0;
        if (swscanf(val, L"%d,%d,%d,%d,%d", &x,&y,&w,&h,&mx) == 5) {
            if (w >= 300 && h >= 200) {
                self.win_x = x;
                self.win_y = y;
                self.win_w = w;
                self.win_h = h;
                self.win_maximized = (mx != 0);
                self.have_win_rect = true;
                return true;
            }
        }
        return false;
    }

    if (_wcsicmp(key, L"activity_panel_w") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 160 && v <= 2000) { self.activity_panel_w = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"left_split_y_svc") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 80 && v <= 4000) { self.left_split_y_svc = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"left_split_y_exe") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 80 && v <= 4000) { self.left_split_y_exe = v; return true; }
        return false;
    }



    if (_wcsicmp(key, L"svc_colw") == 0 || _wcsicmp(key, L"exe_colw") == 0) {
    int kind = (_wcsicmp(key, L"svc_colw") == 0) ? KIND_SVC : KIND_EXE;
    int w[4] = {};
    if (swscanf(val, L"%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3]) == 4) {
        for (int i = 0; i < 4; ++i) self.cols_w[kind][i] = w[i];
        self.cols_have[kind] = true;
        return true;
    }
    return false;
}
if (_wcsicmp(key, L"svc_colorder") == 0 || _wcsicmp(key, L"exe_colorder") == 0) {
    int kind = (_wcsicmp(key, L"svc_colorder") == 0) ? KIND_SVC : KIND_EXE;
    int o[4] = {};
    if (swscanf(val, L"%d,%d,%d,%d", &o[0], &o[1], &o[2], &o[3]) == 4) {
        for (int i = 0; i < 4; ++i) self.cols_order[kind][i] = o[i];
        // Keep Auto-stop (column index 0) pinned to the left-most display position.
        int pos0 = -1;
        for (int i = 0; i < 4; ++i) if (self.cols_order[kind][i] == 0) { pos0 = i; break; }
        if (pos0 > 0) { int tmp = self.cols_order[kind][0]; self.cols_order[kind][0] = self.cols_order[kind][pos0]; self.cols_order[kind][pos0] = tmp; }
        self.cols_have[kind] = true;
        return true;
    }
    return false;
}

return false;
}


static void normalize_exe_input(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    if (!out || cch_out == 0) return;
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[512];
    StringCchCopyW(buf, 512, raw);
    trim_ws_inplace(buf);

    // Strip simple surrounding quotes.
    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, n * sizeof(wchar_t));
        }
    } else if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, n * sizeof(wchar_t));
        }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    // Expand environment vars (e.g., %SystemRoot%).
    wchar_t exp[1024];
    DWORD got = ExpandEnvironmentStringsW(buf, exp, 1024);
    const wchar_t* s = (got > 0 && got < 1024) ? exp : buf;

    // Keep only basename (users often paste full paths).
    const wchar_t* base = s;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') base = p + 1;
    }

    wchar_t name[512];
    StringCchCopyW(name, 512, base);
    trim_ws_inplace(name);
    if (!name[0]) return;

    if (!ends_with_i(name, L".exe")) {
        StringCchCatW(name, 512, L".exe");
    }

    // binsearch_nameidx is case-insensitive; no need to lowercase here.
    StringCchCopyW(out, cch_out, name);
}

static void resolve_exe_launch_path(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    if (!out || cch_out == 0) return;
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[1024];
    StringCchCopyW(buf, 1024, raw);
    trim_ws_inplace(buf);

    // Strip simple surrounding quotes.
    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, n * sizeof(wchar_t));
        }
    } else if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, n * sizeof(wchar_t));
        }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    // Expand env vars.
    wchar_t exp[1024];
    DWORD nExp = ExpandEnvironmentStringsW(buf, exp, 1024);
    if (nExp > 0 && nExp < 1024) {
        StringCchCopyW(buf, 1024, exp);
        trim_ws_inplace(buf);
    }

    // If it looks like a path, canonicalize it first.
    const bool looks_path = (wcschr(buf, L'\\') || wcschr(buf, L'/') || wcschr(buf, L':'));
    if (looks_path) {
        wchar_t full[MAX_PATH];
        DWORD nFull = GetFullPathNameW(buf, MAX_PATH, full, NULL);
        if (nFull > 0 && nFull < MAX_PATH) {
            if (!ends_with_i(full, L".exe")) {
                // Avoid overflow; MAX_PATH buffer.
                if (wcslen(full) + 4 < MAX_PATH) StringCchCatW(full, MAX_PATH, L".exe");
            }
            if (PathFileExistsW(full)) {
                StringCchCopyW(out, cch_out, full);
                return;
            }
        }
        // If it was a path and doesn't exist, fall through to SearchPath.
    }

    // Try SearchPath on PATH (also handles names without .exe).
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

// ----------------------------
// Profile "Start Items" helpers (shell-open + best-effort close)
// ----------------------------
// ----------------------------
// Forward declarations (helpers implemented later in file)
// ----------------------------
static std::wstring exe_watch_key_lower(const std::wstring& raw);
static bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes);
static bool terminate_all_by_name_lower(const wchar_t* exe_lower, int* terminated, int* failed);
static void format_winerr(DWORD e, wchar_t* out, size_t cch);
static bool query_service_status_fast(const wchar_t* svc_name, wchar_t* out_status, size_t cch);
static bool stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch, DWORD* out_err);
static bool start_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch);
static bool start_exe_and_wait(const wchar_t* exe_lower_name, const wchar_t* launch_path_opt, int wait_ms, wchar_t* out_result, size_t cch);

// ----------------------------
// Left info pane (service description)
// ----------------------------
static std::wstring query_service_description_best_effort(const std::wstring& svc_name, DWORD& out_err) {
    out_err = 0;
    std::wstring out;

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { out_err = GetLastError(); return out; }

    SC_HANDLE svc = OpenServiceW(scm, svc_name.c_str(), SERVICE_QUERY_CONFIG);
    if (!svc) { out_err = GetLastError(); CloseServiceHandle(scm); return out; }

    DWORD need = 0;
    QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, NULL, 0, &need);
    DWORD err = GetLastError();
    if (need == 0) {
        out_err = (err ? err : ERROR_INVALID_DATA);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return out;
    }

    std::vector<BYTE> buf(need);
    if (!QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, buf.data(), (DWORD)buf.size(), &need)) {
        out_err = GetLastError();
    } else {
        SERVICE_DESCRIPTIONW* sd = (SERVICE_DESCRIPTIONW*)buf.data();
        if (sd && sd->lpDescription) out = sd->lpDescription;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return out;
}

static void update_left_info_pane(App& self, ItemKind kind, Item* it) {
    if (kind == ItemKind::Svc) {
        if (!self.svc_info_edit) return;
        if (!it) { SetWindowTextW(self.svc_info_edit, L""); return; }

        if (!it->svc_desc_loaded) {
            DWORD err = 0;
            std::wstring desc = query_service_description_best_effort(it->name, err);
            it->svc_desc = desc;
            it->svc_desc_last_err = err;
            it->svc_desc_loaded = true;
        }

        if (!it->svc_desc.empty()) {
            SetWindowTextW(self.svc_info_edit, it->svc_desc.c_str());
        } else if (it->svc_desc_last_err) {
            wchar_t buf[256];
            wsprintfW(buf, L"(No description. Query failed: winerror %lu)", (unsigned long)it->svc_desc_last_err);
            SetWindowTextW(self.svc_info_edit, buf);
        } else {
            SetWindowTextW(self.svc_info_edit, L"(No description)");
        }
    } else if (kind == ItemKind::Exe) {
        if (!self.exe_info_edit) return;
        if (!it) { SetWindowTextW(self.exe_info_edit, L""); return; }

        std::wstring info;
        if (!it->exe_path.empty()) {
            info = L"Path:\r\n" + it->exe_path + L"\r\n";
        }
        info += L"\r\nName:\r\n" + it->name + L"\r\n";
        SetWindowTextW(self.exe_info_edit, info.c_str());
    }
}


// ==================================================
// Theme module
// ==================================================
// Theme forward declarations (ordering-safe)
static void darkmode_allow_window(HWND hwnd, bool allow_dark);
static bool try_set_explorer_theme_mode(HWND hwnd, bool dark);
static LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static BOOL CALLBACK enum_theme_children_proc(HWND hwnd, LPARAM lp);
static BOOL CALLBACK enum_theme_thread_windows_proc(HWND hwnd, LPARAM lp);
static void theme_compute(App& self);

static LRESULT theme_customdraw_tab(App& self, NMCUSTOMDRAW* cd) {
    if (!self.dark_mode || !cd) return CDRF_DODEFAULT;

    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        // Paint the tab background area once.
        if (cd->hdc) FillRect(cd->hdc, &cd->rc, self.br_panel);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        // Fully owner-draw each tab item so it can't stay light.
        HDC hdc = cd->hdc;
        if (!hdc) return CDRF_DODEFAULT;

        // Fill item background
        FillRect(hdc, &cd->rc, self.br_panel);

        // Fetch tab text
        wchar_t txt[256]{0};
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = txt;
        it.cchTextMax = 255;
        int idx = (int)cd->dwItemSpec;
        TabCtrl_GetItem((HWND)cd->hdr.hwndFrom, idx, &it);

        // Draw text
        SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, self.col_text);

    HFONT hfont = (HFONT)SendMessageW((HWND)cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
    HGDIOBJ oldf = NULL;
    if (hfont) oldf = SelectObject(hdc, hfont);
        RECT tr = cd->rc;
        InflateRect(&tr, -6, -2);
        DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldf) SelectObject(hdc, oldf);

        // Optional focus rectangle
        if (cd->uItemState & CDIS_FOCUS) {
            RECT fr = cd->rc;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(hdc, &fr);
        }
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static LRESULT theme_customdraw_header(App& self, NMCUSTOMDRAW* cd) {
    if (!self.dark_mode || !cd) return CDRF_DODEFAULT;

    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        if (cd->hdc) FillRect(cd->hdc, &cd->rc, self.br_panel);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        HDC hdc = cd->hdc;
        if (!hdc) return CDRF_DODEFAULT;

        // Paint header section background
        FillRect(hdc, &cd->rc, self.br_panel);

        // Get header item text
        wchar_t txt[256]{0};
        HDITEMW hi{};
        hi.mask = HDI_TEXT;
        hi.pszText = txt;
        hi.cchTextMax = 255;
        int i = (int)cd->dwItemSpec;
        Header_GetItem((HWND)cd->hdr.hwndFrom, i, &hi);

        // Draw text
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, self.col_text);
        RECT tr = cd->rc;
        InflateRect(&tr, -6, -2);
        DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static LRESULT theme_customdraw_listview(App& self, NMLVCUSTOMDRAW* lcd) {
    if (!self.dark_mode || !lcd) return CDRF_DODEFAULT;

    // Convenience
    const DWORD stage = lcd->nmcd.dwDrawStage;

    switch (stage) {
    case CDDS_PREPAINT:
        // We want per-item + per-subitem callbacks.
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        // Set default dark colors
        COLORREF text = self.col_text;
        COLORREF bk   = self.col_edit_bg;

        // Selected row styling (keep it subtle)
        const bool selected = (lcd->nmcd.uItemState & CDIS_SELECTED) != 0;
        const bool hot      = (lcd->nmcd.uItemState & CDIS_HOT) != 0;
        if (selected) {
            bk = RGB(70, 70, 70);
        } else if (hot) {
            bk = RGB(55, 55, 55);
        }

        lcd->clrText   = text;
        lcd->clrTextBk = bk;

        // Ensure the background is actually painted (some builds won't fill the row)
        HDC hdc = lcd->nmcd.hdc;
        if (hdc) {
            RECT rc = lcd->nmcd.rc;
            HBRUSH br = CreateSolidBrush(bk);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, text);
        }

        return CDRF_NEWFONT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static void theme_release_brushes(App& self) {
    if (!self.theme_owns_brushes) return;
    if (self.br_bg) { DeleteObject(self.br_bg); self.br_bg = NULL; }
    if (self.br_panel) { DeleteObject(self.br_panel); self.br_panel = NULL; }
    if (self.br_edit) { DeleteObject(self.br_edit); self.br_edit = NULL; }
    if (self.br_btn) { DeleteObject(self.br_btn); self.br_btn = NULL; }
    self.theme_owns_brushes = false;
}

static void theme_update_resources(App& self) {
    theme_release_brushes(self);

    if (self.dark_mode) {
        self.col_bg = RGB(32, 32, 32);
        self.col_panel = RGB(38, 38, 38);
        self.col_edit_bg = RGB(45, 45, 45);
        self.col_text = RGB(235, 235, 235);
        self.col_text_dim = RGB(180, 180, 180);

        self.br_bg = CreateSolidBrush(self.col_bg);
        self.br_panel = CreateSolidBrush(self.col_panel);
        self.br_edit = CreateSolidBrush(self.col_edit_bg);
        self.br_btn = CreateSolidBrush(self.col_panel);
        self.theme_owns_brushes = true;
    } else {
        self.col_bg = GetSysColor(COLOR_BTNFACE);
        self.col_panel = GetSysColor(COLOR_BTNFACE);
        self.col_edit_bg = GetSysColor(COLOR_WINDOW);
        self.col_text = GetSysColor(COLOR_WINDOWTEXT);
        self.col_text_dim = GetSysColor(COLOR_GRAYTEXT);

        self.br_bg = GetSysColorBrush(COLOR_BTNFACE);
        self.br_panel = GetSysColorBrush(COLOR_BTNFACE);
        self.br_edit = GetSysColorBrush(COLOR_WINDOW);
        self.br_btn = GetSysColorBrush(COLOR_BTNFACE);
        self.theme_owns_brushes = false;
    }
}

static void theme_release_resources(App& self) {
    // Currently we only own brushes for custom dark/light colors.
    // Keep this separate from theme_update_resources() so WM_DESTROY can cleanly release them.
    theme_release_brushes(self);
}

static HBRUSH theme_handle_ctlcolor(App& self, UINT msg, WPARAM wParam, LPARAM lParam) {
    HDC hdc = (HDC)wParam;
    HWND ctl = (HWND)lParam;
    if (!hdc) return nullptr;

    COLORREF text = self.col_text;
    if (ctl && !IsWindowEnabled(ctl)) text = self.col_text_dim;
    SetTextColor(hdc, text);

    switch (msg) {
    case WM_CTLCOLORSTATIC:
        // Transparent labels (let parent paint), but keep consistent text color.
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, self.col_bg);
        return self.br_bg;
    case WM_CTLCOLOREDIT:
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, self.col_edit_bg);
        return self.br_edit;
    case WM_CTLCOLORLISTBOX:
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, self.col_edit_bg);
        return self.br_edit;
    case WM_CTLCOLORBTN:
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, self.col_panel);
        return self.br_panel;
    }
    return nullptr;
}

static bool theme_draw_owner_button(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON) return false;
    if (!self.dark_mode) return false;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;

    // Background
    HBRUSH br = CreateSolidBrush(pressed ? RGB(55,55,55) : self.col_panel);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    // Border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70,70,70));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);

    // Text
    wchar_t txt[256]{0};
    GetWindowTextW(dis->hwndItem, txt, 255);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? self.col_text_dim : self.col_text);

    HFONT hfont = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldf = NULL;
    if (hfont) oldf = SelectObject(hdc, hfont);

    RECT tr = rc;
    if (pressed) { tr.left += 1; tr.top += 1; }
    DrawTextW(hdc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (oldf) SelectObject(hdc, oldf);

    // Focus rect
    if ((dis->itemState & ODS_FOCUS) && !disabled) {
        RECT fr = rc;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(hdc, &fr);
    }
    return true;
}

static bool theme_draw_owner_tab(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_TAB) return false;
    if (!self.dark_mode) return false;

    HWND htab = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    // Background for the tab item.
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH br = CreateSolidBrush(selected ? RGB(55,55,55) : self.col_panel);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    // Border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70,70,70));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);

    // Text
    wchar_t txt[256]{0};
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    it.pszText = txt;
    it.cchTextMax = 255;
    int idx = (int)dis->itemID;
    TabCtrl_GetItem(htab, idx, &it);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, self.col_text);

    RECT tr = rc;
    InflateRect(&tr, -8, -3);
    DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    return true;
}

static bool theme_draw_owner_menu(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return false;
    if (!self.dark_mode) return false;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH br = CreateSolidBrush(selected ? RGB(55,55,55) : self.col_bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    // Text from the menu handle if available.
    wchar_t txt[256]{0};
    if (dis->hwndItem) {
        GetMenuStringW((HMENU)dis->hwndItem, dis->itemID, txt, 255, MF_BYCOMMAND);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? self.col_text_dim : self.col_text);

    RECT tr = rc;
    InflateRect(&tr, -8, 0);
    DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    return true;
}

static void theme_menu_set_owner_draw(HMENU menu, bool enable) {
    if (!menu) return;
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mi)) continue;

        if (mi.hSubMenu) {
            theme_menu_set_owner_draw(mi.hSubMenu, enable);
        }
        if (mi.fType & MFT_SEPARATOR) continue;

        mi.fMask = MIIM_FTYPE;
        if (enable) mi.fType |= MFT_OWNERDRAW;
        else        mi.fType &= ~MFT_OWNERDRAW;
        SetMenuItemInfoW(menu, i, TRUE, &mi);
    }
}

static void theme_apply_to_control(App& self, HWND child) {
    if (!child) return;
    darkmode_allow_window(child, self.dark_mode);
    wchar_t cls[64]{0};
    GetClassNameW(child, cls, 63);

    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
        try_set_explorer_theme_mode(child, self.dark_mode);
        // ListView colors
        ListView_SetBkColor(child, self.dark_mode ? self.col_edit_bg : GetSysColor(COLOR_WINDOW));
        ListView_SetTextBkColor(child, self.dark_mode ? self.col_edit_bg : GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(child, self.dark_mode ? self.col_text : GetSysColor(COLOR_WINDOWTEXT));

        // Header often ignores listview theming unless explicitly themed.
        HWND hdr = ListView_GetHeader(child);
        if (hdr) {
            try_set_explorer_theme_mode(hdr, self.dark_mode);
            InvalidateRect(hdr, NULL, TRUE);
        }

        InvalidateRect(child, NULL, TRUE);
        return;
    }
    if (lstrcmpiW(cls, WC_TABCONTROLW) == 0) {
        // Force owner-draw tabs in dark mode so they reliably repaint dark on all systems.
        LONG_PTR st = GetWindowLongPtrW(child, GWL_STYLE);
        if (self.dark_mode) st |= TCS_OWNERDRAWFIXED;
        else st &= ~((LONG_PTR)TCS_OWNERDRAWFIXED);
        SetWindowLongPtrW(child, GWL_STYLE, st);
        SetWindowPos(child, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        try_set_explorer_theme_mode(child, self.dark_mode);
        InvalidateRect(child, NULL, TRUE);
        return;
    }
    if (lstrcmpiW(cls, L"Button") == 0) {
        // Push buttons: subclass paint in dark mode so we don't rely on BS_OWNERDRAW + parent WM_DRAWITEM.
        LONG_PTR st = GetWindowLongPtrW(child, GWL_STYLE);

        const LONG_PTR type = (st & BS_TYPEMASK);
        const bool is_push = (type == BS_PUSHBUTTON) || (type == BS_DEFPUSHBUTTON);
        const bool is_checkish =
            (type == BS_AUTOCHECKBOX) || (type == BS_CHECKBOX) ||
            (type == BS_AUTORADIOBUTTON) || (type == BS_RADIOBUTTON) ||
            (type == BS_GROUPBOX);

        if (is_push && !is_checkish) {
            // Ensure owner-draw is OFF (we paint via subclass instead).
            st &= ~((LONG_PTR)BS_OWNERDRAW);
            SetWindowLongPtrW(child, GWL_STYLE, st);

            if (self.dark_mode) {
                SetWindowSubclass(child, DarkButtonSubclassProc, 1, (DWORD_PTR)&self);
            } else {
                RemoveWindowSubclass(child, DarkButtonSubclassProc, 1);
            }

            SetWindowPos(child, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }

        // Theme hint still helps non-push button variants a bit.
        try_set_explorer_theme_mode(child, self.dark_mode);

        InvalidateRect(child, NULL, TRUE);
        return;
    }
    if (lstrcmpiW(cls, WC_HEADER) == 0) {
        try_set_explorer_theme_mode(child, self.dark_mode);
        InvalidateRect(child, NULL, TRUE);
        return;
    }

    if (lstrcmpiW(cls, STATUSCLASSNAMEW) == 0) {
        SendMessageW(child, SB_SETBKCOLOR, 0, (LPARAM)(self.dark_mode ? self.col_panel : CLR_DEFAULT));
        InvalidateRect(child, NULL, TRUE);
        return;
    }
    if (_wcsnicmp(cls, L"RICHEDIT", 7) == 0) {
#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#endif
        SendMessageW(child, EM_SETBKGNDCOLOR, 0, (LPARAM)(self.dark_mode ? self.col_edit_bg : GetSysColor(COLOR_WINDOW)));
        InvalidateRect(child, NULL, TRUE);
        return;
    }
    if (lstrcmpiW(cls, L"EDIT") == 0) {
        // Standard EDIT controls are themed via WM_CTLCOLOREDIT; just force a repaint.
        InvalidateRect(child, NULL, TRUE);
        return;
    }
// Default: let WM_CTLCOLOR handle drawing.
    InvalidateRect(child, NULL, TRUE);
}

static void theme_apply_to_window(App& self, HWND hwnd) {
    if (!hwnd) return;

    // Reduce flicker while we retheme.
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);

    darkmode_allow_window(hwnd, self.dark_mode);
    try_enable_dark_titlebar(hwnd, self.dark_mode);
    EnumChildWindows(hwnd, enum_theme_children_proc, (LPARAM)&self);

    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static HFONT theme_find_font_cached(App& self, UINT dpi) {
    if (!dpi) dpi = 96;
    if (self.ui_font && self.theme_built_dpi == dpi) return self.ui_font;
    for (auto& kv : self.ui_font_cache) {
        if (kv.first == dpi) return kv.second;
    }
    return nullptr;
}

static HFONT theme_ensure_font_for_dpi(App& self, UINT dpi) {
    if (!dpi) dpi = 96;
    // Main font is stored in self.ui_font.
    if (self.ui_font && self.theme_built_dpi == dpi) return self.ui_font;

    if (HFONT f = theme_find_font_cached(self, dpi)) return f;

    HFONT fnew = create_ui_font_for_dpi(dpi);
    if (fnew) self.ui_font_cache.push_back({ dpi, fnew });
    return fnew;
}

static void theme_compute_for_window(App& self, HWND hwnd) {
    // Ensure base theme resources are built (idempotent) and that we have a font
    // for this window's DPI (dialogs can be moved across monitors).
    theme_compute(self);
    UINT dpi = get_dpi_for_hwnd(hwnd);
    (void)theme_ensure_font_for_dpi(self, dpi);
}

static void theme_compute(App& self) {
    // Rebuild theme resources (colors/brushes) and DPI-dependent resources (font/imagelists)
    // in a single place. This function is idempotent: calling it twice with the same
    // (dpi,dark_mode) inputs produces no resource churn.
    const UINT dpi_now = self.dpi ? self.dpi : 96;
    const bool dark_now = self.dark_mode;

    const bool need_brushes = (!self.theme_built_dpi) || (self.theme_built_dark != dark_now);
    const bool need_dpi_resources = (!self.theme_built_dpi) || (self.theme_built_dpi != dpi_now);

    bool changed = false;

    if (need_brushes) {
        theme_update_resources(self);
        changed = true;
    }

    if (need_dpi_resources) {
        // DPI changed: drop any cached per-dialog fonts so we can rebuild lazily.
        for (auto& kv : self.ui_font_cache) { if (kv.second) DeleteObject(kv.second); }
        self.ui_font_cache.clear();

        // UI font (DPI dependent) - create here, apply in theme_apply_all_controls()
        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
            self.ui_font = NULL;
        }
        self.ui_font = create_ui_font_for_dpi(dpi_now);
        changed = true;

        // ListView imagelists (DPI dependent) - create here, attach in theme_apply_all_controls()
        int iconPx = dpi_scale(dpi_now, 16);
        if (iconPx < 12) iconPx = 12;

        if (self.il_svc) { ImageList_Destroy(self.il_svc); self.il_svc = NULL; }
        if (self.il_exe) { ImageList_Destroy(self.il_exe); self.il_exe = NULL; }

        self.il_svc = ImageList_Create(iconPx, iconPx, ILC_COLOR32 | ILC_MASK, 4, 4);
        self.il_exe = ImageList_Create(iconPx, iconPx, ILC_COLOR32 | ILC_MASK, 4, 4);

        if (self.il_svc && self.il_exe) {
            HICON hSvc = (HICON)LoadImageW(NULL, IDI_INFORMATION, IMAGE_ICON, iconPx, iconPx, LR_SHARED);
            HICON hExe = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, iconPx, iconPx, LR_SHARED);

            if (hSvc) ImageList_AddIcon(self.il_svc, hSvc);
            if (hExe) ImageList_AddIcon(self.il_svc, hExe);

            if (hSvc) ImageList_AddIcon(self.il_exe, hSvc);
            if (hExe) ImageList_AddIcon(self.il_exe, hExe);
        }

        changed = true;
    }

    if (changed) {
        self.theme_built_dpi = dpi_now;
        self.theme_built_dark = dark_now;
        self.theme_gen.fetch_add(1, std::memory_order_relaxed);
    }
}

static void theme_apply_all_controls(App& self) {
    // Apply theming to existing windows/controls. This should not allocate heavy resources.
    // Calling it multiple times is safe (idempotent).
    // All UI windows live on the main thread; retheme them all.
    EnumThreadWindows(GetCurrentThreadId(), enum_theme_thread_windows_proc, (LPARAM)&self);

    // Apply font to the main window subtree if we have one.
    if (self.hwnd && self.ui_font) {
        apply_font_recursive(self.hwnd, self.ui_font);
    }

    // Attach imagelists to listviews (if built)
    if (self.lv_svc && self.il_svc) ListView_SetImageList(self.lv_svc, self.il_svc, LVSIL_SMALL);
    if (self.lv_exe && self.il_exe) ListView_SetImageList(self.lv_exe, self.il_exe, LVSIL_SMALL);

    // Keep menubar consistent (owner-draw only in dark mode).
    if (self.hwnd) {
        HMENU mb = GetMenu(self.hwnd);
        if (mb) { theme_menu_set_owner_draw(mb, self.dark_mode); DrawMenuBar(self.hwnd); }
    }
}


// ==================================================
// Tray module
// ==================================================
// Tray forward declarations
static HICON load_app_icon(HINSTANCE hInst, int cx, int cy);

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

static void tray_show_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOWHIDE, L"Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Quit");

    // Required so the menu dismisses correctly
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}


// ==================================================
// Config module
// ==================================================
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
//   STARTITEM|C:\\Path\\discord.exe
//   STARTITEM|C:\\Path\\SomeDocument.pdf
//   STARTEXE|discord.exe|C:\\Path\\discord.exe   (legacy; treated as STARTITEM too)
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
    ModelLockGuard lk(self);
    self.profiles.clear();
    self.active_profile = -1;
    self.have_default_cfg = false;
    self.profile_watch_keys_sp = std::make_shared<std::vector<std::vector<std::wstring>>>();
    self.profile_watch_hashes_sp = std::make_shared<std::vector<std::vector<uint64_t>>>();
}

bool in_profile = false;
ProfileSnapshot cur_profile;
auto commit_profile = [&]() {
    if (!in_profile) return;
    // Ensure watch exes are unique-ish (best-effort)
    std::sort(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end());
    cur_profile.watch_exes.erase(std::unique(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end()),
                                 cur_profile.watch_exes.end());

    cur_profile.watch_keys_lower = cur_profile.watch_exes; // cache normalized watch keys

    {
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
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


// STARTITEM lines: targets to shell-open on profile activation
const wchar_t* STARTITEMP = L"STARTITEM|";
if (_wcsnicmp(line, STARTITEMP, wcslen(STARTITEMP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(STARTITEMP);
        trim_ws_inplace(p);
        std::wstring target = normalize_start_target_best_effort(p);
        if (!target.empty()) {
            bool dup = false;
            for (const auto& r : cur_profile.start_items) {
                if (_wcsicmp(r.target.c_str(), target.c_str()) == 0) { dup = true; break; }
            }
            if (!dup) {
                StartItem si; si.target = target;
                cur_profile.start_items.push_back(std::move(si));
            }
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

// STARTEXE lines (legacy): treated as STARTITEM too (and we keep the old convenience of ensuring the EXE exists in the profile EXE list).
const wchar_t* STARTEXEP = L"STARTEXE|";
if (_wcsnicmp(line, STARTEXEP, wcslen(STARTEXEP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(STARTEXEP);
        trim_ws_inplace(p);

        // Optional legacy format: STARTEXE|name_or_path|fullpath
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

        const wchar_t* best = exe_path_raw2[0] ? exe_path_raw2 : name_raw;
        std::wstring target = normalize_start_target_best_effort(best);
        if (!target.empty()) {
            bool dup = false;
            for (const auto& r : cur_profile.start_items) {
                if (_wcsicmp(r.target.c_str(), target.c_str()) == 0) { dup = true; break; }
            }
            if (!dup) {
                StartItem si; si.target = target;
                cur_profile.start_items.push_back(std::move(si));
            }
        }

        // Old convenience: also ensure it's present in the profile EXE list (Auto-stop OFF by default) so it's visible/managed.
        wchar_t norm[512]; norm[0] = 0;
        normalize_exe_input(best, norm, 512);
        if (norm[0]) {
            wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
            resolve_exe_launch_path(best, exe_path_full, MAX_PATH);

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
            ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
        // Centralized registration keeps uid_map/by_name/dirty/view caches consistent.
        bool ok = register_item_locked(self, kind_from_int(kind), it);
        if (ok) {
            self.items_gen[kind]++;
        } else {
            // duplicate (or OOM) - keep model consistent
            delete it;
            it = nullptr;
        }
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
    ModelLockGuard lk(self);
    snapshot_from_runtime_locked(self, self.default_cfg);
    self.have_default_cfg = true;
    self.active_profile = -1;
    rebuild_profile_watch_keys_locked(self);
}

    free(wtxt);

    if (restored > 0 || settings_loaded || profiles_loaded) request_save_debounced(self);

    // Restart UI timer with any loaded ui_refresh_ms
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}

static void save_config_now(App& self) {
    wchar_t cfg_path[MAX_PATH] = {0};

    ConfigSnapshot def;
    std::vector<ProfileSnapshot> profs;

    // Snapshot active runtime back into its owning snapshot, then copy out everything needed for saving.
    {
        ModelLockGuard lk(self);

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
    
    // Persist UI geometry
    int wx = self.win_x, wy = self.win_y, ww = self.win_w, wh = self.win_h;
    int wmx = self.win_maximized ? 1 : 0;
    {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (self.hwnd && GetWindowPlacement(self.hwnd, &wp)) {
            RECT r = wp.rcNormalPosition;
            int tw = (int)(r.right - r.left);
            int th = (int)(r.bottom - r.top);
            if (tw >= 300 && th >= 200) { wx = (int)r.left; wy = (int)r.top; ww = tw; wh = th; }
            wmx = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;
        } else if (self.have_last_wp) {
            RECT r = self.last_wp.rcNormalPosition;
            int tw = (int)(r.right - r.left);
            int th = (int)(r.bottom - r.top);
            if (tw >= 300 && th >= 200) { wx = (int)r.left; wy = (int)r.top; ww = tw; wh = th; }
        }
    }
    int apw = self.activity_panel_w;
    int lsy_svc = self.left_split_y_svc;
    int lsy_exe = self.left_split_y_exe;

int n = snprintf(header, sizeof(header),
        "SETTING|ui_refresh_ms|%d\n"
        "SETTING|monitor_interval_ms|%d\n"
        "SETTING|autostop_cooldown_ms|%d\n"
        "SETTING|stop_wait_ms|%d\n"
        "SETTING|tray_enabled|%d\n"
        "SETTING|close_to_tray|%d\n"
        "SETTING|dark_mode|%d\n"
"SETTING|win_rect|%d,%d,%d,%d,%d\n"
"SETTING|activity_panel_w|%d\n"
"SETTING|left_split_y_svc|%d\n"
"SETTING|left_split_y_exe|%d\n"
"SETTING|svc_colw|%d,%d,%d,%d\n"
"SETTING|exe_colw|%d,%d,%d,%d\n"
"SETTING|svc_colorder|%d,%d,%d,%d\n"
"SETTING|exe_colorder|%d,%d,%d,%d\n"
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
        def.tray_enabled ? 1 : 0, def.close_to_tray ? 1 : 0,
        self.dark_mode ? 1 : 0,
        wx, wy, ww, wh, wmx,
        apw,
        lsy_svc,
        lsy_exe,

        self.cols_w[KIND_SVC][0], self.cols_w[KIND_SVC][1], self.cols_w[KIND_SVC][2], self.cols_w[KIND_SVC][3],
        self.cols_w[KIND_EXE][0], self.cols_w[KIND_EXE][1], self.cols_w[KIND_EXE][2], self.cols_w[KIND_EXE][3],
        self.cols_order[KIND_SVC][0], self.cols_order[KIND_SVC][1], self.cols_order[KIND_SVC][2], self.cols_order[KIND_SVC][3],
        self.cols_order[KIND_EXE][0], self.cols_order[KIND_EXE][1], self.cols_order[KIND_EXE][2], self.cols_order[KIND_EXE][3]
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

        // Items/targets to start on profile activation (shell-open; any file type)
        for (const auto& r : p.start_items) {
            if (r.target.empty()) continue;
            StringCchPrintfW(wline, 700, L"STARTITEM|%s\n", r.target.c_str());
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


// ==================================================
// Worker threads module
// ==================================================

// Service stop failures can be transient (e.g., service is in a pending state and can't accept controls yet).
// Treat these as "soft" errors: back off and retry later rather than hammering STOP every tick.
static bool is_service_stop_soft_error(DWORD err) {
    switch (err) {
        case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:   // 1061
        case ERROR_SERVICE_BUSY:                 // 1052
        case ERROR_SERVICE_REQUEST_TIMEOUT:      // 1053
        case ERROR_DEPENDENT_SERVICES_RUNNING:   // 1051
        case ERROR_SHUTDOWN_IN_PROGRESS:         // 1115
            return true;
        default:
            return false;
    }
}

static void action_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    for (;;) {
        Action a;
        {
            if (!action_pop_wait(st, self, a)) break;
        }

        // Profile Start Items (shell-open + best-effort close)
        if (a.op == ACTION_LAUNCH_ITEM) {
            StartedProc sp;
            wchar_t result[256];
            std::wstring target = normalize_start_target_best_effort(a.target.empty() ? a.name : a.target);

            bool ok = shell_open_and_track_process(target.c_str(), sp, result, 256);
            if (ok) {
                ModelLockGuard lk(self);
                self.profile_started_procs.emplace_back(std::move(sp));

                if (!target.empty() && ends_with_i(target.c_str(), L".exe")) {
                    std::wstring key = exe_watch_key_lower(target);
                    if (!key.empty()) self.profile_started_exes.insert(std::move(key));
                }
            }

            wchar_t line[900];
            StringCchPrintfW(line, 900, L"%s: %s → %s",
                             target.c_str(), a.reason.c_str(), result);
            post_log(self, line);
            continue;
        }

        if (a.op == ACTION_CLOSE_STARTED) {
            wchar_t result[256];
            close_process_best_effort(a.pid, a.hproc, a.wait_ms, result, 256);
            wchar_t line[900];
            const wchar_t* label = a.name.empty() ? L"(started item)" : a.name.c_str();
            StringCchPrintfW(line, 900, L"%s: %s → %s",
                             label, a.reason.c_str(), result);
            post_log(self, line);
            continue;
        }

        if (a.kind == KIND_SVC) {
            wchar_t result[256];
            if (a.op == ACTION_START) {
                bool ok_start = start_service_and_wait(a.name.c_str(), a.wait_ms, result, 256);

                // Successful manual/profile starts should clear any previous stop backoff state.
                if (ok_start) {
                    ModelLockGuard lk(self);
                    Item* it = list_find(&self.items[KIND_SVC], a.name.c_str());
                    if (it) {
                        it->svc_stop_fail_streak = 0;
                        it->svc_stop_suppress_until_ms = 0;
                        it->svc_last_stop_err = 0;
                    }
                }
            } else {
                DWORD stop_err = 0;
                bool ok_stop = stop_service_and_wait(a.name.c_str(), a.wait_ms, result, 256, &stop_err);

                // Smart suppression: if a service keeps rejecting stop control (e.g. WinError 1061),
                // back off and eventually disable AUTO-STOP to prevent infinite noisy loops.
                uint32_t streak = 0;
                uint64_t backoff_ms = 0;
                bool disabled_autostop = false;

                {
                    const uint64_t now_ms = now_mono_ms();
                    ModelLockGuard lk(self);
                    Item* it = list_find(&self.items[KIND_SVC], a.name.c_str());
                    if (it) {
                        if (ok_stop) {
                            it->svc_stop_fail_streak = 0;
                            it->svc_stop_suppress_until_ms = 0;
                            it->svc_last_stop_err = 0;
                        } else {
                            it->svc_last_stop_err = stop_err;
                            it->svc_stop_fail_streak++;
                            streak = it->svc_stop_fail_streak;

                            if (is_service_stop_soft_error(stop_err)) {
                                // Exponential backoff: 5s, 10s, 20s... capped, with a floor for 1061.
                                uint32_t shift = (streak > 1) ? std::min<uint32_t>(streak - 1, 7u) : 0u;
                                backoff_ms = 5000ull * (1ull << shift);
                                if (stop_err == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) backoff_ms = std::max<uint64_t>(backoff_ms, 30000ull);
                                backoff_ms = std::min<uint64_t>(backoff_ms, 10ull * 60ull * 1000ull);
                            } else {
                                // Other stop failures: short linear backoff, capped.
                                backoff_ms = std::min<uint64_t>(15000ull * (uint64_t)std::min<uint32_t>(streak, 6u), 2ull * 60ull * 1000ull);
                            }

                            it->svc_stop_suppress_until_ms = now_ms + backoff_ms;

                            // After enough consecutive failures, stop trying automatically until the user re-enables it.
                            if (streak >= 8 && it->auto_stop) {
                                it->auto_stop = false;
                                disabled_autostop = true;
                            }
                        }
                    }
                }

                // Optional informational log on repeated failures (kept outside lock).
                if (!ok_stop && backoff_ms) {
                    wchar_t ebuf[200]; format_winerr(stop_err, ebuf, 200);
                    wchar_t msg[700];
                    if (disabled_autostop) {
                        StringCchPrintfW(msg, 700, L"%s: AUTO-STOP disabled after repeated stop failures (%s).", a.name.c_str(), ebuf);
                    } else {
                        StringCchPrintfW(msg, 700, L"%s: stop failed (%s) — suppressing AUTO-STOP for %llu ms (streak %u).",
                                         a.name.c_str(), ebuf, (unsigned long long)backoff_ms, (unsigned)streak);
                    }
                    post_log(self, msg);
                }
            }

            wchar_t line[700];
            StringCchPrintfW(line, 700, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(self, line);
        } else {
            if (a.op == ACTION_START) {
                std::wstring launch;
                {
                    ModelLockGuard lk(self);
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
    action_clear(self);
}

// ----------------------------
// Monitor-thread support types/helpers (moved earlier for declaration order)
// ----------------------------
// ----------------------------
// Batch status fetch (optimized name matching)
// ----------------------------
typedef struct NameIdx {
    const wchar_t* name;
    size_t idx;
} NameIdx;


static bool binsearch_nameidx(const NameIdx* arr, size_t n, const wchar_t* key, size_t* out_idx) {
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

static bool batch_count_processes_by_name_lower(const NameIdx* idx_sorted, size_t idx_n, int* out_counts, size_t n_total) {
    if (out_counts) {
        for (size_t i = 0; i < n_total; i++) out_counts[i] = 0;
    }
    if (!idx_sorted || idx_n == 0 || !out_counts || n_total == 0) return true;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[260];
            StringCchCopyW(name, 260, pe.szExeFile);
            // binsearch_nameidx is case-insensitive; no need to lowercase per-process.

            size_t orig = 0;
            // binsearch_nameidx compares case-insensitively; idx is already sorted by _wcsicmp.
            if (binsearch_nameidx(idx_sorted, idx_n, name, &orig)) {
                if (orig < n_total) out_counts[orig] += 1;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return true;
}



// Fast-path service status fetch: query ONLY the monitored service set (no full EnumServicesStatusEx).
// Uses cached OpenService handles stored in MonScratch to minimize per-tick overhead.
// NOTE: Service handles can become invalid if the service is deleted/renamed; we reopen on failure.
struct MonScratch; // forward declaration (definition is below)
static bool batch_query_service_states_direct(SC_HANDLE scm_in, MonScratch& sc,
                                            wchar_t* const* svc_names, size_t ns, uint32_t gen_s,
                                            wchar_t out_status[][32]);


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

struct MonSnap {
    size_t cap_s = 0, cap_e = 0;
    size_t ns = 0, ne = 0;
    uint32_t gen_s = 0, gen_e = 0;

    wchar_t** svc_names = nullptr;
    wchar_t** exe_names = nullptr;

    bool* svc_auto = nullptr;
    bool* exe_auto = nullptr;

    uint64_t* svc_lastreq = nullptr;
    uint64_t* exe_lastreq = nullptr;

    // Prebuilt sorted indices for allocation-free per-tick matching.
    // Built only when the corresponding names list changes.
    std::vector<NameIdx> svc_idx_sorted;
    std::vector<NameIdx> exe_idx_sorted;
};

struct MonScratch {
    size_t cap_s = 0, cap_e = 0;
    wchar_t (*svc_status)[32] = nullptr;
    wchar_t (*exe_status)[32] = nullptr;
    int* exe_counts = nullptr;

    // Reused buffers to avoid per-tick allocations.
    std::vector<BYTE> svc_enum_buf;

    // Service status fast-path: cache OpenService handles for monitored services.
    // Rebuilt when the service list generation changes.
    std::vector<unique_sc_handle> svc_query_handles;
    uint32_t svc_handles_gen = 0;
};

// Definition (placed after MonScratch so the type is complete).
static bool batch_query_service_states_direct(SC_HANDLE scm_in, MonScratch& sc,
                                            wchar_t* const* svc_names, size_t ns, uint32_t gen_s,
                                            wchar_t out_status[][32]) {
    if (!out_status || ns == 0) return true;
    for (size_t i = 0; i < ns; i++) StringCchCopyW(out_status[i], 32, L"unknown");

    unique_sc_handle scm_local;
    SC_HANDLE scm = scm_in;
    if (!scm) {
        scm_local.reset(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
        scm = scm_local.get();
    }
    if (!scm) return false;

    // Rebuild cached handles when the monitored list changes.
    if (sc.svc_handles_gen != gen_s || sc.svc_query_handles.size() != ns) {
        sc.svc_query_handles.clear();
        sc.svc_query_handles.resize(ns);
        sc.svc_handles_gen = gen_s;
    }

    for (size_t i = 0; i < ns; i++) {
        const wchar_t* name = svc_names ? svc_names[i] : nullptr;
        if (!name || !name[0]) continue;

        if (!sc.svc_query_handles[i]) {
            sc.svc_query_handles[i].reset(OpenServiceW(scm, name, SERVICE_QUERY_STATUS));
        }

        SERVICE_STATUS_PROCESS ssp{};
        DWORD bytes = 0;
        BOOL ok = FALSE;
        if (sc.svc_query_handles[i]) {
            ok = QueryServiceStatusEx(sc.svc_query_handles[i].get(), SC_STATUS_PROCESS_INFO,
                                     (LPBYTE)&ssp, sizeof(ssp), &bytes);
        }
        if (!ok) {
            // Retry once by reopening the service handle.
            sc.svc_query_handles[i].reset();
            sc.svc_query_handles[i].reset(OpenServiceW(scm, name, SERVICE_QUERY_STATUS));
            if (sc.svc_query_handles[i]) {
                ok = QueryServiceStatusEx(sc.svc_query_handles[i].get(), SC_STATUS_PROCESS_INFO,
                                         (LPBYTE)&ssp, sizeof(ssp), &bytes);
            }
        }

        if (ok) {
            StringCchCopyW(out_status[i], 32, svc_state_to_str(ssp.dwCurrentState));
        }
    }

    return true;
}


static void monsnap_free(MonSnap* s) {
    if (!s) return;
    free_name_array(s->svc_names, s->ns);
    free_name_array(s->exe_names, s->ne);
    free(s->svc_names); free(s->exe_names);
    free(s->svc_auto);  free(s->exe_auto);
    free(s->svc_lastreq); free(s->exe_lastreq);

    s->svc_names = s->exe_names = nullptr;
    s->svc_auto = s->exe_auto = nullptr;
    s->svc_lastreq = s->exe_lastreq = nullptr;
    s->cap_s = s->cap_e = s->ns = s->ne = 0;
    s->gen_s = s->gen_e = 0;
    s->svc_idx_sorted.clear();
    s->exe_idx_sorted.clear();
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
    const DWORD step = 100;
    DWORD slept = 0;
    while (slept < total_ms && !st.stop_requested()) {
        DWORD chunk = (total_ms - slept < step) ? (total_ms - slept) : step;
        Sleep(chunk);
        slept += chunk;
    }
    return st.stop_requested();
}

// Forward decl: combined single-snapshot process enumerator (CPU optimization)
static bool snapshot_processes_build_hashes_and_counts_lower(
    const NameIdx* idx_sorted, size_t idx_n,
    int* out_counts, size_t n_total,
    std::vector<uint64_t>* out_hashes
);
static void monitor_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    MonSnap snap;
    MonScratch sc;

    // Reused per-tick: running process exe hashes (lowercased basename)
    std::vector<uint64_t> running_hashes;
    running_hashes.reserve(512);


    // Reuse SCM handle across ticks (falls back to per-call open if needed).
    unique_sc_handle scm_connect(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));

    for (;;) {
        if (st.stop_requested()) break;


// (profile auto-activation moved below to reuse the single per-tick process snapshot)

        int monitor_interval_ms = 1000;
        int cooldown_ms = 0;
        int stop_wait_ms = 0;

        int cur_profile = -1;
        std::shared_ptr<const std::vector<std::vector<uint64_t>>> watches_sp;
        bool watches_needed = false;
        bool any_autostop = false;


        size_t ns = 0, ne = 0;
        uint32_t gen_s = 0, gen_e = 0;

        bool ok_alloc = false;

        // Snapshot pointers + cheap fields under lock.
        {
            ModelLockGuard lk(self);

            monitor_interval_ms = self.monitor_interval_ms;
            cooldown_ms = self.autostop_cooldown_ms;
            stop_wait_ms = self.stop_wait_ms;

            cur_profile = self.active_profile;
            watches_sp = self.profile_watch_hashes_sp;
            if (watches_sp) {
                const auto& watches = *watches_sp;
                for (const auto& ws : watches) { if (!ws.empty()) { watches_needed = true; break; } }
            }

            // Any auto-stop enabled? (used for adaptive tick when idle)
            for (const auto& it : self.items[KIND_SVC].v) { if (it->auto_stop) { any_autostop = true; break; } }
            if (!any_autostop) {
                for (const auto& it : self.items[KIND_EXE].v) { if (it->auto_stop) { any_autostop = true; break; } }
            }

            if (!any_autostop && !watches_needed) {
                // Back off a bit when we're just passively monitoring (less CPU when idle).
                monitor_interval_ms = std::max(monitor_interval_ms, 2500);
            }

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

                    // Rebuild sorted index for fast matching (only when the list changed).
                    snap.svc_idx_sorted.clear();
                    snap.svc_idx_sorted.reserve(ns);
                    for (size_t i = 0; i < ns; i++) {
                        if (snap.svc_names[i] && snap.svc_names[i][0]) snap.svc_idx_sorted.push_back(NameIdx{ snap.svc_names[i], i });
                    }
                    std::sort(snap.svc_idx_sorted.begin(), snap.svc_idx_sorted.end(),
                              [](const NameIdx& a, const NameIdx& b) { return _wcsicmp(a.name, b.name) < 0; });

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

                    // Rebuild sorted index for fast matching (only when the list changed).
                    snap.exe_idx_sorted.clear();
                    snap.exe_idx_sorted.reserve(ne);
                    for (size_t i = 0; i < ne; i++) {
                        if (snap.exe_names[i] && snap.exe_names[i][0]) snap.exe_idx_sorted.push_back(NameIdx{ snap.exe_names[i], i });
                    }
                    std::sort(snap.exe_idx_sorted.begin(), snap.exe_idx_sorted.end(),
                              [](const NameIdx& a, const NameIdx& b) { return _wcsicmp(a.name, b.name) < 0; });

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

            bool ok = batch_query_service_states_direct(scm_connect.get(), sc, snap.svc_names, ns, gen_s, sc.svc_status);
            if (!ok) post_log(self, L"Monitor: batch service enum failed");

            std::vector<size_t> svc_enforce_idx;
            svc_enforce_idx.reserve(ns);

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
                                                        svc_enforce_idx.push_back(i);
                            snap.svc_lastreq[i] = mono_now;

                        }
                    }
                }
            }

            if (!svc_enforce_idx.empty()) {
                std::vector<int> svc_counts;
                svc_counts.reserve(svc_enforce_idx.size());

                {
                    ModelLockGuard lk(self);
                    for (size_t j = 0; j < svc_enforce_idx.size(); ++j) {
                        const size_t idx = svc_enforce_idx[j];
                        Item* it = nullptr;
                        if (idx < self.items[KIND_SVC].v.size()) {
                            Item* cand = self.items[KIND_SVC].v[idx].get();
                            if (cand && snap.svc_names[idx] && _wcsicmp(cand->name.c_str(), snap.svc_names[idx]) == 0) {
                                it = cand;
                            }
                        }
                        if (!it && idx < snap.ns && snap.svc_names[idx]) {
                            // Fallback: list changed since snapshot; find by name (rare)
                            it = list_find(&self.items[KIND_SVC], snap.svc_names[idx]);
                        }
                        if (it) {
                            // If we recently saw repeated stop errors for this service, temporarily suppress AUTO-STOP.
                            if (it->svc_stop_suppress_until_ms && mono_now < it->svc_stop_suppress_until_ms) {
                                svc_counts.push_back(0);
                                continue;
                            }
                            it->last_autostop_mono_ms = mono_now;
                            it->autostop_count++;
                            svc_counts.push_back(it->autostop_count);
                        } else {
                            svc_counts.push_back(0);
                        }
                    }
                }

                for (size_t j = 0; j < svc_enforce_idx.size(); ++j) {
                    const size_t idx = svc_enforce_idx[j];
                    const int c = svc_counts[j];
                    if (c <= 0) continue;
                    if (idx >= ns || !snap.svc_names[idx]) continue;

                    wchar_t msg[600];
                    StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was %s) requested\u2026",
                                     snap.svc_names[idx], c, sc.svc_status[idx]);
                    post_log(self, msg);

                    wchar_t reason[256];
                    StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", c);
                    action_enqueue(self, KIND_SVC, ACTION_STOP, snap.svc_names[idx], reason, stop_wait_ms);
                }
            }

            // Option A: one message per kind per tick
            post_status_bulk(self, KIND_SVC, snap.svc_names, sc.svc_status, ns, wall_now);
        }

        // --- EXEs: batch count processes; 
        // --- Processes: ONE Toolhelp snapshot per tick (optional hashes + exe counts) ---
        {
            const bool need_proc_snapshot = (watches_needed || (ne != 0));
            if (need_proc_snapshot) {
                const NameIdx* idxp = (ne ? snap.exe_idx_sorted.data() : nullptr);
                const size_t idxn = (ne ? snap.exe_idx_sorted.size() : 0);
                int* counts = (ne ? sc.exe_counts : nullptr);
                std::vector<uint64_t>* hashes = (watches_needed ? &running_hashes : nullptr);

                snapshot_processes_build_hashes_and_counts_lower(idxp, idxn, counts, ne, hashes);

                // Profile auto-activation based on watched .exe processes.
                // If multiple profiles match, the first match (config order) wins.
                if (watches_needed && watches_sp && hashes && !hashes->empty()) {
                    int desired = -1;
                    const auto& watches = *watches_sp;
                    for (size_t i = 0; i < watches.size(); i++) {
                        const auto& ws = watches[i];
                        for (auto h : ws) {
                            if (std::binary_search(hashes->begin(), hashes->end(), h)) { desired = (int)i; break; }
                        }
                        if (desired >= 0) break;
                    }
                    if (desired != cur_profile) {
                        PostMessageW(self.hwnd, WM_APP_PROFILE_SWITCH, (WPARAM)desired, 0);
                    }
                }
            } else {
                running_hashes.clear();
                if (sc.exe_counts && ne) memset(sc.exe_counts, 0, ne * sizeof(int));
            }
        }

// derive running/stopped ---
        if (ne && snap.exe_names && sc.exe_counts && sc.exe_status) {
            std::vector<size_t> exe_enforce_idx;
            exe_enforce_idx.reserve(ne);

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
                                                exe_enforce_idx.push_back(i);
                        snap.exe_lastreq[i] = mono_now;

                    }
                }
            }

            if (!exe_enforce_idx.empty()) {
                std::vector<int> exe_counts2;
                exe_counts2.reserve(exe_enforce_idx.size());

                {
                    ModelLockGuard lk(self);
                    for (size_t j = 0; j < exe_enforce_idx.size(); ++j) {
                        const size_t idx = exe_enforce_idx[j];
                        Item* it = nullptr;
                        if (idx < self.items[KIND_EXE].v.size()) {
                            Item* cand = self.items[KIND_EXE].v[idx].get();
                            if (cand && snap.exe_names[idx] && _wcsicmp(cand->name.c_str(), snap.exe_names[idx]) == 0) {
                                it = cand;
                            }
                        }
                        if (!it && idx < snap.ne && snap.exe_names[idx]) {
                            // Fallback: list changed since snapshot; find by name (rare)
                            it = list_find(&self.items[KIND_EXE], snap.exe_names[idx]);
                        }
                        if (it) {
                            it->last_autostop_mono_ms = mono_now;
                            it->autostop_count++;
                            exe_counts2.push_back(it->autostop_count);
                        } else {
                            exe_counts2.push_back(0);
                        }
                    }
                }

                for (size_t j = 0; j < exe_enforce_idx.size(); ++j) {
                    const size_t idx = exe_enforce_idx[j];
                    const int k = exe_counts2[j];
                    if (k <= 0) continue;
                    if (idx >= ne || !snap.exe_names[idx]) continue;

                    wchar_t msg[600];
                    StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was running) requested\u2026",
                                     snap.exe_names[idx], k);
                    post_log(self, msg);

                    wchar_t reason[256];
                    StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", k);
                    action_enqueue(self, KIND_EXE, ACTION_STOP, snap.exe_names[idx], reason, stop_wait_ms);
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


// Rebuild cached WATCH keys for profiles.
// Must be called with self.mtx held.
// Uses shared_ptr so the monitor thread can grab a snapshot cheaply and iterate without allocations.
static inline uint64_t fnv1a64_exe_lower_hash(const wchar_t* s);
static bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes);

static void rebuild_profile_watch_keys_locked(App& self) {
    auto nv = std::make_shared<std::vector<std::vector<std::wstring>>>();
    auto hv = std::make_shared<std::vector<std::vector<uint64_t>>>();
    nv->reserve(self.profiles.size());
    hv->reserve(self.profiles.size());

    for (auto& p : self.profiles) {
        // Keep the cached keys in sync. watch_exes are already normalized lower-case basenames.
        if (p.watch_keys_lower.empty() && !p.watch_exes.empty()) {
            p.watch_keys_lower = p.watch_exes;
        } else if (p.watch_keys_lower.size() != p.watch_exes.size()) {
            // Best-effort resync; avoids stale cache if older configs/UI paths changed.
            p.watch_keys_lower = p.watch_exes;
        }
        nv->push_back(p.watch_keys_lower);

        // Also cache hashes for allocation-free matching in the monitor thread.
        std::vector<uint64_t> hs;
        hs.reserve(p.watch_keys_lower.size());
        for (const auto& k : p.watch_keys_lower) {
            if (k.empty()) continue;
            hs.push_back(fnv1a64_exe_lower_hash(k.c_str())); // already lower-case
        }
        std::sort(hs.begin(), hs.end());
        hs.erase(std::unique(hs.begin(), hs.end()), hs.end());
        hv->push_back(std::move(hs));
    }

    self.profile_watch_keys_sp = std::move(nv);
    self.profile_watch_hashes_sp = std::move(hv);
}


// --------------------------------------------------
// Final-form structure note
// - We removed the global App* and the global app() accessor.
// - Legacy code still uses self.field; we map app() -> `self` via a macro.
//   Any function that uses app() must have an `App& self` in scope.
// - The main window proc is now instance-bound via GWLP_USERDATA.
// --------------------------------------------------
#define app() (self)

// ----------------------------
// Dark mode / theme helpers
// ----------------------------

// ----------------------------
// Dark mode opt-in (UxTheme ordinals)
// ----------------------------
struct DarkModeApi {
    HMODULE ux = NULL;
    // Ordinals commonly used (undocumented; best-effort)
    using PFN_SetPreferredAppMode = int (WINAPI*)(int);
    using PFN_FlushMenuThemes = void (WINAPI*)();
    using PFN_AllowDarkModeForWindow = BOOL (WINAPI*)(HWND, BOOL);

    PFN_SetPreferredAppMode SetPreferredAppMode = nullptr;
    PFN_FlushMenuThemes FlushMenuThemes = nullptr;
    PFN_AllowDarkModeForWindow AllowDarkModeForWindow = nullptr;

    bool inited = false;
};

static DarkModeApi g_dm;
template<typename T>
static T dm_get_ordinal(HMODULE m, int ordinal) {
    if (!m) return nullptr;
    FARPROC fp = GetProcAddress(m, MAKEINTRESOURCEA(ordinal));
    if (!fp) return nullptr;
    // Avoid -Wcast-function-type on GCC/Clang by casting via integer.
    return (T)(uintptr_t)fp;
}


static void darkmode_init_api() {
    if (g_dm.inited) return;
    g_dm.inited = true;
    g_dm.ux = LoadLibraryW(L"uxtheme.dll");
    if (!g_dm.ux) return;

    // These are ordinals used by many Win32 apps. If they don't exist, we simply no-op.
    g_dm.SetPreferredAppMode = dm_get_ordinal<DarkModeApi::PFN_SetPreferredAppMode>(g_dm.ux, 135);
    g_dm.FlushMenuThemes     = dm_get_ordinal<DarkModeApi::PFN_FlushMenuThemes>(g_dm.ux, 136);
    g_dm.AllowDarkModeForWindow = dm_get_ordinal<DarkModeApi::PFN_AllowDarkModeForWindow>(g_dm.ux, 133);
}

static void darkmode_set_app(bool want_dark) {
    darkmode_init_api();
    if (!g_dm.SetPreferredAppMode) return;
    // PreferredAppMode: Default=0, AllowDark=1, ForceDark=2, ForceLight=3
    g_dm.SetPreferredAppMode(want_dark ? 1 : 0);
    if (g_dm.FlushMenuThemes) g_dm.FlushMenuThemes();
}

static void darkmode_allow_window(HWND hwnd, bool allow_dark) {
    if (!hwnd) return;
    darkmode_init_api();
    if (g_dm.AllowDarkModeForWindow) {
        g_dm.AllowDarkModeForWindow(hwnd, allow_dark ? TRUE : FALSE);
    }
}
static bool try_set_explorer_theme_mode(HWND hwnd, bool dark) {
    if (!hwnd) return false;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return false;

    typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    PFN_SetWindowTheme p = nullptr;
    LOAD_PROC(ux, "SetWindowTheme", PFN_SetWindowTheme, p);

    bool ok = false;
    if (p) {
        // "DarkMode_Explorer" is honored by common controls on Win10/11; harmless if ignored.
        p(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ok = true;
    }

    FreeLibrary(ux);
    return ok;
}


// Custom-draw helpers for controls that don't fully honor DarkMode_Explorer on all builds.
// Custom-draw for listviews (items) so text/bk colors are correct in dark mode even when the control ignores LVM_* colors.
// Dark-mode push buttons: subclass paint so we don't rely on BS_OWNERDRAW + parent WM_DRAWITEM.
// This is more reliable because many buttons live inside container child windows (tab pages/panels),
// and WM_DRAWITEM would go to those parents, not the main window.
static LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR /*idSubclass*/, DWORD_PTR refData) {
    App* self = reinterpret_cast<App*>(refData);
    switch (msg) {
    case WM_ERASEBKGND:
        if (self && self->dark_mode) return 1;
        break;
    case WM_ENABLE:
    case WM_THEMECHANGED:
    case WM_STYLECHANGED:
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_PAINT: {
        if (!self || !self->dark_mode) break;

        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{}; GetClientRect(hwnd, &rc);

        const bool disabled = !IsWindowEnabled(hwnd);
        const LRESULT st = SendMessageW(hwnd, BM_GETSTATE, 0, 0);
        const bool pressed = (st & BST_PUSHED) != 0;

        COLORREF bg = pressed ? RGB(55,55,55) : self->col_panel;
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        // Border
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70,70,70));
        HGDIOBJ oldp = SelectObject(hdc, pen);
        HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldb);
        SelectObject(hdc, oldp);
        DeleteObject(pen);

        // Text
        wchar_t txt[256]{0};
        GetWindowTextW(hwnd, txt, 255);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, disabled ? self->col_text_dim : self->col_text);

        // Ensure we draw with the button's assigned font (WM_SETFONT), otherwise GDI may use a default font
        // during subclass painting, causing mismatched text size between light/dark mode.
        HFONT hfont = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
        HGDIOBJ oldf = NULL;
        if (hfont) oldf = SelectObject(hdc, hfont);

        RECT tr = rc;
        if (pressed) { tr.left += 1; tr.top += 1; }
        DrawTextW(hdc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldf) SelectObject(hdc, oldf);

        // Focus rect
        if (GetFocus() == hwnd && !disabled) {
            RECT fr = rc;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(hdc, &fr);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, DarkButtonSubclassProc, 1);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Apply theming (including dark mode opt-in and owner-draw toggles) to a single control.
static BOOL CALLBACK enum_theme_children_proc(HWND child, LPARAM lp) {
    App* self = reinterpret_cast<App*>(lp);
    if (!self) return TRUE;
    theme_apply_to_control(*self, child);
    EnumChildWindows(child, enum_theme_children_proc, lp);
    return TRUE;
}

static bool try_enable_dark_titlebar(HWND hwnd, bool enable);

static BOOL CALLBACK enum_theme_thread_windows_proc(HWND hwnd, LPARAM lp) {
    App* self = reinterpret_cast<App*>(lp);
    if (!self) return TRUE;
    theme_apply_to_window(*self, hwnd);
    return TRUE;
}


// ----------------------------
// Theme + per-window DPI font cache helpers
// ----------------------------
static void theme_compute(App& self);
// Back-compat wrapper (older call sites). Prefer theme_compute()+theme_apply_all_controls().
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

// Forward decls for event-driven UI maintenance (definitions later)
static void ui_timer_suspend(App& self, HWND hwnd);
static void ui_timer_resume(App& self, HWND hwnd);
static void restart_ui_timer(App& self, HWND hwnd);
static void post_model_dirty(App& self);

static void show_main_window(App& self) {
    HWND hwnd = self.hwnd;
    if (!hwnd) return;

    // Restore to the last on-screen position we had before hiding to tray.
    if (self.have_last_wp) {
        WINDOWPLACEMENT wp = self.last_wp;
        wp.length = sizeof(wp);
        // Never re-show as minimized when coming back from tray.
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE || wp.showCmd == SW_SHOWMINNOACTIVE) {
            wp.showCmd = SW_SHOWNORMAL;
        }
        SetWindowPlacement(hwnd, &wp);
    }

    // Ensure visible (close-to-tray uses SW_HIDE)
    ShowWindow(hwnd, SW_SHOW);

    // When restoring from tray/hidden, resume UI maintenance.
    ui_timer_resume(self, hwnd);

    // If minimized, restore. If not, bring to front.
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    // Bring to front (foreground restrictions can otherwise make it look like "nothing happened")
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    restart_ui_timer(self, hwnd);
    post_model_dirty(self);
}

static void hide_main_window(App& self) {
    if (!self.hwnd) return;
    // Capture current placement so we can restore to the same position when showing from tray.
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (GetWindowPlacement(self.hwnd, &wp)) {
        self.last_wp = wp;
        self.have_last_wp = true;
    }
    ui_timer_suspend(self, self.hwnd);
    ShowWindow(self.hwnd, SW_HIDE);
}

static void app_request_exit(App& self, HWND hwnd) {
    self.force_quit = true;
    DestroyWindow(hwnd);
}

// ----------------------------
// FIX: Page container window class that forwards WM_COMMAND / WM_NOTIFY
// ----------------------------
static const wchar_t* PAGE_CLASS = L"PageContainer_C";

static LRESULT CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(page);
    if (msg == WM_COMMAND || msg == WM_NOTIFY ||
        msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORLISTBOX) {
        if (parent) return SendMessageW(parent, msg, wParam, lParam);
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wParam;
        if (hdc) {
            RECT rc; GetClientRect(page, &rc);
            App* self = parent ? reinterpret_cast<App*>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
            HBRUSH br = self ? self->br_bg : GetSysColorBrush(COLOR_BTNFACE);
            FillRect(hdc, &rc, br);
        }
        return 1;
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




// Bulk status update (generation-only, no heap churn):
// - Monitor thread updates Item::last_status/last_update_wall under lock.
// - UI thread repaints only rows that changed (via WM_APP_STATUS_GEN / UI timer).
static void post_status_bulk(App& self, int kind, wchar_t** names, wchar_t (*statuses)[32], size_t n, time_t wall_ts) {
    if (!names || !statuses || n == 0) return;

    bool any_changed = false;

    {
        ModelLockGuard lk(self);
        // Always update global "last seen" so the statusbar can show monitor freshness,
        // but only treat per-row updates as "dirty" when the status text actually changes.
        self.last_any_status_wall = wall_ts;

        // Reserve a bit to reduce reallocs if many rows change at once (rare).
        if (self.dirty_status[kind].capacity() < self.items[kind].v.size())
            self.dirty_status[kind].reserve(self.items[kind].v.size());

        for (size_t i = 0; i < n; i++) {
            if (!names[i] || !names[i][0]) continue;
            Item* it = list_find(&self.items[kind], names[i]);
            if (!it) continue;

            const wchar_t* ns = statuses[i][0] ? statuses[i] : L"unknown";
            if (wcscmp(it->last_status, ns) != 0) {
                // Service flapping detection (restarts within window)
                if (kind == KIND_SVC) {
                    const bool prev_stopped = (_wcsicmp(it->last_status, L"stopped") == 0);
                    const bool now_running  = (_wcsicmp(ns, L"running") == 0);
                    // Reset window if old
                    if (it->flap_window_start_wall == 0 || (wall_ts - it->flap_window_start_wall) > 120) {
                        it->flap_window_start_wall = wall_ts;
                        it->flap_changes = 0;
                        it->flap_restarts = 0;
                        it->flap_flagged = false;
                    }
                    it->flap_changes++;
                    it->flap_last_change_wall = wall_ts;
                    if (prev_stopped && now_running) it->flap_restarts++;
                    if (!it->flap_flagged && it->flap_restarts >= 3) {
                        it->flap_flagged = true;
                        log_linef(self, L"%s: detected frequent restarts (%u in ~2 min)", it->name.c_str(), (unsigned)it->flap_restarts);
                    }
                }

                StringCchCopyW(it->last_status, 32, ns);
                it->last_update_wall = wall_ts; // "Last" column = last change time (not last poll)
                it->status_gen++;
                self.dirty_status[kind].push_back(it);
                any_changed = true;
            }
        }

        if (any_changed) self.status_gen[kind]++;
    }

    // Wake UI to apply only changed rows (no full scan, no string copying).
    if (any_changed) {
        PostMessageW(self.hwnd, WM_APP_STATUS_GEN, (WPARAM)kind, (LPARAM)(INT_PTR)self.status_gen[kind]);
    }
}

static void request_save_debounced(App& self) {
    PostMessageW(self.hwnd, WM_APP_REQUEST_SAVE, 0, 0);
}

// ----------------------------
// Process helpers
// ----------------------------


// ----------------------------
// Profile "Start Items" helpers (shell-open + best-effort close)
// ----------------------------
static std::wstring normalize_start_target_best_effort(const std::wstring& raw) {
    std::wstring s = raw;

    // Trim whitespace
    while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
    if (s.empty()) return s;

    // Strip simple surrounding quotes
    if (s.size() >= 2 && ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\''))) {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
        if (s.empty()) return s;
    }

    // Expand env vars (%TEMP%, etc.) if present
    if (s.find(L'%') != std::wstring::npos) {
        wchar_t exp[2048];
        DWORD got = ExpandEnvironmentStringsW(s.c_str(), exp, 2048);
        if (got > 0 && got < 2048) {
            s.assign(exp);
            while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
            while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
        }
    }

    // If it looks like a filesystem path, canonicalize to full path best-effort.
    // For URLs / shell targets, leave as-is.
    // PathIsURLW covers http/https/ftp/mailto, etc.
    if (!s.empty() && !PathIsURLW(s.c_str())) {
        bool looks_path = (s.find(L'\\') != std::wstring::npos) ||
                          (s.find(L'/')  != std::wstring::npos) ||
                          (s.find(L':')  != std::wstring::npos) ||
                          (s.rfind(L".\\", 0) == 0) ||
                          (s.rfind(L"..\\", 0) == 0) ||
                          (s.rfind(L"./", 0) == 0) ||
                          (s.rfind(L"../", 0) == 0);

        if (looks_path) {
            wchar_t full[MAX_PATH];
            DWORD nFull = GetFullPathNameW(s.c_str(), MAX_PATH, full, NULL);
            if (nFull > 0 && nFull < MAX_PATH) {
                s.assign(full);
            }
        }
    }

    return s;
}

static std::wstring normalize_start_target_best_effort(const wchar_t* raw) {
    if (!raw) return std::wstring();
    return normalize_start_target_best_effort(std::wstring(raw));
}




static std::wstring reg_read_sz_hkcr(const std::wstring& subkey, const wchar_t* value_name) {
    wchar_t buf[2048];
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LONG r = RegGetValueW(HKEY_CLASSES_ROOT, subkey.c_str(), value_name, RRF_RT_REG_SZ, &type, buf, &cb);
    if (r != ERROR_SUCCESS) return std::wstring();
    // Ensure null-terminated
    buf[(sizeof(buf)/sizeof(buf[0])) - 1] = 0;
    return std::wstring(buf);
}

static void wstr_replace_all(std::wstring& s, const wchar_t* from, const std::wstring& to) {
    if (!from || !from[0]) return;
    size_t pos = 0;
    const size_t nfrom = wcslen(from);
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, nfrom, to);
        pos += to.size();
    }
}

static bool create_process_track(const std::wstring& cmdline, const wchar_t* started_target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (cmdline.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty command line.");
        return false;
    }

    std::vector<wchar_t> cl(cmdline.begin(), cmdline.end());
    cl.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(NULL, cl.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        if (out_result && cch) StringCchPrintfW(out_result, cch, L"CreateProcess failed (%lu).", (unsigned long)e);
        return false;
    }

    CloseHandle(pi.hThread);

    out_sp.started_target = started_target ? started_target : L"";
    out_sp.pid = pi.dwProcessId;
    out_sp.hproc.reset(pi.hProcess);
    if (out_result && cch) StringCchPrintfW(out_result, cch, L"Started (pid %lu).", (unsigned long)out_sp.pid);
    return true;
}

static bool start_ahk_via_association_command(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (!target || !target[0]) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty target.");
        return false;
    }

    const wchar_t* ext = PathFindExtensionW(target);
    if (!ext || _wcsicmp(ext, L".ahk") != 0) return false;

    // Get ProgID from HKCR\.ahk (default value)
    std::wstring progid = reg_read_sz_hkcr(L".ahk", NULL);
    if (progid.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"No .ahk association found (install AutoHotkey or associate .ahk).");
        return false;
    }

    // Get open command from HKCR\<ProgID>\shell\open\command
    std::wstring cmdkey = progid + L"\\shell\\open\\command";
    std::wstring cmd = reg_read_sz_hkcr(cmdkey, NULL);
    if (cmd.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Missing association open command for .ahk.");
        return false;
    }

    // Expand env vars in command
    if (cmd.find(L'%') != std::wstring::npos) {
        wchar_t exp[4096];
        DWORD got = ExpandEnvironmentStringsW(cmd.c_str(), exp, 4096);
        if (got > 0 && got < 4096) cmd.assign(exp);
    }

    std::wstring qtarget = L"\"";
    qtarget += target;
    qtarget += L"\"";

    // Replace common placeholders. Do the quoted variants first to avoid double-quotes.
    const std::wstring qt = qtarget;
    wstr_replace_all(cmd, L"\"%1\"", qt);
    wstr_replace_all(cmd, L"\"%L\"", qt);
    wstr_replace_all(cmd, L"\"%l\"", qt);
    wstr_replace_all(cmd, L"%1", qt);
    wstr_replace_all(cmd, L"%L", qt);
    wstr_replace_all(cmd, L"%l", qt);

    // Drop %* (extra args placeholder)
    wstr_replace_all(cmd, L"%*", L"");

    // If no placeholder was present, append the script path
    if (cmd.find(target) == std::wstring::npos && cmd.find(qt) == std::wstring::npos) {
        cmd.push_back(L' ');
        cmd += qt;
    }

    return create_process_track(cmd, target, out_sp, out_result, cch);
}

// If the target is an .exe path, derive the lowercased basename (for fallback stop-by-name).
static bool shell_open_and_track_process(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (!target || !target[0]) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty target.");
        return false;
    }

    // Special-case .ahk: start via file-association command so we reliably get a process handle (for later profile teardown).
    {
        const wchar_t* ext = PathFindExtensionW(target);
        if (ext && _wcsicmp(ext, L".ahk") == 0) {
            StartedProc sp{};
            wchar_t tmp[256];
            tmp[0] = 0;
            if (start_ahk_via_association_command(target, sp, tmp, 256)) {
                out_sp = std::move(sp);
                if (out_result && cch) StringCchCopyW(out_result, cch, tmp);
                return true;
            }
            // If association-start failed, fall through to ShellExecuteEx (it may still work depending on system setup).
            // Keep tmp in case we fail below too.
        }
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = NULL;
    sei.lpVerb = L"open";
    sei.lpFile = target;
    sei.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (out_result && cch) {
            if (e == ERROR_NO_ASSOCIATION) {
                StringCchPrintfW(out_result, cch, L"No association for this file type (error %lu).", (unsigned long)e);
            } else {
                StringCchPrintfW(out_result, cch, L"ShellExecuteEx failed (%lu).", (unsigned long)e);
            }
        }
        return false;
    }

    out_sp.started_target = target;

    if (sei.hProcess) {
        out_sp.hproc.reset(sei.hProcess);
        out_sp.pid = GetProcessId(sei.hProcess);
        if (out_result && cch) {
            if (out_sp.pid) StringCchPrintfW(out_result, cch, L"Started (pid %lu).", (unsigned long)out_sp.pid);
            else StringCchCopyW(out_result, cch, L"Started.");
        }
    } else {
        // Common when opening documents/URLs that reuse an existing app instance.
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Started (no process handle).");
    }

    return true;
}

struct EnumCloseWindowsData {
    DWORD pid = 0;
};

static BOOL CALLBACK enum_close_windows_cb(HWND hwnd, LPARAM lp) {
    EnumCloseWindowsData* d = (EnumCloseWindowsData*)lp;
    if (!d || !d->pid) return TRUE;

    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != d->pid) return TRUE;

    // Skip tooltips/owned popups to reduce noise (still best-effort).
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static void close_process_best_effort(DWORD pid, HANDLE hproc_in, int wait_ms, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;

    unique_handle hproc;
    if (hproc_in && hproc_in != INVALID_HANDLE_VALUE) {
        // Borrow handle: do NOT close (caller owns); duplicate so we can safely manage lifetime.
        HANDLE dup = NULL;
        DuplicateHandle(GetCurrentProcess(), hproc_in, GetCurrentProcess(), &dup,
                        SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, 0);
        if (dup) hproc.reset(dup);
    }

    if (!hproc && pid) {
        HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) hproc.reset(hp);
    }

    if (!hproc) {
        if (out_result && cch) {
            if (pid) StringCchPrintfW(out_result, cch, L"Cannot open process (pid %lu).", (unsigned long)pid);
            else     StringCchCopyW(out_result, cch, L"No process to close.");
        }
        return;
    }

    // If already exited, we're done.
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeProcess(hproc.get(), &code) && code != STILL_ACTIVE) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Already exited.");
        return;
    }

    // Ask nicely: WM_CLOSE to top-level windows for that PID.
    if (!pid) pid = GetProcessId(hproc.get());
    if (pid) {
        EnumCloseWindowsData d{ pid };
        EnumWindows(enum_close_windows_cb, (LPARAM)&d);
    }

    // Wait for graceful exit.
    int ms = wait_ms;
    if (ms < 0) ms = 0;
    DWORD wr = WaitForSingleObject(hproc.get(), (DWORD)ms);

    if (wr == WAIT_OBJECT_0) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Closed.");
        return;
    }

    // Still running: force terminate.
    TerminateProcess(hproc.get(), 1);
    WaitForSingleObject(hproc.get(), 2000);

    if (out_result && cch) StringCchCopyW(out_result, cch, L"Terminated.");
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
            // binsearch_nameidx is case-insensitive; no need to lowercase per-process.
            if (_wcsicmp(name, exe_lower) == 0) count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}


// Build a set of running process exe names (lowercased) from ONE Toolhelp snapshot.

static std::wstring exe_watch_key_lower(const std::wstring& raw) {
    std::wstring s = raw;

    // Trim simple whitespace
    while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
    if (s.empty()) return s;

    // Strip simple quotes
    if (s.size() >= 2 && ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\''))) {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
        if (s.empty()) return s;
    }

    // If a path was provided, use the basename to match Toolhelp's szExeFile.
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);

    // Lower-case (in place) for stable matching
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// Fast, allocation-free hash of a case-insensitive exe basename (typically already basename).
// 64-bit FNV-1a over wchar_t code units (Windows: UTF-16).
static inline uint64_t fnv1a64_exe_lower_hash(const wchar_t* s) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    if (!s) return h;

    for (const wchar_t* p = s; *p; ++p) {
        // Fold to lower without allocating; towlower is fine for ASCII exe names.
        wchar_t c = (wchar_t)towlower(*p);
        h ^= (uint16_t)c;
        h *= FNV_PRIME;
    }
    return h;
}

// Build sorted+unique running exe hashes once per tick (no heap per-process string allocations).
static bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes) {
    out_hashes.clear();

    unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap.get(), &pe)) return false;

    do {
        if (pe.szExeFile[0]) {
            out_hashes.push_back(fnv1a64_exe_lower_hash(pe.szExeFile));
        }
    } while (Process32NextW(snap.get(), &pe));

    std::sort(out_hashes.begin(), out_hashes.end());
    out_hashes.erase(std::unique(out_hashes.begin(), out_hashes.end()), out_hashes.end());
    return true;
}

// One Toolhelp32 process snapshot to optionally produce:
//  - running exe hashes (lowercased basename) for profile watch matching
//  - per-monitored-exe counts (by case-insensitive basename compare)
// This replaces doing multiple full process enumerations per tick.
static bool snapshot_processes_build_hashes_and_counts_lower(
    const NameIdx* idx_sorted, size_t idx_n,
    int* out_counts, size_t n_total,
    std::vector<uint64_t>* out_hashes
) {
    if (out_hashes) out_hashes->clear();
    if (out_counts) {
        for (size_t i = 0; i < n_total; i++) out_counts[i] = 0;
    }

    const bool need_counts = (idx_sorted && idx_n && out_counts && n_total);
    const bool need_hashes = (out_hashes != nullptr);

    if (!need_counts && !need_hashes) return true;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return false; }

    if (out_hashes) out_hashes->reserve(512);

    do {
        if (!pe.szExeFile[0]) continue;

        if (need_hashes) {
            out_hashes->push_back(fnv1a64_exe_lower_hash(pe.szExeFile));
        }

        if (need_counts) {
            size_t orig = 0;
            if (binsearch_nameidx(idx_sorted, idx_n, pe.szExeFile, &orig)) {
                if (orig < n_total) out_counts[orig] += 1;
            }
        }
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);

    if (need_hashes) {
        std::sort(out_hashes->begin(), out_hashes->end());
        out_hashes->erase(std::unique(out_hashes->begin(), out_hashes->end()), out_hashes->end());
    }
    return true;
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



static bool stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch, DWORD* out_err) {
    StringCchCopyW(out_result, cch, L"error");
    if (out_err) *out_err = 0;

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        if (out_err) *out_err = e;
        return false;
    }

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        if (out_err) *out_err = e;
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
        if (out_err) *out_err = e;

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
// ListView helpers
// ----------------------------
static void lv_set_columns(HWND lv, const wchar_t* col0, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3) {
    ListView_DeleteAllItems(lv);
    while (ListView_DeleteColumn(lv, 0)) {}

    // Scale default column widths to the current DPI so the layout feels consistent
    UINT dpi = get_dpi_for_hwnd(lv);
    auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    c.fmt = LVCFMT_CENTER;
    c.pszText = (LPWSTR)col0;
    c.cx = S(90);
    ListView_InsertColumn(lv, 0, &c);

    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)col1;
    c.cx = S(380);
    ListView_InsertColumn(lv, 1, &c);

    c.pszText = (LPWSTR)col2;
    c.cx = S(140);
    ListView_InsertColumn(lv, 2, &c);

    c.pszText = (LPWSTR)col3;
    c.cx = S(220);
    ListView_InsertColumn(lv, 3, &c);

    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
}

static void lv_scale_columns(HWND lv, UINT old_dpi, UINT new_dpi) {
    if (!lv || !old_dpi || !new_dpi || old_dpi == new_dpi) return;

    int cols = 0;
    HWND hdr = ListView_GetHeader(lv);
    if (hdr) cols = Header_GetItemCount(hdr);

    for (int i = 0; i < cols; i++) {
        int w = ListView_GetColumnWidth(lv, i);
        if (w <= 0) continue;
        int nw = MulDiv(w, (int)new_dpi, (int)old_dpi);
        if (nw < 16) nw = 16;
        ListView_SetColumnWidth(lv, i, nw);
    }
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

static int lv_find_item_by_uid(HWND lv, uint32_t uid) {
    if (!lv || !uid) return -1;
    LVFINDINFOW fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)uid;
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

static bool lv_set_row_with(App& self, HWND lv, Item* it, const wchar_t* status_raw, time_t wall_ts) {
    if (!lv || !it || it->name.empty()) return false;

    int idx = lv_find_item_by_uid(lv, it->uid);
    if (idx < 0) idx = lv_find_item_by_name(lv, it->name.c_str());

    // Build status display without heap allocations
    wchar_t status_disp[64];
    status_disp[0] = 0;
    StringCchCopyW(status_disp, 64, (status_raw && status_raw[0]) ? status_raw : L"unknown");
    if (lv == self.lv_exe) {
        // Indicator: EXEs that were started automatically due to an active profile.
        if (self.profile_started_exes.find(it->name) != self.profile_started_exes.end()) {
            StringCchCatW(status_disp, 64, L" (Profile)");
        }
    }
    if (lv == self.lv_svc && it->flap_flagged) {
        wchar_t tmp[64];
        StringCchCopyW(tmp, 64, status_disp);
        StringCchPrintfW(status_disp, 64, L"! %s", tmp);
    }

    // Indicator: services with AUTO-STOP suppression due to repeated stop errors.
    if (lv == self.lv_svc) {
        uint64_t now_ms = now_mono_ms();
        if (it->svc_stop_suppress_until_ms && now_ms < it->svc_stop_suppress_until_ms) {
            // Keep this compact and stable (no countdown) to avoid churn.
            StringCchCatW(status_disp, 64, L" [SUPP");
            if (it->svc_last_stop_err) {
                wchar_t eb[32];
                StringCchPrintfW(eb, 32, L" e%lu", (unsigned long)it->svc_last_stop_err);
                StringCchCatW(status_disp, 64, eb);
            }
            StringCchCatW(status_disp, 64, L"]");
        }
    }

    bool inserted = false;

    if (idx < 0) {
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = ListView_GetItemCount(lv);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(L"");
        item.lParam = (LPARAM)it->uid;
        item.iImage = it->img;

        int row = ListView_InsertItem(lv, &item);
        inserted = (row >= 0);

        // Seed UI caches so subsequent updates can be cheap.
        StringCchCopyW(it->ui_cache_status_disp, 64, status_disp);
        if (it->ui_last_update_wall != wall_ts) {
            fmt_time_local(it->ui_cache_last_text, 64, wall_ts);
            it->ui_last_update_wall = wall_ts;
        }

        ListView_SetItemText(lv, row, 1, (LPWSTR)it->name.c_str());
        ListView_SetItemText(lv, row, 2, it->ui_cache_status_disp);
        ListView_SetItemText(lv, row, 3, it->ui_cache_last_text);

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
        pi.lParam = (LPARAM)it->uid;
        pi.iImage = it->img;
        ListView_SetItem(lv, &pi);

        // Update UI caches (status + last)
        if (wcscmp(it->ui_cache_status_disp, status_disp) != 0) {
            StringCchCopyW(it->ui_cache_status_disp, 64, status_disp);
        }
        if (it->ui_last_update_wall != wall_ts) {
            fmt_time_local(it->ui_cache_last_text, 64, wall_ts);
            it->ui_last_update_wall = wall_ts;
        }

        ListView_SetItemText(lv, idx, 2, it->ui_cache_status_disp);
        ListView_SetItemText(lv, idx, 3, it->ui_cache_last_text);

        BOOL cur = ListView_GetCheckState(lv, idx);
        BOOL want = it->auto_stop ? TRUE : FALSE;
        if ((cur != 0) != (want != 0)) {
            bool prev = self.suppress_lv_notify;
            self.suppress_lv_notify = true;
            ListView_SetCheckState(lv, idx, want);
            self.suppress_lv_notify = prev;
        }
    }

    return inserted;
}

// Update an existing ListView row for Item* (NO insertion). Returns true if updated.
static bool lv_update_row_existing_with(App& self, HWND lv, int kind, Item* it, const wchar_t* status_raw, time_t wall_ts, int* out_row_idx) {
    if (!lv || !it || it->name.empty()) return false;

    int idx = -1;
    auto& mp = self.lv_row_of_item[kind];
    auto itf = mp.find(it);
    if (itf != mp.end()) idx = itf->second;

    // Validate cached row still points at this uid in the current view index
    if (idx >= 0) {
        if (idx >= (int)self.view_uids[kind].size() || self.view_uids[kind][idx] != it->uid) idx = -1;
    }

    if (idx < 0) {
        // Fallback: linear search in view index (no ListView lParam dependency)
        for (int i = 0; i < (int)self.view_uids[kind].size(); ++i) {
            if (self.view_uids[kind][i] == it->uid) { idx = i; break; }
        }
        if (idx < 0) return false;
        mp[it] = idx;
    }

    if (out_row_idx) *out_row_idx = idx;

    // Build / update cached display strings only when needed.
    // (Status text can change due to status_raw or the profile indicator prefix.)
    wchar_t status_disp[64];
    status_disp[0] = 0;
    StringCchCopyW(status_disp, 64, (status_raw && status_raw[0]) ? status_raw : L"unknown");
    if (lv == self.lv_exe) {
        if (self.profile_started_exes.find(it->name) != self.profile_started_exes.end()) {
            // Prefix indicator
            wchar_t tmp[64];
            StringCchCopyW(tmp, 64, status_disp);
            StringCchPrintfW(status_disp, 64, L"* %s", tmp);
        }
    }

    // Only re-format the timestamp string if the wall time changed.
    if (it->ui_last_update_wall != wall_ts) {
        fmt_time_local(it->ui_cache_last_text, 64, wall_ts);
    }

    // Keep cached status display for quick comparisons and potential future owner-data support.
    StringCchCopyW(it->ui_cache_status_disp, 64, status_disp);

    status_disp[0] = 0;
    StringCchCopyW(status_disp, 64, (status_raw && status_raw[0]) ? status_raw : L"unknown");
    if (lv == self.lv_exe) {
        if (self.profile_started_exes.find(it->name) != self.profile_started_exes.end()) {
            // Prefix indicator
            wchar_t tmp[64];
            StringCchCopyW(tmp, 64, status_disp);
            StringCchPrintfW(status_disp, 64, L"* %s", tmp);
        }
    }

    // Avoid churn if nothing changed
    if (it->ui_applied_gen == it->status_gen &&
        wcscmp(it->ui_last_status, it->ui_cache_status_disp) == 0 &&
        it->ui_last_update_wall == wall_ts) {
        return false;
    }
    // Owner-data ListView: text/state are provided via LVN_GETDISPINFO.
    // Redraw the affected row so the control requests fresh display values.
    ListView_RedrawItems(lv, idx, idx);

    StringCchCopyW(it->ui_last_status, 32, it->ui_cache_status_disp);
    it->ui_last_update_wall = wall_ts;
    it->ui_applied_gen = it->status_gen;

    return true;
}


static bool lv_set_row(App& self, HWND lv, Item* it) {
    // Callers that hold self.mtx can use this wrapper safely.
    return lv_set_row_with(self, lv, it, it->last_status, it->last_update_wall);
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

static bool lv_update_row_existing_with(App& self, HWND lv, ItemKind kind, Item* it,
                                       const wchar_t* status_raw, time_t wall_ts, int* out_row_idx) {
    return lv_update_row_existing_with(self, lv, kind_index(kind), it, status_raw, wall_ts, out_row_idx);
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

static void ui_apply_status_updates(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    const int k = kind_index(kind);

    struct UiDelta {
        Item* it;
        wchar_t status[32];
        time_t wall;
    };

    std::vector<UiDelta> deltas;
    deltas.reserve(64);
    std::wstring flt;

    {
        ModelLockGuard lk(self);
        flt = self.filter[k];

        auto& dirty = self.dirty_status[k];
        deltas.reserve(dirty.size());
        for (Item* it : dirty) {
            if (!it) continue;
            UiDelta d{};
            d.it = it;
            StringCchCopyW(d.status, 32, it->last_status);
            d.wall = it->last_update_wall;
            deltas.push_back(d);
        }
        dirty.clear();
    }

    if (deltas.empty()) return;

    // Cheap de-dupe in case the same item changed multiple times before UI applied.
    if (deltas.size() > 1) {
        std::sort(deltas.begin(), deltas.end(),
                  [](const UiDelta& a, const UiDelta& b) { return a.it < b.it; });
        deltas.erase(std::unique(deltas.begin(), deltas.end(),
                                 [](const UiDelta& a, const UiDelta& b) { return a.it == b.it; }),
                     deltas.end());
    }

    int row_first = -1;
    int row_last  = -1;

    const bool big_burst = (deltas.size() > 12);

    // Batch redraw suppression reduces flicker and speeds up large bursts.
    if (big_burst) SendMessageW(lv, WM_SETREDRAW, FALSE, 0);

    for (const auto& d : deltas) {
        if (!d.it) continue;
        if (!item_matches_filter(d.it, flt)) continue;
        int row_idx = -1;
        bool changed = lv_update_row_existing_with(self, lv, kind, d.it, d.status, d.wall, &row_idx);
        if (changed && row_idx >= 0) {
            if (row_first < 0 || row_idx < row_first) row_first = row_idx;
            if (row_last < 0 || row_idx > row_last) row_last = row_idx;
        }
    }

    if (big_burst) SendMessageW(lv, WM_SETREDRAW, TRUE, 0);

    // Only redraw what actually changed (cheap + deterministic).
    if (row_first >= 0 && row_last >= row_first) {
        ListView_RedrawItems(lv, row_first, row_last);
        UpdateWindow(lv);
    }
}

// Back-compat wrapper for legacy int kind call sites
static void ui_apply_status_updates(App& self, int kind) {
    ui_apply_status_updates(self, kind_from_int(kind));
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

static void rebuild_listview_filtered(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    const int k = kind_index(kind);

    // Build view index from authoritative model under lock (uids are stable).
    std::wstring flt;
    {
        ModelLockGuard lk(self);
        flt = self.filter[k];

        self.view_uids[k].clear();
        self.view_uids[k].reserve(self.items[k].v.size());
        self.lv_row_of_item[k].clear();

        for (auto& up : self.items[k].v) {
            Item* it = up.get();
            if (!it) continue;
            if (!item_matches_filter(it, flt)) continue;
            const int row = (int)self.view_uids[k].size();
            self.view_uids[k].push_back(it->uid);
            self.lv_row_of_item[k][it] = row;
        }
    }

    bool prev_suppress = self.suppress_lv_notify;
    self.suppress_lv_notify = true;

    // Owner-data: set item count; rows are virtual and text/state comes via LVN_GETDISPINFO.
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCountEx(lv, (int)self.view_uids[k].size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, FALSE);

    self.suppress_lv_notify = prev_suppress;

    self.lv_needs_layout[k] = true;
    post_model_dirty(self);
}

 // Back-compat wrapper for legacy int kind call sites
static void rebuild_listview_filtered(App& self, int kind) {
    rebuild_listview_filtered(self, kind_from_int(kind));
}


static Item* lv_get_selected_item_ptr(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return nullptr;

    const int k = kind_index(kind);

    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) sel = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
    if (sel < 0) sel = ListView_GetSelectionMark(lv);
    if (sel < 0) return nullptr;

    uint32_t uid = 0;
    {
        ModelLockGuard lk(self);
        if (sel >= 0 && sel < (int)self.view_uids[k].size()) uid = self.view_uids[k][sel];
    }
    if (!uid) return nullptr;

    ModelLockGuard lk(self);
    auto f = self.uid_map.find(uid);
    return (f == self.uid_map.end()) ? nullptr : f->second;
}

static void toggle_autostop_selected(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    Item* sel = lv_get_selected_item_ptr(self, kind);
    if (!sel) { msgbox_warn(self.hwnd, L"Auto-stop", L"Select a row first."); return; }

    bool now = false;
    {
        ModelLockGuard lk(self);
        Item* real = list_find(&self.items[kind], sel->name.c_str());
        if (!real) return;
        real->auto_stop = !real->auto_stop;
        now = real->auto_stop;
    }

    int row = lv_find_item_by_uid(lv, sel->uid);
    if (row >= 0) {
        bool prev = self.suppress_lv_notify;
        self.suppress_lv_notify = true;
        ListView_SetCheckState(lv, row, now ? TRUE : FALSE);
        self.suppress_lv_notify = prev;
    }

    log_linef(self, L"%s: auto-stop %s", sel->name.c_str(), now ? L"enabled" : L"disabled");
    request_save_debounced(self);
}

static bool set_service_start_type(const wchar_t* svc_name, DWORD start_type, DWORD* out_err) {
    if (out_err) *out_err = 0;
    if (!svc_name || !svc_name[0]) { if (out_err) *out_err = ERROR_INVALID_NAME; return false; }

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) { if (out_err) *out_err = GetLastError(); return false; }

    unique_sc_handle svc(OpenServiceW(scm.get(), svc_name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG));
    if (!svc) { if (out_err) *out_err = GetLastError(); return false; }

    BOOL ok = ChangeServiceConfigW(
        svc.get(),
        SERVICE_NO_CHANGE,        // service type
        start_type,               // start type
        SERVICE_NO_CHANGE,        // error control
        NULL, NULL, NULL, NULL, NULL, NULL, NULL
    );

    if (!ok) { if (out_err) *out_err = GetLastError(); return false; }
    return true;
}

static void set_service_start_type_selected(App& self, DWORD start_type) {
    Item* sel = lv_get_selected_item_ptr(self, KIND_SVC);
    if (!sel) { msgbox_warn(self.hwnd, L"Service", L"Select a service row first."); return; }

    DWORD err = 0;
    if (!set_service_start_type(sel->name.c_str(), start_type, &err)) {
        wchar_t buf[256];
        StringCchPrintfW(buf, 256, L"Failed to change start type for '%s'.\n\nError: %lu", sel->name.c_str(), (unsigned long)err);
        msgbox_warn(self.hwnd, L"Service", buf);
        return;
    }

    const wchar_t* st = L"unknown";
    if (start_type == SERVICE_DISABLED) st = L"Disabled";
    else if (start_type == SERVICE_AUTO_START) st = L"Automatic";
    else if (start_type == SERVICE_DEMAND_START) st = L"Manual";

    log_linef(self, L"%s: start type set to %s", sel->name.c_str(), st);
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
static void apply_filter_now(App& self, int kind);
static void capture_columns_now(App& self, int kind);
static void apply_saved_columns(App& self, int kind);
static LRESULT CALLBACK SearchEditSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);


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
    ModelLockGuard lk(self);
    snapshot_active_from_runtime_locked(self);
}

static void rebuild_runtime_items_locked(App& self, const ConfigSnapshot& cfg) {
    // Clear existing items via centralized unregistration (keeps uid_map/by_name/dirty/view caches consistent).
    unregister_all_items_locked(self, ItemKind::Svc);
    unregister_all_items_locked(self, ItemKind::Exe);

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

            // Centralized registration keeps uid_map/by_name/dirty/view caches consistent.
            if (register_item_locked(self, kind_from_int(kind), it)) {
                // items_gen[kind] is bumped by caller when appropriate.
            } else {
                delete it;
                it = nullptr;
            }
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

    // Processes started by the *previous* active profile (close them when leaving)
    std::vector<StartedProc> to_stop_started;

    // Shell-open targets to launch for the *new* profile activation
    std::vector<std::wstring> to_launch;

    {
        ModelLockGuard lk(self);

        // Persist runtime edits into the currently active snapshot before switching.
        snapshot_active_from_runtime_locked(self);

        // Leaving any profile (or default): stop anything we previously started due to profile activation.
        if (!self.profile_started_procs.empty()) {
            to_stop_started.swap(self.profile_started_procs);
        }
        self.profile_started_exes.clear();

        self.active_profile = idx;
        pname = active_profile_name_locked(self);

        const ConfigSnapshot* cfg = &self.default_cfg;
        if (idx >= 0 && idx < (int)self.profiles.size()) cfg = &self.profiles[(size_t)idx].cfg;

        // Gather items to start for the new profile.
        if (idx >= 0 && idx < (int)self.profiles.size()) {
            for (const auto& r : self.profiles[(size_t)idx].start_items) {
                if (r.target.empty()) continue;
                to_launch.push_back(r.target);
            }
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

    // Close anything started by the previously active profile.
    // Do this outside the lock so we don't block UI / monitor thread.
    for (auto& sp : to_stop_started) {
        // Preferred: close/terminate the *exact* PID we started (when we have it).
        if (sp.pid || sp.hproc) {
            Action a;
            a.kind = KIND_EXE;
            a.op = ACTION_CLOSE_STARTED;
            a.name = sp.started_target;
            a.reason = L"profile deactivated (close started item)";
            a.wait_ms = self.stop_wait_ms;
            a.pid = sp.pid;
            if (sp.hproc) {
                HANDLE dup = NULL;
                DuplicateHandle(GetCurrentProcess(), sp.hproc.get(), GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
                a.hproc = dup;
            }
            action_enqueue(self, std::move(a));
continue;
        }

        // Fallback: if ShellExecuteEx didn't give us a process handle (common for some launches),
        // and the item was an .exe, stop by exe basename (may affect existing instances).
        if (!sp.exe_lower.empty()) {
            Action a;
            a.kind = KIND_EXE;
            a.op = ACTION_STOP;
            a.name = sp.exe_lower;
            a.reason = L"profile deactivated (stop started exe)";
            a.wait_ms = self.stop_wait_ms;
            action_enqueue(self, std::move(a));
}
    }

    // Start items for the newly activated profile.
    if (idx >= 0) {
        for (const auto& t : to_launch) {
            Action a;
            a.kind = KIND_EXE;
            a.op = ACTION_LAUNCH_ITEM;
            a.name = t;
            a.target = t;
            a.reason = L"profile activated (launch item)";
            a.wait_ms = 0;
            action_enqueue(self, std::move(a));
}
    }

    // Update tray immediately if needed
    tray_sync(self, GetModuleHandleW(NULL));

    // Refresh listviews (debounced to reduce churn during rapid changes)
    self.filter_pending[KIND_SVC] = true;
    self.filter_pending[KIND_EXE] = true;
    if (self.hwnd) SetTimer(self.hwnd, TIMER_SEARCH_DEBOUNCE, 120, NULL);
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

    // If this is an AUTO-STOP service stop and we've recently seen repeated stop errors,
    // skip enqueueing to prevent infinite retry loops (suppression).
    if (kind == KIND_SVC && op == ACTION_STOP && reason && wcsstr(reason, L"AUTO-STOP") != nullptr) {
        uint64_t now_ms = now_mono_ms();
        bool suppressed = false;
        DWORD last_err = 0;
        uint64_t until_ms = 0;
        {
            ModelLockGuard lk(self);
            Item* it = list_find(&self.items[KIND_SVC], name);
            if (it && it->svc_stop_suppress_until_ms && now_ms < it->svc_stop_suppress_until_ms) {
                suppressed = true;
                last_err = it->svc_last_stop_err;
                until_ms = it->svc_stop_suppress_until_ms;
            }
        }
        if (suppressed) {
            wchar_t msg[400];
            if (last_err) {
                StringCchPrintfW(msg, 400, L"%s: AUTO-STOP suppressed (e%lu) — not enqueueing stop.", name, (unsigned long)last_err);
            } else {
                StringCchPrintfW(msg, 400, L"%s: AUTO-STOP suppressed — not enqueueing stop.", name);
            }
            post_log(self, msg);
            (void)until_ms;
            return;
        }
    }

    action_enqueue(self, std::move(a));
}

// ----------------------------
// Monitor thread
// ----------------------------


// ----------------------------
// Preferences + Help dialogs (unchanged behavior; tiny addition: restart UI timer)
// ----------------------------
static bool prefs_apply(App& self, HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close, HWND c_dark);
static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_prefs_dialog(App& self, HWND parent);
[[maybe_unused]] static LRESULT CALLBACK HelpWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_help_dialog(App& self, HWND parent);

// Profiles dialog
static LRESULT CALLBACK ProfilesWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_profiles_dialog(App& self, HWND parent);

static bool prefs_apply(App& self, HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close, HWND c_dark) {
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

    bool dark_changed = false;
    {
        ModelLockGuard lk(self);

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

        // Theme
        BOOL dark_checked = (SendMessageW(c_dark, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool new_dark = dark_checked ? true : false;
        if (self.dark_mode != new_dark) {
            self.dark_mode = new_dark;
            dark_changed = true;
        }
    }

// Apply tray icon add/remove immediately
tray_sync(self, GetModuleHandleW(NULL));

    if (dark_changed) {
        darkmode_set_app(self.dark_mode);

        // Recompute theme resources once, then apply everywhere (idempotent).
        theme_compute(self);
        theme_apply_all_controls(self);
        self.ui_seen_theme_gen = self.theme_gen.load(std::memory_order_relaxed);

        // Populate listviews from the loaded config (decoupled from status updates).
        rebuild_listview_filtered(self, KIND_SVC);
        rebuild_listview_filtered(self, KIND_EXE);

        HMENU mb = GetMenu(self.hwnd);
        if (mb) {
            CheckMenuItem(mb, IDM_DARKMODE, MF_BYCOMMAND | (self.dark_mode ? MF_CHECKED : MF_UNCHECKED));
            DrawMenuBar(self.hwnd);
        }
    }

    log_linef(self, L"Settings updated: UI %dms, Monitor %dms, Cooldown %dms, StopWait %dms, Tray %d, CloseToTray %d, Dark %d",
              self.ui_refresh_ms, self.monitor_interval_ms, self.autostop_cooldown_ms, self.stop_wait_ms,
              self.tray_enabled ? 1 : 0, self.close_to_tray ? 1 : 0, self.dark_mode ? 1 : 0);

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
    static HWND c_tray, c_close, c_dark;
    static HWND st_cfg = NULL;
    // Per-dialog DPI state for the Preferences window
    static UINT s_prefs_dpi = 96;
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

        c_dark = CreateWindowW(L"BUTTON", L"Dark mode", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            S(12), S(200), S(200), S(22), dlg, (HMENU)IDC_PREF_DARKMODE, NULL, NULL);
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
        SendMessageW(c_dark, BM_SETCHECK, self.dark_mode ? BST_CHECKED : BST_UNCHECKED, 0);


        wchar_t cfgline[MAX_PATH + 64];
        StringCchPrintfW(cfgline, MAX_PATH + 64, L"Config file: %s", self.cfg_path);

        // Path can be long; show it with path ellipsis and size it to the dialog width.
        RECT rcw{}; GetClientRect(dlg, &rcw);
        int cw = rcw.right - rcw.left;
        int cfgX = S(12), cfgY = S(228), cfgH = S(20);
        int cfgW = std::max(S(100), cw - S(24));
        st_cfg = CreateWindowW(L"STATIC", cfgline,
            WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX,
            cfgX, cfgY, cfgW, cfgH, dlg, NULL, NULL, NULL);

CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            S(230), S(262), S(80), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE,
            S(318), S(262), S(80), S(28), dlg, (HMENU)IDC_PREF_APPLY, NULL, NULL);
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            S(406), S(262), S(80), S(28), dlg, (HMENU)IDOK, NULL, NULL);

        // Per-window DPI scaling (Profiles window is modeless; must scale its fixed 96-DPI layout)
        s_prefs_dpi = dpi;
        // Compute theme resources idempotently (allocates brushes/fonts/imagelists as needed)
        theme_compute_for_window(self, dlg);

        // Apply the correct DPI font for this dialog and then apply theme styling to its controls.
        HFONT f = theme_ensure_font_for_dpi(self, dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);
return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, self.br_bg);
        return 1;
    }
case WM_GETMINMAXINFO: {
        // Prevent the Preferences window from being resized so small that the bottom buttons clip.
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = s_prefs_dpi ? s_prefs_dpi : get_dpi_for_hwnd(dlg);
        int minw = dpi_scale(dpi, 510);
        int minh = dpi_scale(dpi, 380);
        // Convert desired client size to window size by adding current non-client deltas.
        RECT rcw{}; GetWindowRect(dlg, &rcw);
        RECT rcc{}; GetClientRect(dlg, &rcc);
        int ncw = (rcw.right - rcw.left) - (rcc.right - rcc.left);
        int nch = (rcw.bottom - rcw.top) - (rcc.bottom - rcc.top);
        mmi->ptMinTrackSize.x = minw + ((ncw>0)?ncw:0);
        mmi->ptMinTrackSize.y = minh + ((nch>0)?nch:0);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);


case WM_KEYDOWN: {
    if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'F')) {
        int sel = 0;
        if (self.center_tabs) {
            sel = (int)SendMessageW(self.center_tabs, TCM_GETCURSEL, 0, 0);
            if (sel < 0) sel = 0;
        }
        HWND edit = (sel == 0) ? self.svc_search : self.exe_search;
        if (edit) {
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return 0; // handled
        }
    }
    break; // let default processing handle other keys
}
case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
// Top bar buttons (custom menu)
if (id == IDC_TOP_SETTINGS && code == BN_CLICKED) {
    if (self.menu_settings) {
        // Keep checkmark in sync
        UINT f = self.dark_mode ? MF_CHECKED : MF_UNCHECKED;
        CheckMenuItem(self.menu_settings, IDM_DARKMODE, MF_BYCOMMAND | f);

        RECT r{}; GetWindowRect(self.btn_settings, &r);
        int cmd = TrackPopupMenuEx(self.menu_settings,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            r.left, r.bottom, dlg, NULL);
        if (cmd) PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
    return 0;
}
if (id == IDC_TOP_QUIT && code == BN_CLICKED) {
    PostMessageW(dlg, WM_CLOSE, 0, 0);
    return 0;
}
        // (Search boxes are on the main window, not Preferences)

        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        if (id == IDC_PREF_APPLY) { prefs_apply(self, dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close, c_dark); return 0; }
        if (id == IDOK) {
            if (prefs_apply(self, dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close, c_dark)) DestroyWindow(dlg);
            return 0;
        }
        return 0;
    }

    case WM_SIZE: {
        // Keep the long config path and bottom buttons sized/anchored to the window width.
        UINT dpi = s_prefs_dpi ? s_prefs_dpi : get_dpi_for_hwnd(dlg);
        auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };
        RECT rcw{}; GetClientRect(dlg, &rcw);
        int cw = rcw.right - rcw.left;
        int m = S(12);
        if (st_cfg) {
            int y = S(228);
            int h = S(20);
            int w = std::max(S(100), cw - (m * 2));
            SetWindowPos(st_cfg, NULL, m, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // Right-align Cancel / Apply / OK
        int btnW = S(80), btnH = S(28), gap = S(8);
        int yBtn = S(262);
        HWND hOK = GetDlgItem(dlg, IDOK);
        HWND hApply = GetDlgItem(dlg, IDC_PREF_APPLY);
        HWND hCancel = GetDlgItem(dlg, IDCANCEL);
        int xOK = cw - m - btnW;
        int xApply = xOK - gap - btnW;
        int xCancel = xApply - gap - btnW;
        if (hCancel) SetWindowPos(hCancel, NULL, xCancel, yBtn, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hApply) SetWindowPos(hApply, NULL, xApply, yBtn, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hOK) SetWindowPos(hOK, NULL, xOK, yBtn, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
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

        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, s_prefs_dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);
        }
        return 0;
    }    case WM_NCDESTROY:
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
    int w = dpi_scale(dpi, 510), h = dpi_scale(dpi, 380); // taller so bottom buttons never clip at high DPI
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

[[maybe_unused]] static LRESULT CALLBACK HelpWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
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
    static HWND b_close;
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

        b_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            S(492), S(362), S(80), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);
        (void)b_close; // suppress unused warning on builds where IDCANCEL handles close automatically
        const wchar_t* help = LR"(Hotkeys

Global
  Ctrl+F    Focus search (Services/EXEs)
  Ctrl+1    Switch to Services tab
  Ctrl+2    Switch to EXEs tab
  Ctrl+S    Save config now
  Ctrl+L    Focus the main list
  F5        Refresh UI (force resync)

Search
  Esc       Clear search (works from search box or list)

Service / EXE table
  Enter     Show details
  Del       Remove selected
  Space     Toggle Auto-stop
  Ctrl+C    Copy selected name
)";

        SetWindowTextW(edit, help);

        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, self.br_bg);
        return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (theme_draw_owner_button(self, dis)) return TRUE;
        if (theme_draw_owner_tab(self, dis)) return TRUE;
        if (theme_draw_owner_menu(self, dis)) return TRUE;
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);
    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(dlg); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void hotkeys_fill_list(HWND lv) {
    struct HK { const wchar_t* key; const wchar_t* action; };
    static const HK k[] = {
        { L"Ctrl+F",        L"Focus search (Services/EXEs)" },
        { L"Esc",           L"Clear search (works from search box or list)" },
        { L"Ctrl+1",        L"Switch to Services tab" },
        { L"Ctrl+2",        L"Switch to EXEs tab" },
        { L"Ctrl+S",        L"Save config now" },
        { L"Ctrl+L",        L"Focus the main list" },
        { L"F5",            L"Refresh UI (force resync)" },
        { L"Ctrl+Shift+D",  L"Toggle Dark Mode" },
        { L"Enter",         L"Show details (selected item)" },
        { L"Del",           L"Remove selected item" },
        { L"Space",         L"Toggle Auto-stop (selected item)" },
        { L"Ctrl+C",        L"Copy selected name" },
    };

    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)(sizeof(k)/sizeof(k[0])); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)k[i].key;
        int row = ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, row, 1, (LPWSTR)k[i].action);
    }
}

static LRESULT CALLBACK HotkeysWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* pself = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(dlg, msg, wParam, lParam);
    }
    case WM_CREATE: {
        pself = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
        if (!pself) return -1;
        App& self = *pself;

        theme_compute_for_window(self, dlg);

        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, dlg, (HMENU)IDC_HOTKEYS_LIST, (HINSTANCE)GetModuleHandleW(NULL), NULL);

        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW c0{}; c0.mask = LVCF_TEXT | LVCF_WIDTH; c0.pszText = (LPWSTR)L"Shortcut"; c0.cx = 140;
        LVCOLUMNW c1{}; c1.mask = LVCF_TEXT | LVCF_WIDTH; c1.pszText = (LPWSTR)L"Action";   c1.cx = 520;
        ListView_InsertColumn(lv, 0, &c0);
        ListView_InsertColumn(lv, 1, &c1);

        hotkeys_fill_list(lv);

        CreateWindowExW(0, L"BUTTON", L"Copy",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 80, 28, dlg, (HMENU)IDC_HOTKEYS_COPY, (HINSTANCE)GetModuleHandleW(NULL), NULL);

        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 80, 28, dlg, (HMENU)IDCANCEL, (HINSTANCE)GetModuleHandleW(NULL), NULL);

        // apply themed fonts
        UINT dpi = get_dpi_for_hwnd(dlg);
        HFONT f = theme_ensure_font_for_dpi(self, dpi);
        SendMessageW(lv, WM_SETFONT, (WPARAM)f, TRUE);

        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(dlg, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int pad = 10;
        int btnW = 96;
        int btnH = 28;
        int gap = 8;

        HWND lv = GetDlgItem(dlg, IDC_HOTKEYS_LIST);
        HWND b_copy = GetDlgItem(dlg, IDC_HOTKEYS_COPY);
        HWND b_close = GetDlgItem(dlg, IDCANCEL);

        int btnY = h - pad - btnH;
        int closeX = w - pad - btnW;
        int copyX  = closeX - gap - btnW;

        if (lv) SetWindowPos(lv, NULL, pad, pad, w - pad*2, btnY - pad - gap, SWP_NOZORDER);
        if (b_copy) SetWindowPos(b_copy, NULL, copyX, btnY, btnW, btnH, SWP_NOZORDER);
        if (b_close) SetWindowPos(b_close, NULL, closeX, btnY, btnW, btnH, SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND: {
        pself = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
        if (!pself) break;
        App& self = *pself;

        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IDC_HOTKEYS_COPY && code == BN_CLICKED) {
            // Build a readable list for pasting: "Key - Action"
            HWND lv = GetDlgItem(dlg, IDC_HOTKEYS_LIST);
            std::wstring out;
            if (lv) {
                int n = ListView_GetItemCount(lv);
                wchar_t kbuf[128], abuf[512];
                for (int i = 0; i < n; i++) {
                    kbuf[0]=0; abuf[0]=0;
                    ListView_GetItemText(lv, i, 0, kbuf, (int)(sizeof(kbuf)/sizeof(kbuf[0])));
                    ListView_GetItemText(lv, i, 1, abuf, (int)(sizeof(abuf)/sizeof(abuf[0])));
                    out += kbuf;
                    out += L"  -  ";
                    out += abuf;
                    out += L"\r\n";
                }
            }
            if (out.empty()) out = L"(no hotkeys)\r\n";
            clipboard_set_text(dlg, out.c_str());
            log_linef(self, L"Hotkeys copied to clipboard.");
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}



static LRESULT CALLBACK HotkeysWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_hotkeys_dialog(App& self, HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HotkeysWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HotkeysDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    UINT dpi = get_dpi_for_hwnd(parent);
    int w = dpi_scale(dpi, 600), h = dpi_scale(dpi, 430);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Hotkeys",
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

// Help menu currently shows the same content as Hotkeys.
static void open_help_dialog(App& self, HWND parent) {
    open_hotkeys_dialog(self, parent);
}



// ----------------------------
// Profiles dialog (manage automatic profile activation WATCH EXEs)
// ----------------------------

// ---------------- Diagnostics dialog ----------------

struct DiagState {
    App* self = nullptr;
    HWND edit = NULL;
    HWND b_copy = NULL;
    HWND b_close = NULL;
    UINT dpi = 96;
};

static std::wstring build_diagnostics_text(App& self) {
    std::wstring out;
    out.reserve(4096);

    auto appendf = [&](const wchar_t* fmt, ...) {
        wchar_t buf[1024];
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfW(buf, 1024, fmt, ap);
        va_end(ap);
        out.append(buf);
    };

    appendf(L"ServicesExeMonitor diagnostics\r\n");
    appendf(L"Built: %hs %hs\r\n", __DATE__, __TIME__);
    appendf(L"\r\n");

    // Basic runtime settings
    appendf(L"UI refresh: %d ms\r\n", self.ui_refresh_ms);
    appendf(L"Monitor tick: %d ms\r\n", self.monitor_interval_ms);
    appendf(L"Autostop cooldown: %d ms\r\n", self.autostop_cooldown_ms);
    appendf(L"Stop wait: %d ms\r\n", self.stop_wait_ms);
    appendf(L"Tray enabled: %d\r\n", self.tray_enabled ? 1 : 0);
    appendf(L"Close-to-tray: %d\r\n", self.close_to_tray ? 1 : 0);
    appendf(L"Dark mode: %d\r\n", self.dark_mode ? 1 : 0);
    appendf(L"\r\n");

    // Queue depth
    size_t qdepth = 0;
    { qdepth = action_qdepth(self); }
    appendf(L"Actions queued: %zu\r\n", qdepth);

    // Model stats + per-item last errors
    size_t svc_n = 0, exe_n = 0;
    {
        ModelLockGuard lk(self);
        svc_n = self.items[KIND_SVC].v.size();
        exe_n = self.items[KIND_EXE].v.size();
    }
    appendf(L"Items: services=%zu, exes=%zu\r\n", svc_n, exe_n);
    appendf(L"\r\n");

    appendf(L"Last service stop errors (non-zero):\r\n");
    int printed = 0;
    {
        ModelLockGuard lk(self);
        for (auto& up : self.items[KIND_SVC].v) {
            Item* it = up.get();
            if (!it) continue;
            if (!it->svc_last_stop_err) continue;

            const DWORD e = it->svc_last_stop_err;
            wchar_t tbuf[64] = L"";
            if (it->svc_stop_suppress_until_ms) {
                StringCchPrintfW(tbuf, 64, L" (suppressed %llums)", (unsigned long long)it->svc_stop_suppress_until_ms);
            }
            appendf(L"  %s : %lu%s\r\n", it->name.c_str(), (unsigned long)e, tbuf);
            if (++printed >= 200) { appendf(L"  ... (truncated)\r\n"); break; }
        }
    }
    if (printed == 0) appendf(L"  (none)\r\n");

    return out;
}

static LRESULT CALLBACK DiagnosticsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    const int IDC_DIAG_COPY = 1001;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) {
            auto* st = new DiagState();
            st->self = self_ptr;
            st->dpi = get_dpi_for_hwnd(dlg);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        }
        return TRUE;
    }

    auto* st = reinterpret_cast<DiagState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!st || !st->self) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *st->self;

    switch (msg) {
    case WM_CREATE: {
        st->dpi = get_dpi_for_hwnd(dlg);
        auto S = [&](int v96) -> int { return dpi_scale(st->dpi, v96); };

        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            S(12), S(12), S(520), S(320), dlg, (HMENU)1, NULL, NULL);

        st->b_copy = CreateWindowW(L"BUTTON", L"Copy diagnostics", WS_CHILD | WS_VISIBLE,
            S(12), S(342), S(150), S(28), dlg, (HMENU)IDC_DIAG_COPY, NULL, NULL);
        st->b_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            S(382), S(342), S(150), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);

        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, st->dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);

        std::wstring text = build_diagnostics_text(self);
        SetWindowTextW(st->edit, text.c_str());
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, self.br_bg);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);

    case WM_SIZE: {
        auto S = [&](int v96) -> int { return dpi_scale(st->dpi, v96); };
        RECT rc{}; GetClientRect(dlg, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        int m = S(12);
        int btnH = S(28);
        int btnW = S(150);
        int gap = S(8);
        int yBtn = ch - m - btnH;
        int editH = std::max(S(80), yBtn - m - gap);
        if (st->edit) SetWindowPos(st->edit, NULL, m, m, cw - (m * 2), editH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (st->b_copy) SetWindowPos(st->b_copy, NULL, m, yBtn, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (st->b_close) SetWindowPos(st->b_close, NULL, cw - m - btnW, yBtn, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_DPICHANGED: {
        UINT new_dpi = LOWORD(wParam);
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        if (rc) SetWindowPos(dlg, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        if (new_dpi && new_dpi != st->dpi) {
            scale_children(dlg, st->dpi, new_dpi);
            st->dpi = new_dpi;
            theme_compute_for_window(self, dlg);
            HFONT f = theme_ensure_font_for_dpi(self, st->dpi);
            if (f) apply_font_recursive(dlg, f);
            theme_apply_to_window(self, dlg);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IDC_DIAG_COPY && code == BN_CLICKED) {
            std::wstring text = build_diagnostics_text(self);
            if (clipboard_set_text(dlg, text.c_str())) {
                log_linef(self, L"Diagnostics copied to clipboard.");
            }
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;

    case WM_NCDESTROY: {
        auto* st2 = reinterpret_cast<DiagState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
        SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        delete st2;
        return 0;
    }
    }

    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void open_diagnostics_dialog(App& self, HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DiagnosticsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"DiagnosticsDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    UINT dpi = get_dpi_for_hwnd(parent);
    int w = dpi_scale(dpi, 560), h = dpi_scale(dpi, 420);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Diagnostics",
        WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, parent, NULL, wc.hInstance, &self);

    ShowWindow(dlg, SW_SHOW);
    BringWindowToTop(dlg);
}
// ----------------------------------------------------
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
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
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
static void prof_fill_start_exe_list(App& self, HWND lb, int prof_idx) {
    if (!lb) return;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    std::vector<StartItem> rows;
    {
        ModelLockGuard lk(self);
        if (prof_idx >= 0 && prof_idx < (int)self.profiles.size()) {
            rows = self.profiles[(size_t)prof_idx].start_items;
        }
    }

    for (const auto& r : rows) {
        if (r.target.empty()) continue;
        std::wstring line = r.target;
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (!rows.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
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
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
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

    // Profile contents editors (services / exes) — editable even when profile is not active
    static HWND lb_svc, e_svc, chk_svc, b_svc_add, b_svc_del, b_svc_toggle;
    static HWND lb_exe, e_exe, chk_exe, b_exe_add, b_exe_del, b_exe_toggle;
    // Start Items list (shell-open; best-effort closed on deactivation)
    static HWND lb_start_exe, e_start_exe, b_start_exe_add, b_start_exe_del;
    static HWND b_close;
    // Static labels we want to reposition on resize

static HWND st_profiles_hdr = NULL, st_name_hdr = NULL;
static HWND st_watch_hdr = NULL, st_watch_add_hdr = NULL;
static HWND st_start_hdr = NULL, st_start_add_hdr = NULL;
static HWND st_svc_hdr = NULL, st_exe_hdr = NULL;
static HWND st_note = NULL;
static HWND tab_right = NULL;

static UINT s_prof_dpi = 96;
    auto do_layout = [&]() {
    RECT rc{}; GetClientRect(dlg, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    const int m  = dpi_scale(s_prof_dpi, 12);
    const int g  = dpi_scale(s_prof_dpi, 10);
    const int g2 = dpi_scale(s_prof_dpi, 6);

    const int headerH = dpi_scale(s_prof_dpi, 22);
    const int bigHdrH = headerH * 2;
    const int labelH  = dpi_scale(s_prof_dpi, 18);
    const int editH   = dpi_scale(s_prof_dpi, 24);
    const int btnH    = dpi_scale(s_prof_dpi, 28);

    const int btnW_sm = dpi_scale(s_prof_dpi, 34);

    // Provide room for the bottom Close button strip
    const int bottom_strip = btnH + g;
    const int content_top  = m;
    const int content_bot  = std::max(content_top, ch - m - bottom_strip);

    // --- Batch child window moves during resize ---
    // This avoids flicker/"mixed up" intermediate layouts while the user drags the sizer.
    SendMessageW(dlg, WM_SETREDRAW, FALSE, 0);
    HDWP hdwp = BeginDeferWindowPos(96);
    auto DW = [&](HWND h, int x, int y, int w, int hgt) {
        if (!h) return;
        if (w < 1) w = 1;
        if (hgt < 1) hgt = 1;
        if (hdwp) {
            hdwp = DeferWindowPos(hdwp, h, NULL, x, y, w, hgt,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
        } else {
            SetWindowPos(h, NULL, x, y, w, hgt,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
        }
    };
    auto DWS = DW;

    // Visibility changes happen while redraw is disabled, so they won't repaint mid-layout.
    auto VIS = [&](HWND h, bool on) { if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE); };

    // Right-side tab selection
    int tab = 0;
    if (tab_right) {
        tab = TabCtrl_GetCurSel(tab_right);
        if (tab < 0) tab = 0;
    }
    const bool show_activation = (tab == 0);
    const bool show_start      = (tab == 1);
    const bool show_services   = (tab == 2);
    const bool show_exes       = (tab == 3);

    // Visibility (right pane)
    VIS(st_watch_hdr, show_activation);
    VIS(lb_watch, show_activation);
    VIS(st_watch_add_hdr, show_activation);
    VIS(e_watch, show_activation);
    VIS(b_watch_add, show_activation);
    VIS(b_watch_del, show_activation);

    VIS(st_start_hdr, show_start);
    VIS(lb_start_exe, show_start);
    VIS(st_start_add_hdr, show_start);
    VIS(e_start_exe, show_start);
    VIS(b_start_exe_add, show_start);
    VIS(b_start_exe_del, show_start);

    // Services tab (profile contents: services)
    VIS(st_note, show_services || show_exes);

    VIS(st_svc_hdr, show_services);
    VIS(lb_svc, show_services);
    VIS(e_svc, show_services);
    VIS(chk_svc, show_services);
    VIS(b_svc_add, show_services);
    VIS(b_svc_toggle, show_services);
    VIS(b_svc_del, show_services);

    // EXEs tab (profile contents: exes)
    VIS(st_exe_hdr, show_exes);
    VIS(lb_exe, show_exes);
    VIS(e_exe, show_exes);
    VIS(chk_exe, show_exes);
    VIS(b_exe_add, show_exes);
    VIS(b_exe_toggle, show_exes);
    VIS(b_exe_del, show_exes);


    // Left + right columns (stable, no vertical splitting)
    const int minLeftW   = dpi_scale(s_prof_dpi, 330);
    const int preferLeft = dpi_scale(s_prof_dpi, 360);
    const int minRightW  = dpi_scale(s_prof_dpi, 420);

    int xL = m;
    int rightAvail = cw - (m * 2) - g - minRightW;
    int leftW = std::min(preferLeft, std::max(minLeftW, rightAvail));
    leftW = std::max(minLeftW, leftW);

    int xR = xL + leftW + g;
    int rightW = std::max(minRightW, cw - m - xR);

    const int yTop = content_top;
    const int yBot = content_bot;

    // LEFT PANE (profiles list + name + actions)
    auto layout_left = [&](int x, int w, int yTop, int yBot) {
        int yy = yTop;

        // Reserve a slim right column for Up/Down when we have enough width.
        const int reserve_min_main = dpi_scale(s_prof_dpi, 260);
        int reserve = (w >= (reserve_min_main + btnW_sm + g2)) ? (btnW_sm + g2) : 0;
        int mainW = std::max(1, w - reserve);
        int upX = x + mainW + (reserve ? g2 : 0);

        DWS(st_profiles_hdr, x, yy, mainW, headerH); yy += headerH + g2;

        // Profiles list (never collapses)
        const int lineH = std::max(16, dpi_scale(s_prof_dpi, 16));
        const int minList = std::max(dpi_scale(s_prof_dpi, 170), lineH * 8);
        const int worstButtonsRows = 4; // conservative
        const int needBelow = headerH + editH + (btnH * worstButtonsRows) + (g2 * 10) + (labelH * 2);
        int listH = std::max(minList, (yBot - yy) / 2);
        listH = std::min(listH, std::max(minList, yBot - yy - needBelow));
        listH = std::max(minList, listH);

        DW(lb_profiles, x, yy, mainW, listH); yy += listH + g;

        DWS(st_name_hdr, x, yy, mainW, headerH); yy += headerH + g2;
        DW(e_name, x, yy, mainW, editH); yy += editH + g2;

        // Action buttons (adaptive; never overlap each other or Up/Down)
        const int wAdd    = dpi_scale(s_prof_dpi, 72);
        const int wRename = dpi_scale(s_prof_dpi, 86);
        const int wDel    = dpi_scale(s_prof_dpi, 78);
        const int padBtns = dpi_scale(s_prof_dpi, 6);

        const int needOneRow = wAdd + wRename + wDel + padBtns * 2;

        if (mainW >= needOneRow) {
            int bx = x;
            DW(b_add, bx, yy, wAdd, btnH); bx += wAdd + padBtns;
            DW(b_rename, bx, yy, wRename, btnH); bx += wRename + padBtns;
            DW(b_del, bx, yy, wDel, btnH);

            if (reserve) DW(b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;
            if (reserve) {
                DW(b_down, upX, yy, btnW_sm, btnH);
                yy += btnH + g2;
            }
        } else if (mainW >= (wAdd + wRename + padBtns)) {
            int w2 = (mainW - padBtns) / 2;
            DW(b_add, x, yy, w2, btnH);
            DW(b_rename, x + w2 + padBtns, yy, mainW - w2 - padBtns, btnH);
            if (reserve) DW(b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(b_del, x, yy, mainW, btnH);
            if (reserve) DW(b_down, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;
        } else {
            DW(b_add, x, yy, mainW, btnH);
            if (reserve) DW(b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(b_rename, x, yy, mainW, btnH);
            if (reserve) DW(b_down, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(b_del, x, yy, mainW, btnH);
            yy += btnH + g2;
        }

        if (!reserve) {
            int half = (mainW - padBtns) / 2;
            half = std::max(dpi_scale(s_prof_dpi, 60), half);
            DW(b_up, x, yy, half, btnH);
            DW(b_down, x + half + padBtns, yy, mainW - half - padBtns, btnH);
            yy += btnH + g2;
        }

        // Active note/status (2 lines)
        DWS(st_active, x, yy, mainW, labelH * 2);
        yy += (labelH * 2) + g2;

        return yy;
    };

    layout_left(xL, leftW, yTop, yBot);

    // Bottom close button (anchored)
    if (b_close) {
        const int closeW = dpi_scale(s_prof_dpi, 96);
        DW(b_close, cw - m - closeW, ch - m - btnH, closeW, btnH);
    }

    // RIGHT PANE: tab control + active page layout
    if (!tab_right) return;

    DW(tab_right, xR, yTop, rightW, yBot - yTop);

    // Page area inside the tab
    RECT pr{ 0, 0, rightW, yBot - yTop };
    TabCtrl_AdjustRect(tab_right, FALSE, &pr);
    int px = xR + pr.left;
    int py = yTop + pr.top;
    int pw = std::max(1, (int)(pr.right - pr.left));
    int ph = std::max(1, (int)(pr.bottom - pr.top));

    auto layout_add_row = [&](HWND st_hdr, HWND edit, HWND b1, HWND b2, int x, int &yy, int w) {
        DWS(st_hdr, x, yy, w, headerH);
        yy += headerH + g2;

        const int btn1W = dpi_scale(s_prof_dpi, 118);
        const int btn2W = dpi_scale(s_prof_dpi, 132);
        const int editMin = dpi_scale(s_prof_dpi, 220);
        const int inlineNeed = editMin + g2 + btn1W + g2 + btn2W;

        if (w >= inlineNeed) {
            int editW = std::max(editMin, w - (btn1W + btn2W + g2 * 2));
            if (editW > w) editW = w;
            int bx = x + editW + g2;
            DW(edit, x, yy, editW, editH);
            DW(b1, bx, yy, btn1W, btnH);
            DW(b2, bx + btn1W + g2, yy, btn2W, btnH);
            yy += std::max(editH, btnH) + g2;
        } else {
            DW(edit, x, yy, w, editH);
            yy += editH + g2;
            int half = (w - g2) / 2;
            half = std::max(dpi_scale(s_prof_dpi, 90), half);
            DW(b1, x, yy, half, btnH);
            DW(b2, x + half + g2, yy, w - half - g2, btnH);
            yy += btnH + g2;
        }
    };

    if (show_activation) {
        int yy = py;
        DWS(st_watch_hdr, px, yy, pw, bigHdrH); yy += bigHdrH + g2;

        const int addNeed = headerH + g2 + std::max(editH, btnH) + g2;
        int avail = ph - (yy - py) - addNeed - g2;
        int listH = std::max(1, avail);
        DW(lb_watch, px, yy, pw, listH); yy += listH + g2;

        layout_add_row(st_watch_add_hdr, e_watch, b_watch_add, b_watch_del, px, yy, pw);
    }
    else if (show_start) {
        int yy = py;
        DWS(st_start_hdr, px, yy, pw, bigHdrH); yy += bigHdrH + g2;

        const int addNeed = headerH + g2 + std::max(editH, btnH) + g2;
        int avail = ph - (yy - py) - addNeed - g2;
        int listH = std::max(1, avail);
        DW(lb_start_exe, px, yy, pw, listH); yy += listH + g2;

        layout_add_row(st_start_add_hdr, e_start_exe, b_start_exe_add, b_start_exe_del, px, yy, pw);
    }
    else if (show_services || show_exes) {
        int yy = py;

        // Note / tips area (show on both Services + EXEs tabs)
        DW(st_note, px, yy, pw, labelH * 3);
        yy += (labelH * 3) + g;

        // Remaining area for list + controls
        int remainingH = std::max(1, ph - (yy - py));

        const int chkH = dpi_scale(s_prof_dpi, 22);
        // Keep bottom controls fully visible; let the list shrink if space is tight.
        const int ctrlTail = editH + g2 + chkH + g2 + btnH + g2 + btnH + g2 + btnH;
        int listH = remainingH - (headerH + g2) - g2 - ctrlTail;
        listH = std::max(1, listH);

        if (show_services) {
            // --- Services content ---
            DWS(st_svc_hdr, px, yy, pw, headerH);
            int yList = yy + headerH + g2;

            DW(lb_svc, px, yList, pw, listH);

            int yCtrl = yList + listH + g2;
            DW(e_svc, px, yCtrl, pw, editH); yCtrl += editH + g2;
            DW(chk_svc, px, yCtrl, pw, chkH); yCtrl += chkH + g2;

            // Buttons stacked (never overlap)
            DW(b_svc_add,    px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(b_svc_toggle, px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(b_svc_del,    px, yCtrl, pw, btnH);
        } else {
            // --- EXEs content ---
            DWS(st_exe_hdr, px, yy, pw, headerH);
            int yList = yy + headerH + g2;

            DW(lb_exe, px, yList, pw, listH);

            int yCtrl = yList + listH + g2;
            DW(e_exe, px, yCtrl, pw, editH); yCtrl += editH + g2;
            DW(chk_exe, px, yCtrl, pw, chkH); yCtrl += chkH + g2;

            DW(b_exe_add,    px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(b_exe_toggle, px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(b_exe_del,    px, yCtrl, pw, btnH);
        }
    }

    // Finish batched layout and repaint once.
    if (hdwp) EndDeferWindowPos(hdwp);
    SendMessageW(dlg, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    };

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
            ModelLockGuard lk(self);
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

        st_profiles_hdr = CreateWindowW(L"STATIC", L"Profiles", WS_CHILD | WS_VISIBLE,
            12, 10, 240, 20, dlg, NULL, NULL, NULL);

        lb_profiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            12, 34, 250, 270, dlg, (HMENU)IDC_PROF_LIST, NULL, NULL);

        st_name_hdr = CreateWindowW(L"STATIC", L"Profile name:", WS_CHILD | WS_VISIBLE,
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


tab_right = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
    280, 10, 520, 560, dlg, (HMENU)IDC_PROFILES_TAB, NULL, NULL);
if (tab_right) {
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Activation";
    TabCtrl_InsertItem(tab_right, 0, &ti);
    ti.pszText = (LPWSTR)L"Start Items";
    TabCtrl_InsertItem(tab_right, 1, &ti);
    ti.pszText = (LPWSTR)L"Services";
    TabCtrl_InsertItem(tab_right, 2, &ti);
    ti.pszText = (LPWSTR)L"EXEs";
    TabCtrl_InsertItem(tab_right, 3, &ti);
    TabCtrl_SetCurSel(tab_right, 0);
}

        st_watch_hdr = CreateWindowW(L"STATIC", L"Activation (WATCH EXEs)\r\nProfile activates if ANY are running", WS_CHILD | WS_VISIBLE,
            280, 10, 460, 20, dlg, NULL, NULL, NULL);

        lb_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 34, 460, 230, dlg, (HMENU)IDC_WATCH_LIST, NULL, NULL);

        st_watch_add_hdr = CreateWindowW(L"STATIC", L"Add WATCH exe (name or full path):", WS_CHILD | WS_VISIBLE,
            280, 270, 300, 18, dlg, NULL, NULL, NULL);

        e_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 290, 340, 24, dlg, (HMENU)IDC_WATCH_EDIT, NULL, NULL);

        b_watch_add = CreateWindowW(L"BUTTON", L"Add WATCH", WS_CHILD | WS_VISIBLE,
            628, 290, 112, 24, dlg, (HMENU)IDC_WATCH_ADD, NULL, NULL);

        b_watch_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            628, 320, 112, 24, dlg, (HMENU)IDC_WATCH_REMOVE, NULL, NULL);

        // ---- Profile contents editor (services / exes) ----

        // Services
        st_svc_hdr = CreateWindowW(L"STATIC", L"Services in this profile:", WS_CHILD | WS_VISIBLE,
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
        st_exe_hdr = CreateWindowW(L"STATIC", L"EXEs in this profile:", WS_CHILD | WS_VISIBLE,
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

        st_note = CreateWindowW(L"STATIC",
            L"Note: If multiple profiles match, the first one in this list wins.\r\n"
            L"Profiles deactivate automatically when none of their WATCH EXEs are running.",
            WS_CHILD | WS_VISIBLE,
            280, 756, 460, 40, dlg, NULL, NULL, NULL);


        // Start Items on activation (separate from Auto-stop lists)

        st_start_hdr = CreateWindowW(L"STATIC",
            L"Start items when this profile activates\r\n(best-effort closed on deactivation):",
            WS_CHILD | WS_VISIBLE,
            280, 804, 460, 18, dlg, NULL, NULL, NULL);

        lb_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 826, 460, 110, dlg, (HMENU)IDC_PSTART_EXE_LIST, NULL, NULL);

        st_start_add_hdr = CreateWindowW(L"STATIC", L"Add start item (file/path/URL):", WS_CHILD | WS_VISIBLE,
            280, 944, 300, 18, dlg, NULL, NULL, NULL);

        e_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 962, 340, 24, dlg, (HMENU)IDC_PSTART_EXE_EDIT, NULL, NULL);

        b_start_exe_add = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
            630, 960, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_ADD, NULL, NULL);

        b_start_exe_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            630, 988, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_REMOVE, NULL, NULL);
        b_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
     660, 1040, 80, 28, dlg, (HMENU)IDCANCEL, NULL, NULL);
        // DPI-aware: scale child controls + apply themed font for this window's DPI.
        s_prof_dpi = get_dpi_for_hwnd(dlg);
        if (s_prof_dpi != 96) {
            scale_children(dlg, 96, s_prof_dpi);
        }
        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, s_prof_dpi);
        if (f) {
            apply_font_recursive(dlg, f);
            if (tab_right) SendMessageW(tab_right, WM_SETFONT, (WPARAM)f, TRUE);
        }

        prof_fill_profile_list(self, lb_profiles, 0);
        refresh_for_selection();

        theme_apply_to_window(self, dlg);
        do_layout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* p = reinterpret_cast<MINMAXINFO*>(lParam);
        if (p) {
            const int minW = dpi_scale(s_prof_dpi, 900);
            const int minH = dpi_scale(s_prof_dpi, 620);
            p->ptMinTrackSize.x = minW;
            p->ptMinTrackSize.y = minH;
        }
        return 0;
    }

    case WM_SIZE:
        do_layout();
        return 0;

    case WM_NOTIFY: {
        auto* nh = reinterpret_cast<NMHDR*>(lParam);
        if (nh && nh->hwndFrom == tab_right && nh->code == TCN_SELCHANGE) {
            do_layout(); // update visibility + layout for the new tab
            return 0;
        }

if (nh && nh->code == NM_CUSTOMDRAW) {
    wchar_t ccls[64]{0};
    GetClassNameW(nh->hwndFrom, ccls, 63);
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
    if (lstrcmpiW(ccls, WC_TABCONTROLW) == 0) {
        return theme_customdraw_tab(self, cd);
    }
    if (lstrcmpiW(ccls, WC_HEADER) == 0) {
        return theme_customdraw_header(self, cd);
    }
    if (lstrcmpiW(ccls, STATUSCLASSNAMEW) == 0) {
        // Status bar: enforce themed text/background. STATUSCLASSNAME sends NM_CUSTOMDRAW.
        // NMCUSTOMDRAW does not have clrText/clrTextBk fields, so set colors on the HDC.
        if (cd->dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
        }
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            SetTextColor(cd->hdc, self.col_text);
            SetBkColor(cd->hdc, self.col_panel);
            return CDRF_NEWFONT;
        }
    }
    if (lstrcmpiW(ccls, WC_LISTVIEWW) == 0) {
        return theme_customdraw_listview(self, (NMLVCUSTOMDRAW*)cd);
    }
}

break;
    }
case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, self.br_bg);
        return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (theme_draw_owner_button(self, dis)) return TRUE;
        if (theme_draw_owner_tab(self, dis)) return TRUE;
        if (theme_draw_owner_menu(self, dis)) return TRUE;
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        int code = HIWORD(wParam);
        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }

        if (id == IDC_PROF_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PROF_ADD) {
            int new_sel = -1;
            {
                ModelLockGuard lk(self);

                // Ensure default snapshot exists
                if (!self.have_default_cfg) {
                    snapshot_from_runtime_locked(self, self.default_cfg);
                    self.have_default_cfg = true;
                }

                ProfileSnapshot p;
                p.name = make_unique_profile_name_locked(self, L"Profile");
                p.cfg = self.default_cfg; // start from default
                self.profiles.push_back(std::move(p));
                rebuild_profile_watch_keys_locked(self);
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
                ModelLockGuard lk(self);

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
                ModelLockGuard lk(self);

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

                    rebuild_profile_watch_keys_locked(self);

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
                ModelLockGuard lk(self);

                if (other >= 0 && other < (int)self.profiles.size()) {
                    std::swap(self.profiles[(size_t)sel], self.profiles[(size_t)other]);

                    rebuild_profile_watch_keys_locked(self);

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
                ModelLockGuard lk(self);
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
                        self.profiles[(size_t)sel].watch_keys_lower = self.profiles[(size_t)sel].watch_exes;
                        rebuild_profile_watch_keys_locked(self);
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
                ModelLockGuard lk(self);
                if (sel >= 0 && sel < (int)self.profiles.size()) {
                    auto& ws = self.profiles[(size_t)sel].watch_exes;
                    if (wsel >= 0 && wsel < (int)ws.size()) {
                        ws.erase(ws.begin() + wsel);
                        removed = true;
                        self.profiles[(size_t)sel].watch_keys_lower = self.profiles[(size_t)sel].watch_exes;
                        rebuild_profile_watch_keys_locked(self);
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
                ModelLockGuard lk(self);
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
                ModelLockGuard lk(self);
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
                ModelLockGuard lk(self);
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
                ModelLockGuard lk(self);
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
                ModelLockGuard lk(self);
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
                ModelLockGuard lk(self);
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
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter a file/path/target to start."); return 0; }

            std::wstring target = normalize_start_target_best_effort(raw);
            if (target.empty()) { msgbox_warn(dlg, L"Profiles", L"That target is empty/invalid."); return 0; }

            // If the target looks like an EXE, keep the convenience behavior:
            // ensure it exists in the profile EXE list too (so the main UI can show it, and we can store exe_path).
            wchar_t norm_exe[512]; norm_exe[0] = 0;
            wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
            const bool is_exe = ends_with_i(target.c_str(), L".exe") != 0;
            if (is_exe) {
                normalize_exe_input(target.c_str(), norm_exe, 512);
                resolve_exe_launch_path(target.c_str(), exe_path_full, MAX_PATH);
            }

            bool added = false;
            bool start_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    ProfileSnapshot& p = self.profiles[(size_t)psel];

                    bool dup = false;
                    for (const auto& r : p.start_items) {
                        if (_wcsicmp(r.target.c_str(), target.c_str()) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        StartItem si;
                        si.target = target;
                        p.start_items.push_back(std::move(si));
                        added = true;
                    }

                    if (is_exe && norm_exe[0]) {
                        bool exists_in_cfg = false;
                        for (auto& rr : p.cfg.items[KIND_EXE]) {
                            if (_wcsicmp(rr.name.c_str(), norm_exe) == 0) {
                                exists_in_cfg = true;
                                if (rr.exe_path.empty() && exe_path_full[0]) rr.exe_path = exe_path_full;
                                break;
                            }
                        }
                        if (!exists_in_cfg) {
                            ItemRow rr;
                            rr.name = norm_exe;
                            rr.auto_stop = false;
                            if (exe_path_full[0]) rr.exe_path = exe_path_full;
                            p.cfg.items[KIND_EXE].push_back(std::move(rr));
                        }
                    }

                    if (self.active_profile == psel) start_now = true;
                }
            }

            if (added) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, lb_start_exe, psel);
                if (is_exe && norm_exe[0]) prof_fill_content_list(self, lb_exe, psel, KIND_EXE);
                refresh_for_selection();
            }

            if (start_now) {
                Action a;
                a.kind = KIND_EXE;
                a.op = ACTION_LAUNCH_ITEM;
                a.name = target;
                a.target = target;
                a.reason = L"profile active (launch item)";
                a.wait_ms = 0;
                action_enqueue(self, std::move(a));
}
            return 0;
        }

        if (id == IDC_PSTART_EXE_REMOVE) {
            int psel = prof_get_sel(lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(lb_start_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a start item first."); return 0; }

            std::wstring target = prof_get_lb_text(lb_start_exe, isel);
            target = normalize_start_target_best_effort(target);
            if (target.empty()) return 0;

            bool removed = false;
            std::vector<StartedProc> to_close;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.profiles.size()) {
                    ProfileSnapshot& p = self.profiles[(size_t)psel];
                    for (size_t i = 0; i < p.start_items.size(); i++) {
                        if (_wcsicmp(p.start_items[i].target.c_str(), target.c_str()) == 0) {
                            p.start_items.erase(p.start_items.begin() + (ptrdiff_t)i);
                            removed = true;
                            break;
                        }
                    }
                }

                // If this profile is currently active, also stop any processes we started for this target.
                if (removed && self.active_profile == psel) {
                    for (size_t i = 0; i < self.profile_started_procs.size();) {
                        if (_wcsicmp(self.profile_started_procs[i].started_target.c_str(), target.c_str()) == 0) {
                            to_close.push_back(std::move(self.profile_started_procs[i]));
                            self.profile_started_procs.erase(self.profile_started_procs.begin() + (ptrdiff_t)i);
                            continue;
                        }
                        i++;
                    }

                    // Best-effort clear UI indicator if it was an EXE.
                    if (ends_with_i(target.c_str(), L".exe")) {
                        std::wstring key = exe_watch_key_lower(target);
                        if (!key.empty()) self.profile_started_exes.erase(key);
                    }
                }
            }

            if (removed) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, lb_start_exe, psel);
                refresh_for_selection();
            }

            // Close outside the lock.
            for (auto& sp : to_close) {
                Action a;
                a.kind = KIND_EXE;
                a.op = ACTION_CLOSE_STARTED;
                a.name = sp.started_target;
                a.reason = L"profile active (close removed start-item)";
                a.wait_ms = self.stop_wait_ms;
                a.pid = sp.pid;
                if (sp.hproc) {
                    HANDLE dup = NULL;
                    DuplicateHandle(GetCurrentProcess(), sp.hproc.get(), GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
                    a.hproc = dup;
                }
                action_enqueue(self, std::move(a));
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
            theme_compute_for_window(self, dlg);
            HFONT f = theme_ensure_font_for_dpi(self, s_prof_dpi);
            if (f) {
                apply_font_recursive(dlg, f);
                if (tab_right) SendMessageW(tab_right, WM_SETFONT, (WPARAM)f, TRUE);
            }
        }
        do_layout();
        return 0;
    }
    case WM_DESTROY:        s_prof_dpi = 96;
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
    int w = dpi_scale(dpi, 980), h = dpi_scale(dpi, 720);
    // Clamp to work area so the dialog never starts off-screen
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        int workW = (int)(wa.right - wa.left);
        int workH = (int)(wa.bottom - wa.top);
        int maxW = std::max(200, workW - dpi_scale(dpi, 40));
        int maxH = std::max(200, workH - dpi_scale(dpi, 40));
        w = std::min(w, maxW);
        h = std::min(h, maxH);
    }
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Profiles",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN | WS_VSCROLL,
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

        FillRect(hdc, &rc, self.dark_mode ? self.br_panel : GetSysColorBrush(COLOR_3DFACE));

        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int cx = w / 2;

        COLORREF shadow = self.dark_mode ? RGB(18, 18, 18) : GetSysColor(COLOR_3DSHADOW);
        COLORREF hilite = self.dark_mode ? RGB(80, 80, 80) : GetSysColor(COLOR_3DHILIGHT);
        HPEN penShadow = CreatePen(PS_SOLID, 1, shadow);
        HPEN penHilite = CreatePen(PS_SOLID, 1, hilite);

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


// Horizontal splitter (for left panel top controls vs. info pane)
static LRESULT CALLBACK HSplitterProc(HWND sw, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = (App*)GetWindowLongPtrW(sw, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(sw, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZENS));
        return TRUE;
    case WM_LBUTTONDOWN: {
        if (!self) break;
        SetCapture(sw);
        // store drag start in window properties
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(sw, &pt);
        SetPropW(sw, L"SEM_DRAG", (HANDLE)(INT_PTR)1);
        SetPropW(sw, L"SEM_DRAG_Y0", (HANDLE)(INT_PTR)pt.y);

        int kind = (GetDlgCtrlID(sw) == IDC_LEFT_HSPLIT_EXE) ? KIND_EXE : KIND_SVC;
        int splitY = (kind == KIND_EXE) ? self->left_split_y_exe : self->left_split_y_svc;
        SetPropW(sw, L"SEM_DRAG_SPLIT0", (HANDLE)(INT_PTR)splitY);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self) break;
        if (GetCapture() != sw) break;
        if ((INT_PTR)GetPropW(sw, L"SEM_DRAG") != 1) break;

        int y0 = (int)(INT_PTR)GetPropW(sw, L"SEM_DRAG_Y0");
        int s0 = (int)(INT_PTR)GetPropW(sw, L"SEM_DRAG_SPLIT0");

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(sw, &pt);
        int dy = pt.y - y0;

        int kind = (GetDlgCtrlID(sw) == IDC_LEFT_HSPLIT_EXE) ? KIND_EXE : KIND_SVC;
        int newSplit = s0 + dy;

        if (kind == KIND_EXE) self->left_split_y_exe = newSplit;
        else self->left_split_y_svc = newSplit;

        // Relayout main window
        if (self->hwnd) layout(*self, self->hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (GetCapture() == sw) ReleaseCapture();
        RemovePropW(sw, L"SEM_DRAG");
        RemovePropW(sw, L"SEM_DRAG_Y0");
        RemovePropW(sw, L"SEM_DRAG_SPLIT0");
        return 0;
    case WM_CAPTURECHANGED:
        RemovePropW(sw, L"SEM_DRAG");
        RemovePropW(sw, L"SEM_DRAG_Y0");
        RemovePropW(sw, L"SEM_DRAG_SPLIT0");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(sw, &ps);
        RECT rc; GetClientRect(sw, &rc);
        // draw a subtle grip line
        int mid = (rc.top + rc.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, rc.left + 4, mid, NULL);
        LineTo(hdc, rc.right - 4, mid);
        SelectObject(hdc, old);
        DeleteObject(pen);
        EndPaint(sw, &ps);
        return 0;
    }
    }
    return DefWindowProcW(sw, msg, wParam, lParam);
}

static void register_hsplitter_class(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HSplitterProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SEM_HSPLITTER";
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_SIZENS);
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
    register_hsplitter_class();

// Top bar buttons (instead of classic menu bar)
self.btn_settings = CreateWindowExW(0, L"BUTTON", L"Settings",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
    0, 0, 0, 0, hwnd, (HMENU)IDC_TOP_SETTINGS, GetModuleHandleW(NULL), NULL);
self.btn_quit = CreateWindowExW(0, L"BUTTON", L"Quit",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
    0, 0, 0, 0, hwnd, (HMENU)IDC_TOP_QUIT, GetModuleHandleW(NULL), NULL);
self.btn_exit_to_tray = CreateWindowExW(0, L"BUTTON", L"To tray",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
    0, 0, 0, 0, hwnd, (HMENU)IDC_TOP_EXIT_TO_TRAY, GetModuleHandleW(NULL), NULL);

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

    // Bottom info pane (service description)
    self.svc_info_label = CreateWindowW(L"STATIC", L"Description", WS_CHILD | WS_VISIBLE,
        10, 220, 220, 18, self.left_page_svc, NULL, NULL, NULL);
    self.svc_info_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        10, 242, 220, 120, self.left_page_svc, NULL, NULL, NULL);

    self.svc_hsplit = CreateWindowExW(0, L"SEM_HSPLITTER", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 6, self.left_page_svc, (HMENU)IDC_LEFT_HSPLIT_SVC, GetModuleHandleW(NULL), (LPVOID)&self);

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

    // Bottom info pane (selected EXE info)
    self.exe_info_label = CreateWindowW(L"STATIC", L"Info", WS_CHILD | WS_VISIBLE,
        10, 220, 220, 18, self.left_page_exe, NULL, NULL, NULL);
    self.exe_info_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        10, 242, 220, 120, self.left_page_exe, NULL, NULL, NULL);

    self.exe_hsplit = CreateWindowExW(0, L"SEM_HSPLITTER", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 6, self.left_page_exe, (HMENU)IDC_LEFT_HSPLIT_EXE, GetModuleHandleW(NULL), (LPVOID)&self);

    // Center: listviews + search
    self.svc_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, self.center_page_svc, (HMENU)IDC_SVC_SEARCH_EDIT, GetModuleHandleW(NULL), NULL);
    self.exe_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, self.center_page_exe, (HMENU)IDC_EXE_SEARCH_EDIT, GetModuleHandleW(NULL), NULL);
    set_cue_banner(self.svc_search, L"Search services (name/status)");
    set_cue_banner(self.exe_search, L"Search EXEs (name/status)");
    SetWindowSubclass(self.svc_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);
    SetWindowSubclass(self.exe_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);



    self.lv_svc = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_svc, (HMENU)IDC_LV_SVC, GetModuleHandleW(NULL), NULL);

    self.lv_exe = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_exe, (HMENU)IDC_LV_EXE, GetModuleHandleW(NULL), NULL);

    lv_set_columns(self.lv_svc, L"Auto Stop", L"Service Name", L"Status", L"Last Update");
    lv_set_columns(self.lv_exe, L"Auto Stop", L"Executable", L"Status", L"Last Update");

    // Owner-data listviews: request state image callbacks so the checkbox (Auto Stop) renders.
    ListView_SetCallbackMask(self.lv_svc, LVIS_STATEIMAGEMASK);
    ListView_SetCallbackMask(self.lv_exe, LVIS_STATEIMAGEMASK);

// Empty-state labels (shown when list has no rows)
self.svc_empty = CreateWindowW(L"STATIC",
    L"No services are being monitored yet.\r\n\r\nUse the left panel to add a service name.",
    WS_CHILD | SS_CENTER,
    0, 0, 10, 10, self.center_page_svc, NULL, NULL, NULL);
self.exe_empty = CreateWindowW(L"STATIC",
    L"No EXEs are being monitored yet.\r\n\r\nUse the left panel to add an EXE name or browse for one.",
    WS_CHILD | SS_CENTER,
    0, 0, 10, 10, self.center_page_exe, NULL, NULL, NULL);
ShowWindow(self.svc_empty, SW_HIDE);
ShowWindow(self.exe_empty, SW_HIDE);

// Apply saved column order/widths (if present in config)
apply_saved_columns(self, KIND_SVC);
apply_saved_columns(self, KIND_EXE);


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
    // ListBox item height needs explicit help under per-monitor DPI.
    listbox_adjust_item_height(self.activity, self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd));
}

static const wchar_t* active_profile_name_locked(const App& self);

// Status bar update (UI thread)
static void update_statusbar(App& self) {
    if (!self.statusbar) return;

    size_t ns = 0, ne = 0;
    {
        ModelLockGuard lk(self);
        ns = self.items[KIND_SVC].v.size();
        ne = self.items[KIND_EXE].v.size();
    }
    std::wstring prof;
    {
        ModelLockGuard lk(self);
        prof = active_profile_name_locked(self);
    }
    if (prof.empty()) prof = L"Default";


    wchar_t ts[64];
    fmt_time_local(ts, 64, self.last_any_status_wall);

    wchar_t text[256];
    size_t actions_q = action_qdepth(self);
    const wchar_t* mon = self.monitor_thread.joinable() ? L"ON" : L"OFF";
    StringCchPrintfW(text, 256, L"Monitoring: %s | Profile: %s | Items: %zu | Last tick: %s | Actions queued: %zu", mon, prof.c_str(), (ns + ne), ts, actions_q);
    SendMessageW(self.statusbar, SB_SETTEXTW, 0, (LPARAM)text);
}

static void apply_filter_now(App& self, int kind) {
    // Read current search text, normalize, rebuild only the affected list.
    wchar_t buf[512]; buf[0] = 0;
    if (kind == KIND_SVC && self.svc_search) GetWindowTextW(self.svc_search, buf, 512);
    if (kind == KIND_EXE && self.exe_search) GetWindowTextW(self.exe_search, buf, 512);
    trim_ws_inplace(buf);
    self.filter[kind] = to_lower_ws(buf);
    rebuild_listview_filtered(self, kind);
    update_statusbar(self);

    // Update empty-state visibility (if present)
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    HWND empty = (kind == KIND_SVC) ? self.svc_empty : self.exe_empty;
    if (lv && empty) {
        int count = ListView_GetItemCount(lv);
        ShowWindow(empty, (count == 0) ? SW_SHOW : SW_HIDE);
    }
}

static void capture_columns_now(App& self, int kind) {
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    int ncol = 4; // your listviews use 4 columns (checkbox + name + status + last)
    if (ncol <= 0) return;

    for (int i = 0; i < ncol; i++) {
        self.cols_w[kind][i] = ListView_GetColumnWidth(lv, i);
    }
    int order[8]{0};
    if (ListView_GetColumnOrderArray(lv, ncol, order)) {
        for (int i = 0; i < ncol; i++) self.cols_order[kind][i] = order[i];
    } else {
        for (int i = 0; i < ncol; i++) self.cols_order[kind][i] = i;
    }
    // Force the checkbox/Auto-stop column (model column 0) to remain the left-most display column.
    {
        int pos0 = -1;
        for (int i = 0; i < ncol; i++) if (self.cols_order[kind][i] == 0) { pos0 = i; break; }
        if (pos0 > 0) {
            int tmp = self.cols_order[kind][0];
            self.cols_order[kind][0] = self.cols_order[kind][pos0];
            self.cols_order[kind][pos0] = tmp;
        }
    }
    self.cols_have[kind] = true;

    request_save_debounced(self);
}

static void apply_saved_columns(App& self, int kind) {
    if (!self.cols_have[kind]) return;
    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    int ncol = 4;
    // Order first, then widths.
    int order[8]{0};
    for (int i = 0; i < ncol; i++) order[i] = self.cols_order[kind][i];
        // Force the checkbox/Auto-stop column (model column 0) to remain the left-most display column.
    {
        int pos0 = -1;
        for (int i = 0; i < ncol; i++) if (order[i] == 0) { pos0 = i; break; }
        if (pos0 > 0) {
            int tmp = order[0];
            order[0] = order[pos0];
            order[pos0] = tmp;
        }
    }
    ListView_SetColumnOrderArray(lv, ncol, order);

    for (int i = 0; i < ncol; i++) {
        int w = self.cols_w[kind][i];
        if (w > 10) ListView_SetColumnWidth(lv, i, w);
    }
}

static LRESULT CALLBACK SearchEditSubclassProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
    App* self = (App*)refData;
    if (!self) return DefSubclassProc(edit, msg, wParam, lParam);

    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                SetWindowTextW(edit, L"");
                // Apply immediately; no debounce on Esc
                int kind = (edit == self->svc_search) ? KIND_SVC : (edit == self->exe_search) ? KIND_EXE : -1;
                if (kind != -1) apply_filter_now(*self, kind);
                return 0;
            }
            break;
    }
    return DefSubclassProc(edit, msg, wParam, lParam);
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
    int topbarH = S(26); // room for top buttons
    int tabH = S(28);

    // Top bar buttons (fit within the left panel width)
    int topBtnH = topbarH - 2;
    int yTop = 1;
    int gapBtn = S(8);
    int avail = leftW - 2*pad;
    int wSettings = S(72);
    int wQuit = S(60);
    // Give the remaining space to "To tray" so it never overlaps the center panel.
    int wTray = avail - (wSettings + gapBtn + wQuit + gapBtn);
    if (wTray < S(72)) wTray = S(72);
    // If even that doesn't fit (very narrow left panel), clamp everything proportionally.
    if (wSettings + gapBtn + wQuit + gapBtn + wTray > avail) {
        int wEach = (avail - 2*gapBtn) / 3;
        if (wEach < S(48)) wEach = S(48);
        wSettings = wQuit = wTray = wEach;
    }

    int x = pad;
    SetWindowPos(self.btn_settings, NULL, x, yTop, wSettings, topBtnH, SWP_NOZORDER);
    x += wSettings + gapBtn;
    SetWindowPos(self.btn_quit, NULL, x, yTop, wQuit, topBtnH, SWP_NOZORDER);
    x += wQuit + gapBtn;
    SetWindowPos(self.btn_exit_to_tray, NULL, x, yTop, wTray, topBtnH, SWP_NOZORDER);

    int tabsY = topbarH;
    SetWindowPos(self.left_tabs, NULL, pad, tabsY + pad, leftW - 2*pad, tabH, SWP_NOZORDER);

    int leftX = pad;
    int leftY = tabsY + pad + tabH;
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
    
        // Split left panel horizontally: top controls vs. info pane (draggable)
        int pageH = leftH;
        int topNeeded = y2 + 2*(btnH + btnGap) + btnH + inPad;

        const int splitterH = S(6);
        const int infoMinH = S(48);
        int bottomMin = inPad + labelH + S(6) + infoMinH + inPad;

        int splitY = self.left_split_y_svc;
        if (splitY <= 0) splitY = pageH / 2;
        splitY = clamp_int(splitY, topNeeded, pageH - (bottomMin + splitterH));
        self.left_split_y_svc = splitY;

        // splitter bar
        if (self.svc_hsplit) SetWindowPos(self.svc_hsplit, NULL, 0, splitY, lw, splitterH, SWP_NOZORDER);

        int infoW = std::max(S(80), lw - 2*inPad);
        int infoLabelY = splitY + splitterH + inPad;
        int infoEditY = infoLabelY + labelH + S(6);
        int infoEditH = pageH - infoEditY - inPad;
        if (infoEditH < infoMinH) infoEditH = infoMinH;

        if (self.svc_info_label) SetWindowPos(self.svc_info_label, NULL, inPad, infoLabelY, infoW, labelH, SWP_NOZORDER);
        if (self.svc_info_edit)  SetWindowPos(self.svc_info_edit,  NULL, inPad, infoEditY, infoW, infoEditH, SWP_NOZORDER);

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
    
        // Split left panel horizontally: top controls vs. info pane (draggable)
        int pageH = leftH;
        int topNeeded = y2 + 2*(btnH + btnGap) + btnH + inPad;

        const int splitterH = S(6);
        const int infoMinH = S(48);
        int bottomMin = inPad + labelH + S(6) + infoMinH + inPad;

        int splitY = self.left_split_y_exe;
        if (splitY <= 0) splitY = pageH / 2;
        splitY = clamp_int(splitY, topNeeded, pageH - (bottomMin + splitterH));
        self.left_split_y_exe = splitY;

        if (self.exe_hsplit) SetWindowPos(self.exe_hsplit, NULL, 0, splitY, lw, splitterH, SWP_NOZORDER);

        int infoW = std::max(S(80), lw - 2*inPad);
        int infoLabelY = splitY + splitterH + inPad;
        int infoEditY = infoLabelY + labelH + S(6);
        int infoEditH = pageH - infoEditY - inPad;
        if (infoEditH < infoMinH) infoEditH = infoMinH;

        if (self.exe_info_label) SetWindowPos(self.exe_info_label, NULL, inPad, infoLabelY, infoW, labelH, SWP_NOZORDER);
        if (self.exe_info_edit)  SetWindowPos(self.exe_info_edit,  NULL, inPad, infoEditY, infoW, infoEditH, SWP_NOZORDER);

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
// Empty-state labels occupy the list area (same rect as the list view)
if (self.svc_empty) SetWindowPos(self.svc_empty, NULL, cPad, lvY, std::max(S(80), cw - 2*cPad), lvH, SWP_NOZORDER);
if (self.exe_empty) SetWindowPos(self.exe_empty, NULL, cPad, lvY, std::max(S(80), cw - 2*cPad), lvH, SWP_NOZORDER);


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
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
        // Centralized registration keeps uid_map/by_name/dirty/view caches consistent.
        bool ok = register_item_locked(self, kind_from_int(kind), it);
        if (ok) {
            self.items_gen[kind]++;
        } else {
            // duplicate (or OOM) - keep model consistent
            delete it;
            it = nullptr;
        }
    }

    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    (void)lv;
    // Owner-data view: rebuild the view index (filter/sort) so rows map to stable uids.
    rebuild_listview_filtered(self, kind);

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
        ModelLockGuard lk(self);
        Item* it = list_find(&self.items[kind], name);
        if (it) {
            unregister_item_locked(self, kind_from_int(kind), it);
            self.items_gen[kind]++;
            removed = true;
        }
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
        ModelLockGuard lk(self);
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
        ModelLockGuard lk(self);
        wait_ms = self.stop_wait_ms; // reuse existing wait window
    }
    action_enqueue(self, kind, ACTION_START, name, L"manual start", wait_ms);
}

// Row checkbox toggle
static bool cmd_set_autostop_uid(App& self, uint32_t uid, bool enabled, std::wstring* out_name_opt = nullptr) {
    std::wstring name_for_log;
    bool changed = false;
    {
        ModelLockGuard lk(self);
        Item* it = uid_to_item_ptr_unlocked(self, uid);
        if (!it) return false;
        if (it->auto_stop != enabled) {
            it->auto_stop = enabled;
            changed = true;
        }
        name_for_log = it->name;
    }
    if (out_name_opt) *out_name_opt = name_for_log;
    if (changed && !name_for_log.empty()) {
        log_linef(self, L"%s: auto-stop %s", name_for_log.c_str(), enabled ? L"enabled" : L"disabled");
        post_model_dirty(self);
        request_save_debounced(self);
    }
    return changed;
}

static bool handle_main_listview_notify(App& self, HWND hwnd, NMHDR* nh, LPARAM lParam, LRESULT& out) {
    if (!nh) return false;
    if (!(nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE)) return false;

    const ItemKind kind = (nh->idFrom == IDC_LV_SVC) ? ItemKind::Svc : ItemKind::Exe;
    const int kind_i = kind_index(kind);

    if (nh->code == LVN_GETDISPINFO) {
        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
        if (!di) { out = 0; return true; }

        const int row = di->item.iItem;
        if (row < 0) { out = 0; return true; }

        uint32_t uid = 0;
        {
            ModelLockGuard lk(self);
            if (row < (int)self.view_uids[kind_i].size()) uid = self.view_uids[kind_i][row];
        }
        if (!uid) { out = 0; return true; }

        Item* itp = nullptr;
        {
            ModelLockGuard lk(self);
            auto f = self.uid_map.find(uid);
            if (f != self.uid_map.end()) itp = f->second;
        }
        if (!itp) { out = 0; return true; }

        // Provide stable lParam for notifications (uid identity)
        if (di->item.mask & LVIF_PARAM) di->item.lParam = (LPARAM)uid;

        // Checkbox state (Auto Stop) lives in the state image.
        if (di->item.mask & LVIF_STATE) {
            di->item.stateMask = LVIS_STATEIMAGEMASK;
            di->item.state = INDEXTOSTATEIMAGEMASK(itp->auto_stop ? 2 : 1);
        }

        if ((di->item.mask & LVIF_TEXT) && di->item.pszText) {
            // Provide text pointers that remain valid after returning.
            
// Ensure UI caches are populated even if this row hasn't been marked "dirty" yet.
// With owner-data ListView, rows will request text on demand; do not rely on dirty-only updates
// to seed Status/Last Update for items that never change.
if (itp->ui_cache_status_disp[0] == 0 || itp->ui_cache_last_text[0] == 0) {
    ModelLockGuard lk(self);
    if (itp->ui_cache_status_disp[0] == 0) {
        if (itp->last_status[0]) StringCchCopyW(itp->ui_cache_status_disp, 64, itp->last_status);
        else itp->ui_cache_status_disp[0] = 0;
    }
    if (itp->ui_cache_last_text[0] == 0) {
        if (itp->last_update_wall) fmt_time_local(itp->ui_cache_last_text, 64, itp->last_update_wall);
        else itp->ui_cache_last_text[0] = 0;
    }
}

switch (di->item.iSubItem) {
                case 0: di->item.pszText = (LPWSTR)L""; break; // checkbox-only column
                case 1: di->item.pszText = (LPWSTR)itp->name.c_str(); break;
                case 2: di->item.pszText = (LPWSTR)itp->ui_cache_status_disp; break;
                case 3: di->item.pszText = (LPWSTR)itp->ui_cache_last_text; break;
                default: di->item.pszText = (LPWSTR)L""; break;
            }
        }

        out = 0;
        return true;
    }


    if (nh->code == LVN_ITEMCHANGED) {
        if (self.suppress_lv_notify) { out = 0; return true; }
        NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
        if (lv && (lv->uChanged & LVIF_STATE)) {
            
// Checkbox toggle (auto-stop)
            if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) {
                uint32_t uid = 0;
                {
                    ModelLockGuard lk(self);
                    if (lv->iItem >= 0 && lv->iItem < (int)self.view_uids[kind_i].size()) uid = self.view_uids[kind_i][lv->iItem];
                }
                
                const bool enabled = (ListView_GetCheckState(nh->hwndFrom, lv->iItem) != 0);
                (void)cmd_set_autostop_uid(self, uid, enabled);
            }
            // Selection change: update left info pane
            if ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED) {
                Item* sel = lv_get_selected_item_ptr(self, kind);
                update_left_info_pane(self, kind, sel);
            }
        }
        out = 0;
        return true;
    }

    // Double-click / Enter opens a quick details dialog
    if (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE) {
        show_item_details(self, kind_i);
        out = 0;
        return true;
    }

    if (nh->code == LVN_KEYDOWN) {
        NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
        if (!kd) return false;

        if ((GetKeyState(VK_CONTROL) & 0x8000) && kd->wVKey == 'C') {
            Item* it = lv_get_selected_item_ptr(self, kind);
            if (it) clipboard_set_text(hwnd, it->name.c_str());
            out = 0;
            return true;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && kd->wVKey == 'F') {
            HWND edit = (kind == ItemKind::Svc) ? self.svc_search : self.exe_search;
            if (edit) {
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            out = 0;
            return true;
        }
    }

    return false;
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
static void ui_timer_suspend(App& self, HWND hwnd) {
    if (self.ui_timer_suspended) return;
    self.ui_timer_suspended = true;
    KillTimer(hwnd, TIMER_UI_REFRESH);
}

static UINT ui_refresh_fallback_interval_ms(const App& self) {
    // The UI is primarily event-driven (WM_APP_MODEL_DIRTY). This timer exists only as a
    // safety net and should be *slow* to avoid needless wakeups.
    int ms = self.ui_refresh_ms;
    if (ms < 1000) ms = 1000;
    if (ms > 2000) ms = 2000;
    return (UINT)ms;
}

static void ui_timer_resume(App& self, HWND hwnd) {
    if (!self.ui_timer_suspended) return;
    self.ui_timer_suspended = false;
    // Resume with a *slow* fallback interval (event-driven updates do the real work).
    SetTimer(hwnd, TIMER_UI_REFRESH, ui_refresh_fallback_interval_ms(self), NULL);
}

static void post_model_dirty(App& self) {
    // Coalesce: many status changes can arrive quickly; one maintenance pass is enough.
    if (self.ui_dirty_posted) return;
    self.ui_dirty_posted = true;
    PostMessageW(self.hwnd, WM_APP_MODEL_DIRTY, 0, 0);
}

static void restart_ui_timer(App& self, HWND hwnd) {
    // Only run the UI timer when the main window is actually visible and not suspended.
    if (self.ui_timer_suspended) { KillTimer(hwnd, TIMER_UI_REFRESH); return; }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) { KillTimer(hwnd, TIMER_UI_REFRESH); return; }

    KillTimer(hwnd, TIMER_UI_REFRESH);
    SetTimer(hwnd, TIMER_UI_REFRESH, ui_refresh_fallback_interval_ms(self), NULL);
}

static void ui_refresh_if_needed(App& self, HWND hwnd) {
    // Avoid heavy UI churn when hidden/minimized/tray.
    if (self.ui_timer_suspended || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
    // Apply theme only when it changes (decoupled from status updates).
    uint32_t tg = self.theme_gen.load(std::memory_order_relaxed);
    if (self.ui_seen_theme_gen != tg) {
        theme_apply_all_controls(self);
        self.ui_seen_theme_gen = tg;
        InvalidateRect(hwnd, NULL, TRUE);
    }

    // Layout/column maintenance (triggered when we know something changed).
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
}



// --------------------------------------------------
// Main window proc (final-form instance binding)
// Stores `App*` in GWLP_USERDATA and forwards to AppWndProc(self,...).
// --------------------------------------------------
static LRESULT AppWndProc(App& self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void schedule_initial_layout(App& self, HWND hwnd) {
    if (self.did_initial_layout) return;
    if (self.initial_layout_scheduled) return;
    self.initial_layout_scheduled = true;
    PostMessageW(hwnd, WM_APP_INITIAL_LAYOUT, 0, 0);
}

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

        // Capture initial DPI for this window.
        self.dpi = get_dpi_for_hwnd(hwnd);
        if (self.dpi == 0) self.dpi = 96;

        // Compute theme + DPI-dependent resources (font/imagelists/brushes) in one place.
        theme_compute(self);

        // Optional: modern backdrop (harmless if unsupported)
        try_enable_mica_backdrop(hwnd);

        build_ui(self, hwnd);

        // Config is loaded before window creation (so dark-mode opt-in is early).
        theme_apply_all_controls(self);

        // Build Settings popup menu (we use a custom top bar instead of a classic menu bar
// because classic menu bars don't reliably support dark mode).
self.menu_settings = CreatePopupMenu();
AppendMenuW(self.menu_settings, MF_STRING, IDM_PREFS, L"Preferences…");
AppendMenuW(self.menu_settings, MF_STRING, IDM_PROFILES, L"Profiles…");
AppendMenuW(self.menu_settings, MF_STRING | (self.dark_mode ? MF_CHECKED : 0), IDM_DARKMODE, L"Dark mode");
AppendMenuW(self.menu_settings, MF_STRING, IDM_HOTKEYS, L"Hotkeys…");
    AppendMenuW(self.menu_settings, MF_STRING, IDM_DIAGNOSTICS, L"Diagnostics…");
AppendMenuW(self.menu_settings, MF_SEPARATOR, 0, NULL);
AppendMenuW(self.menu_settings, MF_STRING, IDM_EXIT_TO_TRAY, L"Exit to tray");
AppendMenuW(self.menu_settings, MF_STRING, IDM_EXIT, L"Quit");
// Register for Explorer restart message so we can re-add the tray icon
self.taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");



        // Allow Explorer to deliver tray callback messages to an elevated window
        allow_uipi_message(hwnd, WM_APP_TRAYICON);
        if (self.taskbar_restart_msg) allow_uipi_message(hwnd, self.taskbar_restart_msg);

        if (!enum_services_build_disp_cache(&self.disp_cache)) {
            log_linef(self, L"Display cache: enumerate failed");
        }

        // Start UI timer (may be restarted by Preferences)
        restart_ui_timer(self, hwnd);

        // Apply tray icon setting from config
        tray_sync(self, GetModuleHandleW(NULL));


        // Populate listviews from config/model (decoupled from status updates)
        rebuild_listview_filtered(self, KIND_SVC);
        rebuild_listview_filtered(self, KIND_EXE);
        self.lv_needs_layout[KIND_SVC] = true;
        self.lv_needs_layout[KIND_EXE] = true;

        // Ensure we always perform a "final" layout pass after the window is shown
        // and has a non-trivial client area. This avoids the "tiny box until resize"
        // issue on some systems/launch paths.
        schedule_initial_layout(self, hwnd);


        // Start background workers (C++20 std::jthread: auto-join + cooperative stop).
        self.action_thread = std::jthread(action_thread_main, &self);
        self.monitor_thread = std::jthread(monitor_thread_main, &self);

        update_statusbar(self);
        return 0;
    }

    case WM_SHOWWINDOW:
        // Backstop: ensure initial layout happens after the window becomes visible.
        if (wParam && !self.did_initial_layout) {
            schedule_initial_layout(self, hwnd);
        }
        return 0;

    case WM_WINDOWPOSCHANGED: {
        // Backstop: some restore paths adjust the window size/pos without a clean WM_SIZE
        // at a time when all children are ready. If size changed and we haven't done the
        // stabilized initial layout yet, schedule it.
        if (!self.did_initial_layout) {
            const WINDOWPOS* wp = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (wp && !(wp->flags & SWP_NOSIZE)) {
                schedule_initial_layout(self, hwnd);
            }
        }
        return 0;
    }

    case WM_APP_INITIAL_LAYOUT: {
        // Deferred one-time layout pass once we have a real client size.
        // We may get called very early; keep re-posting a few times until the client
        // rect is non-trivial.
        self.initial_layout_scheduled = false;

        if (self.did_initial_layout) return 0;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        if (cw <= 1 || ch <= 1) {
            if (self.initial_layout_tries++ < 25) {
                schedule_initial_layout(self, hwnd);
            }
            return 0;
        }

        // Re-sync DPI one more time (startup can be odd across monitors / scaling transitions).
        UINT dpi_now = get_dpi_for_hwnd(hwnd);
        if (dpi_now == 0) dpi_now = 96;
        if (self.dpi == 0) self.dpi = dpi_now;

        if (dpi_now != self.dpi) {
            UINT oldDpi = self.dpi;
            UINT newDpi = dpi_now;
            self.dpi = newDpi;

            // Keep the Activity panel width feeling consistent across DPI changes
            if (self.activity_panel_w > 0 && oldDpi && newDpi && oldDpi != newDpi) {
                self.activity_panel_w = MulDiv(self.activity_panel_w, (int)newDpi, (int)oldDpi);
            }

            // Rebuild DPI-dependent theme resources once, then apply them.
            theme_compute(self);
            theme_apply_all_controls(self);

            // Keep ListView columns feeling consistent across DPI changes
            lv_scale_columns(self.lv_svc, oldDpi, newDpi);
            lv_scale_columns(self.lv_exe, oldDpi, newDpi);
            self.lv_needs_layout[KIND_SVC] = true;
            self.lv_needs_layout[KIND_EXE] = true;

            // ListBox item height needs explicit help under per-monitor DPI
            listbox_adjust_item_height(self.activity, newDpi);
        }

        // Final layout + paint
        layout(self, hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateWindow(hwnd);

        self.did_initial_layout = true;
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

        // Rebuild DPI-dependent theme resources once, then apply them.
        theme_compute(self);
        theme_apply_all_controls(self);

        // Keep ListView columns feeling consistent across DPI changes
        lv_scale_columns(self.lv_svc, oldDpi, newDpi);
        lv_scale_columns(self.lv_exe, oldDpi, newDpi);
        self.lv_needs_layout[KIND_SVC] = true;
        self.lv_needs_layout[KIND_EXE] = true;

        // ListBox item height needs explicit help under per-monitor DPI
        listbox_adjust_item_height(self.activity, newDpi);

        // Layout with the new scaling
        layout(self, hwnd);
        return 0;
    }


    case WM_ENTERSIZEMOVE:
        // During interactive resizing, some systems deliver fewer WM_SIZE messages.
        // Run a lightweight layout tick so child panels track the drag smoothly.
        self.in_size_move = true;
        SetTimer(hwnd, TIMER_LIVE_RESIZE, 16, NULL); // ~60fps; work is just SetWindowPos
        return 0;

    case WM_EXITSIZEMOVE:
        self.in_size_move = false;
        KillTimer(hwnd, TIMER_LIVE_RESIZE);
        // Final layout + one maintenance pass (autosize/status) after drag ends.
        layout(self, hwnd);
        post_model_dirty(self);
        restart_ui_timer(self, hwnd);
        return 0;

    case WM_SIZE:
        // IMPORTANT: When minimized, Windows sends WM_SIZE with a 0x0 client area.
        // If we run our layout logic here, we clamp the right "Activity" panel
        // to a tiny width and permanently overwrite the user's splitter size.
        // That shows up as the internal panels "reset" when you restore the window.
        if (wParam == SIZE_MINIMIZED) {
            ui_timer_suspend(self, hwnd);
            self.in_size_move = false;
            KillTimer(hwnd, TIMER_LIVE_RESIZE);
            return 0;
        }

        // Restored/maximized: resume UI maintenance.
        if (self.ui_timer_suspended) ui_timer_resume(self, hwnd);

        layout(self, hwnd);
        post_model_dirty(self);
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

// Persist column widths/order when user drags headers and provide header context menu
        if (nh && (nh->code == HDN_ENDTRACKW || nh->code == HDN_ENDDRAG)) {
            if (self.lv_svc && nh->hwndFrom == ListView_GetHeader(self.lv_svc)) {
                capture_columns_now(self, KIND_SVC);
                return 0;
            }
            if (self.lv_exe && nh->hwndFrom == ListView_GetHeader(self.lv_exe)) {
                capture_columns_now(self, KIND_EXE);
                return 0;
            }
        }
        if (nh && nh->code == NM_RCLICK) {
            HWND hdr_s = self.lv_svc ? ListView_GetHeader(self.lv_svc) : NULL;
            HWND hdr_e = self.lv_exe ? ListView_GetHeader(self.lv_exe) : NULL;
            int kind = -1;
            if (hdr_s && nh->hwndFrom == hdr_s) kind = KIND_SVC;
            if (hdr_e && nh->hwndFrom == hdr_e) kind = KIND_EXE;
            if (kind != -1) {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, IDM_RESET_COLUMNS, L"Reset columns");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                return 0;
            }
        }
                {
            LRESULT handled = 0;
            if (handle_main_listview_notify(self, hwnd, nh, lParam, handled)) return handled;
        }



// Dark-mode custom draw for tabs + headers (some builds ignore DarkMode_Explorer for these)
if (nh && nh->code == NM_CUSTOMDRAW) {
    wchar_t ccls[64]{0};
    GetClassNameW(nh->hwndFrom, ccls, 63);
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
    if (lstrcmpiW(ccls, WC_TABCONTROLW) == 0) {
        return theme_customdraw_tab(self, cd);
    }
    if (lstrcmpiW(ccls, WC_HEADER) == 0) {
        return theme_customdraw_header(self, cd);
    }
    if (lstrcmpiW(ccls, STATUSCLASSNAMEW) == 0) {
        // Status bar: enforce themed text/background. STATUSCLASSNAME sends NM_CUSTOMDRAW.
        // NMCUSTOMDRAW does not have clrText/clrTextBk fields, so set colors on the HDC.
        if (cd->dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
        }
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            SetTextColor(cd->hdc, self.col_text);
            SetBkColor(cd->hdc, self.col_panel);
            return CDRF_NEWFONT;
        }
    }
    if (lstrcmpiW(ccls, WC_LISTVIEWW) == 0) {
        return theme_customdraw_listview(self, (NMLVCUSTOMDRAW*)cd);
    }
}


        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, self.br_bg);
        return 1;
    }
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (!mis) break;
        if (self.dark_mode && (mis->CtlType == ODT_MENU || mis->CtlType == ODT_TAB)) {
            HDC hdc = GetDC(hwnd);
            HFONT oldf = NULL;
            if (hdc && self.ui_font) oldf = (HFONT)SelectObject(hdc, self.ui_font);
            SIZE sz{0,0};
            wchar_t txt[256]{0};

            if (mis->CtlType == ODT_MENU && mis->itemID) {
                // itemData is not used; query the current menu text by command id
                HMENU mb = GetMenu(hwnd);
                if (mb) GetMenuStringW(mb, mis->itemID, txt, 255, MF_BYCOMMAND);
                if (txt[0] && hdc) GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &sz);
                mis->itemHeight = (UINT)std::max(18, dpi_scale(self.dpi, 22));
                mis->itemWidth  = (UINT)std::max(40, (int)sz.cx + dpi_scale(self.dpi, 24));
            } else if (mis->CtlType == ODT_TAB) {
                mis->itemHeight = (UINT)std::max(18, dpi_scale(self.dpi, 22));
            }

            if (hdc && oldf) SelectObject(hdc, oldf);
            if (hdc) ReleaseDC(hwnd, hdc);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (theme_draw_owner_button(self, dis)) return TRUE;
        if (theme_draw_owner_tab(self, dis)) return TRUE;
        if (theme_draw_owner_menu(self, dis)) return TRUE;
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Top bar buttons (custom menu instead of classic menubar)
        if (id == IDC_TOP_SETTINGS && code == BN_CLICKED) {
            if (self.menu_settings && self.btn_settings) {
                // Keep checkmark in sync
                UINT f = self.dark_mode ? MF_CHECKED : MF_UNCHECKED;
                CheckMenuItem(self.menu_settings, IDM_DARKMODE, MF_BYCOMMAND | f);

                RECT r{};
                GetWindowRect(self.btn_settings, &r);
                int cmd = TrackPopupMenuEx(self.menu_settings,
                    TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                    r.left, r.bottom, hwnd, NULL);
                if (cmd) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
            }
            return 0;
        }
        if (id == IDC_TOP_QUIT && code == BN_CLICKED) {
            // Always full quit, regardless of close-to-tray preference.
            app_request_exit(self, hwnd);
            return 0;
        }
        if (id == IDC_TOP_EXIT_TO_TRAY && code == BN_CLICKED) {
            // Ensure tray icon is enabled, then hide to tray.
            if (!self.tray_enabled) {
                self.tray_enabled = true;
                tray_sync(self, GetModuleHandleW(NULL));
            }
            hide_main_window(self);
            return 0;
        }


        // Live filtering (search boxes) — debounced so typing doesn’t rebuild on every keystroke
        if ((id == IDC_SVC_SEARCH_EDIT || id == IDC_EXE_SEARCH_EDIT) && code == EN_CHANGE) {
            int kind = (id == IDC_SVC_SEARCH_EDIT) ? KIND_SVC : KIND_EXE;
            self.filter_pending[kind] = true;
            self.filter_debounce_last_ms = GetTickCount();
            SetTimer(hwnd, TIMER_SEARCH_DEBOUNCE, 120, NULL);
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

        if (id == IDM_RESET_COLUMNS) {
    int sel = 0;
    if (self.center_tabs) {
        sel = (int)SendMessageW(self.center_tabs, TCM_GETCURSEL, 0, 0);
        if (sel < 0) sel = 0;
    }
    int kind = (sel == 0) ? KIND_SVC : KIND_EXE;
    self.cols_have[kind] = false;

    auto S = [&](int v96) -> int { return dpi_scale(self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd), v96); };

    HWND lv = (kind == KIND_SVC) ? self.lv_svc : self.lv_exe;
    if (lv) {
        ListView_SetColumnWidth(lv, 0, S(40));
        ListView_SetColumnWidth(lv, 1, S(260));
        ListView_SetColumnWidth(lv, 2, S(180));
        ListView_SetColumnWidth(lv, 3, S(220));
        int order[4] = {0,1,2,3};
        ListView_SetColumnOrderArray(lv, 4, order);
        capture_columns_now(self, kind);
    }
    return 0;
}

if (id == IDM_PREFS) { open_prefs_dialog(self, hwnd); return 0; }
        if (id == IDM_PROFILES) { open_profiles_dialog(self, hwnd); return 0; }
        if (id == IDM_HOTKEYS) { open_hotkeys_dialog(self, hwnd); return 0; }
        if (id == IDM_DIAGNOSTICS) { open_diagnostics_dialog(self, hwnd); return 0; }
                if (id == IDM_FOCUS_SEARCH) {
            int sel = 0;
            if (self.center_tabs) {
                sel = (int)SendMessageW(self.center_tabs, TCM_GETCURSEL, 0, 0);
                if (sel < 0) sel = 0;
            }
            HWND edit = (sel == 0) ? self.svc_search : self.exe_search;
            if (edit) {
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        if (id == IDM_TAB_SVC) {
            if (self.center_tabs) {
                TabCtrl_SetCurSel(self.center_tabs, 0);
                sync_tabs_to(self, 0);
            }
            return 0;
        }
        if (id == IDM_TAB_EXE) {
            if (self.center_tabs) {
                TabCtrl_SetCurSel(self.center_tabs, 1);
                sync_tabs_to(self, 1);
            }
            return 0;
        }
        if (id == IDM_SAVE_CONFIG_NOW) {
            save_config_now(self);
            return 0;
        }
        if (id == IDM_FOCUS_MAIN_LIST) {
            int sel = 0;
            if (self.center_tabs) {
                sel = (int)SendMessageW(self.center_tabs, TCM_GETCURSEL, 0, 0);
                if (sel < 0) sel = 0;
            }
            HWND lv = (sel == 0) ? self.lv_svc : self.lv_exe;
            if (lv) SetFocus(lv);
            return 0;
        }
        if (id == IDM_REFRESH_UI) {
            // Force a UI resync regardless of gen counters.
            self.ui_seen_status_gen[KIND_SVC] = 0;
            self.ui_seen_status_gen[KIND_EXE] = 0;
            rebuild_listview_filtered(self, KIND_SVC);
            rebuild_listview_filtered(self, KIND_EXE);
            PostMessageW(hwnd, WM_APP_MODEL_DIRTY, 0, 0);
            return 0;
        }

if (id == IDM_HELP)  { open_help_dialog(self, hwnd);  return 0; }
        if (id == IDM_DARKMODE || id == IDM_TRAY_DARKMODE) {
            self.dark_mode = !self.dark_mode;
            darkmode_set_app(self.dark_mode);
            theme_compute(self);
            theme_apply_all_controls(self);
            self.ui_seen_theme_gen = self.theme_gen.load(std::memory_order_relaxed);

            HMENU mb = GetMenu(hwnd);
            if (mb) {
                CheckMenuItem(mb, IDM_DARKMODE, MF_BYCOMMAND | (self.dark_mode ? MF_CHECKED : MF_UNCHECKED));
                DrawMenuBar(hwnd);
            }
            request_save_debounced(self);
            return 0;
        }
        if (id == IDM_EXIT_TO_TRAY) {
            // Ensure tray icon is enabled, then hide the window.
            if (!self.tray_enabled) {
                self.tray_enabled = true;
                tray_sync(self, GetModuleHandleW(NULL));
            }
            hide_main_window(self);
            return 0;
        }
        if (id == IDM_EXIT)  { app_request_exit(self, hwnd); return 0; }


        if (id == IDM_TRAY_SHOWHIDE) {
            // From the tray menu, "Show window" always brings the window up and focuses it.
            show_main_window(self);
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

    case WM_APP_STATUS_GEN: {
        int kind = (int)wParam;
        uint32_t gen = (uint32_t)(uintptr_t)lParam;
        if ((unsigned)kind < 2) {
            if (self.ui_seen_status_gen[kind] == gen) return 0;
            self.ui_seen_status_gen[kind] = gen;
        }
        ui_apply_status_updates(self, kind);
        post_model_dirty(self);
        return 0;
    }

    case WM_APP_STATUS: {
        StatusMsg* m = (StatusMsg*)lParam;
        if (m && m->name) {
            Item* it = nullptr;
            bool changed = false;
            {
                ModelLockGuard lk(self);
                it = list_find(&self.items[m->kind], m->name);
                if (it) {
                    self.last_any_status_wall = m->wall_ts;
                    if (wcscmp(it->last_status, m->status) != 0) {
                        StringCchCopyW(it->last_status, 32, m->status);
                        it->last_update_wall = m->wall_ts;
                        it->status_gen++;
                        self.status_gen[m->kind]++;
                        self.dirty_status[m->kind].push_back(it);
                        changed = true;
                    }
                }
            }

            if (it && changed) {
                bool ins = lv_set_row_with(self, m->kind == KIND_SVC ? self.lv_svc : self.lv_exe,
                                           it, m->status, m->wall_ts);
                if (ins) self.lv_needs_layout[m->kind] = true;
                post_model_dirty(self);
            }

            free(m->name);
            free(m);
        }
        return 0;
    }

    case WM_APP_MODEL_DIRTY: {
        // Clear coalesce flag early so nested causes can schedule again if needed.
        self.ui_dirty_posted = false;

        // If the window isn't visible (tray/minimized) or UI updates are suspended,
        // skip expensive UI work. We'll refresh on restore.
        if (self.ui_timer_suspended || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            return 0;
        }

        ui_refresh_if_needed(self, hwnd);
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
if (wParam == TIMER_SEARCH_DEBOUNCE) {
    KillTimer(hwnd, TIMER_SEARCH_DEBOUNCE);
    if (self.filter_pending[KIND_SVC]) { self.filter_pending[KIND_SVC] = false; apply_filter_now(self, KIND_SVC); }
    if (self.filter_pending[KIND_EXE]) { self.filter_pending[KIND_EXE] = false; apply_filter_now(self, KIND_EXE); }
    return 0;
}
        if (wParam == TIMER_DEBOUNCE_SAVE) {
            KillTimer(hwnd, TIMER_DEBOUNCE_SAVE);
            self.save_pending = false;
            save_config_now(self);
            return 0;
        }
        if (wParam == TIMER_UI_REFRESH) {
            // Fallback maintenance tick (kept for safety, but mostly event-driven).
            post_model_dirty(self);
            return 0;
        }
        if (wParam == TIMER_LIVE_RESIZE) {
            if (self.in_size_move) {
                layout(self, hwnd);
            }
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
            if (kind == KIND_SVC) {
                AppendMenuW(m, MF_STRING, IDM_CTX_SET_START_DISABLED, L"Set start type: Disabled");
                AppendMenuW(m, MF_STRING, IDM_CTX_SET_START_AUTO, L"Set start type: Automatic");
                AppendMenuW(m, MF_STRING, IDM_CTX_SET_START_MANUAL, L"Set start type: Manual");
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuW(m, MF_STRING, IDM_CTX_COPY_NAME, L"Copy name");
            if (kind == KIND_EXE) AppendMenuW(m, MF_STRING, IDM_CTX_OPEN_LOC, L"Open file location");

            SetForegroundWindow(hwnd); // menu dismiss behavior
            UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);

            switch (cmd) {
            case IDM_CTX_STOP:        stop_selected(self, kind); break;
            case IDM_CTX_REMOVE:      remove_selected(self, kind); break;
            case IDM_CTX_TOGGLE_AUTO: toggle_autostop_selected(self, kind); break;
            case IDM_CTX_SET_START_DISABLED: if (kind==KIND_SVC) set_service_start_type_selected(self, SERVICE_DISABLED); break;
            case IDM_CTX_SET_START_AUTO:     if (kind==KIND_SVC) set_service_start_type_selected(self, SERVICE_AUTO_START); break;
            case IDM_CTX_SET_START_MANUAL:   if (kind==KIND_SVC) set_service_start_type_selected(self, SERVICE_DEMAND_START); break;
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
        //   lParam = mouse/key notification (WM_* / NIN_*)
        // NOTE: with NOTIFYICON_VERSION_4, Windows may pack additional info into HIWORD(lParam).
        // We only care about the actual event code in LOWORD(lParam).
        const UINT ev = (UINT)LOWORD(lParam);
        switch (ev) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case NIN_BALLOONUSERCLICK:
            show_main_window(self);
            return 0;

        case WM_RBUTTONDOWN:
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

        action_clear(self);

        dispcache_free(&self.disp_cache);
        {
            ModelLockGuard lk(self);
            unregister_all_items_locked(self, ItemKind::Svc);
            unregister_all_items_locked(self, ItemKind::Exe);
        }

for (auto& kv : self.ui_font_cache) { if (kv.second) DeleteObject(kv.second); }
        self.ui_font_cache.clear();

        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
            self.ui_font = NULL;
        }

        theme_release_resources(self);

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

    // Load config EARLY so dark-mode opt-in can happen before any common controls are created.
    compute_default_cfg_path(self.cfg_path, MAX_PATH);

    list_init(&self.items[KIND_SVC]);
    list_init(&self.items[KIND_EXE]);


    self.items_gen[KIND_SVC] = 1;
    self.items_gen[KIND_EXE] = 1;
    self.ui_refresh_ms = 1000;
    self.monitor_interval_ms = 1000;
    self.autostop_cooldown_ms = 15000;
    self.stop_wait_ms = 10000;
    // Default persisted sizes (may be overridden by config)
    // self.activity_panel_w default is set before load_config (and may be overridden)
    self.left_split_y_svc = 260;
    self.left_split_y_exe = 260;
    self.have_win_rect = false;
    self.win_x = CW_USEDEFAULT;
    self.win_y = CW_USEDEFAULT;
    self.win_w = 1100;
    self.win_h = 600;
    self.win_maximized = false;



    self.active_profile = -1;
    self.have_default_cfg = false;
    self.profile_watch_keys_sp = std::make_shared<std::vector<std::vector<std::wstring>>>();
    self.profile_watch_hashes_sp = std::make_shared<std::vector<std::vector<uint64_t>>>();

self.tray_enabled = true;   // show tray icon by default
self.close_to_tray = false; // X closes normally unless enabled
self.tray_added = false;
self.force_quit = false;
self.taskbar_restart_msg = 0;

    // Startup layout stabilization
    self.did_initial_layout = false;
    self.initial_layout_scheduled = false;
    self.initial_layout_tries = 0;

    // Apply user config now (before creating common controls), so dark mode can fully theme tabs/listviews.
    load_config(self);
    darkmode_set_app(self.dark_mode);

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);

    // self.activity_panel_w default is set before load_config (and may be overridden)
    self.last_any_status_wall = 0;


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

    int start_x = CW_USEDEFAULT;
    int start_y = CW_USEDEFAULT;
    int start_w = 1100;
    int start_h = 600;
    if (self.have_win_rect) {
        start_x = self.win_x;
        start_y = self.win_y;
        start_w = self.win_w;
        start_h = self.win_h;
    }

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, self.app_title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        start_x, start_y, start_w, start_h,
        NULL, NULL, hInst, &app_instance
    );
    if (!hwnd) return 0;

    // Global accelerators (work anywhere in app)
    ACCEL accels[7] = {};
    // Ctrl+F -> focus search
    accels[0].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[0].key   = (WORD)'F';
    accels[0].cmd   = IDM_FOCUS_SEARCH;

    // Ctrl+1 / Ctrl+2 -> switch tabs
    accels[1].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[1].key   = (WORD)'1';
    accels[1].cmd   = IDM_TAB_SVC;

    accels[2].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[2].key   = (WORD)'2';
    accels[2].cmd   = IDM_TAB_EXE;

    // Ctrl+S -> save config now
    accels[3].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[3].key   = (WORD)'S';
    accels[3].cmd   = IDM_SAVE_CONFIG_NOW;

    // Ctrl+L -> focus main list
    accels[4].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[4].key   = (WORD)'L';
    accels[4].cmd   = IDM_FOCUS_MAIN_LIST;

    // F5 -> force refresh
    accels[5].fVirt = (BYTE)FVIRTKEY;
    accels[5].key   = (WORD)VK_F5;
    accels[5].cmd   = IDM_REFRESH_UI;

    // Ctrl+D -> toggle dark mode
    accels[6].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[6].key   = (WORD)'D';
    accels[6].cmd   = IDM_DARKMODE;

    app_instance.accel = CreateAcceleratorTableW(accels, 7);

    int nshow = show;
    if (self.have_win_rect && self.win_maximized) nshow = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, nshow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (app_instance.accel && TranslateAcceleratorW(hwnd, app_instance.accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (app_instance.accel) DestroyAcceleratorTable(app_instance.accel);
    return 0;
}