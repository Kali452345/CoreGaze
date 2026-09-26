# Startup Management

CoreGaze supports a tray toggle for launching at Windows sign-in.

## How It Works
- A tray checkbox entry, `Launch on Windows Startup`, toggles startup behavior.
- State is persisted in config (`[General] StartWithWindows`) and mirrored to HKCU Run.
- Startup scope is current-user only, no elevation required.
- Installer also exposes an optional startup task (`Start CoreGaze automatically when I sign in`) that seeds the same HKCU Run value during install.
- The Run value is `"<path>\CoreGaze.exe" --background`. At sign-in CoreGaze starts with only the overlay and the tray icon; any other start also opens the process window (`docs/process_window.md`).
  - `LoadAppSettings` rewrites the value on every start while the setting is on. An entry written by an older version, without `--background`, is therefore updated the first time the new version runs.

## APIs Used
- `RegOpenKeyExW`
- `RegSetValueExW`
- `RegDeleteValueW`
- `RegQueryValueExW`
- `GetModuleFileNameW`

## Registry Location
- `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`
- Value name: `CoreGaze`

## Integration
- Tray menu wiring lives in `main.cpp` command handling and menu build paths.
- Runtime settings save/load remains centralized through `LoadAppSettings` and `SaveAppSettings`.
- Installer wiring lives in `installer/CoreGaze.iss` (`[Tasks]` + `[Registry]` entries for `CoreGaze`).

## Starting as Administrator
With `Always Run as Administrator` on (`[General] AlwaysElevated=1`), the Run entry still starts CoreGaze without elevation. That instance immediately hands over to the elevated scheduled task `CoreGaze Elevated Startup` (no UAC prompt) and exits. The task instance, started with `--from-task`, then starts elevated. If the task can't be started, CoreGaze continues without elevation. See `docs/elevation.md`.
