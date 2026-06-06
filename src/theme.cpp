// theme.cpp — Theme / dark-mode visual helpers
#include "app.h"

// Forward declaration — defined in darkmode.cpp
extern LRESULT CALLBACK DarkButtonSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// --------------------------------------------------
// Internal helpers (static)
// --------------------------------------------------

static LRESULT theme_customdraw_tab(App& self, NMCUSTOMDRAW* cd) {
    if (!self.theme.dark_mode || !cd) return CDRF_DODEFAULT;

    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        // Paint the tab background area once.
        if (cd->hdc) FillRect(cd->hdc, &cd->rc, self.theme.br_panel);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        // Fully owner-draw each tab item so it can't stay light.
        HDC hdc = cd->hdc;
        if (!hdc) return CDRF_DODEFAULT;

        // Fill item background
        FillRect(hdc, &cd->rc, self.theme.br_panel);

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
    SetTextColor(hdc, self.theme.col_text);

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
    if (!self.theme.dark_mode || !cd) return CDRF_DODEFAULT;

    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        if (cd->hdc) FillRect(cd->hdc, &cd->rc, self.theme.br_panel);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        HDC hdc = cd->hdc;
        if (!hdc) return CDRF_DODEFAULT;

        // Paint header section background
        FillRect(hdc, &cd->rc, self.theme.br_panel);

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
        SetTextColor(hdc, self.theme.col_text);
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
    if (!lcd) return CDRF_DODEFAULT;

    const DWORD stage = lcd->nmcd.dwDrawStage;

    switch (stage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        // Palette is already cached on ThemeState for both modes — no need to
        // re-query GetSysColor per cell in light mode.
        COLORREF text = self.theme.col_text;
        COLORREF bk   = self.theme.col_edit_bg;

        const bool selected = (lcd->nmcd.uItemState & CDIS_SELECTED) != 0;
        const bool hot      = (lcd->nmcd.uItemState & CDIS_HOT) != 0;

        // Dark mode: custom selection/hot background
        if (self.theme.dark_mode) {
            if (selected) bk = DK_BORDER;
            else if (hot) bk = DK_PRESSED;
        }

        // Colored status text for subitem 2 (Status column) — both modes
        if (lcd->iSubItem == 2) {
            int row = (int)lcd->nmcd.dwItemSpec;
            HWND lvHwnd = lcd->nmcd.hdr.hwndFrom;
            int k = -1;
            if (lvHwnd == self.lv_svc) k = KIND_SVC;
            else if (lvHwnd == self.lv_exe) k = KIND_EXE;

            if (k >= 0 && row >= 0 && row < (int)self.view_uids[k].size()) {
                uint32_t uid = self.view_uids[k][row];
                auto it_f = self.uid_map.find(uid);
                if (it_f != self.uid_map.end()) {
                    Item* itp = it_f->second;
                    if (itp) {
                        // Use pre-classified status_kind (set whenever last_status
                        // changes) instead of _wcsicmp in this per-cell hot path.
                        if (itp->status_kind == 1) {           // running
                            text = self.theme.dark_mode ? RGB(80, 200, 80) : RGB(0, 140, 0);
                        } else if (itp->status_kind == 2) {    // stopped
                            if (itp->auto_stop) {
                                text = self.theme.dark_mode ? RGB(200, 80, 80) : RGB(180, 40, 40);
                            } else {
                                text = self.theme.dark_mode ? RGB(140, 140, 140) : RGB(128, 128, 128);
                            }
                        }
                    }
                }
            }
        }

        lcd->clrText   = text;
        lcd->clrTextBk = bk;

        // Dark mode: manually paint background (some builds won't fill the row)
        if (self.theme.dark_mode) {
            HDC hdc = lcd->nmcd.hdc;
            if (hdc) {
                RECT rc = lcd->nmcd.rc;
                HBRUSH br = selected ? self.theme.br_border
                          : hot      ? self.theme.br_pressed
                          :            self.theme.br_edit;
                FillRect(hdc, &rc, br);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, text);
            }
        }

        return CDRF_NEWFONT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static void theme_release_brushes(App& self) {
    // Light-mode pill objects are always owned (not system brushes)
    if (self.theme.br_lt_accent) { DeleteObject(self.theme.br_lt_accent); self.theme.br_lt_accent = NULL; }
    if (self.theme.pen_lt_accent) { DeleteObject(self.theme.pen_lt_accent); self.theme.pen_lt_accent = NULL; }
    if (self.theme.pen_lt_border) { DeleteObject(self.theme.pen_lt_border); self.theme.pen_lt_border = NULL; }

    if (!self.theme.owns_brushes) return;
    if (self.theme.br_bg) { DeleteObject(self.theme.br_bg); self.theme.br_bg = NULL; }
    if (self.theme.br_panel) { DeleteObject(self.theme.br_panel); self.theme.br_panel = NULL; }
    if (self.theme.br_edit) { DeleteObject(self.theme.br_edit); self.theme.br_edit = NULL; }
    if (self.theme.br_btn) { DeleteObject(self.theme.br_btn); self.theme.br_btn = NULL; }
    if (self.theme.br_pressed) { DeleteObject(self.theme.br_pressed); self.theme.br_pressed = NULL; }
    if (self.theme.br_border) { DeleteObject(self.theme.br_border); self.theme.br_border = NULL; }
    if (self.theme.br_accent) { DeleteObject(self.theme.br_accent); self.theme.br_accent = NULL; }
    if (self.theme.pen_border) { DeleteObject(self.theme.pen_border); self.theme.pen_border = NULL; }
    if (self.theme.pen_accent) { DeleteObject(self.theme.pen_accent); self.theme.pen_accent = NULL; }
    if (self.theme.pen_separator) { DeleteObject(self.theme.pen_separator); self.theme.pen_separator = NULL; }
    self.theme.owns_brushes = false;
}

static void theme_update_resources(App& self) {
    theme_release_brushes(self);

    if (self.theme.dark_mode) {
        self.theme.col_bg = RGB(32, 32, 32);
        self.theme.col_panel = RGB(38, 38, 38);
        self.theme.col_edit_bg = RGB(45, 45, 45);
        self.theme.col_text = RGB(235, 235, 235);
        self.theme.col_text_dim = RGB(180, 180, 180);

        self.theme.br_bg = CreateSolidBrush(self.theme.col_bg);
        self.theme.br_panel = CreateSolidBrush(self.theme.col_panel);
        self.theme.br_edit = CreateSolidBrush(self.theme.col_edit_bg);
        self.theme.br_btn = CreateSolidBrush(self.theme.col_panel);

        // Cached hot-path GDI objects
        self.theme.br_pressed = CreateSolidBrush(DK_PRESSED);
        self.theme.br_border = CreateSolidBrush(DK_BORDER);
        self.theme.br_accent = CreateSolidBrush(DK_ACCENT);
        self.theme.pen_border = CreatePen(PS_SOLID, 1, DK_BORDER);
        self.theme.pen_accent = CreatePen(PS_SOLID, 1, DK_ACCENT);
        self.theme.pen_separator = CreatePen(PS_SOLID, 1, DK_SEPARATOR);

        self.theme.owns_brushes = true;
    } else {
        self.theme.col_bg = GetSysColor(COLOR_BTNFACE);
        self.theme.col_panel = GetSysColor(COLOR_BTNFACE);
        self.theme.col_edit_bg = GetSysColor(COLOR_WINDOW);
        self.theme.col_text = GetSysColor(COLOR_WINDOWTEXT);
        self.theme.col_text_dim = GetSysColor(COLOR_GRAYTEXT);

        self.theme.br_bg = GetSysColorBrush(COLOR_BTNFACE);
        self.theme.br_panel = GetSysColorBrush(COLOR_BTNFACE);
        self.theme.br_edit = GetSysColorBrush(COLOR_WINDOW);
        self.theme.br_btn = GetSysColorBrush(COLOR_BTNFACE);
        self.theme.owns_brushes = false;

        // Light-mode pill tab cached GDI (always owned, freed independently of owns_brushes)
        self.theme.br_lt_accent = CreateSolidBrush(LT_ACCENT);
        self.theme.pen_lt_accent = CreatePen(PS_SOLID, 1, LT_ACCENT);
        self.theme.pen_lt_border = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
    }
}

static HFONT theme_find_font_cached(App& self, UINT dpi) {
    if (!dpi) dpi = 96;
    if (self.ui_font && self.theme.built_dpi == dpi) return self.ui_font;
    auto it = self.ui_font_cache.find(dpi);
    return (it != self.ui_font_cache.end()) ? it->second : nullptr;
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

void theme_apply_to_control(App& self, HWND child) {
    if (!child) return;
    darkmode_allow_window(child, self.theme.dark_mode);
    wchar_t cls[64]{0};
    GetClassNameW(child, cls, 63);

    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
        // Use DarkMode_CFD theme for dark scrollbars; Explorer theme for light mode.
        if (self.theme.dark_mode) {
            dll_cache_init();
            if (g_dll_cache.SetWindowTheme)
                g_dll_cache.SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        } else {
            try_set_explorer_theme_mode(child, false);
        }
        // ListView colors
        ListView_SetBkColor(child, self.theme.dark_mode ? self.theme.col_edit_bg : GetSysColor(COLOR_WINDOW));
        ListView_SetTextBkColor(child, self.theme.dark_mode ? self.theme.col_edit_bg : GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(child, self.theme.dark_mode ? self.theme.col_text : GetSysColor(COLOR_WINDOWTEXT));

        // Header often ignores listview theming unless explicitly themed.
        HWND hdr = ListView_GetHeader(child);
        if (hdr) {
            try_set_explorer_theme_mode(hdr, self.theme.dark_mode);
            InvalidateRect(hdr, NULL, FALSE);
        }

        InvalidateRect(child, NULL, FALSE);
        return;
    }
    if (lstrcmpiW(cls, WC_TABCONTROLW) == 0) {
        // Force owner-draw tabs in dark mode so they reliably repaint dark on all systems.
        LONG_PTR st = GetWindowLongPtrW(child, GWL_STYLE);
        if (self.theme.dark_mode) st |= TCS_OWNERDRAWFIXED;
        else st &= ~((LONG_PTR)TCS_OWNERDRAWFIXED);
        SetWindowLongPtrW(child, GWL_STYLE, st);
        SetWindowPos(child, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        try_set_explorer_theme_mode(child, self.theme.dark_mode);
        InvalidateRect(child, NULL, FALSE);
        return;
    }
    if (lstrcmpiW(cls, L"Button") == 0) {
        // Push buttons: subclass paint in dark mode so we don't rely on BS_OWNERDRAW + parent WM_DRAWITEM.
        LONG_PTR st = GetWindowLongPtrW(child, GWL_STYLE);

        const LONG_PTR type = (st & BS_TYPEMASK);

        // Pill-style buttons must keep BS_OWNERDRAW — they're drawn via WM_DRAWITEM in their parent.
        int ctl_id = GetDlgCtrlID(child);
        const bool is_pill = (ctl_id == IDC_TOOLBAR_TAB_SVC || ctl_id == IDC_TOOLBAR_TAB_EXE
                           || (ctl_id >= IDC_PROF_TAB_ACTIVATION && ctl_id <= IDC_PROF_TAB_EXE));
        if (is_pill) {
            // Just apply theme hint + invalidate; don't change style or subclass.
            try_set_explorer_theme_mode(child, self.theme.dark_mode);
            InvalidateRect(child, NULL, FALSE);
            return;
        }

        const bool is_push = (type == BS_PUSHBUTTON) || (type == BS_DEFPUSHBUTTON)
                          || (type == BS_OWNERDRAW);
        const bool is_checkish =
            (type == BS_AUTOCHECKBOX) || (type == BS_CHECKBOX) ||
            (type == BS_AUTORADIOBUTTON) || (type == BS_RADIOBUTTON) ||
            (type == BS_GROUPBOX);

        if (is_push && !is_checkish) {
            // Ensure owner-draw is OFF (we paint via subclass instead).
            // Convert BS_OWNERDRAW to BS_PUSHBUTTON so the subclass handles painting.
            if (type == BS_OWNERDRAW) {
                st = (st & ~((LONG_PTR)BS_TYPEMASK)) | BS_PUSHBUTTON;
            }
            st &= ~((LONG_PTR)BS_OWNERDRAW);
            SetWindowLongPtrW(child, GWL_STYLE, st);

            // Always install subclass — it handles painting in both light and dark mode,
            // since we've converted BS_OWNERDRAW to BS_PUSHBUTTON and the system won't draw these.
            SetWindowSubclass(child, DarkButtonSubclassProc, 1, (DWORD_PTR)&self);

            SetWindowPos(child, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }

        // Theme hint still helps non-push button variants a bit.
        try_set_explorer_theme_mode(child, self.theme.dark_mode);

        InvalidateRect(child, NULL, FALSE);
        return;
    }
    if (lstrcmpiW(cls, WC_HEADER) == 0) {
        try_set_explorer_theme_mode(child, self.theme.dark_mode);
        InvalidateRect(child, NULL, FALSE);
        return;
    }

    if (lstrcmpiW(cls, STATUSCLASSNAMEW) == 0) {
        SendMessageW(child, SB_SETBKCOLOR, 0, (LPARAM)(self.theme.dark_mode ? self.theme.col_panel : CLR_DEFAULT));
        InvalidateRect(child, NULL, FALSE);
        return;
    }
    if (_wcsnicmp(cls, L"RICHEDIT", 8) == 0) {
#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#endif
        SendMessageW(child, EM_SETBKGNDCOLOR, 0, (LPARAM)(self.theme.dark_mode ? self.theme.col_edit_bg : GetSysColor(COLOR_WINDOW)));
        InvalidateRect(child, NULL, FALSE);
        return;
    }
    if (lstrcmpiW(cls, L"EDIT") == 0 || lstrcmpiW(cls, L"ListBox") == 0) {
        // Dark scrollbars via DarkMode_CFD; Explorer theme for light mode.
        dll_cache_init();
        if (g_dll_cache.SetWindowTheme)
            g_dll_cache.SetWindowTheme(child, self.theme.dark_mode ? L"DarkMode_CFD" : L"Explorer", nullptr);
        InvalidateRect(child, NULL, FALSE);
        return;
    }
// Default: let WM_CTLCOLOR handle drawing.
    InvalidateRect(child, NULL, TRUE);
}

// --------------------------------------------------
// Public API (declared in app.h)
// --------------------------------------------------

void theme_release_resources(App& self) {
    // Currently we only own brushes for custom dark/light colors.
    // Keep this separate from theme_update_resources() so WM_DESTROY can cleanly release them.
    theme_release_brushes(self);
}

HBRUSH theme_handle_ctlcolor(App& self, UINT msg, WPARAM wParam, LPARAM lParam) {
    HDC hdc = (HDC)wParam;
    HWND ctl = (HWND)lParam;
    if (!hdc) return nullptr;

    COLORREF text = self.theme.col_text;
    if (ctl && !IsWindowEnabled(ctl)) text = self.theme.col_text_dim;
    SetTextColor(hdc, text);

    switch (msg) {
    case WM_CTLCOLORSTATIC:
        // Transparent labels (let parent paint), but keep consistent text color.
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, self.theme.col_bg);
        return self.theme.br_bg;
    case WM_CTLCOLOREDIT:
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, self.theme.col_edit_bg);
        return self.theme.br_edit;
    case WM_CTLCOLORLISTBOX:
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, self.theme.col_edit_bg);
        return self.theme.br_edit;
    case WM_CTLCOLORBTN:
        SetBkMode(hdc, TRANSPARENT);
        SetBkColor(hdc, self.theme.col_panel);
        return self.theme.br_panel;
    }
    return nullptr;
}

bool theme_draw_owner_button(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON) return false;

    int ctlId = (int)dis->CtlID;

    // Pill-style tab buttons (draw in both light and dark mode)
    if (ctlId == IDC_TOOLBAR_TAB_SVC || ctlId == IDC_TOOLBAR_TAB_EXE) {
        bool is_selected = (ctlId == IDC_TOOLBAR_TAB_SVC && self.active_tab == 0) ||
                           (ctlId == IDC_TOOLBAR_TAB_EXE && self.active_tab == 1);
        return theme_draw_pill(self, dis, is_selected);
    }

    // Regular owner-draw buttons (both light and dark mode)
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool hot      = (dis->itemState & ODS_HOTLIGHT) != 0;

    if (self.theme.dark_mode) {
        // Dark mode — use cached brushes/pens
        FillRect(hdc, &rc, pressed ? self.theme.br_pressed : self.theme.br_panel);
        HGDIOBJ oldp = SelectObject(hdc, self.theme.pen_border);
        HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldb);
        SelectObject(hdc, oldp);
    } else {
        // Light mode — system colors
        COLORREF bgCol = pressed ? GetSysColor(COLOR_BTNHIGHLIGHT)
                       : hot     ? GetSysColor(COLOR_BTNFACE)
                       :           GetSysColor(COLOR_BTNFACE);
        HBRUSH bgBr = CreateSolidBrush(bgCol);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);
        // Standard 3D-style border
        DrawEdge(hdc, &rc, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
    }

    // Text
    wchar_t txt[256]{0};
    GetWindowTextW(dis->hwndItem, txt, 255);

    SetBkMode(hdc, TRANSPARENT);
    if (self.theme.dark_mode) {
        SetTextColor(hdc, disabled ? self.theme.col_text_dim : self.theme.col_text);
    } else {
        SetTextColor(hdc, disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_BTNTEXT));
    }

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

bool theme_draw_pill(App& self, const DRAWITEMSTRUCT* dis, bool is_selected) {
    if (!dis) return false;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    // Fill background behind pill (parent bg)
    FillRect(hdc, &rc, self.theme.br_bg);

    // Draw pill shape
    int radius = (rc.bottom - rc.top) / 2;
    if (radius < 4) radius = 4;

    if (is_selected) {
        HBRUSH pillBr = self.theme.dark_mode ? self.theme.br_accent : self.theme.br_lt_accent;
        HPEN pillPen = self.theme.dark_mode ? self.theme.pen_accent : self.theme.pen_lt_accent;
        HGDIOBJ oldBr = SelectObject(hdc, pillBr);
        HGDIOBJ oldPen = SelectObject(hdc, pillPen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
    } else {
        HPEN borderPen = self.theme.dark_mode ? self.theme.pen_border : self.theme.pen_lt_border;
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
    }

    // Text
    wchar_t txt[64]{0};
    GetWindowTextW(dis->hwndItem, txt, 63);
    SetBkMode(hdc, TRANSPARENT);
    if (is_selected) {
        SetTextColor(hdc, RGB(255, 255, 255));
    } else {
        SetTextColor(hdc, self.theme.dark_mode ? self.theme.col_text_dim : RGB(80, 80, 80));
    }
    HFONT hfont = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldf = NULL;
    if (hfont) oldf = SelectObject(hdc, hfont);
    DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldf) SelectObject(hdc, oldf);

    return true;
}

bool theme_draw_owner_tab(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_TAB) return false;
    if (!self.theme.dark_mode) return false;

    HWND htab = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    // Background — use cached brushes
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(hdc, &rc, selected ? self.theme.br_pressed : self.theme.br_panel);

    // Border — use cached pen
    HGDIOBJ oldp = SelectObject(hdc, self.theme.pen_border);
    HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);

    // Text
    wchar_t txt[256]{0};
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    it.pszText = txt;
    it.cchTextMax = 255;
    int idx = (int)dis->itemID;
    TabCtrl_GetItem(htab, idx, &it);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, self.theme.col_text);

    RECT tr = rc;
    InflateRect(&tr, -8, -3);
    DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    return true;
}

bool theme_draw_owner_menu(App& self, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return false;
    if (!self.theme.dark_mode) return false;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(hdc, &rc, selected ? self.theme.br_pressed : self.theme.br_bg);

    // Text from the menu handle if available.
    wchar_t txt[256]{0};
    if (dis->hwndItem) {
        GetMenuStringW((HMENU)dis->hwndItem, dis->itemID, txt, 255, MF_BYCOMMAND);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? self.theme.col_text_dim : self.theme.col_text);

    RECT tr = rc;
    InflateRect(&tr, -8, 0);
    DrawTextW(hdc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    return true;
}

void theme_apply_to_window(App& self, HWND hwnd) {
    if (!hwnd) return;

    // Reduce flicker while we retheme.
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);

    darkmode_allow_window(hwnd, self.theme.dark_mode);
    try_enable_dark_titlebar(hwnd, self.theme.dark_mode);
    EnumChildWindows(hwnd, enum_theme_children_proc, (LPARAM)&self);

    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

HFONT theme_ensure_font_for_dpi(App& self, UINT dpi) {
    if (!dpi) dpi = 96;
    // Main font is stored in self.ui_font.
    if (self.ui_font && self.theme.built_dpi == dpi) return self.ui_font;

    if (HFONT f = theme_find_font_cached(self, dpi)) return f;

    HFONT fnew = create_ui_font_for_dpi(dpi);
    if (fnew) self.ui_font_cache.emplace(dpi, fnew);
    return fnew;
}

void theme_compute_for_window(App& self, HWND hwnd) {
    // Ensure base theme resources are built (idempotent) and that we have a font
    // for this window's DPI (dialogs can be moved across monitors).
    theme_compute(self);
    UINT dpi = get_dpi_for_hwnd(hwnd);
    (void)theme_ensure_font_for_dpi(self, dpi);
}

void theme_compute(App& self) {
    // Rebuild theme resources (colors/brushes) and DPI-dependent resources (font/imagelists)
    // in a single place. This function is idempotent: calling it twice with the same
    // (dpi,dark_mode) inputs produces no resource churn.
    const UINT dpi_now = self.dpi ? self.dpi : 96;
    const bool dark_now = self.theme.dark_mode;

    const bool need_brushes = (!self.theme.built_dpi) || (self.theme.built_dark != dark_now);
    const bool need_dpi_resources = (!self.theme.built_dpi) || (self.theme.built_dpi != dpi_now);

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
        self.theme.built_dpi = dpi_now;
        self.theme.built_dark = dark_now;
        self.theme.gen.fetch_add(1, std::memory_order_relaxed);
    }
}

void theme_apply_all_controls(App& self) {
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
        if (mb) { theme_menu_set_owner_draw(mb, self.theme.dark_mode); DrawMenuBar(self.hwnd); }
    }
}

LRESULT theme_handle_customdraw(App& self, NMHDR* nh, LPARAM lParam, bool& handled) {
    handled = false;
    if (!nh || nh->code != NM_CUSTOMDRAW) return 0;

    wchar_t ccls[64]{0};
    GetClassNameW(nh->hwndFrom, ccls, 63);
    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lParam);

    if (lstrcmpiW(ccls, WC_TABCONTROLW) == 0) {
        handled = true;
        return theme_customdraw_tab(self, cd);
    }
    if (lstrcmpiW(ccls, WC_HEADER) == 0) {
        handled = true;
        return theme_customdraw_header(self, cd);
    }
    // Note: Win32 statusbar ignores NM_CUSTOMDRAW for text rendering — the
    // statusbar uses SBT_OWNERDRAW + WM_DRAWITEM (see main.cpp) instead.
    if (lstrcmpiW(ccls, WC_LISTVIEWW) == 0) {
        handled = true;
        return theme_customdraw_listview(self, (NMLVCUSTOMDRAW*)cd);
    }
    return 0;
}
