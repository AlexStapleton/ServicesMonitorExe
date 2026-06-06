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
            FillRect(hdc, &rc, br);
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
        self.splitter_dragging = true;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(self.hwnd, &pt);

        self.splitter_drag_start_x = pt.x;
        self.splitter_drag_start_w = (self.detail_panel_w > 0) ? self.detail_panel_w : DETAIL_DEFAULT_W;
        return 0;
    }

    case WM_MOUSEMOVE:
        if (self.splitter_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(self.hwnd, &pt);

            int dx = pt.x - self.splitter_drag_start_x;
            int desired = self.splitter_drag_start_w - dx; // drag right => narrower detail panel
            self.detail_panel_w = desired;

            // Layout clamps sizes; keep it snappy during drags.
            layout(self, self.hwnd);
            return 0;
        }
        return 0;

    case WM_LBUTTONUP:
        if (self.splitter_dragging) {
            self.splitter_dragging = false;
            ReleaseCapture();
            request_save_debounced(self);
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

        FillRect(hdc, &rc, self.theme.dark_mode ? self.theme.br_panel : GetSysColorBrush(COLOR_3DFACE));

        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        int cx = w / 2;

        COLORREF shadow = self.theme.dark_mode ? RGB(18, 18, 18) : GetSysColor(COLOR_3DSHADOW);
        COLORREF hilite = self.theme.dark_mode ? DK_HIGHLIGHT : GetSysColor(COLOR_3DHILIGHT);
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
        if (s) {
            FillRect(hdc, &rc, s->theme.dark_mode ? s->theme.br_panel : GetSysColorBrush(COLOR_3DFACE));
        } else {
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_3DFACE));
        }
        // draw a subtle grip line using theme-aware colors
        int mid = (rc.top + rc.bottom) / 2;
        COLORREF grip = (s && s->theme.dark_mode) ? DK_HIGHLIGHT : GetSysColor(COLOR_3DSHADOW);
        HPEN pen = CreatePen(PS_SOLID, 1, grip);
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

    // -- Toolbar: pill-style tab buttons (BS_OWNERDRAW for theme.cpp drawing) --
    self.toolbar_tab_svc = CreateWindowExW(0, L"BUTTON", L"Services",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_TAB_SVC, hInst, NULL);
    self.toolbar_tab_exe = CreateWindowExW(0, L"BUTTON", L"EXEs",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_TAB_EXE, hInst, NULL);

    // Toolbar search edits (one per kind; show/hide based on active_tab)
    self.svc_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_SVC_SEARCH_EDIT, hInst, NULL);
    self.exe_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_EXE_SEARCH_EDIT, hInst, NULL);
    set_cue_banner(self.svc_search, L"Search services (name or status)...");
    set_cue_banner(self.exe_search, L"Search EXEs (name or status)...");
    SetWindowSubclass(self.svc_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);
    SetWindowSubclass(self.exe_search, SearchEditSubclassProc, 1, (DWORD_PTR)&self);

    // Toolbar icon buttons (BS_OWNERDRAW for dark mode theming)
    self.toolbar_settings = CreateWindowExW(0, L"BUTTON", L"\u2699",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_SETTINGS, hInst, NULL);
    self.toolbar_profiles = CreateWindowExW(0, L"BUTTON", L"\xD83D\xDCCB",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_PROFILES, hInst, NULL);
    self.toolbar_tray = CreateWindowExW(0, L"BUTTON", L"\u23EC",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_TRAY, hInst, NULL);
    self.toolbar_quit = CreateWindowExW(0, L"BUTTON", L"\u2715",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR_QUIT, hInst, NULL);

    // -- Action strip --
    self.action_add_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 100, 24, hwnd, (HMENU)(INT_PTR)IDC_ACTION_ADD_EDIT, hInst, NULL);
    set_cue_banner(self.action_add_edit, L"Service name or display name\u2026");

    self.action_add_btn = CreateWindowExW(0, L"BUTTON", L"+ Add",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTION_ADD_BTN, hInst, NULL);
    self.action_browse_btn = CreateWindowExW(0, L"BUTTON", L"Browse",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTION_BROWSE_BTN, hInst, NULL);
    self.action_stop_btn = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTION_STOP_BTN, hInst, NULL);
    self.action_start_btn = CreateWindowExW(0, L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTION_START_BTN, hInst, NULL);
    self.action_remove_btn = CreateWindowExW(0, L"BUTTON", L"Remove",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTION_REMOVE_BTN, hInst, NULL);

    // -- Center pages (listviews) --
    self.center_page_svc = make_page(hwnd);
    self.center_page_exe = make_page(hwnd);

    self.lv_svc = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_svc, (HMENU)(INT_PTR)IDC_LV_SVC, hInst, NULL);

    self.lv_exe = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 100, 100, self.center_page_exe, (HMENU)(INT_PTR)IDC_LV_EXE, hInst, NULL);

    lv_set_columns(self.lv_svc, L"Auto Stop", L"Service Name", L"Status", L"Last Update");
    lv_set_columns(self.lv_exe, L"Auto Stop", L"Executable", L"Status", L"Path", L"Last Update");

    // Owner-data listviews: request state image callbacks so the checkbox (Auto Stop) renders.
    ListView_SetCallbackMask(self.lv_svc, LVIS_STATEIMAGEMASK);
    ListView_SetCallbackMask(self.lv_exe, LVIS_STATEIMAGEMASK);

    // Empty-state labels (shown when list has no rows)
    self.svc_empty = CreateWindowW(L"STATIC",
        L"No services are being monitored yet.\r\n\r\nUse the toolbar to add a service name.",
        WS_CHILD | SS_CENTER,
        0, 0, 10, 10, self.center_page_svc, NULL, NULL, NULL);
    self.exe_empty = CreateWindowW(L"STATIC",
        L"No EXEs are being monitored yet.\r\n\r\nUse the toolbar to add an EXE name or browse for one.",
        WS_CHILD | SS_CENTER,
        0, 0, 10, 10, self.center_page_exe, NULL, NULL, NULL);
    ShowWindow(self.svc_empty, SW_HIDE);
    ShowWindow(self.exe_empty, SW_HIDE);

    // Apply saved column order/widths (if present in config)
    apply_saved_columns(self, ItemKind::Svc);
    apply_saved_columns(self, ItemKind::Exe);

    // Explorer theming for modern look (safe if unavailable)
    try_set_explorer_theme(self.lv_svc);
    try_set_explorer_theme(self.lv_exe);

    // -- Detail panel (right side) --
    self.detail_panel = make_page(hwnd);

    self.detail_name = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 10, 10, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_NAME, hInst, NULL);
    // Themed separator line (SS_OWNERDRAW so we can paint with theme colors)
    self.detail_sep = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        0, 0, 10, 2, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_SEP, hInst, NULL);
    self.detail_status_lbl = CreateWindowW(L"STATIC", L"Status:",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_STATUS_LBL, hInst, NULL);
    self.detail_autostop = CreateWindowW(L"STATIC", L"Auto-stop:",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_AUTOSTOP, hInst, NULL);
    self.detail_desc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 10, 10, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_DESC, hInst, NULL);
    self.detail_stop_btn = CreateWindowW(L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_STOP_BTN, hInst, NULL);
    self.detail_start_btn = CreateWindowW(L"BUTTON", L"Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_START_BTN, hInst, NULL);
    self.detail_remove_btn = CreateWindowW(L"BUTTON", L"Remove",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_REMOVE_BTN, hInst, NULL);
    self.detail_copy_btn = CreateWindowW(L"BUTTON", L"Copy",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, self.detail_panel, (HMENU)(INT_PTR)IDC_DETAIL_COPY_BTN, hInst, NULL);

    // -- Vertical splitter between list and detail panel --
    self.splitter = CreateWindowExW(0, SPLITTER_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, SPLITTER_WIDTH, 100, hwnd, (HMENU)(INT_PTR)IDC_SPLITTER, hInst, NULL);

    // -- Horizontal splitter above activity --
    self.hsplit_activity = CreateWindowExW(0, L"SEM_HSPLITTER", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 6, hwnd, (HMENU)(INT_PTR)IDC_HSPLIT_ACTIVITY, hInst, (LPVOID)&self);

    // -- Activity log --
    self.activity_collapse_btn = CreateWindowExW(0, L"BUTTON", L"\u25BC Collapse",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_ACTIVITY_COLLAPSE, hInst, NULL);

    self.activity = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
        0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_ACTIVITY, NULL, NULL);

    // -- Status bar --
    self.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, hInst, NULL);

    // -- Tooltips --
    self.hwnd_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, hInst, NULL);
    if (self.hwnd_tooltip) {
        SendMessageW(self.hwnd_tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
        add_tooltip(self.hwnd_tooltip, self.toolbar_settings, L"Settings menu");
        add_tooltip(self.hwnd_tooltip, self.toolbar_profiles, L"Profiles");
        add_tooltip(self.hwnd_tooltip, self.toolbar_tray,     L"Minimize to tray");
        add_tooltip(self.hwnd_tooltip, self.toolbar_quit,     L"Quit application");
        add_tooltip(self.hwnd_tooltip, self.action_add_btn,    L"Add item to monitor list");
        add_tooltip(self.hwnd_tooltip, self.action_stop_btn,   L"Stop selected item");
        add_tooltip(self.hwnd_tooltip, self.action_start_btn,  L"Start selected item");
        add_tooltip(self.hwnd_tooltip, self.action_remove_btn, L"Remove selected from list");
        add_tooltip(self.hwnd_tooltip, self.action_browse_btn, L"Browse for executable file");
        add_tooltip(self.hwnd_tooltip, self.activity_collapse_btn, L"Collapse / expand activity log (Ctrl+G)");
    }

    // Initial tab state
    self.active_tab = 0;
    show_only(self.center_page_svc, self.center_page_exe, true);
    ShowWindow(self.exe_search, SW_HIDE);  // hide EXE search initially
    ShowWindow(self.action_browse_btn, SW_HIDE);  // hide Browse initially

    // Apply modern font once everything exists
    if (self.ui_font) apply_font_recursive(hwnd, self.ui_font);
    // ListBox item height needs explicit help under per-monitor DPI.
    listbox_adjust_item_height(self.activity, self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd));
}

// Status bar update (UI thread)
void update_statusbar(App& self) {
    if (!self.statusbar) return;

    size_t ns = 0, ne = 0;
    {
        ModelReadGuard lk(self);
        ns = self.items[KIND_SVC].v.size();
        ne = self.items[KIND_EXE].v.size();
    }
    std::wstring prof;
    {
        ModelReadGuard lk(self);
        prof = active_profile_name_locked(self);
    }
    if (prof.empty()) prof = L"Default";

    wchar_t ts[64];
    fmt_time_local(ts, 64, self.last_any_status_wall);

    wchar_t text[256];
    size_t actions_q = action_qdepth(self);
    const wchar_t* mon = self.threads.monitor_thread.joinable() ? L"ON" : L"OFF";
    StringCchPrintfW(text, 256, L"\u25CF Monitoring: %s | Profile: %s | %zu items | Queue: %zu", mon, prof.c_str(), (ns + ne), actions_q);

    // Owner-draw: cache the text and flag part 0 so WM_DRAWITEM renders it
    // with theme-aware colors (Win32 statusbar ignores NM_CUSTOMDRAW for text).
    self.statusbar_text = text;
    SendMessageW(self.statusbar, SB_SETTEXTW, 0 | SBT_OWNERDRAW, (LPARAM)self.statusbar_text.c_str());
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

static LRESULT CALLBACK SearchEditSubclassProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
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
            break;
    }
    return DefSubclassProc(edit, msg, wParam, lParam);
}


void layout(App& self, HWND hwnd) {
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

    int pad = S(LAYOUT_PAD);
    int toolbarH = S(TOOLBAR_H);
    int actionH = S(ACTION_STRIP_H);
    int splitterW = std::max(4, S(SPLITTER_WIDTH));
    int splitterHH = S(HSPLITTER_HEIGHT);

    // ---- Toolbar row ----
    {
        int btnW_tab = S(80);
        int btnH = toolbarH - S(LAYOUT_PAD);
        int gap = S(LAYOUT_PAD);
        int x = pad;

        SetWindowPos(self.toolbar_tab_svc, NULL, x, 2, btnW_tab, btnH, SWP_NOZORDER);
        x += btnW_tab + gap;
        SetWindowPos(self.toolbar_tab_exe, NULL, x, 2, btnW_tab, btnH, SWP_NOZORDER);
        x += btnW_tab + gap + S(8);

        // Icon buttons on the right — use cached HWNDs
        int iconW = S(34);
        int rx = W - pad;

        rx -= iconW;
        if (self.toolbar_quit) SetWindowPos(self.toolbar_quit, NULL, rx, 2, iconW, btnH, SWP_NOZORDER);
        rx -= iconW + gap;
        if (self.toolbar_tray) SetWindowPos(self.toolbar_tray, NULL, rx, 2, iconW, btnH, SWP_NOZORDER);
        rx -= iconW + gap;
        if (self.toolbar_profiles) SetWindowPos(self.toolbar_profiles, NULL, rx, 2, iconW, btnH, SWP_NOZORDER);
        rx -= iconW + gap;
        if (self.toolbar_settings) SetWindowPos(self.toolbar_settings, NULL, rx, 2, iconW, btnH, SWP_NOZORDER);

        // Search edits fill space between tabs and icon buttons.
        // Allow shrinking to 0 when the window is narrow so icon buttons aren't pushed off-screen.
        int searchX = x;
        int searchW = std::max(0, rx - gap - searchX);
        int searchH = S(24);
        int searchY = (toolbarH - searchH) / 2;

        if (self.svc_search) SetWindowPos(self.svc_search, NULL, searchX, searchY, searchW, searchH, SWP_NOZORDER);
        if (self.exe_search) SetWindowPos(self.exe_search, NULL, searchX, searchY, searchW, searchH, SWP_NOZORDER);
    }

    // ---- Action strip row ----
    {
        int y = toolbarH;
        int editH = S(24);
        int editY = y + (actionH - editH) / 2;
        int btnH = S(26);
        int btnY = y + (actionH - btnH) / 2;
        int gap = S(LAYOUT_PAD);

        // Right-side action buttons — position from right edge first so we know
        // how much space the left group (edit + Add + Browse) can use.
        int abW = S(78);
        int rx = W - pad;
        rx -= abW;
        if (self.action_remove_btn) SetWindowPos(self.action_remove_btn, NULL, rx, btnY, abW, btnH, SWP_NOZORDER);
        rx -= abW + gap;
        if (self.action_start_btn) SetWindowPos(self.action_start_btn, NULL, rx, btnY, abW, btnH, SWP_NOZORDER);
        rx -= abW + gap;
        if (self.action_stop_btn) SetWindowPos(self.action_stop_btn, NULL, rx, btnY, abW, btnH, SWP_NOZORDER);
        int rightGroupLeft = rx;  // left edge of Stop button

        // Left-side controls — edit box is flexible, buttons are fixed
        int x = pad;
        int addW = S(56);
        int browseW = S(64);

        // Space available for the edit box: total left area minus fixed buttons and gaps
        int fixedLeft = addW + gap;
        if (self.active_tab == 1) fixedLeft += browseW + gap;
        int separatorGap = S(16);
        int editW = rightGroupLeft - separatorGap - fixedLeft - x - gap;
        if (editW < S(60)) editW = S(60);
        if (editW > S(240)) editW = S(240);

        if (self.action_add_edit) SetWindowPos(self.action_add_edit, NULL, x, editY, editW, editH, SWP_NOZORDER);
        x += editW + gap;

        if (self.action_add_btn) SetWindowPos(self.action_add_btn, NULL, x, btnY, addW, btnH, SWP_NOZORDER);
        x += addW + gap;

        // Browse button (EXE tab only)
        if (self.action_browse_btn) SetWindowPos(self.action_browse_btn, NULL, x, btnY, browseW, btnH, SWP_NOZORDER);
    }

    int contentTop = toolbarH + actionH;
    int contentH = H - contentTop;  // remaining below toolbar+action, above statusbar

    // ---- Activity area (bottom) ----
    int actH;
    int collapseH = S(COLLAPSE_BTN_H);
    if (self.activity_collapsed) {
        actH = collapseH;
    } else {
        actH = self.activity_panel_h;
        actH = clamp_int(actH, S(ACTIVITY_MIN_H), contentH - S(MIN_CONTENT_ABOVE_ACTIVITY));
        self.activity_panel_h = actH;
    }

    // Position collapse button and activity
    int actY = H - actH;
    int hsplitY = actY - splitterHH;

    if (self.hsplit_activity) SetWindowPos(self.hsplit_activity, NULL, 0, hsplitY, W, splitterHH, SWP_NOZORDER);

    if (self.activity_collapse_btn) {
        int cbtnW = S(90);
        SetWindowPos(self.activity_collapse_btn, NULL, W - cbtnW - pad, actY, cbtnW, collapseH, SWP_NOZORDER);
        // Only update text when state actually changes (avoids redundant redraws)
        static bool last_collapsed = !self.activity_collapsed; // force first update
        if (last_collapsed != self.activity_collapsed) {
            SetWindowTextW(self.activity_collapse_btn, self.activity_collapsed ? L"\u25B2 Expand" : L"\u25BC Collapse");
            last_collapsed = self.activity_collapsed;
        }
    }

    if (self.activity_collapsed) {
        ShowWindow(self.activity, SW_HIDE);
        if (self.hsplit_activity) ShowWindow(self.hsplit_activity, SW_HIDE);
    } else {
        ShowWindow(self.activity, SW_SHOW);
        if (self.hsplit_activity) ShowWindow(self.hsplit_activity, SW_SHOW);
        // Activity label area + listbox
        int actListY = actY + collapseH;
        int actListH = std::max(S(20), (H - actListY));
        SetWindowPos(self.activity, NULL, pad, actListY, W - 2*pad, actListH, SWP_NOZORDER);
    }

    // ---- Main area (center list + splitter + detail panel) ----
    int mainTop = contentTop;
    int mainH = hsplitY - mainTop;
    if (mainH < S(60)) mainH = S(60);

    // Detail panel width
    int detailW = self.detail_panel_w;
    int minDetailW = S(DETAIL_MIN_W);
    int maxDetailW = W - S(200) - splitterW;
    detailW = clamp_int(detailW, minDetailW, maxDetailW);
    self.detail_panel_w = detailW;

    // List area width
    int listW = W - detailW - splitterW;
    if (listW < S(200)) listW = S(200);

    // Position list pages — only reposition the active tab's controls
    int cPad = S(4);
    int lvW = std::max(S(80), listW - 2*cPad);
    int lvH = std::max(S(60), mainH - 2*cPad);

    if (self.active_tab == 0) {
        SetWindowPos(self.center_page_svc, NULL, 0, mainTop, listW, mainH, SWP_NOZORDER);
        SetWindowPos(self.lv_svc, NULL, cPad, cPad, lvW, lvH, SWP_NOZORDER);
        if (self.svc_empty) SetWindowPos(self.svc_empty, NULL, cPad, cPad, lvW, lvH, SWP_NOZORDER);
    } else {
        SetWindowPos(self.center_page_exe, NULL, 0, mainTop, listW, mainH, SWP_NOZORDER);
        SetWindowPos(self.lv_exe, NULL, cPad, cPad, lvW, lvH, SWP_NOZORDER);
        if (self.exe_empty) SetWindowPos(self.exe_empty, NULL, cPad, cPad, lvW, lvH, SWP_NOZORDER);
    }

    // Splitter
    if (self.splitter) {
        int sx = listW;
        SetWindowPos(self.splitter, NULL, sx, mainTop, splitterW, mainH, SWP_NOZORDER);
    }

    // Detail panel
    int detailX = listW + splitterW;
    if (self.detail_panel) SetWindowPos(self.detail_panel, NULL, detailX, mainTop, detailW, mainH, SWP_NOZORDER);

    // Detail panel internal layout
    if (self.detail_panel) {
        int dp = S(8);
        int dy = dp;
        int dw = detailW - 2*dp;
        int labelH = S(18);
        int btnH = S(26);
        int gap = S(DETAIL_BTN_GAP);

        // Name (bold header)
        if (self.detail_name) {
            SetWindowPos(self.detail_name, NULL, dp, dy, dw, S(20), SWP_NOZORDER);
            dy += S(20) + gap;
        }

        // Separator (etched horz static)
        HWND sep = FindWindowExW(self.detail_panel, NULL, L"Static", L"");
        if (sep && sep != self.detail_name && sep != self.detail_status_lbl && sep != self.detail_autostop) {
            SetWindowPos(sep, NULL, dp, dy, dw, 2, SWP_NOZORDER);
            dy += 2 + gap;
        }

        // Status label
        if (self.detail_status_lbl) {
            SetWindowPos(self.detail_status_lbl, NULL, dp, dy, dw, labelH, SWP_NOZORDER);
            dy += labelH + gap;
        }

        // Auto-stop label
        if (self.detail_autostop) {
            SetWindowPos(self.detail_autostop, NULL, dp, dy, dw, labelH, SWP_NOZORDER);
            dy += labelH + gap;
        }

        // Description edit (fills remaining space above buttons)
        int btnsH = 2*btnH + 2*gap;  // two rows of buttons
        int descH = std::max(S(40), mainH - dy - btnsH - dp - gap);
        if (self.detail_desc) {
            SetWindowPos(self.detail_desc, NULL, dp, dy, dw, descH, SWP_NOZORDER);
            dy += descH + gap;
        }

        // Action buttons at bottom — 2x2 grid so text always fits
        int bbW = (dw - gap) / 2;
        if (bbW < S(50)) bbW = S(50);
        int bx = dp;
        if (self.detail_stop_btn) { SetWindowPos(self.detail_stop_btn, NULL, bx, dy, bbW, btnH, SWP_NOZORDER); }
        if (self.detail_start_btn) { SetWindowPos(self.detail_start_btn, NULL, bx + bbW + gap, dy, bbW, btnH, SWP_NOZORDER); }
        dy += btnH + gap;
        if (self.detail_remove_btn) { SetWindowPos(self.detail_remove_btn, NULL, bx, dy, bbW, btnH, SWP_NOZORDER); }
        if (self.detail_copy_btn) { SetWindowPos(self.detail_copy_btn, NULL, bx + bbW + gap, dy, bbW, btnH, SWP_NOZORDER); }
    }

    // On resize we only need to redistribute the Name column to fill available
    // space — the other column widths should not be overridden by AUTOSIZE since
    // they either come from saved user prefs or have stable default widths.
    // Full autosize (lv_needs_layout) is only set by item-count changes, profile
    // switches, and DPI changes where column widths genuinely need recalculating.
    lv_apply_name_fill(self, ItemKind::Svc);
    lv_apply_name_fill(self, ItemKind::Exe);
}
