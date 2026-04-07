# Startup Management

CoreGaze supports a tray toggle for launching at Windows sign-in.

## How It Works
- A tray checkbox entry, `Launch on Windows Startup`, toggles startup behavior.
- State is persisted in config (`[General] StartWithWindows`) and mirrored to HKCU Run.
- Startup scope is current-user only, no elevation required.

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
