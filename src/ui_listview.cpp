#include "app.h"

// ----------------------------
// ListView helpers
// ----------------------------
void lv_set_columns(HWND lv, const wchar_t* col0, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3, const wchar_t* col4) {
    ListView_DeleteAllItems(lv);
    while (ListView_DeleteColumn(lv, 0)) {}

    // Scale default column widths to the current DPI so the layout feels consistent.
    // These defaults match the "Reset Columns" handler (IDM_RESET_COLUMNS).
    UINT dpi = get_dpi_for_hwnd(lv);
    auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    c.fmt = LVCFMT_CENTER;
    c.pszText = (LPWSTR)col0;
    c.cx = S(40);
    ListView_InsertColumn(lv, 0, &c);

    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)col1;
    c.cx = S(260);
    ListView_InsertColumn(lv, 1, &c);

    c.pszText = (LPWSTR)col2;
    c.cx = S(100);
    ListView_InsertColumn(lv, 2, &c);

    c.pszText = (LPWSTR)col3;
    c.cx = S(200);
    ListView_InsertColumn(lv, 3, &c);

    c.pszText = (LPWSTR)col4;
    c.cx = S(160);
    ListView_InsertColumn(lv, 4, &c);

    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
}

void lv_set_columns(HWND lv, const wchar_t* col0, const wchar_t* col1, const wchar_t* col2, const wchar_t* col3) {
    ListView_DeleteAllItems(lv);
    while (ListView_DeleteColumn(lv, 0)) {}

    UINT dpi = get_dpi_for_hwnd(lv);
    auto S = [&](int v96) -> int { return dpi_scale(dpi, v96); };

    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    c.fmt = LVCFMT_CENTER;
    c.pszText = (LPWSTR)col0;
    c.cx = S(40);
    ListView_InsertColumn(lv, 0, &c);

    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)col1;
    c.cx = S(260);
    ListView_InsertColumn(lv, 1, &c);

    c.pszText = (LPWSTR)col2;
    c.cx = S(100);
    ListView_InsertColumn(lv, 2, &c);

    c.pszText = (LPWSTR)col3;
    c.cx = S(160);
    ListView_InsertColumn(lv, 3, &c);

    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
}

void lv_scale_columns(HWND lv, UINT old_dpi, UINT new_dpi) {
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


int lv_find_item_by_name(HWND lv, const wchar_t* name) {
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++) {
        wchar_t buf[512];
        ListView_GetItemText(lv, i, 1, buf, 512);
        if (_wcsicmp(buf, name) == 0) return i;
    }
    return -1;
}

int lv_find_item_by_uid(HWND lv, uint32_t uid) {
    if (!lv || !uid) return -1;
    LVFINDINFOW fi{};
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)uid;
    return ListView_FindItem(lv, -1, &fi);
}

void lv_autosize_status_last(App& self, ItemKind kind) {
    const int k = ki(kind);
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    // If the user already has saved column widths, don't override them with
    // LVSCW_AUTOSIZE which only measures currently-visible rows in owner-data
    // mode and gives inconsistent results.
    if (self.cols_have[k]) return;

    HWND hdr = ListView_GetHeader(lv);
    int cols = hdr ? Header_GetItemCount(hdr) : 5;
    for (int i = 2; i < cols; i++)
        ListView_SetColumnWidth(lv, i, LVSCW_AUTOSIZE_USEHEADER);
}

void lv_apply_name_fill(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    RECT rc;
    GetClientRect(lv, &rc);
    int total = rc.right - rc.left;

    HWND hdr = ListView_GetHeader(lv);
    int cols = hdr ? Header_GetItemCount(hdr) : 5;

    int used = ListView_GetColumnWidth(lv, 0);  // col 0 (Auto Stop)
    for (int i = 2; i < cols; i++)
        used += ListView_GetColumnWidth(lv, i);

    // DPI-scale the slack to prevent horizontal scrollbar at high DPI
    UINT dpi = self.dpi ? self.dpi : get_dpi_for_hwnd(lv);
    int slack = dpi_scale(dpi, 30);
    int avail = total - (used + slack);
    if (avail < dpi_scale(dpi, 120)) avail = dpi_scale(dpi, 120);
    ListView_SetColumnWidth(lv, 1, avail);
}

bool lv_set_row_with(App& self, HWND lv, Item* it, const wchar_t* status_raw, time_t wall_ts) {
    if (!lv || !it || it->name.empty()) return false;

    int idx = lv_find_item_by_uid(lv, it->uid);
    if (idx < 0) idx = lv_find_item_by_name(lv, it->name.c_str());

    // Build status display without heap allocations
    wchar_t status_disp[kStatusDispBuf];
    status_disp[0] = 0;
    StringCchCopyW(status_disp, kStatusDispBuf, (status_raw && status_raw[0]) ? status_raw : L"unknown");
    if (lv == self.lv_exe) {
        // Indicator: EXEs that were started automatically due to an active profile.
        if (self.prof.started_exes.find(it->name) != self.prof.started_exes.end()) {
            StringCchCatW(status_disp, kStatusDispBuf, L" (Profile)");
        }
    }

    // Indicator: services with AUTO-STOP suppression due to repeated stop errors.
    if (lv == self.lv_svc) {
        uint64_t now_ms = now_mono_ms();
        if (it->svc_stop_suppress_until_ms && now_ms < it->svc_stop_suppress_until_ms) {
            // Keep this compact and stable (no countdown) to avoid churn.
            StringCchCatW(status_disp, kStatusDispBuf, L" [SUPP");
            if (it->svc_last_stop_err) {
                wchar_t eb[32];
                StringCchPrintfW(eb, 32, L" e%lu", (unsigned long)it->svc_last_stop_err);
                StringCchCatW(status_disp, kStatusDispBuf, eb);
            }
            StringCchCatW(status_disp, kStatusDispBuf, L"]");
        }
    }

    bool inserted = false;

    if (idx < 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = ListView_GetItemCount(lv);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(L"");
        item.lParam = (LPARAM)it->uid;
        item.iImage = it->img;

        int row = ListView_InsertItem(lv, &item);
        inserted = (row >= 0);

        // Seed UI caches so subsequent updates can be cheap.
        StringCchCopyW(it->ui_cache_status_disp, kStatusDispBuf, status_disp);
        if (it->ui_last_update_wall != wall_ts) {
            fmt_time_local(it->ui_cache_last_text, 64, wall_ts);
            it->ui_last_update_wall = wall_ts;
        }

        ListView_SetItemText(lv, row, 1, (LPWSTR)it->name.c_str());
        ListView_SetItemText(lv, row, 2, it->ui_cache_status_disp);
        ListView_SetItemText(lv, row, 3, it->ui_cache_last_text);

        {
            SuppressLvNotifyGuard guard(self);
            ListView_SetCheckState(lv, row, it->auto_stop ? TRUE : FALSE);
        }
    } else {
        // Ensure the row is bound to this Item* for O(1) future lookup
        LVITEMW pi{};
        pi.mask = LVIF_PARAM | LVIF_IMAGE;
        pi.iItem = idx;
        pi.lParam = (LPARAM)it->uid;
        pi.iImage = it->img;
        ListView_SetItem(lv, &pi);

        // Update UI caches (status + last)
        if (wcscmp(it->ui_cache_status_disp, status_disp) != 0) {
            StringCchCopyW(it->ui_cache_status_disp, kStatusDispBuf, status_disp);
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
            SuppressLvNotifyGuard guard(self);
            ListView_SetCheckState(lv, idx, want);
        }
    }

    return inserted;
}

// Update an existing ListView row for Item* (NO insertion). Returns true if updated.
bool lv_update_row_existing_with(App& self, HWND lv, ItemKind kind, Item* it, const wchar_t* status_raw, time_t wall_ts, int* out_row_idx) {
    if (!lv || !it || it->name.empty()) return false;
    const int k = ki(kind);

    int idx = -1;
    auto& mp = self.lv_row_of_item[k];
    auto itf = mp.find(it);
    if (itf != mp.end()) idx = itf->second;

    // Validate cached row still points at this uid in the current view index
    if (idx >= 0) {
        if (idx >= (int)self.view_uids[k].size() || self.view_uids[k][idx] != it->uid) idx = -1;
    }

    if (idx < 0) {
        // Fallback: linear search in view index (no ListView lParam dependency)
        for (int i = 0; i < (int)self.view_uids[k].size(); ++i) {
            if (self.view_uids[k][i] == it->uid) { idx = i; break; }
        }
        if (idx < 0) return false;
        mp[it] = idx;
    }

    if (out_row_idx) *out_row_idx = idx;

    // Build / update cached display strings only when needed.
    // (Status text can change due to status_raw or the profile indicator prefix.)
    wchar_t status_disp[kStatusDispBuf];
    status_disp[0] = 0;
    StringCchCopyW(status_disp, kStatusDispBuf, (status_raw && status_raw[0]) ? status_raw : L"unknown");
    if (lv == self.lv_exe) {
        if (self.prof.started_exes.find(it->name) != self.prof.started_exes.end()) {
            // Prefix indicator
            wchar_t tmp[kStatusDispBuf];
            StringCchCopyW(tmp, kStatusDispBuf, status_disp);
            StringCchPrintfW(status_disp, kStatusDispBuf, L"* %s", tmp);
        }
    }

    // Only re-format the timestamp string if the wall time changed.
    if (it->ui_last_update_wall != wall_ts) {
        fmt_time_local(it->ui_cache_last_text, 64, wall_ts);
    }

    // Keep cached status display for quick comparisons and potential future owner-data support.
    StringCchCopyW(it->ui_cache_status_disp, kStatusDispBuf, status_disp);

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


bool lv_set_row(App& self, HWND lv, Item* it) {
    // Callers that hold self.mtx can use this wrapper safely.
    return lv_set_row_with(self, lv, it, it->last_status, it->last_update_wall);
}


bool lv_get_selected_name(HWND lv, wchar_t* out, size_t cch) {
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
void activity_append(App& self, HWND lb, const wchar_t* line) {
    if (!lb) return;

    int count = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    if (count >= MAX_LOG_LINES) {
        int drop = count - MAX_LOG_LINES + 1;
        for (int i = 0; i < drop; i++) SendMessageW(lb, LB_DELETESTRING, 0, 0);
    }

    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line);

    // Estimate horizontal extent from character count (avoids HDC + font selection per line)
    int len = (int)wcslen(line);
    int avgCharW = dpi_scale(self.dpi ? self.dpi : 96, 7);  // ~7px per char at 96 DPI
    int estimated = len * avgCharW + dpi_scale(self.dpi ? self.dpi : 96, 24);
    int cur = (int)SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0);
    if (estimated > cur) SendMessageW(lb, LB_SETHORIZONTALEXTENT, (WPARAM)estimated, 0);

    int newCount = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    SendMessageW(lb, LB_SETTOPINDEX, (WPARAM)std::max(0, newCount - 1), 0);
}

void ui_apply_status_updates(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    const int k = ki(kind);

    struct UiDelta {
        Item* it;
        wchar_t status[32];
        time_t wall;
    };

    std::vector<UiDelta> deltas;
    std::wstring flt;

    {
        ModelLockGuard lk(self);
        flt = self.filter[k];

        auto& dirty = self.dirty_status[k];
        // De-dupe during collection with a set — O(n) instead of sort+unique O(n log n)
        std::unordered_set<Item*> seen;
        seen.reserve(dirty.size());
        deltas.reserve(dirty.size());
        for (Item* it : dirty) {
            if (!it || !seen.insert(it).second) continue;
            UiDelta d{};
            d.it = it;
            StringCchCopyW(d.status, 32, it->last_status);
            d.wall = it->last_update_wall;
            deltas.push_back(d);
        }
        dirty.clear();
    }

    if (deltas.empty()) return;

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


void log_linef(App& self, const wchar_t* fmt, ...) {
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
bool item_matches_filter(const Item* it, const std::wstring& flt_lower) {
    if (!it) return false;
    if (flt_lower.empty()) return true;

    // Match against cached lowercase name (no allocation)
    if (it->name_lower.find(flt_lower) != std::wstring::npos) return true;

    // Case-insensitive status match (no allocation or lowercasing needed)
    if (StrStrIW(it->last_status, flt_lower.c_str()) != nullptr) return true;

    return false;
}

void rebuild_listview_filtered(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    const int k = ki(kind);

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

    {
        SuppressLvNotifyGuard guard(self);

        // Owner-data: set item count; rows are virtual and text/state comes via LVN_GETDISPINFO.
        SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
        ListView_SetItemCountEx(lv, (int)self.view_uids[k].size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lv, NULL, FALSE);
    }

    self.lv_needs_layout[k] = true;
    post_model_dirty(self);

    // Re-apply active sort after rebuilding the view
    lv_sort_view(self, kind);
}

// ----------------------------
// Column sorting
// ----------------------------

// Returns the text key for a given column index for sorting purposes.
// For owner-data listviews the text lives on the Item, not the control.
static const wchar_t* sort_key_for_item(const Item* it, ItemKind kind, int col) {
    if (!it) return L"";
    if (kind == ItemKind::Svc) {
        // Services: 0=Auto Stop, 1=Name, 2=Status, 3=Last Update
        switch (col) {
            case 1: return it->name.c_str();
            case 2: return it->ui_cache_status_disp;
            case 3: return it->ui_cache_last_text;
            default: return L"";
        }
    } else {
        // EXEs: 0=Auto Stop, 1=Name, 2=Status, 3=Path, 4=Last Update
        switch (col) {
            case 1: return it->name.c_str();
            case 2: return it->ui_cache_status_disp;
            case 3: return it->exe_path.c_str();
            case 4: return it->ui_cache_last_text;
            default: return L"";
        }
    }
}

void lv_sort_view(App& self, ItemKind kind) {
    const int k = ki(kind);
    const int col = self.sort_col[k];
    if (col < 0) return;

    const bool asc = self.sort_asc[k];

    {
        ModelLockGuard lk(self);

        // Sort view_uids in-place by resolving each uid → Item* for comparison.
        std::stable_sort(self.view_uids[k].begin(), self.view_uids[k].end(),
            [&](uint32_t a_uid, uint32_t b_uid) -> bool {
                Item* a = uid_to_item_ptr_unlocked(self, a_uid);
                Item* b = uid_to_item_ptr_unlocked(self, b_uid);

                int cmp;
                if (col == 0) {
                    // Auto Stop column: sort by checkbox state
                    bool as = a ? a->auto_stop : false;
                    bool bs = b ? b->auto_stop : false;
                    cmp = (as == bs) ? 0 : (as ? -1 : 1);
                } else {
                    const wchar_t* ak = sort_key_for_item(a, kind, col);
                    const wchar_t* bk = sort_key_for_item(b, kind, col);
                    cmp = _wcsicmp(ak, bk);
                }
                return asc ? (cmp < 0) : (cmp > 0);
            });

        // Rebuild row-of-item map
        self.lv_row_of_item[k].clear();
        for (int i = 0; i < (int)self.view_uids[k].size(); ++i) {
            Item* it = uid_to_item_ptr_unlocked(self, self.view_uids[k][i]);
            if (it) self.lv_row_of_item[k][it] = i;
        }
    }

    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (lv) {
        InvalidateRect(lv, NULL, FALSE);
    }

    lv_apply_sort_header(self, kind);
}

void lv_apply_sort_header(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    HWND hdr = ListView_GetHeader(lv);
    if (!hdr) return;
    int ncol = Header_GetItemCount(hdr);

    const int k = ki(kind);
    const int sort_c = self.sort_col[k];

    for (int i = 0; i < ncol; i++) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTDOWN | HDF_SORTUP);
        if (i == sort_c) {
            hi.fmt |= self.sort_asc[k] ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(hdr, i, &hi);
    }
}

void lv_on_column_click(App& self, ItemKind kind, int col) {
    const int k = ki(kind);

    if (self.sort_col[k] == col) {
        // Same column: toggle direction
        self.sort_asc[k] = !self.sort_asc[k];
    } else {
        self.sort_col[k] = col;
        self.sort_asc[k] = true;
    }

    lv_sort_view(self, kind);
}


Item* lv_get_selected_item_ptr(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return nullptr;

    const int k = ki(kind);

    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) sel = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
    if (sel < 0) sel = ListView_GetSelectionMark(lv);
    if (sel < 0) return nullptr;

    ModelReadGuard lk(self);
    if (sel < 0 || sel >= (int)self.view_uids[k].size()) return nullptr;
    uint32_t uid = self.view_uids[k][sel];
    if (!uid) return nullptr;
    auto f = self.uid_map.find(uid);
    return (f == self.uid_map.end()) ? nullptr : f->second;
}

void toggle_autostop_selected(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    if (!lv) return;

    Item* sel = lv_get_selected_item_ptr(self, kind);
    if (!sel) { msgbox_warn(self.hwnd, L"Auto-stop", L"Select a row first."); return; }

    bool now = false;
    {
        ModelReadGuard lk(self);
        Item* real = uid_to_item_ptr_unlocked(self, sel->uid);
        if (!real) return;
        now = !real->auto_stop;
    }

    (void)cmd_set_autostop_uid(self, sel->uid, now);

    int row = lv_find_item_by_uid(lv, sel->uid);
    if (row >= 0) {
        {
            SuppressLvNotifyGuard guard(self);
            ListView_SetCheckState(lv, row, now ? TRUE : FALSE);
        }
        // Only the toggled row's status coloring can change — repaint just
        // that row instead of the whole control.
        ListView_RedrawItems(lv, row, row);
    }
}

void show_item_details(App& self, ItemKind kind) {
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
        (kind == ItemKind::Svc) ? L"Service" : L"EXE",
        it->last_status,
        last,
        it->auto_stop ? L"Enabled" : L"Disabled",
        it->autostop_count);

    msgbox_info(self.hwnd, L"Details", buf);
}

void open_file_location_best_effort(App& self, const wchar_t* exeOrPath) {
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
