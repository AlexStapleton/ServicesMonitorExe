# ServicesExeMonitor v167_v5 - Full Code Analysis

## Overview

- **File**: `ServicesExeMonitor_v167_v5.cpp` - 10,668 lines, ~384KB, single monolithic source file
- **Language**: C++20 Win32 Unicode (MinGW-w64)
- **Purpose**: Windows Service + EXE monitor with auto-stop policy, profiles, dark mode, system tray

---

## CRITICAL: Security Vulnerabilities

### 1. Command Injection via `_wsystem()` (Line 3262-3263)

```cpp
StringCchPrintfW(cmd, 512, L"taskkill /IM \"%s\" /F >NUL 2>NUL", a.name.c_str());
_wsystem(cmd);
```

**Risk**: `a.name` comes from config/user input. A crafted EXE name like `foo" & net user hacker P@ss /add & "` would escape the quotes and execute arbitrary commands **as Administrator** (since the app requires elevation).

**Fix**: Replace with `CreateProcessW` calling `taskkill.exe` directly with argv-style arguments (no shell interpretation), or better yet, use `TerminateProcess` API directly since you already have the PID from the Toolhelp snapshot.

### 2. Raw `HANDLE` in `Action` Struct (Line 1110)

```cpp
HANDLE hproc = NULL; // duplicated handle; action worker closes it
```

The `Action` struct uses a raw `HANDLE` instead of `unique_handle`. If an action is popped but never fully processed (e.g., thread stop requested mid-queue), the handle leaks. Every other handle in the codebase uses RAII wrappers - this one was missed.

**Fix**: Use `unique_handle` and `.release()` at the point of consumption.

---

## HIGH: Correctness Bugs

### 3. Inconsistent FNV-1a Hash Offset Basis (Lines 928 vs 4694)

Two separate FNV-1a implementations use **different offset basis values**:

| Location | Offset Basis | Correct? |
|----------|-------------|----------|
| `WStrCIHash::fnv1a64_ci` (line 928) | `1469598103934665603` | **Wrong** (truncated) |
| `fnv1a64_exe_lower_hash` (line 4694) | `14695981039346656037` | Correct |

The hash at line 928 is used for the `ItemList::by_name` map (case-insensitive item lookup). The hash at line 4694 is used for profile watch-exe matching. While they operate on different data sets today, the mismatch is a latent bug and the truncated value at line 928 will produce worse hash distribution.

**Fix**: Consolidate into a single `fnv1a64` function with the correct offset basis (`14695981039346656037ULL`).

### 4. Config Saved Partially Outside Lock

In `save_config_now`, a snapshot of data is taken under the model lock, but the actual file I/O and some data reads happen outside the lock. If another thread modifies profiles or items between the snapshot and the write, the saved config could be inconsistent.

**Fix**: Take a complete deep copy of all data needed for serialization under the lock, then write outside the lock.

### 5. `dispcache_free` Doesn't Actually Free (Line 1067-1070)

```cpp
static void dispcache_free(DispCache* D) {
    if (!D) return;
    D->m.clear();
}
```

This clears the map contents but doesn't free the `DispCache` object itself. If the caller expects this to be a full cleanup, it's a misleading API. More importantly, `clear()` on an `unordered_map` is redundant since the destructor handles it.

**Fix**: Remove the function entirely; rely on `DispCache`'s automatic destructor.

### 6. `list_remove_name` O(n) Scan Despite Having a Hash Map (Lines 1000-1018)

The function looks up `target` in `by_name` (O(1)) then does a linear scan of the vector to find the same pointer. This defeats the purpose of the hash map.

**Fix**: Store the vector index in the map value, or use `std::find` with the pointer comparison as a targeted fix.

---

## HIGH: Performance Issues

### 7. Repeated Process Snapshots Per Monitor Tick

`monitor_thread_main` calls `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` multiple times per tick:
- Once in `build_running_exe_hashes_lower` for profile matching
- Once per monitored EXE in `process_count_by_name_lower`
- Potentially again in `terminate_all_by_name_lower` during action processing

Each snapshot is a kernel call that freezes the process list. With 20+ monitored EXEs, this means 20+ snapshots per second.

**Fix**: Take **one** snapshot per tick. Build a `std::unordered_map<std::wstring, int>` (name -> count) from it, and use that for all lookups in the same cycle. Pass this map to the profile-matching and status-update functions.

### 8. Per-Item String Allocations in Filter Path

When filtering the ListView, each item's display name is lowercased into a new `std::wstring` for comparison. With 500+ services this runs every debounce tick.

**Fix**: Cache the lowercased name in the `Item` struct (set once at creation, costs ~40 bytes per item). Filter against the cached value.

### 9. `wcsdup_heap` Allocations for Log Messages (Line 4256-4264)

```cpp
LogMsg* m = (LogMsg*)malloc(sizeof(LogMsg));
m->text = wcsdup_heap(text ? text : L"");
```

Every log message does two heap allocations (`malloc` + `wcsdup_heap`), then two `free` calls on the UI thread. This is unnecessary churn.

**Fix**: Use `std::wstring` inside `LogMsg` or a pre-allocated ring buffer for log messages.

### 10. LoadLibrary/FreeLibrary Not Called Per-Use but UxTheme Kept Loaded

The dark mode API loads `uxtheme.dll` once and never frees it (line 3991). This is actually fine for a DLL that stays loaded for app lifetime, but the code should document this is intentional. The real issue is that `g_dm` is a global mutable static, making the init function non-thread-safe if called from multiple threads (unlikely but worth noting).

---

## MEDIUM: Architecture Issues

### 11. 10,668-Line Monolithic File

The entire application (UI, threading, config, dark mode, tray, profiles, dialogs, layout engine) lives in one file. This makes:
- Compilation slow (any change recompiles everything)
- Navigation extremely difficult
- Testing individual components impossible

**Suggested split**:
| Module | Contents | Approx Lines |
|--------|----------|-------------|
| `main.cpp` | `wWinMain`, `AppWndProc`, message loop | ~800 |
| `app.h` | `App` struct, `Item`, `ItemList`, constants, IDs | ~400 |
| `config.cpp/.h` | `load_config`, `save_config_now`, `parse_setting` | ~600 |
| `monitor.cpp/.h` | `monitor_thread_main`, process/service queries | ~800 |
| `action.cpp/.h` | `action_thread_main`, action queue, service start/stop | ~600 |
| `ui_layout.cpp/.h` | `build_ui`, layout engine, splitters | ~800 |
| `ui_listview.cpp/.h` | ListView helpers, owner-data dispatch, filtering | ~600 |
| `ui_dialogs.cpp/.h` | Preferences, Help, Diagnostics, Profiles dialogs | ~1800 |
| `darkmode.cpp/.h` | Dark mode API, custom draw, theme brushes | ~800 |
| `tray.cpp/.h` | System tray management | ~200 |
| `util.cpp/.h` | String helpers, RAII wrappers, DPI helpers | ~400 |

### 12. `App` Struct is a God Object (~220 Fields)

The `App` struct (starting line 1116) holds everything: window handles, fonts, brushes, mutex, data model, config, profiles, tray state, splitter state, filter state, column widths, timers, and more.

**Fix**: Break into focused sub-structs:
```cpp
struct App {
    UiState ui;          // window handles, fonts, brushes, DPI
    DataModel model;     // items, uid_map, generation counters
    ConfigState config;  // settings, profiles, config path
    TrayState tray;      // nid, tray_added, tray_enabled
    ThreadState threads; // jthreads, stop sources, action queue
    LayoutState layout;  // splitter positions, column widths
};
```

### 13. `#define app() (self)` Macro (Line 3954)

This macro exists as a migration artifact from when a global `App*` was used. It obscures code and breaks IDE navigation.

**Fix**: Remove the macro. All call sites already have `App& self` in scope.

### 14. Forward Declarations Span Hundreds of Lines

Functions are declared forward then defined much later (sometimes 3000+ lines away). This is a symptom of the monolithic file. After splitting into modules, forward declarations become unnecessary within each file.

---

## MEDIUM: Threading & Concurrency

### 15. Static Locals in Dialog WndProcs

Dialog procedures like the Profiles dialog use `static App* self` to capture state:

```cpp
static LRESULT CALLBACK ProfilesDlgProc(...) {
    static App* s_app = nullptr;
    // ...
}
```

If two dialog instances were ever opened simultaneously (unlikely but possible via keyboard shortcuts + race), they would corrupt each other's state.

**Fix**: Use `DWLP_USER` / `SetWindowLongPtr` to store per-dialog instance data, as is already done for the main window.

### 16. Lock Ordering Complexity

The code implements manual lock-order enforcement with `thread_local int g_model_lock_depth` (line 1354). This is fragile — it catches violations at runtime but doesn't prevent them at compile time.

**Fix**: Consider using `std::scoped_lock` with multiple mutexes where needed, or redesign so only one mutex is required (e.g., use a message-passing queue from worker threads to the UI thread, eliminating shared mutable state).

### 17. `Sleep(150)` Busy-Wait Loop (Line 3270)

```cpp
while (!st.stop_requested() && now_mono_ms() < deadline) {
    if (process_count_by_name_lower(a.name.c_str()) == 0) break;
    Sleep(150);
}
```

This polls with `Sleep` and takes a process snapshot every 150ms. There are several instances of this pattern.

**Fix**: Use `WaitForSingleObject` on the process handle (if available) or `RegisterWaitForSingleObject` for event-driven notification when a process exits.

---

## MEDIUM: Code Quality

### 18. Mixed C and C++ Memory Management

The codebase mixes `malloc`/`free` with `std::vector`/`std::unique_ptr`/`std::wstring`. Examples:
- `load_config`: `malloc` for file buffer (line 2559), `malloc` for wide string (line 2566)
- `LogMsg`: `malloc` for struct + `wcsdup_heap` (line 4256-4258)
- `StatusMsg`: C-style struct with raw `wchar_t*` member
- `MonScratch`: `malloc`/`realloc`/`free` for status arrays (lines ~3850-3904)

Meanwhile, `ItemList`, `ProfileSnapshot`, `ConfigSnapshot` all use modern C++ containers.

**Fix**: Replace C allocations with C++ equivalents:
- File I/O: `std::vector<char>` or `std::string`
- `LogMsg`/`StatusMsg`: Use `std::wstring` members
- `MonScratch`: Use `std::vector<std::wstring>` and `std::vector<wchar_t[32]>`

### 19. `typedef struct X { ... } X;` Pattern (Lines 1116, 4253, 4268)

```cpp
typedef struct App { ... } App;
typedef struct LogMsg { wchar_t* text; } LogMsg;
typedef struct StatusMsg { ... } StatusMsg;
```

This is a C pattern unnecessary in C++. In C++, `struct App { ... };` is sufficient.

**Fix**: Remove `typedef` wrapper from all struct definitions.

### 20. Magic Numbers Throughout

Examples:
- `Sleep(150)` — why 150ms?
- `sz > 5 * 1024 * 1024` — 5MB config limit (line 2558)
- `wchar_t result[256]`, `wchar_t cmd[512]`, `wchar_t line[900]` — various buffer sizes with no named constants
- `DPI(8)`, `DPI(12)`, `DPI(24)` — pixel margins scattered through layout code

**Fix**: Define named constants:
```cpp
constexpr int kPollIntervalMs = 150;
constexpr long kMaxConfigBytes = 5 * 1024 * 1024;
constexpr int kResultBufLen = 256;
constexpr int kLayoutMargin = 8;  // in DIP
```

### 21. Duplicate Status Display Logic

`lv_update_row_existing_with` builds a display string for the "Status" column, and a similar string is built elsewhere for the owner-data `LVN_GETDISPINFO` handler. The formatting logic is duplicated.

**Fix**: Extract a `format_status_display(Item&) -> std::wstring` function.

### 22. Repeated Normalize/Trim Patterns

String normalization (lowercase, trim whitespace, extract basename) is implemented inline in many places rather than through shared utility functions.

**Fix**: Consolidate into a small set of utility functions: `to_lower(wstring_view)`, `trim(wstring_view)`, `basename(wstring_view)`.

---

## LOW: Minor Issues & Improvements

### 23. Build Comment References Wrong File (Line 5)

```cpp
// g++ ... ServicesExeMonitor_v95.cpp ...
```

The build instruction references `_v95.cpp` but the file is `_v167_v5.cpp`.

**Fix**: Update build comment or make it generic.

### 24. `#include <cstdlib>` After Code (Line 106)

Several C headers are included after the RAII structs are already defined (lines 106-110). While not a bug (headers are allowed anywhere), it's unconventional and confusing.

**Fix**: Move all `#include` directives to the top of the file.

### 25. `unique_handle` Doesn't Handle `INVALID_HANDLE_VALUE` Consistently

The constructor accepts any `HANDLE` including `INVALID_HANDLE_VALUE`, but `operator bool()` returns false for it. This is correct but `reset()` only closes if `h != INVALID_HANDLE_VALUE`, which means assigning `INVALID_HANDLE_VALUE` then calling `reset()` won't leak — but it's confusing.

**Fix**: Add a comment documenting the semantics, or reject `INVALID_HANDLE_VALUE` in the constructor.

### 26. Empty `dispcache_push` and `dispcache_lookup` Wrappers

These thin wrappers around `unordered_map::operator[]` and `find` add no value over direct map access. They're remnants of when the cache was a C-style array.

**Fix**: Remove wrappers, use the map directly.

### 27. `[[maybe_unused]]` on `list_remove_name` (Line 1000)

This attribute suppresses the unused-function warning, suggesting the function may not actually be called. Dead code should be removed, not annotated.

**Fix**: Verify if it's used. If not, delete it.

### 28. `memset(&wc, 0, sizeof(wc))` Instead of Value Initialization (Line 4241)

```cpp
WNDCLASSW wc;
memset(&wc, 0, sizeof(wc));
```

**Fix**: Use `WNDCLASSW wc{};` — cleaner and cannot miss padding bytes.

### 29. Tray Menu Leak on `TrackPopupMenu` Failure

```cpp
HMENU menu = CreatePopupMenu();
// ... AppendMenuW ...
TrackPopupMenu(menu, ...);
DestroyMenu(menu);
```

If `TrackPopupMenu` throws or the window is destroyed mid-display, `DestroyMenu` may not execute. Minor since the app would be exiting anyway.

### 30. No Error Handling on `fread` (Line 2561)

```cpp
fread(bytes, 1, (size_t)sz, f);
```

The return value is ignored. If `fread` returns fewer bytes than expected (disk error, truncated file), the buffer will contain uninitialized data.

**Fix**: Check return value and handle short reads.

---

## Proposed Change Priority

### Phase 1: Critical Fixes — DONE
1. ~~**Fix command injection**~~ — Replaced `_wsystem` with `CreateProcessW` (no shell interpretation)
2. ~~**Fix HANDLE leak**~~ — `Action::hproc` now uses `unique_handle` with RAII cleanup
3. ~~**Fix FNV hash mismatch**~~ — Unified to correct offset basis `14695981039346656037ULL`

### Phase 2: Performance — DONE
4. ~~**Single process snapshot per tick**~~ — `build_running_exe_hashes_lower` now also counts EXE processes via optional `NameIdx` parameter; monitor loop takes ONE `CreateToolhelp32Snapshot` instead of two
5. ~~**Cache lowercased names**~~ — Added `Item::name_lower` (set once in `register_item_locked`); `item_matches_filter` uses cached value (zero allocations per filter tick)
6. ~~**Replace malloc/free with C++ containers**~~ — ALL malloc/free eliminated:
    - `MonSnap::svc_names`/`exe_names`: `vector<wchar_t*>` + `wcsdup_heap`/`free` → `vector<std::wstring>` (RAII cleanup)
    - `LogMsg`: `malloc` struct + `wcsdup_heap` text → `new std::wstring` / `delete` (single allocation)
    - `StatusMsg`: Removed entirely (dead code — `WM_APP_STATUS` was never posted; replaced by generation-based `WM_APP_STATUS_GEN`)
    - `load_config`: `malloc` byte/wchar buffers → `std::vector<char>` + `std::wstring`
    - `save_config_now` `write_wline` lambda: per-call `malloc`/`free` → reusable `std::vector<char>` buffer
    - `wcsdup_heap` helper function: Removed (no callers remain)
    - `WM_APP_STATUS` define and handler: Removed (dead code)

### Phase 3: Architecture (Maintainability) — PARTIAL
7. **Split into modules** — Deferred (too large/risky for single pass; file naming suggests single-file preference)
8. **Break up App struct** — Deferred (would touch thousands of field references across 10K+ lines)
9. ~~**Remove `#define app() (self)` macro**~~ — Removed (was unused)
10. ~~**Fix dialog static locals**~~ — Converted PrefsWndProc, HelpWndProc, ProfilesWndProc to per-instance state structs (`PrefsState`, `ProfilesState`); follows existing `DiagState` pattern
    - Also removed unused `batch_count_processes_by_name_lower` (compiler warning fix)
11. ~~**Complete ItemKind migration**~~ — Eliminated dual kind system:
    - `enum { KIND_SVC, KIND_EXE }` → `constexpr int` (retained only for direct array indexing)
    - `Item::kind` and `Action::kind` changed from `int` to `ItemKind`
    - 17 function signatures converted from `int kind` → `ItemKind kind`
    - All forward declarations updated to match
    - All call sites updated (`KIND_SVC` → `ItemKind::Svc`, `KIND_EXE` → `ItemKind::Exe`)
    - Removed 4 back-compat `int kind` wrapper functions
    - Removed `kind_from_int()` bridge function; renamed `kind_index()` → `ki()`
12. ~~**Fix relaunch_as_admin argument injection**~~ — Replaced naive quoting with proper `escape_argv()` using Windows CommandLineToArgvW escaping convention (backslash-doubled before quotes, embedded quotes escaped). Fixed-size 4096-char buffer replaced with `std::wstring`.

### Phase 5: Final Polish (Deep Dive Audit) — DONE
13. ~~**Drain pending WM_APP_LOG on exit**~~ — WM_DESTROY now drains pending WM_APP_LOG messages from the queue, deleting their `std::wstring*` payloads to prevent exit-time leak.
14. ~~**Fix double-lock in lv_get_selected_item_ptr**~~ — Merged two separate lock acquisitions into a single `ModelLockGuard` scope (was wasteful and fragile).
15. ~~**Remove dead code**~~ — Removed `list_free`, `list_push` (never called, marked `[[maybe_unused]]`), `snapshot_active_from_runtime` (non-locked wrapper never called; `_locked` version kept), and stale `[[maybe_unused]]` from `HelpWndProc`/`prof_get_lb_text` which ARE used.
16. ~~**Replace memset with value initialization**~~ — All 6 remaining `memset(&x, 0, sizeof(x))` → `Type x{}` (NONCLIENTMETRICSW, LVCOLUMNW, LVFINDINFOW, LVITEMW ×2, TCITEMW).
17. ~~**Fix status_disp buffer overflow risk**~~ — Increased `ui_cache_status_disp` and local `status_disp` from `[64]` to `[128]`; added `constexpr kStatusDispBuf = 128` constant used at all 12 call sites.
18. ~~**Fix indentation issues**~~ — Corrected misindented `continue`, closing braces in `apply_profile_index`, font cache cleanup in WM_DESTROY, and `tray_enabled`/`close_to_tray` init in wWinMain.
19. ~~**Convert #define IDs to constexpr**~~ — All 70+ `#define` macros for WM_APP messages, timer IDs, menu IDs (IDM_*), and control IDs (IDC_*) converted to `constexpr int`/`constexpr UINT`/`constexpr UINT_PTR`. Kept only legitimate `#define` uses (preprocessor guards, function-like macros, SDK compat defines).
20. ~~**Named constants for magic numbers**~~ — Added `kStatusDispBuf`, `kForceTermWaitMs`, and named uxtheme ordinals (`kOrdAllowDarkModeForWindow`, `kOrdSetPreferredAppMode`, `kOrdFlushMenuThemes`).

### Phase 6: Dark Mode Consistency — DONE
21. ~~**Dark scrollbars**~~ — Applied `DarkMode_CFD` theme to ListViews, Edit controls, and Listboxes in dark mode. This enables native dark scrollbars on Windows 10 1903+ and Windows 11.
22. ~~**Dark ListView checkboxes**~~ — **REVERTED.** Initially added `create_dark_checkbox_imagelist()` that programmatically drew dark-themed checkbox bitmaps and applied them as state imagelists. Checkboxes stopped rendering entirely, so the feature was removed. ListViews now use standard system-themed state images (light checkboxes appear on dark background — a known visual inconsistency).
23. ~~**System dark/light mode detection**~~ — Added `WM_SETTINGCHANGE` handler that detects `"ImmersiveColorSet"` broadcasts, reads `AppsUseLightTheme` from registry, and auto-toggles dark mode to match the system.
24. ~~**Consolidated NM_CUSTOMDRAW dispatch**~~ — Extracted shared `theme_handle_customdraw()` helper. Replaced duplicate inline blocks in main WndProc and Profiles dialog. Added dark-mode custom draw support to Help, Hotkeys, and Diagnostics dialogs.
25. ~~**Listbox dark theme support**~~ — Added `"ListBox"` class handling in `theme_apply_to_control` (was entirely missing).

### High-Impact Fixes (Cross-Phase) — DONE
- ~~**Fix #4: Config save atomicity**~~ — `save_config_now` now captures `dark_mode`, `cols_w[][]`, `cols_order[][]` into locals before file I/O
- ~~**Fix #17: Replace Sleep(150) busy-waits**~~ — Four locations fixed:
    - `stop_service_and_wait`: Uses SCM `dwWaitHint` (clamped [100, 2000] ms) instead of hardcoded 150ms
    - `start_service_and_wait`: Same `dwWaitHint`-based polling
    - EXE terminate wait: `terminate_all_by_name_lower` now returns process handles; caller uses `WaitForMultipleObjects` instead of polling snapshots
    - `start_exe_and_wait`: Keeps `pi.hProcess` from `CreateProcessW`; uses `WaitForSingleObject` instead of polling (ShellExecute path falls back to 250ms poll)
- ~~**Fix #30: fread error check**~~ — `load_config` now checks `fread` return value; handles short reads gracefully
- ~~**Fix #6/#27: Dead code removal**~~ — Removed unused `list_remove_name` (was `[[maybe_unused]]` — never called)

### Phase 4: Code Quality (Polish) — DONE
11. ~~**Replace magic numbers with named constants**~~ — Added `kMaxConfigBytes`, `kScmPollMinMs`, `kScmPollMaxMs`, `kExePollFallbackMs`, `kTaskkillTimeoutMs`; all callsites updated
12. ~~**Consolidate duplicate string normalization**~~ — Added `str_lower_inplace(std::wstring&)` and `str_lower_inplace(wchar_t*)` helpers; replaced all 7 inline `CharLowerBuffW` patterns
13. ~~**Remove dead code and C-ism wrappers**~~ — Removed `dispcache_free` (redundant `.clear()`), `dispcache_push` (trivial map assignment); inlined at call sites. Kept `dispcache_lookup` (has non-trivial lowercasing logic)
14. ~~**Fix build comment, header ordering**~~ — Updated build comment to reference correct filename; moved C headers (`cstdlib`, `ctime`, `cstdio`, `cstdarg`, `cstring`, `cwctype`) to top with organized grouping
15. ~~**Replace `typedef struct` with plain `struct`**~~ — Removed C-style `typedef` from `App`, `NameIdx`, `LogMsg`, `StatusMsg`
16. ~~**Replace `memset` with value initialization**~~ — All 8 `WNDCLASSW`/`WNDCLASSEXW` instances now use `wc{}` instead of `memset(&wc, 0, sizeof(wc))`

---

## Summary Statistics

| Category | Count |
|----------|-------|
| Critical (Security) | 2 |
| High (Correctness) | 4 |
| High (Performance) | 4 |
| Medium (Architecture) | 4 |
| Medium (Threading) | 3 |
| Medium (Code Quality) | 5 |
| Low (Minor) | 8 |
| **Total** | **30** |
