// monitor.cpp — Monitor thread, process/service query functions, info pane helpers
#include "app.h"

// ==================================================
// Info pane helpers
// ==================================================

std::wstring query_service_description_best_effort(const std::wstring& svc_name, DWORD& out_err) {
    out_err = 0;
    std::wstring out;

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { out_err = GetLastError(); return out; }

    SC_HANDLE svc = OpenServiceW(scm, svc_name.c_str(), SERVICE_QUERY_CONFIG);
    if (!svc) { out_err = GetLastError(); CloseServiceHandle(scm); return out; }

    DWORD need = 0;
    QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, NULL, 0, &need);
    DWORD err = GetLastError();
    if (need == 0) {
        out_err = (err ? err : ERROR_INVALID_DATA);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return out;
    }

    std::vector<BYTE> buf(need);
    if (!QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, buf.data(), (DWORD)buf.size(), &need)) {
        out_err = GetLastError();
    } else {
        SERVICE_DESCRIPTIONW* sd = (SERVICE_DESCRIPTIONW*)buf.data();
        if (sd && sd->lpDescription) out = sd->lpDescription;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return out;
}

void update_detail_panel(App& self, ItemKind kind, Item* it) {
    // Clear all detail controls if no item
    if (!it) {
        if (self.detail_name) SetWindowTextW(self.detail_name, L"");
        if (self.detail_status_lbl) SetWindowTextW(self.detail_status_lbl, L"Status:");
        if (self.detail_autostop) SetWindowTextW(self.detail_autostop, L"Auto-stop:");
        if (self.detail_desc) SetWindowTextW(self.detail_desc, L"");
        return;
    }

    // Name
    if (self.detail_name) SetWindowTextW(self.detail_name, it->name.c_str());

    // Status
    if (self.detail_status_lbl) {
        wchar_t buf[128];
        StringCchPrintfW(buf, 128, L"Status: %s", it->last_status[0] ? it->last_status : L"unknown");
        SetWindowTextW(self.detail_status_lbl, buf);
    }

    // Auto-stop
    if (self.detail_autostop) {
        wchar_t buf[128];
        if (it->auto_stop) {
            StringCchPrintfW(buf, 128, L"Auto-stop: Enabled (%d times)", it->autostop_count);
        } else {
            StringCchCopyW(buf, 128, L"Auto-stop: Disabled");
        }
        SetWindowTextW(self.detail_autostop, buf);
    }

    // Description / Info
    if (self.detail_desc) {
        if (kind == ItemKind::Svc) {
            // Lazy-load service description
            if (!it->svc_desc_loaded) {
                DWORD err = 0;
                std::wstring desc = query_service_description_best_effort(it->name, err);
                it->svc_desc = desc;
                it->svc_desc_last_err = err;
                it->svc_desc_loaded = true;
            }

            if (!it->svc_desc.empty()) {
                SetWindowTextW(self.detail_desc, it->svc_desc.c_str());
            } else if (it->svc_desc_last_err) {
                wchar_t buf[256];
                StringCchPrintfW(buf, 256, L"(No description. Query failed: winerror %lu)", (unsigned long)it->svc_desc_last_err);
                SetWindowTextW(self.detail_desc, buf);
            } else {
                SetWindowTextW(self.detail_desc, L"(No description)");
            }
        } else {
            std::wstring info;
            if (!it->exe_path.empty()) {
                info = L"Path:\r\n" + it->exe_path;
            } else {
                info = L"Name: " + it->name;
            }
            SetWindowTextW(self.detail_desc, info.c_str());
        }
    }

    // Show/hide Stop vs Start buttons based on status
    if (self.detail_stop_btn && self.detail_start_btn) {
        bool is_running = (_wcsicmp(it->last_status, L"running") == 0);
        ShowWindow(self.detail_stop_btn, is_running ? SW_SHOW : SW_HIDE);
        ShowWindow(self.detail_start_btn, is_running ? SW_HIDE : SW_SHOW);
    }
}


// ==================================================
// Monitor-thread support helpers
// ==================================================

// Forward declaration
struct MonScratch;
static bool batch_query_service_states_direct(SC_HANDLE scm_in, MonScratch& sc,
                                            const std::vector<std::wstring>& svc_names, size_t ns, uint32_t gen_s,
                                            std::vector<StatusStr>& out_status);

// Forward declarations for functions defined later in this file.
static bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes,
                                           const NameIdx* idx_sorted,
                                           size_t idx_n,
                                           int* out_counts,
                                           size_t n_total,
                                           bool collect_hashes = true);


static bool should_skip_autostop_for_service_status(const wchar_t* status) {
    if (!status) return true;
    return (_wcsicmp(status, L"unknown") == 0) ||
           (_wcsicmp(status, L"not found") == 0) ||
           (_wcsicmp(status, L"stopping") == 0) ||
           (_wcsicmp(status, L"starting") == 0) ||
           (_wcsicmp(status, L"resuming") == 0) ||
           (_wcsicmp(status, L"pausing") == 0);
}


struct MonSnap {
    size_t ns = 0, ne = 0;
    uint32_t gen_s = 0, gen_e = 0;

    std::vector<std::wstring> svc_names;
    std::vector<std::wstring> exe_names;

    std::vector<uint8_t> svc_auto;    // uint8_t avoids std::vector<bool> specialization
    std::vector<uint8_t> exe_auto;

    std::vector<uint64_t> svc_lastreq;
    std::vector<uint64_t> exe_lastreq;

    // Stable per-item uids captured at snapshot time. Let the status-apply path
    // resolve Item* by integer uid_map lookup instead of re-hashing the name
    // (FNV-1a + case-insensitive compare) for every item on every tick.
    std::vector<uint32_t> svc_uids;
    std::vector<uint32_t> exe_uids;

    std::vector<NameIdx> svc_idx_sorted;
    std::vector<NameIdx> exe_idx_sorted;

    // Default destructor handles cleanup — no manual free needed.
    MonSnap() = default;
    MonSnap(const MonSnap&) = delete;
    MonSnap& operator=(const MonSnap&) = delete;
};

struct MonScratch {
    std::vector<StatusStr> svc_status;
    std::vector<StatusStr> exe_status;
    std::vector<int> exe_counts;

    std::vector<BYTE> svc_enum_buf;

    std::vector<unique_sc_handle> svc_query_handles;
    uint32_t svc_handles_gen = 0;
};

// Definition (placed after MonScratch so the type is complete).
static bool batch_query_service_states_direct(SC_HANDLE scm_in, MonScratch& sc,
                                            const std::vector<std::wstring>& svc_names, size_t ns, uint32_t gen_s,
                                            std::vector<StatusStr>& out_status) {
    if (ns == 0) return true;
    for (size_t i = 0; i < ns; i++) StringCchCopyW(out_status[i].buf, 32, L"unknown");

    unique_sc_handle scm_local;
    SC_HANDLE scm = scm_in;
    if (!scm) {
        scm_local.reset(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
        scm = scm_local.get();
    }
    if (!scm) return false;

    // Rebuild cached handles when the monitored list changes.
    if (sc.svc_handles_gen != gen_s || sc.svc_query_handles.size() != ns) {
        sc.svc_query_handles.clear();
        sc.svc_query_handles.resize(ns);
        sc.svc_handles_gen = gen_s;
    }

    for (size_t i = 0; i < ns; i++) {
        if (i >= svc_names.size() || svc_names[i].empty()) continue;
        const wchar_t* name = svc_names[i].c_str();

        if (!sc.svc_query_handles[i]) {
            sc.svc_query_handles[i].reset(OpenServiceW(scm, name, SERVICE_QUERY_STATUS));
        }

        SERVICE_STATUS_PROCESS ssp{};
        DWORD bytes = 0;
        BOOL ok = FALSE;
        if (sc.svc_query_handles[i]) {
            ok = QueryServiceStatusEx(sc.svc_query_handles[i].get(), SC_STATUS_PROCESS_INFO,
                                     (LPBYTE)&ssp, sizeof(ssp), &bytes);
        }
        if (!ok) {
            sc.svc_query_handles[i].reset();
            sc.svc_query_handles[i].reset(OpenServiceW(scm, name, SERVICE_QUERY_STATUS));
            if (sc.svc_query_handles[i]) {
                ok = QueryServiceStatusEx(sc.svc_query_handles[i].get(), SC_STATUS_PROCESS_INFO,
                                         (LPBYTE)&ssp, sizeof(ssp), &bytes);
            }
        }

        if (ok) {
            StringCchCopyW(out_status[i].buf, 32, svc_state_to_str(ssp.dwCurrentState));
        }
    }

    return true;
}


// Resize MonSnap/MonScratch vectors to accommodate ns services and ne executables.
static void mons_ensure_sizes(MonSnap& snap, MonScratch& sc, size_t ns, size_t ne) {
    snap.svc_names.resize(ns);
    snap.exe_names.resize(ne);
    snap.svc_auto.resize(ns, 0);
    snap.exe_auto.resize(ne, 0);
    snap.svc_lastreq.resize(ns, 0);
    snap.exe_lastreq.resize(ne, 0);
    snap.svc_uids.resize(ns, 0);
    snap.exe_uids.resize(ne, 0);
    sc.svc_status.resize(ns);
    sc.exe_status.resize(ne);
    sc.exe_counts.resize(ne, 0);
}

// Wait for up to total_ms, but wake immediately if stop is requested or mon_wake is set.
// Returns true if stop was requested (caller should exit the loop).
static bool sleep_ms_cooperative(std::stop_token st, App& self, DWORD total_ms) {
    std::unique_lock<std::shared_mutex> lk(self.mtx);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
    self.threads.mon_cv.wait_until(lk, deadline, [&]{
        return st.stop_requested() || self.threads.mon_wake;
    });
    self.threads.mon_wake = false;  // consumed
    return st.stop_requested();
}

void monitor_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    MonSnap snap;
    MonScratch sc;

    // Reused per-tick: running process exe hashes (lowercased basename)
    std::vector<uint64_t> running_hashes;
    running_hashes.reserve(512);


    // Reuse SCM handle across ticks (falls back to per-call open if needed).
    unique_sc_handle scm_connect(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));

    for (;;) {
        if (st.stop_requested()) break;


        int monitor_interval_ms = 1000;
        int cooldown_ms = 0;
        int stop_wait_ms = 0;

        size_t ns = 0, ne = 0;
        uint32_t gen_s = 0, gen_e = 0;

        // Snapshot pointers + cheap fields under shared (read) lock.
        {
            ModelReadGuard lk(self);

            monitor_interval_ms = self.cfg.monitor_interval_ms;
            cooldown_ms = self.cfg.autostop_cooldown_ms;
            stop_wait_ms = self.cfg.stop_wait_ms;

            ns = self.items[KIND_SVC].v.size();
            ne = self.items[KIND_EXE].v.size();
            gen_s = self.items_gen[KIND_SVC];
            gen_e = self.items_gen[KIND_EXE];

            mons_ensure_sizes(snap, sc, ns, ne);

            // Refresh name copies ONLY if list changed (add/remove), but always refresh auto/lastreq.
            if (gen_s != snap.gen_s || ns != snap.ns) {
                for (size_t i = 0; i < ns; i++) {
                    Item* it = self.items[KIND_SVC].v[i].get();
                    snap.svc_names[i] = (it && !it->name.empty()) ? it->name : std::wstring{};
                }

                snap.svc_idx_sorted.clear();
                snap.svc_idx_sorted.reserve(ns);
                for (size_t i = 0; i < ns; i++) {
                    if (!snap.svc_names[i].empty()) snap.svc_idx_sorted.push_back(NameIdx{ snap.svc_names[i].c_str(), i });
                }
                std::sort(snap.svc_idx_sorted.begin(), snap.svc_idx_sorted.end(),
                          [](const NameIdx& a, const NameIdx& b) { return _wcsicmp(a.name, b.name) < 0; });

                snap.ns = ns;
                snap.gen_s = gen_s;
            } else {
                snap.ns = ns;
            }

            if (gen_e != snap.gen_e || ne != snap.ne) {
                for (size_t i = 0; i < ne; i++) {
                    Item* it = self.items[KIND_EXE].v[i].get();
                    snap.exe_names[i] = (it && !it->name.empty()) ? it->name : std::wstring{};
                }

                snap.exe_idx_sorted.clear();
                snap.exe_idx_sorted.reserve(ne);
                for (size_t i = 0; i < ne; i++) {
                    if (!snap.exe_names[i].empty()) snap.exe_idx_sorted.push_back(NameIdx{ snap.exe_names[i].c_str(), i });
                }
                std::sort(snap.exe_idx_sorted.begin(), snap.exe_idx_sorted.end(),
                          [](const NameIdx& a, const NameIdx& b) { return _wcsicmp(a.name, b.name) < 0; });

                snap.ne = ne;
                snap.gen_e = gen_e;
            } else {
                snap.ne = ne;
            }

            for (size_t i = 0; i < ns; i++) {
                Item* it = self.items[KIND_SVC].v[i].get();
                snap.svc_auto[i] = it ? (it->auto_stop ? 1 : 0) : 0;
                snap.svc_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                snap.svc_uids[i] = it ? it->uid : 0;
            }
            for (size_t i = 0; i < ne; i++) {
                Item* it = self.items[KIND_EXE].v[i].get();
                snap.exe_auto[i] = it ? (it->auto_stop ? 1 : 0) : 0;
                snap.exe_lastreq[i] = it ? it->last_autostop_mono_ms : 0;
                snap.exe_uids[i] = it ? it->uid : 0;
            }
        }

        time_t wall_now = time(NULL);
        uint64_t mono_now = now_mono_ms();
        uint64_t cd_ms = (uint64_t)cooldown_ms;

        // --- Single Toolhelp snapshot: build profile hashes AND count EXE processes ---
        // This replaces two separate CreateToolhelp32Snapshot calls with one.
        //
        // The process-table snapshot is the most expensive thing this thread does,
        // so we only take it when something actually consumes its output:
        //   - exe counts are needed when there are monitored EXE rows (ne > 0)
        //   - the running-hash set is needed only for profile auto-activation,
        //     i.e. when at least one profile defines watch keys.
        // A services-only / no-profile config can then idle with no per-tick snapshot.
        int cur_profile = -1;
        std::shared_ptr<const std::vector<std::vector<uint64_t>>> watches_sp;
        {
            ModelReadGuard lk(self);
            cur_profile = self.prof.active;
            watches_sp = self.prof.watch_hashes_sp;
        }
        const bool need_hashes = (watches_sp && !watches_sp->empty());

        if (ne) {
            std::fill(sc.exe_counts.begin(), sc.exe_counts.end(), 0);
            build_running_exe_hashes_lower(running_hashes,
                                           snap.exe_idx_sorted.data(), snap.exe_idx_sorted.size(),
                                           sc.exe_counts.data(), ne, need_hashes);
        } else {
            build_running_exe_hashes_lower(running_hashes, nullptr, 0, nullptr, 0, need_hashes);
        }

        // Profile auto-activation based on watched .exe processes.
        // If multiple profiles match, the first match (config order) wins.
        {
            int cur = cur_profile;

            int desired = -1;
            if (watches_sp) {
                const auto& watches = *watches_sp;
                for (size_t i = 0; i < watches.size(); i++) {
                    const auto& ws = watches[i];
                    for (auto h : ws) {
                        if (std::binary_search(running_hashes.begin(), running_hashes.end(), h)) { desired = (int)i; break; }
                    }
                    if (desired >= 0) break;
                }
            }

            if (desired != cur) {
                PostMessageW(self.hwnd, WM_APP_PROFILE_SWITCH, (WPARAM)desired, 0);
            }
        }

        // --- Services: batch query via EnumServicesStatusEx + per-item fallback ---
        if (ns) {
            for (size_t i = 0; i < ns; i++) StringCchCopyW(sc.svc_status[i].buf, 32, L"unknown");

            bool ok = batch_query_service_states_direct(scm_connect.get(), sc, snap.svc_names, ns, gen_s, sc.svc_status);
            if (!ok) post_log(self, L"Monitor: batch service enum failed");

            std::vector<size_t> svc_enforce_idx;
            svc_enforce_idx.reserve(ns);

            for (size_t i = 0; i < ns; i++) {
                if (snap.svc_names[i].empty()) continue;

                if (_wcsicmp(sc.svc_status[i].buf, L"unknown") == 0) {
                    wchar_t stbuf[32];
                    if (query_service_status_fast(snap.svc_names[i].c_str(), stbuf, 32)) StringCchCopyW(sc.svc_status[i].buf, 32, stbuf);
                    else StringCchCopyW(sc.svc_status[i].buf, 32, L"not found");
                }

                if (snap.svc_auto[i] && !should_skip_autostop_for_service_status(sc.svc_status[i].buf)) {
                    if (_wcsicmp(sc.svc_status[i].buf, L"stopped") != 0) {
                        if ((mono_now - snap.svc_lastreq[i]) >= cd_ms) {
                                                        svc_enforce_idx.push_back(i);
                            snap.svc_lastreq[i] = mono_now;

                        }
                    }
                }
            }

            if (!svc_enforce_idx.empty()) {
                std::vector<int> svc_counts;
                svc_counts.reserve(svc_enforce_idx.size());

                {
                    ModelLockGuard lk(self);
                    for (size_t j = 0; j < svc_enforce_idx.size(); ++j) {
                        const size_t idx = svc_enforce_idx[j];
                        Item* it = nullptr;
                        if (idx < self.items[KIND_SVC].v.size()) {
                            Item* cand = self.items[KIND_SVC].v[idx].get();
                            if (cand && !snap.svc_names[idx].empty() && _wcsicmp(cand->name.c_str(), snap.svc_names[idx].c_str()) == 0) {
                                it = cand;
                            }
                        }
                        if (!it && idx < snap.ns && !snap.svc_names[idx].empty()) {
                            // Fallback: list changed since snapshot; find by name (rare)
                            it = list_find(&self.items[KIND_SVC], snap.svc_names[idx].c_str());
                        }
                        if (it) {
                            // If we recently saw repeated stop errors for this service, temporarily suppress AUTO-STOP.
                            if (it->svc_stop_suppress_until_ms && mono_now < it->svc_stop_suppress_until_ms) {
                                svc_counts.push_back(0);
                                continue;
                            }
                            it->last_autostop_mono_ms = mono_now;
                            it->autostop_count++;
                            svc_counts.push_back(it->autostop_count);
                        } else {
                            svc_counts.push_back(0);
                        }
                    }
                }

                for (size_t j = 0; j < svc_enforce_idx.size(); ++j) {
                    const size_t idx = svc_enforce_idx[j];
                    const int c = svc_counts[j];
                    if (c <= 0) continue;
                    if (idx >= ns || snap.svc_names[idx].empty()) continue;

                    wchar_t msg[600];
                    StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was %s) requested\u2026",
                                     snap.svc_names[idx].c_str(), c, sc.svc_status[idx].buf);
                    post_log(self, msg);

                    wchar_t reason[256];
                    StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", c);
                    action_enqueue(self, ItemKind::Svc, ACTION_STOP, snap.svc_names[idx].c_str(), reason, stop_wait_ms);
                }
            }

            // Option A: one message per kind per tick
            post_status_bulk(self, ItemKind::Svc, snap.svc_names, snap.svc_uids, sc.svc_status, ns, wall_now);
        }

        // --- EXEs: derive running/stopped from counts already populated by the unified snapshot ---
        if (ne) {
            // exe_counts were already filled by build_running_exe_hashes_lower above.

            std::vector<size_t> exe_enforce_idx;
            exe_enforce_idx.reserve(ne);

            for (size_t i = 0; i < ne; i++) {
                if (snap.exe_names[i].empty()) {
                    StringCchCopyW(sc.exe_status[i].buf, 32, L"unknown");
                    continue;
                }

                int c = sc.exe_counts[i];
                const wchar_t* status = (c > 0) ? L"running" : L"stopped";
                StringCchCopyW(sc.exe_status[i].buf, 32, status);

                if (snap.exe_auto[i] && _wcsicmp(status, L"stopped") != 0) {
                    if ((mono_now - snap.exe_lastreq[i]) >= cd_ms) {
                                                exe_enforce_idx.push_back(i);
                        snap.exe_lastreq[i] = mono_now;

                    }
                }
            }

            if (!exe_enforce_idx.empty()) {
                std::vector<int> exe_counts2;
                exe_counts2.reserve(exe_enforce_idx.size());

                {
                    ModelLockGuard lk(self);
                    for (size_t j = 0; j < exe_enforce_idx.size(); ++j) {
                        const size_t idx = exe_enforce_idx[j];
                        Item* it = nullptr;
                        if (idx < self.items[KIND_EXE].v.size()) {
                            Item* cand = self.items[KIND_EXE].v[idx].get();
                            if (cand && !snap.exe_names[idx].empty() && _wcsicmp(cand->name.c_str(), snap.exe_names[idx].c_str()) == 0) {
                                it = cand;
                            }
                        }
                        if (!it && idx < snap.ne && !snap.exe_names[idx].empty()) {
                            // Fallback: list changed since snapshot; find by name (rare)
                            it = list_find(&self.items[KIND_EXE], snap.exe_names[idx].c_str());
                        }
                        if (it) {
                            it->last_autostop_mono_ms = mono_now;
                            it->autostop_count++;
                            exe_counts2.push_back(it->autostop_count);
                        } else {
                            exe_counts2.push_back(0);
                        }
                    }
                }

                for (size_t j = 0; j < exe_enforce_idx.size(); ++j) {
                    const size_t idx = exe_enforce_idx[j];
                    const int k = exe_counts2[j];
                    if (k <= 0) continue;
                    if (idx >= ne || snap.exe_names[idx].empty()) continue;

                    wchar_t msg[600];
                    StringCchPrintfW(msg, 600, L"%s: AUTO-STOP enforce #%d (was running) requested\u2026",
                                     snap.exe_names[idx].c_str(), k);
                    post_log(self, msg);

                    wchar_t reason[256];
                    StringCchPrintfW(reason, 256, L"AUTO-STOP enforce #%d", k);
                    action_enqueue(self, ItemKind::Exe, ACTION_STOP, snap.exe_names[idx].c_str(), reason, stop_wait_ms);
                }
            }

            post_status_bulk(self, ItemKind::Exe, snap.exe_names, snap.exe_uids, sc.exe_status, ne, wall_now);
        }

        DWORD wait_ms = (DWORD)std::max(50, monitor_interval_ms);
        if (sleep_ms_cooperative(st, self, wait_ms)) break;
    }
    // MonSnap and MonScratch destructors clean up automatically via std::wstring/vector RAII.
}


// Rebuild cached WATCH keys for profiles.
// Must be called with self.mtx held.
// Uses shared_ptr so the monitor thread can grab a snapshot cheaply and iterate without allocations.
void rebuild_profile_watch_keys_locked(App& self) {
    auto nv = std::make_shared<std::vector<std::vector<std::wstring>>>();
    auto hv = std::make_shared<std::vector<std::vector<uint64_t>>>();
    nv->reserve(self.prof.profiles.size());
    hv->reserve(self.prof.profiles.size());

    for (auto& p : self.prof.profiles) {
        // Keep the cached keys in sync. watch_exes are already normalized lower-case basenames.
        if (p.watch_keys_lower.empty() && !p.watch_exes.empty()) {
            p.watch_keys_lower = p.watch_exes;
        } else if (p.watch_keys_lower.size() != p.watch_exes.size()) {
            // Best-effort resync; avoids stale cache if older configs/UI paths changed.
            p.watch_keys_lower = p.watch_exes;
        }
        nv->push_back(p.watch_keys_lower);

        // Also cache hashes for allocation-free matching in the monitor thread.
        std::vector<uint64_t> hs;
        hs.reserve(p.watch_keys_lower.size());
        for (const auto& k : p.watch_keys_lower) {
            if (k.empty()) continue;
            hs.push_back(fnv1a64_exe_lower_hash(k.c_str())); // already lower-case
        }
        std::sort(hs.begin(), hs.end());
        hs.erase(std::unique(hs.begin(), hs.end()), hs.end());
        hv->push_back(std::move(hs));
    }

    self.prof.watch_keys_sp = std::move(nv);
    self.prof.watch_hashes_sp = std::move(hv);
}


// ==================================================
// Process query helpers
// ==================================================

std::wstring normalize_start_target_best_effort(const std::wstring& raw) {
    std::wstring s = raw;

    // Trim whitespace
    while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
    if (s.empty()) return s;

    // Strip simple surrounding quotes
    if (s.size() >= 2 && ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\''))) {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
        if (s.empty()) return s;
    }

    // Expand env vars (%TEMP%, etc.) if present. If stack buffer is too small,
    // grow once rather than silently using the unexpanded string.
    if (s.find(L'%') != std::wstring::npos) {
        wchar_t exp[2048];
        DWORD got = ExpandEnvironmentStringsW(s.c_str(), exp, 2048);
        if (got == 0) {
            // failure — keep original
        } else if (got <= 2048) {
            s.assign(exp);
        } else {
            std::wstring big((size_t)got, L'\0');
            DWORD got2 = ExpandEnvironmentStringsW(s.c_str(), big.data(), got);
            if (got2 > 0 && got2 <= got) {
                big.resize((size_t)got2 - 1);  // drop trailing NUL
                s = std::move(big);
            }
        }
        while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
    }

    // If it looks like a filesystem path, canonicalize to full path best-effort.
    // For URLs / shell targets, leave as-is.
    // PathIsURLW covers http/https/ftp/mailto, etc.
    if (!s.empty() && !PathIsURLW(s.c_str())) {
        bool looks_path = (s.find(L'\\') != std::wstring::npos) ||
                          (s.find(L'/')  != std::wstring::npos) ||
                          (s.find(L':')  != std::wstring::npos) ||
                          (s.rfind(L".\\", 0) == 0) ||
                          (s.rfind(L"..\\", 0) == 0) ||
                          (s.rfind(L"./", 0) == 0) ||
                          (s.rfind(L"../", 0) == 0);

        if (looks_path) {
            wchar_t full[MAX_PATH];
            DWORD nFull = GetFullPathNameW(s.c_str(), MAX_PATH, full, NULL);
            if (nFull > 0 && nFull < MAX_PATH) {
                s.assign(full);
            }
        }
    }

    return s;
}

std::wstring normalize_start_target_best_effort(const wchar_t* raw) {
    if (!raw) return std::wstring();
    return normalize_start_target_best_effort(std::wstring(raw));
}


static std::wstring reg_read_sz_hkcr(const std::wstring& subkey, const wchar_t* value_name) {
    wchar_t buf[2048];
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LONG r = RegGetValueW(HKEY_CLASSES_ROOT, subkey.c_str(), value_name, RRF_RT_REG_SZ, &type, buf, &cb);
    if (r != ERROR_SUCCESS) return std::wstring();
    // Ensure null-terminated
    buf[(sizeof(buf)/sizeof(buf[0])) - 1] = 0;
    return std::wstring(buf);
}

static void wstr_replace_all(std::wstring& s, const wchar_t* from, const std::wstring& to) {
    if (!from || !from[0]) return;
    size_t pos = 0;
    const size_t nfrom = wcslen(from);
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, nfrom, to);
        pos += to.size();
    }
}

static bool create_process_track(const std::wstring& cmdline, const wchar_t* started_target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (cmdline.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty command line.");
        return false;
    }

    std::vector<wchar_t> cl(cmdline.begin(), cmdline.end());
    cl.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(NULL, cl.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) {
        DWORD e = GetLastError();
        if (out_result && cch) StringCchPrintfW(out_result, cch, L"CreateProcess failed (%lu).", (unsigned long)e);
        return false;
    }

    CloseHandle(pi.hThread);

    out_sp.started_target = started_target ? started_target : L"";
    out_sp.pid = pi.dwProcessId;
    out_sp.hproc.reset(pi.hProcess);
    if (out_result && cch) StringCchPrintfW(out_result, cch, L"Started (pid %lu).", (unsigned long)out_sp.pid);
    return true;
}

static bool start_ahk_via_association_command(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (!target || !target[0]) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty target.");
        return false;
    }

    const wchar_t* ext = PathFindExtensionW(target);
    if (!ext || _wcsicmp(ext, L".ahk") != 0) return false;

    // Get ProgID from HKCR\.ahk (default value)
    std::wstring progid = reg_read_sz_hkcr(L".ahk", NULL);
    if (progid.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"No .ahk association found (install AutoHotkey or associate .ahk).");
        return false;
    }

    // Get open command from HKCR\<ProgID>\shell\open\command
    std::wstring cmdkey = progid + L"\\shell\\open\\command";
    std::wstring cmd = reg_read_sz_hkcr(cmdkey, NULL);
    if (cmd.empty()) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Missing association open command for .ahk.");
        return false;
    }

    // Expand env vars in command
    if (cmd.find(L'%') != std::wstring::npos) {
        wchar_t exp[4096];
        DWORD got = ExpandEnvironmentStringsW(cmd.c_str(), exp, 4096);
        if (got > 0 && got < 4096) cmd.assign(exp);
    }

    std::wstring qtarget = L"\"";
    qtarget += target;
    qtarget += L"\"";

    // Replace common placeholders. Do the quoted variants first to avoid double-quotes.
    const std::wstring qt = qtarget;
    wstr_replace_all(cmd, L"\"%1\"", qt);
    wstr_replace_all(cmd, L"\"%L\"", qt);
    wstr_replace_all(cmd, L"\"%l\"", qt);
    wstr_replace_all(cmd, L"%1", qt);
    wstr_replace_all(cmd, L"%L", qt);
    wstr_replace_all(cmd, L"%l", qt);

    // Drop %* (extra args placeholder)
    wstr_replace_all(cmd, L"%*", L"");

    // If no placeholder was present, append the script path
    if (cmd.find(target) == std::wstring::npos && cmd.find(qt) == std::wstring::npos) {
        cmd.push_back(L' ');
        cmd += qt;
    }

    return create_process_track(cmd, target, out_sp, out_result, cch);
}

// If the target is an .exe path, derive the lowercased basename (for fallback stop-by-name).
bool shell_open_and_track_process(const wchar_t* target, StartedProc& out_sp, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;
    out_sp = StartedProc{};
    if (!target || !target[0]) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Empty target.");
        return false;
    }

    // Special-case .ahk: start via file-association command so we reliably get a process handle (for later profile teardown).
    {
        const wchar_t* ext = PathFindExtensionW(target);
        if (ext && _wcsicmp(ext, L".ahk") == 0) {
            StartedProc sp{};
            wchar_t tmp[256];
            tmp[0] = 0;
            if (start_ahk_via_association_command(target, sp, tmp, 256)) {
                out_sp = std::move(sp);
                if (out_result && cch) StringCchCopyW(out_result, cch, tmp);
                return true;
            }
            // If association-start failed, fall through to ShellExecuteEx (it may still work depending on system setup).
            // Keep tmp in case we fail below too.
        }
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = NULL;
    sei.lpVerb = L"open";
    sei.lpFile = target;
    sei.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (out_result && cch) {
            if (e == ERROR_NO_ASSOCIATION) {
                StringCchPrintfW(out_result, cch, L"No association for this file type (error %lu).", (unsigned long)e);
            } else {
                StringCchPrintfW(out_result, cch, L"ShellExecuteEx failed (%lu).", (unsigned long)e);
            }
        }
        return false;
    }

    out_sp.started_target = target;

    if (sei.hProcess) {
        out_sp.hproc.reset(sei.hProcess);
        out_sp.pid = GetProcessId(sei.hProcess);
        if (out_result && cch) {
            if (out_sp.pid) StringCchPrintfW(out_result, cch, L"Started (pid %lu).", (unsigned long)out_sp.pid);
            else StringCchCopyW(out_result, cch, L"Started.");
        }
    } else {
        // Common when opening documents/URLs that reuse an existing app instance.
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Started (no process handle).");
    }

    return true;
}

struct EnumCloseWindowsData {
    DWORD pid = 0;
};

static BOOL CALLBACK enum_close_windows_cb(HWND hwnd, LPARAM lp) {
    EnumCloseWindowsData* d = (EnumCloseWindowsData*)lp;
    if (!d || !d->pid) return TRUE;

    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != d->pid) return TRUE;

    // Skip tooltips/owned popups to reduce noise (still best-effort).
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void close_process_best_effort(DWORD pid, HANDLE hproc_in, int wait_ms, wchar_t* out_result, size_t cch) {
    if (out_result && cch) out_result[0] = 0;

    unique_handle hproc;
    if (hproc_in && hproc_in != INVALID_HANDLE_VALUE) {
        // Borrow handle: do NOT close (caller owns); duplicate so we can safely manage lifetime.
        HANDLE dup = NULL;
        DuplicateHandle(GetCurrentProcess(), hproc_in, GetCurrentProcess(), &dup,
                        SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, 0);
        if (dup) hproc.reset(dup);
    }

    if (!hproc && pid) {
        HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) hproc.reset(hp);
    }

    if (!hproc) {
        if (out_result && cch) {
            if (pid) StringCchPrintfW(out_result, cch, L"Cannot open process (pid %lu).", (unsigned long)pid);
            else     StringCchCopyW(out_result, cch, L"No process to close.");
        }
        return;
    }

    // If already exited, we're done.
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeProcess(hproc.get(), &code) && code != STILL_ACTIVE) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Already exited.");
        return;
    }

    // Ask nicely: WM_CLOSE to top-level windows for that PID.
    if (!pid) pid = GetProcessId(hproc.get());
    if (pid) {
        EnumCloseWindowsData d{ pid };
        EnumWindows(enum_close_windows_cb, (LPARAM)&d);
    }

    // Wait for graceful exit.
    int ms = wait_ms;
    if (ms < 0) ms = 0;
    DWORD wr = WaitForSingleObject(hproc.get(), (DWORD)ms);

    if (wr == WAIT_OBJECT_0) {
        if (out_result && cch) StringCchCopyW(out_result, cch, L"Closed.");
        return;
    }

    // Still running: force terminate.
    TerminateProcess(hproc.get(), 1);
    WaitForSingleObject(hproc.get(), kForceTermWaitMs);

    if (out_result && cch) StringCchCopyW(out_result, cch, L"Terminated.");
}

int process_count_by_name_lower(const wchar_t* exe_lower) {
    if (!exe_lower || !exe_lower[0]) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int count = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_lower) == 0) count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}


// Build a set of running process exe names (lowercased) from ONE Toolhelp snapshot.

std::wstring exe_watch_key_lower(const std::wstring& raw) {
    std::wstring s = raw;

    // Trim simple whitespace
    while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
    if (s.empty()) return s;

    // Strip simple quotes
    if (s.size() >= 2 && ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\''))) {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && (s.front()==L' '||s.front()==L'\t'||s.front()==L'\r'||s.front()==L'\n')) s.erase(s.begin());
        while (!s.empty() && (s.back()==L' '||s.back()==L'\t'||s.back()==L'\r'||s.back()==L'\n'))  s.pop_back();
        if (s.empty()) return s;
    }

    // If a path was provided, use the basename to match Toolhelp's szExeFile.
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);

    str_lower_inplace(s);
    return s;
}

// Fast, allocation-free hash of a case-insensitive exe basename (typically already basename).
// 64-bit FNV-1a over wchar_t code units (Windows: UTF-16).
uint64_t fnv1a64_exe_lower_hash(const wchar_t* s) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    if (!s) return h;

    for (const wchar_t* p = s; *p; ++p) {
        // Fold to lower without allocating; towlower is fine for ASCII exe names.
        wchar_t c = (wchar_t)towlower(*p);
        h ^= (uint16_t)c;
        h *= FNV_PRIME;
    }
    return h;
}

// Build sorted+unique running exe hashes once per tick (no heap per-process string allocations).
// Optionally also counts processes by name using the pre-sorted NameIdx array,
// eliminating the need for a second Toolhelp snapshot in the same tick.
//
// Internal 5-param version used by the monitor thread.
static bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes,
                                           const NameIdx* idx_sorted,
                                           size_t idx_n,
                                           int* out_counts,
                                           size_t n_total,
                                           bool collect_hashes) {
    out_hashes.clear();
    if (out_counts) {
        for (size_t i = 0; i < n_total; i++) out_counts[i] = 0;
    }

    const bool want_counts = (idx_sorted && idx_n > 0 && out_counts);

    // Nothing to produce: skip the (relatively expensive) full process-table
    // snapshot entirely. Lets a services-only / no-profile config go idle.
    if (!collect_hashes && !want_counts) return true;

    unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap.get(), &pe)) return false;

    do {
        if (pe.szExeFile[0]) {
            if (collect_hashes) out_hashes.push_back(fnv1a64_exe_lower_hash(pe.szExeFile));

            // If counting by name, look up this process in the sorted index.
            if (want_counts) {
                size_t orig = 0;
                if (binsearch_nameidx(idx_sorted, idx_n, pe.szExeFile, &orig)) {
                    if (orig < n_total) out_counts[orig] += 1;
                }
            }
        }
    } while (Process32NextW(snap.get(), &pe));

    if (collect_hashes) {
        std::sort(out_hashes.begin(), out_hashes.end());
        out_hashes.erase(std::unique(out_hashes.begin(), out_hashes.end()), out_hashes.end());
    }
    return true;
}

// Public 2-param overload matching app.h declaration.
bool build_running_exe_hashes_lower(std::vector<uint64_t>& out_hashes, std::vector<NameIdx>* out_names_opt) {
    (void)out_names_opt; // reserved for future use
    return build_running_exe_hashes_lower(out_hashes, nullptr, 0, nullptr, 0);
}

bool terminate_all_by_name_lower(const wchar_t* exe_lower, int* terminated, int* failed,
                                        std::vector<unique_handle>* out_handles) {
    if (terminated) *terminated = 0;
    if (failed) *failed = 0;

    unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return false;

    do {
        // _wcsicmp folds case, so compare szExeFile directly — no copy/lowercase needed.
        if (_wcsicmp(pe.szExeFile, exe_lower) == 0) {
            unique_handle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID));
            if (!h) {
                if (failed) (*failed)++;
                continue;
            }
            BOOL ok = TerminateProcess(h.get(), 1);
            if (ok) {
                if (terminated) (*terminated)++;
                // Keep handle for caller to wait on process exit
                if (out_handles) out_handles->push_back(std::move(h));
            } else {
                if (failed) (*failed)++;
            }
        }
    } while (Process32NextW(snap.get(), &pe));

    return true;
}


// ==================================================
// Service query helpers
// ==================================================

// ----------------------------
// Win32 error formatting
// ----------------------------
void format_winerr(DWORD e, wchar_t* out, size_t cch) {
    if (!out || cch == 0) return;
    out[0] = 0;

    wchar_t sys[256];
    sys[0] = 0;

    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, sys, 256, NULL
    );
    trim_ws_inplace(sys);

    if (sys[0]) {
        StringCchPrintfW(out, cch, L"winerror %lu: %s", e, sys);
    } else {
        StringCchPrintfW(out, cch, L"winerror %lu", e);
    }
}

// ----------------------------
// Service helpers
// ----------------------------

bool query_service_status_fast(const wchar_t* svc_name, wchar_t* out_status, size_t cch) {
    StringCchCopyW(out_status, cch, L"unknown");

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) return false;

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_QUERY_STATUS));
    if (!h) return false;

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes = 0;
    BOOL ok = QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes);
    if (!ok) return false;

    StringCchCopyW(out_status, cch, svc_state_to_str(ssp.dwCurrentState));
    return true;
}



bool stop_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch, DWORD* out_err) {
    StringCchCopyW(out_result, cch, L"error");
    if (out_err) *out_err = 0;

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        if (out_err) *out_err = e;
        return false;
    }

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        if (out_err) *out_err = e;
        return false;
    }

    SERVICE_STATUS_PROCESS ssp0{};
    DWORD bytes0 = 0;
    if (QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp0, sizeof(ssp0), &bytes0)) {
        if (ssp0.dwCurrentState == SERVICE_STOPPED) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }
    }

    SERVICE_STATUS st{};
    BOOL ok = ControlService(h.get(), SERVICE_CONTROL_STOP, &st);
    if (!ok) {
        DWORD e = GetLastError();
        if (out_err) *out_err = e;

        if (e == ERROR_SERVICE_NOT_ACTIVE) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: stop refused (%s)", ebuf);
        return false;
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, L"stop requested");
        return true;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes = 0;
    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;

    while (now_mono_ms() < deadline) {
        if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes)) {
            DWORD e = GetLastError();
            wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
            StringCchPrintfW(out_result, cch, L"stop requested (status query failed: %s)", ebuf);
            return true;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED) {
            StringCchCopyW(out_result, cch, L"stopped");
            return true;
        }

        // Use SCM's dwWaitHint if available; clamp to sane bounds.
        DWORD hint = ssp.dwWaitHint / 10; // SCM recommends 1/10 of hint
        if (hint < kScmPollMinMs) hint = kScmPollMinMs;
        if (hint > kScmPollMaxMs) hint = kScmPollMaxMs;
        uint64_t remain = deadline - now_mono_ms();
        if (remain < hint) hint = (DWORD)remain;
        Sleep(hint);
    }

    StringCchCopyW(out_result, cch, L"stop requested (still stopping or refused)");
    return true;
}

bool start_service_and_wait(const wchar_t* svc_name, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT));
    if (!scm) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenSCManager failed (%s)", ebuf);
        return false;
    }

    unique_sc_handle h(OpenServiceW(scm.get(), svc_name, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!h) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: OpenService failed (%s)", ebuf);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) {
        DWORD e = GetLastError();
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: status query failed (%s)", ebuf);
        return false;
    }

    if (ssp.dwCurrentState == SERVICE_RUNNING) {
        StringCchCopyW(out_result, cch, L"already running");
        return true;
    }

    if (!StartServiceW(h.get(), 0, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_ALREADY_RUNNING) {
            StringCchCopyW(out_result, cch, L"already running");
            return true;
        }
        wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
        StringCchPrintfW(out_result, cch, L"error: start failed (%s)", ebuf);
        return false;
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, L"start requested");
        return true;
    }

    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;
    for (;;) {
        if (now_mono_ms() >= deadline) break;
        if (!QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) break;
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            StringCchCopyW(out_result, cch, L"running");
            return true;
        }
        if (ssp.dwCurrentState == SERVICE_STOPPED) break;
        // Use SCM's dwWaitHint if available; clamp to sane bounds.
        DWORD hint = ssp.dwWaitHint / 10;
        if (hint < kScmPollMinMs) hint = kScmPollMinMs;
        if (hint > kScmPollMaxMs) hint = kScmPollMaxMs;
        uint64_t remain = deadline - now_mono_ms();
        if (remain < hint) hint = (DWORD)remain;
        Sleep(hint);
    }

    // Final status snapshot
    if (QueryServiceStatusEx(h.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes_needed)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) StringCchCopyW(out_result, cch, L"running");
        else if (ssp.dwCurrentState == SERVICE_START_PENDING) StringCchCopyW(out_result, cch, L"start pending");
        else StringCchCopyW(out_result, cch, L"start requested");
    } else {
        StringCchCopyW(out_result, cch, L"start requested");
    }
    return true;
}

bool start_exe_and_wait(const wchar_t* exe_lower_name, const wchar_t* launch_path_opt, int wait_ms, wchar_t* out_result, size_t cch) {
    StringCchCopyW(out_result, cch, L"error");

    if (!exe_lower_name || !exe_lower_name[0]) {
        StringCchCopyW(out_result, cch, L"error: invalid name");
        return false;
    }

    if (process_count_by_name_lower(exe_lower_name) > 0) {
        StringCchCopyW(out_result, cch, L"already running");
        return true;
    }

    wchar_t path[MAX_PATH] = {0};

    // Atomic existence probe (no handle open → no TOCTOU window opened here;
    // CreateProcessW below will do its own verification on the chosen path).
    auto exists_regular = [](const wchar_t* p) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fad)) return false;
        return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };

    if (launch_path_opt && launch_path_opt[0] && exists_regular(launch_path_opt)) {
        StringCchCopyW(path, MAX_PATH, launch_path_opt);
    } else {
        // Try SearchPath on PATH using the *original* exe name; best effort.
        // SearchPathW already validates existence at call time.
        wchar_t found[MAX_PATH] = {0};
        DWORD r = SearchPathW(NULL, exe_lower_name, NULL, MAX_PATH, found, NULL);
        if (r > 0 && r < MAX_PATH) {
            StringCchCopyW(path, MAX_PATH, found);
        } else {
            // Try with ".exe" suffix if not present
            if (!ends_with_i(exe_lower_name, L".exe")) {
                wchar_t buf2[260];
                StringCchCopyW(buf2, 260, exe_lower_name);
                StringCchCatW(buf2, 260, L".exe");
                r = SearchPathW(NULL, buf2, NULL, MAX_PATH, found, NULL);
                if (r > 0 && r < MAX_PATH) {
                    StringCchCopyW(path, MAX_PATH, found);
                }
            }
        }
    }

    // If we still don't have a path, fall back to ShellExecute using the name.
    bool used_shell_execute = false;
    if (!path[0]) {
        HINSTANCE h = ShellExecuteW(NULL, L"open", exe_lower_name, NULL, NULL, SW_SHOWNORMAL);
        // MSDN: ShellExecute returns a value > 32 on success.
        if ((uintptr_t)h <= 32) {
            StringCchCopyW(out_result, cch, L"error: not found on PATH (add with full path or use Browse\u2026)");
            return false;
        }
        used_shell_execute = true;
    } else {
        // CreateProcess
        wchar_t cmd[2 * MAX_PATH + 8];
        StringCchPrintfW(cmd, _countof(cmd), L"\"%s\"", path);

        // Working directory = folder of exe
        wchar_t workdir[MAX_PATH];
        StringCchCopyW(workdir, MAX_PATH, path);
        PathRemoveFileSpecW(workdir);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            NULL,
            cmd,
            NULL, NULL,
            FALSE,
            0,
            NULL,
            workdir[0] ? workdir : NULL,
            &si,
            &pi
        );
        if (!ok) {
            DWORD e = GetLastError();
            wchar_t ebuf[300]; format_winerr(e, ebuf, 300);
            wchar_t msg[300];
            StringCchPrintfW(msg, 300, L"error: start failed (%s)", ebuf);
            StringCchCopyW(out_result, cch, msg);
            return false;
        }
        CloseHandle(pi.hThread);

        // Use process handle to wait for startup instead of polling snapshots.
        if (wait_ms > 0) {
            DWORD wr = WaitForSingleObject(pi.hProcess, (DWORD)wait_ms);
            CloseHandle(pi.hProcess);
            if (wr == WAIT_TIMEOUT) {
                // Process still running after wait — success.
                StringCchCopyW(out_result, cch, L"running");
                return true;
            }
            // Process exited during wait — report started but not confirmed running.
            StringCchCopyW(out_result, cch, L"started (exited quickly)");
            return true;
        }
        CloseHandle(pi.hProcess);
    }

    if (wait_ms <= 0) {
        StringCchCopyW(out_result, cch, used_shell_execute ? L"started (ShellExecute)" : L"started");
        return true;
    }

    // ShellExecute path: no process handle, poll with snapshot.
    uint64_t deadline = now_mono_ms() + (uint64_t)wait_ms;
    while (now_mono_ms() < deadline) {
        if (process_count_by_name_lower(exe_lower_name) > 0) {
            StringCchCopyW(out_result, cch, L"running");
            return true;
        }
        Sleep(kExePollFallbackMs);
    }

    StringCchCopyW(out_result, cch, L"start requested");
    return true;
}



bool enum_services_build_disp_cache(DispCache* out_cache) {
    out_cache->m.clear();

    unique_sc_handle scm(OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm) return false;

    DWORD bytes_needed = 0;
    DWORD count = 0;
    DWORD resume = 0;

    constexpr DWORD kMaxEnumBuf = 512 * 1024;  // cap at 512 KB
    std::vector<BYTE> buf(128 * 1024);

    for (;;) {
        BOOL ok = EnumServicesStatusExW(
            scm.get(),
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32,
            SERVICE_STATE_ALL,
            buf.data(),
            (DWORD)buf.size(),
            &bytes_needed,
            &count,
            &resume,
            NULL
        );

        if (ok) break;

        DWORD e = GetLastError();
        if (e != ERROR_MORE_DATA || bytes_needed == 0) return false;

        DWORD new_size = bytes_needed + 16 * 1024;
        if (new_size > kMaxEnumBuf) return false;  // refuse unbounded growth
        buf.resize(new_size);
    }

    auto* arr = (ENUM_SERVICE_STATUS_PROCESSW*)buf.data();
    for (DWORD i = 0; i < count; i++) {
        const wchar_t* disp = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
        const wchar_t* key  = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
        if (!disp[0] || !key[0]) continue;

        std::wstring d(disp);
        str_lower_inplace(d);
        out_cache->m[d] = key;
    }

    out_cache->built_at_ms = now_mono_ms();
    return true;
}


void resolve_service_name(App& self, const wchar_t* raw, wchar_t* out, size_t cch) {
    StringCchCopyW(out, cch, raw ? raw : L"");
    trim_ws_inplace(out);
    if (!out[0]) return;

    wchar_t st[32];
    if (query_service_status_fast(out, st, 32)) return;

    const wchar_t* key = dispcache_lookup(&self.disp_cache, out);
    if (key) { StringCchCopyW(out, cch, key); return; }

    // Only rebuild if cache is stale (>30s) or never built
    constexpr uint64_t kCacheTTLMs = 30000;
    uint64_t now = now_mono_ms();
    if (self.disp_cache.built_at_ms == 0 || (now - self.disp_cache.built_at_ms) > kCacheTTLMs) {
        enum_services_build_disp_cache(&self.disp_cache);
        key = dispcache_lookup(&self.disp_cache, out);
        if (key) { StringCchCopyW(out, cch, key); return; }
    }
}
