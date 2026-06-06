// app.h — Central type definitions, constants, IDs, App struct
#pragma once
#include "util.h"

// --------------------------------------------------
// Named constants
// --------------------------------------------------
constexpr long  kMaxConfigBytes      = 5 * 1024 * 1024;
constexpr DWORD kScmPollMinMs        = 100;
constexpr DWORD kScmPollMaxMs        = 2000;
constexpr DWORD kExePollFallbackMs   = 250;
constexpr int   kStatusDispBuf       = 128;
constexpr DWORD kForceTermWaitMs     = 2000;

// --------------------------------------------------
// Kind constants
// --------------------------------------------------
constexpr int KIND_SVC = 0;
constexpr int KIND_EXE = 1;

enum class ItemKind : uint8_t { Svc = 0, Exe = 1 };
inline constexpr int ki(ItemKind k) {
    return (k == ItemKind::Svc) ? KIND_SVC : KIND_EXE;
}
inline constexpr const wchar_t* kind_name(ItemKind k) {
    return (k == ItemKind::Svc) ? L"Service" : L"EXE";
}

#define APP_TITLE L"Windows Service / EXE Monitor"

inline std::wstring exe_stem_title() {
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

// --------------------------------------------------
// WM_APP messages
// --------------------------------------------------
constexpr UINT WM_APP_LOG              = WM_APP + 1;
constexpr UINT WM_APP_REQUEST_SAVE     = WM_APP + 3;
constexpr UINT WM_APP_RESTART_UI_TIMER = WM_APP + 4;
constexpr UINT WM_APP_PROFILE_SWITCH   = WM_APP + 6;
constexpr UINT WM_APP_STATUS_GEN       = WM_APP + 7;
constexpr UINT WM_APP_MODEL_DIRTY      = WM_APP + 8;
constexpr UINT WM_APP_TRAYICON         = WM_APP + 10;
constexpr UINT WM_APP_INITIAL_LAYOUT   = WM_APP + 42;

// --------------------------------------------------
// Timer IDs
// --------------------------------------------------
constexpr UINT_PTR TIMER_DEBOUNCE_SAVE   = 102;
constexpr UINT_PTR TIMER_UI_REFRESH      = 103;
constexpr UINT_PTR TIMER_LIVE_RESIZE     = 104;
constexpr UINT_PTR TIMER_SEARCH_DEBOUNCE = 105;

constexpr int MAX_LOG_LINES = 1000;

// --------------------------------------------------
// Menu IDs
// --------------------------------------------------
constexpr int IDM_PREFS             = 40001;
constexpr int IDM_HELP              = 40002;
constexpr int IDM_EXIT              = 40003;
constexpr int IDM_PROFILES          = 40008;
constexpr int IDM_DARKMODE          = 40010;

constexpr int IDM_TRAY_SHOWHIDE      = 40004;
constexpr int IDM_TRAY_PREFS         = 40005;
constexpr int IDM_TRAY_CLOSE_TO_TRAY = 40006;
constexpr int IDM_TRAY_EXIT          = 40007;
constexpr int IDM_TRAY_PROFILES      = 40009;
constexpr int IDM_TRAY_DARKMODE      = 40011;
constexpr int IDM_RESET_COLUMNS      = 40012;
constexpr int IDM_HOTKEYS            = 40013;
constexpr int IDM_EXIT_TO_TRAY       = 40014;
constexpr int IDM_DIAGNOSTICS        = 40015;
constexpr int IDM_TAB_SVC            = 40016;
constexpr int IDM_TAB_EXE            = 40017;
constexpr int IDM_SAVE_CONFIG_NOW    = 40018;
constexpr int IDM_FOCUS_MAIN_LIST    = 40019;
constexpr int IDM_REFRESH_UI         = 40020;
constexpr int IDM_FOCUS_SEARCH       = 40025;
constexpr int IDM_TOGGLE_ACTIVITY    = 40026;

constexpr int IDM_CTX_STOP           = 40100;
constexpr int IDM_CTX_REMOVE         = 40101;
constexpr int IDM_CTX_TOGGLE_AUTO    = 40102;
constexpr int IDM_CTX_COPY_NAME      = 40103;
constexpr int IDM_CTX_OPEN_LOC       = 40104;
constexpr int IDM_LOG_COPY_LINE      = 40110;
constexpr int IDM_LOG_CLEAR_ALL      = 40111;

// --------------------------------------------------
// Control IDs
// --------------------------------------------------
constexpr int IDC_TOOLBAR_TAB_SVC   = 5010;
constexpr int IDC_TOOLBAR_TAB_EXE   = 5011;
constexpr int IDC_TOOLBAR_SETTINGS  = 5012;
constexpr int IDC_TOOLBAR_PROFILES  = 5013;
constexpr int IDC_TOOLBAR_TRAY      = 5014;
constexpr int IDC_TOOLBAR_QUIT      = 5015;
constexpr int IDC_ACTION_ADD_EDIT   = 5020;
constexpr int IDC_ACTION_ADD_BTN    = 5021;
constexpr int IDC_ACTION_BROWSE_BTN = 5022;
constexpr int IDC_ACTION_STOP_BTN   = 5023;
constexpr int IDC_ACTION_START_BTN  = 5024;
constexpr int IDC_ACTION_REMOVE_BTN = 5025;
constexpr int IDC_DETAIL_PANEL      = 5030;
constexpr int IDC_DETAIL_NAME       = 5031;
constexpr int IDC_DETAIL_STATUS_LBL = 5032;
constexpr int IDC_DETAIL_AUTOSTOP   = 5033;
constexpr int IDC_DETAIL_DESC       = 5034;
constexpr int IDC_DETAIL_STOP_BTN   = 5035;
constexpr int IDC_DETAIL_START_BTN  = 5036;
constexpr int IDC_DETAIL_REMOVE_BTN = 5037;
constexpr int IDC_DETAIL_COPY_BTN   = 5038;
constexpr int IDC_DETAIL_SEP        = 5039;
constexpr int IDC_ACTIVITY_COLLAPSE = 5040;
constexpr int IDC_HSPLIT_ACTIVITY   = 7005;

constexpr int IDC_STATUSBAR      = 5900;

constexpr int IDC_LV_SVC         = 6001;
constexpr int IDC_LV_EXE         = 6002;
constexpr int IDC_SVC_SEARCH_EDIT = 6101;
constexpr int IDC_EXE_SEARCH_EDIT = 6102;

constexpr int IDC_ACTIVITY        = 7001;
constexpr int IDC_SPLITTER        = 7002;
constexpr int TRAY_UID            = 1;

constexpr int IDC_HOTKEYS_LIST  = 7201;
constexpr int IDC_HOTKEYS_COPY  = 7202;

constexpr int IDC_PREF_UI_MS         = 8001;
constexpr int IDC_PREF_MON_S         = 8002;
constexpr int IDC_PREF_CD_S          = 8003;
constexpr int IDC_PREF_SW_S          = 8004;
constexpr int IDC_PREF_APPLY         = 8005;
constexpr int IDC_PREF_TRAY_ENABLE   = 8006;
constexpr int IDC_PREF_CLOSE_TO_TRAY = 8007;
constexpr int IDC_PREF_DARKMODE      = 8008;

constexpr int IDC_PROF_LIST          = 8101;
constexpr int IDC_PROF_NAME_EDIT     = 8102;
constexpr int IDC_PROF_ADD           = 8103;
constexpr int IDC_PROF_RENAME        = 8104;
constexpr int IDC_PROF_DELETE        = 8105;
constexpr int IDC_PROF_MOVE_UP       = 8106;
constexpr int IDC_PROF_MOVE_DOWN     = 8107;
constexpr int IDC_PROF_ACTIVE_LABEL  = 8108;
constexpr int IDC_WATCH_LIST         = 8201;
constexpr int IDC_WATCH_EDIT         = 8202;
constexpr int IDC_WATCH_ADD          = 8203;
constexpr int IDC_WATCH_REMOVE       = 8204;

constexpr int IDC_PCONT_SVC_LIST     = 8301;
constexpr int IDC_PCONT_SVC_EDIT     = 8302;
constexpr int IDC_PCONT_SVC_AUTOSTOP = 8303;
constexpr int IDC_PCONT_SVC_ADD      = 8304;
constexpr int IDC_PCONT_SVC_REMOVE   = 8305;
constexpr int IDC_PCONT_SVC_TOGGLE   = 8306;

constexpr int IDC_PCONT_EXE_LIST     = 8311;
constexpr int IDC_PCONT_EXE_EDIT     = 8312;
constexpr int IDC_PCONT_EXE_AUTOSTOP = 8313;
constexpr int IDC_PCONT_EXE_ADD      = 8314;
constexpr int IDC_PCONT_EXE_REMOVE   = 8315;
constexpr int IDC_PCONT_EXE_TOGGLE   = 8316;

constexpr int IDC_PSTART_EXE_LIST    = 8321;
constexpr int IDC_PSTART_EXE_EDIT    = 8322;
constexpr int IDC_PSTART_EXE_ADD     = 8323;
constexpr int IDC_PSTART_EXE_REMOVE  = 8324;
constexpr int IDC_PROFILES_TAB       = 8325;
constexpr int IDC_PROF_TAB_ACTIVATION = 8326;
constexpr int IDC_PROF_TAB_START      = 8327;
constexpr int IDC_PROF_TAB_SVC        = 8328;
constexpr int IDC_PROF_TAB_EXE        = 8329;

// --------------------------------------------------
// Layout constants
// --------------------------------------------------
constexpr int SPLITTER_WIDTH     = 6;   // vertical splitter (at 96 DPI)
constexpr int HSPLITTER_HEIGHT   = 6;   // horizontal splitter (at 96 DPI)
constexpr int LAYOUT_PAD         = 4;   // standard inter-control padding (at 96 DPI)
constexpr int COLLAPSE_BTN_H    = 22;  // activity collapse button height (at 96 DPI)
constexpr int MIN_CONTENT_ABOVE_ACTIVITY = 120; // min space for list area above activity (at 96 DPI)
constexpr int DETAIL_BTN_GAP    = 6;   // gap between detail panel buttons (at 96 DPI)

constexpr int NUM_LV_COLS       = 5;   // Auto Stop, Name, Status, Description, Last Update
constexpr int TOOLBAR_H         = 38;  // at 96 DPI
constexpr int ACTION_STRIP_H    = 34;  // at 96 DPI
constexpr int DETAIL_MIN_W      = 180;
constexpr int DETAIL_DEFAULT_W  = 260;
constexpr int ACTIVITY_MIN_H    = 50;
constexpr int ACTIVITY_DEFAULT_H = 130;

// --------------------------------------------------
// Dark-mode palette constants (reused across files)
// --------------------------------------------------
constexpr COLORREF DK_PRESSED     = RGB(55, 55, 55);   // pressed/selected button background
constexpr COLORREF DK_BORDER      = RGB(70, 70, 70);   // button/control border
constexpr COLORREF DK_HIGHLIGHT   = RGB(80, 80, 80);   // splitter grip, hover highlight
constexpr COLORREF DK_SEPARATOR   = RGB(60, 60, 60);   // separator lines
constexpr COLORREF DK_ACCENT      = RGB(60, 120, 200); // accent (active tab underline)
constexpr COLORREF LT_ACCENT      = RGB(0, 90, 180);   // light-mode accent

// --------------------------------------------------
// Data model
// --------------------------------------------------
struct Item {
    ItemKind kind = ItemKind::Svc;
    uint32_t uid = 0;
    std::wstring name;
    std::wstring name_lower;
    bool auto_stop = false;
    int img = 0;
    std::wstring exe_path;

    bool svc_desc_loaded = false;
    DWORD svc_desc_last_err = 0;
    std::wstring svc_desc;

    wchar_t  last_status[32]{};
    time_t   last_update_wall = 0;
    // Cached numeric classification of last_status — avoids per-cell _wcsicmp
    // in the ListView custom-draw hot path. Updated whenever last_status is
    // written. 0=other/unknown, 1=running, 2=stopped.
    uint8_t  status_kind = 0;

    wchar_t  ui_last_status[32]{};
    time_t   ui_last_update_wall = 0;
    wchar_t  ui_cache_status_disp[kStatusDispBuf]{};
    wchar_t  ui_cache_last_text[64]{};

    uint32_t status_gen = 0;
    uint32_t ui_applied_gen = 0;
    uint64_t last_autostop_mono_ms = 0;
    int      autostop_count = 0;

    uint32_t svc_stop_fail_streak = 0;
    uint64_t svc_stop_suppress_until_ms = 0;
    DWORD    svc_last_stop_err = 0;
};

// Write last_status and keep status_kind in sync. All code paths that update
// Item::last_status MUST go through this helper so the custom-draw hot path
// (which reads status_kind, not the string) stays correct.
inline void set_item_status(Item& it, const wchar_t* s) {
    if (!s) s = L"";
    StringCchCopyW(it.last_status, 32, s);
    if (_wcsicmp(s, L"running") == 0)      it.status_kind = 1;
    else if (_wcsicmp(s, L"stopped") == 0) it.status_kind = 2;
    else                                    it.status_kind = 0;
}

struct ItemList {
    std::vector<std::unique_ptr<Item>> v;

    struct WStrCIHash {
        using is_transparent = void;
        static inline uint64_t fnv1a64_ci(std::wstring_view s) noexcept {
            uint64_t h = 14695981039346656037ULL;
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

inline void list_init(ItemList* L) {
    if (!L) return;
    L->v.clear();
    L->by_name.clear();
}

inline Item* list_find(ItemList* L, const wchar_t* name) {
    if (!L || !name || !name[0]) return nullptr;
    auto it = L->by_name.find(name);
    return (it == L->by_name.end()) ? NULL : it->second;
}

// --------------------------------------------------
// Profile / config snapshots
// --------------------------------------------------
struct ItemRow {
    std::wstring name;
    bool auto_stop = false;
    std::wstring exe_path;
};

struct StartItem {
    std::wstring target;
};

// UI refresh fallback timer range (ms). The UI is primarily push-driven by the
// monitor thread; this timer only catches theme regeneration, deferred listview
// layout, and statusbar ticks. Lower = snappier fallback recovery + slightly
// more CPU; higher = near-zero CPU but staler fallback paths.
constexpr int UI_REFRESH_MS_MIN = 250;
constexpr int UI_REFRESH_MS_MAX = 5000;
constexpr int UI_REFRESH_MS_DEFAULT = 1000;

struct ConfigSnapshot {
    int ui_refresh_ms = UI_REFRESH_MS_DEFAULT;
    int monitor_interval_ms = 1000;
    int autostop_cooldown_ms = 15000;
    int stop_wait_ms = 10000;
    bool tray_enabled = true;
    bool close_to_tray = false;
    std::vector<ItemRow> items[2];
};

struct ProfileSnapshot {
    std::wstring name;
    std::vector<std::wstring> watch_exes;
    std::vector<std::wstring> watch_keys_lower;
    std::vector<StartItem> start_items;
    ConfigSnapshot cfg;
};

inline void cfg_clear_items(ConfigSnapshot& c) {
    c.items[KIND_SVC].clear();
    c.items[KIND_EXE].clear();
}

// --------------------------------------------------
// Display-name cache
// --------------------------------------------------
struct DispCache {
    std::unordered_map<std::wstring, std::wstring> m;
    uint64_t built_at_ms = 0;  // monotonic timestamp of last rebuild
};

inline const wchar_t* dispcache_lookup(DispCache* D, const wchar_t* disp) {
    if (!D || !disp) return nullptr;
    std::wstring tmp(disp);
    str_lower_inplace(tmp);
    auto it = D->m.find(tmp);
    if (it == D->m.end()) return nullptr;
    return it->second.c_str();
}

// --------------------------------------------------
// Action queue types
// --------------------------------------------------
enum { ACTION_STOP = 0, ACTION_START = 1, ACTION_LAUNCH_ITEM = 2, ACTION_CLOSE_STARTED = 3 };

struct Action {
    ItemKind kind = ItemKind::Svc;
    int op = ACTION_STOP;
    std::wstring name;
    std::wstring reason;
    int wait_ms = 0;
    std::wstring target;
    DWORD pid = 0;
    unique_handle hproc;
};

// --------------------------------------------------
// Sub-structs
// --------------------------------------------------
struct ThemeState {
    bool dark_mode = false;
    bool owns_brushes = false;
    UINT built_dpi = 0;
    bool built_dark = false;

    COLORREF col_bg = 0;
    COLORREF col_panel = 0;
    COLORREF col_edit_bg = 0;
    COLORREF col_text = 0;
    COLORREF col_text_dim = 0;
    HBRUSH br_bg = NULL;
    HBRUSH br_panel = NULL;
    HBRUSH br_edit = NULL;
    HBRUSH br_btn = NULL;

    // Cached GDI objects for hot-path painting (dark mode only, NULL in light mode)
    HBRUSH br_pressed = NULL;   // DK_PRESSED background
    HBRUSH br_border = NULL;    // DK_BORDER fill (selected row bg)
    HBRUSH br_accent = NULL;    // DK_ACCENT fill
    HPEN   pen_border = NULL;   // DK_BORDER outline
    HPEN   pen_accent = NULL;   // DK_ACCENT outline
    HPEN   pen_separator = NULL; // DK_SEPARATOR outline (detail panel)

    // Light-mode pill tab cached GDI (avoids per-paint create/delete)
    HBRUSH br_lt_accent = NULL;   // LT_ACCENT fill
    HPEN   pen_lt_accent = NULL;  // LT_ACCENT outline
    HPEN   pen_lt_border = NULL;  // light border outline RGB(180,180,180)

    std::atomic<uint32_t> gen{1};
};

struct ProfileState {
    ConfigSnapshot default_cfg;
    std::vector<ProfileSnapshot> profiles;
    int active = -1;
    bool have_default_cfg = false;

    std::shared_ptr<const std::vector<std::vector<std::wstring>>> watch_keys_sp;
    std::shared_ptr<const std::vector<std::vector<uint64_t>>> watch_hashes_sp;

    std::vector<StartedProc> started_procs;
    std::unordered_set<std::wstring> started_exes;
};

struct TrayState {
    bool enabled = false;
    bool close_to_tray = false;
    bool added = false;
    bool force_quit = false;
    NOTIFYICONDATAW nid{};
    UINT taskbar_restart_msg = 0;
};

struct ConfigSettings {
    int ui_refresh_ms = UI_REFRESH_MS_DEFAULT;
    int monitor_interval_ms = 1000;
    int autostop_cooldown_ms = 15000;
    int stop_wait_ms = 10000;
    wchar_t cfg_path[MAX_PATH]{};
};

constexpr int kActionWorkerCount = 4;

struct ThreadState {
    std::jthread monitor_thread;
    std::vector<std::jthread> action_threads;

    std::condition_variable_any mon_cv;
    bool mon_wake = false;  // set true + notify to wake monitor immediately

    std::mutex action_mtx;
    std::condition_variable action_cv;
    std::deque<Action> action_q;

    bool save_pending = false;
};

// --------------------------------------------------
// App State (god object with sub-structs)
// --------------------------------------------------
// Layout / resize mechanics (UI-thread only): splitter drag tracking, the
// one-time initial-layout stabilization, and live-resize state. Grouped out of
// the App god-object (item 4.2) since these fields are only touched together by
// the layout/resize code paths.
struct LayoutState {
    bool splitter_dragging = false;
    int  splitter_drag_start_x = 0;
    int  splitter_drag_start_w = 0;
    bool hsplit_dragging = false;
    int  hsplit_drag_start_y = 0;
    int  hsplit_drag_start_h = 0;

    bool did_initial_layout = false;
    bool initial_layout_scheduled = false;
    UINT initial_layout_tries = 0;

    bool in_size_move = false;
};

struct App {
    HWND hwnd = NULL;
    HACCEL accel = NULL;

    HMENU menu_settings = NULL;
    UINT dpi = 96;

    std::wstring app_title;

    HWND center_page_svc = NULL;
    HWND center_page_exe = NULL;

    HWND lv_svc = NULL;
    HWND lv_exe = NULL;

    HWND svc_search = NULL;
    HWND exe_search = NULL;

    HIMAGELIST il_svc = NULL;
    HIMAGELIST il_exe = NULL;

    std::wstring filter[2];
    std::vector<uint32_t> view_uids[2];

    bool filter_pending[2]{};
    DWORD filter_debounce_last_ms = 0;

    bool cols_have[2]{};
    int  cols_w[2][NUM_LV_COLS]{};
    int  cols_order[2][NUM_LV_COLS]{};

    int  sort_col[2]{ -1, -1 };   // -1 = no sort active
    bool sort_asc[2]{ true, true };

    HWND svc_empty = NULL;
    HWND exe_empty = NULL;

    // -- Toolbar --
    HWND toolbar_tab_svc = NULL;
    HWND toolbar_tab_exe = NULL;
    HWND toolbar_settings = NULL;
    HWND toolbar_profiles = NULL;
    HWND toolbar_tray = NULL;
    HWND toolbar_quit = NULL;

    // -- Action strip --
    HWND action_add_edit = NULL;
    HWND action_add_btn = NULL;
    HWND action_browse_btn = NULL;
    HWND action_stop_btn = NULL;
    HWND action_start_btn = NULL;
    HWND action_remove_btn = NULL;

    // -- Detail panel (right side) --
    HWND detail_panel = NULL;
    HWND detail_name = NULL;
    HWND detail_status_lbl = NULL;
    HWND detail_autostop = NULL;
    HWND detail_desc = NULL;
    HWND detail_stop_btn = NULL;
    HWND detail_start_btn = NULL;
    HWND detail_remove_btn = NULL;
    HWND detail_copy_btn = NULL;
    HWND detail_sep = NULL;
    int  detail_panel_w = DETAIL_DEFAULT_W;

    // -- Activity (bottom) --
    HWND activity_collapse_btn = NULL;
    int  activity_panel_h = ACTIVITY_DEFAULT_H;
    bool activity_collapsed = false;

    // -- Active tab tracking --
    int  active_tab = 0;  // 0=Services, 1=EXEs

    HWND activity = NULL;
    HWND statusbar = NULL;
    std::wstring statusbar_text;  // cached text for owner-drawn statusbar part
    HWND hwnd_tooltip = NULL;

    HWND splitter = NULL;
    HWND hsplit_activity = NULL;
    LayoutState layout_state;   // splitter/hsplit drag, initial-layout, live-resize
    HFONT ui_font = NULL;
    std::unordered_map<UINT, HFONT> ui_font_cache;

    std::shared_mutex mtx;
    ItemList items[2];

    uint32_t next_uid = 1;
    std::unordered_map<uint32_t, Item*> uid_map;

    uint32_t items_gen[2]{};

    uint32_t status_gen[2]{};
    std::vector<Item*> dirty_status[2];
    std::unordered_map<const Item*, int> lv_row_of_item[2];

    DispCache disp_cache;

    ConfigSettings cfg;
    ProfileState prof;
    TrayState tray;

    WINDOWPLACEMENT last_wp{};
    bool have_last_wp = false;
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0;
    bool win_maximized = false;
    bool have_win_rect = false;

    ThemeState theme;
    uint32_t ui_seen_theme_gen = 0;

    uint32_t ui_seen_status_gen[2]{};
    bool ui_dirty_posted = false;
    bool ui_timer_suspended = false;

    ThreadState threads;

    bool suppress_lv_notify = false;
    bool lv_needs_layout[2]{};
    time_t last_any_status_wall = 0;

    // Deferred log messages queued before the UI window exists (e.g. during load_config).
    std::vector<std::wstring> deferred_logs;
};

// --------------------------------------------------
// Debug lock order enforcement
// --------------------------------------------------
#if !defined(NDEBUG)
extern thread_local int g_model_lock_depth;
inline void dbg_model_lock_inc() { ++g_model_lock_depth; }
inline void dbg_model_lock_dec() { --g_model_lock_depth; }
inline void dbg_assert_model_unlocked(const char* where) {
    if (g_model_lock_depth != 0) {
        OutputDebugStringA("LOCK ORDER VIOLATION: action-queue API called while holding self.mtx at ");
        OutputDebugStringA(where);
        OutputDebugStringA("\n");
        DebugBreak();
    }
}
#else
inline void dbg_model_lock_inc() {}
inline void dbg_model_lock_dec() {}
inline void dbg_assert_model_unlocked(const char*) {}
#endif

// Exclusive (write) lock on the model — use when modifying model data.
struct ModelLockGuard {
    std::lock_guard<std::shared_mutex> lk;
    explicit ModelLockGuard(App& self_) : lk(self_.mtx) { dbg_model_lock_inc(); }
    ~ModelLockGuard() { dbg_model_lock_dec(); }
};

// Shared (read) lock on the model — use when only reading model data.
// Multiple readers can hold this concurrently; writers are blocked until all readers release.
struct ModelReadGuard {
    std::shared_lock<std::shared_mutex> lk;
    explicit ModelReadGuard(App& self_) : lk(self_.mtx) { dbg_model_lock_inc(); }
    ~ModelReadGuard() { dbg_model_lock_dec(); }
};

struct SuppressLvNotifyGuard {
    App& self;
    bool prev;
    explicit SuppressLvNotifyGuard(App& s) : self(s), prev(s.suppress_lv_notify) {
        self.suppress_lv_notify = true;
    }
    ~SuppressLvNotifyGuard() { self.suppress_lv_notify = prev; }
    SuppressLvNotifyGuard(const SuppressLvNotifyGuard&) = delete;
    SuppressLvNotifyGuard& operator=(const SuppressLvNotifyGuard&) = delete;
};

// --------------------------------------------------
// UID / item registration helpers
// --------------------------------------------------
inline Item* uid_to_item_ptr_unlocked(App& self, uint32_t uid) {
    auto it = self.uid_map.find(uid);
    return (it == self.uid_map.end()) ? nullptr : it->second;
}

void item_cache_name_lower(Item* it);
bool register_item_locked(App& self, ItemKind kind, Item* it);
void unregister_item_locked(App& self, ItemKind kind, Item* it);
void unregister_all_items_locked(App& self, ItemKind kind);

// --------------------------------------------------
// Action queue helpers (must not hold self.mtx!)
// --------------------------------------------------
void   action_enqueue(App& self, Action&& a);
void   action_enqueue_batch(App& self, std::vector<Action>&& batch);
size_t action_qdepth(App& self);
void   action_clear(App& self);
bool   action_pop_wait(std::stop_token st, App& self, Action& out);

// Convenience overload for simple stop/start actions
void   action_enqueue(App& self, ItemKind kind, int op, const wchar_t* name, const wchar_t* reason, int wait_ms);

// --------------------------------------------------
// Monitor status types (used by monitor and UI)
// --------------------------------------------------
struct StatusStr { wchar_t buf[32]; };

struct NameIdx {
    const wchar_t* name;
    size_t idx;
};

bool binsearch_nameidx(const NameIdx* arr, size_t n, const wchar_t* key, size_t* out_idx);

// --------------------------------------------------
// Cross-module function declarations
// --------------------------------------------------

// main.cpp / messaging
void post_log(App& self, const wchar_t* text);
void log_linef(App& self, const wchar_t* fmt, ...);
void post_status_bulk(App& self, ItemKind kind, const std::vector<std::wstring>& names, const std::vector<uint32_t>& uids, const std::vector<StatusStr>& statuses, size_t n, time_t wall_ts);
void request_save_debounced(App& self);
void post_model_dirty(App& self);

// theme
void theme_apply_to_control(App& self, HWND child);
void theme_compute(App& self);
void theme_apply_all_controls(App& self);
void theme_compute_for_window(App& self, HWND hwnd);
HFONT theme_ensure_font_for_dpi(App& self, UINT dpi);
void theme_apply_to_window(App& self, HWND hwnd);
HBRUSH theme_handle_ctlcolor(App& self, UINT msg, WPARAM wParam, LPARAM lParam);
bool theme_draw_owner_button(App& self, const DRAWITEMSTRUCT* dis);
bool theme_draw_pill(App& self, const DRAWITEMSTRUCT* dis, bool is_selected);
bool theme_draw_owner_tab(App& self, const DRAWITEMSTRUCT* dis);
bool theme_draw_owner_menu(App& self, const DRAWITEMSTRUCT* dis);
LRESULT theme_handle_customdraw(App& self, NMHDR* nh, LPARAM lParam, bool& handled);
void theme_release_resources(App& self);

// darkmode
void darkmode_init_api();
void darkmode_set_app(bool want_dark);
void darkmode_allow_window(HWND hwnd, bool allow_dark);
bool try_set_explorer_theme_mode(HWND hwnd, bool dark);
LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR idSubclass, DWORD_PTR refData);
BOOL CALLBACK enum_theme_children_proc(HWND child, LPARAM lp);
BOOL CALLBACK enum_theme_thread_windows_proc(HWND hwnd, LPARAM lp);

// tray
void tray_remove(App& self);
void tray_add(App& self, HINSTANCE hInst);
void tray_sync(App& self, HINSTANCE hInst);
void tray_show_menu(HWND hwnd);

// config
void load_config(App& self);
void save_config_now(App& self);
bool parse_setting(App& self, const wchar_t* key, const wchar_t* val);
void normalize_exe_input(const wchar_t* raw, wchar_t* out, size_t cch_out);
void resolve_exe_launch_path(const wchar_t* raw, wchar_t* out, size_t cch_out);
void compute_default_cfg_path(wchar_t* out, size_t cch);

// monitor / service-process helpers
void   rebuild_profile_watch_keys_locked(App& self);
int    process_count_by_name_lower(const wchar_t* exe_lower);
std::wstring exe_watch_key_lower(const std::wstring& raw);
uint64_t fnv1a64_exe_lower_hash(const wchar_t* s);
bool   build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes, std::vector<NameIdx>* out_names_opt);
bool   terminate_all_by_name_lower(const wchar_t* exe_lower, int* terminated, int* failed,
                                   std::vector<unique_handle>* out_handles);
void   format_winerr(DWORD e, wchar_t* out, size_t cch);
bool   query_service_status_fast(const wchar_t* svc_name, wchar_t* out_status, size_t cch);
bool   stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch, DWORD* out_err);
bool   start_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch);
bool   start_exe_and_wait(const wchar_t* exe_lower_name, const wchar_t* launch_path_opt, int wait_ms, wchar_t* out_result, size_t cch);
bool   enum_services_build_disp_cache(DispCache* out_cache);
void   resolve_service_name(App& self, const wchar_t* raw, wchar_t* out, size_t cch);
std::wstring query_service_description_best_effort(const std::wstring& svc_name, DWORD& out_err);
std::wstring normalize_start_target_best_effort(const std::wstring& raw);
std::wstring normalize_start_target_best_effort(const wchar_t* raw);
bool   shell_open_and_track_process(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch);
void   close_process_best_effort(DWORD pid, HANDLE hproc_in, int wait_ms, wchar_t* out_result, size_t cch);

// monitor thread
void monitor_thread_main(std::stop_token st, App* self_ptr);
void action_thread_main(std::stop_token st, App* self_ptr);

// UI listview
void lv_set_columns(HWND lv, const wchar_t* c0, const wchar_t* c1, const wchar_t* c2, const wchar_t* c3, const wchar_t* c4);
void lv_set_columns(HWND lv, const wchar_t* c0, const wchar_t* c1, const wchar_t* c2, const wchar_t* c3);
void lv_scale_columns(HWND lv, UINT old_dpi, UINT new_dpi);
int  lv_find_item_by_name(HWND lv, const wchar_t* name);
int  lv_find_item_by_uid(HWND lv, uint32_t uid);
void lv_autosize_status_last(App& self, ItemKind kind);
void lv_apply_name_fill(App& self, ItemKind kind);
bool lv_set_row_with(App& self, HWND lv, Item* it, const wchar_t* status_raw, time_t wall_ts);
bool lv_update_row_existing_with(App& self, HWND lv, ItemKind kind, Item* it, const wchar_t* status_raw, time_t wall_ts, int* out_row_idx);
bool lv_set_row(App& self, HWND lv, Item* it);
bool lv_get_selected_name(HWND lv, wchar_t* out, size_t cch);
void activity_append(App& self, HWND lb, const wchar_t* line);
void ui_apply_status_updates(App& self, ItemKind kind);
void log_linef(App& self, const wchar_t* fmt, ...);
bool item_matches_filter(const Item* it, const std::wstring& flt_lower);
void rebuild_listview_filtered(App& self, ItemKind kind);
void lv_sort_view(App& self, ItemKind kind);
void lv_apply_sort_header(App& self, ItemKind kind);
void lv_on_column_click(App& self, ItemKind kind, int col);
Item* lv_get_selected_item_ptr(App& self, ItemKind kind);
void toggle_autostop_selected(App& self, ItemKind kind);
bool cmd_set_autostop_uid(App& self, uint32_t uid, bool enabled, std::wstring* out_name_opt = nullptr);
void show_item_details(App& self, ItemKind kind);
void update_detail_panel(App& self, ItemKind kind, Item* it);
void open_file_location_best_effort(App& self, const wchar_t* exeOrPath);

// UI layout
void show_only(HWND a, HWND b, bool show_a);
void layout(App& self, HWND hwnd);
void build_ui(App& self, HWND hwnd);
void update_statusbar(App& self);
void apply_filter_now(App& self, ItemKind kind);
void capture_columns_now(App& self, ItemKind kind);
void apply_saved_columns(App& self, ItemKind kind);
void register_splitter_class();
void register_page_class();

// profiles
bool parse_setting_to_cfg(ConfigSnapshot& cfg, const wchar_t* key, const wchar_t* val);
void rebuild_runtime_items_locked(App& self, const ConfigSnapshot& cfg);
void snapshot_from_runtime_locked(App& self, ConfigSnapshot& out);
void snapshot_active_from_runtime_locked(App& self);
void apply_profile_index(App& self, int idx);
const wchar_t* active_profile_name_locked(const App& self);

// dialogs
void open_prefs_dialog(App& self, HWND parent);
void open_help_dialog(App& self, HWND parent);
void open_hotkeys_dialog(App& self, HWND parent);
void open_diagnostics_dialog(App& self, HWND parent);
void open_profiles_dialog(App& self, HWND parent);

// main window
void show_main_window(App& self);
void hide_main_window(App& self);
void app_request_exit(App& self, HWND hwnd);
HICON load_app_icon(HINSTANCE hInst, int cx, int cy);
