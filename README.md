# CoreGaze

CoreGaze is a lightweight Windows resource-monitor overlay built with C++, Win32, DirectX 11, and Dear ImGui.

## Features
- Always-on-top desktop overlay with low-overhead polling cadence.
- CPU, RAM, GPU, Disk, and Network telemetry.
- Tray-based runtime controls and persisted settings.
- Startup management via tray toggle and installer startup checkbox.
- Single-instance guard and crash diagnostics.
- Inno Setup installer with upgrade-safe AppId.

## Requirements
- Windows 10/11 (x64)
- CMake 3.20+
- MinGW-w64 toolchain (for example [w64devkit](https://github.com/skeeto/w64devkit); `build-release.ps1` picks it up automatically from `%LOCALAPPDATA%\w64devkit\bin`)
- Inno Setup 6 (`ISCC.exe`) for installer generation (`build-installer.ps1` offers to install it via winget)

MinGW builds link the C++ runtime statically, so end users do not need the Visual C++ Redistributable.

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
./scripts/build-installer.ps1 -Version 1.0.0
```

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
