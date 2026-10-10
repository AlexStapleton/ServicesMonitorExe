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
            if (selected) bk = RGB(65, 65, 65);
            else if (hot) bk = RGB(50, 50, 50);
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
                        text = theme_status_color(self, itp->status_kind, itp->auto_stop);
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
                SetDCBrushColor(hdc, bk);
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
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
    HGDIOBJ* objs[] = {
        (HGDIOBJ*)&self.theme.br_bg, (HGDIOBJ*)&self.theme.br_panel, (HGDIOBJ*)&self.theme.br_edit,
        (HGDIOBJ*)&self.theme.br_btn, (HGDIOBJ*)&self.theme.br_pressed, (HGDIOBJ*)&self.theme.br_border,
        (HGDIOBJ*)&self.theme.br_accent, (HGDIOBJ*)&self.theme.pen_border, (HGDIOBJ*)&self.theme.pen_accent,
        (HGDIOBJ*)&self.theme.pen_separator, (HGDIOBJ*)&self.theme.br_card,
    };
    if (self.theme.owns_brushes) {
        for (HGDIOBJ* o : objs) { if (*o) DeleteObject(*o); }
    }
    for (HGDIOBJ* o : objs) *o = NULL;
    self.theme.owns_brushes = false;
}

// Palette loosely follows the Windows 11 (Fluent) light/dark resources so the
// app sits naturally next to Settings/Explorer. All brushes are owned in both
// modes now (light mode used to borrow 3D system colors, which is where the
// Windows-95 look came from).
static void theme_update_resources(App& self) {
    theme_release_brushes(self);
    auto& t = self.theme;

    if (t.dark_mode) {
        t.col_bg           = RGB(32, 32, 32);
        t.col_panel        = RGB(32, 32, 32);
        t.col_card         = RGB(43, 43, 43);
        t.col_edit_bg      = RGB(39, 39, 39);
        t.col_text         = RGB(240, 240, 240);
        t.col_text_dim     = RGB(166, 166, 166);
        t.col_border       = RGB(58, 58, 58);
        t.col_ctrl         = RGB(45, 45, 45);
        t.col_ctrl_hot     = RGB(56, 56, 56);
        t.col_ctrl_pressed = RGB(39, 39, 39);
        t.col_accent       = RGB(76, 160, 224);
        t.col_on_accent    = RGB(0, 0, 0);
        t.col_ok           = RGB(108, 203, 95);
        t.col_warn         = RGB(252, 185, 0);
        t.col_err          = RGB(255, 153, 164);
    } else {
        t.col_bg           = RGB(243, 243, 243);
        t.col_panel        = RGB(243, 243, 243);
        t.col_card         = RGB(251, 251, 251);
        t.col_edit_bg      = RGB(255, 255, 255);
        t.col_text         = RGB(26, 26, 26);
        t.col_text_dim     = RGB(96, 96, 96);
        t.col_border       = RGB(219, 219, 219);
        t.col_ctrl         = RGB(253, 253, 253);
        t.col_ctrl_hot     = RGB(245, 245, 245);
        t.col_ctrl_pressed = RGB(237, 237, 237);
        t.col_accent       = RGB(0, 95, 184);
        t.col_on_accent    = RGB(255, 255, 255);
        t.col_ok           = RGB(15, 123, 15);
        t.col_warn         = RGB(157, 93, 0);
        t.col_err          = RGB(196, 43, 28);
    }

    t.br_bg      = CreateSolidBrush(t.col_bg);
    t.br_panel   = CreateSolidBrush(t.col_panel);
    t.br_card    = CreateSolidBrush(t.col_card);
    t.br_edit    = CreateSolidBrush(t.col_edit_bg);
    t.br_btn     = CreateSolidBrush(t.col_ctrl);
    t.br_pressed = CreateSolidBrush(t.col_ctrl_hot);
    t.br_border  = CreateSolidBrush(t.dark_mode ? RGB(65, 65, 65) : RGB(229, 241, 251));  // list selection
    t.br_accent  = CreateSolidBrush(t.col_accent);
    t.pen_border = CreatePen(PS_SOLID, 1, t.col_border);
    t.pen_accent = CreatePen(PS_SOLID, 1, t.col_accent);
    t.pen_separator = CreatePen(PS_SOLID, 1, t.col_border);
    t.owns_brushes = true;
}

// --------------------------------------------------
// Fonts
// --------------------------------------------------
static int CALLBACK font_exists_proc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
}

static bool font_family_installed(const wchar_t* face) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, LF_FACESIZE, face);
    bool found = false;
    EnumFontFamiliesExW(hdc, &lf, font_exists_proc, (LPARAM)&found, 0);
    ReleaseDC(NULL, hdc);
    return found;
}

// Segoe Fluent Icons (Win11) or Segoe MDL2 Assets (Win10). Resolved once.
static const wchar_t* icon_font_face() {
    static int state = -1;  // -1 unknown, 0 none, 1 fluent, 2 mdl2
    if (state < 0) {
        if (font_family_installed(L"Segoe Fluent Icons")) state = 1;
        else if (font_family_installed(L"Segoe MDL2 Assets")) state = 2;
        else state = 0;
    }
    return state == 1 ? L"Segoe Fluent Icons" : state == 2 ? L"Segoe MDL2 Assets" : nullptr;
}

static void theme_release_fonts(App& self) {
    for (HFONT* f : { &self.theme.font_title, &self.theme.font_bold, &self.theme.font_icon }) {
        if (*f) { DeleteObject(*f); *f = NULL; }
    }
}

static void theme_build_fonts(App& self, UINT dpi) {
    theme_release_fonts(self);
    LOGFONTW base{};
    if (self.ui_font) GetObjectW(self.ui_font, sizeof(base), &base);
    else { base.lfHeight = -dpi_scale(dpi, 12); StringCchCopyW(base.lfFaceName, LF_FACESIZE, L"Segoe UI"); }
    base.lfQuality = CLEARTYPE_QUALITY;

    LOGFONTW lf = base;
    lf.lfWeight = FW_SEMIBOLD;
    self.theme.font_bold = CreateFontIndirectW(&lf);

    lf.lfHeight = MulDiv(base.lfHeight, 4, 3);  // ~1.33x the body size
    self.theme.font_title = CreateFontIndirectW(&lf);

    if (const wchar_t* face = icon_font_face()) {
        LOGFONTW li{};
        li.lfHeight = -dpi_scale(dpi, 14);
        li.lfWeight = FW_NORMAL;
        li.lfCharSet = DEFAULT_CHARSET;
        li.lfQuality = CLEARTYPE_QUALITY;
        StringCchCopyW(li.lfFaceName, LF_FACESIZE, face);
        self.theme.font_icon = CreateFontIndirectW(&li);
    }
}

// --------------------------------------------------
// Drawing primitives
// --------------------------------------------------
void theme_fill_round(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCBrushColor(hdc, fill);
    SetDCPenColor(hdc, border);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
}

COLORREF theme_status_color(const App& self, uint8_t status_kind, bool auto_stop) {
    switch (status_kind) {
    case 1: return auto_stop ? self.theme.col_warn : self.theme.col_ok;  // running (warn: will be stopped)
    case 3: return self.theme.col_warn;                                  // transitioning
    case 2:                                                              // stopped
    case 4:                                                              // not installed
    default: return self.theme.col_text_dim;
    }
}

void ui_button_style(HWND btn, wchar_t glyph, UINT flags) {
    if (!btn) return;
    SetPropW(btn, L"SEM_GLYPH", (HANDLE)(UINT_PTR)glyph);
    SetPropW(btn, L"SEM_BTNSTYLE", (HANDLE)(UINT_PTR)flags);
    InvalidateRect(btn, NULL, FALSE);
}

// Paints every push button in the app (main window and dialogs) via
// DarkButtonSubclassProc: rounded, flat, hover/pressed states, optional icon
// glyph and accent fill.
void theme_paint_button(App& self, HWND btn, HDC hdc) {
    const auto& t = self.theme;
    RECT rc{}; GetClientRect(btn, &rc);
    const UINT dpi = self.dpi ? self.dpi : 96;

    const UINT flags = (UINT)(UINT_PTR)GetPropW(btn, L"SEM_BTNSTYLE");
    const wchar_t glyph = (wchar_t)(UINT_PTR)GetPropW(btn, L"SEM_GLYPH");
    const bool hot = GetPropW(btn, L"SEM_HOT") != NULL;
    const bool disabled = !IsWindowEnabled(btn);
    const bool pressed = (SendMessageW(btn, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    const bool accent = (flags & BTNSTYLE_ACCENT) && !disabled;
    const bool subtle = (flags & BTNSTYLE_SUBTLE) != 0;

    // Parent background first (rounded corners leave the corners visible).
    HWND parent = GetParent(btn);
    HBRUSH parent_br = (HBRUSH)SendMessageW(parent, WM_CTLCOLORBTN, (WPARAM)hdc, (LPARAM)btn);
    FillRect(hdc, &rc, parent_br ? parent_br : t.br_bg);

    COLORREF fill, border, text;
    if (accent) {
        fill = t.col_accent;
        if (pressed)  fill = RGB(GetRValue(fill) * 85 / 100, GetGValue(fill) * 85 / 100, GetBValue(fill) * 85 / 100);
        else if (hot) fill = RGB(GetRValue(fill) * 92 / 100, GetGValue(fill) * 92 / 100, GetBValue(fill) * 92 / 100);
        border = fill;
        text = t.col_on_accent;
    } else {
        fill = pressed ? t.col_ctrl_pressed : hot ? t.col_ctrl_hot : t.col_ctrl;
        border = t.col_border;
        text = disabled ? t.col_text_dim : t.col_text;
    }

    if (!subtle || hot || pressed) {
        theme_fill_round(hdc, rc, dpi_scale(dpi, 4), fill, subtle ? fill : border);
    }

    wchar_t txt[128]{};
    GetWindowTextW(btn, txt, 127);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);

    const bool have_glyph = glyph && t.font_icon;
    const bool icon_only = have_glyph && (flags & BTNSTYLE_ICON_ONLY);

    HFONT font = (HFONT)SendMessageW(btn, WM_GETFONT, 0, 0);
    if (!font) font = self.ui_font;

    SIZE ts{0, 0}, gs{0, 0};
    const int gap = dpi_scale(dpi, 8);
    HGDIOBJ oldf = SelectObject(hdc, font);
    if (!icon_only && txt[0]) GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &ts);
    if (have_glyph) {
        SelectObject(hdc, t.font_icon);
        GetTextExtentPoint32W(hdc, &glyph, 1, &gs);
    }
    int total = (int)gs.cx + ((have_glyph && ts.cx) ? gap : 0) + (int)ts.cx;
    int x = rc.left + std::max<int>(dpi_scale(dpi, 6), ((rc.right - rc.left) - total) / 2);

    if (have_glyph) {
        RECT gr{ x, rc.top, x + gs.cx, rc.bottom };
        DrawTextW(hdc, &glyph, 1, &gr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_NOCLIP);
        x += gs.cx + (ts.cx ? gap : 0);
    }
    if (!icon_only) {
        SelectObject(hdc, font);
        RECT tr{ x, rc.top, rc.right - dpi_scale(dpi, 4), rc.bottom };
        DrawTextW(hdc, txt, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (have_glyph ? 0 : 0));
    }
    SelectObject(hdc, oldf);

    if (GetFocus() == btn && !disabled && (SendMessageW(btn, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
        RECT fr = rc;
        InflateRect(&fr, -dpi_scale(dpi, 2), -dpi_scale(dpi, 2));
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(hdc, GetStockObject(DC_PEN));
        SetDCPenColor(hdc, t.col_text);
        RoundRect(hdc, fr.left, fr.top, fr.right, fr.bottom, dpi_scale(dpi, 6), dpi_scale(dpi, 6));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
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
            // Keep BS_OWNERDRAW (drawn via the parent's WM_DRAWITEM); the
            // subclass only adds hover tracking for owner-draw buttons.
            SetWindowSubclass(child, DarkButtonSubclassProc, 1, (DWORD_PTR)&self);
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
    // Keep this separate from theme_update_resources() so WM_DESTROY can cleanly release them.
    theme_release_brushes(self);
    theme_release_fonts(self);
}

HBRUSH theme_handle_ctlcolor(App& self, UINT msg, WPARAM wParam, LPARAM lParam) {
    HDC hdc = (HDC)wParam;
    HWND ctl = (HWND)lParam;
    if (!hdc) return nullptr;

    COLORREF text = self.theme.col_text;
    if (ctl && !IsWindowEnabled(ctl)) text = self.theme.col_text_dim;

    // Detail "card" children sit on the card surface, with a few accent colors.
    if (ctl && self.detail_panel && GetParent(ctl) == self.detail_panel) {
        if (ctl == self.detail_status_lbl) text = self.detail_status_col ? self.detail_status_col : self.theme.col_text;
        else if (ctl == self.detail_kind || ctl == self.detail_meta) text = self.theme.col_text_dim;
        SetTextColor(hdc, text);
        SetBkMode(hdc, (msg == WM_CTLCOLOREDIT) ? OPAQUE : TRANSPARENT);
        SetBkColor(hdc, self.theme.col_card);
        return self.theme.br_card;
    }
    if (ctl && (ctl == self.svc_empty || ctl == self.exe_empty)) {
        SetTextColor(hdc, self.theme.col_text_dim);
        SetBkMode(hdc, TRANSPARENT);
        return self.theme.br_edit;
    }
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

    // Tab-style buttons (main window header)
    if (ctlId == IDC_TOOLBAR_TAB_SVC || ctlId == IDC_TOOLBAR_TAB_EXE) {
        bool is_selected = (ctlId == IDC_TOOLBAR_TAB_SVC && self.active_tab == 0) ||
                           (ctlId == IDC_TOOLBAR_TAB_EXE && self.active_tab == 1);
        return theme_draw_pill(self, dis, is_selected);
    }

    // Any other owner-draw button normally gets converted to subclass painting
    // by theme_apply_to_control; paint identically if one slips through.
    theme_paint_button(self, dis->hwndItem, dis->hDC);
    return true;
}

// "Selector bar" tabs (Windows 11 style): plain text, selected tab gets
// semibold text and a short rounded accent bar underneath; hover gets a
// subtle rounded fill. Used by the main header and the Profiles dialog.
bool theme_draw_pill(App& self, const DRAWITEMSTRUCT* dis, bool is_selected) {
    if (!dis) return false;
    const auto& t = self.theme;
    const UINT dpi = self.dpi ? self.dpi : 96;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    HWND parent = GetParent(dis->hwndItem);
    HBRUSH parent_br = (HBRUSH)SendMessageW(parent, WM_CTLCOLORBTN, (WPARAM)hdc, (LPARAM)dis->hwndItem);
    FillRect(hdc, &rc, parent_br ? parent_br : t.br_bg);

    const bool hot = GetPropW(dis->hwndItem, L"SEM_HOT") != NULL;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    if (hot || pressed) {
        RECT hr = rc;
        InflateRect(&hr, 0, -dpi_scale(dpi, 2));
        theme_fill_round(hdc, hr, dpi_scale(dpi, 4), pressed ? t.col_ctrl_pressed : t.col_ctrl_hot,
                         pressed ? t.col_ctrl_pressed : t.col_ctrl_hot);
    }

    wchar_t txt[64]{0};
    GetWindowTextW(dis->hwndItem, txt, 63);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, is_selected ? t.col_text : t.col_text_dim);
    HFONT f = is_selected && t.font_bold ? t.font_bold : (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldf = f ? SelectObject(hdc, f) : NULL;
    DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldf) SelectObject(hdc, oldf);

    if (is_selected) {
        const int barW = dpi_scale(dpi, 18);
        const int barH = std::max(2, dpi_scale(dpi, 3));
        const int cx = (rc.left + rc.right) / 2;
        RECT bar{ cx - barW / 2, rc.bottom - barH - dpi_scale(dpi, 2), cx + barW / 2, rc.bottom - dpi_scale(dpi, 2) };
        theme_fill_round(hdc, bar, barH / 2 + 1, t.col_accent, t.col_accent);
    }

    if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
        RECT fr = rc;
        InflateRect(&fr, -dpi_scale(dpi, 1), -dpi_scale(dpi, 1));
        DrawFocusRect(hdc, &fr);
    }
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
        theme_build_fonts(self, dpi_now);
        changed = true;

        // ListView "spacer" imagelists (DPI dependent): 1px wide, row-height tall,
        // no images. The small imagelist height sets the report-view row height,
        // which is the standard way to get roomier rows without owner-draw. (The
        // old generic info/app icons on every row added noise, not information.)
        const int rowPx = dpi_scale(dpi_now, 28);

        if (self.il_svc) { ImageList_Destroy(self.il_svc); self.il_svc = NULL; }
        if (self.il_exe) { ImageList_Destroy(self.il_exe); self.il_exe = NULL; }

        self.il_svc = ImageList_Create(1, rowPx, ILC_COLOR32, 0, 1);
        self.il_exe = ImageList_Create(1, rowPx, ILC_COLOR32, 0, 1);

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
        if (self.detail_name && self.theme.font_title)
            SendMessageW(self.detail_name, WM_SETFONT, (WPARAM)self.theme.font_title, FALSE);
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
