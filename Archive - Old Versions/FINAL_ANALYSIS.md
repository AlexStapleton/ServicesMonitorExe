# Final Deep Analysis — ServicesMonitor C++ Rewrite

**Date:** 2026-04-04
**Scope:** Full codebase review (~11,200 LOC across 15 source files in `src/`)
**Focus:** Performance, Security, Features / Quality

This document is a consolidated, prioritized list of remaining issues found during a final sweep. Citations are `file:line` against the current tree. Treat any line numbers as approximate anchors — verify before editing.

---

## 1. Performance

### High

1. **Linear ListView name lookup in hot path** — `ui_listview.cpp:93-101`
   `lv_find_item_by_name()` walks every row calling `ListView_GetItemText` + `_wcsicmp`. It is invoked during insertion/update paths, which makes batch refresh effectively O(n²). Prefer `ListView_FindItem` with `LVFI_PARAM` (as already used by `lv_find_item_by_uid`) or maintain a name→row cache.

2. **Per-iteration stack buffer in process termination loop** — `monitor.cpp:1027-1047`
   `terminate_all_by_name_lower()` allocates `wchar_t tmp[MAX_PATH]` *inside* the `do { … } while(Process32NextW)` loop. Hoist it outside — trivial fix, removes per-iteration stack churn.

3. **Full-control invalidation after sort / autostop toggle** — `ui_listview.cpp:566, 652`
   `InvalidateRect(lv, NULL, FALSE)` repaints the entire ListView. Use `ListView_RedrawItems(lv, first, last)` for the affected row range (the pattern used correctly in `lv_update_row_existing_with` around line 299).

4. **Custom-draw hot path re-parses status string per cell** — `theme.cpp:137-145`
   Per-subitem `_wcsicmp(it->last_status, L"running" / L"stopped")` runs on every paint. Cache a small status enum on `Item` when status is updated and branch on that integer instead.

5. **O(n·m) name matching in process hash build** — `monitor.cpp:995-1000`
   `build_running_exe_hashes_lower()` binary-searches the sorted monitored-name index for every running process. Invert the loop: for each *monitored* exe, locate the range in the already-sorted process list once.

### Medium

6. **Two `post_status_bulk` posts per tick** — `monitor.cpp:472, 547`
   Services and EXEs produce separate bulk posts per monitor tick. Merge into one payload per tick to reduce message-queue pressure during churn.

7. **`GetSysColor` called per cell in light mode** — `theme.cpp:111-112`
   Dark mode caches palette colors in `ThemeState`; light mode path queries `GetSysColor(COLOR_*)` per cell. Cache the light palette at theme-update time symmetrically.

8. **Redundant `fmt_time_local` on unchanged rows** — `ui_listview.cpp:225, 285`
   Formatting runs even when `wall_ts` hasn't changed since the last UI apply. Gate the `wcsftime`/`localtime_s` call behind the wall-ts delta check that already exists upstream.

9. **Sort comparator re-derives keys each comparison** — `ui_listview.cpp:526-554`
   `lv_sort_view()` calls `sort_key_for_item()` inside the comparator. Build a `{uid, key_ptr}` array once, sort that, then rebuild `view_uids`.

10. **Unconditional vector resizes per tick** — `monitor.cpp:231-241`
    `mons_ensure_sizes()` calls `resize()` unconditionally. Guard with a size-equality check so the allocator path isn't touched in steady state.

### Low

11. **Duplicate `lv_set_columns` implementations** — `ui_listview.cpp:6-74` — code duplication (maintenance, not speed).
12. **String trim via `erase`/`pop_back` in normalizer** — `monitor.cpp:600-622` — switch to `find_first_not_of` / `substr`.
13. **UI refresh timer cadence** — `app.h:70` — confirm `TIMER_UI_REFRESH` isn't ticking faster than the eye can see; bump to ≥250ms if so.
14. **Verify `theme_release_brushes` ordering** — `theme.cpp:195-237` — RAII looks correct; worth a single read-through to confirm no leak on repeated dark-mode toggles.

---

## 2. Security

### High

1. **TOCTOU between `PathFileExistsW` and use** — `config.cpp:269-270`, `monitor.cpp:1279-1280`
   Classic check-then-use on attacker-influenced paths. Replace the existence probe with the actual `CreateFileW` / `CreateProcessW` call and treat failure as "not found."

2. **`ExpandEnvironmentStringsW` silent truncation fallback** — `config.cpp:203`, `monitor.cpp:616`
   If expansion exceeds 1024 wchars, code falls back to the *unexpanded* original string. A crafted env var can bypass any subsequent path validation that assumed expansion happened. Detect `got > bufsize`, grow once, and reject on second failure.

3. **Command-line composition for `taskkill.exe`** — `action.cpp:169`
   `StringCchPrintfW(cmdline, 512, L"taskkill.exe /IM \"%s\" /F", a.name.c_str())` embeds a user-configurable name into a command line. Quoting stops typical spaces but not embedded quotes. Prefer direct `TerminateProcess` on PIDs you already enumerated, or build the argv with `CreateProcessW` and escape per `CommandLineToArgvW` rules. Also bump the buffer — 512 wchars is tight for `MAX_PATH`-class names plus formatting.

4. **`SearchPathW(NULL, …)` current-directory search** — `config.cpp:279`, `monitor.cpp:1284, 1290`
   `SearchPathW` with a NULL path searches the current directory first. Call `SetDllDirectoryW(L"")` early in startup and prefer explicit search roots (e.g. `GetSystemDirectoryW`) for exe resolution.

### Medium

5. **`LoadLibraryW` without full path for system DLLs** — `darkmode.cpp:36`, `util.cpp:15-17, 128`
   `uxtheme.dll`, `dwmapi.dll`, `user32.dll` load by name. Use `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` to defeat planting.

6. **Config deserialization has minimal length/charset validation** — `config.cpp:393-415`
   Whole-file read + line parse with no explicit caps on service/exe/profile name lengths, no UTF-8 validation. Enforce per-field caps (e.g. 260 chars) and reject non-UTF8 lines rather than storing them into UI state.

7. **Profile name not validated for path-traversal characters** — `ui_dialogs.cpp:1807`
   Empty-only check. If profile names ever feed into filenames or log paths in the future, `..\..\` becomes a problem. Whitelist `[A-Za-z0-9 _-]{1,64}` now while it's cheap.

8. **Backoff shift / deadline underflow** — `action.cpp:102-105`, `monitor.cpp:1177-1178`
   Clamp is present, but `remain = deadline - now_mono_ms()` can underflow after clock jitter before the subsequent `DWORD` cast. Add an explicit `if (now >= deadline) break;` before subtraction.

9. **`ShellExecuteW` return comparison** — `monitor.cpp:1305`, `util.cpp:419`
   `(INT_PTR)h <= 32`. Functionally fine on Windows, but MSDN's canonical form is `> 32`; or just migrate to `ShellExecuteExW` which returns BOOL + `hProcess`.

10. **Taskkill buffer sizing** — `action.cpp:127, 169`
    512-wchar buffer for `taskkill.exe /IM "<name>" /F`. `StringCchPrintfW` truncates safely, but the truncation path silently produces a malformed command. Either size for `MAX_PATH + fixed` or remove the taskkill path entirely (see #3).

### Low

11. **Registry handle leak at startup** — `main.cpp:770-777`
    `RegOpenKeyExW` succeeds, `RegQueryValueExW` runs, `RegCloseKey` missing on the success path. One-shot leak, but cheap to fix.

12. **Unvalidated `GetWindowTextW` buffer** — `main.cpp:137`
    Fixed 512-wchar read. Use `GetWindowTextLengthW` first, or cap explicitly so the reject/truncate path is intentional.

13. **NULL/empty service name assumption** — `monitor.cpp:1117, 1197`
    `OpenServiceW(..., svc_name, ...)` trusts upstream. Add a single guard at function entry so empty-name rows can't reach SCM.

14. **Format surface in log lines** — `action.cpp:44-45`
    Format string is constant and safe today; only noted because the three `%s` substitutions come from user-controllable strings. Keep format literals hard-coded.

---

## 3. Features & Code Quality

### High

1. **Accessibility is unimplemented**
   No `WM_GETOBJECT` / UIA provider. ListView has no `LVS_EX_INFOTIP`; search boxes lack mnemonics; detail pane isn't reachable by tab order. Minimum viable fix: label every control, set mnemonics, expose status column values via accessible name.

2. **High-contrast mode not detected** — `main.cpp`, `theme.cpp`
   Dark mode auto-syncs on `WM_SETTINGCHANGE("ImmersiveColorSet")` but ignores `SPI_GETHIGHCONTRAST`. Add a branch that forces system colors when high contrast is active.

3. **Config save is not crash-safe** — `config.cpp` save path
   Current flow writes then renames, but there's no `FlushFileBuffers` before the rename and no cleanup of stranded `.tmp` files on startup. Add `FlushFileBuffers` and sweep leftover temps during load. A CRC/size footer would also let `load_config` reject truncated files instead of importing partials.

4. **Modeless "modal" dialogs allow background mutation** — `ui_dialogs.cpp`
   Preferences / Profiles / Diagnostics run modeless and don't disable the owner window. User can edit main-window state concurrently. Either make them true modal (`DialogBoxParam`) or `EnableWindow(main, FALSE)` for the lifetime.

5. **Tray resilience after Explorer restart** — `tray.cpp`
   `WM_TASKBARCREATED` is registered, but the handler should unconditionally re-add the icon and retry `NIM_MODIFY` with fallback to `NIM_ADD` when the shell returns failure.

### Medium

6. **ListView column click does not sort** — `ui_listview.cpp`, notify handler in `main.cpp`
   Headers are visible but there's no `LVN_COLUMNCLICK` sort. With `lv_sort_view` already present, the missing piece is the notify hookup + ascending/descending toggle state.

7. **No progress feedback for slow operations** — `monitor.cpp`, `action.cpp`
   Service enumeration and start/stop waits can take hundreds of ms. Post interim status ("Enumerating…", "Waiting for stop…") and flip cursor to `IDC_WAIT` while in a synchronous UI-initiated op.

8. **Pending service states hidden** — `monitor.cpp:8-39` and status display path
   `SERVICE_START_PENDING`/`STOP_PENDING` collapse to "Running"/"Stopped". Surface them as "Starting…"/"Stopping…" so users can see transitions.

9. **Hardcoded English strings everywhere** — every `.cpp`
   No string table, no RC resource load. Future localization would require extracting all `L"…"` literals used in UI. Even without localization, centralizing them eases tone consistency.

10. **Activity log silently drops oldest entries** — `app.h:74` (`MAX_LOG_LINES=1000`)
    No visible marker when the ring buffer wraps. Add either an "Export log…" entry or a small "+N older discarded" indicator.

11. **Config save debounce can drop on fast exit** — `main.cpp` WM_DESTROY
    If the user toggles a setting and closes immediately, the 1s debounce may not fire. Force a synchronous `save_config_now` on `WM_CLOSE`/`WM_DESTROY` when `save_pending` is set.

### Low

12. **HICON loaded at startup, never freed** — `tray.cpp:4-11`
    Process-exit cleanup makes it harmless, but add a `DestroyIcon` in `WM_DESTROY` for cleanliness.
13. **No drag-and-drop of exe paths** — `main.cpp` WndProc — handy UX add via `DragAcceptFiles` + `WM_DROPFILES`.
14. **Tooltips only on toolbar** — `ui_layout.cpp:436-445` — no column-header or status tooltips.
15. **ListView column widths don't rescale on monitor move** — `ui_layout.cpp` — fonts handle DPI but column pixel widths do not.
16. **`IDM_RESET_COLUMNS` declared but not wired up** — complete the handler to restore default widths/order.

---

## Suggested Order of Attack

1. **Security quick wins** (1h): DLL-search hardening (#5), TOCTOU replacements (#1), registry handle leak (#11), profile-name whitelist (#7).
2. **Perf quick wins** (1h): hoist `tmp[MAX_PATH]` (#2), targeted `ListView_RedrawItems` (#3), cached status enum (#4), light-mode palette cache (#7).
3. **Crash-safe config save + startup temp sweep** (#3 Features).
4. **Taskkill replacement with direct `TerminateProcess`** (#3 Security) — kills two issues (injection + buffer sizing).
5. **ListView column sort wiring** (#6 Features) — small, user-visible win.
6. **High-contrast + modal-dialog correctness** (#2, #4 Features).
7. **Accessibility pass** — larger effort, schedule separately.

---

## Notes

- Several items were previously tracked in `ANALYSIS.md` / `PERFORMANCE_ANALYSIS.md`; this pass is the delta still outstanding against the current tree.
- Nothing found rises to the level of a critical production blocker — the earlier refactor phases (command-injection removal, RAII wrappers, process-snapshot batching, modularization) have already handled the worst offenders. Remaining items are hardening, polish, and accessibility.
