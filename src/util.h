// util.h — RAII wrappers, string helpers, DPI, admin, misc utilities
#pragma once

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

// Win32 headers
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <strsafe.h>

// C++ standard library
#include <algorithm>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <new>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <string_view>
#include <chrono>

// C standard library
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwctype>

// --------------------------------------------------
// Macros (must be macros: used by preprocessor / variadic)
// --------------------------------------------------
#define LOAD_PROC(module, procName, Type, outVar)            \
    do {                                                     \
        FARPROC _fp = GetProcAddress((module), (procName));  \
        (outVar) = (Type)(void*)(_fp);                       \
    } while (0)

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif
#ifndef MSGFLT_ADD
#define MSGFLT_ADD 1
#endif

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

struct StartedProc {
    std::wstring started_target;
    std::wstring exe_lower;
    DWORD pid = 0;
    unique_handle hproc;
};

// --------------------------------------------------
// DLL cache (dwmapi.dll, uxtheme.dll)
// --------------------------------------------------
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(void*);
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef int  (WINAPI *PFN_GetSystemMetricsForDpi)(int, UINT);
typedef BOOL (WINAPI *PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

struct DllCache {
    bool inited = false;
    HMODULE dwm = NULL;
    HMODULE ux  = NULL;
    HMODULE user32 = NULL;
    PFN_DwmSetWindowAttribute DwmSetWindowAttribute = nullptr;
    PFN_SetWindowTheme        SetWindowTheme         = nullptr;
    PFN_GetDpiForWindow       GetDpiForWindow        = nullptr;
    PFN_GetDpiForSystem       GetDpiForSystem        = nullptr;
};

extern DllCache g_dll_cache;

void dll_cache_init();

// --------------------------------------------------
// DPI helpers
// --------------------------------------------------

template <typename T>
inline T get_proc(HMODULE mod, const char* name) {
    if (!mod || !name) return (T)nullptr;
    FARPROC fp = GetProcAddress(mod, name);
    return reinterpret_cast<T>(reinterpret_cast<void*>(fp));
}

UINT  get_dpi_for_hwnd(HWND hwnd);
int   dpi_scale(UINT dpi, int v96);
void  listbox_adjust_item_height(HWND lb, UINT dpi);
void  enable_dpi_awareness();
HFONT create_ui_font_for_dpi(UINT dpi);
void  apply_font_recursive(HWND root, HFONT font);

// --------------------------------------------------
// Child-window scaling
// --------------------------------------------------
struct ScaleChildrenCtx {
    HWND parent;
    UINT old_dpi;
    UINT new_dpi;
};

BOOL CALLBACK enum_scale_children_proc(HWND child, LPARAM lp);
void scale_children(HWND parent, UINT old_dpi, UINT new_dpi);

// --------------------------------------------------
// Small helpers
// --------------------------------------------------
uint64_t now_mono_ms();
void     fmt_time_local(wchar_t* buf, size_t cch, time_t t);
void     trim_ws_inplace(wchar_t* s);
void     str_lower_inplace(std::wstring& s);
void     str_lower_inplace(wchar_t* s);
int      clamp_int(int v, int lo, int hi);
bool     ends_with_i(const wchar_t* s, const wchar_t* suf);
void     msgbox_err(HWND parent, const wchar_t* title, const wchar_t* text);
void     msgbox_info(HWND parent, const wchar_t* title, const wchar_t* text);
void     msgbox_warn(HWND parent, const wchar_t* title, const wchar_t* text);

// --------------------------------------------------
// Facelift helpers
// --------------------------------------------------
void         set_cue_banner(HWND edit, const wchar_t* text);
bool         try_enable_mica_backdrop(HWND hwnd);
bool         clipboard_set_text(HWND hwnd, const wchar_t* text);
std::wstring to_lower_ws(const wchar_t* s);
bool         looks_like_path(const wchar_t* s);

// --------------------------------------------------
// Dark titlebar / explorer theme
// --------------------------------------------------
bool try_enable_dark_titlebar(HWND hwnd, bool enable);
bool try_set_explorer_theme(HWND hwnd);

// --------------------------------------------------
// UIPI message filter
// --------------------------------------------------
void allow_uipi_message(HWND hwnd, UINT msg);

// --------------------------------------------------
// Admin / UAC
// --------------------------------------------------
BOOL         is_running_as_admin();
std::wstring escape_argv(const wchar_t* arg);
BOOL         relaunch_as_admin();
void         ensure_admin_or_exit();

// --------------------------------------------------
// Service status string
// --------------------------------------------------
const wchar_t* svc_state_to_str(DWORD st);
