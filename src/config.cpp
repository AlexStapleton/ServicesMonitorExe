#include "app.h"
#include <io.h>      // _fileno
#include <stdio.h>   // _commit lives in io.h; fileno in stdio

// ============================================================
// Configuration parsing
// ============================================================

// Config file format version. Bump when the on-disk format changes in a way
// that needs migration. Written as the first line of the save header and read
// back here so the loader can detect files from a newer build.
static constexpr int kConfigVersion = 1;

bool parse_setting(App& self, const wchar_t* key, const wchar_t* val) {
    if (!key || !val) return false;

    if (_wcsicmp(key, L"config_version") == 0) {
        int v = _wtoi(val);
        if (v > kConfigVersion) {
            self.deferred_logs.push_back(
                L"Config was written by a newer version of the app; "
                L"unrecognized settings will be ignored.");
        }
        return true; // accepted (no runtime field needed yet)
    }

    // NOTE: Settings are stored in milliseconds (ms).
    // For backward compatibility, we still accept legacy *_s keys (seconds) and convert to ms.
    auto parse_int = [&](int* out) -> bool {
        if (!out) return false;
        *out = _wtoi(val);
        return true;
    };

    auto parse_seconds_to_ms = [&](int* out_ms) -> bool {
        if (!out_ms) return false;
        double s = _wtof(val);
        if (s < 0) return false;
        long long ms = (long long)(s * 1000.0 + 0.5);
        if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
        *out_ms = (int)ms;
        return true;
    };

    if (_wcsicmp(key, L"ui_refresh_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v < UI_REFRESH_MS_MIN) v = UI_REFRESH_MS_MIN;
        if (v > UI_REFRESH_MS_MAX) v = UI_REFRESH_MS_MAX;
        self.cfg.ui_refresh_ms = v;
        return true;
    }
    if (_wcsicmp(key, L"monitor_interval_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { self.cfg.monitor_interval_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"monitor_interval_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms) && ms >= 200) { self.cfg.monitor_interval_ms = ms; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { self.cfg.autostop_cooldown_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { self.cfg.autostop_cooldown_ms = ms; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { self.cfg.stop_wait_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { self.cfg.stop_wait_ms = ms; return true; }
        return false;
    }

    // Tray settings
    if (_wcsicmp(key, L"tray_enabled") == 0) {
        int v = 0; parse_int(&v);
        self.tray.enabled = (v != 0);
        if (!self.tray.enabled) self.tray.close_to_tray = false;
        return true;
    }
    if (_wcsicmp(key, L"close_to_tray") == 0) {
        int v = 0; parse_int(&v);
        self.tray.close_to_tray = (v != 0);
        if (!self.tray.enabled) self.tray.close_to_tray = false;
        return true;
    }

    if (_wcsicmp(key, L"dark_mode") == 0) {
        int v = 0; parse_int(&v);
        self.theme.dark_mode = (v != 0);
        return true;
    }
    if (_wcsicmp(key, L"win_rect") == 0) {
        // x,y,w,h,max (max: 0/1)
        int x=0,y=0,w=0,h=0,mx=0;
        if (swscanf(val, L"%d,%d,%d,%d,%d", &x,&y,&w,&h,&mx) == 5) {
            if (w >= 300 && h >= 200) {
                self.win_x = x;
                self.win_y = y;
                self.win_w = w;
                self.win_h = h;
                self.win_maximized = (mx != 0);
                self.have_win_rect = true;
                return true;
            }
        }
        return false;
    }

    // Backward compat: old activity_panel_w maps to detail_panel_w
    if (_wcsicmp(key, L"activity_panel_w") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 160 && v <= 2000) { self.detail_panel_w = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"detail_panel_w") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 100 && v <= 2000) { self.detail_panel_w = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"activity_panel_h") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 30 && v <= 2000) { self.activity_panel_h = v; return true; }
        return false;
    }

    if (_wcsicmp(key, L"activity_collapsed") == 0) {
        int v = 0; parse_int(&v);
        self.activity_collapsed = (v != 0);
        return true;
    }

    if (_wcsicmp(key, L"active_tab") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0 && v <= 1) { self.active_tab = v; return true; }
        return false;
    }

    // Legacy: silently ignore old left_split_y fields
    if (_wcsicmp(key, L"left_split_y_svc") == 0 || _wcsicmp(key, L"left_split_y_exe") == 0) {
        return true;
    }



    if (_wcsicmp(key, L"svc_colw") == 0 || _wcsicmp(key, L"exe_colw") == 0) {
    int kind = (_wcsicmp(key, L"svc_colw") == 0) ? KIND_SVC : KIND_EXE;
    int w[NUM_LV_COLS] = {};
    int n = swscanf(val, L"%d,%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3], &w[4]);
    if (n >= 4) {
        // Backward compat: if only 4 values, use defaults for column 4
        if (n == 4) { w[4] = 160; /* default Last Update width */ }
        for (int i = 0; i < NUM_LV_COLS; ++i) self.cols_w[kind][i] = w[i];
        self.cols_have[kind] = true;
        return true;
    }
    return false;
}
if (_wcsicmp(key, L"svc_colorder") == 0 || _wcsicmp(key, L"exe_colorder") == 0) {
    int kind = (_wcsicmp(key, L"svc_colorder") == 0) ? KIND_SVC : KIND_EXE;
    int o[NUM_LV_COLS] = {};
    int n = swscanf(val, L"%d,%d,%d,%d,%d", &o[0], &o[1], &o[2], &o[3], &o[4]);
    if (n >= 4) {
        // Backward compat: if only 4 values, add default for 5th column
        if (n == 4) { o[4] = 4; }
        for (int i = 0; i < NUM_LV_COLS; ++i) self.cols_order[kind][i] = o[i];
        // Keep Auto-stop (column index 0) pinned to the left-most display position.
        int pos0 = -1;
        for (int i = 0; i < NUM_LV_COLS; ++i) if (self.cols_order[kind][i] == 0) { pos0 = i; break; }
        if (pos0 > 0) { int tmp = self.cols_order[kind][0]; self.cols_order[kind][0] = self.cols_order[kind][pos0]; self.cols_order[kind][pos0] = tmp; }
        self.cols_have[kind] = true;
        return true;
    }
    return false;
}

return false;
}


void normalize_exe_input(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    if (!out || cch_out == 0) return;
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[512];
    StringCchCopyW(buf, 512, raw);
    trim_ws_inplace(buf);

    // Strip simple surrounding quotes.
    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, (n - 1) * sizeof(wchar_t));
        }
    } else if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, (n - 1) * sizeof(wchar_t));
        }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    // Expand environment vars (e.g., %SystemRoot%). If expansion would exceed
    // our stack buffer, retry with a heap buffer. Silently falling back to the
    // unexpanded original is unsafe — attacker-controlled env vars could
    // bypass any subsequent path validation that assumed expansion happened.
    wchar_t exp[1024];
    std::wstring exp_heap;
    const wchar_t* s = buf;
    DWORD got = ExpandEnvironmentStringsW(buf, exp, 1024);
    if (got == 0) {
        // Expansion failed entirely — reject rather than using raw input.
        return;
    } else if (got <= 1024) {
        s = exp;
    } else {
        // Needs more room. `got` is the required size in wchars (incl. NUL).
        exp_heap.resize(got);
        DWORD got2 = ExpandEnvironmentStringsW(buf, exp_heap.data(), got);
        if (got2 == 0 || got2 > got) return;
        s = exp_heap.c_str();
    }

    // Keep only basename (users often paste full paths).
    const wchar_t* base = s;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') base = p + 1;
    }

    wchar_t name[512];
    StringCchCopyW(name, 512, base);
    trim_ws_inplace(name);
    if (!name[0]) return;

    if (!ends_with_i(name, L".exe")) {
        StringCchCatW(name, 512, L".exe");
    }

    // binsearch_nameidx is case-insensitive; no need to lowercase here.
    StringCchCopyW(out, cch_out, name);
}

void resolve_exe_launch_path(const wchar_t* raw, wchar_t* out, size_t cch_out) {
    if (!out || cch_out == 0) return;
    out[0] = 0;
    if (!raw) return;

    wchar_t buf[1024];
    StringCchCopyW(buf, 1024, raw);
    trim_ws_inplace(buf);

    // Strip simple surrounding quotes.
    if (buf[0] == L'"') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'"') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, (n - 1) * sizeof(wchar_t));
        }
    } else if (buf[0] == L'\'') {
        size_t n = wcslen(buf);
        if (n > 1 && buf[n - 1] == L'\'') {
            buf[n - 1] = 0;
            memmove(buf, buf + 1, (n - 1) * sizeof(wchar_t));
        }
    }
    trim_ws_inplace(buf);
    if (!buf[0]) return;

    // Expand env vars. Reject on truncation rather than silently using the
    // unexpanded input (attacker-controlled env vars could bypass later
    // validation).
    {
        wchar_t exp[1024];
        DWORD nExp = ExpandEnvironmentStringsW(buf, exp, 1024);
        if (nExp == 0) return;
        if (nExp > 1024) return;  // too large to safely fit our path buffer
        StringCchCopyW(buf, 1024, exp);
        trim_ws_inplace(buf);
    }

    // Helper: probe without opening a handle but using an atomic-ish check
    // (GetFileAttributesExW is a single call — no check-then-use window on the
    // same path like PathFileExistsW + later use would have in-place replaced).
    auto path_is_regular_file = [](const wchar_t* p) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fad)) return false;
        return (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };

    // If it looks like a path, canonicalize it first.
    const bool looks_path = (wcschr(buf, L'\\') || wcschr(buf, L'/') || wcschr(buf, L':'));
    if (looks_path) {
        wchar_t full[MAX_PATH];
        DWORD nFull = GetFullPathNameW(buf, MAX_PATH, full, NULL);
        if (nFull > 0 && nFull < MAX_PATH) {
            if (!ends_with_i(full, L".exe")) {
                // Avoid overflow; MAX_PATH buffer.
                if (wcslen(full) + 4 < MAX_PATH) StringCchCatW(full, MAX_PATH, L".exe");
            }
            if (path_is_regular_file(full)) {
                StringCchCopyW(out, cch_out, full);
                return;
            }
        }
        // If it was a path and doesn't exist, fall through to SearchPath.
    }

    // Try SearchPath on PATH (also handles names without .exe). Trust the
    // return value — SearchPathW already verifies existence at call time, so
    // an extra PathFileExistsW would only add a TOCTOU window.
    wchar_t found[MAX_PATH];
    DWORD r = SearchPathW(NULL, buf, NULL, MAX_PATH, found, NULL);
    if (r > 0 && r < MAX_PATH) {
        StringCchCopyW(out, cch_out, found);
        return;
    }

    // Try with ".exe" explicitly if missing.
    if (!ends_with_i(buf, L".exe")) {
        wchar_t buf2[1024];
        StringCchCopyW(buf2, 1024, buf);
        StringCchCatW(buf2, 1024, L".exe");
        r = SearchPathW(NULL, buf2, NULL, MAX_PATH, found, NULL);
        if (r > 0 && r < MAX_PATH) {
            StringCchCopyW(out, cch_out, found);
            return;
        }
    }
}

// ============================================================
// Config path helper
// ============================================================

void compute_default_cfg_path(wchar_t* out, size_t cch) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* last = wcsrchr(exe, L'\\');
    if (last) *last = 0;
    StringCchPrintfW(out, cch, L"%s\\windows_service_monitor.cfg", exe);
}

// ============================================================
// Profile-aware config snapshot parser (parse_setting_to_cfg)
// ============================================================

bool parse_setting_to_cfg(ConfigSnapshot& cfg, const wchar_t* key, const wchar_t* val) {
    if (!key || !val) return false;

    auto parse_int = [&](int* out) -> bool {
        if (!out) return false;
        *out = _wtoi(val);
        return true;
    };
    auto parse_seconds_to_ms = [&](int* out_ms) -> bool {
        if (!out_ms) return false;
        double s = _wtof(val);
        if (s < 0) return false;
        long long ms = (long long)(s * 1000.0 + 0.5);
        if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
        *out_ms = (int)ms;
        return true;
    };

    if (_wcsicmp(key, L"ui_refresh_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v < UI_REFRESH_MS_MIN) v = UI_REFRESH_MS_MIN;
        if (v > UI_REFRESH_MS_MAX) v = UI_REFRESH_MS_MAX;
        cfg.ui_refresh_ms = v;
        return true;
    }

    if (_wcsicmp(key, L"monitor_interval_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 200) { cfg.monitor_interval_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"monitor_interval_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms) && ms >= 200) { cfg.monitor_interval_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"autostop_cooldown_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { cfg.autostop_cooldown_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"autostop_cooldown_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { cfg.autostop_cooldown_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"stop_wait_ms") == 0) {
        int v = 0; parse_int(&v);
        if (v >= 0) { cfg.stop_wait_ms = v; return true; }
        return false;
    }
    if (_wcsicmp(key, L"stop_wait_s") == 0) {
        int ms = 0;
        if (parse_seconds_to_ms(&ms)) { cfg.stop_wait_ms = ms; return true; }
        return false;
    }

    if (_wcsicmp(key, L"tray_enabled") == 0) {
        int v = 0; parse_int(&v);
        cfg.tray_enabled = (v != 0);
        if (!cfg.tray_enabled) cfg.close_to_tray = false;
        return true;
    }
    if (_wcsicmp(key, L"close_to_tray") == 0) {
        int v = 0; parse_int(&v);
        cfg.close_to_tray = (v != 0);
        if (!cfg.tray_enabled) cfg.close_to_tray = false;
        return true;
    }

    return false;
}

// ============================================================
// Config load / save
// ============================================================

void load_config(App& self) {
    // Sweep any stranded "<cfg>.tmp" left by a prior crashed save. Keeping
    // stale temps around risks confusing future saves if the primary file is
    // ever deleted by a user.
    {
        wchar_t stale[MAX_PATH];
        StringCchPrintfW(stale, MAX_PATH, L"%s.tmp", self.cfg.cfg_path);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(stale, GetFileExInfoStandard, &fad) &&
            !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            DeleteFileW(stale);
        }
    }

    FILE* f = NULL;
    _wfopen_s(&f, self.cfg.cfg_path, L"rb");
    if (!f) {
        // First run or missing config — not an error, just use defaults.
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > kMaxConfigBytes) {
        fclose(f);
        if (sz > kMaxConfigBytes)
            self.deferred_logs.push_back(L"Config file too large — using defaults");
        return;
    }
    std::vector<char> bytes((size_t)sz + 1);
    size_t nread = fread(bytes.data(), 1, (size_t)sz, f);
    fclose(f);
    if (nread == 0) {
        self.deferred_logs.push_back(L"Config file could not be read — using defaults");
        return;
    }
    bytes[nread] = 0;

    // Skip a UTF-8 BOM (EF BB BF) if one is present. The app never writes a BOM,
    // but an editor (Notepad, some VS Code configs) may add one — without this the
    // bytes attach to the first token and the first SETTING line silently fails.
    const char* mb = bytes.data();
    if (nread >= 3 && (unsigned char)mb[0] == 0xEF
                   && (unsigned char)mb[1] == 0xBB
                   && (unsigned char)mb[2] == 0xBF) {
        mb += 3;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, mb, -1, NULL, 0);
    std::wstring wtxt_str((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, mb, -1, wtxt_str.data(), wlen);
    bytes.clear();  // free UTF-8 buffer early
    wchar_t* wtxt = wtxt_str.data();  // wcstok_s operates in-place on this buffer

    int restored = 0;
    bool settings_loaded = false;

// Profiles: default config is loaded as before (settings + monitored rows outside PROFILE blocks).
// Additional profiles can be defined in the cfg file like:
//   PROFILE|Gaming
//   WATCH|game.exe
//   STARTITEM|C:\\Path\\discord.exe
//   STARTITEM|C:\\Path\\SomeDocument.pdf
//   STARTEXE|discord.exe|C:\\Path\\discord.exe   (legacy; treated as STARTITEM too)
//   SETTING|autostop_cooldown_ms|5000
//   EXE|discord.exe|1
//   SVC|wuauserv|0
//   ENDPROFILE
//
// A profile activates automatically when ANY WATCH exe is running.
// If multiple profiles match, the first match in the config wins.
// When no profiles match, the app falls back to Default.
bool profiles_loaded = false;
{
    ModelLockGuard lk(self);
    self.prof.profiles.clear();
    self.prof.active = -1;
    self.prof.have_default_cfg = false;
    self.prof.watch_keys_sp = std::make_shared<std::vector<std::vector<std::wstring>>>();
    self.prof.watch_hashes_sp = std::make_shared<std::vector<std::vector<uint64_t>>>();
}

bool in_profile = false;
ProfileSnapshot cur_profile;
auto commit_profile = [&]() {
    if (!in_profile) return;
    // Ensure watch exes are unique-ish (best-effort)
    std::sort(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end());
    cur_profile.watch_exes.erase(std::unique(cur_profile.watch_exes.begin(), cur_profile.watch_exes.end()),
                                 cur_profile.watch_exes.end());

    cur_profile.watch_keys_lower = cur_profile.watch_exes; // cache normalized watch keys

    {
        ModelLockGuard lk(self);
        self.prof.profiles.push_back(std::move(cur_profile));
    }
    cur_profile = ProfileSnapshot{};
    in_profile = false;
    profiles_loaded = true;
};
auto start_profile = [&](const wchar_t* name) {
    commit_profile();
    in_profile = true;
    cur_profile = ProfileSnapshot{};
    if (name) cur_profile.name = name;

    // Inherit current app settings as the base for this profile.
    {
        ModelLockGuard lk(self);
        cur_profile.cfg.ui_refresh_ms = self.cfg.ui_refresh_ms;
        cur_profile.cfg.monitor_interval_ms = self.cfg.monitor_interval_ms;
        cur_profile.cfg.autostop_cooldown_ms = self.cfg.autostop_cooldown_ms;
        cur_profile.cfg.stop_wait_ms = self.cfg.stop_wait_ms;
        cur_profile.cfg.tray_enabled = self.tray.enabled;
        cur_profile.cfg.close_to_tray = self.tray.close_to_tray;
    }
};


    wchar_t* ctx = NULL;
    wchar_t* line = wcstok_s(wtxt, L"\n", &ctx);
    while (line) {
        trim_ws_inplace(line);
        if (!line[0] || line[0] == L'#') { line = wcstok_s(NULL, L"\n", &ctx); continue; }


const wchar_t* SETP = L"SETTING|";
if (_wcsnicmp(line, SETP, wcslen(SETP)) == 0) {
    wchar_t* p = line + wcslen(SETP);
    wchar_t* bar = wcschr(p, L'|');
    if (bar) {
        *bar = 0;
        if (in_profile) {
            if (parse_setting_to_cfg(cur_profile.cfg, p, bar + 1)) settings_loaded = true;
        } else {
            if (parse_setting(self, p, bar + 1)) settings_loaded = true;
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}



// Profile blocks
const wchar_t* PROFP = L"PROFILE|";
if (_wcsnicmp(line, PROFP, wcslen(PROFP)) == 0) {
    wchar_t* pname = line + wcslen(PROFP);
    trim_ws_inplace(pname);
    start_profile(pname);
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}
if (_wcsicmp(line, L"ENDPROFILE") == 0 || _wcsicmp(line, L"PROFILE_END") == 0) {
    commit_profile();
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}
const wchar_t* WATCHP = L"WATCH|";
if (_wcsnicmp(line, WATCHP, wcslen(WATCHP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(WATCHP);
        trim_ws_inplace(p);
        wchar_t norm[512];
        normalize_exe_input(p, norm, 512);
        if (norm[0]) cur_profile.watch_exes.push_back(norm);
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}


// STARTITEM lines: targets to shell-open on profile activation
const wchar_t* STARTITEMP = L"STARTITEM|";
if (_wcsnicmp(line, STARTITEMP, wcslen(STARTITEMP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(STARTITEMP);
        trim_ws_inplace(p);
        std::wstring target = normalize_start_target_best_effort(p);
        if (!target.empty()) {
            bool dup = false;
            for (const auto& r : cur_profile.start_items) {
                if (_wcsicmp(r.target.c_str(), target.c_str()) == 0) { dup = true; break; }
            }
            if (!dup) {
                StartItem si; si.target = target;
                cur_profile.start_items.push_back(std::move(si));
            }
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

// STARTEXE lines (legacy): treated as STARTITEM too (and we keep the old convenience of ensuring the EXE exists in the profile EXE list).
const wchar_t* STARTEXEP = L"STARTEXE|";
if (_wcsnicmp(line, STARTEXEP, wcslen(STARTEXEP)) == 0) {
    if (in_profile) {
        wchar_t* p = line + wcslen(STARTEXEP);
        trim_ws_inplace(p);

        // Optional legacy format: STARTEXE|name_or_path|fullpath
        wchar_t name_raw[1024]; name_raw[0] = 0;
        wchar_t exe_path_raw2[1024]; exe_path_raw2[0] = 0;
        wchar_t* bar = wcschr(p, L'|');
        if (bar) {
            *bar = 0;
            StringCchCopyW(name_raw, 1024, p);
            StringCchCopyW(exe_path_raw2, 1024, bar + 1);
        } else {
            StringCchCopyW(name_raw, 1024, p);
        }
        trim_ws_inplace(name_raw);
        trim_ws_inplace(exe_path_raw2);

        const wchar_t* best = exe_path_raw2[0] ? exe_path_raw2 : name_raw;
        std::wstring target = normalize_start_target_best_effort(best);
        if (!target.empty()) {
            bool dup = false;
            for (const auto& r : cur_profile.start_items) {
                if (_wcsicmp(r.target.c_str(), target.c_str()) == 0) { dup = true; break; }
            }
            if (!dup) {
                StartItem si; si.target = target;
                cur_profile.start_items.push_back(std::move(si));
            }
        }

        // Old convenience: also ensure it's present in the profile EXE list (Auto-stop OFF by default) so it's visible/managed.
        wchar_t norm[512]; norm[0] = 0;
        normalize_exe_input(best, norm, 512);
        if (norm[0]) {
            wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;
            resolve_exe_launch_path(best, exe_path_full, MAX_PATH);

            bool exists_in_cfg = false;
            for (auto& r : cur_profile.cfg.items[KIND_EXE]) {
                if (_wcsicmp(r.name.c_str(), norm) == 0) {
                    exists_in_cfg = true;
                    if (r.exe_path.empty() && exe_path_full[0]) r.exe_path = exe_path_full;
                    break;
                }
            }
            if (!exists_in_cfg) {
                ItemRow rr;
                rr.name = norm;
                rr.auto_stop = false;
                if (exe_path_full[0]) rr.exe_path = exe_path_full;
                cur_profile.cfg.items[KIND_EXE].push_back(std::move(rr));
            }
        }
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

        ItemKind kind = ItemKind::Svc;
        wchar_t name[512]; name[0] = 0;
        wchar_t exe_path_raw[1024]; exe_path_raw[0] = 0; // optional EXE launch path
        int auto_stop = 0;

        if (_wcsnicmp(line, L"SVC|", 4) == 0 || _wcsnicmp(line, L"EXE|", 4) == 0) {
            kind = (_wcsnicmp(line, L"EXE|", 4) == 0) ? ItemKind::Exe : ItemKind::Svc;
            wchar_t* p = line + 4;
            wchar_t* a = wcschr(p, L'|');
            if (a) {
                *a = 0;
                StringCchCopyW(name, 512, p);

                wchar_t* b = wcschr(a + 1, L'|');
                if (b) {
                    *b = 0;
                    auto_stop = _wtoi(a + 1);
                    StringCchCopyW(exe_path_raw, 1024, b + 1);
                } else {
                    auto_stop = _wtoi(a + 1);
                }
            } else {
                StringCchCopyW(name, 512, p);
                auto_stop = 0;
            }
        } else {
            wchar_t* a = wcschr(line, L'|');
            if (a) { *a = 0; StringCchCopyW(name, 512, line); auto_stop = _wtoi(a + 1); }
            else { StringCchCopyW(name, 512, line); auto_stop = 0; }
            kind = ItemKind::Svc;
        }

        trim_ws_inplace(name);
        trim_ws_inplace(exe_path_raw);
        if (!name[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        const int kind_i = ki(kind);
        wchar_t norm[512];
        if (kind == ItemKind::Exe) {
            normalize_exe_input(name, norm, 512);
            if (!norm[0]) { line = wcstok_s(NULL, L"\n", &ctx); continue; }
        } else {
            StringCchCopyW(norm, 512, name);
        }

        wchar_t exe_path_full[MAX_PATH]; exe_path_full[0] = 0;

        if (kind == ItemKind::Exe && exe_path_raw[0]) {
            resolve_exe_launch_path(exe_path_raw, exe_path_full, MAX_PATH);
        }


if (in_profile) {
    // Store into the profile snapshot instead of the live runtime list.
    bool dup = false;
    for (const auto& r : cur_profile.cfg.items[kind_i]) {
        if (_wcsicmp(r.name.c_str(), norm) == 0) { dup = true; break; }
    }
    if (!dup) {
        ItemRow r;
        r.name = norm;
        r.auto_stop = (auto_stop == 1);
        if (kind == ItemKind::Exe && exe_path_full[0]) r.exe_path = exe_path_full;
        cur_profile.cfg.items[kind_i].push_back(std::move(r));
    }
    line = wcstok_s(NULL, L"\n", &ctx);
    continue;
}

        Item* exist = NULL;
        {
            ModelLockGuard lk(self);
            exist = list_find(&self.items[kind_i], norm);
        }
        if (exist) { line = wcstok_s(NULL, L"\n", &ctx); continue; }

        Item* it = new (std::nothrow) Item();
        if (!it) break;
        it->kind = kind;
        it->name = norm;
        if (kind == ItemKind::Exe && exe_path_full[0]) it->exe_path = exe_path_full;
        it->auto_stop = (auto_stop == 1);
        it->img = (kind == ItemKind::Svc) ? 0 : 1;
        it->autostop_count = 0;
        it->last_autostop_mono_ms = 0;
        it->last_update_wall = time(NULL);

        if (kind == ItemKind::Svc) {
            wchar_t st[32];
            if (query_service_status_fast(norm, st, 32)) set_item_status(*it, st);
            else set_item_status(*it, L"not found");
        } else {
            int c = process_count_by_name_lower(norm);
            set_item_status(*it, (c > 0) ? L"running" : L"stopped");
        }

        {
        ModelLockGuard lk(self);
        // Centralized registration keeps uid_map/by_name/dirty/view caches consistent.
        bool ok = register_item_locked(self, kind, it);
        if (ok) {
            self.items_gen[kind_i]++;
        } else {
            // duplicate (or OOM) - keep model consistent
            delete it;
            it = nullptr;
        }
    }

        lv_set_row(self, kind == ItemKind::Svc ? self.lv_svc : self.lv_exe, it);

        log_linef(self, L"Monitoring restored (%s): %s (status: %s)",
                  (kind == ItemKind::Svc) ? L"service" : L"exe", it->name.c_str(), it->last_status);
        restored++;

        line = wcstok_s(NULL, L"\n", &ctx);
    }


// If the file ended while inside a profile block, commit it.
commit_profile();

// Persist the loaded Default config snapshot.
{
    ModelLockGuard lk(self);
    snapshot_from_runtime_locked(self, self.prof.default_cfg);
    self.prof.have_default_cfg = true;
    self.prof.active = -1;
    rebuild_profile_watch_keys_locked(self);
}
{
    std::lock_guard<std::shared_mutex> lk2(self.mtx);
    self.threads.mon_wake = true;
}
self.threads.mon_cv.notify_one();  // wake monitor — config loaded

    // wtxt_str (and wtxt pointer into it) freed automatically when function returns.

    if (restored > 0 || settings_loaded || profiles_loaded) request_save_debounced(self);

    // Restart UI timer with any loaded ui_refresh_ms
    PostMessageW(self.hwnd, WM_APP_RESTART_UI_TIMER, 0, 0);
}

void save_config_now(App& self) {
    wchar_t cfg_path[MAX_PATH] = {0};

    ConfigSnapshot def;
    std::vector<ProfileSnapshot> profs;

    // Snapshot active runtime back into its owning snapshot, then copy out everything needed for saving.
    {
        ModelLockGuard lk(self);

        // Ensure we have a default snapshot even if config was never loaded (fresh run).
        if (!self.prof.have_default_cfg) {
            snapshot_from_runtime_locked(self, self.prof.default_cfg);
            self.prof.have_default_cfg = true;
            self.prof.active = -1;
        }

        snapshot_active_from_runtime_locked(self);

        def = self.prof.default_cfg;
        profs = self.prof.profiles;

        StringCchCopyW(cfg_path, MAX_PATH, self.cfg.cfg_path);
    }

    wchar_t tmp[MAX_PATH];
    StringCchPrintfW(tmp, MAX_PATH, L"%s.tmp", cfg_path);

    FILE* f = NULL;
    _wfopen_s(&f, tmp, L"wb");
    if (!f) {
        log_linef(self, L"Config save failed: could not create temp file");
        return;
    }
    bool write_ok = true;  // track cumulative write success

    // Header + default settings
    char header[4096];

    // Snapshot UI-only fields into locals.
    // save_config_now is only called from the UI thread, so these reads are safe
    // without the model lock (UI-thread-only fields are never written by other threads).
    int wx = self.win_x, wy = self.win_y, ww = self.win_w, wh = self.win_h;
    int wmx = self.win_maximized ? 1 : 0;
    {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (self.hwnd && GetWindowPlacement(self.hwnd, &wp)) {
            RECT r = wp.rcNormalPosition;
            int tw = (int)(r.right - r.left);
            int th = (int)(r.bottom - r.top);
            if (tw >= 300 && th >= 200) { wx = (int)r.left; wy = (int)r.top; ww = tw; wh = th; }
            wmx = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;
        } else if (self.have_last_wp) {
            RECT r = self.last_wp.rcNormalPosition;
            int tw = (int)(r.right - r.left);
            int th = (int)(r.bottom - r.top);
            if (tw >= 300 && th >= 200) { wx = (int)r.left; wy = (int)r.top; ww = tw; wh = th; }
        }
    }
    int dpw = self.detail_panel_w;
    int aph = self.activity_panel_h;
    int acol = self.activity_collapsed ? 1 : 0;
    int atab = self.active_tab;
    int dark = self.theme.dark_mode ? 1 : 0;
    int svc_cw[NUM_LV_COLS], exe_cw[NUM_LV_COLS], svc_co[NUM_LV_COLS], exe_co[NUM_LV_COLS];
    for (int i = 0; i < NUM_LV_COLS; i++) {
        svc_cw[i] = self.cols_w[KIND_SVC][i];
        exe_cw[i] = self.cols_w[KIND_EXE][i];
        svc_co[i] = self.cols_order[KIND_SVC][i];
        exe_co[i] = self.cols_order[KIND_EXE][i];
    }

int n = snprintf(header, sizeof(header),
        "SETTING|config_version|%d\n"
        "SETTING|ui_refresh_ms|%d\n"
        "SETTING|monitor_interval_ms|%d\n"
        "SETTING|autostop_cooldown_ms|%d\n"
        "SETTING|stop_wait_ms|%d\n"
        "SETTING|tray_enabled|%d\n"
        "SETTING|close_to_tray|%d\n"
        "SETTING|dark_mode|%d\n"
"SETTING|win_rect|%d,%d,%d,%d,%d\n"
"SETTING|detail_panel_w|%d\n"
"SETTING|activity_panel_h|%d\n"
"SETTING|activity_collapsed|%d\n"
"SETTING|active_tab|%d\n"
"SETTING|svc_colw|%d,%d,%d,%d,%d\n"
"SETTING|exe_colw|%d,%d,%d,%d,%d\n"
"SETTING|svc_colorder|%d,%d,%d,%d,%d\n"
"SETTING|exe_colorder|%d,%d,%d,%d,%d\n"
        "# Lines:\n"
        "#   SVC|<service_key_name>|<auto_stop 0/1>\n"
        "#   EXE|<exe_name>|<auto_stop 0/1>\n"
        "# Profiles:\n"
        "#   PROFILE|<name>\n"
        "#   WATCH|<exe_name>            (any WATCH running activates the profile)\n"
        "#   SETTING|<key>|<val>         (optional overrides for this profile)\n"
        "#   SVC|...\n"
        "#   EXE|...\n"
        "#   ENDPROFILE\n",
        kConfigVersion,
        def.ui_refresh_ms, def.monitor_interval_ms, def.autostop_cooldown_ms, def.stop_wait_ms,
        def.tray_enabled ? 1 : 0, def.close_to_tray ? 1 : 0,
        dark,
        wx, wy, ww, wh, wmx,
        dpw,
        aph,
        acol,
        atab,
        svc_cw[0], svc_cw[1], svc_cw[2], svc_cw[3], svc_cw[4],
        exe_cw[0], exe_cw[1], exe_cw[2], exe_cw[3], exe_cw[4],
        svc_co[0], svc_co[1], svc_co[2], svc_co[3], svc_co[4],
        exe_co[0], exe_co[1], exe_co[2], exe_co[3], exe_co[4]
    );
    if (n < 0 || (size_t)n >= sizeof(header)) {
        fclose(f);
        DeleteFileW(tmp);
        log_linef(self, L"Config save failed: header buffer overflow");
        return;
    }
    if (fwrite(header, 1, (size_t)n, f) != (size_t)n) write_ok = false;

    std::vector<char> utf8_buf(2048);  // pre-allocated for typical line lengths
    auto write_wline = [&](const wchar_t* wline) {
        int blen = WideCharToMultiByte(CP_UTF8, 0, wline, -1, NULL, 0, NULL, NULL);
        if (blen <= 0) return;
        if ((size_t)blen > utf8_buf.size()) utf8_buf.resize((size_t)blen);
        WideCharToMultiByte(CP_UTF8, 0, wline, -1, utf8_buf.data(), blen, NULL, NULL);
        size_t to_write = (size_t)(blen - 1);
        if (fwrite(utf8_buf.data(), 1, to_write, f) != to_write) write_ok = false;
    };

    // Truncation-proof formatted line writer. Fast path uses a stack buffer for
    // the common case; if a long name/path would overflow it we grow on the heap
    // so config rows are never silently truncated (which would lose data on save).
    auto write_wfmt = [&](const wchar_t* fmt, auto... args) {
        wchar_t stackbuf[700];
        if (StringCchPrintfW(stackbuf, ARRAYSIZE(stackbuf), fmt, args...) == S_OK) {
            write_wline(stackbuf);
            return;
        }
        for (size_t cap = ARRAYSIZE(stackbuf) * 2; cap <= (1u << 20); cap *= 2) {
            std::wstring buf(cap, L'\0');
            if (StringCchPrintfW(buf.data(), cap, fmt, args...) == S_OK) {
                buf.resize(wcslen(buf.c_str()));
                write_wline(buf.c_str());
                return;
            }
        }
        // Pathologically long (>1M wchars): drop the row rather than truncate.
        write_ok = false;
    };

    // Default items
    for (const auto& r : def.items[KIND_SVC]) {
        if (r.name.empty()) continue;
        write_wfmt(L"SVC|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
    }
    for (const auto& r : def.items[KIND_EXE]) {
        if (r.name.empty()) continue;
        if (!r.exe_path.empty()) {
            write_wfmt(L"EXE|%s|%d|%s\n", r.name.c_str(), r.auto_stop ? 1 : 0, r.exe_path.c_str());
        } else {
            write_wfmt(L"EXE|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
        }
    }

    // Profiles
    for (const auto& p : profs) {
        if (p.name.empty()) continue;

        write_wfmt(L"\nPROFILE|%s\n", p.name.c_str());

        for (const auto& w : p.watch_exes) {
            if (w.empty()) continue;
            write_wfmt(L"WATCH|%s\n", w.c_str());
        }

        // Items/targets to start on profile activation (shell-open; any file type)
        for (const auto& r : p.start_items) {
            if (r.target.empty()) continue;
            write_wfmt(L"STARTITEM|%s\n", r.target.c_str());
        }



        // Emit profile settings explicitly (simple + predictable). Since profile cfg inherits defaults on load,
        // saving them makes the profile self-contained and resilient to future default changes.
        write_wfmt(L"SETTING|ui_refresh_ms|%d\n", p.cfg.ui_refresh_ms);
        write_wfmt(L"SETTING|monitor_interval_ms|%d\n", p.cfg.monitor_interval_ms);
        write_wfmt(L"SETTING|autostop_cooldown_ms|%d\n", p.cfg.autostop_cooldown_ms);
        write_wfmt(L"SETTING|stop_wait_ms|%d\n", p.cfg.stop_wait_ms);
        write_wfmt(L"SETTING|tray_enabled|%d\n", p.cfg.tray_enabled ? 1 : 0);
        write_wfmt(L"SETTING|close_to_tray|%d\n", p.cfg.close_to_tray ? 1 : 0);

        for (const auto& r : p.cfg.items[KIND_SVC]) {
            if (r.name.empty()) continue;
            write_wfmt(L"SVC|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
        }
        for (const auto& r : p.cfg.items[KIND_EXE]) {
            if (r.name.empty()) continue;
            if (!r.exe_path.empty()) {
                write_wfmt(L"EXE|%s|%d|%s\n", r.name.c_str(), r.auto_stop ? 1 : 0, r.exe_path.c_str());
            } else {
                write_wfmt(L"EXE|%s|%d\n", r.name.c_str(), r.auto_stop ? 1 : 0);
            }
        }

        write_wline(L"ENDPROFILE\n");
    }

    // Flush userland buffers, then force a disk-level sync before rename so
    // a crash between fclose() and MoveFileExW() can't leave the replacement
    // with torn contents. _commit() = FlushFileBuffers() on the underlying HANDLE.
    if (fflush(f) != 0) write_ok = false;
    if (write_ok) {
        int fd = _fileno(f);
        if (fd < 0 || _commit(fd) != 0) write_ok = false;
    }
    // fclose returns non-zero on flush failure (e.g. disk full)
    if (fclose(f) != 0) write_ok = false;

    if (!write_ok) {
        DeleteFileW(tmp);
        log_linef(self, L"Config save failed: write error (disk full?)");
        return;
    }

    if (!MoveFileExW(tmp, cfg_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        log_linef(self, L"Config save failed: could not replace config file (err %lu)", GetLastError());
    }
}
