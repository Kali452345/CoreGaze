# Process Window (Task Manager-Style Process List)

`ProcessWindow` (`ProcessWindow.h` / `ProcessWindow.cpp`) is CoreGaze's process list: a normal, resizable top-level window with a sortable, filterable table of every process. It is built to cost a fraction of Task Manager.

## Opening It
- Tray menu: **Processes...** (the first and default item).
- Hotkey: **Ctrl+Shift+Alt+P**. Ctrl+Shift+P alone was avoided because editors and browsers use it.
- Command line: `CoreGaze.exe --open-processes`. "Restart as administrator" uses this flag to reopen the window.

If the window is already open, these bring it to the front. Closing the window only hides the list; CoreGaze keeps running in the tray.

## Cost Model
Nothing exists until the window opens. Closing it frees everything:
- the window
- its swap chain
- its ImGui context and font atlas
- the `ProcessMonitor` buffers
- the per-process GPU tracking
- the ETW session

While it is open:
- **Sampling** follows the update speed (0.5 s, 1 s, 2 s or Paused). Each sample is one `ProcessMonitor::Sample()` call, plus the per-process GPU usage from `SystemMonitor`'s existing GPU engine poll, plus the ETW counters when elevated.
- **Drawing happens only when needed:**
  - after a sample;
  - after input, for three frames, capped at 60 fps;
  - once more about 0.7 s after input stops, so hover tooltips appear.
  - An idle window therefore draws once per refresh.
- **Minimized:** nothing is sampled or drawn.
- **Display off or workstation locked:** the main loop suspends everything, as it does for the HUD.

Measured on the development machine (i5-8350U, 8 logical processors, about 380 processes, 1 s refresh, idle window):

| | CPU (share of the whole machine) | Private memory |
|---|---|---|
| CoreGaze, HUD only | 0.12% | 20 MB |
| CoreGaze, HUD + process window | 0.15% | 22 MB |
| Task Manager (Processes page, same moment) | 1.69% | 115 MB |

## Rendering
The window shares the HUD's D3D11 device and has its own:
- `IDXGISwapChain1`, created with `FLIP_DISCARD` (`FLIP_SEQUENTIAL` on Windows 8.x);
- `ImGuiContext`, with its own Win32 and DX11 backend instances.

Every entry point (`Tick`/`RenderFrame` and the window procedure) switches to that context and restores the previous one. The HUD's context is therefore current everywhere else. `Present(0, 0)` is used because `Tick` already caps the frame rate and the HUD's loop must not block on vsync.

While the user drags the window border, Windows runs its own modal loop and the main loop doesn't run. `WM_SIZE` therefore resizes the swap chain and draws a frame directly.

The window follows per-monitor DPI (`WM_DPICHANGED`): the style is scaled with `ScaleAllSizes`, and the 15 px Segoe UI font is rasterized at the monitor's scale through `FontScaleDpi`.

## Table
| Column | Meaning |
|---|---|
| Name | Image name. With **Group by name**, processes sharing a name collapse into one row, e.g. `msedge.exe (49)`, with summed values; expand it with the arrow, a double-click, the Right key or Enter. |
| PID | Process id (blank on group rows) |
| CPU | Share of all logical processors, computed like Task Manager |
| Memory | Private working set (Task Manager's Memory column) |
| Disk | Disk read + write (administrator only, from ETW; hover for the read/write split) |
| Network | TCP + UDP send + receive in bits/s (administrator only, from ETW; hover for the split) |
| GPU | Busiest GPU engine of the process |
| GPU engine | That engine's type (3D, Copy, Video Decode, ...) |
| I/O | All I/O bytes/s: files, pipes, devices and network. Available without administrator rights. |
| Threads, Handles, Working set, Commit, Session, Parent PID | Hidden by default; right-click a header to show them |

- **Sorting:** click a header (numeric columns sort descending first). Group rows are sorted by their summed value, and the processes inside a group by the same column.
- **Layout:** columns can be resized, reordered and hidden. ImGui saves the layout and sort order to `%APPDATA%\CoreGaze\processes_table.ini`.
- **Totals row:** pinned under the header. It shows the process count (or "N of M" while filtering), total CPU, memory load, total GPU and total I/O; with administrator rights it also shows total disk and network.
- **Heat tint:** CPU, Memory, Disk, Network, GPU and I/O cells are tinted amber in proportion to the load, as in Task Manager. Zero values are dimmed.
- **Rendering:** rows are virtualized with `ImGuiListClipper`, so only visible rows are drawn.

## Filtering and Keyboard
- The filter box (**Ctrl+F**) matches name substrings case-insensitively. If it is all digits, it also matches PIDs containing those digits. **Esc** clears it.
- **Up/Down/Page Up/Page Down/Home/End** move the selection. **Left/Right** collapse/expand a group; Left on a member jumps to its group.
- **Delete** opens End task; **Ctrl+C** copies name and PID (every member for a group); **F5** refreshes (useful while Paused).
- The selection follows the process (by PID) or group (by name) across refreshes and re-sorts.

## Actions
Right-click a row, or use the toolbar's **End task** button:
- **End task:** a confirmation dialog names the target: one process, or all members of a group.
  - Each target is re-opened only if its creation time still matches the one captured at click time, so a reused PID is never ended.
  - Never ended:
    - PIDs 0 and 4;
    - CoreGaze itself (use Exit);
    - processes that `IsProcessCritical` reports as critical (ending csrss, wininit or similar would crash Windows).
  - The status bar reports how many were ended, denied, critical or already gone.
- **Set priority:** High, Above normal, Normal, Below normal or Low. For a single process, the current class is checked.
- **Open file location:** runs `explorer /select,` on the image path.
- **Properties:** opens the shell properties sheet for the image.
- **Copy:** name and PID, tab-separated.
- **Expand/Collapse:** for groups.

Without administrator rights, elevated and service processes can't be ended, reprioritized or located. The status bar suggests **Restart as administrator** when that happens.

## Administrator Mode
When CoreGaze is not elevated, the toolbar's right side shows a **Restart as administrator** button. It posts `WM_COREGAZE_RESTART_ELEVATED` to the HUD window, which relaunches CoreGaze through UAC with `--open-processes` (see `docs/elevation.md`).

When elevated, opening the window starts the ETW kernel session that feeds the Disk and Network columns (see `docs/etw_disk_network.md`). Closing the window stops it. The toolbar shows "Administrator: disk and network on". If the session can't start, it shows the error code.

## Settings
Stored in `config.ini` under `[Processes]`:

| Key | Meaning |
|---|---|
| `RefreshMs` | 500, 1000, 2000, or 0 (Paused) |
| `GroupByName` | 1 = group processes by name |
| `AlwaysOnTop` | 1 = keep the window above others |
| `WindowLeft/Top/Right/Bottom` | Normal (restored) placement, in workspace coordinates |
| `Maximized` | 1 = open maximized |

A saved placement that is no longer on any monitor is ignored, and the window opens centered on the primary monitor at 1000x640 (at 100% scaling).

## Main Loop Integration
- `ProcessWindow::Tick()` runs every main-loop iteration while the window is open. It samples and draws when due, and returns how long the loop may sleep.
- The loop now waits until the earliest of: the next HUD poll, the process window's next deadline, or a message.
- `SystemMonitor::Update()` returns whether it polled. The HUD redraws only after a poll or when something visible changed (settings, display change, resume, ALT held or dragging, a size change). Otherwise every wake-up for the process window would also redraw the HUD, with a vsync-blocking `Present`.
