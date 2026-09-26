# Installer And Upgrade Flow

CoreGaze ships with an Inno Setup installer script at `installer/CoreGaze.iss`.

## Upgrade Behavior
- Uses a stable `AppId` so reinstalling newer versions upgrades in place.
- Installer closes running overlay processes before file replacement.
- Legacy binary cleanup removes `TaskManagerOverlay.exe` from install directory during install.

## Metadata
- Installer now publishes richer product metadata through Inno Setup fields:
	- `AppPublisherURL`
	- `AppSupportURL`
	- `AppUpdatesURL`
	- `AppContact`
- Defaults point to the CoreGaze GitHub repository, issues page, and releases page.

## Config Preservation
- User settings remain in `%APPDATA%\\CoreGaze\\config.ini`.
- Uninstall does not delete roaming config by default.
- Runtime one-time migration already supports `%APPDATA%\\TaskManagerOverlay\\config.ini` to `%APPDATA%\\CoreGaze\\config.ini`.

## Runtime Dependencies
- CoreGaze is built with MinGW-w64 and links the C++ runtime statically, so the installer does not ship or run the Visual C++ Redistributable.
- `build-installer.ps1` always passes `/DNoVcRedistBundle`. The `.iss` still contains the optional VC++ bootstrap entries, enabled only with `/DIncludeVcRedist=1` if the project ever switches to MSVC.

## Build Integration
- The version has a single source: `COREGAZE_VERSION` in `CMakeLists.txt` (default `1.0.0`, overridable with `-DCOREGAZE_VERSION=x.y.z`). CMake splits it into the exe's `VERSIONINFO` and the `COREGAZE_VERSION_TEXT` string that `main.cpp` uses for update checks and crash reports.
- `build-installer.ps1` reads the version from `build\CoreGaze.exe` and passes it as `/DMyAppVersion`. `-Version <x.y.z>` is optional and only acts as a check: the script fails if it differs from the exe.
- Compiling `CoreGaze.iss` directly (without `/DMyAppVersion`) reads the version from `..\build\CoreGaze.exe` via `GetVersionComponents`.
- `scripts/build-installer.ps1` auto-detects `iscc.exe` from PATH, known install folders, or registry installs, and offers a winget install when missing.
- One-command unsigned release flow (build + installer):
	- `./scripts/build-release.ps1`

## Install Scope And UAC
- Default behavior remains machine-wide install (`Program Files`) and requests admin privileges.
- Per-user install mode is available to avoid UAC prompts:
	- `./scripts/build-installer.ps1 -PerUserInstall`
	- `./scripts/build-release.ps1 -PerUserInstall`
- In per-user mode, install path switches to `%LOCALAPPDATA%\\Programs\\CoreGaze` and installer privileges drop to `lowest`.

## Startup Option
- Installer now includes a startup task: `Start CoreGaze automatically when I sign in`.
- When selected, installer writes `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\CoreGaze` to launch CoreGaze at user sign-in.
- If not selected, installer removes that value during install to keep startup disabled.

## GitHub Releases
- The release workflow is defined in `.github/workflows/github-release.yml`.
- On `v*` tag push (or manual dispatch with a tag), it:
	- builds `CoreGaze.exe` with the runner's MinGW-w64 toolchain (`MinGW Makefiles` generator)
	- compiles the installer with the tag's version (`v1.2.3` -> `1.2.3`)
	- publishes `CoreGaze-Setup-<version>.exe` to GitHub Releases
- The workflow configures CMake with `-DCOREGAZE_VERSION=<tag version>`, so the exe, the update check, and the installer all carry the tag's version with no manual bump.
