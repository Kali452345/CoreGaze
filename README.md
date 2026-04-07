# CoreGaze

CoreGaze is a lightweight Windows resource-monitor overlay built with C++, Win32, DirectX 11, and Dear ImGui.

## Features
- Always-on-top desktop overlay with low-overhead polling cadence.
- CPU, RAM, GPU, Disk, and Network telemetry.
- Tray-based runtime controls and persisted settings.
- Single-instance guard and crash diagnostics.
- Inno Setup installer with upgrade-safe AppId.

## Requirements
- Windows 10/11 (x64)
- CMake + Ninja (or a compatible CMake generator)
- A C++ toolchain compatible with this project setup
- Inno Setup 6 (`ISCC.exe`) for installer generation

## Build (Local)
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## One-Command Unsigned Release (Build + Installer)
```powershell
./scripts/build-release.ps1
```

This flow:
- Builds the app in release mode.
- Ensures `vc_redist.x64.exe` is present (downloads automatically if missing).
- Bundles the runtime bootstrapper into the installer so users do not need to fetch VC++ runtime manually.
- Produces installer output under `build/installer/`.

## Installer Only
```powershell
./scripts/build-installer.ps1
```

## Per-User Installer (No UAC)
```powershell
./scripts/build-release.ps1 -PerUserInstall
```

## Optional: Skip VC++ Bootstrap Bundling
```powershell
./scripts/build-release.ps1 -SkipVcRedistDownload
```

Use this only when you intentionally do not want runtime bootstrap packaging.

## Repository
- GitHub: https://github.com/Kali452345/CoreGaze
- Issues: https://github.com/Kali452345/CoreGaze/issues
- Releases: https://github.com/Kali452345/CoreGaze/releases
