// darkmode.cpp — Dark mode API functions (UxTheme undocumented ordinals, theming callbacks)
#include "app.h"

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


void darkmode_init_api() {
    if (g_dm.inited) return;
    g_dm.inited = true;
    g_dm.ux = GetModuleHandleW(L"uxtheme.dll");
    if (!g_dm.ux) g_dm.ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dm.ux) return;

    // Undocumented uxtheme ordinals (well-known, used by many Win32 dark-mode apps).
    // If they don't exist on this OS version, we simply no-op.
    constexpr int kOrdAllowDarkModeForWindow = 133;
    constexpr int kOrdSetPreferredAppMode    = 135;
    constexpr int kOrdFlushMenuThemes        = 136;
    g_dm.SetPreferredAppMode = dm_get_ordinal<DarkModeApi::PFN_SetPreferredAppMode>(g_dm.ux, kOrdSetPreferredAppMode);
    g_dm.FlushMenuThemes     = dm_get_ordinal<DarkModeApi::PFN_FlushMenuThemes>(g_dm.ux, kOrdFlushMenuThemes);
    g_dm.AllowDarkModeForWindow = dm_get_ordinal<DarkModeApi::PFN_AllowDarkModeForWindow>(g_dm.ux, kOrdAllowDarkModeForWindow);
}

void darkmode_set_app(bool want_dark) {
    darkmode_init_api();
    if (!g_dm.SetPreferredAppMode) return;
    // PreferredAppMode: Default=0, AllowDark=1, ForceDark=2, ForceLight=3
    g_dm.SetPreferredAppMode(want_dark ? 1 : 0);
    if (g_dm.FlushMenuThemes) g_dm.FlushMenuThemes();
}

void darkmode_allow_window(HWND hwnd, bool allow_dark) {
    if (!hwnd) return;
    darkmode_init_api();
    if (g_dm.AllowDarkModeForWindow) {
        g_dm.AllowDarkModeForWindow(hwnd, allow_dark ? TRUE : FALSE);
    }
}
bool try_set_explorer_theme_mode(HWND hwnd, bool dark) {
    if (!hwnd) return false;
    dll_cache_init();
    if (!g_dll_cache.SetWindowTheme) return false;
    g_dll_cache.SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return true;
}


// Custom-draw helpers for controls that don't fully honor DarkMode_Explorer on all builds.
// Custom-draw for listviews (items) so text/bk colors are correct in dark mode even when the control ignores LVM_* colors.
// Dark-mode push buttons: subclass paint so we don't rely on BS_OWNERDRAW + parent WM_DRAWITEM.
// This is more reliable because many buttons live inside container child windows (tab pages/panels),
// and WM_DRAWITEM would go to those parents, not the main window.
LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR /*idSubclass*/, DWORD_PTR refData) {
    App* self = reinterpret_cast<App*>(refData);
    // Owner-draw buttons (the tab "pills") are painted by their parent's
    // WM_DRAWITEM; we only add hover tracking for them.
    const bool owner_draw = (GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(hwnd, L"SEM_HOT")) {
            SetPropW(hwnd, L"SEM_HOT", (HANDLE)1);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"SEM_HOT");
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        if (self && !owner_draw) return 1;  // we paint the full rect
        break;
    case WM_ENABLE:
    case WM_THEMECHANGED:
    case WM_STYLECHANGED:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case BM_SETSTATE: {
        LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    case WM_PAINT: {
        if (!self || owner_draw) break;
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        // Double-buffer: rounded fills + text in two passes would flicker on hover.
        RECT rc{}; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, rc.right, rc.bottom) : NULL;
        if (mem && bmp) {
            HGDIOBJ oldb = SelectObject(mem, bmp);
            theme_paint_button(*self, hwnd, mem);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldb);
        } else {
            theme_paint_button(*self, hwnd, hdc);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemovePropW(hwnd, L"SEM_HOT");
        RemovePropW(hwnd, L"SEM_GLYPH");
        RemovePropW(hwnd, L"SEM_BTNSTYLE");
        RemoveWindowSubclass(hwnd, DarkButtonSubclassProc, 1);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Apply theming (including dark mode opt-in and owner-draw toggles) to a single control.
BOOL CALLBACK enum_theme_children_proc(HWND child, LPARAM lp) {
    App* self = reinterpret_cast<App*>(lp);
    if (!self) return TRUE;
    theme_apply_to_control(*self, child);
    EnumChildWindows(child, enum_theme_children_proc, lp);
    return TRUE;
}

// Forward declaration (defined in util.cpp, declared in util.h)
// bool try_enable_dark_titlebar(HWND hwnd, bool enable);

BOOL CALLBACK enum_theme_thread_windows_proc(HWND hwnd, LPARAM lp) {
    App* self = reinterpret_cast<App*>(lp);
    if (!self) return TRUE;
    theme_apply_to_window(*self, hwnd);
    return TRUE;
}
