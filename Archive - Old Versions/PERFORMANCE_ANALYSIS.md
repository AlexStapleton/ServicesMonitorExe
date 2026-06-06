# Performance Analysis — ServicesExeMonitor

Date: 2026-03-28

## CRITICAL — Hot-path overhead (every frame / every repaint)

### 1. GDI object churn in every paint handler
- **Files:** theme.cpp:162-164, 437-497, 535-579 / darkmode.cpp:101-112
- `CreateSolidBrush` + `CreatePen` + `DeleteObject` on every WM_DRAWITEM / NM_CUSTOMDRAW call. With 10 visible buttons + listview rows, that's 50+ kernel-mode GDI alloc/free cycles per repaint.
- **Fix:** Cache `br_pressed`, `br_border_pen`, `br_accent`, `pen_accent` in `ThemeState`. Create once in `theme_update_resources()`, reuse everywhere.
- **Status:** DONE — cached br_pressed, br_border, br_accent, pen_border, pen_accent in ThemeState

### 2. Double lock acquisition in LVN_GETDISPINFO
- **File:** main.cpp:316-348
- Virtual listview dispatch info handler (fires for every visible row on every repaint) acquires the model lock twice — once to read the UID, once to look up the Item. Also holds the lock during `fmt_time_local()` formatting.
- **Fix:** Single lock scope, copy needed fields out, format outside the lock.
- **Status:** DONE — consolidated 3 lock scopes into 1

### 3. Layout repositions invisible windows
- **File:** ui_layout.cpp:743-756
- Both `center_page_svc`/`center_page_exe` and both `lv_svc`/`lv_exe` get `SetWindowPos` on every layout call, even though only one tab is visible. During resize drags, layout fires continuously.
- **Fix:** Only reposition the active tab's controls.
- **Status:** DONE — layout skips inactive tab's page/lv/empty controls

### 4. Repeated GetDlgItem in layout hot path
- **File:** ui_layout.cpp:626-638
- 4x `GetDlgItem` calls (toolbar buttons) on every layout pass. Each is an O(n) child window search.
- **Fix:** Cache these HWNDs in the App struct during `build_ui()`.
- **Status:** DONE — toolbar_settings/profiles/tray/quit cached in App struct

## HIGH — Monitor thread overhead

### 5. Full process snapshot per exe count query
- **Files:** monitor.cpp:902-920, 1365-1373, 194-198
- `process_count_by_name_lower()` calls `CreateToolhelp32Snapshot` (enumerates ALL OS processes) just to count one exe. Called in polling loops every 150ms during start/stop waits — potentially 33+ full snapshots per wait operation.
- **Fix:** Remove redundant StringCchCopy in inner loop — compare pe.szExeFile directly.
- **Status:** DONE — eliminated per-process string copy

### 6. Service enumeration buffer growth without cap
- **Files:** monitor.cpp:1381-1427
- `enum_services_build_disp_cache` allocates 128KB+ buffers per retry with no upper bound. Called as fallback on every cache miss in `resolve_service_name`.
- **Fix:** Cap buffer size at 512KB, add TTL-based cache (30s) so full re-enum only happens periodically.
- **Status:** DONE — added kMaxEnumBuf cap + built_at_ms TTL in DispCache

## MEDIUM — Per-item work in filter/update paths

### 7. Per-item status lowercase conversion during filtering
- **File:** ui_listview.cpp:415-419
- Every call to `item_matches_filter()` converts the status string to lowercase in a stack buffer. With 200 items, that's 200 string copies per filter keystroke.
- **Fix:** Use `StrStrIW` (shlwapi) for case-insensitive substring search — zero allocation.
- **Status:** DONE — replaced StringCchCopy+str_lower+wcsstr with single StrStrIW call

### 8. Sort+unique dedup in status updates
- **File:** ui_listview.cpp:351-356
- `std::sort` + `std::unique` on dirty items vector. O(n log n) when a simple `std::unordered_set<Item*>` during collection would be O(n).
- **Fix:** Collect into a set, then iterate.
- **Status:** DONE — dedup during collection with unordered_set, also removed redundant reserve(64)

### 9. TextExtent measurement on every activity log line
- **File:** ui_listview.cpp:295-309
- Each `activity_append()` call acquires an HDC, selects a font, measures text width, releases DC, and sends 3 messages. For burst logging, this is severe.
- **Fix:** Estimate width from character count (~7px/char at 96 DPI, DPI-scaled).
- **Status:** DONE — eliminated HDC/font/ReleaseDC per append

### 10. Blanket InvalidateRect during theme application
- **File:** theme.cpp:269-369
- 10+ calls to `InvalidateRect(child, NULL, TRUE)` — full-control invalidation with background erase on every child. Causes cascading repaints.
- **Fix:** Changed all erase flags from TRUE to FALSE; parent already does RedrawWindow(RDW_ALLCHILDREN).
- **Status:** DONE — all InvalidateRect calls in theme_apply_to_control now use FALSE

### 11. Redundant vector reserve
- **File:** ui_listview.cpp:327-336
- `deltas.reserve(64)` immediately followed by `deltas.reserve(dirty.size())`. First call is always overridden.
- **Fix:** Remove the first reserve.
- **Status:** DONE — merged into fix #8 (single reserve(dirty.size()) now)

## LOW — Startup / infrequent paths

### 12. SetWindowTextW on collapse button every layout
- **File:** ui_layout.cpp:711
- Text set unconditionally even when state hasn't changed, triggers unnecessary redraws.
- **Fix:** Only update when `activity_collapsed` changes.
- **Status:** DONE — tracks last_collapsed state, only calls SetWindowTextW on change

### 13. Config save resizes utf8_buf per line
- **File:** config.cpp:863-871
- `WideCharToMultiByte` called twice per line (measure + convert), vector resize each time.
- **Fix:** Pre-allocate to 2048 bytes, only grow if needed.
- **Status:** DONE — initial reserve(2048) avoids resizing for typical lines

### 14. Dialog listbox fills without WM_SETREDRAW
- **File:** ui_dialogs.cpp:829-920
- Loops of `LB_ADDSTRING` without suppressing redraws. Causes flicker and O(n) repaints.
- **Fix:** Wrap in `WM_SETREDRAW FALSE` / `TRUE` + InvalidateRect(FALSE).
- **Status:** DONE — all 4 fill functions wrapped

### 15. GetModuleHandleW / GetProcAddress not cached for DPI functions
- **File:** util.cpp:86-95
- `get_dpi_for_hwnd()` does module lookup on every call.
- **Fix:** Cache GetDpiForWindow/GetDpiForSystem in DllCache at startup.
- **Status:** DONE — function pointers resolved once in dll_cache_init()

---

## Pass 2 — Additional optimizations (2026-03-28)

### 16. Condition variable wake-up latency on model changes
- **Files:** main.cpp (add/remove item), profiles.cpp (apply_profile_index), config.cpp (load_config)
- Monitor thread sleeps up to `monitor_interval_ms` (default 1000ms) via `mon_cv.wait_until`. Model mutations (add/remove items, profile switch, config load) didn't signal the CV, causing up to 1s delay before the monitor sees changes.
- **Fix:** Added `mon_cv.notify_one()` after every model mutation that bumps `items_gen`.
- **Status:** DONE — 4 notify points: add item, remove item, profile switch, config load

### 17. Font cache linear scan → O(1) lookup
- **Files:** app.h, theme.cpp
- `ui_font_cache` was `vector<pair<UINT, HFONT>>` with linear scan in `theme_find_font_cached()`. Multi-monitor setups with different DPIs would degrade.
- **Fix:** Changed to `std::unordered_map<UINT, HFONT>` for O(1) lookup by DPI.
- **Status:** DONE — find/insert/iterate all updated

### 18. dll_cache_init() called on every get_dpi_for_hwnd()
- **Files:** util.cpp, main.cpp
- `get_dpi_for_hwnd()` is a hot-path function called during layout/paint. It called `dll_cache_init()` which checks `g_dll_cache.inited` every time — a branch in every DPI query.
- **Fix:** Moved `dll_cache_init()` to wWinMain startup; removed the call from `get_dpi_for_hwnd()`.
- **Status:** DONE — single startup call, hot path is just pointer checks

### 19. LoadLibraryW → GetModuleHandleW for already-loaded DLLs
- **File:** util.cpp
- `dll_cache_init()` used `LoadLibraryW` for dwmapi.dll and uxtheme.dll, bumping ref counts unnecessarily. These DLLs are already loaded on DWM-enabled systems.
- **Fix:** Prefer `GetModuleHandleW` (no ref-count bump), fall back to `LoadLibraryW` if not yet loaded.
- **Status:** DONE — avoids unnecessary library load overhead

### 20. Light-mode pill tab GDI object churn
- **Files:** app.h, theme.cpp
- Pill-tab button drawing in light mode created/deleted `CreateSolidBrush` and `CreatePen` on every `WM_DRAWITEM`. Dark mode was fixed in pass 1 but light mode still churned.
- **Fix:** Added `br_lt_accent`, `pen_lt_accent`, `pen_lt_border` to ThemeState. Created once in `theme_update_resources()`, freed in `theme_release_brushes()`.
- **Status:** DONE — zero per-paint GDI alloc in both light and dark mode pill tabs

## Pass 3 — Bug fixes and cleanup (2026-03-28)

### 21. Light-mode BS_OWNERDRAW buttons invisible
- **File:** theme.cpp
- All action/toolbar buttons created with `BS_OWNERDRAW` were not painted in light mode. `theme_draw_owner_button()` returned false for non-pill buttons when `!dark_mode`, and `DarkButtonSubclassProc` skipped paint in light mode. Result: completely blank buttons in light mode.
- **Fix:** Extended `theme_draw_owner_button()` to handle both light and dark mode. Light mode uses system colors (`COLOR_BTNFACE`, `COLOR_BTNTEXT`) and `DrawEdge(EDGE_RAISED/SUNKEN)`.
- **Status:** DONE

### 22. darkmode.cpp LoadLibraryW → GetModuleHandleW
- **File:** darkmode.cpp
- `darkmode_init_api()` used `LoadLibraryW(L"uxtheme.dll")` even though uxtheme is typically already loaded. Same pattern fixed in util.cpp (#19).
- **Fix:** Try `GetModuleHandleW` first, fall back to `LoadLibraryW`.
- **Status:** DONE

### 23. Redundant theme_apply_to_control code for EDIT/ListBox
- **File:** theme.cpp
- Identical `dll_cache_init()` + `SetWindowTheme` blocks duplicated for EDIT and ListBox controls, with redundant calls in both dark/light branches.
- **Fix:** Merged into a single combined `if` with ternary theme selection.
- **Status:** DONE

### 24. _wcsnicmp RICHEDIT comparison length off by one
- **File:** theme.cpp
- `_wcsnicmp(cls, L"RICHEDIT", 7)` compared only 7 chars instead of 8. Would incorrectly match any class starting with "RICHEDI".
- **Fix:** Changed to `_wcsnicmp(cls, L"RICHEDIT", 8)`.
- **Status:** DONE
