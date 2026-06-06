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
//       -lcomctl32 -ladvapi32 -lshell32 -lgdi32 -luser32 -luxtheme -ldwmapi -lshlwapi
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
#include <strsafe.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <unordered_map>
#include <new>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <chrono>

#include <cstdint>
#include <cstddef>
// <stdbool.h> not needed in C++
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <cstring>   // memcpy

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

    // NOTE: On MinGW/GCC, GetProcAddress returns FARPROC (generic func pointer).
    // Casting FARPROC to a more specific function pointer can trigger -Wcast-function-type,
    // and many builds treat warnings as errors. We locally suppress that warning here.
    FARPROC fp = GetProcAddress(user32, "ChangeWindowMessageFilterEx");
    if (fp) {
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
        PFN_CWMFE pEx = (PFN_CWMFE)fp;
#if defined(__clang__)
#   if __has_warning("-Wcast-function-type")
#       pragma clang diagnostic pop
#   endif
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#   pragma GCC diagnostic pop
#endif

        (void)pEx(hwnd, msg, MSGFLT_ALLOW, NULL);
        return;
    }

    fp = GetProcAddress(user32, "ChangeWindowMessageFilter");
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
#define TIMER_DEBOUNCE_SAVE  102
#define TIMER_UI_REFRESH     103

#define MAX_LOG_LINES 1000

#define LOAD_PROC(module, procName, Type, outVar)            \
    do {                                                     \
        (outVar) = NULL;                                     \
        FARPROC __fp = GetProcAddress((module), (procName)); \
        if (__fp) memcpy(&(outVar), &__fp, sizeof(outVar));  \
    } while (0)

// Menu IDs
#define IDM_PREFS  40001
#define IDM_HELP   40002
#define IDM_EXIT   40003

// Tray menu / commands
#define IDM_TRAY_SHOWHIDE      40004
#define IDM_TRAY_PREFS         40005
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
#define IDC_SVC_REMOVE_BTN   5104

#define IDC_EXE_ADD_EDIT     5201
#define IDC_EXE_ADD_BTN      5202
#define IDC_EXE_STOP_BTN     5203
#define IDC_EXE_REMOVE_BTN   5204

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

// Tray forward declarations
static void tray_remove(void);
static void tray_add(HINSTANCE hInst);
static void tray_sync(HINSTANCE hInst);
static void show_main_window(void);
static void hide_main_window(void);
static void app_request_exit(HWND hwnd);
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

static HFONT create_ui_font(void) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

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
struct Action {
    int kind = 0;
    std::wstring name;
    std::wstring reason;
    int wait_ms = 0;
};

// ----------------------------
// App State
// ----------------------------
typedef struct App {
    HWND hwnd;

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

static App g_app;

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

static void tray_remove(void) {
    if (g_app.tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.nid);
        g_app.tray_added = false;
        ZeroMemory(&g_app.nid, sizeof(g_app.nid));
    }
}

static void tray_add(HINSTANCE hInst) {
    if (!g_app.hwnd) return;
    if (!g_app.tray_enabled) return;

    ZeroMemory(&g_app.nid, sizeof(g_app.nid));
    g_app.nid.cbSize = sizeof(g_app.nid);
    g_app.nid.hWnd = g_app.hwnd;
    g_app.nid.uID = TRAY_UID;
    g_app.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_APP_TRAYICON;

    HICON ico = load_app_icon(hInst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    g_app.nid.hIcon = ico ? ico : LoadIconW(NULL, IDI_APPLICATION);
    StringCchCopyW(g_app.nid.szTip, ARRAYSIZE(g_app.nid.szTip), g_app.app_title.c_str());

    if (Shell_NotifyIconW(NIM_ADD, &g_app.nid)) {
        g_app.tray_added = true;
        g_app.nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_app.nid);
    }
}

static void tray_sync(HINSTANCE hInst) {
    if (!g_app.tray_enabled) {
        tray_remove();
        return;
    }
    if (!g_app.tray_added) tray_add(hInst);
}

static void show_main_window(void) {
    HWND hwnd = g_app.hwnd;
    if (!hwnd) return;

    // Make sure it's visible (close-to-tray uses SW_HIDE)
    ShowWindow(hwnd, SW_SHOW);

    // Restore if minimized
    ShowWindow(hwnd, SW_RESTORE);

    // Bring to front (foreground restrictions can otherwise make it look like "nothing happened")
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
}

static void hide_main_window(void) {
    if (!g_app.hwnd) return;
    ShowWindow(g_app.hwnd, SW_HIDE);
}

static void app_request_exit(HWND hwnd) {
    g_app.force_quit = true;
    DestroyWindow(hwnd);
}

static void tray_show_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    BOOL vis = IsWindowVisible(hwnd);

    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOWHIDE, vis ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_PREFS, L"Preferences\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    UINT ctt = g_app.close_to_tray ? MF_CHECKED : 0;
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

static void post_log(const wchar_t* text) {
    LogMsg* m = (LogMsg*)malloc(sizeof(LogMsg));
    if (!m) return;
    m->text = wcsdup_heap(text ? text : L"");
    if (!m->text) { free(m); return; }

    if (!PostMessageW(g_app.hwnd, WM_APP_LOG, 0, (LPARAM)m)) {
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

static void post_status(int kind, const wchar_t* name, const wchar_t* status, time_t wall_ts) {
    StatusMsg* m = (StatusMsg*)malloc(sizeof(StatusMsg));
    if (!m) return;
    m->kind = kind;
    m->name = wcsdup_heap(name ? name : L"");
    if (!m->name) { free(m); return; }
    StringCchCopyW(m->status, 32, status ? status : L"unknown");
    m->wall_ts = wall_ts;

    if (!PostMessageW(g_app.hwnd, WM_APP_STATUS, 0, (LPARAM)m)) {
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

static void post_status_bulk(int kind, wchar_t** names, wchar_t (*statuses)[32], size_t n, time_t wall_ts) {
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

    if (!PostMessageW(g_app.hwnd, WM_APP_STATUS_BULK, 0, (LPARAM)m)) {
        free(m);
    }
}

static void request_save_debounced(void) {
    PostMessageW(g_app.hwnd, WM_APP_REQUEST_SAVE, 0, 0);
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
    if (!exe_lower || !exe_lower[0]) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return true; }

    do {
        wchar_t name[260];
        StringCchCopyW(name, 260, pe.szExeFile);
        CharLowerBuffW(name, (DWORD)wcslen(name));
        if (_wcsicmp(name, exe_lower) == 0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!h) {
                if (failed) (*failed)++;
                continue;
            }
            BOOL ok = TerminateProcess(h, 1);
            if (ok) {
                if (terminated) (*terminated)++;
            } else {
                if (failed) (*failed)++;
            }
            CloseHandle(h);
        }
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
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
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE h = OpenServiceW(scm, svc_name, SERVICE_QUERY_STATUS);
    if (!h) { CloseServiceHandle(scm); return false; }

    SERVICE_STATUS_PROCESS ssp;
    DWORD bytes = 0;
    BOOL ok = QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes);
    CloseServiceHandle(h);
    CloseServiceHandle(scm);

    if (!ok) return false;
    StringCchCopyW(out_status, cch, svc_state_to_str(ssp.dwCurrentState));
    return true;
}

static bool stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        return false;
    }

    SC_HANDLE h = OpenServiceW(scm, svc_name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        CloseServiceHandle(scm);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp0;
    DWORD bytes0 = 0;
    if (QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp0, sizeof(ssp0), &bytes0)) {
        if (ssp0.dwCurrentState == SERVICE_STOPPED) {
            CloseServiceHandle(h);
            CloseServiceHandle(scm);
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }
    }

    SERVICE_STATUS st;
    BOOL ok = ControlService(h, SERVICE_CONTROL_STOP, &st);
    if (!ok) {
        DWORD e = GetLastError();

        if (e == ERROR_SERVICE_NOT_ACTIVE) {
            CloseServiceHandle(h);
            CloseServiceHandle(scm);
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        CloseServiceHandle(h);
        CloseServiceHandle(scm);
        StringCchPrintfW(out_result, cch, L"error: stop refused (%s)", ebuf);
        return false;
    }

    if (wait_ms <= 0) {
        CloseServiceHandle(h);
        CloseServiceHandle(scm);
        StringCchCopyW(out_result, cch, L"stop requested");
        return true;
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD bytes = 0;
    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;

    while (now_mono_ms() < deadline) {
        if (!QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes)) {
            DWORD e = GetLastError();
            wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
            StringCchPrintfW(out_result, cch, L"stop requested (status query failed: %s)", ebuf);
            CloseServiceHandle(h);
            CloseServiceHandle(scm);
            return true;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED) {
            CloseServiceHandle(h);
            CloseServiceHandle(scm);
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        Sleep(150);
    }

    CloseServiceHandle(h);
    CloseServiceHandle(scm);
    StringCchCopyW(out_result, cch, L"stop requested (still stopping or refused)");
    return true;
}

static bool enum_services_build_disp_cache(DispCache* out_cache) {
    dispcache_free(out_cache);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    DWORD cap = 128 * 1024;
    BYTE* buf = (BYTE*)malloc(cap);
    if (!buf) { CloseServiceHandle(scm); return false; }

    DWORD bytesNeeded = 0, returned = 0, resume = 0;

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
                if (!nb) { free(buf); CloseServiceHandle(scm); return false; }
                buf = nb;
                continue;
            }
            free(buf);
            CloseServiceHandle(scm);
            return false;
        }

        ENUM_SERVICE_STATUS_PROCESSW* rows = (ENUM_SERVICE_STATUS_PROCESSW*)buf;
        for (DWORD i = 0; i < returned; i++) {
            const wchar_t* key = rows[i].lpServiceName ? rows[i].lpServiceName : L"";
            const wchar_t* disp = rows[i].lpDisplayName ? rows[i].lpDisplayName : L"";
            if (key[0] && disp[0]) {
                wchar_t dlow[512];
                StringCchCopyW(dlow, 512, disp);
                CharLowerBuffW(dlow, (DWORD)wcslen(dlow));
                dispcache_push(out_cache, dlow, key);
            }
        }

        if (resume == 0) break;
    }

    free(buf);
    CloseServiceHandle(scm);
    return true;
}

static void resolve_service_name(const wchar_t* raw, wchar_t* out, size_t cch) {
    StringCchCopyW(out, cch, raw ? raw : L"");
    trim_ws_inplace(out);
    if (!out[0]) return;

    wchar_t st[32];
    if (query_service_status_fast(out, st, 32)) return;

    const wchar_t* key = dispcache_lookup(&g_app.disp_cache, out);
    if (key) { StringCchCopyW(out, cch, key); return; }

    enum_services_build_disp_cache(&g_app.disp_cache);
    key = dispcache_lookup(&g_app.disp_cache, out);
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
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
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

static void lv_set_row(HWND lv, Item* it) {
    if (!lv || !it || it->name.empty()) return;

    int idx = lv_find_item_by_ptr(lv, it);
    if (idx < 0) idx = lv_find_item_by_name(lv, it->name.c_str());

    wchar_t last[64];
    fmt_time_local(last, 64, it->last_update_wall);

    if (idx < 0) {
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(lv);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(L"");
        item.lParam = (LPARAM)it;
        int row = ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, row, 1, (LPWSTR)it->name.c_str());
        ListView_SetItemText(lv, row, 2, (LPWSTR)it->last_status);
        ListView_SetItemText(lv, row, 3, last);

        g_app.suppress_lv_notify = true;
        ListView_SetCheckState(lv, row, it->auto_stop ? TRUE : FALSE);
        g_app.suppress_lv_notify = false;
    } else {
        // Ensure the row is bound to this Item* for O(1) future lookup
        LVITEMW pi;
        memset(&pi, 0, sizeof(pi));
        pi.mask = LVIF_PARAM;
        pi.iItem = idx;
        pi.lParam = (LPARAM)it;
        ListView_SetItem(lv, &pi);

        ListView_SetItemText(lv, idx, 2, (LPWSTR)it->last_status);
        ListView_SetItemText(lv, idx, 3, last);

        BOOL cur = ListView_GetCheckState(lv, idx);
        BOOL want = it->auto_stop ? TRUE : FALSE;
        if ((cur != 0) != (want != 0)) {
            g_app.suppress_lv_notify = true;
            ListView_SetCheckState(lv, idx, want);
            g_app.suppress_lv_notify = false;
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
static void activity_append(HWND lb, const wchar_t* line) {
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

    int cur = (int)SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0);
    if (sz.cx + 24 > cur) SendMessageW(lb, LB_SETHORIZONTALEXTENT, (WPARAM)(sz.cx + 24), 0);

    int newCount = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    SendMessageW(lb, LB_SETTOPINDEX, (WPARAM)std::max(0, newCount - 1), 0);
}

static void log_linef(const wchar_t* fmt, ...) {
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
    activity_append(g_app.activity, line);
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
static bool parse_setting(const wchar_t* key, const wchar_t* val) {
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
        if (v >= 200) { g_app.ui_refresh_ms = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"monitor_interval_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { g_app.monitor_interval_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"monitor_interval_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms) && ms >= 200) { g_app.monitor_interval_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"autostop_cooldown_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { g_app.autostop_cooldown_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { g_app.autostop_cooldown_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"stop_wait_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { g_app.stop_wait_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { g_app.stop_wait_ms = ms; return true; }
        return false;
    }

    // Tray settings (were written previously but not parsed; now supported)
    if (_wcsicmp(key, L"tray_enabled") == 0) {
        int v = 0; parse_int(&v);
        g_app.tray_enabled = (v != 0);
        if (!g_app.tray_enabled) g_app.close_to_tray = false;
        return true;
    }
    if (_wcsicmp(key, L"close_to_tray") == 0) {
        int v = 0; parse_int(&v);
        g_app.close_to_tray = (v != 0);
        if (!g_app.tray_enabled) g_app.close_to_tray = false;
        return true;
    }

    return false;
}




static void load_config(void) {
    FILE* f = NULL;
    _wfopen_s(&f, g_app.cfg_path, L"rb");
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
                if (parse_setting(p, bar + 1)) settings_loaded = true;
            }
            line = wcstok_s(NULL, L"\n", &ctx);
            continue;
        }

        int kind = KIND_SVC;
        wchar_t name[512]; name[0] = 0;
        int auto_stop = 0;

        if (_wcsnicmp(line, L"SVC|", 4) == 0 || _wcsnicmp(line, L"EXE|", 4) == 0) {
            kind = (_wcsnicmp(line, L"EXE|", 4) == 0) ? KIND_EXE : KIND_SVC;
            wchar_t* p = line + 4;
            wchar_t* a = wcschr(p, L'|');
            if (a) { *a = 0; StringCchCopyW(name, 512, p); auto_stop = _wtoi(a + 1); }
            else { StringCchCopyW(name, 512, p); auto_stop = 0; }
        } else {
            wchar_t* a = wcschr(line, L'|');
            if (a) { *a = 0; StringCchCopyW(name, 512, line); auto_stop = _wtoi(a + 1); }
            else { StringCchCopyW(name, 512, line); auto_stop = 0; }
            kind = KIND_SVC;
        }

        trim_ws_inplace(name);
        if (!name[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        wchar_t norm[512];
        if (kind == KIND_EXE) {
            normalize_exe_input(name, norm, 512);
            if (!norm[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }
        } else {
            StringCchCopyW(norm, 512, name);
        }

        Item* exist = NULL;
        {
            std::lock_guard<std::mutex> lk(g_app.mtx);
            exist = list_find(&g_app.items[kind], norm);
        }
        if (exist) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        Item* it = new (std::nothrow) Item();
        if (!it) break;
        it->kind = kind;
        it->name = norm;
        it->auto_stop = (auto_stop == 1);
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
            std::lock_guard<std::mutex> lk(g_app.mtx);
            list_push(&g_app.items[kind], it);
            g_app.items_gen[kind]++;
        }

        lv_set_row(kind == KIND_SVC ? g_app.lv_svc : g_app.lv_exe, it);

        log_linef(L"Monitoring restored (%s): %s (status: %s)",
                  (kind == KIND_SVC) ? L"service" : L"exe", it->name.c_str(), it->last_status);
        restored++;

        line = wcstok_s(NULL, L"\n", &ctx);
    }

    free(wtxt);

    if (restored > 0 || settings_loaded) request_save_debounced();

    // Restart UI timer with any loaded ui_refresh_ms
    PostMessageW(g_app.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}

typedef struct SaveRow {
    wchar_t* name;
    int auto_stop;
} SaveRow;

static void free_saverows(SaveRow* r, size_t n) {
    if (!r) return;
    for (size_t i = 0; i < n; i++) free(r[i].name);
    free(r);
}

static void save_config_now(void) {
    size_t ns = 0, ne = 0;
    int ui_refresh_ms = 1000;
    int monitor_interval_ms = 1000;
    int autostop_cooldown_ms = 15000;
    int stop_wait_ms = 10000;
    int tray_enabled = 1;
    int close_to_tray = 0;
    wchar_t cfg_path[MAX_PATH] = {0};

    SaveRow* svcs = NULL;
    SaveRow* exes = NULL;

    // Snapshot settings + counts under lock.
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);

        ui_refresh_ms = g_app.ui_refresh_ms;
        monitor_interval_ms = g_app.monitor_interval_ms;
        autostop_cooldown_ms = g_app.autostop_cooldown_ms;
        stop_wait_ms = g_app.stop_wait_ms;
        tray_enabled = g_app.tray_enabled ? 1 : 0;
        close_to_tray = g_app.close_to_tray ? 1 : 0;

        ns = g_app.items[KIND_SVC].v.size();
        ne = g_app.items[KIND_EXE].v.size();

        StringCchCopyW(cfg_path, MAX_PATH, g_app.cfg_path);
    }

    svcs = (SaveRow*)calloc(ns, sizeof(SaveRow));
    exes = (SaveRow*)calloc(ne, sizeof(SaveRow));
    if ((ns && !svcs) || (ne && !exes)) {
        free_saverows(svcs, ns);
        free_saverows(exes, ne);
        return;
    }

    // Copy monitored items under lock (best-effort if list changed while allocating).
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);

        size_t cur_ns = g_app.items[KIND_SVC].v.size();
        size_t cur_ne = g_app.items[KIND_EXE].v.size();

        size_t copy_ns = (cur_ns < ns) ? cur_ns : ns;
        size_t copy_ne = (cur_ne < ne) ? cur_ne : ne;

        for (size_t i = 0; i < copy_ns; i++) {
            Item* it = g_app.items[KIND_SVC].v[i].get();
            svcs[i].name = (it && !it->name.empty()) ? wcsdup_heap(it->name.c_str()) : NULL;
            svcs[i].auto_stop = (it && it->auto_stop) ? 1 : 0;
        }
        for (size_t i = 0; i < copy_ne; i++) {
            Item* it = g_app.items[KIND_EXE].v[i].get();
            exes[i].name = (it && !it->name.empty()) ? wcsdup_heap(it->name.c_str()) : NULL;
            exes[i].auto_stop = (it && it->auto_stop) ? 1 : 0;
        }
    }

    wchar_t tmp[MAX_PATH];
    StringCchPrintfW(tmp, MAX_PATH, L"%s.tmp", cfg_path);

    FILE* f = NULL;
    _wfopen_s(&f, tmp, L"wb");
    if (!f) { free_saverows(svcs, ns); free_saverows(exes, ne); return; }

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
        "#   EXE|<exe_name>|<auto_stop 0/1>\n",
        ui_refresh_ms, monitor_interval_ms, autostop_cooldown_ms, stop_wait_ms,
        tray_enabled, close_to_tray
    );
    fwrite(header, 1, (size_t)n, f);

    for (size_t i = 0; i < ns; i++) {
        if (!svcs[i].name || !svcs[i].name[0]) continue;
        wchar_t wline[600];
        StringCchPrintfW(wline, 600, L"SVC|%s|%d\n", svcs[i].name, svcs[i].auto_stop ? 1 : 0);
        int blen = WideCharToMultiByte(CP_UTF8, 0, wline, -1, NULL, 0, NULL, NULL);
        char* b = (char*)malloc((size_t)blen);
        if (!b) continue;
        WideCharToMultiByte(CP_UTF8, 0, wline, -1, b, blen, NULL, NULL);
        fwrite(b, 1, (size_t)(blen - 1), f);
        free(b);
    }
    for (size_t i = 0; i < ne; i++) {
        if (!exes[i].name || !exes[i].name[0]) continue;
        wchar_t wline[600];
        StringCchPrintfW(wline, 600, L"EXE|%s|%d\n", exes[i].name, exes[i].auto_stop ? 1 : 0);
        int blen = WideCharToMultiByte(CP_UTF8, 0, wline, -1, NULL, 0, NULL, NULL);
        char* b = (char*)malloc((size_t)blen);
        if (!b) continue;
        WideCharToMultiByte(CP_UTF8, 0, wline, -1, b, blen, NULL, NULL);
        fwrite(b, 1, (size_t)(blen - 1), f);
        free(b);
    }

    fclose(f);
    MoveFileExW(tmp, cfg_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    free_saverows(svcs, ns);
    free_saverows(exes, ne);
}



// ----------------------------
// Action worker queue
// ----------------------------
static void action_enqueue(int kind, const wchar_t* name, const wchar_t* reason, int wait_ms) {
    if (!name || !name[0]) return;

    Action a;
    a.kind = kind;
    a.name = name;
    a.reason = reason ? reason : L"";
    a.wait_ms = wait_ms;

    {
        std::lock_guard<std::mutex> lk(g_app.action_mtx);
        g_app.action_q.emplace_back(std::move(a));
    }
    g_app.action_cv.notify_one();
}

static void action_thread_main(std::stop_token st) {
    for (;;) {
        Action a;
        {
            std::unique_lock<std::mutex> lk(g_app.action_mtx);
            g_app.action_cv.wait(lk, [&] { return st.stop_requested() || !g_app.action_q.empty(); });
            if (st.stop_requested()) break;

            a = std::move(g_app.action_q.front());
            g_app.action_q.pop_front();
        }

        if (a.kind == KIND_SVC) {
            wchar_t result[256];
            stop_service_and_wait(a.name.c_str(), a.wait_ms, result, 256);

            wchar_t line[700];
            StringCchPrintfW(line, 700, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(line);
        } else {
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
            post_log(line);
        }
    }

    // Best-effort drop any remaining queued work on shutdown (old behavior: pending actions were freed/dropped).
    {
        std::lock_guard<std::mutex> lk(g_app.action_mtx);
        g_app.action_q.clear();
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

static void monitor_thread_main(std::stop_token st) {
    MonSnap snap;
    MonScratch sc;
    memset(&snap, 0, sizeof(snap));
    memset(&sc, 0, sizeof(sc));

    for (;;) {
        if (st.stop_requested()) break;

        int monitor_interval_ms = 1000;
        int cooldown_ms = 0;
        int stop_wait_ms = 0;

        size_t ns = 0, ne = 0;
        uint32_t gen_s = 0, gen_e = 0;

        bool ok_alloc = false;

        // Snapshot pointers + cheap fields under lock.
        {
            std::lock_guard<std::mutex> lk(g_app.mtx);

            monitor_interval_ms = g_app.monitor_interval_ms;
            cooldown_ms = g_app.autostop_cooldown_ms;
            stop_wait_ms = g_app.stop_wait_ms;

            ns = g_app.items[KIND_SVC].v.size();
            ne = g_app.items[KIND_EXE].v.size();
            gen_s = g_app.items_gen[KIND_SVC];
            gen_e = g_app.items_gen[KIND_EXE];

            ok_alloc = mons_ensure_snap(&snap, ns, ne) && mons_ensure_scratch(&sc, ns, ne);
            if (ok_alloc) {
                // Refresh name copies ONLY if list changed (add/remove), but always refresh auto/lastreq.
                if (gen_s != snap.gen_s || ns != snap.ns) {
                    // Free old names in range [0, snap.ns)
                    free_name_array(snap.svc_names, snap.ns);
                    for (size_t i = 0; i < ns; i++) {
                        Item* it = g_app.items[KIND_SVC].v[i].get();
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
                        Item* it = g_app.items[KIND_EXE].v[i].get();
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
                    Item* it = g_app.items[KIND_SVC].v[i].get();
                    snap.svc_auto[i] = it ? it->auto_stop : false;
                    snap.svc_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                }
                for (size_t i = 0; i < ne; i++) {
                    Item* it = g_app.items[KIND_EXE].v[i].get();
                    snap.exe_auto[i] = it ? it->auto_stop : false;
                    snap.exe_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                }
            }
        }

        if (!ok_alloc) {
            // If we can't allocate snapshot/scratch buffers, skip this cycle.
            post_log(L"Monitor: out of memory (skipping cycle)");
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
            if (!ok) post_log(L"Monitor: batch service enum failed");

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
                                std::lock_guard<std::mutex> lk(g_app.mtx);
                                Item* it = list_find(&g_app.items[KIND_SVC], snap.svc_names[i]);
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
                                post_log(msg);

                                wchar_t reason[256];
                                StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", c);
                                action_enqueue(KIND_SVC, snap.svc_names[i], reason, stop_wait_ms);
                            }
                        }
                    }
                }
            }

            // Option A: one message per kind per tick
            post_status_bulk(KIND_SVC, snap.svc_names, sc.svc_status, ns, wall_now);
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
                            std::lock_guard<std::mutex> lk(g_app.mtx);
                            Item* it = list_find(&g_app.items[KIND_EXE], snap.exe_names[i]);
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
                            post_log(msg);

                            wchar_t reason[256];
                            StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", k);
                            action_enqueue(KIND_EXE, snap.exe_names[i], reason, stop_wait_ms);
                        }
                    }
                }
            }

            post_status_bulk(KIND_EXE, snap.exe_names, sc.exe_status, ne, wall_now);
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
static bool prefs_apply(HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close);
static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_prefs_dialog(HWND parent);
static LRESULT CALLBACK HelpWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void open_help_dialog(HWND parent);

static bool prefs_apply(HWND dlg, HWND e_ui, HWND e_mon, HWND e_cd, HWND e_sw, HWND c_tray, HWND c_close) {
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
        std::lock_guard<std::mutex> lk(g_app.mtx);

        g_app.ui_refresh_ms = ui;
        g_app.monitor_interval_ms = mon;
        g_app.autostop_cooldown_ms = cd;
        g_app.stop_wait_ms = sw;

        // Tray settings
        BOOL tray_checked = (SendMessageW(c_tray, BM_GETCHECK, 0, 0) == BST_CHECKED);
        BOOL close_checked = (SendMessageW(c_close, BM_GETCHECK, 0, 0) == BST_CHECKED);

        bool tray_en = tray_checked ? true : false;
        bool close_en = close_checked ? true : false;

        if (close_en) tray_en = true; // close-to-tray requires tray icon

        g_app.tray_enabled = tray_en;
        g_app.close_to_tray = tray_en && close_en;

        if (!g_app.tray_enabled) g_app.close_to_tray = false;
    }

// Apply tray icon add/remove immediately
tray_sync(GetModuleHandleW(NULL));

    log_linef(L"Settings updated: UI %dms, Monitor %dms, Cooldown %dms, StopWait %dms, Tray %d, CloseToTray %d",
              g_app.ui_refresh_ms, g_app.monitor_interval_ms, g_app.autostop_cooldown_ms, g_app.stop_wait_ms, g_app.tray_enabled ? 1 : 0, g_app.close_to_tray ? 1 : 0);

    request_save_debounced();
    PostMessageW(g_app.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
    return true;
}

static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND e_ui, e_mon, e_cd, e_sw;
    static HWND c_tray, c_close;
    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        CreateWindowW(L"STATIC", L"UI Refresh (ms):", WS_CHILD | WS_VISIBLE,
            12, 14, 160, 20, dlg, NULL, NULL, NULL);
        e_ui = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            180, 12, 140, 24, dlg, (HMENU)IDC_PREF_UI_MS, NULL, NULL);

        CreateWindowW(L"STATIC", L"Monitor Interval (ms):", WS_CHILD | WS_VISIBLE,
            12, 48, 160, 20, dlg, NULL, NULL, NULL);
        e_mon = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            180, 46, 140, 24, dlg, (HMENU)IDC_PREF_MON_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Autostop Cooldown (ms):", WS_CHILD | WS_VISIBLE,
            12, 82, 160, 20, dlg, NULL, NULL, NULL);
        e_cd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            180, 80, 140, 24, dlg, (HMENU)IDC_PREF_CD_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Stop Wait (ms):", WS_CHILD | WS_VISIBLE,
            12, 116, 160, 20, dlg, NULL, NULL, NULL);
        e_sw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            180, 114, 140, 24, dlg, (HMENU)IDC_PREF_SW_S, NULL, NULL);


// Tray options
c_tray = CreateWindowW(L"BUTTON", L"Enable tray icon", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    12, 152, 200, 22, dlg, (HMENU)IDC_PREF_TRAY_ENABLE, NULL, NULL);

c_close = CreateWindowW(L"BUTTON", L"Close to tray (hide on X)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    12, 176, 240, 22, dlg, (HMENU)IDC_PREF_CLOSE_TO_TRAY, NULL, NULL);
        wchar_t buf[64];
        StringCchPrintfW(buf, 64, L"%d", g_app.ui_refresh_ms);
        SetWindowTextW(e_ui, buf);
        StringCchPrintfW(buf, 64, L"%d", g_app.monitor_interval_ms);
        SetWindowTextW(e_mon, buf);
        StringCchPrintfW(buf, 64, L"%d", g_app.autostop_cooldown_ms);
        SetWindowTextW(e_cd, buf);
        StringCchPrintfW(buf, 64, L"%d", g_app.stop_wait_ms);
        SetWindowTextW(e_sw, buf);

SendMessageW(c_tray, BM_SETCHECK, g_app.tray_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
SendMessageW(c_close, BM_SETCHECK, g_app.close_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);


        wchar_t cfgline[MAX_PATH + 64];
        StringCchPrintfW(cfgline, MAX_PATH + 64, L"Config file: %s", g_app.cfg_path);
        CreateWindowW(L"STATIC", cfgline, WS_CHILD | WS_VISIBLE,
            12, 210, 470, 20, dlg, NULL, NULL, NULL);

        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            230, 244, 80, 28, dlg, (HMENU)IDCANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE,
            318, 244, 80, 28, dlg, (HMENU)IDC_PREF_APPLY, NULL, NULL);
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            406, 244, 80, 28, dlg, (HMENU)IDOK, NULL, NULL);

        if (g_app.ui_font) apply_font_recursive(dlg, g_app.ui_font);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        if (id == IDC_PREF_APPLY) { prefs_apply(dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close); return 0; }
        if (id == IDOK) {
            if (prefs_apply(dlg, e_ui, e_mon, e_cd, e_sw, c_tray, c_close)) DestroyWindow(dlg);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

static void open_prefs_dialog(HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PrefsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PrefsDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    int w = 510, h = 310;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Preferences",
        WS_CAPTION | WS_POPUP | WS_SYSMENU,
        x, y, w, h, parent, NULL, wc.hInstance, NULL);

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
    static HWND edit;
    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        CreateWindowW(L"STATIC", L"What the settings do", WS_CHILD | WS_VISIBLE,
            12, 10, 400, 20, dlg, NULL, NULL, NULL);

        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
            WS_VSCROLL | WS_HSCROLL,
            12, 34, 560, 320, dlg, NULL, NULL, NULL);

        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            492, 362, 80, 28, dlg, (HMENU)IDCANCEL, NULL, NULL);

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
        if (g_app.ui_font) apply_font_recursive(dlg, g_app.ui_font);
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

static void open_help_dialog(HWND parent) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HelpWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"HelpDialogClass_C";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    RECT pr; GetWindowRect(parent, &pr);
    int w = 600, h = 430;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Settings Help",
        WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, parent, NULL, wc.hInstance, NULL);

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

static void layout(HWND hwnd);

static const wchar_t* SPLITTER_CLASS = L"SplitterBar_C";

static LRESULT CALLBACK SplitterProc(HWND sw, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN: {
        SetCapture(sw);
        g_app.splitter_dragging = true;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_app.hwnd, &pt);

        g_app.splitter_drag_start_x = pt.x;
        g_app.splitter_drag_start_w = (g_app.activity_panel_w > 0) ? g_app.activity_panel_w : 360;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_app.splitter_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(g_app.hwnd, &pt);

            int dx = pt.x - g_app.splitter_drag_start_x;
            int desired = g_app.splitter_drag_start_w - dx; // drag right => narrower Activity panel
            g_app.activity_panel_w = desired;

            // Layout clamps sizes; keep it snappy during drags.
            layout(g_app.hwnd);
            return 0;
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_app.splitter_dragging) {
            g_app.splitter_dragging = false;
            ReleaseCapture();
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        g_app.splitter_dragging = false;
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

static void build_ui(HWND hwnd) {
    register_splitter_class();
    g_app.left_tabs = make_tab(hwnd, IDC_LEFT_TABS);
    g_app.center_tabs = make_tab(hwnd, IDC_CENTER_TABS);

    tab_add(g_app.left_tabs, 0, L"Services");
    tab_add(g_app.left_tabs, 1, L"EXEs");
    tab_add(g_app.center_tabs, 0, L"Services");
    tab_add(g_app.center_tabs, 1, L"EXEs");

    g_app.left_page_svc = make_page(hwnd);
    g_app.left_page_exe = make_page(hwnd);
    g_app.center_page_svc = make_page(hwnd);
    g_app.center_page_exe = make_page(hwnd);

    // Left: Services controls (same functionality, cleaner labels)
    CreateWindowW(L"STATIC", L"Add Windows Service", WS_CHILD | WS_VISIBLE,
        10, 10, 220, 18, g_app.left_page_svc, NULL, NULL, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 34, 220, 24, g_app.left_page_svc, (HMENU)IDC_SVC_ADD_EDIT, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
        10, 64, 220, 28, g_app.left_page_svc, (HMENU)IDC_SVC_ADD_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Stop selected", WS_CHILD | WS_VISIBLE,
        10, 110, 220, 28, g_app.left_page_svc, (HMENU)IDC_SVC_STOP_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
        10, 144, 220, 28, g_app.left_page_svc, (HMENU)IDC_SVC_REMOVE_BTN, NULL, NULL);

    // Left: EXE controls
    CreateWindowW(L"STATIC", L"Add Executable (.exe)", WS_CHILD | WS_VISIBLE,
        10, 10, 220, 18, g_app.left_page_exe, NULL, NULL, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 34, 220, 24, g_app.left_page_exe, (HMENU)IDC_EXE_ADD_EDIT, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
        10, 64, 220, 28, g_app.left_page_exe, (HMENU)IDC_EXE_ADD_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Stop selected", WS_CHILD | WS_VISIBLE,
        10, 110, 220, 28, g_app.left_page_exe, (HMENU)IDC_EXE_STOP_BTN, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
        10, 144, 220, 28, g_app.left_page_exe, (HMENU)IDC_EXE_REMOVE_BTN, NULL, NULL);

    // Center: listviews
    g_app.lv_svc = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, g_app.center_page_svc, (HMENU)IDC_LV_SVC, GetModuleHandleW(NULL), NULL);

    g_app.lv_exe = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, g_app.center_page_exe, (HMENU)IDC_LV_EXE, GetModuleHandleW(NULL), NULL);

    lv_set_columns(g_app.lv_svc, L"Auto Stop", L"Service Name", L"Status", L"Last Update");
    lv_set_columns(g_app.lv_exe, L"Auto Stop", L"Executable", L"Status", L"Last Update");

    // Explorer theming for modern look (safe if unavailable)
    try_set_explorer_theme(g_app.lv_svc);
    try_set_explorer_theme(g_app.lv_exe);
    try_set_explorer_theme(g_app.left_tabs);
    try_set_explorer_theme(g_app.center_tabs);

    // Right: activity
    // Draggable splitter between center and Activity
    g_app.splitter = CreateWindowExW(0, SPLITTER_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, SPLITTER_WIDTH, 100, hwnd, (HMENU)IDC_SPLITTER, GetModuleHandleW(NULL), NULL);

    g_app.activity_label = CreateWindowW(L"STATIC", L"Activity", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 18, hwnd, NULL, NULL, NULL);

    g_app.activity = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, hwnd, (HMENU)IDC_ACTIVITY, NULL, NULL);

    // Status bar (new; uses your UI refresh setting)
    g_app.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, GetModuleHandleW(NULL), NULL);

    TabCtrl_SetCurSel(g_app.left_tabs, 0);
    TabCtrl_SetCurSel(g_app.center_tabs, 0);
    show_only(g_app.left_page_svc, g_app.left_page_exe, true);
    show_only(g_app.center_page_svc, g_app.center_page_exe, true);

    // Apply modern font once everything exists
    if (g_app.ui_font) apply_font_recursive(hwnd, g_app.ui_font);
}

// Status bar update (UI thread)
static void update_statusbar(void) {
    if (!g_app.statusbar) return;

    size_t ns = 0, ne = 0;
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        ns = g_app.items[KIND_SVC].v.size();
        ne = g_app.items[KIND_EXE].v.size();
    }

    wchar_t ts[64];
    fmt_time_local(ts, 64, g_app.last_any_status_wall);

    wchar_t text[256];
    StringCchPrintfW(text, 256, L"Services: %zu   EXEs: %zu   Last update: %s", ns, ne, ts);
    SendMessageW(g_app.statusbar, SB_SETTEXTW, 0, (LPARAM)text);
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    // Status bar takes bottom space
    int sbH = 0;
    if (g_app.statusbar) {
        SendMessageW(g_app.statusbar, WM_SIZE, 0, 0);
        RECT sr; GetWindowRect(g_app.statusbar, &sr);
        sbH = (sr.bottom - sr.top);
        SetWindowPos(g_app.statusbar, NULL, 0, H - sbH, W, sbH, SWP_NOZORDER);
        H -= sbH;
    }

    int leftW = 270;
if (W < leftW + 360 + 300) {
    leftW = std::max(220, W / 5);
}

int rightW = (g_app.activity_panel_w > 0) ? g_app.activity_panel_w : 360;

// Clamp to sane bounds while always fitting in the client area.
// (If the window is extremely narrow, center/right may get small, but we avoid negative sizes.)
int minRightW = ACTIVITY_MIN_W;
int minCenterW = CENTER_MIN_W;

int maxRightW = W - leftW - minCenterW;
if (maxRightW < minRightW) maxRightW = W - leftW - 100;
if (maxRightW < 80) maxRightW = 80;

rightW = clamp_int(rightW, 80, maxRightW);

int centerW = W - leftW - rightW;
if (centerW < 80) {
    centerW = 80;
    rightW = W - leftW - centerW;
    if (rightW < 80) rightW = 80;
}

// If there's enough room to honor minimum center width, do it by shrinking the Activity panel.
if (centerW < minCenterW) {
    int wantRight = W - leftW - minCenterW;
    if (wantRight >= minRightW) {
        rightW = wantRight;
        centerW = minCenterW;
    }
}

g_app.activity_panel_w = rightW;

    int pad = 8;
    int tabH = 28;

    SetWindowPos(g_app.left_tabs, NULL, pad, pad, leftW - 2*pad, tabH, SWP_NOZORDER);
    int leftX = pad;
    int leftY = pad + tabH;
    int leftH = H - leftY - pad;
    SetWindowPos(g_app.left_page_svc, NULL, leftX, leftY, leftW - 2*pad, leftH, SWP_NOZORDER);
    SetWindowPos(g_app.left_page_exe, NULL, leftX, leftY, leftW - 2*pad, leftH, SWP_NOZORDER);

    int cx = leftW + pad;
    SetWindowPos(g_app.center_tabs, NULL, cx, pad, centerW - 2*pad, tabH, SWP_NOZORDER);
    int cy = pad + tabH;
    int ch = H - cy - pad;
    SetWindowPos(g_app.center_page_svc, NULL, cx, cy, centerW - 2*pad, ch, SWP_NOZORDER);
    SetWindowPos(g_app.center_page_exe, NULL, cx, cy, centerW - 2*pad, ch, SWP_NOZORDER);

    SetWindowPos(g_app.lv_svc, NULL, 0, 0, centerW - 2*pad, ch, SWP_NOZORDER);
    SetWindowPos(g_app.lv_exe, NULL, 0, 0, centerW - 2*pad, ch, SWP_NOZORDER);

    int rx = leftW + centerW + pad;

    if (g_app.splitter) {
        int sx = leftW + centerW - (SPLITTER_WIDTH / 2);
        SetWindowPos(g_app.splitter, NULL, sx, pad, SPLITTER_WIDTH, H - 2*pad, SWP_NOZORDER);
    }
    int labelH = 18;
    HWND label = g_app.activity_label;
    if (label) SetWindowPos(label, NULL, rx, pad, rightW - 2*pad, labelH, SWP_NOZORDER);

    SetWindowPos(g_app.activity, NULL, rx, pad + labelH + 6, rightW - 2*pad, H - (pad + labelH + 6) - pad, SWP_NOZORDER);

    // Mark for autosize on next UI tick (less churn during resize drags)
    g_app.lv_needs_layout[KIND_SVC] = true;
    g_app.lv_needs_layout[KIND_EXE] = true;
}

// ----------------------------
// Add/Remove/Stop handlers
// ----------------------------
static void add_item(int kind) {
    HWND page = (kind == KIND_SVC) ? g_app.left_page_svc : g_app.left_page_exe;
    int editId = (kind == KIND_SVC) ? IDC_SVC_ADD_EDIT : IDC_EXE_ADD_EDIT;
    HWND edit = GetDlgItem(page, editId);

    wchar_t raw[512];
    GetWindowTextW(edit, raw, 512);
    trim_ws_inplace(raw);
    if (!raw[0]) {
        msgbox_warn(g_app.hwnd, L"Add", L"Name is required.");
        return;
    }

    wchar_t name[512];
    if (kind == KIND_SVC) {
        resolve_service_name(raw, name, 512);
        wchar_t st[32];
        if (!query_service_status_fast(name, st, 32)) {
            wchar_t err[900];
            StringCchPrintfW(err, 900,
                L"Service not found.\n\nInput: %s\nResolved as: %s\n\nTip: try the service key name (e.g., 'wuauserv').",
                raw, name);
            msgbox_err(g_app.hwnd, L"Add service", err);
            return;
        }
    } else {
        normalize_exe_input(raw, name, 512);
        if (!name[0]) {
            msgbox_err(g_app.hwnd, L"Add EXE", L"Executable name is invalid.");
            return;
        }
    }

    Item* exist = NULL;
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        exist = list_find(&g_app.items[kind], name);
    }
    if (exist) {
        msgbox_info(g_app.hwnd, L"Add", L"Already monitored.");
        return;
    }

    Item* it = new (std::nothrow) Item();
    if (!it) return;
    it->kind = kind;
    it->name = name;
    it->auto_stop = false;
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
        std::lock_guard<std::mutex> lk(g_app.mtx);
        list_push(&g_app.items[kind], it);
        g_app.items_gen[kind]++;
    }

    HWND lv = (kind == KIND_SVC) ? g_app.lv_svc : g_app.lv_exe;
    lv_set_row(lv, it);

    log_linef(L"Monitoring started (%s): %s (status: %s)",
              (kind == KIND_SVC) ? L"service" : L"exe", it->name.c_str(), it->last_status);
    request_save_debounced();

    g_app.lv_needs_layout[kind] = true;
    update_statusbar();

    SetWindowTextW(edit, L"");
}

static void remove_selected(int kind) {
    HWND lv = (kind == KIND_SVC) ? g_app.lv_svc : g_app.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) return;

    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        removed = list_remove_name(&g_app.items[kind], name);
        if (removed) g_app.items_gen[kind]++;
    }
    if (!removed) return;

    int idx = lv_find_item_by_name(lv, name);
    if (idx >= 0) ListView_DeleteItem(lv, idx);

    log_linef(L"Monitoring removed (%s): %s", (kind == KIND_SVC) ? L"service" : L"exe", name);
    request_save_debounced();

    g_app.lv_needs_layout[kind] = true;
    update_statusbar();
}

static void stop_selected(int kind) {
    HWND lv = (kind == KIND_SVC) ? g_app.lv_svc : g_app.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) {
        msgbox_warn(g_app.hwnd, L"Stop", L"Select a row first.");
        return;
    }

    log_linef(L"%s: %s\u2026", name, (kind == KIND_SVC) ? L"stop requested" : L"terminate requested");
    int wait_ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        wait_ms = g_app.stop_wait_ms;
    }
    action_enqueue(kind, name, L"manual stop", wait_ms);
}

// Row checkbox toggle
static void toggle_autostop_by_row(int kind, int row) {
    HWND lv = (kind == KIND_SVC) ? g_app.lv_svc : g_app.lv_exe;

    wchar_t name[512];
    ListView_GetItemText(lv, row, 1, name, 512);
    if (!name[0]) return;

    bool enabled = (ListView_GetCheckState(lv, row) != 0);

    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        Item* it = list_find(&g_app.items[kind], name);
        if (it) it->auto_stop = enabled;
    }

    log_linef(L"%s: auto-stop %s", name, enabled ? L"enabled" : L"disabled");
    request_save_debounced();
}

// ----------------------------
// Main window proc
// ----------------------------
static void set_tab_sel_if(HWND tab, int sel) {
    if (!tab) return;
    int cur = TabCtrl_GetCurSel(tab);
    if (cur != sel) TabCtrl_SetCurSel(tab, sel);
}

static void sync_tabs_to(int sel) {
    if (sel < 0) sel = 0;
    if (sel > 1) sel = 1;

    set_tab_sel_if(g_app.left_tabs, sel);
    set_tab_sel_if(g_app.center_tabs, sel);

    // Keep both sides in lock-step so the left actions always correspond
    // to the visible center list.
    show_only(g_app.left_page_svc, g_app.left_page_exe, (sel == 0));
    show_only(g_app.center_page_svc, g_app.center_page_exe, (sel == 0));
}
static void restart_ui_timer(HWND hwnd) {
    KillTimer(hwnd, TIMER_UI_REFRESH);
    int ms = std::max(200, g_app.ui_refresh_ms);
    SetTimer(hwnd, TIMER_UI_REFRESH, (UINT)ms, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    
if (g_app.taskbar_restart_msg && msg == g_app.taskbar_restart_msg) {
    // Explorer restarted; re-add tray icon
    g_app.tray_added = false;
    tray_sync(GetModuleHandleW(NULL));
    return 0;
}

switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;

        // Optional: dark title bar (harmless if unsupported)
        try_enable_dark_titlebar(hwnd);

        build_ui(hwnd);

        HMENU menubar = CreateMenu();
        HMENU settings = CreatePopupMenu();
        AppendMenuW(settings, MF_STRING, IDM_PREFS, L"Preferences\u2026");
        AppendMenuW(settings, MF_SEPARATOR, 0, NULL);
        AppendMenuW(settings, MF_STRING, IDM_HELP, L"Help\u2026");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)settings, L"Settings");
        AppendMenuW(menubar, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menubar, MF_STRING, IDM_EXIT, L"Exit");
        SetMenu(hwnd, menubar);

// Register for Explorer restart message so we can re-add the tray icon
g_app.taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");



        // Allow Explorer to deliver tray callback messages to an elevated window
        allow_uipi_message(hwnd, WM_APP_TRAYICON);
        if (g_app.taskbar_restart_msg) allow_uipi_message(hwnd, g_app.taskbar_restart_msg);

        if (!enum_services_build_disp_cache(&g_app.disp_cache)) {
            log_linef(L"Display cache: enumerate failed");
        }

        // Start UI timer early (may be restarted after config load)
        restart_ui_timer(hwnd);

        load_config();

// Apply tray icon setting from config
tray_sync(GetModuleHandleW(NULL));


        // Start background workers (C++20 std::jthread: auto-join + cooperative stop).
        g_app.action_thread = std::jthread(action_thread_main);
        g_app.monitor_thread = std::jthread(monitor_thread_main);

        update_statusbar();
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lParam;
        
if (nh->idFrom == IDC_LEFT_TABS && nh->code == TCN_SELCHANGE) {
    int sel = TabCtrl_GetCurSel(g_app.left_tabs);
    sync_tabs_to(sel);
    return 0;
}
if (nh->idFrom == IDC_CENTER_TABS && nh->code == TCN_SELCHANGE) {
    int sel = TabCtrl_GetCurSel(g_app.center_tabs);
    sync_tabs_to(sel);
    return 0;
}
        if ((nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE) && nh->code == LVN_ITEMCHANGED) {
            if (g_app.suppress_lv_notify) return 0;
            NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
            if (lv->uChanged & LVIF_STATE) {
                int kind = (nh->idFrom == IDC_LV_SVC) ? KIND_SVC : KIND_EXE;
                if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) {
                    toggle_autostop_by_row(kind, lv->iItem);
                }
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDM_PREFS) { open_prefs_dialog(hwnd); return 0; }
        if (id == IDM_HELP)  { open_help_dialog(hwnd);  return 0; }
        if (id == IDM_EXIT)  { app_request_exit(hwnd); return 0; }

        
if (id == IDM_TRAY_SHOWHIDE) {
    if (IsWindowVisible(hwnd)) hide_main_window(); else show_main_window();
    return 0;
}
if (id == IDM_TRAY_PREFS) { open_prefs_dialog(hwnd); return 0; }
if (id == IDM_TRAY_CLOSE_TO_TRAY) {
    g_app.close_to_tray = !g_app.close_to_tray;
    if (g_app.close_to_tray) g_app.tray_enabled = true;
    request_save_debounced();
    return 0;
}
if (id == IDM_TRAY_EXIT) { app_request_exit(hwnd); return 0; }
switch (id) {
        case IDC_SVC_ADD_BTN: add_item(KIND_SVC); return 0;
        case IDC_EXE_ADD_BTN: add_item(KIND_EXE); return 0;
        case IDC_SVC_REMOVE_BTN: remove_selected(KIND_SVC); return 0;
        case IDC_EXE_REMOVE_BTN: remove_selected(KIND_EXE); return 0;
        case IDC_SVC_STOP_BTN: stop_selected(KIND_SVC); return 0;
        case IDC_EXE_STOP_BTN: stop_selected(KIND_EXE); return 0;
        }
        return 0;
    }

    case WM_APP_RESTART_UI_TIMER:
        restart_ui_timer(hwnd);
        return 0;

    case WM_APP_LOG: {
        LogMsg* m = (LogMsg*)lParam;
        if (m) {
            log_linef(L"%s", m->text ? m->text : L"");
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
                std::lock_guard<std::mutex> lk(g_app.mtx);
                for (uint32_t i = 0; i < m->n; i++) {
                if (e[i].name_off == 0) continue;
                wchar_t* name = (wchar_t*)(base + e[i].name_off);
                if (!name || !name[0]) continue;

                Item* it = list_find(&g_app.items[m->kind], name);
                if (it) {
                    StringCchCopyW(it->last_status, 32, e[i].status);
                    it->last_update_wall = m->wall_ts;
                    lv_set_row(m->kind == KIND_SVC ? g_app.lv_svc : g_app.lv_exe, it);
                }
                }
            }

            g_app.last_any_status_wall = m->wall_ts;
            g_app.lv_needs_layout[m->kind] = true;

            free(m);
        }
        return 0;
    }



    case WM_APP_STATUS: {
        StatusMsg* m = (StatusMsg*)lParam;
        if (m && m->name) {
            {
                std::lock_guard<std::mutex> lk(g_app.mtx);
                Item* it = list_find(&g_app.items[m->kind], m->name);
                if (it) {
                StringCchCopyW(it->last_status, 32, m->status);
                it->last_update_wall = m->wall_ts;
                lv_set_row(m->kind == KIND_SVC ? g_app.lv_svc : g_app.lv_exe, it);
                }
            }

            g_app.last_any_status_wall = m->wall_ts;
            g_app.lv_needs_layout[m->kind] = true;

            free(m->name);
            free(m);
        }
        return 0;
    }

    case WM_APP_REQUEST_SAVE:
        if (!g_app.save_pending) {
            g_app.save_pending = true;
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        } else {
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DEBOUNCE_SAVE) {
            KillTimer(hwnd, TIMER_DEBOUNCE_SAVE);
            g_app.save_pending = false;
            save_config_now();
            return 0;
        }
        if (wParam == TIMER_UI_REFRESH) {
            // Throttled UI maintenance
            if (g_app.lv_needs_layout[KIND_SVC]) {
                lv_autosize_status_last(g_app.lv_svc);
                lv_apply_name_fill(g_app.lv_svc);
                g_app.lv_needs_layout[KIND_SVC] = false;
            }
            if (g_app.lv_needs_layout[KIND_EXE]) {
                lv_autosize_status_last(g_app.lv_exe);
                lv_apply_name_fill(g_app.lv_exe);
                g_app.lv_needs_layout[KIND_EXE] = false;
            }
            update_statusbar();
            return 0;
        }
        return 0;

    
case WM_APP_TRAYICON: {
    // Different Windows versions deliver different codes; handle broadly.
    switch ((UINT)lParam) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case NIN_BALLOONUSERCLICK:
        show_main_window();
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
        if (!g_app.force_quit && g_app.close_to_tray && g_app.tray_enabled) {
            hide_main_window();
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        tray_remove();

        // Stop background threads before freeing shared state.
        if (g_app.monitor_thread.joinable()) g_app.monitor_thread.request_stop();
        if (g_app.action_thread.joinable()) g_app.action_thread.request_stop();
        g_app.action_cv.notify_all();

        if (g_app.monitor_thread.joinable()) g_app.monitor_thread.join();
        if (g_app.action_thread.joinable()) g_app.action_thread.join();

        save_config_now();

        {
            std::lock_guard<std::mutex> lk(g_app.action_mtx);
            g_app.action_q.clear();
        }

        dispcache_free(&g_app.disp_cache);
        list_free(&g_app.items[KIND_SVC]);
        list_free(&g_app.items[KIND_EXE]);

        if (g_app.ui_font && g_app.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(g_app.ui_font);
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
    enable_dpi_awareness();
    ensure_admin_or_exit();

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);

    list_init(&g_app.items[KIND_SVC]);
    list_init(&g_app.items[KIND_EXE]);


    g_app.items_gen[KIND_SVC] = 1;
    g_app.items_gen[KIND_EXE] = 1;
    g_app.ui_refresh_ms = 1000;
    g_app.monitor_interval_ms = 1000;
    g_app.autostop_cooldown_ms = 15000;
    g_app.stop_wait_ms = 10000;

g_app.tray_enabled = true;   // show tray icon by default
g_app.close_to_tray = false; // X closes normally unless enabled
g_app.tray_added = false;
g_app.force_quit = false;
g_app.taskbar_restart_msg = 0;

    g_app.activity_panel_w = 360;
    g_app.last_any_status_wall = 0;

    compute_default_cfg_path(g_app.cfg_path, MAX_PATH);

    g_app.ui_font = create_ui_font();
    g_app.app_title = exe_stem_title();
	
		WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
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
        wc.lpszClassName, g_app.app_title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 600,
        NULL, NULL, hInst, NULL
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