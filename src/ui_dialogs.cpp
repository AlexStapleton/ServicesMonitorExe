#include "app.h"

// ============================================================
// ui_dialogs.cpp  --  All dialog window procedures and openers
// ============================================================

// --------------- Preferences dialog ---------------

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

    if (ui < UI_REFRESH_MS_MIN || ui > UI_REFRESH_MS_MAX) {
        wchar_t errmsg[128];
        StringCchPrintfW(errmsg, 128, L"UI Refresh must be between %d and %d ms.", UI_REFRESH_MS_MIN, UI_REFRESH_MS_MAX);
        msgbox_err(dlg, L"Preferences", errmsg);
        return false;
    }
    if (mon < 200) { msgbox_err(dlg, L"Preferences", L"Monitor Interval must be at least 200 ms."); return false; }
    if (cd < 0) { msgbox_err(dlg, L"Preferences", L"Autostop Cooldown (ms) cannot be negative."); return false; }
    if (sw < 0) { msgbox_err(dlg, L"Preferences", L"Stop Wait (ms) cannot be negative."); return false; }

    bool dark_changed = false;
    {
        ModelLockGuard lk(self);

        self.cfg.ui_refresh_ms = ui;
        self.cfg.monitor_interval_ms = mon;
        self.cfg.autostop_cooldown_ms = cd;
        self.cfg.stop_wait_ms = sw;

        // Tray settings
        BOOL tray_checked = (SendMessageW(c_tray, BM_GETCHECK, 0, 0) == BST_CHECKED);
        BOOL close_checked = (SendMessageW(c_close, BM_GETCHECK, 0, 0) == BST_CHECKED);

        bool tray_en = tray_checked ? true : false;
        bool close_en = close_checked ? true : false;

        if (close_en) tray_en = true; // close-to-tray requires tray icon

        self.tray.enabled = tray_en;
        self.tray.close_to_tray = tray_en && close_en;

        if (!self.tray.enabled) self.tray.close_to_tray = false;

        // Theme
        BOOL dark_checked = (SendMessageW(c_dark, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool new_dark = dark_checked ? true : false;
        if (self.theme.dark_mode != new_dark) {
            self.theme.dark_mode = new_dark;
            dark_changed = true;
        }
    }

// Apply tray icon add/remove immediately
tray_sync(self, GetModuleHandleW(NULL));

    if (dark_changed) {
        darkmode_set_app(self.theme.dark_mode);

        // Recompute theme resources once, then apply everywhere (idempotent).
        theme_compute(self);
        theme_apply_all_controls(self);
        self.ui_seen_theme_gen = self.theme.gen.load(std::memory_order_relaxed);

        // Populate listviews from the loaded config (decoupled from status updates).
        rebuild_listview_filtered(self, ItemKind::Svc);
        rebuild_listview_filtered(self, ItemKind::Exe);

        HMENU mb = GetMenu(self.hwnd);
        if (mb) {
            CheckMenuItem(mb, IDM_DARKMODE, MF_BYCOMMAND | (self.theme.dark_mode ? MF_CHECKED : MF_UNCHECKED));
            DrawMenuBar(self.hwnd);
        }
    }

    log_linef(self, L"Settings updated: UI %dms, Monitor %dms, Cooldown %dms, StopWait %dms, Tray %d, CloseToTray %d, Dark %d",
              self.cfg.ui_refresh_ms, self.cfg.monitor_interval_ms, self.cfg.autostop_cooldown_ms, self.cfg.stop_wait_ms,
              self.tray.enabled ? 1 : 0, self.tray.close_to_tray ? 1 : 0, self.theme.dark_mode ? 1 : 0);

    request_save_debounced(self);
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
    return true;
}

// NOTE: theme_handle_customdraw lives in theme.cpp

struct PrefsState {
    App* app = nullptr;
    HWND e_ui = NULL, e_mon = NULL, e_cd = NULL, e_sw = NULL;
    HWND c_tray = NULL, c_close = NULL, c_dark = NULL;
    HWND st_cfg = NULL;
    UINT dpi = 96;
};

static LRESULT CALLBACK PrefsWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (app_ptr) {
            auto* ps = new (std::nothrow) PrefsState();
            if (ps) { ps->app = app_ptr; ps->dpi = get_dpi_for_hwnd(dlg); }
            SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ps));
        }
        return TRUE;
    }
    auto* ps = reinterpret_cast<PrefsState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!ps || !ps->app) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *ps->app;
    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        UINT dpi = get_dpi_for_hwnd(dlg);
        ps->dpi = dpi;
        auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

        wchar_t ui_label[64];
        StringCchPrintfW(ui_label, 64, L"UI Refresh (ms, %d\u2013%d):", UI_REFRESH_MS_MIN, UI_REFRESH_MS_MAX);
        CreateWindowW(L"STATIC", ui_label, WS_CHILD | WS_VISIBLE,
            S(12), S(14), S(160), S(20), dlg, NULL, NULL, NULL);
        ps->e_ui = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(12), S(140), S(24), dlg, (HMENU)IDC_PREF_UI_MS, NULL, NULL);

        CreateWindowW(L"STATIC", L"Monitor Interval (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(48), S(160), S(20), dlg, NULL, NULL, NULL);
        ps->e_mon = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(46), S(140), S(24), dlg, (HMENU)IDC_PREF_MON_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Autostop Cooldown (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(82), S(160), S(20), dlg, NULL, NULL, NULL);
        ps->e_cd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(80), S(140), S(24), dlg, (HMENU)IDC_PREF_CD_S, NULL, NULL);

        CreateWindowW(L"STATIC", L"Stop Wait (ms):", WS_CHILD | WS_VISIBLE,
            S(12), S(116), S(160), S(20), dlg, NULL, NULL, NULL);
        ps->e_sw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            S(180), S(114), S(140), S(24), dlg, (HMENU)IDC_PREF_SW_S, NULL, NULL);


        // Tray options
        ps->c_tray = CreateWindowW(L"BUTTON", L"Enable tray icon", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            S(12), S(152), S(200), S(22), dlg, (HMENU)IDC_PREF_TRAY_ENABLE, NULL, NULL);

        ps->c_close = CreateWindowW(L"BUTTON", L"Close to tray (hide on X)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            S(12), S(176), S(240), S(22), dlg, (HMENU)IDC_PREF_CLOSE_TO_TRAY, NULL, NULL);

        ps->c_dark = CreateWindowW(L"BUTTON", L"Dark mode", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            S(12), S(200), S(200), S(22), dlg, (HMENU)IDC_PREF_DARKMODE, NULL, NULL);
        wchar_t buf[64];
        StringCchPrintfW(buf, 64, L"%d", self.cfg.ui_refresh_ms);
        SetWindowTextW(ps->e_ui, buf);
        StringCchPrintfW(buf, 64, L"%d", self.cfg.monitor_interval_ms);
        SetWindowTextW(ps->e_mon, buf);
        StringCchPrintfW(buf, 64, L"%d", self.cfg.autostop_cooldown_ms);
        SetWindowTextW(ps->e_cd, buf);
        StringCchPrintfW(buf, 64, L"%d", self.cfg.stop_wait_ms);
        SetWindowTextW(ps->e_sw, buf);

        SendMessageW(ps->c_tray, BM_SETCHECK, self.tray.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(ps->c_close, BM_SETCHECK, self.tray.close_to_tray ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(ps->c_dark, BM_SETCHECK, self.theme.dark_mode ? BST_CHECKED : BST_UNCHECKED, 0);


        wchar_t cfgline[MAX_PATH + 64];
        StringCchPrintfW(cfgline, MAX_PATH + 64, L"Config file: %s", self.cfg.cfg_path);

        // Path can be long; show it with path ellipsis and size it to the dialog width.
        RECT rcw{}; GetClientRect(dlg, &rcw);
        int cw = rcw.right - rcw.left;
        int cfgX = S(12), cfgY = S(228), cfgH = S(20);
        int cfgW = std::max(S(100), cw - S(24));
        ps->st_cfg = CreateWindowW(L"STATIC", cfgline,
            WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX,
            cfgX, cfgY, cfgW, cfgH, dlg, NULL, NULL, NULL);

CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            S(230), S(262), S(80), S(28), dlg, (HMENU)IDCANCEL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE,
            S(318), S(262), S(80), S(28), dlg, (HMENU)IDC_PREF_APPLY, NULL, NULL);
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE,
            S(406), S(262), S(80), S(28), dlg, (HMENU)IDOK, NULL, NULL);

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
        FillRect(hdc, &rc, self.theme.br_bg);
        return 1;
    }
case WM_GETMINMAXINFO: {
        // Prevent the Preferences window from being resized so small that the bottom buttons clip.
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = ps->dpi ? ps->dpi : get_dpi_for_hwnd(dlg);
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
        HWND edit = (self.active_tab == 0) ? self.svc_search : self.exe_search;
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
if (id == IDC_TOOLBAR_SETTINGS && code == BN_CLICKED) {
    if (self.menu_settings) {
        // Keep checkmark in sync
        UINT f = self.theme.dark_mode ? MF_CHECKED : MF_UNCHECKED;
        CheckMenuItem(self.menu_settings, IDM_DARKMODE, MF_BYCOMMAND | f);

        HWND tbSet = GetDlgItem(self.hwnd, IDC_TOOLBAR_SETTINGS);
        RECT r{};
        if (tbSet) GetWindowRect(tbSet, &r);
        else { GetCursorPos((LPPOINT)&r); r.bottom = r.top; }
        int cmd = TrackPopupMenuEx(self.menu_settings,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            r.left, r.bottom, dlg, NULL);
        if (cmd) PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
    return 0;
}
if (id == IDC_TOOLBAR_QUIT && code == BN_CLICKED) {
    PostMessageW(dlg, WM_CLOSE, 0, 0);
    return 0;
}
        // (Search boxes are on the main window, not Preferences)

        if (id == IDCANCEL) { DestroyWindow(dlg); return 0; }
        if (id == IDC_PREF_APPLY) { prefs_apply(self, dlg, ps->e_ui, ps->e_mon, ps->e_cd, ps->e_sw, ps->c_tray, ps->c_close, ps->c_dark); return 0; }
        if (id == IDOK) {
            if (prefs_apply(self, dlg, ps->e_ui, ps->e_mon, ps->e_cd, ps->e_sw, ps->c_tray, ps->c_close, ps->c_dark)) DestroyWindow(dlg);
            return 0;
        }
        return 0;
    }

    case WM_SIZE: {
        // Keep the long config path and bottom buttons sized/anchored to the window width.
        UINT dpi = ps->dpi ? ps->dpi : get_dpi_for_hwnd(dlg);
        auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };
        RECT rcw{}; GetClientRect(dlg, &rcw);
        int cw = rcw.right - rcw.left;
        int m = S(12);
        if (ps->st_cfg) {
            int y = S(228);
            int h = S(20);
            int w = std::max(S(100), cw - (m * 2));
            SetWindowPos(ps->st_cfg, NULL, m, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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
        if (new_dpi && new_dpi != ps->dpi) {
            scale_children(dlg, ps->dpi, new_dpi);
            ps->dpi = new_dpi;

        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, ps->dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);
        }
        return 0;
    }    case WM_NCDESTROY: {
        delete ps;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

void open_prefs_dialog(App& self, HWND parent) {
    WNDCLASSW wc{};
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

// --------------- Hotkeys dialog ---------------

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

        // apply themed fonts + dark mode
        UINT dpi = get_dpi_for_hwnd(dlg);
        HFONT f = theme_ensure_font_for_dpi(self, dpi);
        if (f) apply_font_recursive(dlg, f);
        theme_apply_to_window(self, dlg);

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
    case WM_NOTIFY: {
        auto* nh = reinterpret_cast<NMHDR*>(lParam);
        if (nh && nh->code == NM_CUSTOMDRAW) {
            bool handled = false;
            LRESULT lr = theme_handle_customdraw(*pself, nh, lParam, handled);
            if (handled) return lr;
        }
        break;
    }
    case WM_ERASEBKGND: {
        if (!pself) break;
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, pself->theme.br_bg);
        return 1;
    }
    case WM_DRAWITEM: {
        if (!pself) break;
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (theme_draw_owner_button(*pself, dis)) return TRUE;
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        if (pself) return (LRESULT)theme_handle_ctlcolor(*pself, msg, wParam, lParam);
        break;
    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}



void open_hotkeys_dialog(App& self, HWND parent) {
    WNDCLASSW wc{};
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

// --------------- Help dialog ---------------

// Help menu currently shows the same content as Hotkeys.
void open_help_dialog(App& self, HWND parent) {
    open_hotkeys_dialog(self, parent);
}



// --------------- Diagnostics dialog ---------------

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
    appendf(L"UI refresh: %d ms\r\n", self.cfg.ui_refresh_ms);
    appendf(L"Monitor tick: %d ms\r\n", self.cfg.monitor_interval_ms);
    appendf(L"Autostop cooldown: %d ms\r\n", self.cfg.autostop_cooldown_ms);
    appendf(L"Stop wait: %d ms\r\n", self.cfg.stop_wait_ms);
    appendf(L"Tray enabled: %d\r\n", self.tray.enabled ? 1 : 0);
    appendf(L"Close-to-tray: %d\r\n", self.tray.close_to_tray ? 1 : 0);
    appendf(L"Dark mode: %d\r\n", self.theme.dark_mode ? 1 : 0);
    appendf(L"\r\n");

    // Queue depth
    size_t qdepth = 0;
    { qdepth = action_qdepth(self); }
    appendf(L"Actions queued: %zu\r\n", qdepth);

    // Model stats + per-item last errors
    size_t svc_n = 0, exe_n = 0;
    {
        ModelReadGuard lk(self);
        svc_n = self.items[KIND_SVC].v.size();
        exe_n = self.items[KIND_EXE].v.size();
    }
    appendf(L"Items: services=%zu, exes=%zu\r\n", svc_n, exe_n);
    appendf(L"\r\n");

    appendf(L"Last service stop errors (non-zero):\r\n");
    int printed = 0;
    {
        ModelReadGuard lk(self);
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
        FillRect(hdc, &rc, self.theme.br_bg);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)theme_handle_ctlcolor(self, msg, wParam, lParam);

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (theme_draw_owner_button(self, dis)) return TRUE;
        break;
    }

    case WM_NOTIFY: {
        auto* nh = reinterpret_cast<NMHDR*>(lParam);
        if (nh && nh->code == NM_CUSTOMDRAW) {
            bool handled = false;
            LRESULT lr = theme_handle_customdraw(self, nh, lParam, handled);
            if (handled) return lr;
        }
        break;
    }

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

void open_diagnostics_dialog(App& self, HWND parent) {
    WNDCLASSW wc{};
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

// --------------- Profiles dialog ---------------

static std::wstring make_unique_profile_name_locked(const App& self, const wchar_t* base) {
    std::wstring b = base && base[0] ? base : L"Profile";
    auto exists_ci = [&](const std::wstring& name) -> bool {
        for (const auto& p : self.prof.profiles) {
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

    SendMessageW(lb_profiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb_profiles, LB_RESETCONTENT, 0, 0);

    std::vector<std::wstring> names;
    int active = -1;
    {
        ModelReadGuard lk(self);
        active = self.prof.active;
        names.reserve(self.prof.profiles.size());
        for (const auto& p : self.prof.profiles) names.push_back(p.name.empty() ? L"(unnamed)" : p.name);
    }

    for (size_t i = 0; i < names.size(); i++) {
        SendMessageW(lb_profiles, LB_ADDSTRING, 0, (LPARAM)names[i].c_str());
    }
    SendMessageW(lb_profiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb_profiles, NULL, FALSE);

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
    SendMessageW(lb_watch, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb_watch, LB_RESETCONTENT, 0, 0);

    std::vector<std::wstring> ws;
    {
        ModelReadGuard lk(self);
        if (prof_idx >= 0 && prof_idx < (int)self.prof.profiles.size()) {
            ws = self.prof.profiles[(size_t)prof_idx].watch_exes;
        }
    }
    for (const auto& w : ws) {
        if (w.empty()) continue;
        SendMessageW(lb_watch, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
    SendMessageW(lb_watch, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb_watch, NULL, FALSE);
    if (!ws.empty()) SendMessageW(lb_watch, LB_SETCURSEL, 0, 0);
}

static void prof_fill_content_list(App& self, HWND lb, int prof_idx, ItemKind kind) {
    if (!lb) return;
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    // Merge: profile items + main list items (deduped, profile auto-stop wins)
    std::vector<ItemRow> merged;
    {
        ModelReadGuard lk(self);

        // Start with profile items (these have authoritative auto_stop state)
        if (prof_idx >= 0 && prof_idx < (int)self.prof.profiles.size()) {
            merged = self.prof.profiles[(size_t)prof_idx].cfg.items[ki(kind)];
        }

        // Build set of names already in profile (case-insensitive)
        std::unordered_set<std::wstring> seen;
        for (const auto& r : merged) {
            std::wstring low = r.name;
            for (auto& c : low) c = towlower(c);
            seen.insert(std::move(low));
        }

        // Add main list items not already in profile (auto_stop = false)
        const int k = ki(kind);
        for (const auto& up : self.items[k].v) {
            if (!up || up->name.empty()) continue;
            std::wstring low = up->name;
            for (auto& c : low) c = towlower(c);
            if (seen.count(low)) continue;
            ItemRow r;
            r.name = up->name;
            r.auto_stop = false;
            if (kind == ItemKind::Exe && !up->exe_path.empty()) r.exe_path = up->exe_path;
            merged.push_back(std::move(r));
        }
    }

    // Sort merged list alphabetically
    std::sort(merged.begin(), merged.end(), [](const ItemRow& a, const ItemRow& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    for (const auto& r : merged) {
        if (r.name.empty()) continue;
        int idx = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)r.name.c_str());
        if (idx >= 0) SendMessageW(lb, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)(r.auto_stop ? 1 : 0));
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, FALSE);
    if (!merged.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
}
static void prof_fill_start_exe_list(App& self, HWND lb, int prof_idx) {
    if (!lb) return;
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);

    std::vector<StartItem> rows;
    {
        ModelReadGuard lk(self);
        if (prof_idx >= 0 && prof_idx < (int)self.prof.profiles.size()) {
            rows = self.prof.profiles[(size_t)prof_idx].start_items;
        }
    }

    for (const auto& r : rows) {
        if (r.target.empty()) continue;
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)r.target.c_str());
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, FALSE);
    if (!rows.empty()) SendMessageW(lb, LB_SETCURSEL, 0, 0);
}


static std::wstring prof_get_lb_text(HWND lb, int idx) {
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
        ModelReadGuard lk(self);
        idx = self.prof.active;
        const ConfigSnapshot* cfg = &self.prof.default_cfg;
        if (idx >= 0 && idx < (int)self.prof.profiles.size()) cfg = &self.prof.profiles[(size_t)idx].cfg;

        self.cfg.ui_refresh_ms = cfg->ui_refresh_ms;
        self.cfg.monitor_interval_ms = cfg->monitor_interval_ms;
        self.cfg.autostop_cooldown_ms = cfg->autostop_cooldown_ms;
        self.cfg.stop_wait_ms = cfg->stop_wait_ms;
        self.tray.enabled = cfg->tray_enabled;
        self.tray.close_to_tray = cfg->tray_enabled && cfg->close_to_tray;

        rebuild_runtime_items_locked(self, *cfg);

        svc_ptrs.reserve(self.items[KIND_SVC].v.size());
        for (auto& up : self.items[KIND_SVC].v) if (up) svc_ptrs.push_back(up.get());

        exe_ptrs.reserve(self.items[KIND_EXE].v.size());
        for (auto& up : self.items[KIND_EXE].v) if (up) exe_ptrs.push_back(up.get());
    }

    tray_sync(self, GetModuleHandleW(NULL));

    rebuild_listview_filtered(self, ItemKind::Svc);
    rebuild_listview_filtered(self, ItemKind::Exe);

    self.lv_needs_layout[KIND_SVC] = true;
    self.lv_needs_layout[KIND_EXE] = true;
    update_statusbar(self);
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}


static void prof_update_active_label(App& self, HWND st_active) {
    if (!st_active) return;
    std::wstring name;
    {
        ModelReadGuard lk(self);
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

struct ProfilesState {
    App* app = nullptr;
    HWND lb_profiles = NULL, e_name = NULL, b_add = NULL, b_rename = NULL, b_del = NULL, b_up = NULL, b_down = NULL, st_active = NULL;
    HWND lb_watch = NULL, e_watch = NULL, b_watch_add = NULL, b_watch_del = NULL;
    HWND lb_svc = NULL, e_svc = NULL, chk_svc = NULL, b_svc_add = NULL, b_svc_del = NULL, b_svc_toggle = NULL;
    HWND lb_exe = NULL, e_exe = NULL, chk_exe = NULL, b_exe_add = NULL, b_exe_del = NULL, b_exe_toggle = NULL;
    HWND lb_start_exe = NULL, e_start_exe = NULL, b_start_exe_add = NULL, b_start_exe_del = NULL;
    HWND b_close = NULL;
    HWND st_profiles_hdr = NULL, st_name_hdr = NULL;
    HWND st_watch_hdr = NULL, st_watch_add_hdr = NULL;
    HWND st_start_hdr = NULL, st_start_add_hdr = NULL;
    HWND st_svc_hdr = NULL, st_exe_hdr = NULL;
    HWND st_note = NULL;
    // Pill tab buttons (replaces WC_TABCONTROL for consistency with main window)
    HWND pill_activation = NULL, pill_start = NULL, pill_svc = NULL, pill_exe = NULL;
    int  prof_tab = 0;  // 0=Activation, 1=Start Items, 2=Services, 3=EXEs
    UINT dpi = 96;
};

static LRESULT CALLBACK ProfilesWndProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) {
            auto* pst = new ProfilesState();
            pst->app = self_ptr;
            pst->dpi = get_dpi_for_hwnd(dlg);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pst));
        }
        return TRUE;
    }
    auto* pst = reinterpret_cast<ProfilesState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (!pst || !pst->app) return DefWindowProcW(dlg, msg, wParam, lParam);
    App& self = *pst->app;
    auto do_layout = [&]() {
    RECT rc{}; GetClientRect(dlg, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    const int m  = dpi_scale(pst->dpi, 12);
    const int g  = dpi_scale(pst->dpi, 10);
    const int g2 = dpi_scale(pst->dpi, 6);

    const int headerH = dpi_scale(pst->dpi, 22);
    const int bigHdrH = headerH * 2;
    const int labelH  = dpi_scale(pst->dpi, 18);
    const int editH   = dpi_scale(pst->dpi, 24);
    const int btnH    = dpi_scale(pst->dpi, 28);

    const int btnW_sm = dpi_scale(pst->dpi, 34);

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

    // Right-side pill tab selection
    const int tab = pst->prof_tab;
    const bool show_activation = (tab == 0);
    const bool show_start      = (tab == 1);
    const bool show_services   = (tab == 2);
    const bool show_exes       = (tab == 3);

    // Visibility (right pane)
    VIS(pst->st_watch_hdr, show_activation);
    VIS(pst->lb_watch, show_activation);
    VIS(pst->st_watch_add_hdr, show_activation);
    VIS(pst->e_watch, show_activation);
    VIS(pst->b_watch_add, show_activation);
    VIS(pst->b_watch_del, show_activation);

    VIS(pst->st_start_hdr, show_start);
    VIS(pst->lb_start_exe, show_start);
    VIS(pst->st_start_add_hdr, show_start);
    VIS(pst->e_start_exe, show_start);
    VIS(pst->b_start_exe_add, show_start);
    VIS(pst->b_start_exe_del, show_start);

    // Services tab (profile contents: services)
    VIS(pst->st_note, show_services || show_exes);

    VIS(pst->st_svc_hdr, show_services);
    VIS(pst->lb_svc, show_services);
    VIS(pst->e_svc, show_services);
    VIS(pst->chk_svc, show_services);
    VIS(pst->b_svc_add, show_services);
    VIS(pst->b_svc_toggle, show_services);
    VIS(pst->b_svc_del, show_services);

    // EXEs tab (profile contents: exes)
    VIS(pst->st_exe_hdr, show_exes);
    VIS(pst->lb_exe, show_exes);
    VIS(pst->e_exe, show_exes);
    VIS(pst->chk_exe, show_exes);
    VIS(pst->b_exe_add, show_exes);
    VIS(pst->b_exe_toggle, show_exes);
    VIS(pst->b_exe_del, show_exes);


    // Left + right columns (stable, no vertical splitting)
    const int minLeftW   = dpi_scale(pst->dpi, 330);
    const int preferLeft = dpi_scale(pst->dpi, 360);
    const int minRightW  = dpi_scale(pst->dpi, 420);

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
        const int reserve_min_main = dpi_scale(pst->dpi, 260);
        int reserve = (w >= (reserve_min_main + btnW_sm + g2)) ? (btnW_sm + g2) : 0;
        int mainW = std::max(1, w - reserve);
        int upX = x + mainW + (reserve ? g2 : 0);

        DWS(pst->st_profiles_hdr, x, yy, mainW, headerH); yy += headerH + g2;

        // Profiles list (never collapses)
        const int lineH = std::max(16, dpi_scale(pst->dpi, 16));
        const int minList = std::max(dpi_scale(pst->dpi, 170), lineH * 8);
        const int worstButtonsRows = 4; // conservative
        const int needBelow = headerH + editH + (btnH * worstButtonsRows) + (g2 * 10) + (labelH * 2);
        int listH = std::max(minList, (yBot - yy) / 2);
        listH = std::min(listH, std::max(minList, yBot - yy - needBelow));
        listH = std::max(minList, listH);

        DW(pst->lb_profiles, x, yy, mainW, listH); yy += listH + g;

        DWS(pst->st_name_hdr, x, yy, mainW, headerH); yy += headerH + g2;
        DW(pst->e_name, x, yy, mainW, editH); yy += editH + g2;

        // Action buttons (adaptive; never overlap each other or Up/Down)
        const int wAdd    = dpi_scale(pst->dpi, 72);
        const int wRename = dpi_scale(pst->dpi, 86);
        const int wDel    = dpi_scale(pst->dpi, 78);
        const int padBtns = dpi_scale(pst->dpi, 6);

        const int needOneRow = wAdd + wRename + wDel + padBtns * 2;

        if (mainW >= needOneRow) {
            int bx = x;
            DW(pst->b_add, bx, yy, wAdd, btnH); bx += wAdd + padBtns;
            DW(pst->b_rename, bx, yy, wRename, btnH); bx += wRename + padBtns;
            DW(pst->b_del, bx, yy, wDel, btnH);

            if (reserve) DW(pst->b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;
            if (reserve) {
                DW(pst->b_down, upX, yy, btnW_sm, btnH);
                yy += btnH + g2;
            }
        } else if (mainW >= (wAdd + wRename + padBtns)) {
            int w2 = (mainW - padBtns) / 2;
            DW(pst->b_add, x, yy, w2, btnH);
            DW(pst->b_rename, x + w2 + padBtns, yy, mainW - w2 - padBtns, btnH);
            if (reserve) DW(pst->b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(pst->b_del, x, yy, mainW, btnH);
            if (reserve) DW(pst->b_down, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;
        } else {
            DW(pst->b_add, x, yy, mainW, btnH);
            if (reserve) DW(pst->b_up, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(pst->b_rename, x, yy, mainW, btnH);
            if (reserve) DW(pst->b_down, upX, yy, btnW_sm, btnH);
            yy += btnH + g2;

            DW(pst->b_del, x, yy, mainW, btnH);
            yy += btnH + g2;
        }

        if (!reserve) {
            int half = (mainW - padBtns) / 2;
            half = std::max(dpi_scale(pst->dpi, 60), half);
            DW(pst->b_up, x, yy, half, btnH);
            DW(pst->b_down, x + half + padBtns, yy, mainW - half - padBtns, btnH);
            yy += btnH + g2;
        }

        // Active note/status (2 lines)
        DWS(pst->st_active, x, yy, mainW, labelH * 2);
        yy += (labelH * 2) + g2;

        return yy;
    };

    layout_left(xL, leftW, yTop, yBot);

    // Bottom close button (anchored)
    if (pst->b_close) {
        const int closeW = dpi_scale(pst->dpi, 96);
        DW(pst->b_close, cw - m - closeW, ch - m - btnH, closeW, btnH);
    }

    // RIGHT PANE: pill tabs + active page layout
    const int pillH = dpi_scale(pst->dpi, 28);
    const int pillGap = dpi_scale(pst->dpi, 6);
    const int pillW_act = dpi_scale(pst->dpi, 90);
    const int pillW_start = dpi_scale(pst->dpi, 90);
    const int pillW_svc = dpi_scale(pst->dpi, 80);
    const int pillW_exe = dpi_scale(pst->dpi, 60);

    // Position pill buttons in a row
    int px_pill = xR;
    DW(pst->pill_activation, px_pill, yTop, pillW_act, pillH); px_pill += pillW_act + pillGap;
    DW(pst->pill_start, px_pill, yTop, pillW_start, pillH); px_pill += pillW_start + pillGap;
    DW(pst->pill_svc, px_pill, yTop, pillW_svc, pillH); px_pill += pillW_svc + pillGap;
    DW(pst->pill_exe, px_pill, yTop, pillW_exe, pillH);

    // Content area below pills
    int px = xR;
    int py = yTop + pillH + g2;
    int pw = std::max(1, rightW);
    int ph = std::max(1, yBot - py);

    auto layout_add_row = [&](HWND st_hdr, HWND edit, HWND b1, HWND b2, int x, int &yy, int w) {
        DWS(st_hdr, x, yy, w, headerH);
        yy += headerH + g2;

        const int btn1W = dpi_scale(pst->dpi, 118);
        const int btn2W = dpi_scale(pst->dpi, 132);
        const int editMin = dpi_scale(pst->dpi, 220);
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
            half = std::max(dpi_scale(pst->dpi, 90), half);
            DW(b1, x, yy, half, btnH);
            DW(b2, x + half + g2, yy, w - half - g2, btnH);
            yy += btnH + g2;
        }
    };

    if (show_activation) {
        int yy = py;
        DWS(pst->st_watch_hdr, px, yy, pw, bigHdrH); yy += bigHdrH + g2;

        const int addNeed = headerH + g2 + std::max(editH, btnH) + g2;
        int avail = ph - (yy - py) - addNeed - g2;
        int listH = std::max(1, avail);
        DW(pst->lb_watch, px, yy, pw, listH); yy += listH + g2;

        layout_add_row(pst->st_watch_add_hdr, pst->e_watch, pst->b_watch_add, pst->b_watch_del, px, yy, pw);
    }
    else if (show_start) {
        int yy = py;
        DWS(pst->st_start_hdr, px, yy, pw, bigHdrH); yy += bigHdrH + g2;

        const int addNeed = headerH + g2 + std::max(editH, btnH) + g2;
        int avail = ph - (yy - py) - addNeed - g2;
        int listH = std::max(1, avail);
        DW(pst->lb_start_exe, px, yy, pw, listH); yy += listH + g2;

        layout_add_row(pst->st_start_add_hdr, pst->e_start_exe, pst->b_start_exe_add, pst->b_start_exe_del, px, yy, pw);
    }
    else if (show_services || show_exes) {
        int yy = py;

        // Note / tips area (show on both Services + EXEs tabs)
        DW(pst->st_note, px, yy, pw, labelH * 3);
        yy += (labelH * 3) + g;

        // Remaining area for list + controls
        int remainingH = std::max(1, ph - (yy - py));

        const int chkH = dpi_scale(pst->dpi, 22);
        // Keep bottom controls fully visible; let the list shrink if space is tight.
        const int ctrlTail = editH + g2 + chkH + g2 + btnH + g2 + btnH + g2 + btnH;
        int listH = remainingH - (headerH + g2) - g2 - ctrlTail;
        listH = std::max(1, listH);

        if (show_services) {
            // --- Services content ---
            DWS(pst->st_svc_hdr, px, yy, pw, headerH);
            int yList = yy + headerH + g2;

            DW(pst->lb_svc, px, yList, pw, listH);

            int yCtrl = yList + listH + g2;
            DW(pst->e_svc, px, yCtrl, pw, editH); yCtrl += editH + g2;
            DW(pst->chk_svc, px, yCtrl, pw, chkH); yCtrl += chkH + g2;

            // Buttons stacked (never overlap)
            DW(pst->b_svc_add,    px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(pst->b_svc_toggle, px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(pst->b_svc_del,    px, yCtrl, pw, btnH);
        } else {
            // --- EXEs content ---
            DWS(pst->st_exe_hdr, px, yy, pw, headerH);
            int yList = yy + headerH + g2;

            DW(pst->lb_exe, px, yList, pw, listH);

            int yCtrl = yList + listH + g2;
            DW(pst->e_exe, px, yCtrl, pw, editH); yCtrl += editH + g2;
            DW(pst->chk_exe, px, yCtrl, pw, chkH); yCtrl += chkH + g2;

            DW(pst->b_exe_add,    px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(pst->b_exe_toggle, px, yCtrl, pw, btnH); yCtrl += btnH + g2;
            DW(pst->b_exe_del,    px, yCtrl, pw, btnH);
        }
    }

    // Finish batched layout and repaint once.
    if (hdwp) EndDeferWindowPos(hdwp);
    SendMessageW(dlg, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    };

    auto refresh_for_selection = [&]() {
        int sel = prof_get_sel(pst->lb_profiles);
        // Button enable/disable so stored HWNDs are actually used
        EnableWindow(pst->b_add, TRUE);
        EnableWindow(pst->b_rename, sel >= 0);
        EnableWindow(pst->b_del, sel >= 0);
        EnableWindow(pst->b_up, sel > 0);
        int profN = (int)SendMessageW(pst->lb_profiles, LB_GETCOUNT, 0, 0);
        EnableWindow(pst->b_down, sel >= 0 && sel < profN - 1);
        EnableWindow(pst->b_watch_add, sel >= 0);
        EnableWindow(pst->b_watch_del, sel >= 0);
        std::wstring pname;
        {
            ModelReadGuard lk(self);
            if (sel >= 0 && sel < (int)self.prof.profiles.size()) pname = self.prof.profiles[(size_t)sel].name;
        }
        if (pname.empty()) pname = L"(unnamed)";
        prof_set_edit_text(pst->e_name, pname);
        prof_fill_watch_list(self, pst->lb_watch, sel);
        prof_fill_content_list(self, pst->lb_svc, sel, ItemKind::Svc);
        prof_fill_content_list(self, pst->lb_exe, sel, ItemKind::Exe);
        prof_fill_start_exe_list(self, pst->lb_start_exe, sel);

        int svcsel = prof_get_sel(pst->lb_svc);
        int exesel = prof_get_sel(pst->lb_exe);
        EnableWindow(pst->b_svc_add, sel >= 0);
        EnableWindow(pst->b_svc_del, sel >= 0 && svcsel >= 0);
        EnableWindow(pst->b_svc_toggle, sel >= 0 && svcsel >= 0);
        EnableWindow(pst->b_exe_add, sel >= 0);
        EnableWindow(pst->b_exe_del, sel >= 0 && exesel >= 0);
        EnableWindow(pst->b_exe_toggle, sel >= 0 && exesel >= 0);
        int starts = prof_get_sel(pst->lb_start_exe);
        EnableWindow(pst->b_start_exe_add, sel >= 0);
        EnableWindow(pst->b_start_exe_del, sel >= 0 && starts >= 0);

        prof_update_active_label(self, pst->st_active);
    };

    switch (msg) {
    case WM_CREATE: {
        (void)lParam;

        pst->st_profiles_hdr = CreateWindowW(L"STATIC", L"Profiles", WS_CHILD | WS_VISIBLE,
            12, 10, 240, 20, dlg, NULL, NULL, NULL);

        pst->lb_profiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            12, 34, 250, 270, dlg, (HMENU)IDC_PROF_LIST, NULL, NULL);

        pst->st_name_hdr = CreateWindowW(L"STATIC", L"Profile name:", WS_CHILD | WS_VISIBLE,
            12, 310, 100, 18, dlg, NULL, NULL, NULL);

        pst->e_name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            12, 330, 250, 24, dlg, (HMENU)IDC_PROF_NAME_EDIT, NULL, NULL);

        pst->b_add = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
            12, 362, 60, 26, dlg, (HMENU)IDC_PROF_ADD, NULL, NULL);
        pst->b_rename = CreateWindowW(L"BUTTON", L"Rename", WS_CHILD | WS_VISIBLE,
            78, 362, 70, 26, dlg, (HMENU)IDC_PROF_RENAME, NULL, NULL);
        pst->b_del = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE,
            154, 362, 70, 26, dlg, (HMENU)IDC_PROF_DELETE, NULL, NULL);

        pst->b_up = CreateWindowW(L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE,
            230, 362, 32, 26, dlg, (HMENU)IDC_PROF_MOVE_UP, NULL, NULL);
        pst->b_down = CreateWindowW(L"BUTTON", L"Down", WS_CHILD | WS_VISIBLE,
            230, 392, 32, 26, dlg, (HMENU)IDC_PROF_MOVE_DOWN, NULL, NULL);


        // Pill-style tab buttons (consistent with main window)
        pst->pill_activation = CreateWindowExW(0, L"BUTTON", L"Activation",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, dlg, (HMENU)(INT_PTR)IDC_PROF_TAB_ACTIVATION, NULL, NULL);
        pst->pill_start = CreateWindowExW(0, L"BUTTON", L"Start Items",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, dlg, (HMENU)(INT_PTR)IDC_PROF_TAB_START, NULL, NULL);
        pst->pill_svc = CreateWindowExW(0, L"BUTTON", L"Services",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, dlg, (HMENU)(INT_PTR)IDC_PROF_TAB_SVC, NULL, NULL);
        pst->pill_exe = CreateWindowExW(0, L"BUTTON", L"EXEs",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, dlg, (HMENU)(INT_PTR)IDC_PROF_TAB_EXE, NULL, NULL);
        pst->prof_tab = 0;

        pst->st_watch_hdr = CreateWindowW(L"STATIC", L"Activation (WATCH EXEs)\r\nProfile activates if ANY are running", WS_CHILD | WS_VISIBLE,
            280, 10, 460, 20, dlg, NULL, NULL, NULL);

        pst->lb_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 34, 460, 230, dlg, (HMENU)IDC_WATCH_LIST, NULL, NULL);

        pst->st_watch_add_hdr = CreateWindowW(L"STATIC", L"Add WATCH exe (name or full path):", WS_CHILD | WS_VISIBLE,
            280, 270, 300, 18, dlg, NULL, NULL, NULL);

        pst->e_watch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 290, 340, 24, dlg, (HMENU)IDC_WATCH_EDIT, NULL, NULL);

        pst->b_watch_add = CreateWindowW(L"BUTTON", L"Add WATCH", WS_CHILD | WS_VISIBLE,
            628, 290, 112, 24, dlg, (HMENU)IDC_WATCH_ADD, NULL, NULL);

        pst->b_watch_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            628, 320, 112, 24, dlg, (HMENU)IDC_WATCH_REMOVE, NULL, NULL);

        // ---- Profile contents editor (services / exes) ----

        // Services
        pst->st_svc_hdr = CreateWindowW(L"STATIC", L"Services in this profile:", WS_CHILD | WS_VISIBLE,
            280, 374, 220, 18, dlg, NULL, NULL, NULL);

        pst->lb_svc = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
            | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            280, 396, 220, 190, dlg, (HMENU)IDC_PCONT_SVC_LIST, NULL, NULL);

        pst->e_svc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 592, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_EDIT, NULL, NULL);

        pst->chk_svc = CreateWindowW(L"BUTTON", L"Auto-stop", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            280, 620, 220, 22, dlg, (HMENU)IDC_PCONT_SVC_AUTOSTOP, NULL, NULL);

        pst->b_svc_add = CreateWindowW(L"BUTTON", L"Add service", WS_CHILD | WS_VISIBLE,
            280, 646, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_ADD, NULL, NULL);

        pst->b_svc_toggle = CreateWindowW(L"BUTTON", L"Toggle auto-stop", WS_CHILD | WS_VISIBLE,
            280, 674, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_TOGGLE, NULL, NULL);

        pst->b_svc_del = CreateWindowW(L"BUTTON", L"Remove selected service", WS_CHILD | WS_VISIBLE,
            280, 702, 220, 24, dlg, (HMENU)IDC_PCONT_SVC_REMOVE, NULL, NULL);

        // EXEs
        pst->st_exe_hdr = CreateWindowW(L"STATIC", L"EXEs in this profile:", WS_CHILD | WS_VISIBLE,
            520, 374, 220, 18, dlg, NULL, NULL, NULL);

        pst->lb_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
            | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            520, 396, 220, 190, dlg, (HMENU)IDC_PCONT_EXE_LIST, NULL, NULL);

        pst->e_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            520, 592, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_EDIT, NULL, NULL);

        pst->chk_exe = CreateWindowW(L"BUTTON", L"Auto-stop", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            520, 620, 220, 22, dlg, (HMENU)IDC_PCONT_EXE_AUTOSTOP, NULL, NULL);

        pst->b_exe_add = CreateWindowW(L"BUTTON", L"Add exe", WS_CHILD | WS_VISIBLE,
            520, 646, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_ADD, NULL, NULL);

        pst->b_exe_toggle = CreateWindowW(L"BUTTON", L"Toggle auto-stop", WS_CHILD | WS_VISIBLE,
            520, 674, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_TOGGLE, NULL, NULL);

        pst->b_exe_del = CreateWindowW(L"BUTTON", L"Remove selected exe", WS_CHILD | WS_VISIBLE,
            520, 702, 220, 24, dlg, (HMENU)IDC_PCONT_EXE_REMOVE, NULL, NULL);

        pst->st_active = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            280, 734, 460, 18, dlg, (HMENU)IDC_PROF_ACTIVE_LABEL, NULL, NULL);

        pst->st_note = CreateWindowW(L"STATIC",
            L"Note: If multiple profiles match, the first one in this list wins.\r\n"
            L"Profiles deactivate automatically when none of their WATCH EXEs are running.",
            WS_CHILD | WS_VISIBLE,
            280, 756, 460, 40, dlg, NULL, NULL, NULL);


        // Start Items on activation (separate from Auto-stop lists)

        pst->st_start_hdr = CreateWindowW(L"STATIC",
            L"Start items when this profile activates\r\n(best-effort closed on deactivation):",
            WS_CHILD | WS_VISIBLE,
            280, 804, 460, 18, dlg, NULL, NULL, NULL);

        pst->lb_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            280, 826, 460, 110, dlg, (HMENU)IDC_PSTART_EXE_LIST, NULL, NULL);

        pst->st_start_add_hdr = CreateWindowW(L"STATIC", L"Add start item (file/path/URL):", WS_CHILD | WS_VISIBLE,
            280, 944, 300, 18, dlg, NULL, NULL, NULL);

        pst->e_start_exe = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 962, 340, 24, dlg, (HMENU)IDC_PSTART_EXE_EDIT, NULL, NULL);

        pst->b_start_exe_add = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE,
            630, 960, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_ADD, NULL, NULL);

        pst->b_start_exe_del = CreateWindowW(L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE,
            630, 988, 110, 26, dlg, (HMENU)IDC_PSTART_EXE_REMOVE, NULL, NULL);
        pst->b_close = CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
     660, 1040, 80, 28, dlg, (HMENU)IDCANCEL, NULL, NULL);
        // DPI-aware: scale child controls + apply themed font for this window's DPI.
        pst->dpi = get_dpi_for_hwnd(dlg);
        if (pst->dpi != 96) {
            scale_children(dlg, 96, pst->dpi);
        }
        theme_compute_for_window(self, dlg);
        HFONT f = theme_ensure_font_for_dpi(self, pst->dpi);
        if (f) {
            apply_font_recursive(dlg, f);
            // Pill buttons get font via apply_font_recursive
        }

        prof_fill_profile_list(self, pst->lb_profiles, 0);
        refresh_for_selection();

        theme_apply_to_window(self, dlg);
        do_layout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* p = reinterpret_cast<MINMAXINFO*>(lParam);
        if (p) {
            const int minW = dpi_scale(pst->dpi, 900);
            const int minH = dpi_scale(pst->dpi, 620);
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
        if (nh && nh->code == NM_CUSTOMDRAW) {
            bool handled = false;
            LRESULT lr = theme_handle_customdraw(self, nh, lParam, handled);
            if (handled) return lr;
        }
        break;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, self.theme.br_bg);
        return 1;
    }
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis && mis->CtlType == ODT_LISTBOX &&
            (mis->CtlID == IDC_PCONT_SVC_LIST || mis->CtlID == IDC_PCONT_EXE_LIST)) {
            mis->itemHeight = dpi_scale(pst->dpi, 20);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        // Owner-draw checklist boxes (Services / EXEs)
        if (dis && dis->CtlType == ODT_LISTBOX &&
            (dis->CtlID == IDC_PCONT_SVC_LIST || dis->CtlID == IDC_PCONT_EXE_LIST)) {
            if (dis->itemID == (UINT)-1) break;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool selected = (dis->itemState & ODS_SELECTED) != 0;
            bool checked  = (SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0) == 1);

            // Background
            COLORREF bgCol = selected
                ? (self.theme.dark_mode ? DK_BORDER : GetSysColor(COLOR_HIGHLIGHT))
                : (self.theme.dark_mode ? self.theme.col_edit_bg : GetSysColor(COLOR_WINDOW));
            HBRUSH bgBr = CreateSolidBrush(bgCol);
            FillRect(hdc, &rc, bgBr);
            DeleteObject(bgBr);

            // Checkbox
            int chkSize = dpi_scale(pst->dpi, 14);
            int chkX = rc.left + dpi_scale(pst->dpi, 4);
            int chkY = rc.top + (rc.bottom - rc.top - chkSize) / 2;
            RECT chkRc = { chkX, chkY, chkX + chkSize, chkY + chkSize };

            // Checkbox border
            COLORREF borderCol = self.theme.dark_mode ? DK_BORDER : RGB(160, 160, 160);
            HPEN chkPen = CreatePen(PS_SOLID, 1, borderCol);
            HGDIOBJ oldPen = SelectObject(hdc, chkPen);
            HGDIOBJ oldBr2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            if (checked) {
                // Filled checkbox with accent color
                COLORREF accentCol = self.theme.dark_mode ? DK_ACCENT : LT_ACCENT;
                HBRUSH accentBr = CreateSolidBrush(accentCol);
                SelectObject(hdc, accentBr);
                int r2 = dpi_scale(pst->dpi, 3);
                RoundRect(hdc, chkRc.left, chkRc.top, chkRc.right, chkRc.bottom, r2, r2);
                SelectObject(hdc, oldBr2);
                DeleteObject(accentBr);

                // Checkmark
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"\u2713", 1, &chkRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                // Empty checkbox
                int r2 = dpi_scale(pst->dpi, 3);
                RoundRect(hdc, chkRc.left, chkRc.top, chkRc.right, chkRc.bottom, r2, r2);
                SelectObject(hdc, oldBr2);
            }
            SelectObject(hdc, oldPen);
            DeleteObject(chkPen);

            // Text
            wchar_t txt[256]{};
            SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)txt);

            RECT textRc = rc;
            textRc.left = chkRc.right + dpi_scale(pst->dpi, 6);
            SetBkMode(hdc, TRANSPARENT);
            COLORREF txtCol = selected
                ? (self.theme.dark_mode ? self.theme.col_text : GetSysColor(COLOR_HIGHLIGHTTEXT))
                : (self.theme.dark_mode ? self.theme.col_text : GetSysColor(COLOR_WINDOWTEXT));
            SetTextColor(hdc, txtCol);

            HFONT hfont = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
            HGDIOBJ oldf = NULL;
            if (hfont) oldf = SelectObject(hdc, hfont);
            DrawTextW(hdc, txt, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (oldf) SelectObject(hdc, oldf);

            // Focus rect
            if (dis->itemState & ODS_FOCUS) {
                RECT fr = rc;
                InflateRect(&fr, -1, -1);
                DrawFocusRect(hdc, &fr);
            }
            return TRUE;
        }
        // Pill tab buttons — draw with selection state
        if (dis && dis->CtlType == ODT_BUTTON) {
            int pid = (int)dis->CtlID;
            if (pid >= IDC_PROF_TAB_ACTIVATION && pid <= IDC_PROF_TAB_EXE) {
                bool is_sel = (pid - IDC_PROF_TAB_ACTIVATION) == pst->prof_tab;
                if (theme_draw_pill(self, dis, is_sel)) return TRUE;
            }
        }
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

        // Pill tab button clicks
        if (id >= IDC_PROF_TAB_ACTIVATION && id <= IDC_PROF_TAB_EXE && code == BN_CLICKED) {
            pst->prof_tab = id - IDC_PROF_TAB_ACTIVATION;
            // Repaint all pills so selection state updates
            InvalidateRect(pst->pill_activation, NULL, FALSE);
            InvalidateRect(pst->pill_start, NULL, FALSE);
            InvalidateRect(pst->pill_svc, NULL, FALSE);
            InvalidateRect(pst->pill_exe, NULL, FALSE);
            do_layout();
            return 0;
        }

        if (id == IDC_PROF_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PROF_ADD) {
            int new_sel = -1;
            {
                ModelLockGuard lk(self);

                // Ensure default snapshot exists
                if (!self.prof.have_default_cfg) {
                    snapshot_from_runtime_locked(self, self.prof.default_cfg);
                    self.prof.have_default_cfg = true;
                }

                ProfileSnapshot p;
                p.name = make_unique_profile_name_locked(self, L"Profile");
                p.cfg = self.prof.default_cfg; // start from default
                self.prof.profiles.push_back(std::move(p));
                rebuild_profile_watch_keys_locked(self);
                new_sel = (int)self.prof.profiles.size() - 1;
            }
            request_save_debounced(self);
            prof_fill_profile_list(self, pst->lb_profiles, new_sel);
            refresh_for_selection();
            return 0;
        }

        if (id == IDC_PROF_RENAME) {
            int sel = prof_get_sel(pst->lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring name = prof_get_edit_text(pst->e_name);
            if (name.empty()) { msgbox_warn(dlg, L"Profiles", L"Profile name cannot be empty."); return 0; }
            if (name.size() > 64) {
                msgbox_warn(dlg, L"Profiles", L"Profile name is too long (max 64 characters).");
                return 0;
            }
            // Reject reserved filesystem characters and path separators so the
            // name is safe if it is ever concatenated into a path or log line.
            for (wchar_t ch : name) {
                if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) {
                    msgbox_warn(dlg, L"Profiles",
                        L"Profile name contains an invalid character.\n\n"
                        L"Avoid: < > : \" / \\ | ? * and control characters.");
                    return 0;
                }
            }

            bool ok = false;
            {
                ModelLockGuard lk(self);

                // Uniqueness check
                for (size_t i = 0; i < self.prof.profiles.size(); i++) {
                    if ((int)i == sel) continue;
                    if (_wcsicmp(self.prof.profiles[i].name.c_str(), name.c_str()) == 0) {
                        msgbox_warn(dlg, L"Profiles", L"A profile with that name already exists.");
                        return 0;
                    }
                }

                if (sel >= 0 && sel < (int)self.prof.profiles.size()) {
                    self.prof.profiles[(size_t)sel].name = name;
                    ok = true;
                }
            }
            if (ok) {
                request_save_debounced(self);
                prof_fill_profile_list(self, pst->lb_profiles, sel);
                refresh_for_selection();
                update_statusbar(self);
            }
            return 0;
        }

        if (id == IDC_PROF_DELETE) {
            int sel = prof_get_sel(pst->lb_profiles);
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

                if (sel >= 0 && sel < (int)self.prof.profiles.size()) {
                    // Fix active_profile index
                    if (self.prof.active == sel) {
                        need_switch_default = true;
                    } else if (self.prof.active > sel) {
                        self.prof.active--;
                    }

                    self.prof.profiles.erase(self.prof.profiles.begin() + sel);

                    rebuild_profile_watch_keys_locked(self);

                    int count = (int)self.prof.profiles.size();
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
            prof_fill_profile_list(self, pst->lb_profiles, new_sel);
            refresh_for_selection();
            update_statusbar(self);
            return 0;
        }

        if (id == IDC_PROF_MOVE_UP || id == IDC_PROF_MOVE_DOWN) {
            int sel = prof_get_sel(pst->lb_profiles);
            if (sel < 0) return 0;

            int dir = (id == IDC_PROF_MOVE_UP) ? -1 : +1;
            int other = sel + dir;

            bool changed = false;
            int new_sel = sel;

            {
                ModelLockGuard lk(self);

                if (other >= 0 && other < (int)self.prof.profiles.size()) {
                    std::swap(self.prof.profiles[(size_t)sel], self.prof.profiles[(size_t)other]);

                    rebuild_profile_watch_keys_locked(self);

                    // keep active_profile tracking correct
                    if (self.prof.active == sel) self.prof.active = other;
                    else if (self.prof.active == other) self.prof.active = sel;

                    changed = true;
                    new_sel = other;
                }
            }

            if (changed) {
                request_save_debounced(self);
                prof_fill_profile_list(self, pst->lb_profiles, new_sel);
                refresh_for_selection();
                update_statusbar(self);
            }
            return 0;
        }

        if (id == IDC_WATCH_ADD) {
            int sel = prof_get_sel(pst->lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(pst->e_watch);
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter an .exe name or path first."); return 0; }

            wchar_t norm[512]; norm[0] = 0;
            normalize_exe_input(raw.c_str(), norm, 512);
            if (!norm[0]) { msgbox_warn(dlg, L"Profiles", L"That doesn't look like a valid .exe name or path."); return 0; }

            bool added = false;
            {
                ModelLockGuard lk(self);
                if (sel >= 0 && sel < (int)self.prof.profiles.size()) {
                    auto& ws = self.prof.profiles[(size_t)sel].watch_exes;
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
                        self.prof.profiles[(size_t)sel].watch_keys_lower = self.prof.profiles[(size_t)sel].watch_exes;
                        rebuild_profile_watch_keys_locked(self);
                    }
                }
            }

            if (added) {
                SetWindowTextW(pst->e_watch, L"");
                request_save_debounced(self);
                prof_fill_watch_list(self, pst->lb_watch, sel);
                refresh_for_selection();
            } else {
                msgbox_info(dlg, L"Profiles", L"That WATCH exe is already in the list.");
            }
            return 0;
        }

        if (id == IDC_WATCH_REMOVE) {
            int sel = prof_get_sel(pst->lb_profiles);
            if (sel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            int wsel = prof_get_sel(pst->lb_watch);
            if (wsel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a WATCH exe first."); return 0; }

            bool removed = false;
            {
                ModelLockGuard lk(self);
                if (sel >= 0 && sel < (int)self.prof.profiles.size()) {
                    auto& ws = self.prof.profiles[(size_t)sel].watch_exes;
                    if (wsel >= 0 && wsel < (int)ws.size()) {
                        ws.erase(ws.begin() + wsel);
                        removed = true;
                        self.prof.profiles[(size_t)sel].watch_keys_lower = self.prof.profiles[(size_t)sel].watch_exes;
                        rebuild_profile_watch_keys_locked(self);
                    }
                }
            }

            if (removed) {
                request_save_debounced(self);
                prof_fill_watch_list(self, pst->lb_watch, sel);
                refresh_for_selection();
            }
            return 0;
        }

        // ---- Profile contents editors (services / exes) ----
        // Single-click toggles auto-stop checkbox in services list
        if (id == IDC_PCONT_SVC_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            int psel = prof_get_sel(pst->lb_profiles);
            int sel2 = prof_get_sel(pst->lb_svc);
            EnableWindow(pst->b_svc_del, psel >= 0 && sel2 >= 0);
            EnableWindow(pst->b_svc_toggle, psel >= 0 && sel2 >= 0);
            if (psel >= 0 && sel2 >= 0)
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_PCONT_SVC_TOGGLE, BN_CLICKED), 0);
            return 0;
        }
        // Single-click toggles auto-stop checkbox in exes list
        if (id == IDC_PCONT_EXE_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            int psel = prof_get_sel(pst->lb_profiles);
            int sel2 = prof_get_sel(pst->lb_exe);
            EnableWindow(pst->b_exe_del, psel >= 0 && sel2 >= 0);
            EnableWindow(pst->b_exe_toggle, psel >= 0 && sel2 >= 0);
            if (psel >= 0 && sel2 >= 0)
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_PCONT_EXE_TOGGLE, BN_CLICKED), 0);
            return 0;
        }

        if (id == IDC_PCONT_SVC_ADD) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring name = prof_get_edit_text(pst->e_svc);
            if (name.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter a service name first."); return 0; }

            bool autostop = (SendMessageW(pst->chk_svc, BM_GETCHECK, 0, 0) == BST_CHECKED);

            bool added = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_SVC];
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
                        apply_now = (self.prof.active == psel);
                    }
                }
            }
            if (!added) { msgbox_info(dlg, L"Profiles", L"That service is already in this profile."); return 0; }

            SetWindowTextW(pst->e_svc, L"");
            SendMessageW(pst->chk_svc, BM_SETCHECK, BST_UNCHECKED, 0);

            request_save_debounced(self);
            prof_fill_content_list(self, pst->lb_svc, psel, ItemKind::Svc);
            refresh_for_selection();

            if (apply_now) apply_active_profile_cfg_inplace(self);
            return 0;
        }

        if (id == IDC_PCONT_SVC_TOGGLE) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(pst->lb_svc);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a service first."); return 0; }

            // Get name from listbox (merged list — may not match profile rows index)
            std::wstring name = prof_get_lb_text(pst->lb_svc, isel);
            if (name.empty()) return 0;

            bool changed = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_SVC];
                    // Find by name
                    bool found = false;
                    for (auto& r : rows) {
                        if (_wcsicmp(r.name.c_str(), name.c_str()) == 0) {
                            r.auto_stop = !r.auto_stop;
                            found = true;
                            changed = true;
                            break;
                        }
                    }
                    // Not in profile yet (came from main list) — add with auto_stop=true
                    if (!found) {
                        rows.push_back(ItemRow{ name, true, L"" });
                        std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) {
                            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                        });
                        changed = true;
                    }
                    if (changed) apply_now = (self.prof.active == psel);
                }
            }
            if (changed) {
                request_save_debounced(self);
                // Update checkbox in-place — flip item data and repaint just that item
                LRESULT cur = SendMessageW(pst->lb_svc, LB_GETITEMDATA, (WPARAM)isel, 0);
                SendMessageW(pst->lb_svc, LB_SETITEMDATA, (WPARAM)isel, cur ? 0 : 1);
                RECT itemRc{};
                SendMessageW(pst->lb_svc, LB_GETITEMRECT, (WPARAM)isel, (LPARAM)&itemRc);
                InvalidateRect(pst->lb_svc, &itemRc, FALSE);
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_SVC_REMOVE) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(pst->lb_svc);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a service first."); return 0; }

            // Get name from listbox (merged list)
            std::wstring name = prof_get_lb_text(pst->lb_svc, isel);
            if (name.empty()) return 0;

            bool removed = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_SVC];
                    for (auto it = rows.begin(); it != rows.end(); ++it) {
                        if (_wcsicmp(it->name.c_str(), name.c_str()) == 0) {
                            rows.erase(it);
                            removed = true;
                            apply_now = (self.prof.active == psel);
                            break;
                        }
                    }
                }
            }
            if (removed) {
                request_save_debounced(self);
                prof_fill_content_list(self, pst->lb_svc, psel, ItemKind::Svc);
                refresh_for_selection();
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_EXE_ADD) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(pst->e_exe);
            if (raw.empty()) { msgbox_warn(dlg, L"Profiles", L"Enter an .exe name or path first."); return 0; }

            wchar_t norm[512]; norm[0] = 0;
            normalize_exe_input(raw.c_str(), norm, 512);
            if (!norm[0]) { msgbox_warn(dlg, L"Profiles", L"That doesn't look like a valid .exe name or path."); return 0; }

            bool autostop = (SendMessageW(pst->chk_exe, BM_GETCHECK, 0, 0) == BST_CHECKED);

            bool added = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_EXE];
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
                        apply_now = (self.prof.active == psel);
                    }
                }
            }
            if (!added) { msgbox_info(dlg, L"Profiles", L"That .exe is already in this profile."); return 0; }

            SetWindowTextW(pst->e_exe, L"");
            SendMessageW(pst->chk_exe, BM_SETCHECK, BST_UNCHECKED, 0);

            request_save_debounced(self);
            prof_fill_content_list(self, pst->lb_exe, psel, ItemKind::Exe);
            refresh_for_selection();
            if (apply_now) apply_active_profile_cfg_inplace(self);
            return 0;
        }

        if (id == IDC_PCONT_EXE_TOGGLE) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(pst->lb_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select an exe first."); return 0; }

            // Get name from listbox (merged list)
            std::wstring name = prof_get_lb_text(pst->lb_exe, isel);
            if (name.empty()) return 0;

            bool changed = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_EXE];
                    bool found = false;
                    for (auto& r : rows) {
                        if (_wcsicmp(r.name.c_str(), name.c_str()) == 0) {
                            r.auto_stop = !r.auto_stop;
                            found = true;
                            changed = true;
                            break;
                        }
                    }
                    // Not in profile yet (came from main list) — add with auto_stop=true
                    if (!found) {
                        rows.push_back(ItemRow{ name, true, L"" });
                        std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) {
                            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                        });
                        changed = true;
                    }
                    if (changed) apply_now = (self.prof.active == psel);
                }
            }
            if (changed) {
                request_save_debounced(self);
                // Update checkbox in-place — flip item data and repaint just that item
                LRESULT cur = SendMessageW(pst->lb_exe, LB_GETITEMDATA, (WPARAM)isel, 0);
                SendMessageW(pst->lb_exe, LB_SETITEMDATA, (WPARAM)isel, cur ? 0 : 1);
                RECT itemRc{};
                SendMessageW(pst->lb_exe, LB_GETITEMRECT, (WPARAM)isel, (LPARAM)&itemRc);
                InvalidateRect(pst->lb_exe, &itemRc, FALSE);
                if (apply_now) apply_active_profile_cfg_inplace(self);
            }
            return 0;
        }

        if (id == IDC_PCONT_EXE_REMOVE) {
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(pst->lb_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select an exe first."); return 0; }

            // Get name from listbox (merged list)
            std::wstring name = prof_get_lb_text(pst->lb_exe, isel);
            if (name.empty()) return 0;

            bool removed = false;
            bool apply_now = false;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    auto& rows = self.prof.profiles[(size_t)psel].cfg.items[KIND_EXE];
                    for (auto it = rows.begin(); it != rows.end(); ++it) {
                        if (_wcsicmp(it->name.c_str(), name.c_str()) == 0) {
                            rows.erase(it);
                            removed = true;
                            apply_now = (self.prof.active == psel);
                            break;
                        }
                    }
                }
            }
            if (removed) {
                request_save_debounced(self);
                prof_fill_content_list(self, pst->lb_exe, psel, ItemKind::Exe);
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
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }

            std::wstring raw = prof_get_edit_text(pst->e_start_exe);
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
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    ProfileSnapshot& p = self.prof.profiles[(size_t)psel];

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

                    if (self.prof.active == psel) start_now = true;
                }
            }

            if (added) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, pst->lb_start_exe, psel);
                if (is_exe && norm_exe[0]) prof_fill_content_list(self, pst->lb_exe, psel, ItemKind::Exe);
                refresh_for_selection();
            }

            if (start_now) {
                Action a;
                a.kind = ItemKind::Exe;
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
            int psel = prof_get_sel(pst->lb_profiles);
            if (psel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a profile first."); return 0; }
            int isel = prof_get_sel(pst->lb_start_exe);
            if (isel < 0) { msgbox_warn(dlg, L"Profiles", L"Select a start item first."); return 0; }

            std::wstring target = prof_get_lb_text(pst->lb_start_exe, isel);
            target = normalize_start_target_best_effort(target);
            if (target.empty()) return 0;

            bool removed = false;
            std::vector<StartedProc> to_close;
            {
                ModelLockGuard lk(self);
                if (psel >= 0 && psel < (int)self.prof.profiles.size()) {
                    ProfileSnapshot& p = self.prof.profiles[(size_t)psel];
                    for (size_t i = 0; i < p.start_items.size(); i++) {
                        if (_wcsicmp(p.start_items[i].target.c_str(), target.c_str()) == 0) {
                            p.start_items.erase(p.start_items.begin() + (ptrdiff_t)i);
                            removed = true;
                            break;
                        }
                    }
                }

                // If this profile is currently active, also stop any processes we started for this target.
                if (removed && self.prof.active == psel) {
                    for (size_t i = 0; i < self.prof.started_procs.size();) {
                        if (_wcsicmp(self.prof.started_procs[i].started_target.c_str(), target.c_str()) == 0) {
                            to_close.push_back(std::move(self.prof.started_procs[i]));
                            self.prof.started_procs.erase(self.prof.started_procs.begin() + (ptrdiff_t)i);
                            continue;
                        }
                        i++;
                    }

                    // Best-effort clear UI indicator if it was an EXE.
                    if (ends_with_i(target.c_str(), L".exe")) {
                        std::wstring key = exe_watch_key_lower(target);
                        if (!key.empty()) self.prof.started_exes.erase(key);
                    }
                }
            }

            if (removed) {
                request_save_debounced(self);
                prof_fill_start_exe_list(self, pst->lb_start_exe, psel);
                refresh_for_selection();
            }

            // Close outside the lock.
            for (auto& sp : to_close) {
                Action a;
                a.kind = ItemKind::Exe;
                a.op = ACTION_CLOSE_STARTED;
                a.name = sp.started_target;
                a.reason = L"profile active (close removed start-item)";
                a.wait_ms = self.cfg.stop_wait_ms;
                a.pid = sp.pid;
                if (sp.hproc) {
                    HANDLE dup = NULL;
                    if (DuplicateHandle(GetCurrentProcess(), sp.hproc.get(), GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                        a.hproc.reset(dup);
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
        if (new_dpi && new_dpi != pst->dpi) {
            scale_children(dlg, pst->dpi, new_dpi);
            pst->dpi = new_dpi;
            theme_compute_for_window(self, dlg);
            HFONT f = theme_ensure_font_for_dpi(self, pst->dpi);
            if (f) {
                apply_font_recursive(dlg, f);
                // Pill buttons get font via apply_font_recursive
            }
        }
        do_layout();
        return 0;
    }
    case WM_DESTROY:        pst->dpi = 96;
        return 0;

    case WM_CLOSE:
        DestroyWindow(dlg);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        delete pst;
        return 0;
    }
    return DefWindowProcW(dlg, msg, wParam, lParam);
}

void open_profiles_dialog(App& self, HWND parent) {
    WNDCLASSW wc{};
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
