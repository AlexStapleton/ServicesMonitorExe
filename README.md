# ServicesMonitorExe

A lightweight Windows desktop app that **keeps chosen Windows services and executables stopped** — and switches behavior automatically based on which programs you're running.

It's a single native `.exe` (no installer, no runtime dependencies) with a single plain-text `.cfg` file created next to it.

---

## What it's for

Some background services and helper executables start themselves repeatedly — on boot, on update, or whenever a parent app launches — even when you don't want them running. ServicesMonitorExe watches a list you define and, for any item with **Auto-Stop** enabled, stops it again whenever it comes back, on a configurable interval.

**Profiles** take this further: a profile activates automatically when a *watched program* is running, swapping in a different set of items and settings, and optionally launching companion apps/files. When the watched program exits, you drop back to your default set. For example:

- *"While `game.exe` is running, stop these telemetry/updater services and launch Discord; otherwise leave them alone."*
- *"Keep `SomeUpdater.exe` and a vendor service stopped at all times, but pause enforcement for 15s after each stop so I'm not fighting a service that legitimately restarts."*

---

## Features

**Monitoring & enforcement**
- Monitors both **Windows services** (via the Service Control Manager) and **executables** (by process name).
- Per-item **Auto-Stop** toggle — when on, a running item is stopped again on the next monitor tick.
- **Cooldown** between repeated stop attempts, and **error backoff** that temporarily suspends Auto-Stop for a service that keeps refusing to stop (prevents tight retry loops).
- Manual **Start / Stop / Remove** actions for any item.
- Executables are terminated via process handles, with a `taskkill /F` fallback.

**Profiles**
- Named configuration snapshots that **auto-activate** when any of their *watched* `.exe` names is detected running (first match wins; falls back to **Default** when none match).
- Each profile carries its own item list, Auto-Stop flags, and timing settings.
- **Start Items**: files, apps, shortcuts, folders, or URLs to shell-open when the profile activates; processes the app launched are closed again when the profile deactivates.

**Interface**
- Two tabs — **Services** and **Executables** — each a sortable, column-persistent list (Auto-Stop, Name, Status, Path, Last Update).
- Per-tab **search/filter** (debounced).
- **Detail panel** showing the selected item's name, live status, Auto-Stop state, and (for services) description, with quick actions.
- **Activity log** pane recording status changes and enforcement actions.
- **System tray** support with minimize-to-tray and close-to-tray.
- **Dark mode** (immersive dark title bar; Mica backdrop on Windows 11 where available).
- **Per-monitor DPI** awareness for crisp scaling across mixed-DPI displays.
- Window placement, panel sizes, column widths/order, active tab, and theme are all persisted.

---

## Requirements

- **Windows 10 or 11** (x64).
- **Administrator privileges.** Stopping most Windows services requires elevation, so the app relaunches itself elevated (UAC) at startup.

---

## Usage

1. Launch `ServicesExeMonitor.exe` (accept the UAC prompt).
2. On the **Services** or **Executables** tab, type a service key name or an executable name (e.g. `wuauserv` or `notepad.exe`) and add it. For executables you can also **Browse…** to a file.
3. Tick **Auto-Stop** on the items you want kept stopped.
4. (Optional) Open **Profiles** to define watched programs, per-profile item sets, and Start Items.
5. Tune timing in **Preferences** (all values in milliseconds):
   - **UI Refresh** — how often the list repaints.
   - **Monitor Interval** — how often statuses are polled / enforcement runs (default `1000`, min `200`).
   - **Autostop Cooldown** — minimum gap between repeated stop attempts on the same item (default `15000`).
   - **Stop Wait** — how long to wait for a stop/terminate to complete (default `10000`).

Settings and your item/profile lists are saved to **`windows_service_monitor.cfg`** next to the executable.

### Keyboard shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+F` | Focus the search box |
| `Ctrl+1` | Switch to the Services tab |
| `Ctrl+2` | Switch to the Executables tab |
| `Ctrl+L` | Focus the main list |
| `Ctrl+S` | Save configuration now |
| `Ctrl+D` | Toggle dark mode |
| `Ctrl+G` | Toggle the activity log pane |
| `F5`     | Refresh the view |

---

## Building

The project builds with the **MinGW-w64 GCC** toolchain (C++20). The easiest setup is [MSYS2](https://www.msys2.org/) using the **UCRT64** environment:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc make
```

Then, from the repository root:

```bash
make
```

This compiles the sources in `src/`, builds the resource object from `app.rc` (icon + manifest) with `windres`, and links **`ServicesExeMonitor.exe`** into the repository root. Useful targets:

```bash
make syntax   # compile-only check (no link)
make clean    # remove build output
```

The build is fully static (`-static`), so the resulting `.exe` has no external runtime dependencies.

---

## Project layout

```
src/
  main.cpp         Entry point, window procedure & message handlers, accelerators
  monitor.cpp      Monitor thread: status polling + Auto-Stop enforcement
  action.cpp       Action worker thread: start/stop/terminate operations
  config.cpp       Config + profile load/save (atomic, versioned)
  profiles.cpp     Profile activation / watched-exe matching
  ui_layout.cpp    Window layout, splitters, resizing
  ui_listview.cpp  List views, rows, filtering
  ui_dialogs.cpp   Preferences / Profiles / Hotkeys / Diagnostics dialogs
  theme.cpp        Theme palette & control theming
  darkmode.cpp     Dark-mode plumbing
  tray.cpp         System tray icon
  app.cpp          App-wide helpers
  util.cpp/.h      Small shared utilities
  app.h            Core data model & shared declarations
app.rc / app.ico / app.manifest   Resources
Makefile           Build
```

### How it works (brief)

- A **monitor thread** wakes on the monitor interval, takes a single process snapshot per tick (skipped entirely when there's nothing to watch), queries service states, decides which Auto-Stop items need enforcing, and posts UI updates.
- An **action thread** performs the actual start/stop/terminate work off the UI thread, so the interface stays responsive.
- The **UI** is event-driven: only rows whose status actually changed are repainted.
- Configuration saves are **atomic** (temp file → fsync → rename) and carry a version marker.

---

## License

Licensed under the **GNU Affero General Public License v3.0** — see [LICENSE](LICENSE).
