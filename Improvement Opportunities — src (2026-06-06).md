# Improvement Opportunities — `src/` tree

**Scope:** The modular `src/` build (`main.cpp`, `monitor.cpp`, `action.cpp`, `config.cpp`, `profiles.cpp`, `theme.cpp`, `darkmode.cpp`, `tray.cpp`, `ui_*.cpp`, `app.h`, `util.*`), built via the `Makefile` → `ServicesExeMonitor.exe`. As of 2026-06-06 this is the **canonical source of truth**; the single-file `ServicesExeMonitor_Claude_v1.cpp` / `_v2.cpp` monoliths are now secondary.

**Date:** 2026-06-06
**Supersedes:** the analysis in `Improvement Opportunities.md` (2026-06-02), which was written against the v1 monolith and is now partly stale (see "Already fixed" below). Older docs (`ANALYSIS.md`, `PERFORMANCE_ANALYSIS.md`, `FINAL_ANALYSIS.md`, `Code_Review_Claude_v1.md`) describe still-older revisions.

---

## Progress checklist (updated 2026-06-06)

Implemented this round (each its own git commit; build verified clean via `make` after every change):

- [x] **5.1** — `git init` + `.gitignore` + initial commit; deleted `Archive - Old Versions/` (195 files) and `Windows 10 Versions/` (recoverable from the initial commit).
- [x] **4.1** — monolith copies retired (already archived by the author; `src/` is the sole source).
- [x] **5.2** — consolidated analysis docs down to this one living file (retired stale `ANALYSIS.md`).
- [x] **2.1** — UTF-8 BOM stripped on config load (`config.cpp`).
- [x] **2.2** — `config_version` marker emitted + read, with newer-than-current detection (`config.cpp`).
- [x] **2.3** — config row writes are now truncation-proof via `write_wfmt` (the data-loss path). *Log-message truncation (cosmetic) deliberately left.*
- [x] **1.1** — `post_status_bulk` resolves items by stable `uid` (O(1)) instead of per-tick name rehash.

Open / not started:

- [ ] **3.1** — elevation model (MEDIUM) — **needs a product decision** (see item).
- [ ] **4.2** — group loose `App` UI/window members into sub-structs (MEDIUM, no functional change; large churn, no tests).
- [ ] **4.3** — split the large `AppWndProc` switch (MEDIUM, behavior-preserving; regression risk without runtime tests).
- [ ] **4.4** — standardize on `ItemKind` over the dual `int`/enum representation (LOW).
- [ ] **1.2** — avoid per-tick `auto`/`lastreq` copy under the model lock (LOW; only matters at large item counts).
- [ ] **2.2b** — config integrity beyond the version line (optional checksum) — not pursued.

---

## Already fixed since the last review (verified in `src/`)

These were flagged previously and are **done** — listed so they are not re-investigated:

- **Monitor CV "wake early" was a no-op** *(was [HIGH])* — `ThreadState` now carries a `mon_wake` flag (`app.h:474`); `sleep_ms_cooperative` includes it in the wait predicate and consumes it (`monitor.cpp:246–252`); writers set it under the lock before `notify_one()` (`main.cpp:62`, `profiles.cpp:147`, `config.cpp:807`). Config/list/profile changes now wake the monitor promptly.
- **`snprintf` header length used unclamped** *(was [LOW])* — `save_config_now` now checks `if (n < 0 || (size_t)n >= sizeof(header))` and aborts safely (`config.cpp:932–937`).
- **Config save failed silently** *(was [LOW])* — every failure path now logs via `log_linef` (fopen, header overflow, write error, rename), and the save is atomic: temp file → `fflush` → `_commit` (fsync) → `fclose` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` (`config.cpp:848–852`, `1021–1038`). This is now genuinely robust.
- **Per-tick process-table snapshot taken even when nothing consumes it** *(perf)* — `build_running_exe_hashes_lower` gained a `collect_hashes` flag and early-returns before `CreateToolhelp32Snapshot` when there are no EXE rows and no profile watch keys; the monitor loop computes `need_hashes` from `prof.watch_hashes_sp` and passes it down (`monitor.cpp:124`, `351`, `982`). A services-only / no-profile config now idles with no per-second snapshot.
- **Redundant per-process string copy + lowercase in process enumeration** *(perf)* — `process_count_by_name_lower` (`monitor.cpp:915`) and `terminate_all_by_name_lower` (`monitor.cpp:1025`) now compare `pe.szExeFile` directly with `_wcsicmp`, no per-iteration buffer.

---

## Overall assessment

This is **mature, carefully engineered code**. The threading model (cooperative `std::jthread` workers, a documented and debug-enforced lock order via `ModelLockGuard`/`ModelReadGuard`, generation-counter-driven event UI refresh), the RAII Win32 handle wrappers, and the hardened config I/O are all solid and should be preserved through any refactor. The steady-state hot paths (monitor tick, action thread, UI status apply) have no obvious remaining waste.

The opportunities below are therefore **refinements, a few latent robustness gaps, and structural/repo-hygiene work** — not foundational defects. Items are tagged with severity and a concrete location.

---

## 1. Performance (remaining)

### 1.1 [LOW] `post_status_bulk` re-hashes every monitored name every tick
**Where:** `main.cpp:32–34`.

```cpp
for (size_t i = 0; i < n; i++) {
    Item* it = list_find(&self.items[k], names[i].c_str());   // FNV-1a + map lookup, per item, per tick
    ...
}
```

The monitor already knows the positional `Item*` for each name (it built `snap` from the same vector). Re-resolving every name by hash on every tick is redundant. Negligible at a handful of items; an avoidable O(n) hashing pass under the model lock once you monitor hundreds.

**Fix:** pass the resolved `Item*` (or stable `uid`) array from the monitor snapshot into the status-apply path instead of re-looking-up by name. Mildly invasive (touches the monitor→UI status contract), which is why it was deferred.

### 1.2 [LOW] Monitor copies all names/auto/lastreq under the model lock every tick
**Where:** `monitor.cpp` snapshot block (the per-item loops that fill `snap.svc_auto` / `snap.exe_auto` / `*_lastreq`, ~`320–344`).

Names are only re-copied when the list generation changes (good), but `auto_stop` and `last_autostop_mono_ms` are copied for every item under `ModelLockGuard` each tick. Fine at realistic counts; would become lock-contention pressure at large scale. Low priority — only worth it if 1.1 is also done and item counts are expected to be large.

---

## 2. Correctness & robustness (remaining)

### 2.1 [LOW] No UTF-8 BOM handling on config load
**Where:** `config.cpp:441–468`.

The loader reads the file as UTF-8 bytes and converts with `MultiByteToWideChar(CP_UTF8, …)` with **no BOM strip**. The app's own save path writes no BOM, so round-tripping is fine — but if a user opens the `.cfg` in an editor that adds a BOM (Notepad, some VS Code configs), the bytes `EF BB BF` become a leading `﻿` on the first token, so the first line (`SETTING|ui_refresh_ms|…`) silently fails to parse.

**Fix:** after the `MultiByteToWideChar` conversion, skip a leading `L'﻿'` before tokenizing (or strip `EF BB BF` from the byte buffer first).

### 2.2 [LOW] No config version / integrity marker
**Where:** save header (`config.cpp:892+`), load (`config.cpp:466+`). No `VERSION` field is emitted.

The format is line-oriented and parsed best-effort, so a truncated or hand-edited file is partially applied with no signal. A leading `SETTING|config_version|N` (or a dedicated `VERSION|N` line) would enable future migrations and let the loader detect/repair partial files. The atomic save (already in place) protects against torn writes; this protects against *format* drift.

### 2.3 [LOW] Fixed-size formatting buffers truncate long names/paths silently
**Where:** throughout the log/format paths — e.g. `wchar_t wline[700]` in `config.cpp` (`953`, `985`, `993–1012`), the `msg`/`reason`/`line` buffers in `monitor.cpp` and `action.cpp`.

`StringCchPrintfW` truncates *safely* (no overflow), but a service with a long display name or a long launch path yields a cut-off config line or log entry. For config rows this can mean **data loss on save** (a path longer than the buffer is silently truncated and then persisted truncated). Prefer `std::wstring`-based formatting for the config writer specifically; log truncation is cosmetic and lower priority.

---

## 3. Security / UX posture

### 3.1 [MEDIUM] Whole-app forced elevation
**Where:** `ensure_admin_or_exit()` called early in startup (`main.cpp:1513`).

The app relaunches elevated unconditionally because *stopping services* requires admin. Users who only monitor **EXEs** (no elevation needed) are still pushed through UAC on every launch, and the whole process — including all UI, config parsing, and the tray — runs at high integrity, widening the attack surface (e.g. the UIPI message-filter shims exist precisely because of this).

**Options:**
- Defer elevation until a privileged action (service stop) is actually attempted, relaunching elevated on demand.
- Or run unelevated with service-stop disabled and a clear in-UI "Elevate" affordance.

This is a design change, not a quick fix, but it's the most user-visible item here.

---

## 4. Architecture & maintainability

### 4.1 [HIGH→resolved-in-principle] Retire the monolith copies now that `src/` is canonical
The decision to make `src/` the source of truth removes the prior "two divergent copies will drift" risk — **as long as the monoliths stop being hand-edited.** Remaining action: delete or clearly archive `ServicesExeMonitor_Claude_v1.cpp`, `ServicesExeMonitor_Claude_v2.cpp`, and the standalone single-file build command/`app_res.o`, so there is exactly one place to edit. Update `How to Build.txt` to point only at the `Makefile`.

### 4.2 [MEDIUM] `App` is still a large aggregate
**Where:** `struct App` in `app.h`.

`ThemeState`, `ProfileState`, `TrayState`, `ConfigSettings`, and `ThreadState` are already extracted (good). A cluster of loose UI/window members remains — control HWNDs, per-tab search/edit/info handles, column-persistence arrays, splitter-drag state, initial-layout flags. Grouping these into a `UiState`/`LayoutState` sub-struct would clarify which fields are UI-thread-only (relevant to the lock-free reads in `save_config_now`).

### 4.3 [MEDIUM] `AppWndProc` is a very large switch
**Where:** `main.cpp` (window procedure handling `WM_CREATE`, layout, all `WM_APP_*`, tray callbacks, commands, destroy/cleanup).

Extract per-message handlers (`on_create`, `on_command`, `on_tray`, `on_destroy`, …) returning `LRESULT`. Beyond readability, this makes the `WM_DESTROY` teardown ordering (threads → drain queued log messages → free items → fonts → theme) auditable in isolation.

### 4.4 [LOW] Dual kind representation: `KIND_SVC/KIND_EXE` (int) vs `enum class ItemKind`
**Where:** bridged everywhere via `ki(kind)`.

Arrays are indexed with `ki(kind)` while control flow switches on `ItemKind`. Carrying both and converting at each boundary is a recurring wrong-index risk. Consider standardizing on `ItemKind` with one `to_index()` helper. Relatedly, the svc/exe auto-stop enforcement blocks in `monitor.cpp` are near-duplicates that could collapse into one templated/parameterized helper keyed on `ItemKind`.

---

## 5. Repository hygiene

### 5.1 [MEDIUM] No VCS; ~190 hand-saved snapshots in `Old Versions/`
The project is **not a git repository**, yet `Old Versions/` (plus `Windows 10 Versions/`) holds ~190 `ServicesExeMonitor_vNN.cpp` copies. This is version control by file copy: it bloats the tree, pollutes `grep`/search, and makes "what changed between vN and vN+1" expensive.

**Recommendation:** `git init`, commit the current `src/` state, then delete the manual snapshot folders (history lives in git). This also makes future reviews diff-able and lets you drop the redundant `.md` analyses into a single living doc.

### 5.2 [LOW] Consolidate the analysis docs
There are now five overlapping `.md` reviews (`ANALYSIS.md`, `PERFORMANCE_ANALYSIS.md`, `FINAL_ANALYSIS.md`, `Code_Review_Claude_v1.md`, `Improvement Opportunities.md`) plus this one, several written against superseded revisions. Once git is in place, keep one living document and delete the stale ones.

---

## 6. Not yet reviewed (gaps in this analysis)

For honesty about coverage — these areas were **not** examined in depth and may hold their own opportunities:

- **Startup / layout / DPI path** (`ui_layout.cpp`, the initial-layout stabilization logic) — not in the hot loop, so low perf impact, but un-audited for correctness.
- **`ui_dialogs.cpp`** (~103 KB) — the largest TU; preferences/profiles dialog logic largely unread here.
- **Theme/dark-mode subclassing** (`theme.cpp`, `darkmode.cpp`) — GDI brush lifetimes and subclass cleanup not audited.
- **Concurrency correctness** beyond the documented lock order — e.g. the lifetime of `Item*` pointers handed between threads, and the started-process tracking on profile switch (`profiles.cpp`).

A focused correctness/robustness pass on the monitor + action + profiles threads, and a read of `ui_dialogs.cpp`, would be the natural next investigations.

---

## Suggested order of attack

1. **5.1 — `git init` + delete `Old Versions/`** and the monolith copies (4.1). Stops drift, makes everything after this measurable and diff-able. Highest leverage, lowest code risk.
2. **2.1 / 2.2 / 2.3 (config row buffer) — cheap robustness wins.** BOM strip, a version marker, and `std::wstring` formatting for the config writer (the one place truncation = data loss).
3. **3.1 — decide the elevation model.** User-visible; worth a deliberate decision even if the implementation lands later.
4. **4.2 / 4.3 — structural refactors** (`App` grouping, `AppWndProc` split) within the now-canonical `src/` tree.
5. **1.1 / 1.2 — the deferred perf items**, only if large item counts are expected.
6. **6 — open an audit** of `ui_dialogs.cpp` and the profiles/started-process lifetime.

## What is already good (do not regress)
- The debug-enforced lock ordering (`ModelLockGuard` / `ModelReadGuard` + `dbg_assert_model_unlocked`) — backbone of thread safety.
- Atomic, fsync'd, logged config save (`config.cpp`).
- RAII Win32 handle wrappers (`unique_handle`, `unique_sc_handle`).
- `escape_argv` + `CreateProcessW`-based termination (closed real injection holes).
- Generation-counter event-driven UI refresh and the single-snapshot-per-tick monitor design (now gated to skip the snapshot entirely when idle).
