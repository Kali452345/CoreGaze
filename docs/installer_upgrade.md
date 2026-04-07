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

## Runtime Bootstrap
- Installer now bundles Microsoft Visual C++ runtime bootstrapper by default.
- `scripts/build-installer.ps1` auto-downloads `installer/prereqs/vc_redist.x64.exe` when missing.
- The runtime bootstrapper is packaged and executed during install only when VC runtime is missing.
- Runtime detection uses `HKLM\\SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64` (`Installed=1`).
- To opt out of bootstrap bundling for a specific build:
	- `./scripts/build-installer.ps1 -SkipVcRedistDownload`

## Build Integration
- Installer script resolves app version from built `CoreGaze.exe` when available.
- `scripts/build-installer.ps1` compiles the installer and now auto-detects `iscc.exe` from PATH or registry installs.
- One-command unsigned release flow (build + installer):
	- `./scripts/build-release.ps1`
- To skip runtime bootstrap download in one-command flow:
	- `./scripts/build-release.ps1 -SkipVcRedistDownload`

## Install Scope And UAC
- Default behavior remains machine-wide install (`Program Files`) and requests admin privileges.
- Per-user install mode is available to avoid UAC prompts:
	- `./scripts/build-installer.ps1 -PerUserInstall`
	- `./scripts/build-release.ps1 -PerUserInstall`
- In per-user mode, install path switches to `%LOCALAPPDATA%\\Programs\\CoreGaze` and installer privileges drop to `lowest`.
