#include "app.h"

// Forward declarations
static LRESULT CALLBACK SearchEditSubclassProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData);

// ----------------------------
// Page class
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
            HBRUSH br = self ? self->theme.br_bg : GetSysColorBrush(COLOR_BTNFACE);
            if (self && page == self->detail_panel) {
                // Detail "card": rounded, slightly raised surface with a hairline border.
                FillRect(hdc, &rc, self->theme.br_bg);
                theme_fill_round(hdc, rc, dpi_scale(self->dpi ? self->dpi : 96, 6),
                                 self->theme.col_card, self->theme.col_border);
            } else {
                FillRect(hdc, &rc, br);
            }
        }
        return 1;
    }
    return DefWindowProcW(page, msg, wParam, lParam);
}

void register_page_class(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = PageProc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = PAGE_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
}

// ----------------------------
// UI construction
// ----------------------------

// Forward declaration (defined below, declared in app.h)
void layout(App& self, HWND hwnd);

// Vertical splitter (between list area and detail panel)
static const wchar_t* SPLITTER_CLASS = L"SplitterBar_C";

static LRESULT CALLBACK SplitterProc(HWND sw, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* selfp = reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(sw), GWLP_USERDATA));
    if (!selfp) return DefWindowProcW(sw, msg, wParam, lParam);
    App& self = *selfp;
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN: {
        SetCapture(sw);
        self.layout_state.splitter_dragging = true;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(self.hwnd, &pt);

        self.layout_state.splitter_drag_start_x = pt.x;
        self.layout_state.splitter_drag_start_w = (self.detail_panel_w > 0) ? self.detail_panel_w : DETAIL_DEFAULT_W;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (self.layout_state.splitter_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(self.hwnd, &pt);

            int dx = pt.x - self.layout_state.splitter_drag_start_x;
            int desired = self.layout_state.splitter_drag_start_w - dx; // drag right => narrower detail panel
            self.detail_panel_w = desired;

            // Layout clamps sizes; keep it snappy during drags.
            layout(self, self.hwnd);
            return 0;
        }
        return 0;

    case WM_LBUTTONUP:
        if (self.layout_state.splitter_dragging) {
            self.layout_state.splitter_dragging = false;
            ReleaseCapture();
            request_save_debounced(self);
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        self.layout_state.splitter_dragging = false;
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(sw, &ps);
        RECT rc;
        GetClientRect(sw, &rc);

        // Flat gutter (the old etched 3D grip read as Windows 95). The resize
        // cursor is the affordance.
        FillRect(hdc, &rc, self.theme.br_bg);
        EndPaint(sw, &ps);
        return 0;
    }
    }
    return DefWindowProcW(sw, msg, wParam, lParam);
}

void register_splitter_class(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = SplitterProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SPLITTER_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_SIZEWE);
    RegisterClassW(&wc);
}

// Horizontal splitter (for activity panel resize)
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
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(sw, &pt);
        SetPropW(sw, L"SEM_DRAG", (HANDLE)(INT_PTR)1);
        SetPropW(sw, L"SEM_DRAG_Y0", (HANDLE)(INT_PTR)pt.y);
        SetPropW(sw, L"SEM_DRAG_H0", (HANDLE)(INT_PTR)self->activity_panel_h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self) break;
        if (GetCapture() != sw) break;
        if ((INT_PTR)GetPropW(sw, L"SEM_DRAG") != 1) break;

        int y0 = (int)(INT_PTR)GetPropW(sw, L"SEM_DRAG_Y0");
        int h0 = (int)(INT_PTR)GetPropW(sw, L"SEM_DRAG_H0");

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(sw, &pt);
        int dy = pt.y - y0;

        // Dragging UP increases activity height
        int newH = h0 - dy;
        self->activity_panel_h = newH;
        self->activity_collapsed = false;

        if (self->hwnd) layout(*self, self->hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (GetCapture() == sw) {
            ReleaseCapture();
            if (self) request_save_debounced(*self);
        }
        RemovePropW(sw, L"SEM_DRAG");
        RemovePropW(sw, L"SEM_DRAG_Y0");
        RemovePropW(sw, L"SEM_DRAG_H0");
        return 0;
    case WM_CAPTURECHANGED:
        RemovePropW(sw, L"SEM_DRAG");
        RemovePropW(sw, L"SEM_DRAG_Y0");
        RemovePropW(sw, L"SEM_DRAG_H0");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(sw, &ps);
        RECT rc; GetClientRect(sw, &rc);
        App* s = (App*)GetWindowLongPtrW(sw, GWLP_USERDATA);
        FillRect(hdc, &rc, s ? s->theme.br_bg : GetSysColorBrush(COLOR_3DFACE));
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

    WNDCLASSW wc{};
    wc.lpfnWndProc = HSplitterProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SEM_HSPLITTER";
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_SIZENS);
    RegisterClassW(&wc);
}

static HWND make_page(HWND parent) {
    register_page_class();
    return CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, parent, NULL, GetModuleHandleW(NULL), NULL);
}

void show_only(HWND a, HWND b, bool show_a) {
    ShowWindow(a, show_a ? SW_SHOW : SW_HIDE);
    ShowWindow(b, show_a ? SW_HIDE : SW_SHOW);
}

static void add_tooltip(HWND tip, HWND ctrl, const wchar_t* text) {
    if (!tip || !ctrl) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(ctrl);
    ti.uId = (UINT_PTR)ctrl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void build_ui(App& self, HWND hwnd) {
    register_splitter_class();
    register_hsplitter_class();

    HINSTANCE hInst = GetModuleHandleW(NULL);
    auto button = [&](HWND parent, const wchar_t* text, int id, DWORD extra_style = WS_VISIBLE) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | extra_style,
                               0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, hInst, NULL);
    };

    // ---- Header: tabs | search | profile chip + settings ----
    // Tabs stay BS_OWNERDRAW (theme_draw_pill draws the selector-bar style).
    self.toolbar_tab_svc = CreateWindowExW(0, L"BUTTON", L"Services",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_TAB_SVC, hInst, NULL);
    self.toolbar_tab_exe = CreateWindowExW(0, L"BUTTON", L"Executables",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_TAB_EXE, hInst, NULL);

    // Search edits (one per kind; shown for the active tab). Borderless: the
    // main window paints a rounded frame + magnifier around them.
    self.svc_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_SVC_SEARCH_EDIT, hInst, NULL);
    self.exe_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_EXE_SEARCH_EDIT, hInst, NULL);
    set_cue_banner(self.svc_search, L"Search services");
    set_cue_banner(self.exe_search, L"Search executables");
    SetWindowSubclass(self.svc_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);
    SetWindowSubclass(self.exe_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);

    // Profile "chip": shows the active profile, opens the Profiles dialog.
    self.toolbar_profiles = button(hwnd, L"Default", IDC_TOOLBAR_PROFILES);
    ui_button_style(self.toolbar_profiles, GLYPH_PROFILE, BTNSTYLE_SUBTLE);
    self.toolbar_settings = button(hwnd, L"Settings", IDC_TOOLBAR_SETTINGS);
    ui_button_style(self.toolbar_settings, GLYPH_SETTINGS, BTNSTYLE_SUBTLE | BTNSTYLE_ICON_ONLY);
    // The old in-window "minimize to tray" and "quit" buttons duplicated the
    // title bar / settings menu (both still available there and in the tray).
    self.toolbar_tray = NULL;
    self.toolbar_quit = NULL;

    // ---- Add row ----
    self.action_add_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_ACTION_ADD_EDIT, hInst, NULL);
    set_cue_banner(self.action_add_edit, L"Service name or display name…");
    SetWindowSubclass(self.action_add_edit, SearchEditSubclassProc, 2, (DWORD_PTR)&self);

    self.action_add_btn = button(hwnd, L"Add", IDC_ACTION_ADD_BTN);
    ui_button_style(self.action_add_btn, GLYPH_ADD, BTNSTYLE_ACCENT);
    self.action_browse_btn = button(hwnd, L"Browse…", IDC_ACTION_BROWSE_BTN, 0);
    ui_button_style(self.action_browse_btn, GLYPH_FOLDER, 0);
    // Stop/Start/Remove moved to the detail panel + context menu (they were
    // duplicated in both places).
    self.action_stop_btn = self.action_start_btn = self.action_remove_btn = NULL;

    // ---- Center pages (listviews) ----
    self.center_page_svc = make_page(hwnd);
    self.center_page_exe = make_page(hwnd);

    self.lv_svc = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_svc, (HMENU)(INT_PTR)IDC_LV_SVC, hInst, NULL);
    self.lv_exe = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_exe, (HMENU)(INT_PTR)IDC_LV_EXE, hInst, NULL);

    lv_set_columns(self.lv_svc, L"Auto-stop", L"Service", L"Status", L"Last change");
    lv_set_columns(self.lv_exe, L"Auto-stop", L"Executable", L"Status", L"Path", L"Last change");

    // Owner-data listviews: request state image callbacks so the checkbox (Auto Stop) renders.
    ListView_SetCallbackMask(self.lv_svc, LVIS_STATEIMAGEMASK);
    ListView_SetCallbackMask(self.lv_exe, LVIS_STATEIMAGEMASK);

    // Empty-state labels (shown when list has no rows)
    self.svc_empty = CreateWindowW(L"STATIC",
        L"No services monitored yet.\r\n\r\nType a service name (e.g. wuauserv) or its display name above and click Add.",
        WS_CHILD | SS_CENTER,
        0, 0, 10, 10, self.center_page_svc, NULL, NULL, NULL);
    self.exe_empty = CreateWindowW(L"STATIC",
        L"No executables monitored yet.\r\n\r\nType a program name (e.g. notepad.exe) above, or click Browse… to pick one.",
        WS_CHILD | SS_CENTER,
        0, 0, 10, 10, self.center_page_exe, NULL, NULL, NULL);
    ShowWindow(self.svc_empty, SW_HIDE);
    ShowWindow(self.exe_empty, SW_HIDE);

    apply_saved_columns(self, ItemKind::Svc);
    apply_saved_columns(self, ItemKind::Exe);

    try_set_explorer_theme(self.lv_svc);
    try_set_explorer_theme(self.lv_exe);

    // ---- Detail panel (right side "card") ----
    self.detail_panel = make_page(hwnd);
    HWND dp = self.detail_panel;

    self.detail_name = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_NAME, hInst, NULL);
    self.detail_kind = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_KIND, hInst, NULL);
    self.detail_sep = NULL;  // spacing replaces the old etched separator
    self.detail_status_lbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_STATUS_LBL, hInst, NULL);
    // Auto-stop is now an actual toggle here (it used to be a read-only label).
    self.detail_autostop = CreateWindowW(L"BUTTON", L"Keep stopped (auto-stop)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_AUTOSTOP, hInst, NULL);
    self.detail_meta = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_META, hInst, NULL);
    self.detail_desc = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 10, 10, dp, (HMENU)(INT_PTR)IDC_DETAIL_DESC, hInst, NULL);

    self.detail_stop_btn = button(dp, L"Stop", IDC_DETAIL_STOP_BTN);
    ui_button_style(self.detail_stop_btn, GLYPH_STOP, 0);
    self.detail_start_btn = button(dp, L"Start", IDC_DETAIL_START_BTN);
    ui_button_style(self.detail_start_btn, GLYPH_PLAY, 0);
    self.detail_remove_btn = button(dp, L"Remove", IDC_DETAIL_REMOVE_BTN);
    ui_button_style(self.detail_remove_btn, GLYPH_DELETE, 0);
    self.detail_copy_btn = button(dp, L"Copy name", IDC_DETAIL_COPY_BTN);
    ui_button_style(self.detail_copy_btn, GLYPH_COPY, 0);
    self.detail_open_btn = button(dp, L"Open location", IDC_DETAIL_OPEN_BTN, 0);
    ui_button_style(self.detail_open_btn, GLYPH_FOLDER, 0);

    // ---- Splitters ----
    self.splitter = CreateWindowExW(0, SPLITTER_CLASS, L"", WS_CHILD | WS_VISIBLE,
        0, 0, SPLITTER_WIDTH, 100, hwnd, (HMENU)(INT_PTR)IDC_SPLITTER, hInst, NULL);
    self.hsplit_activity = CreateWindowExW(0, L"SEM_HSPLITTER", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 10, 6, hwnd, (HMENU)(INT_PTR)IDC_HSPLIT_ACTIVITY, hInst, (LPVOID)&self);

    // ---- Activity log: header row ("Activity" painted by main_paint) + list ----
    self.activity_clear_btn = button(hwnd, L"Clear", IDC_ACTIVITY_CLEAR);
    ui_button_style(self.activity_clear_btn, GLYPH_CLEAR, BTNSTYLE_SUBTLE);
    self.activity_collapse_btn = button(hwnd, L"Hide", IDC_ACTIVITY_COLLAPSE);
    ui_button_style(self.activity_collapse_btn, GLYPH_DOWN, BTNSTYLE_SUBTLE);

    self.activity = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY,
        0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_ACTIVITY, NULL, NULL);

    // ---- Status bar ----
    self.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, hInst, NULL);

    // ---- Tooltips ----
    self.hwnd_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, hInst, NULL);
    if (self.hwnd_tooltip) {
        SendMessageW(self.hwnd_tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
        add_tooltip(self.hwnd_tooltip, self.toolbar_settings, L"Settings, dark mode, tray, quit");
        add_tooltip(self.hwnd_tooltip, self.toolbar_profiles, L"Active profile — click to manage profiles");
        add_tooltip(self.hwnd_tooltip, self.action_add_btn,    L"Start monitoring this item (Enter)");
        add_tooltip(self.hwnd_tooltip, self.action_browse_btn, L"Pick an executable file");
        add_tooltip(self.hwnd_tooltip, self.activity_collapse_btn, L"Show / hide the activity log (Ctrl+G)");
        add_tooltip(self.hwnd_tooltip, self.activity_clear_btn, L"Clear the activity log");
        add_tooltip(self.hwnd_tooltip, self.detail_autostop, L"Stop this item again whenever it starts (Space in the list)");
    }

    // Initial tab state
    self.active_tab = 0;
    show_only(self.center_page_svc, self.center_page_exe, true);
    ShowWindow(self.exe_search, SW_HIDE);
    ShowWindow(self.action_browse_btn, SW_HIDE);

    if (self.ui_font) apply_font_recursive(hwnd, self.ui_font);
    if (self.theme.font_title && self.detail_name) SendMessageW(self.detail_name, WM_SETFONT, (WPARAM)self.theme.font_title, FALSE);
    listbox_adjust_item_height(self.activity, self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd));

    update_detail_panel(self, ItemKind::Svc, nullptr);
}

// Status bar update (UI thread)
void update_statusbar(App& self) {
    if (!self.statusbar) return;

    size_t n[2]{}, running = 0, autos = 0, attention = 0;
    std::wstring prof;
    {
        ModelReadGuard lk(self);
        n[KIND_SVC] = self.items[KIND_SVC].v.size();
        n[KIND_EXE] = self.items[KIND_EXE].v.size();
        for (auto& up : self.items[self.active_tab == 1 ? KIND_EXE : KIND_SVC].v) {
            if (!up) continue;
            if (up->status_kind == 1) { running++; if (up->auto_stop) attention++; }
            if (up->auto_stop) autos++;
        }
        prof = active_profile_name_locked(self);
    }
    if (prof.empty()) prof = L"Default";

    // Tab labels carry counts; only touch them when the text actually changes.
    auto set_if_changed = [](HWND h, const std::wstring& text) {
        if (!h) return;
        wchar_t cur[128]{};
        GetWindowTextW(h, cur, 127);
        if (text != cur) { SetWindowTextW(h, text.c_str()); InvalidateRect(h, NULL, FALSE); }
    };
    set_if_changed(self.toolbar_tab_svc, L"Services  " + std::to_wstring(n[KIND_SVC]));
    set_if_changed(self.toolbar_tab_exe, L"Executables  " + std::to_wstring(n[KIND_EXE]));
    set_if_changed(self.toolbar_profiles, L"Profile: " + prof);

    // Summary for the active tab, painted at the right of the add row.
    wchar_t sum[160];
    const size_t total = n[self.active_tab == 1 ? KIND_EXE : KIND_SVC];
    if (attention)
        StringCchPrintfW(sum, 160, L"%zu running · %zu auto-stop · %zu to stop", running, autos, attention);
    else
        StringCchPrintfW(sum, 160, L"%zu of %zu running · %zu auto-stop", running, total, autos);
    if (self.summary_text != sum) {
        self.summary_text = sum;
        if (self.hwnd) InvalidateRect(self.hwnd, &self.layout_state.rc_summary, TRUE);
    }

    wchar_t ts[64];
    fmt_time_short(ts, 64, self.last_any_status_wall);
    size_t actions_q = action_qdepth(self);
    const bool mon = self.threads.monitor_thread.joinable();

    wchar_t text[256];
    if (actions_q)
        StringCchPrintfW(text, 256, L"%s  ·  Profile: %s  ·  %zu action%s pending  ·  Last change %s",
                         mon ? L"Monitoring" : L"Monitor stopped", prof.c_str(), actions_q, actions_q == 1 ? L"" : L"s", ts);
    else
        StringCchPrintfW(text, 256, L"%s  ·  Profile: %s  ·  Last change %s",
                         mon ? L"Monitoring" : L"Monitor stopped", prof.c_str(), ts);

    // Owner-draw: cache the text and flag part 0 so WM_DRAWITEM renders it
    // with theme-aware colors (Win32 statusbar ignores NM_CUSTOMDRAW for text).
    if (self.statusbar_text != text) {
        self.statusbar_text = text;
        SendMessageW(self.statusbar, SB_SETTEXTW, 0 | SBT_OWNERDRAW, (LPARAM)self.statusbar_text.c_str());
    }
}

void apply_filter_now(App& self, ItemKind kind) {
    const int k = ki(kind);
    // Read current search text, normalize, rebuild only the affected list.
    wchar_t buf[512]; buf[0] = 0;
    if (kind == ItemKind::Svc && self.svc_search) GetWindowTextW(self.svc_search, buf, 512);
    if (kind == ItemKind::Exe && self.exe_search) GetWindowTextW(self.exe_search, buf, 512);
    trim_ws_inplace(buf);
    self.filter[k] = to_lower_ws(buf);
    rebuild_listview_filtered(self, kind);
    update_statusbar(self);

    // Update empty-state visibility (if present)
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    HWND empty = (kind == ItemKind::Svc) ? self.svc_empty : self.exe_empty;
    if (lv && empty) {
        int count = ListView_GetItemCount(lv);
        ShowWindow(empty, (count == 0) ? SW_SHOW : SW_HIDE);
    }
}

void capture_columns_now(App& self, ItemKind kind) {
    const int k = ki(kind);
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    HWND hdr = ListView_GetHeader(lv);
    int ncol = hdr ? Header_GetItemCount(hdr) : 0;
    if (ncol <= 0 || ncol > NUM_LV_COLS) return;

    for (int i = 0; i < NUM_LV_COLS; i++) {
        self.cols_w[k][i] = (i < ncol) ? ListView_GetColumnWidth(lv, i) : 0;
    }
    int order[8]{0};
    if (ListView_GetColumnOrderArray(lv, ncol, order)) {
        for (int i = 0; i < ncol; i++) self.cols_order[k][i] = order[i];
    } else {
        for (int i = 0; i < ncol; i++) self.cols_order[k][i] = i;
    }
    for (int i = ncol; i < NUM_LV_COLS; i++) self.cols_order[k][i] = i;
    // Force the checkbox/Auto-stop column (model column 0) to remain the left-most display column.
    {
        int pos0 = -1;
        for (int i = 0; i < ncol; i++) if (self.cols_order[k][i] == 0) { pos0 = i; break; }
        if (pos0 > 0) {
            int tmp = self.cols_order[k][0];
            self.cols_order[k][0] = self.cols_order[k][pos0];
            self.cols_order[k][pos0] = tmp;
        }
    }
    self.cols_have[k] = true;

    request_save_debounced(self);
}

void apply_saved_columns(App& self, ItemKind kind) {
    const int k = ki(kind);
    if (!self.cols_have[k]) return;
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    HWND hdr = ListView_GetHeader(lv);
    int ncol = hdr ? Header_GetItemCount(hdr) : 0;
    if (ncol <= 0 || ncol > NUM_LV_COLS) return;

    // Order first, then widths.
    int order[8]{0};
    for (int i = 0; i < ncol; i++) order[i] = self.cols_order[k][i];
    // Clamp saved order values to valid range for this listview
    for (int i = 0; i < ncol; i++) {
        if (order[i] < 0 || order[i] >= ncol) order[i] = i;
    }
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
        int w = self.cols_w[k][i];
        if (w > 10) ListView_SetColumnWidth(lv, i, w);
    }
}

// Shared subclass for the header search edits (id 1) and the add-item edit (id 2).
static LRESULT CALLBACK SearchEditSubclassProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData) {
    App* self = (App*)refData;
    if (!self) return DefSubclassProc(edit, msg, wParam, lParam);

    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                SetWindowTextW(edit, L"");
                // Apply immediately; no debounce on Esc
                if (edit == self->svc_search) apply_filter_now(*self, ItemKind::Svc);
                else if (edit == self->exe_search) apply_filter_now(*self, ItemKind::Exe);
                return 0;
            }
            if (wParam == VK_RETURN && id == 2) {
                PostMessageW(self->hwnd, WM_COMMAND, MAKEWPARAM(IDC_ACTION_ADD_BTN, BN_CLICKED), (LPARAM)self->action_add_btn);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) return 0;  // no ding
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            // Repaint the frame so the focus underline follows the caret.
            const RECT& fr = (id == 2) ? self->layout_state.rc_add_frame : self->layout_state.rc_search_frame;
            InvalidateRect(self->hwnd, &fr, TRUE);
            break;
        }
    }
    return DefSubclassProc(edit, msg, wParam, lParam);
}


void layout(App& self, HWND hwnd) {
    UINT dpi = self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd);
    auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };
    auto& L = self.layout_state;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    // Old frame/label areas must be repainted after the move.
    InvalidateRect(hwnd, NULL, TRUE);

    // Status bar takes bottom space
    if (self.statusbar) {
        SendMessageW(self.statusbar, WM_SIZE, 0, 0);
        RECT sr; GetWindowRect(self.statusbar, &sr);
        int sbH = (sr.bottom - sr.top);
        SetWindowPos(self.statusbar, NULL, 0, H - sbH, W, sbH, SWP_NOZORDER);
        H -= sbH;
    }

    const int margin = S(12);          // outer gutter
    const int gap = S(8);
    const int toolbarH = S(TOOLBAR_H);
    const int actionH = S(ACTION_STRIP_H);
    const int splitterW = std::max(4, S(SPLITTER_WIDTH));
    const int splitterHH = S(HSPLITTER_HEIGHT);
    const int ctlH = S(32);            // standard control height (Fluent: 32)
    const int editH = S(20);           // edit inside its painted frame

    auto frame_edit = [&](HWND edit, RECT frame, int left_pad) {
        // Borderless edit vertically centered inside a painted rounded frame.
        int fh = frame.bottom - frame.top;
        SetWindowPos(edit, NULL, frame.left + left_pad, frame.top + (fh - editH) / 2,
                     std::max(1, (int)(frame.right - frame.left) - left_pad - S(8)), editH, SWP_NOZORDER);
    };

    // ---- Header row: tabs | search | profile chip, settings ----
    {
        int y = (toolbarH - ctlH) / 2;
        int x = margin - S(4);

        auto tab_w = [&](HWND h) {
            wchar_t t[64]{}; GetWindowTextW(h, t, 63);
            HDC hdc = GetDC(h);
            HGDIOBJ of = SelectObject(hdc, self.theme.font_bold ? self.theme.font_bold : self.ui_font);
            SIZE sz{}; GetTextExtentPoint32W(hdc, t, (int)wcslen(t), &sz);
            SelectObject(hdc, of); ReleaseDC(h, hdc);
            return (int)sz.cx + S(24);
        };
        int w1 = tab_w(self.toolbar_tab_svc), w2 = tab_w(self.toolbar_tab_exe);
        SetWindowPos(self.toolbar_tab_svc, NULL, x, y, w1, ctlH + S(4), SWP_NOZORDER);
        x += w1 + S(2);
        SetWindowPos(self.toolbar_tab_exe, NULL, x, y, w2, ctlH + S(4), SWP_NOZORDER);
        x += w2 + S(16);

        int rx = W - margin;
        // Icon-only when the icon font exists (Win10/11); text fallback otherwise.
        const int iconW = self.theme.font_icon ? ctlH : S(80);
        rx -= iconW;
        if (self.toolbar_settings) SetWindowPos(self.toolbar_settings, NULL, rx, y, iconW, ctlH, SWP_NOZORDER);
        rx -= S(4);
        int chipW = S(170);
        rx -= chipW;
        if (self.toolbar_profiles) SetWindowPos(self.toolbar_profiles, NULL, rx, y, chipW, ctlH, SWP_NOZORDER);
        rx -= S(12);

        // Search fills the middle, capped so it doesn't stretch absurdly wide.
        int searchW = std::max(0, std::min(rx - x, S(420)));
        L.rc_search_frame = RECT{ x, y, x + searchW, y + ctlH };
        const int glyphPad = self.theme.font_icon ? S(32) : S(10);
        if (self.svc_search) frame_edit(self.svc_search, L.rc_search_frame, glyphPad);
        if (self.exe_search) frame_edit(self.exe_search, L.rc_search_frame, glyphPad);
        L.header_line_y = toolbarH - 1;
    }

    // ---- Add row: [ edit ][+ Add][Browse…]          summary ----
    {
        int y = toolbarH + (actionH - ctlH) / 2;
        int x = margin;
        int editW = std::min(S(320), std::max(S(140), W / 3));
        L.rc_add_frame = RECT{ x, y, x + editW, y + ctlH };
        frame_edit(self.action_add_edit, L.rc_add_frame, S(10));
        x += editW + gap;
        const int addW = S(84);
        if (self.action_add_btn) SetWindowPos(self.action_add_btn, NULL, x, y, addW, ctlH, SWP_NOZORDER);
        x += addW + gap;
        const int browseW = S(100);
        if (self.action_browse_btn) SetWindowPos(self.action_browse_btn, NULL, x, y, browseW, ctlH, SWP_NOZORDER);
        if (self.active_tab == 1) x += browseW + gap;
        L.rc_summary = RECT{ x + S(12), y, W - margin, y + ctlH };
    }

    int contentTop = toolbarH + actionH;
    int contentH = H - contentTop;

    // ---- Activity area (bottom) ----
    int actH;
    int hdrH = S(COLLAPSE_BTN_H);
    if (self.activity_collapsed) {
        actH = hdrH + S(4);
    } else {
        actH = self.activity_panel_h;
        actH = clamp_int(actH, S(ACTIVITY_MIN_H) + hdrH, std::max(S(ACTIVITY_MIN_H) + hdrH, contentH - S(MIN_CONTENT_ABOVE_ACTIVITY)));
        self.activity_panel_h = actH;
    }

    int actY = H - actH;
    int hsplitY = actY - splitterHH;
    if (self.hsplit_activity) SetWindowPos(self.hsplit_activity, NULL, 0, hsplitY, W, splitterHH, SWP_NOZORDER);

    {
        int bw = S(84), bh = S(26);
        int by = actY + (hdrH - bh) / 2;
        if (self.activity_collapse_btn) {
            SetWindowPos(self.activity_collapse_btn, NULL, W - margin - bw, by, bw, bh, SWP_NOZORDER);
            static int last_collapsed = -1;
            if (last_collapsed != (int)self.activity_collapsed) {
                SetWindowTextW(self.activity_collapse_btn, self.activity_collapsed ? L"Show" : L"Hide");
                ui_button_style(self.activity_collapse_btn, self.activity_collapsed ? GLYPH_UP : GLYPH_DOWN, BTNSTYLE_SUBTLE);
                last_collapsed = (int)self.activity_collapsed;
            }
        }
        if (self.activity_clear_btn) {
            SetWindowPos(self.activity_clear_btn, NULL, W - margin - 2 * bw - S(4), by, bw, bh, SWP_NOZORDER);
            ShowWindow(self.activity_clear_btn, self.activity_collapsed ? SW_HIDE : SW_SHOW);
        }
        L.rc_activity_hdr = RECT{ margin, actY, W / 2, actY + hdrH };
    }

    if (self.activity_collapsed) {
        ShowWindow(self.activity, SW_HIDE);
        if (self.hsplit_activity) ShowWindow(self.hsplit_activity, SW_HIDE);
        L.rc_log_frame = RECT{};
    } else {
        ShowWindow(self.activity, SW_SHOW);
        if (self.hsplit_activity) ShowWindow(self.hsplit_activity, SW_SHOW);
        int listY = actY + hdrH;
        int listH = std::max(S(20), (H - listY) - S(8));
        SetWindowPos(self.activity, NULL, margin + 1, listY + 1, W - 2 * margin - 2, listH - 2, SWP_NOZORDER);
        L.rc_log_frame = RECT{ margin, listY, W - margin, listY + listH };
    }

    // ---- Main area (list + splitter + detail card) ----
    int mainTop = contentTop;
    int mainH = std::max(S(60), hsplitY - mainTop);

    int detailW = clamp_int(self.detail_panel_w, S(DETAIL_MIN_W), std::max(S(DETAIL_MIN_W), W - S(260) - splitterW - 2 * margin));
    self.detail_panel_w = detailW;

    int listW = std::max(S(200), W - detailW - splitterW - 2 * margin);

    // The page window hosts the listview with a 1px inset; main_paint draws
    // the hairline frame around it.
    HWND page = (self.active_tab == 0) ? self.center_page_svc : self.center_page_exe;
    HWND lv   = (self.active_tab == 0) ? self.lv_svc : self.lv_exe;
    HWND empty = (self.active_tab == 0) ? self.svc_empty : self.exe_empty;
    SetWindowPos(page, NULL, margin + 1, mainTop + 1, listW - 2, mainH - 2, SWP_NOZORDER);
    SetWindowPos(lv, NULL, 0, 0, listW - 2, mainH - 2, SWP_NOZORDER);
    if (empty) SetWindowPos(empty, NULL, S(24), std::max(S(40), mainH / 3), std::max(1, listW - S(48)), S(80), SWP_NOZORDER);
    L.rc_list_frame = RECT{ margin, mainTop, margin + listW, mainTop + mainH };

    if (self.splitter) SetWindowPos(self.splitter, NULL, margin + listW, mainTop, splitterW, mainH, SWP_NOZORDER);

    int detailX = margin + listW + splitterW;
    if (self.detail_panel) SetWindowPos(self.detail_panel, NULL, detailX, mainTop, detailW, mainH, SWP_NOZORDER);

    // Detail card internals
    if (self.detail_panel) {
        const int pad = S(16);
        int dy = pad;
        const int dw = detailW - 2 * pad;
        const int lineH = S(20);
        const int btnH = S(32);
        const int g = S(8);

        auto place = [&](HWND h, int hgt, int after) {
            if (!h) return;
            SetWindowPos(h, NULL, pad, dy, dw, hgt, SWP_NOZORDER);
            dy += hgt + after;
        };
        place(self.detail_name, S(28), S(2));
        place(self.detail_kind, lineH, S(14));
        place(self.detail_status_lbl, lineH, S(10));
        place(self.detail_autostop, S(24), S(2));
        place(self.detail_meta, lineH, S(14));

        // Buttons anchored to the bottom: 2 columns x up to 2 rows (+ open location for EXEs).
        const bool show_open = (self.active_tab == 1) && self.detail_uid;
        const int rows = 2;
        int by = mainH - pad - rows * btnH - (rows - 1) * g;
        int descH = std::max(0, by - g - dy);
        place(self.detail_desc, descH, 0);

        const int bw = (dw - g) / 2;
        SetWindowPos(self.detail_stop_btn, NULL, pad, by, bw, btnH, SWP_NOZORDER);
        SetWindowPos(self.detail_start_btn, NULL, pad, by, bw, btnH, SWP_NOZORDER);
        SetWindowPos(self.detail_remove_btn, NULL, pad + bw + g, by, dw - bw - g, btnH, SWP_NOZORDER);
        by += btnH + g;
        SetWindowPos(self.detail_copy_btn, NULL, pad, by, show_open ? bw : dw, btnH, SWP_NOZORDER);
        if (self.detail_open_btn) {
            SetWindowPos(self.detail_open_btn, NULL, pad + bw + g, by, dw - bw - g, btnH, SWP_NOZORDER);
            ShowWindow(self.detail_open_btn, show_open ? SW_SHOW : SW_HIDE);
        }
        InvalidateRect(self.detail_panel, NULL, TRUE);
    }

    lv_apply_name_fill(self, ItemKind::Svc);
    lv_apply_name_fill(self, ItemKind::Exe);
}

// Main-window painting for the bits that aren't child controls: hairlines,
// rounded input frames (+ search glyph), the add-row summary and the
// "Activity" header label.
void main_paint(App& self, HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    const auto& t = self.theme;
    const auto& L = self.layout_state;
    const UINT dpi = self.dpi ? self.dpi : 96;
    auto S = [&](int v) { return dpi_scale(dpi, v); };

    FillRect(hdc, &ps.rcPaint, t.br_bg);

    // Header hairline
    if (L.header_line_y >= 0) {
        RECT cr; GetClientRect(hwnd, &cr);
        RECT line{ 0, L.header_line_y, cr.right, L.header_line_y + 1 };
        SetDCBrushColor(hdc, t.col_border);
        FillRect(hdc, &line, (HBRUSH)GetStockObject(DC_BRUSH));
    }

    auto frame = [&](const RECT& r, HWND edit, COLORREF fill) {
        if (r.right <= r.left) return;
        const bool focused = edit && GetFocus() == edit;
        theme_fill_round(hdc, r, S(4), fill, t.col_border);
        if (focused) {
            // Fluent text box: accent underline when focused.
            RECT u{ r.left + S(1), r.bottom - S(2), r.right - S(1), r.bottom };
            SetDCBrushColor(hdc, t.col_accent);
            FillRect(hdc, &u, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    };

    HWND search = self.active_tab == 1 ? self.exe_search : self.svc_search;
    frame(L.rc_search_frame, search, t.col_edit_bg);
    if (t.font_icon && L.rc_search_frame.right > L.rc_search_frame.left) {
        RECT gr = L.rc_search_frame;
        gr.left += S(10);
        gr.right = gr.left + S(18);
        HGDIOBJ of = SelectObject(hdc, t.font_icon);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, t.col_text_dim);
        wchar_t g = GLYPH_SEARCH;
        DrawTextW(hdc, &g, 1, &gr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        SelectObject(hdc, of);
    }
    frame(L.rc_add_frame, self.action_add_edit, t.col_edit_bg);

    // List + log hairline frames
    auto hair = [&](const RECT& r, COLORREF fill) {
        if (r.right <= r.left) return;
        theme_fill_round(hdc, r, S(4), fill, t.col_border);
    };
    hair(L.rc_list_frame, t.col_edit_bg);
    hair(L.rc_log_frame, t.col_edit_bg);

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ of = SelectObject(hdc, self.ui_font);
    if (!self.summary_text.empty()) {
        SetTextColor(hdc, t.col_text_dim);
        RECT r = L.rc_summary;
        DrawTextW(hdc, self.summary_text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (L.rc_activity_hdr.right > L.rc_activity_hdr.left) {
        SelectObject(hdc, t.font_bold ? t.font_bold : self.ui_font);
        SetTextColor(hdc, t.col_text);
        RECT r = L.rc_activity_hdr;
        DrawTextW(hdc, L"Activity", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    }
    SelectObject(hdc, of);
    EndPaint(hwnd, &ps);
}
