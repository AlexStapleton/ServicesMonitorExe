// main.cpp — wWinMain, MainWndProc/AppWndProc, item CRUD, tab sync, UI timers
#include "app.h"

// ============================================================
// UI logging (via message to main thread)
// ============================================================
void post_log(App& self, const wchar_t* text) {
    auto* m = new (std::nothrow) std::wstring(text ? text : L"");
    if (!m) return;

    if (!PostMessageW(self.hwnd, WM_APP_LOG, 0, (LPARAM)m)) {
        delete m;
    }
}

// Bulk status update (generation-only, no heap churn):
// - Monitor thread updates Item::last_status/last_update_wall under lock.
// - UI thread repaints only rows that changed (via WM_APP_STATUS_GEN / UI timer).
void post_status_bulk(App& self, ItemKind kind, const std::vector<std::wstring>& names, const std::vector<StatusStr>& statuses, size_t n, time_t wall_ts) {
    if (n == 0) return;
    const int k = ki(kind);

    bool any_changed = false;

    {
        ModelLockGuard lk(self);
        self.last_any_status_wall = wall_ts;

        if (self.dirty_status[k].capacity() < self.items[k].v.size())
            self.dirty_status[k].reserve(self.items[k].v.size());

        for (size_t i = 0; i < n; i++) {
            if (i >= names.size() || names[i].empty()) continue;
            Item* it = list_find(&self.items[k], names[i].c_str());
            if (!it) continue;

            const wchar_t* ns = statuses[i].buf[0] ? statuses[i].buf : L"unknown";
            if (wcscmp(it->last_status, ns) != 0) {
                set_item_status(*it, ns);
                it->last_update_wall = wall_ts;
                it->status_gen++;
                self.dirty_status[k].push_back(it);
                any_changed = true;
            }
        }

        if (any_changed) self.status_gen[k]++;
    }

    if (any_changed) {
        PostMessageW(self.hwnd, WM_APP_STATUS_GEN, (WPARAM)k, (LPARAM)(INT_PTR)self.status_gen[k]);
    }
}

void request_save_debounced(App& self) {
    PostMessageW(self.hwnd, WM_APP_REQUEST_SAVE, 0, 0);
}

static void wake_monitor(App& self) {
    {
        std::lock_guard<std::shared_mutex> lk(self.mtx);
        self.threads.mon_wake = true;
    }
    self.threads.mon_cv.notify_one();
}

void post_model_dirty(App& self) {
    if (self.ui_dirty_posted) return;
    self.ui_dirty_posted = true;
    PostMessageW(self.hwnd, WM_APP_MODEL_DIRTY, 0, 0);
    wake_monitor(self);
}

// ============================================================
// Show / hide / exit helpers
// ============================================================
static void ui_timer_suspend(App& self, HWND hwnd);
static void ui_timer_resume(App& self, HWND hwnd);
static void restart_ui_timer(App& self, HWND hwnd);

void show_main_window(App& self) {
    HWND hwnd = self.hwnd;
    if (!hwnd) return;

    if (self.have_last_wp) {
        WINDOWPLACEMENT wp = self.last_wp;
        wp.length = sizeof(wp);
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE || wp.showCmd == SW_SHOWMINNOACTIVE) {
            wp.showCmd = SW_SHOWNORMAL;
        }
        SetWindowPlacement(hwnd, &wp);
    }

    ShowWindow(hwnd, SW_SHOW);
    ui_timer_resume(self, hwnd);

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    restart_ui_timer(self, hwnd);
    post_model_dirty(self);
}

void hide_main_window(App& self) {
    if (!self.hwnd) return;
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (GetWindowPlacement(self.hwnd, &wp)) {
        self.last_wp = wp;
        self.have_last_wp = true;
    }
    ui_timer_suspend(self, self.hwnd);
    ShowWindow(self.hwnd, SW_HIDE);
}

void app_request_exit(App& self, HWND hwnd) {
    self.tray.force_quit = true;
    DestroyWindow(hwnd);
}

// ============================================================
// Item CRUD
// ============================================================
static void add_item(App& self, ItemKind kind) {
    const int k = ki(kind);
    HWND edit = self.action_add_edit;

    wchar_t raw[512];
    GetWindowTextW(edit, raw, 512);
    trim_ws_inplace(raw);
    if (!raw[0]) {
        msgbox_warn(self.hwnd, L"Add", L"Name is required.");
        return;
    }

    wchar_t name[512];
    wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
    if (kind == ItemKind::Svc) {
        resolve_service_name(self, raw, name, 512);
        wchar_t st[32];
        if (!query_service_status_fast(name, st, 32)) {
            wchar_t err[900];
            StringCchPrintfW(err, 900,
                L"Service not found.\n\nInput: %s\nResolved as: %s\n\nTip: try the service key name (e.g., 'wuauserv').",
                raw, name);
            msgbox_err(self.hwnd, L"Add service", err);
            return;
        }
    } else {
        normalize_exe_input(raw, name, 512);
        if (!name[0]) {
            msgbox_err(self.hwnd, L"Add EXE", L"Executable name is invalid.");
            return;
        }
        resolve_exe_launch_path(raw, exe_path_full, MAX_PATH);
    }

    Item* exist = NULL;
    {
        ModelReadGuard lk(self);
        exist = list_find(&self.items[k], name);
    }
    if (exist) {
        msgbox_info(self.hwnd, L"Add", L"Already monitored.");
        return;
    }

    Item* it = new (std::nothrow) Item();
    if (!it) return;
    it->kind = kind;
    it->name = name;
    if (kind == ItemKind::Exe && exe_path_full[0]) it->exe_path = exe_path_full;
    it->auto_stop = false;
    it->img = (kind == ItemKind::Svc) ? 0 : 1;
    it->autostop_count = 0;
    it->last_autostop_mono_ms = 0;
    it->last_update_wall = time(NULL);

    if (kind == ItemKind::Svc) {
        wchar_t st[32];
        query_service_status_fast(name, st, 32);
        set_item_status(*it, st);
    } else {
        int c = process_count_by_name_lower(name);
        set_item_status(*it, (c > 0) ? L"running" : L"stopped");
    }

    {
        ModelLockGuard lk(self);
        bool ok = register_item_locked(self, kind, it);
        if (ok) {
            self.items_gen[k]++;
        } else {
            delete it;
            it = nullptr;
        }
    }
    wake_monitor(self);  // wake monitor to pick up new item immediately

    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    (void)lv;
    rebuild_listview_filtered(self, kind);

    log_linef(self, L"Monitoring started (%s): %s (status: %s)",
              (kind == ItemKind::Svc) ? L"service" : L"exe", it->name.c_str(), it->last_status);
    request_save_debounced(self);

    self.lv_needs_layout[k] = true;
    update_statusbar(self);

    SetWindowTextW(edit, L"");
}

static void remove_selected(App& self, ItemKind kind) {
    const int k = ki(kind);
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) return;

    bool removed = false;
    {
        ModelLockGuard lk(self);
        Item* it = list_find(&self.items[k], name);
        if (it) {
            unregister_item_locked(self, kind, it);
            self.items_gen[k]++;
            removed = true;
        }
    }
    if (!removed) return;
    wake_monitor(self);  // wake monitor to update snapshot

    int idx = lv_find_item_by_name(lv, name);
    if (idx >= 0) ListView_DeleteItem(lv, idx);

    log_linef(self, L"Monitoring removed (%s): %s", (kind == ItemKind::Svc) ? L"service" : L"exe", name);
    request_save_debounced(self);

    self.lv_needs_layout[k] = true;
    update_statusbar(self);
}

static void stop_selected(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) {
        msgbox_warn(self.hwnd, L"Stop", L"Select a row first.");
        return;
    }

    log_linef(self, L"%s: %s\u2026", name, (kind == ItemKind::Svc) ? L"stop requested" : L"terminate requested");
    int wait_ms = 0;
    {
        ModelReadGuard lk(self);
        wait_ms = self.cfg.stop_wait_ms;
    }
    action_enqueue(self, kind, ACTION_STOP, name, L"manual stop", wait_ms);
}

static void start_selected(App& self, ItemKind kind) {
    HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
    wchar_t name[512];
    if (!lv_get_selected_name(lv, name, 512)) {
        msgbox_warn(self.hwnd, L"Start", L"Select a row first.");
        return;
    }

    log_linef(self, L"%s: %s\u2026", name, (kind == ItemKind::Svc) ? L"start requested" : L"launch requested");
    int wait_ms = 0;
    {
        ModelReadGuard lk(self);
        wait_ms = self.cfg.stop_wait_ms;
    }
    action_enqueue(self, kind, ACTION_START, name, L"manual start", wait_ms);
}

// Row checkbox toggle
bool cmd_set_autostop_uid(App& self, uint32_t uid, bool enabled, std::wstring* out_name_opt) {
    std::wstring name_for_log;
    bool changed = false;
    {
        ModelLockGuard lk(self);
        Item* it = uid_to_item_ptr_unlocked(self, uid);
        if (!it) return false;
        if (it->auto_stop != enabled) {
            it->auto_stop = enabled;
            changed = true;
        }
        name_for_log = it->name;
    }
    if (out_name_opt) *out_name_opt = name_for_log;
    if (changed && !name_for_log.empty()) {
        log_linef(self, L"%s: auto-stop %s", name_for_log.c_str(), enabled ? L"enabled" : L"disabled");
        post_model_dirty(self);
        request_save_debounced(self);
    }
    return changed;
}

// ============================================================
// ListView notify handler
// ============================================================
static bool handle_main_listview_notify(App& self, HWND hwnd, NMHDR* nh, LPARAM lParam, LRESULT& out) {
    if (!nh) return false;
    if (!(nh->idFrom == IDC_LV_SVC || nh->idFrom == IDC_LV_EXE)) return false;

    const ItemKind kind = (nh->idFrom == IDC_LV_SVC) ? ItemKind::Svc : ItemKind::Exe;
    const int kind_i = ki(kind);

    if (nh->code == LVN_GETDISPINFO) {
        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
        if (!di) { out = 0; return true; }

        const int row = di->item.iItem;
        if (row < 0) { out = 0; return true; }

        // Shared lock: resolve UID → Item* and populate UI caches.
        // UI cache fields (ui_cache_*) are only accessed from the UI thread,
        // so a read lock on the model is sufficient to safely read last_status/last_update_wall.
        Item* itp = nullptr;
        uint32_t uid = 0;
        {
            ModelReadGuard lk(self);
            if (row < (int)self.view_uids[kind_i].size()) uid = self.view_uids[kind_i][row];
            if (uid) {
                auto f = self.uid_map.find(uid);
                if (f != self.uid_map.end()) itp = f->second;
            }
            // Populate UI caches while we hold the lock (avoids a third lock later)
            if (itp) {
                if (itp->ui_cache_status_disp[0] == 0 && itp->last_status[0])
                    StringCchCopyW(itp->ui_cache_status_disp, kStatusDispBuf, itp->last_status);
                if (itp->ui_cache_last_text[0] == 0 && itp->last_update_wall)
                    fmt_time_local(itp->ui_cache_last_text, 64, itp->last_update_wall);
            }
        }
        if (!itp) { out = 0; return true; }

        if (di->item.mask & LVIF_PARAM) di->item.lParam = (LPARAM)uid;

        if (di->item.mask & LVIF_STATE) {
            di->item.stateMask = LVIS_STATEIMAGEMASK;
            di->item.state = INDEXTOSTATEIMAGEMASK(itp->auto_stop ? 2 : 1);
        }

        if ((di->item.mask & LVIF_TEXT) && di->item.pszText) {
            switch (di->item.iSubItem) {
                case 0: di->item.pszText = (LPWSTR)L""; break;      // checkbox
                case 1: di->item.pszText = (LPWSTR)itp->name.c_str(); break;  // name
                case 2: di->item.pszText = (LPWSTR)itp->ui_cache_status_disp; break;  // status
                case 3: {
                    if (kind == ItemKind::Svc) {
                        // Services: col 3 = Last Update (no Description column)
                        di->item.pszText = (LPWSTR)itp->ui_cache_last_text;
                    } else {
                        // EXEs: col 3 = Path
                        di->item.pszText = !itp->exe_path.empty()
                            ? (LPWSTR)itp->exe_path.c_str() : (LPWSTR)L"";
                    }
                    break;
                }
                case 4: di->item.pszText = (LPWSTR)itp->ui_cache_last_text; break;  // last update (EXE only)
                default: di->item.pszText = (LPWSTR)L""; break;
            }
        }

        out = 0;
        return true;
    }

    if (nh->code == NM_CLICK) {
        HWND lvh = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
        if (lvh) {
            DWORD pos = GetMessagePos();
            POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
            ScreenToClient(lvh, &pt);
            LVHITTESTINFO hti{};
            hti.pt = pt;
            if (ListView_SubItemHitTest(lvh, &hti) != -1) {
                const bool on_checkbox = (hti.iSubItem == 0) && ((hti.flags & LVHT_ONITEMSTATEICON) != 0);
                if (on_checkbox && hti.iItem >= 0) {
                    uint32_t uid = 0;
                    {
                        ModelReadGuard lk(self);
                        if (hti.iItem < (int)self.view_uids[kind_i].size()) uid = self.view_uids[kind_i][hti.iItem];
                    }
                    if (uid) {
                        const bool now = (ListView_GetCheckState(lvh, hti.iItem) == FALSE);

                        {
                            SuppressLvNotifyGuard guard(self);
                            ListView_SetCheckState(lvh, hti.iItem, now ? TRUE : FALSE);
                        }

                        (void)cmd_set_autostop_uid(self, uid, now);
                        InvalidateRect(lvh, NULL, FALSE);
                        UpdateWindow(lvh);
                    }
                    out = 0;
                    return true;
                }
            }
        }
    }

    if (nh->code == LVN_ITEMCHANGED) {
        if (self.suppress_lv_notify) { out = 0; return true; }
        NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
        if (lv && (lv->uChanged & LVIF_STATE)) {
            // Checkbox toggle (auto-stop)
            if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) {
                if (lv->iItem >= 0) {
                    uint32_t uid = 0;
                    {
                        ModelReadGuard lk(self);
                        if (lv->iItem < (int)self.view_uids[kind_i].size()) uid = self.view_uids[kind_i][lv->iItem];
                    }

                    const UINT state_img = ((UINT)lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    const bool enabled = (state_img >= 2);

                    if (uid) (void)cmd_set_autostop_uid(self, uid, enabled);
                }
            }
            // Selection change: update left info pane
            if ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED) {
                Item* sel = lv_get_selected_item_ptr(self, kind);
                update_detail_panel(self, kind, sel);
            }
        }
        out = 0;
        return true;
    }

    if (nh->code == LVN_COLUMNCLICK) {
        NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
        if (nmlv) lv_on_column_click(self, kind, nmlv->iSubItem);
        out = 0;
        return true;
    }

    // Double-click / Enter opens a quick details dialog
    if (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE) {
        show_item_details(self, kind);
        out = 0;
        return true;
    }

    if (nh->code == LVN_KEYDOWN) {
        NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
        if (!kd) return false;

        if ((GetKeyState(VK_CONTROL) & 0x8000) && kd->wVKey == 'C') {
            Item* it = lv_get_selected_item_ptr(self, kind);
            if (it) clipboard_set_text(hwnd, it->name.c_str());
            out = 0;
            return true;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && kd->wVKey == 'F') {
            HWND edit = (kind == ItemKind::Svc) ? self.svc_search : self.exe_search;
            if (edit) {
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            out = 0;
            return true;
        }
        if (kd->wVKey == VK_SPACE) {
            toggle_autostop_selected(self, kind);
            out = 0;
            return true;
        }
    }

    return false;
}

// ============================================================
// Tab sync + UI timer management
// ============================================================
static void sync_tabs_to(App& self, int sel) {
    if (sel < 0) sel = 0;
    if (sel > 1) sel = 1;

    self.active_tab = sel;

    show_only(self.center_page_svc, self.center_page_exe, (sel == 0));

    // Show/hide the appropriate search edit
    if (self.svc_search) ShowWindow(self.svc_search, (sel == 0) ? SW_SHOW : SW_HIDE);
    if (self.exe_search) ShowWindow(self.exe_search, (sel == 1) ? SW_SHOW : SW_HIDE);

    // Show/hide Browse button (EXE only)
    if (self.action_browse_btn) ShowWindow(self.action_browse_btn, (sel == 1) ? SW_SHOW : SW_HIDE);

    // Update cue banner on the add edit
    if (self.action_add_edit) {
        if (sel == 0)
            set_cue_banner(self.action_add_edit, L"Service name or display name\u2026");
        else
            set_cue_banner(self.action_add_edit, L"EXE name or full path\u2026");
    }

    // Update pill button visual state
    if (self.toolbar_tab_svc) InvalidateRect(self.toolbar_tab_svc, NULL, FALSE);
    if (self.toolbar_tab_exe) InvalidateRect(self.toolbar_tab_exe, NULL, FALSE);

    // Re-layout to adjust Browse button space
    if (self.hwnd) layout(self, self.hwnd);
}

static void ui_timer_suspend(App& self, HWND hwnd) {
    if (self.ui_timer_suspended) return;
    self.ui_timer_suspended = true;
    KillTimer(hwnd, TIMER_UI_REFRESH);
}

static UINT ui_refresh_fallback_interval_ms(const App& self) {
    int ms = self.cfg.ui_refresh_ms;
    if (ms < UI_REFRESH_MS_MIN) ms = UI_REFRESH_MS_MIN;
    if (ms > UI_REFRESH_MS_MAX) ms = UI_REFRESH_MS_MAX;
    return (UINT)ms;
}

static void ui_timer_resume(App& self, HWND hwnd) {
    if (!self.ui_timer_suspended) return;
    self.ui_timer_suspended = false;
    SetTimer(hwnd, TIMER_UI_REFRESH, ui_refresh_fallback_interval_ms(self), NULL);
}

static void restart_ui_timer(App& self, HWND hwnd) {
    if (self.ui_timer_suspended) { KillTimer(hwnd, TIMER_UI_REFRESH); return; }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) { KillTimer(hwnd, TIMER_UI_REFRESH); return; }

    KillTimer(hwnd, TIMER_UI_REFRESH);
    SetTimer(hwnd, TIMER_UI_REFRESH, ui_refresh_fallback_interval_ms(self), NULL);
}

static void ui_refresh_if_needed(App& self, HWND hwnd) {
    if (self.ui_timer_suspended || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;

    uint32_t tg = self.theme.gen.load(std::memory_order_relaxed);
    if (self.ui_seen_theme_gen != tg) {
        theme_apply_all_controls(self);
        self.ui_seen_theme_gen = tg;
        InvalidateRect(hwnd, NULL, TRUE);
    }

    if (self.lv_needs_layout[KIND_SVC]) {
        lv_autosize_status_last(self, ItemKind::Svc);
        lv_apply_name_fill(self, ItemKind::Svc);
        self.lv_needs_layout[KIND_SVC] = false;
    }
    if (self.lv_needs_layout[KIND_EXE]) {
        lv_autosize_status_last(self, ItemKind::Exe);
        lv_apply_name_fill(self, ItemKind::Exe);
        self.lv_needs_layout[KIND_EXE] = false;
    }

    update_statusbar(self);
}

// ============================================================
// Main window proc (GWLP_USERDATA binding)
// ============================================================
static LRESULT AppWndProc(App& self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void schedule_initial_layout(App& self, HWND hwnd) {
    if (self.did_initial_layout) return;
    if (self.initial_layout_scheduled) return;
    self.initial_layout_scheduled = true;
    PostMessageW(hwnd, WM_APP_INITIAL_LAYOUT, 0, 0);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self_ptr = reinterpret_cast<App*>(cs ? cs->lpCreateParams : nullptr);
        if (self_ptr) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self_ptr));
            self_ptr->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self_ptr = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self_ptr) return DefWindowProcW(hwnd, msg, wParam, lParam);

    App& self = *self_ptr;
    return AppWndProc(self, hwnd, msg, wParam, lParam);
}

static LRESULT AppWndProc(App& self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

if (self.tray.taskbar_restart_msg && msg == self.tray.taskbar_restart_msg) {
    self.tray.added = false;
    tray_sync(self, GetModuleHandleW(NULL));
    return 0;
}

switch (msg) {
    case WM_CREATE: {
        self.hwnd = hwnd;

        self.dpi = get_dpi_for_hwnd(hwnd);
        if (self.dpi == 0) self.dpi = 96;

        theme_compute(self);

        try_enable_mica_backdrop(hwnd);

        int saved_tab = self.active_tab; // preserve before build_ui resets to 0
        build_ui(self, hwnd);

        theme_apply_all_controls(self);

        self.menu_settings = CreatePopupMenu();
        AppendMenuW(self.menu_settings, MF_STRING, IDM_PREFS, L"Preferences\u2026");
        AppendMenuW(self.menu_settings, MF_STRING, IDM_PROFILES, L"Profiles\u2026");
        AppendMenuW(self.menu_settings, MF_STRING | (self.theme.dark_mode ? MF_CHECKED : 0), IDM_DARKMODE, L"Dark mode");
        AppendMenuW(self.menu_settings, MF_STRING, IDM_HOTKEYS, L"Hotkeys\u2026");
        AppendMenuW(self.menu_settings, MF_STRING, IDM_DIAGNOSTICS, L"Diagnostics\u2026");
        AppendMenuW(self.menu_settings, MF_SEPARATOR, 0, NULL);
        AppendMenuW(self.menu_settings, MF_STRING, IDM_EXIT_TO_TRAY, L"Exit to tray");
        AppendMenuW(self.menu_settings, MF_STRING, IDM_EXIT, L"Quit");
        self.tray.taskbar_restart_msg = RegisterWindowMessageW(L"TaskbarCreated");

        allow_uipi_message(hwnd, WM_APP_TRAYICON);
        if (self.tray.taskbar_restart_msg) allow_uipi_message(hwnd, self.tray.taskbar_restart_msg);

        if (!enum_services_build_disp_cache(&self.disp_cache)) {
            log_linef(self, L"Display cache: enumerate failed");
        }

        restart_ui_timer(self, hwnd);

        tray_sync(self, GetModuleHandleW(NULL));

        rebuild_listview_filtered(self, ItemKind::Svc);
        rebuild_listview_filtered(self, ItemKind::Exe);
        self.lv_needs_layout[KIND_SVC] = true;
        self.lv_needs_layout[KIND_EXE] = true;

        // Restore saved active tab (load_config sets it before build_ui resets to 0)
        if (saved_tab >= 0 && saved_tab <= 1) sync_tabs_to(self, saved_tab);

        schedule_initial_layout(self, hwnd);

        self.threads.action_threads.reserve(kActionWorkerCount);
        for (int i = 0; i < kActionWorkerCount; i++)
            self.threads.action_threads.emplace_back(action_thread_main, &self);
        self.threads.monitor_thread = std::jthread(monitor_thread_main, &self);

        // Flush any log messages queued before the UI existed (e.g. config load errors).
        for (auto& msg : self.deferred_logs) post_log(self, msg.c_str());
        self.deferred_logs.clear();
        self.deferred_logs.shrink_to_fit();

        update_statusbar(self);
        return 0;
    }

    case WM_SHOWWINDOW:
        if (wParam && !self.did_initial_layout) {
            schedule_initial_layout(self, hwnd);
        }
        return 0;

    case WM_WINDOWPOSCHANGED: {
        if (!self.did_initial_layout) {
            const WINDOWPOS* wp = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (wp && !(wp->flags & SWP_NOSIZE)) {
                schedule_initial_layout(self, hwnd);
            }
        }
        return 0;
    }

    case WM_APP_INITIAL_LAYOUT: {
        self.initial_layout_scheduled = false;

        if (self.did_initial_layout) return 0;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        if (cw <= 1 || ch <= 1) {
            if (self.initial_layout_tries++ < 25) {
                schedule_initial_layout(self, hwnd);
            }
            return 0;
        }

        UINT dpi_now = get_dpi_for_hwnd(hwnd);
        if (dpi_now == 0) dpi_now = 96;
        if (self.dpi == 0) self.dpi = dpi_now;

        if (dpi_now != self.dpi) {
            UINT oldDpi = self.dpi;
            UINT newDpi = dpi_now;
            self.dpi = newDpi;

            if (self.detail_panel_w > 0 && oldDpi && newDpi && oldDpi != newDpi) {
                self.detail_panel_w = MulDiv(self.detail_panel_w, (int)newDpi, (int)oldDpi);
            }

            theme_compute(self);
            theme_apply_all_controls(self);

            lv_scale_columns(self.lv_svc, oldDpi, newDpi);
            lv_scale_columns(self.lv_exe, oldDpi, newDpi);
            self.lv_needs_layout[KIND_SVC] = true;
            self.lv_needs_layout[KIND_EXE] = true;

            listbox_adjust_item_height(self.activity, newDpi);
        }

        layout(self, hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateWindow(hwnd);

        self.did_initial_layout = true;
        return 0;
    }

    case WM_DPICHANGED: {
        UINT oldDpi = self.dpi ? self.dpi : 96;
        UINT newDpi = HIWORD(wParam);
        if (newDpi == 0) newDpi = get_dpi_for_hwnd(hwnd);
        self.dpi = newDpi;

        if (self.detail_panel_w > 0 && oldDpi && newDpi && oldDpi != newDpi) {
            self.detail_panel_w = MulDiv(self.detail_panel_w, (int)newDpi, (int)oldDpi);
        }

        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        if (prc) {
            SetWindowPos(hwnd, NULL, prc->left, prc->top,
                prc->right - prc->left, prc->bottom - prc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        theme_compute(self);
        theme_apply_all_controls(self);

        lv_scale_columns(self.lv_svc, oldDpi, newDpi);
        lv_scale_columns(self.lv_exe, oldDpi, newDpi);
        self.lv_needs_layout[KIND_SVC] = true;
        self.lv_needs_layout[KIND_EXE] = true;

        listbox_adjust_item_height(self.activity, newDpi);

        layout(self, hwnd);
        return 0;
    }

    case WM_SETTINGCHANGE: {
        // High-contrast toggle: when active, force the system palette so
        // accessibility themes are respected instead of our dark/light colors.
        if (wParam == SPI_SETHIGHCONTRAST ||
            (lParam && wcscmp((const wchar_t*)lParam, L"HighContrast") == 0)) {
            HIGHCONTRASTW hc{};
            hc.cbSize = sizeof(hc);
            if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
                bool hc_on = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
                // When HC turns on we force light/system so GetSysColor pulls
                // the accessibility palette. When it turns off, leave whatever
                // the user previously had (do not clobber their dark toggle).
                if (hc_on && self.theme.dark_mode) {
                    self.theme.dark_mode = false;
                    darkmode_set_app(false);
                    theme_compute(self);
                    theme_apply_all_controls(self);
                    self.ui_seen_theme_gen = self.theme.gen.load(std::memory_order_relaxed);
                } else {
                    // HC might have changed colors even without flipping our
                    // dark flag — rebuild cached palette/brushes.
                    theme_compute(self);
                    theme_apply_all_controls(self);
                    self.ui_seen_theme_gen = self.theme.gen.load(std::memory_order_relaxed);
                }
            }
            return 0;
        }
        if (lParam && wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet") == 0) {
            HKEY hk = NULL;
            bool sys_dark = false;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
                DWORD val = 1, sz = sizeof(val);
                if (RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL,
                        (BYTE*)&val, &sz) == ERROR_SUCCESS) {
                    sys_dark = (val == 0);
                }
                RegCloseKey(hk);
            }
            if (sys_dark != self.theme.dark_mode) {
                self.theme.dark_mode = sys_dark;
                darkmode_set_app(self.theme.dark_mode);
                theme_compute(self);
                theme_apply_all_controls(self);
                self.ui_seen_theme_gen = self.theme.gen.load(std::memory_order_relaxed);
                request_save_debounced(self);
            }
        }
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        self.in_size_move = true;
        SetTimer(hwnd, TIMER_LIVE_RESIZE, 16, NULL);
        return 0;

    case WM_EXITSIZEMOVE:
        self.in_size_move = false;
        KillTimer(hwnd, TIMER_LIVE_RESIZE);
        layout(self, hwnd);
        post_model_dirty(self);
        restart_ui_timer(self, hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            UINT dpi = self.dpi ? self.dpi : 96;
            // Minimum window size so all toolbar/action strip controls fit.
            // 700 accommodates EXE tab: edit+Add+Browse+Stop+Start+Remove+gaps+pad.
            int minCW = dpi_scale(dpi, 700);
            int minCH = dpi_scale(dpi, 340);
            RECT rcw{}, rcc{};
            if (self.hwnd && GetWindowRect(self.hwnd, &rcw) && GetClientRect(self.hwnd, &rcc)) {
                int ncW = (rcw.right - rcw.left) - (rcc.right - rcc.left);
                int ncH = (rcw.bottom - rcw.top) - (rcc.bottom - rcc.top);
                mmi->ptMinTrackSize.x = minCW + ncW;
                mmi->ptMinTrackSize.y = minCH + ncH;
            } else {
                mmi->ptMinTrackSize.x = minCW;
                mmi->ptMinTrackSize.y = minCH;
            }
        }
        return 0;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ui_timer_suspend(self, hwnd);
            self.in_size_move = false;
            KillTimer(hwnd, TIMER_LIVE_RESIZE);
            return 0;
        }

        if (self.ui_timer_suspended) ui_timer_resume(self, hwnd);

        layout(self, hwnd);
        post_model_dirty(self);
        return 0;

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lParam;

        if (nh && (nh->code == HDN_ENDTRACKW || nh->code == HDN_ENDDRAG)) {
            if (self.lv_svc && nh->hwndFrom == ListView_GetHeader(self.lv_svc)) {
                capture_columns_now(self, ItemKind::Svc);
                return 0;
            }
            if (self.lv_exe && nh->hwndFrom == ListView_GetHeader(self.lv_exe)) {
                capture_columns_now(self, ItemKind::Exe);
                return 0;
            }
        }
        if (nh && nh->code == NM_RCLICK) {
            HWND hdr_s = self.lv_svc ? ListView_GetHeader(self.lv_svc) : NULL;
            HWND hdr_e = self.lv_exe ? ListView_GetHeader(self.lv_exe) : NULL;
            int kind = -1;
            if (hdr_s && nh->hwndFrom == hdr_s) kind = KIND_SVC;
            if (hdr_e && nh->hwndFrom == hdr_e) kind = KIND_EXE;
            if (kind != -1) {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, IDM_RESET_COLUMNS, L"Reset columns");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                return 0;
            }
        }
        {
            LRESULT handled = 0;
            if (handle_main_listview_notify(self, hwnd, nh, lParam, handled)) return handled;
        }

        // Dark-mode custom draw
        if (nh && nh->code == NM_CUSTOMDRAW) {
            bool handled = false;
            LRESULT lr = theme_handle_customdraw(self, nh, lParam, handled);
            if (handled) return lr;
        }

        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, self.theme.br_bg);
        return 1;
    }
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (!mis) break;
        if (self.theme.dark_mode && (mis->CtlType == ODT_MENU || mis->CtlType == ODT_TAB)) {
            HDC hdc = GetDC(hwnd);
            HFONT oldf = NULL;
            if (hdc && self.ui_font) oldf = (HFONT)SelectObject(hdc, self.ui_font);
            SIZE sz{0,0};
            wchar_t txt[256]{0};

            if (mis->CtlType == ODT_MENU && mis->itemID) {
                HMENU mb = GetMenu(hwnd);
                if (mb) GetMenuStringW(mb, mis->itemID, txt, 255, MF_BYCOMMAND);
                if (txt[0] && hdc) GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &sz);
                mis->itemHeight = (UINT)std::max(18, dpi_scale(self.dpi, 22));
                mis->itemWidth  = (UINT)std::max(40, (int)sz.cx + dpi_scale(self.dpi, 24));
            } else if (mis->CtlType == ODT_TAB) {
                mis->itemHeight = (UINT)std::max(18, dpi_scale(self.dpi, 22));
            }

            if (hdc && oldf) SelectObject(hdc, oldf);
            if (hdc) ReleaseDC(hwnd, hdc);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        // Owner-drawn statusbar part (theme-aware text color — Win32 statusbar
        // ignores NM_CUSTOMDRAW for text, so we render it ourselves).
        if (dis && dis->CtlID == IDC_STATUSBAR) {
            RECT rc = dis->rcItem;
            HBRUSH bg = self.theme.dark_mode ? self.theme.br_panel : GetSysColorBrush(COLOR_BTNFACE);
            FillRect(dis->hDC, &rc, bg);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, self.theme.dark_mode ? self.theme.col_text : GetSysColor(COLOR_BTNTEXT));
            HFONT oldf = NULL;
            if (self.ui_font) oldf = (HFONT)SelectObject(dis->hDC, self.ui_font);
            RECT tr = rc;
            tr.left += 6;  // small left padding matches default statusbar layout
            DrawTextW(dis->hDC, self.statusbar_text.c_str(), (int)self.statusbar_text.size(), &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (oldf) SelectObject(dis->hDC, oldf);
            return TRUE;
        }
        // Detail panel separator (themed horizontal line)
        if (dis && dis->CtlID == IDC_DETAIL_SEP && dis->CtlType == ODT_STATIC) {
            // Use cached pen in dark mode; light mode uses a stock-ish approach
            HPEN pen = self.theme.dark_mode ? self.theme.pen_separator : (HPEN)GetStockObject(DC_PEN);
            HPEN old = (HPEN)SelectObject(dis->hDC, pen);
            if (!self.theme.dark_mode) SetDCPenColor(dis->hDC, GetSysColor(COLOR_3DSHADOW));
            int mid = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            MoveToEx(dis->hDC, dis->rcItem.left, mid, NULL);
            LineTo(dis->hDC, dis->rcItem.right, mid);
            SelectObject(dis->hDC, old);
            return TRUE;
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

        // Toolbar tab buttons
        if (id == IDC_TOOLBAR_TAB_SVC && code == BN_CLICKED) {
            sync_tabs_to(self, 0);
            request_save_debounced(self);
            return 0;
        }
        if (id == IDC_TOOLBAR_TAB_EXE && code == BN_CLICKED) {
            sync_tabs_to(self, 1);
            request_save_debounced(self);
            return 0;
        }
        if (id == IDC_TOOLBAR_SETTINGS && code == BN_CLICKED) {
            if (self.menu_settings) {
                UINT f = self.theme.dark_mode ? MF_CHECKED : MF_UNCHECKED;
                CheckMenuItem(self.menu_settings, IDM_DARKMODE, MF_BYCOMMAND | f);

                HWND tbSet = GetDlgItem(hwnd, IDC_TOOLBAR_SETTINGS);
                RECT r{};
                if (tbSet) GetWindowRect(tbSet, &r);
                else { GetCursorPos((LPPOINT)&r); r.bottom = r.top; }
                int cmd = TrackPopupMenuEx(self.menu_settings,
                    TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                    r.left, r.bottom, hwnd, NULL);
                if (cmd) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
            }
            return 0;
        }
        if (id == IDC_TOOLBAR_PROFILES && code == BN_CLICKED) {
            open_profiles_dialog(self, hwnd);
            return 0;
        }
        if (id == IDC_TOOLBAR_TRAY && code == BN_CLICKED) {
            if (!self.tray.enabled) {
                self.tray.enabled = true;
                tray_sync(self, GetModuleHandleW(NULL));
            }
            hide_main_window(self);
            return 0;
        }
        if (id == IDC_TOOLBAR_QUIT && code == BN_CLICKED) {
            app_request_exit(self, hwnd);
            return 0;
        }

        // Action strip buttons
        if (id == IDC_ACTION_ADD_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            add_item(self, kind);
            return 0;
        }
        if (id == IDC_ACTION_STOP_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            stop_selected(self, kind);
            return 0;
        }
        if (id == IDC_ACTION_START_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            start_selected(self, kind);
            return 0;
        }
        if (id == IDC_ACTION_REMOVE_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            remove_selected(self, kind);
            return 0;
        }

        // Detail panel buttons
        if (id == IDC_DETAIL_STOP_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            stop_selected(self, kind);
            return 0;
        }
        if (id == IDC_DETAIL_START_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            start_selected(self, kind);
            return 0;
        }
        if (id == IDC_DETAIL_REMOVE_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            remove_selected(self, kind);
            return 0;
        }
        if (id == IDC_DETAIL_COPY_BTN && code == BN_CLICKED) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            Item* it = lv_get_selected_item_ptr(self, kind);
            if (it) clipboard_set_text(hwnd, it->name.c_str());
            return 0;
        }

        // Activity collapse
        if (id == IDC_ACTIVITY_COLLAPSE && code == BN_CLICKED) {
            self.activity_collapsed = !self.activity_collapsed;
            layout(self, hwnd);
            request_save_debounced(self);
            return 0;
        }

        if ((id == IDC_SVC_SEARCH_EDIT || id == IDC_EXE_SEARCH_EDIT) && code == EN_CHANGE) {
            int kind = (id == IDC_SVC_SEARCH_EDIT) ? KIND_SVC : KIND_EXE;
            self.filter_pending[kind] = true;
            self.filter_debounce_last_ms = GetTickCount();
            SetTimer(hwnd, TIMER_SEARCH_DEBOUNCE, 120, NULL);
            return 0;
        }

        if (id == IDC_ACTION_BROWSE_BTN && code == BN_CLICKED) {
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                if (self.action_add_edit) SetWindowTextW(self.action_add_edit, file);
            }
            return 0;
        }

        if (id == IDM_RESET_COLUMNS) {
            ItemKind kind = (self.active_tab == 0) ? ItemKind::Svc : ItemKind::Exe;
            const int kind_i = ki(kind);
            self.cols_have[kind_i] = false;

            auto S = [&](int v96) -> int { return dpi_scale(self.dpi ? self.dpi : get_dpi_for_hwnd(hwnd), v96); };

            HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;
            if (lv) {
                HWND hdr = ListView_GetHeader(lv);
                int ncol = hdr ? Header_GetItemCount(hdr) : 0;
                ListView_SetColumnWidth(lv, 0, S(40));
                ListView_SetColumnWidth(lv, 1, S(260));
                ListView_SetColumnWidth(lv, 2, S(100));
                if (ncol == 4) {
                    // Services: Auto Stop, Service Name, Status, Last Update
                    ListView_SetColumnWidth(lv, 3, S(160));
                    int order[4] = {0,1,2,3};
                    ListView_SetColumnOrderArray(lv, 4, order);
                } else {
                    // EXEs: Auto Stop, Executable, Status, Path, Last Update
                    ListView_SetColumnWidth(lv, 3, S(200));
                    ListView_SetColumnWidth(lv, 4, S(160));
                    int order[5] = {0,1,2,3,4};
                    ListView_SetColumnOrderArray(lv, 5, order);
                }
                capture_columns_now(self, kind);
                lv_apply_name_fill(self, kind);
            }
            return 0;
        }

        if (id == IDM_PREFS) { open_prefs_dialog(self, hwnd); return 0; }
        if (id == IDM_PROFILES) { open_profiles_dialog(self, hwnd); return 0; }
        if (id == IDM_HOTKEYS) { open_hotkeys_dialog(self, hwnd); return 0; }
        if (id == IDM_DIAGNOSTICS) { open_diagnostics_dialog(self, hwnd); return 0; }
        if (id == IDM_FOCUS_SEARCH) {
            HWND edit = (self.active_tab == 0) ? self.svc_search : self.exe_search;
            if (edit) {
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        if (id == IDM_TAB_SVC) {
            sync_tabs_to(self, 0);
            return 0;
        }
        if (id == IDM_TAB_EXE) {
            sync_tabs_to(self, 1);
            return 0;
        }
        if (id == IDM_SAVE_CONFIG_NOW) {
            save_config_now(self);
            return 0;
        }
        if (id == IDM_FOCUS_MAIN_LIST) {
            HWND lv = (self.active_tab == 0) ? self.lv_svc : self.lv_exe;
            if (lv) SetFocus(lv);
            return 0;
        }
        if (id == IDM_REFRESH_UI) {
            self.ui_seen_status_gen[KIND_SVC] = 0;
            self.ui_seen_status_gen[KIND_EXE] = 0;
            rebuild_listview_filtered(self, ItemKind::Svc);
            rebuild_listview_filtered(self, ItemKind::Exe);
            PostMessageW(hwnd, WM_APP_MODEL_DIRTY, 0, 0);
            return 0;
        }

        if (id == IDM_TOGGLE_ACTIVITY) {
            self.activity_collapsed = !self.activity_collapsed;
            layout(self, hwnd);
            request_save_debounced(self);
            return 0;
        }
        if (id == IDM_HELP) { open_help_dialog(self, hwnd); return 0; }
        if (id == IDM_DARKMODE || id == IDM_TRAY_DARKMODE) {
            self.theme.dark_mode = !self.theme.dark_mode;
            darkmode_set_app(self.theme.dark_mode);
            theme_compute(self);
            theme_apply_all_controls(self);
            self.ui_seen_theme_gen = self.theme.gen.load(std::memory_order_relaxed);

            HMENU mb = GetMenu(hwnd);
            if (mb) {
                CheckMenuItem(mb, IDM_DARKMODE, MF_BYCOMMAND | (self.theme.dark_mode ? MF_CHECKED : MF_UNCHECKED));
                DrawMenuBar(hwnd);
            }
            request_save_debounced(self);
            return 0;
        }
        if (id == IDM_EXIT_TO_TRAY) {
            if (!self.tray.enabled) {
                self.tray.enabled = true;
                tray_sync(self, GetModuleHandleW(NULL));
            }
            hide_main_window(self);
            return 0;
        }
        if (id == IDM_EXIT) { app_request_exit(self, hwnd); return 0; }

        if (id == IDM_TRAY_SHOWHIDE) {
            show_main_window(self);
            return 0;
        }
        if (id == IDM_TRAY_PREFS) { open_prefs_dialog(self, hwnd); return 0; }
        if (id == IDM_TRAY_PROFILES) { open_profiles_dialog(self, hwnd); return 0; }
        if (id == IDM_TRAY_CLOSE_TO_TRAY) {
            self.tray.close_to_tray = !self.tray.close_to_tray;
            if (self.tray.close_to_tray) self.tray.enabled = true;
            request_save_debounced(self);
            return 0;
        }
        if (id == IDM_TRAY_EXIT) { app_request_exit(self, hwnd); return 0; }
        return 0;
    }

    case WM_APP_PROFILE_SWITCH: {
        int idx = (int)(INT_PTR)wParam;
        apply_profile_index(self, idx);
        return 0;
    }

    case WM_APP_RESTART_UI_TIMER:
        restart_ui_timer(self, hwnd);
        return 0;

    case WM_APP_LOG: {
        auto* m = (std::wstring*)lParam;
        if (m) {
            log_linef(self, L"%s", m->c_str());
            delete m;
        }
        return 0;
    }

    case WM_APP_STATUS_GEN: {
        int kind_i = (int)wParam;
        uint32_t gen = (uint32_t)(uintptr_t)lParam;
        if ((unsigned)kind_i < 2) {
            if (self.ui_seen_status_gen[kind_i] == gen) return 0;
            self.ui_seen_status_gen[kind_i] = gen;
        }
        ItemKind kind = static_cast<ItemKind>(kind_i);
        ui_apply_status_updates(self, kind);
        post_model_dirty(self);
        return 0;
    }

    case WM_APP_MODEL_DIRTY: {
        self.ui_dirty_posted = false;

        if (self.ui_timer_suspended || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            return 0;
        }

        ui_refresh_if_needed(self, hwnd);
        return 0;
    }

    case WM_APP_REQUEST_SAVE:
        if (!self.threads.save_pending) {
            self.threads.save_pending = true;
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        } else {
            SetTimer(hwnd, TIMER_DEBOUNCE_SAVE, 400, NULL);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_SEARCH_DEBOUNCE) {
            KillTimer(hwnd, TIMER_SEARCH_DEBOUNCE);
            if (self.filter_pending[KIND_SVC]) { self.filter_pending[KIND_SVC] = false; apply_filter_now(self, ItemKind::Svc); }
            if (self.filter_pending[KIND_EXE]) { self.filter_pending[KIND_EXE] = false; apply_filter_now(self, ItemKind::Exe); }
            return 0;
        }
        if (wParam == TIMER_DEBOUNCE_SAVE) {
            KillTimer(hwnd, TIMER_DEBOUNCE_SAVE);
            self.threads.save_pending = false;
            save_config_now(self);
            return 0;
        }
        if (wParam == TIMER_UI_REFRESH) {
            post_model_dirty(self);
            return 0;
        }
        if (wParam == TIMER_LIVE_RESIZE) {
            if (self.in_size_move) {
                layout(self, hwnd);
            }
            return 0;
        }
        return 0;

    case WM_CONTEXTMENU: {
        HWND target = (HWND)wParam;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (target == self.lv_svc || target == self.lv_exe) {
            ItemKind kind = (target == self.lv_svc) ? ItemKind::Svc : ItemKind::Exe;
            HWND lv = (kind == ItemKind::Svc) ? self.lv_svc : self.lv_exe;

            if (pt.x == -1 && pt.y == -1) {
                int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
                if (sel < 0) sel = ListView_GetSelectionMark(lv);
                RECT ir{};
                if (sel >= 0 && ListView_GetItemRect(lv, sel, &ir, LVIR_BOUNDS)) {
                    pt.x = ir.left + 20;
                    pt.y = ir.top + 20;
                    ClientToScreen(lv, &pt);
                } else {
                    GetCursorPos(&pt);
                }
            } else {
                POINT cpt = pt;
                ScreenToClient(lv, &cpt);
                LVHITTESTINFO ht{};
                ht.pt = cpt;
                int hit = ListView_HitTest(lv, &ht);
                if (hit >= 0) {
                    ListView_SetItemState(lv, hit, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(lv, hit, FALSE);
                }
            }

            HMENU m = CreatePopupMenu();
            if (!m) return 0;

            AppendMenuW(m, MF_STRING, IDM_CTX_STOP, L"Stop");
            AppendMenuW(m, MF_STRING, IDM_CTX_REMOVE, L"Remove");
            AppendMenuW(m, MF_STRING, IDM_CTX_TOGGLE_AUTO, L"Toggle auto-stop");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, IDM_CTX_COPY_NAME, L"Copy name");
            if (kind == ItemKind::Exe) AppendMenuW(m, MF_STRING, IDM_CTX_OPEN_LOC, L"Open file location");

            SetForegroundWindow(hwnd);
            UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);

            switch (cmd) {
            case IDM_CTX_STOP:        stop_selected(self, kind); break;
            case IDM_CTX_REMOVE:      remove_selected(self, kind); break;
            case IDM_CTX_TOGGLE_AUTO: toggle_autostop_selected(self, kind); break;
            case IDM_CTX_COPY_NAME: {
                Item* it = lv_get_selected_item_ptr(self, kind);
                if (it) clipboard_set_text(hwnd, it->name.c_str());
            } break;
            case IDM_CTX_OPEN_LOC: {
                Item* it = lv_get_selected_item_ptr(self, kind);
                if (it) open_file_location_best_effort(self, it->name.c_str());
            } break;
            default: break;
            }
            return 0;
        }

        // Activity log context menu
        if (target == self.activity) {
            if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);

            int sel = (int)SendMessageW(self.activity, LB_GETCURSEL, 0, 0);

            HMENU m = CreatePopupMenu();
            if (!m) return 0;

            AppendMenuW(m, MF_STRING | (sel >= 0 ? 0 : MF_GRAYED), IDM_LOG_COPY_LINE, L"Copy line");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            int count = (int)SendMessageW(self.activity, LB_GETCOUNT, 0, 0);
            AppendMenuW(m, MF_STRING | (count > 0 ? 0 : MF_GRAYED), IDM_LOG_CLEAR_ALL, L"Clear all");

            SetForegroundWindow(hwnd);
            UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);

            if (cmd == IDM_LOG_COPY_LINE && sel >= 0) {
                int len = (int)SendMessageW(self.activity, LB_GETTEXTLEN, (WPARAM)sel, 0);
                if (len > 0) {
                    std::wstring txt((size_t)len + 1, L'\0');
                    SendMessageW(self.activity, LB_GETTEXT, (WPARAM)sel, (LPARAM)txt.data());
                    txt.resize((size_t)len);
                    clipboard_set_text(hwnd, txt.c_str());
                }
            } else if (cmd == IDM_LOG_CLEAR_ALL) {
                SendMessageW(self.activity, LB_RESETCONTENT, 0, 0);
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_APP_TRAYICON: {
        const UINT ev = (UINT)LOWORD(lParam);
        switch (ev) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case NIN_BALLOONUSERCLICK:
            show_main_window(self);
            return 0;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_show_menu(hwnd);
            return 0;

        default:
            return 0;
        }
    }
    case WM_CLOSE:
        if (!self.tray.force_quit && self.tray.close_to_tray && self.tray.enabled) {
            hide_main_window(self);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        tray_remove(self);

        if (self.il_svc) { ImageList_Destroy(self.il_svc); self.il_svc = NULL; }
        if (self.il_exe) { ImageList_Destroy(self.il_exe); self.il_exe = NULL; }

        if (self.threads.monitor_thread.joinable()) self.threads.monitor_thread.request_stop();
        for (auto& t : self.threads.action_threads)
            if (t.joinable()) t.request_stop();
        self.threads.action_cv.notify_all();

        if (self.threads.monitor_thread.joinable()) self.threads.monitor_thread.join();
        for (auto& t : self.threads.action_threads)
            if (t.joinable()) t.join();
        action_clear(self);

        save_config_now(self);

        {
            MSG drain_msg;
            while (PeekMessageW(&drain_msg, self.hwnd, WM_APP_LOG, WM_APP_LOG, PM_REMOVE)) {
                auto* m = (std::wstring*)drain_msg.lParam;
                delete m;
            }
        }

        action_clear(self);

        self.disp_cache.m.clear();
        {
            ModelLockGuard lk(self);
            unregister_all_items_locked(self, ItemKind::Svc);
            unregister_all_items_locked(self, ItemKind::Exe);
        }

        for (auto& kv : self.ui_font_cache) { if (kv.second) DeleteObject(kv.second); }
        self.ui_font_cache.clear();

        if (self.ui_font && self.ui_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(self.ui_font);
            self.ui_font = NULL;
        }

        theme_release_resources(self);

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// Entry point
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    App app_instance{};

    App& self = app_instance;

    // Harden DLL search: force System32-only resolution for any subsequent
    // LoadLibrary* calls. Defeats current-directory / PATH planting attacks.
    // Must run before dll_cache_init() / InitCommonControlsEx().
    {
        typedef BOOL (WINAPI *PFN_SetDefaultDllDirectories)(DWORD);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            // Cast via uintptr_t to silence -Wcast-function-type on GCC/Clang.
            FARPROC fp = GetProcAddress(k32, "SetDefaultDllDirectories");
            auto pSetDefaultDllDirectories = (PFN_SetDefaultDllDirectories)(uintptr_t)fp;
            if (pSetDefaultDllDirectories) {
                pSetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        SetDllDirectoryW(L"");  // remove current dir from search path
    }

    enable_dpi_awareness();
    dll_cache_init();  // resolve DLL function pointers once at startup
    ensure_admin_or_exit();

    compute_default_cfg_path(self.cfg.cfg_path, MAX_PATH);

    list_init(&self.items[KIND_SVC]);
    list_init(&self.items[KIND_EXE]);

    self.items_gen[KIND_SVC] = 1;
    self.items_gen[KIND_EXE] = 1;
    self.have_win_rect = false;
    self.win_x = CW_USEDEFAULT;
    self.win_y = CW_USEDEFAULT;
    self.win_w = 1100;
    self.win_h = 600;
    self.win_maximized = false;

    self.prof.watch_keys_sp = std::make_shared<std::vector<std::vector<std::wstring>>>();
    self.prof.watch_hashes_sp = std::make_shared<std::vector<std::vector<uint64_t>>>();

    self.tray.enabled = true;

    self.did_initial_layout = false;
    self.initial_layout_scheduled = false;
    self.initial_layout_tries = 0;

    load_config(self);
    darkmode_set_app(self.theme.dark_mode);

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);

    self.last_any_status_wall = 0;

    self.ui_font = NULL;
    self.app_title = exe_stem_title();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
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

    int start_x = CW_USEDEFAULT;
    int start_y = CW_USEDEFAULT;
    int start_w = 1100;
    int start_h = 600;
    if (self.have_win_rect) {
        start_x = self.win_x;
        start_y = self.win_y;
        start_w = self.win_w;
        start_h = self.win_h;
    }

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, self.app_title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        start_x, start_y, start_w, start_h,
        NULL, NULL, hInst, &app_instance
    );
    if (!hwnd) return 0;

    ACCEL accels[8] = {};
    accels[0].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[0].key   = (WORD)'F';
    accels[0].cmd   = IDM_FOCUS_SEARCH;

    accels[1].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[1].key   = (WORD)'1';
    accels[1].cmd   = IDM_TAB_SVC;

    accels[2].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[2].key   = (WORD)'2';
    accels[2].cmd   = IDM_TAB_EXE;

    accels[3].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[3].key   = (WORD)'S';
    accels[3].cmd   = IDM_SAVE_CONFIG_NOW;

    accels[4].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[4].key   = (WORD)'L';
    accels[4].cmd   = IDM_FOCUS_MAIN_LIST;

    accels[5].fVirt = (BYTE)FVIRTKEY;
    accels[5].key   = (WORD)VK_F5;
    accels[5].cmd   = IDM_REFRESH_UI;

    accels[6].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[6].key   = (WORD)'D';
    accels[6].cmd   = IDM_DARKMODE;

    accels[7].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    accels[7].key   = (WORD)'G';
    accels[7].cmd   = IDM_TOGGLE_ACTIVITY;

    app_instance.accel = CreateAcceleratorTableW(accels, 8);

    int nshow = show;
    if (self.have_win_rect && self.win_maximized) nshow = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, nshow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (app_instance.accel && TranslateAcceleratorW(hwnd, app_instance.accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (app_instance.accel) DestroyAcceleratorTable(app_instance.accel);
    return 0;
}
