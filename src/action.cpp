// action.cpp — Action thread implementation.
#include "app.h"

// Treat these as "soft" errors: back off and retry later rather than hammering STOP every tick.
static bool is_service_stop_soft_error(DWORD err) {
    switch (err) {
        case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:   // 1061
        case ERROR_SERVICE_BUSY:                 // 1052
        case ERROR_SERVICE_REQUEST_TIMEOUT:      // 1053
        case ERROR_DEPENDENT_SERVICES_RUNNING:   // 1051
        case ERROR_SHUTDOWN_IN_PROGRESS:         // 1115
            return true;
        default:
            return false;
    }
}

void action_thread_main(std::stop_token st, App* self_ptr) {
    App& self = *self_ptr;
    for (;;) {
        Action a;
        {
            if (!action_pop_wait(st, self, a)) break;
        }

        // Profile Start Items (shell-open + best-effort close)
        if (a.op == ACTION_LAUNCH_ITEM) {
            StartedProc sp;
            wchar_t result[256];
            std::wstring target = normalize_start_target_best_effort(a.target.empty() ? a.name : a.target);

            bool ok = shell_open_and_track_process(target.c_str(), sp, result, 256);
            if (ok) {
                ModelLockGuard lk(self);
                self.prof.started_procs.emplace_back(std::move(sp));

                if (!target.empty() && ends_with_i(target.c_str(), L".exe")) {
                    std::wstring key = exe_watch_key_lower(target);
                    if (!key.empty()) self.prof.started_exes.insert(std::move(key));
                }
            }

            wchar_t line[900];
            StringCchPrintfW(line, 900, L"%s: %s → %s",
                             target.c_str(), a.reason.c_str(), result);
            post_log(self, line);
            continue;
        }

        if (a.op == ACTION_CLOSE_STARTED) {
            wchar_t result[256];
            close_process_best_effort(a.pid, a.hproc.get(), a.wait_ms, result, 256);
            wchar_t line[900];
            const wchar_t* label = a.name.empty() ? L"(started item)" : a.name.c_str();
            StringCchPrintfW(line, 900, L"%s: %s → %s",
                             label, a.reason.c_str(), result);
            post_log(self, line);
            continue;
        }

        if (a.kind == ItemKind::Svc) {
            wchar_t result[256];
            if (a.op == ACTION_START) {
                bool ok_start = start_service_and_wait(a.name.c_str(), a.wait_ms, result, 256);

                // Successful manual/profile starts should clear any previous stop backoff state.
                if (ok_start) {
                    ModelLockGuard lk(self);
                    Item* it = list_find(&self.items[KIND_SVC], a.name.c_str());
                    if (it) {
                        it->svc_stop_fail_streak = 0;
                        it->svc_stop_suppress_until_ms = 0;
                        it->svc_last_stop_err = 0;
                    }
                }
            } else {
                DWORD stop_err = 0;
                bool ok_stop = stop_service_and_wait(a.name.c_str(), a.wait_ms, result, 256, &stop_err);

                // Smart suppression: if a service keeps rejecting stop control (e.g. WinError 1061),
                // back off and eventually disable AUTO-STOP to prevent infinite noisy loops.
                uint32_t streak = 0;
                uint64_t backoff_ms = 0;
                bool disabled_autostop = false;

                {
                    const uint64_t now_ms = now_mono_ms();
                    ModelLockGuard lk(self);
                    Item* it = list_find(&self.items[KIND_SVC], a.name.c_str());
                    if (it) {
                        if (ok_stop) {
                            it->svc_stop_fail_streak = 0;
                            it->svc_stop_suppress_until_ms = 0;
                            it->svc_last_stop_err = 0;
                        } else {
                            it->svc_last_stop_err = stop_err;
                            it->svc_stop_fail_streak++;
                            streak = it->svc_stop_fail_streak;

                            if (is_service_stop_soft_error(stop_err)) {
                                // Exponential backoff: 5s, 10s, 20s... capped, with a floor for 1061.
                                uint32_t shift = (streak > 1) ? std::min<uint32_t>(streak - 1, 7u) : 0u;
                                backoff_ms = 5000ull * (1ull << shift);
                                if (stop_err == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) backoff_ms = std::max<uint64_t>(backoff_ms, 30000ull);
                                backoff_ms = std::min<uint64_t>(backoff_ms, 10ull * 60ull * 1000ull);
                            } else {
                                // Other stop failures: short linear backoff, capped.
                                backoff_ms = std::min<uint64_t>(15000ull * (uint64_t)std::min<uint32_t>(streak, 6u), 2ull * 60ull * 1000ull);
                            }

                            it->svc_stop_suppress_until_ms = now_ms + backoff_ms;

                            // After enough consecutive failures, stop trying automatically until the user re-enables it.
                            if (streak >= 8 && it->auto_stop) {
                                it->auto_stop = false;
                                disabled_autostop = true;
                            }
                        }
                    }
                }

                // Optional informational log on repeated failures (kept outside lock).
                if (!ok_stop && backoff_ms) {
                    wchar_t ebuf[200]; format_winerr(stop_err, ebuf, 200);
                    wchar_t msg[700];
                    if (disabled_autostop) {
                        StringCchPrintfW(msg, 700, L"%s: AUTO-STOP disabled after repeated stop failures (%s).", a.name.c_str(), ebuf);
                    } else {
                        StringCchPrintfW(msg, 700, L"%s: stop failed (%s) — suppressing AUTO-STOP for %llu ms (streak %u).",
                                         a.name.c_str(), ebuf, (unsigned long long)backoff_ms, (unsigned)streak);
                    }
                    post_log(self, msg);
                }
            }

            wchar_t line[700];
            StringCchPrintfW(line, 700, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(self, line);
        } else {
            if (a.op == ACTION_START) {
                std::wstring launch;
                {
                    ModelReadGuard lk(self);
                    Item* it = list_find(&self.items[KIND_EXE], a.name.c_str());
                    if (it) launch = it->exe_path;
                }
                wchar_t result[256];
                start_exe_and_wait(a.name.c_str(), launch.c_str(), a.wait_ms, result, 256);

                wchar_t line[700];
                StringCchPrintfW(line, 700, L"%s: %s → %s",
                                 a.name.c_str(), a.reason.c_str(), result);
                post_log(self, line);
                continue;
            }

            int terminated = 0, failed = 0;
            std::vector<unique_handle> term_handles;
            bool ok = terminate_all_by_name_lower(a.name.c_str(), &terminated, &failed, &term_handles);

            wchar_t result[256];
            if (!ok) {
                StringCchCopyW(result, 256, L"error: terminate failed");
            } else {
                // Wait for terminated processes to fully exit using handles
                // instead of polling with Sleep(150) + process snapshots.
                if (a.wait_ms > 0 && !term_handles.empty()) {
                    DWORD n = (DWORD)std::min(term_handles.size(), (size_t)MAXIMUM_WAIT_OBJECTS);
                    HANDLE raw[MAXIMUM_WAIT_OBJECTS];
                    for (DWORD i = 0; i < n; i++) raw[i] = term_handles[i].get();
                    DWORD remaining = (DWORD)a.wait_ms;
                    WaitForMultipleObjects(n, raw, TRUE, remaining);
                    term_handles.clear();
                }
                if (terminated == 0 && failed == 0) {
                    // No matching process was found in the snapshot.
                    StringCchCopyW(result, 256, L"not running");
                } else if (failed && !terminated) {
                    StringCchPrintfW(result, 256, L"error: failed to terminate (%d)", failed);
                } else if (failed) {
                    StringCchPrintfW(result, 256, L"terminated (%d), failed (%d)", terminated, failed);
                } else {
                    StringCchPrintfW(result, 256, L"terminated (%d)", terminated);
                }
            }

            wchar_t line[800];
            StringCchPrintfW(line, 800, L"%s: %s → %s",
                             a.name.c_str(), a.reason.c_str(), result);
            post_log(self, line);
        }
    }

    // Remaining queued work is cleared during WM_DESTROY after all workers are joined.
}
