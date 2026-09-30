# CoreGaze

CoreGaze is a lightweight Windows resource-monitor overlay built with C++, Win32, DirectX 11, and Dear ImGui.

## Features
- **Always-on-top Desktop HUD**: Low-overhead hardware telemetry overlay with per-monitor v2 DPI awareness, ALT-drag repositioning, and click-through transparency.
- **CPU Telemetry & Power Consumption**: Dynamic GHz, accurate % Processor Time (matching Windows 11 Task Manager), live CPU package wattage via Windows Energy Meter / RAPL, and domain breakdown hover tooltip (Cores, DRAM, Uncore); see `docs/power_metrics.md`.
- **Live Temperature Metrics**: CPU, GPU, and NVMe/SSD storage temperatures in °C or °F with automatic thermal warning/critical color coding; see `docs/temperature_metrics.md`.
- **Task Manager-style Process Window** (tray `Processes...` or `Ctrl+Shift+Alt+P`):
  - Per-process CPU, estimated power draw (Watts), private working set memory, GPU engine usage, and total I/O.
  - Optional elevated kernel ETW session for true per-process Disk read/write and Network send/receive rates.
  - Sorting, instant filtering (Ctrl+F), name grouping with child expansion, adjustable font scaling (Ctrl + Wheel), End task with PID-reuse protection, priority management, and clipboard export. Consumes a tiny fraction of Task Manager's CPU and memory; see `docs/process_window.md`.
- **Multi-Adapter GPU Monitoring**: Targeted, Highest-Load, Multi-GPU (all rows), or Aggregate modes with dedicated VRAM bars.
- **Multi-Drive & Network Identity**: Selected-drive fixed storage monitoring with capacity indicators, and event-driven primary/secondary network adapters with live SSID/cellular labeling.
- **Elevation & Background Startup**: Non-elevated by default; optional UAC restart or seamless `Always Run as Administrator` via an on-demand scheduled task; see `docs/elevation.md`.
- **Runtime Tray Hub & Settings**: Comprehensive context menu, hotkeys (`Ctrl+Shift+Alt+O` for overlay, `Ctrl+Shift+Alt+P` for processes), and persistent INI configuration.
- **Installer & CI**: Inno Setup installer with upgrade-safe AppId, automatic VC++ redist avoidance via MinGW static runtime, and automated GitHub Actions release workflow.

## Requirements
- Windows 10/11 (x64)
- CMake 3.20+
- MinGW-w64 toolchain (for example [w64devkit](https://github.com/skeeto/w64devkit); `build-release.ps1` picks it up automatically from `%LOCALAPPDATA%\w64devkit\bin`)
- Inno Setup 6 (`ISCC.exe`) for installer generation (`build-installer.ps1` offers to install it via winget)

MinGW builds link the C++ runtime statically, so end users do not need the Visual C++ Redistributable.

Dear ImGui is fetched by CMake at configure time, pinned to a release commit (currently v1.92.9b) in `CMakeLists.txt`. To upgrade it, replace the hash with a newer release tag's commit, rebuild, and check the HUD.

## Build (Local)
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## One-Command Unsigned Release (Build + Installer)
```powershell
./scripts/build-release.ps1
```

This builds `build/CoreGaze.exe` in release mode and produces the installer under `build/installer/`.

## Installer Only
```powershell
./scripts/build-installer.ps1
```

The installer takes its version from `build/CoreGaze.exe`. The version itself lives in one place, `COREGAZE_VERSION` in `CMakeLists.txt`; override it per build with `cmake -S . -B build "-DCOREGAZE_VERSION=1.2.3"` (keep the quotes in PowerShell).

During setup, you can enable the installer task "Start CoreGaze automatically when I sign in" to configure startup immediately.

## Per-User Installer (No UAC)
```powershell
./scripts/build-release.ps1 -PerUserInstall
```

## Publishing a GitHub Release
Push a `vX.Y.Z` tag (or run the workflow manually). `.github/workflows/github-release.yml` builds with MinGW on the runner, compiles the installer with the tag's version, and attaches `CoreGaze-Setup-X.Y.Z.exe` to the release.

## Repository
- GitHub: https://github.com/Kali452345/CoreGaze
- Issues: https://github.com/Kali452345/CoreGaze/issues
- Releases: https://github.com/Kali452345/CoreGaze/releases
