# System Tray Control Hub

The tray menu is the runtime control surface for telemetry visibility, sampling cadence, and data-source selection.

## Goals
- Keep controls event-driven (no polling loop in tray code).
- Apply settings live at runtime.
- Persist user choices to `%APPDATA%\CoreGaze\config.ini`.
- Auto-migrate legacy settings from `%APPDATA%\TaskManagerOverlay\config.ini` on first run when the CoreGaze config file is missing.

## Core Controls
- Overlay visibility toggle (`Show Overlay`; double-clicking the tray icon does the same).
- `Reset Overlay Position`: moves the HUD back to the default top-right corner of the work area and clears the saved position.
- Metric visibility toggles (CPU, RAM, GPU, Disk, Network).
- Polling cadence (`500ms`, `1000ms`, `2000ms`).
- Disk drive selection (fixed drives only), plus `Select All` / `Select None`.
- Launch on Windows startup toggle (HKCU Run key).
- `Exit / Quit`.

## GPU Controls
- `Show Dedicated VRAM`: toggles the VRAM bar under each GPU row.
- Display mode submenu:
  - Targeted
  - Highest-Load
  - Multi-GPU
  - Aggregate
- Source submenu:
  - selects adapter index used by Targeted mode

## Network Controls
- Primary adapter submenu:
  - Auto (default-route based)
  - Manual adapter selection by interface index
- Secondary adapter submenu:
  - enable/disable secondary monitoring
  - manual secondary adapter pick
- Network display mode submenu:
  - Primary RX/TX split
  - Primary + Secondary RX/TX
  - (The former `Primary total Mbps` entry was removed because the HUD always renders split bars; a saved value of `0` loads as split.)

## Temperature Controls
- `Temperatures` submenu:
  - Show Temperatures (also stops temperature polling when off)
  - Celsius / Fahrenheit (grayed out while temperatures are hidden)
- See `temperature_metrics.md`.

## Persistence Keys
- `Display`: `VisibleMask`, `OverlayVisible`
- `Polling`: `IntervalMs`
- `Disk`: `SelectedMask`
- `GPU`: `DisplayMode`, `SelectedAdapterIndex`, `ShowVram`
- `Network`: `PrimaryMode`, `PrimaryIfIndex`, `SecondaryEnabled`, `SecondaryIfIndex`, `DisplayMode`
- `Temperature`: `Show`, `Fahrenheit`
- `General`: `StartWithWindows`
- `Window`: `HasSavedPos`, `PosX`, `PosY` (HUD position after an ALT-drag, in physical pixels; written on exit and with any tray change)
- `Version`: `ConfigSchemaVersion` (currently 2; see `dpi_scaling.md` for the 1 -> 2 position migration), `LastLaunchedVersion`

## Startup Integration
- Registry path: `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`
- Registry value: `CoreGaze`
- Tray toggles update config and registry together so startup behavior remains deterministic.

## Runtime Application
`ApplyRuntimeSettings(...)` pushes tray selections into monitor setters immediately. This keeps backend polling and HUD rendering aligned with current menu state without restart.
