#include "app.h"

void snapshot_from_runtime_locked(App& self, ConfigSnapshot& out) {
    out.ui_refresh_ms = self.cfg.ui_refresh_ms;
    out.monitor_interval_ms = self.cfg.monitor_interval_ms;
    out.autostop_cooldown_ms = self.cfg.autostop_cooldown_ms;
    out.stop_wait_ms = self.cfg.stop_wait_ms;
    out.tray_enabled = self.tray.enabled;
    out.close_to_tray = self.tray.close_to_tray;

    cfg_clear_items(out);

    for (int k = 0; k < 2; k++) {
        out.items[k].reserve(self.items[k].v.size());
        for (auto& up : self.items[k].v) {
            Item* it = up.get();
            if (!it || it->name.empty()) continue;
            ItemRow r;
            r.name = it->name;
            r.auto_stop = it->auto_stop;
            if (k == KIND_EXE && !it->exe_path.empty()) r.exe_path = it->exe_path;
            out.items[k].push_back(std::move(r));
        }
    }
}

void snapshot_active_from_runtime_locked(App& self) {
    if (self.prof.active >= 0 && self.prof.active < (int)self.prof.profiles.size()) {
        snapshot_from_runtime_locked(self, self.prof.profiles[(size_t)self.prof.active].cfg);
    } else {
        snapshot_from_runtime_locked(self, self.prof.default_cfg);
        self.prof.have_default_cfg = true;
    }
}

void rebuild_runtime_items_locked(App& self, const ConfigSnapshot& cfg) {
    // Clear existing items via centralized unregistration (keeps uid_map/by_name/dirty/view caches consistent).
    unregister_all_items_locked(self, ItemKind::Svc);
    unregister_all_items_locked(self, ItemKind::Exe);

for (int k = 0; k < 2; k++) {
        const ItemKind ik = (k == KIND_EXE) ? ItemKind::Exe : ItemKind::Svc;
        for (const auto& row : cfg.items[k]) {
            if (row.name.empty()) continue;

            Item* it = new (std::nothrow) Item();
            if (!it) continue;

            it->kind = ik;
            it->name = row.name;
            if (k == KIND_EXE && !row.exe_path.empty()) it->exe_path = row.exe_path;
            it->auto_stop = row.auto_stop;
            it->img = (k == KIND_SVC) ? 0 : 1;
            it->autostop_count = 0;
            it->last_autostop_mono_ms = 0;
            it->last_update_wall = time(NULL);

            if (k == KIND_SVC) {
                wchar_t st[32];
                if (query_service_status_fast(it->name.c_str(), st, 32)) set_item_status(*it, st);
                else set_item_status(*it, L"not found");
            } else {
                int c = process_count_by_name_lower(it->name.c_str());
                set_item_status(*it, (c > 0) ? L"running" : L"stopped");
            }

            // Centralized registration keeps uid_map/by_name/dirty/view caches consistent.
            if (register_item_locked(self, ik, it)) {
                // items_gen[k] is bumped by caller when appropriate.
            } else {
                delete it;
                it = nullptr;
            }
        }
    }

    self.items_gen[KIND_SVC]++;
    self.items_gen[KIND_EXE]++;
}

const wchar_t* active_profile_name_locked(const App& self) {
    if (self.prof.active >= 0 && self.prof.active < (int)self.prof.profiles.size()) {
        const auto& p = self.prof.profiles[(size_t)self.prof.active].name;
        return p.empty() ? L"(unnamed)" : p.c_str();
    }
    return L"Default";
}

// UI thread: apply config snapshot to runtime + listviews
void apply_profile_index(App& self, int idx) {
    // idx: -1 = default, 0..N-1 = profiles
    std::vector<Item*> svc_ptrs;
    std::vector<Item*> exe_ptrs;
    std::wstring pname;

    // Processes started by the *previous* active profile (close them when leaving)
    std::vector<StartedProc> to_stop_started;

    // Shell-open targets to launch for the *new* profile activation
    std::vector<std::wstring> to_launch;

    {
        ModelLockGuard lk(self);

        // Persist runtime edits into the currently active snapshot before switching.
        snapshot_active_from_runtime_locked(self);

        // Leaving any profile (or default): stop anything we previously started due to profile activation.
        if (!self.prof.started_procs.empty()) {
            to_stop_started.swap(self.prof.started_procs);
        }
        self.prof.started_exes.clear();

        self.prof.active = idx;
        pname = active_profile_name_locked(self);

        const ConfigSnapshot* cfg = &self.prof.default_cfg;
        if (idx >= 0 && idx < (int)self.prof.profiles.size()) cfg = &self.prof.profiles[(size_t)idx].cfg;

        // Gather items to start for the new profile.
        if (idx >= 0 && idx < (int)self.prof.profiles.size()) {
            for (const auto& r : self.prof.profiles[(size_t)idx].start_items) {
                if (r.target.empty()) continue;
                to_launch.push_back(r.target);
            }
        }

        // Apply settings
        self.cfg.ui_refresh_ms = cfg->ui_refresh_ms;
        self.cfg.monitor_interval_ms = cfg->monitor_interval_ms;
        self.cfg.autostop_cooldown_ms = cfg->autostop_cooldown_ms;
        self.cfg.stop_wait_ms = cfg->stop_wait_ms;
        self.tray.enabled = cfg->tray_enabled;
        self.tray.close_to_tray = cfg->tray_enabled && cfg->close_to_tray;

        rebuild_runtime_items_locked(self, *cfg);

        // Snapshot Item* for list view updates outside the lock
        svc_ptrs.reserve(self.items[KIND_SVC].v.size());
        for (auto& up : self.items[KIND_SVC].v) if (up) svc_ptrs.push_back(up.get());

        exe_ptrs.reserve(self.items[KIND_EXE].v.size());
        for (auto& up : self.items[KIND_EXE].v) if (up) exe_ptrs.push_back(up.get());
    }
    {
        std::lock_guard<std::shared_mutex> lk2(self.mtx);
        self.threads.mon_wake = true;
    }
    self.threads.mon_cv.notify_one();  // wake monitor — item list changed

    // Close anything started by the previously active profile.
    // Do this outside the lock so we don't block UI / monitor thread.
    for (auto& sp : to_stop_started) {
        // Preferred: close/terminate the *exact* PID we started (when we have it).
        if (sp.pid || sp.hproc) {
            Action a;
            a.kind = ItemKind::Exe;
            a.op = ACTION_CLOSE_STARTED;
            a.name = sp.started_target;
            a.reason = L"profile deactivated (close started item)";
            a.wait_ms = self.cfg.stop_wait_ms;
            a.pid = sp.pid;
            if (sp.hproc) {
                HANDLE dup = NULL;
                if (DuplicateHandle(GetCurrentProcess(), sp.hproc.get(), GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    a.hproc.reset(dup);
            }
            action_enqueue(self, std::move(a));
            continue;
        }

        // Fallback: if ShellExecuteEx didn't give us a process handle (common for some launches),
        // and the item was an .exe, stop by exe basename (may affect existing instances).
        if (!sp.exe_lower.empty()) {
            Action a;
            a.kind = ItemKind::Exe;
            a.op = ACTION_STOP;
            a.name = sp.exe_lower;
            a.reason = L"profile deactivated (stop started exe)";
            a.wait_ms = self.cfg.stop_wait_ms;
            action_enqueue(self, std::move(a));
        }
    }

    // Start items for the newly activated profile.
    if (idx >= 0) {
        for (const auto& t : to_launch) {
            Action a;
            a.kind = ItemKind::Exe;
            a.op = ACTION_LAUNCH_ITEM;
            a.name = t;
            a.target = t;
            a.reason = L"profile activated (launch item)";
            a.wait_ms = 0;
            action_enqueue(self, std::move(a));
        }
    }

    // Update tray immediately if needed
    tray_sync(self, GetModuleHandleW(NULL));

    // Refresh listviews (debounced to reduce churn during rapid changes)
    self.filter_pending[KIND_SVC] = true;
    self.filter_pending[KIND_EXE] = true;
    if (self.hwnd) SetTimer(self.hwnd, TIMER_SEARCH_DEBOUNCE, 120, NULL);
    self.lv_needs_layout[KIND_SVC] = true;
    self.lv_needs_layout[KIND_EXE] = true;
    update_statusbar(self);

    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
    request_save_debounced(self);

    if (idx >= 0) {
        log_linef(self, L"Profile activated: %s", pname.c_str());
    } else {
        log_linef(self, L"Profile deactivated: back to Default");
    }
}


// ----------------------------
// Action worker queue
// ----------------------------
void action_enqueue(App& self, ItemKind kind, int op, const wchar_t* name, const wchar_t* reason, int wait_ms) {
    if (!name || !name[0]) return;

    Action a;
    a.kind = kind;
    a.op = op;
    a.name = name;
    a.reason = reason ? reason : L"";
    a.wait_ms = wait_ms;

    // If this is an AUTO-STOP service stop and we've recently seen repeated stop errors,
    // skip enqueueing to prevent infinite retry loops (suppression).
    if (kind == ItemKind::Svc && op == ACTION_STOP && reason && wcsstr(reason, L"AUTO-STOP") != nullptr) {
        uint64_t now_ms = now_mono_ms();
        bool suppressed = false;
        DWORD last_err = 0;
        uint64_t until_ms = 0;
        {
            ModelReadGuard lk(self);
            Item* it = list_find(&self.items[KIND_SVC], name);
            if (it && it->svc_stop_suppress_until_ms && now_ms < it->svc_stop_suppress_until_ms) {
                suppressed = true;
                last_err = it->svc_last_stop_err;
                until_ms = it->svc_stop_suppress_until_ms;
            }
        }
        if (suppressed) {
            wchar_t msg[400];
            if (last_err) {
                StringCchPrintfW(msg, 400, L"%s: AUTO-STOP suppressed (e%lu) — not enqueueing stop.", name, (unsigned long)last_err);
            } else {
                StringCchPrintfW(msg, 400, L"%s: AUTO-STOP suppressed — not enqueueing stop.", name);
            }
            post_log(self, msg);
            (void)until_ms;
            return;
        }
    }

    action_enqueue(self, std::move(a));
}
