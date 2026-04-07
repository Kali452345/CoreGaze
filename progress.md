# Project Progress

## 2026-04-01: Core Build Environment and Initial Transparent Window

**Summary of Work Done:**
- Set up a robust build build system using CMake.
- Integrated Dear ImGui dependencies natively using `FetchContent` to download straight from the ImGui GitHub repository. 
- Integrated DirectX 11 and Win32 backends in the source definitions.
- Created `main.cpp` providing the Win32 application entry point (`WinMain`).
- Created the core Win32 Window with critical extended window styles: `WS_EX_LAYERED`, `WS_EX_TRANSPARENT`, and `WS_EX_TOPMOST` combined with the `WS_POPUP` style to create a borderless, transparent, "always on top", click-through hardware-accelerated overlay. 
- The frame was extended into the client area via `DwmExtendFrameIntoClientArea` for true DWM transparency.
- Initialized DirectX 11 device and swap chain handling rendering.
- Rendered a primary "Hello Task Manager" widget.

**Current State:**
- The application can be compiled with CMake / MSVC.
- It displays a transparent, click-through overlay anchored. 

**Immediate Next Steps:**
- Integrate the necessary bare-metal Win32 APIs for physical and virtual memory polling.
- Setup an abstraction / polling mechanism that adheres to standard library restrictions in tight loops (zero-allocation polling). 
- Plot System Resource utilization using ImGui APIs in the display layer.
## 2026-04-01: Monitoring Engine and ImGui Frontend UI Integration

**Summary of Work Done:**
- Created the SystemMonitor class that fetches metrics internally via GlobalMemoryStatusEx and GetSystemTimes.
- Implemented a 1000ms delta non-blocking timer in SystemMonitor::Update to avoid CPU pipeline spamming during rendering loops.
- Processed CPU times (Idle, Kernel, User) by calculating differentials (deltas) over the polling period.
- Updated main.cpp UI frontend to feature floating ProgressBar elements denoting fractional thresholds for hardware specs.
- Replaced original background alpha with 0.0f rendering metrics natively in the desktop environment space.

**Current State:**
- SystemMonitor is fully operational polling at exactly 1Hz.
- Floating HUD overlay operates properly over applications seamlessly rendering UI stats.

**Immediate Next Steps:**
- Test application and integrate GPU metrics (via DXGI / NVAPI) or Network I/O metrics.


## 2026-04-01: PDH Integration and iGPU Protections

**Summary of Work Done:**
- Replaced GetSystemTimes tracking logic in SystemMonitor.cpp with the Windows Performance Data Helper API (pdh.h).
- Polled new % Disk Usage and Network Mbps metrics avoiding manual Win32 math bottlenecks using wildcard \\Network Interface(*)\\Bytes Total/sec.
- Added MsgWaitForMultipleObjects to the main.cpp UI loop, saving 99% of CPU spinning by blocking Windows Message pumps entirely unless forced to wake by user interactions or 1000ms stat expiration ticks.
- Updated Dear ImGui visuals to a dark gray 60% opacity floating box with rounded corners and uniquely colored tracking progress bars separating variables by type (Blue, Green, Orange, Purple).

**Current State:**
- System logic is successfully throttling execution on integrated hardware, dropping base overhead to 0% at idle.

**Immediate Next Steps:**
- Abstract styling out into config headers and consider enabling modular system tray support.


## 2026-04-01: UI Scaling and Dynamic WS_EX_TRANSPARENT Hot-Swapping

**Summary of Work Done:**
- Overrode default ImGui sizing by loading \segoeui.ttf\ at 22px internally.
- Enforced a global layout scale (\ScaleAllSizes(1.2f)\) fixing UX rendering readability issues on larger displays.
- Built a hot-swapping Win32 API bridge in the main loop to dynamically add/remove \WS_EX_TRANSPARENT\ via \GetWindowLong\ & \SetWindowLong\ checks bound to \(GetAsyncKeyState(VK_MENU) & 0x8000)\.
- Stripped \ImGuiWindowFlags_NoMove\ if ALT is held, allowing repositioning over the Desktop.
- Inlaid a slightly dimmed HUD hint \*(Hold ALT to move)*\ resolving user friction.

**Current State:**
- The user can cleanly read their statistics natively over anything, while holding ALT allows them to drag the bounds of the ImGui window resolving rigid placement.

**Immediate Next Steps:**
- Polish and stabilize to an overarching configuration profile saving bounds.


## 2026-04-01: Full-Screen Virtual Canvas & ImGui Clipping Fix

**Summary of Work Done:**
- Resolved ImGui clipping bounds by replacing the hardcoded 800x600 Win32 application resolution with pure virtual screen queries.
- Used \GetSystemMetrics(SM_CXVIRTUALSCREEN)\ and \SM_CYVIRTUALSCREEN\ to seamlessly stretch the invisible DX11 canvas over every connected display monitor.
- Corrected \ImGui::SetNextWindowPos\ to push the overlay to the top right corner natively, explicitly bounding it via \ImGuiCond_FirstUseEver\ to prevent drag-fighting after the initial render.

**Current State:**
- Multi-monitor dragging and collision bounds are now correct. Holding ALT allows the overlay to move seamlessly anywhere across the virtual screen bounds.

**Immediate Next Steps:**
- Save layout positions into an INI context so coordinates survive system reboots.


## 2026-04-05: CPU Frequency (GHz) Tracking Component

**Summary of Work Done:**
- Integrated CPU Core Clock parsing to accompany total usage percentage.
- Added Registry hook querying \HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0(~MHz)\ on initialization to lock in base CPU boundaries.
- Added \\\Processor Information(_Total)\\% Processor Performance\ wildcard via PDH to dynamically fetch core multipliers natively.
- Bound math logic tracking multiplier deltas to dynamically calculate current Active GHz speed.
- Wielded these logic ties into ImGui rendering format hooks updating UI CPU progress label elements seamlessly.

**Current State:**
- CPU progress bar now prints in-line Dynamic GHz stats over percent loads without inducing WMI stuttering.

**Immediate Next Steps:**
- Await instructions to expand logic engine bounds into tracking GPU memory via DXGI pointers and D3DKMTQueryStatistics engine hooks.


- Fixed an ImGui assertion crash where a missing PushStyleColor for the CPU progress bar caused PopStyleColor() to fail.


## 2026-04-05: CPU Row Rendering Regression Fix

**Summary of Work Done:**
- Restored the missing CPU `ImGui::ProgressBar(...)` draw call in the overlay render block.
- Fixed an empty CPU panel caused by a style color push/pop pair that had no rendered widget between it.
- Kept style stack behavior balanced and consistent with the RAM/Disk/Network render pattern.

**Current State:**
- CPU row now renders both the blue progress bar and live `%.1f%% @ %.2f GHz` text payload.

**Immediate Next Steps:**
- Proceed with GPU metrics integration using low-overhead PDH and DXGI paths.


## 2026-04-05: GPU Monitoring Engine and HUD Integration

**Summary of Work Done:**
- Extended `SystemMonitor` to accept the active D3D11 device and resolve the currently active DXGI adapter.
- Added DXGI GPU memory polling on a 1000ms cadence using `QueryVideoMemoryInfo` for dedicated (local) and shared (non-local) memory.
- Added PDH wildcard polling for `\\GPU Engine(*)\\Utilization Percentage` and filtered instances to the active adapter using LUID matching.
- Aggregated engine utilization by engine type (`3D`, `Compute`, `Copy`, `Decode`, `Encode`, `VideoProcessing`, `GDI Render`) and surfaced the busiest engine label with utilization percent.
- Replaced per-poll wildcard heap churn for network parsing with reusable PDH buffers, and reused the same buffer strategy for GPU wildcard parsing.
- Integrated a new GPU row in the ImGui HUD with utilization-based progress fallback to memory ratio when utilization is unavailable.

**Current State:**
- Overlay now displays GPU utilization, active engine label, dedicated GPU memory usage, and shared GPU memory usage with adapter-bound filtering and graceful fallback behavior.

**Immediate Next Steps:**
- Validate values under idle, video decode, and 3D load against Windows Task Manager and tune any clamping behavior if needed.


## 2026-04-05: System Tray Control Hub (Phase 1 Start)

**Summary of Work Done:**
- Added a Win32 system tray icon and right-click context menu as the runtime control hub.
- Added visibility toggles for CPU, RAM, GPU, Disk, and Network sections.
- Added polling rate controls (`500ms`, `1000ms`, `2000ms`) and wired them into runtime monitor cadence.
- Added persistent settings storage in `%APPDATA%\\TaskManagerOverlay\\config.ini`.
- Added logic gating in `SystemMonitor` so disabled metrics are no longer polled.
- Added disk selection mask plumbing and tray drive selection checkboxes (phase-1 gating foundation).
- Updated HUD rendering to honor visibility toggles and skip section drawing when disabled.
- Added hidden-overlay mode control from tray while keeping process and tray menu alive.

**Current State:**
- The app now supports tray-driven runtime settings with persistence and low-overhead metric polling gates.

**Immediate Next Steps:**
- Implement full GPU display modes (Targeted, Highest-Load, Multi-GPU, Aggregate).
- Implement physical-disk-level targeted polling with IOPS + active-time output per selected disks.
- Implement active default-gateway network adapter detection with upload and download split.


## 2026-04-05: Active GPU Name and Network SSID Display

**Summary of Work Done:**
- Added active GPU adapter name capture from DXGI adapter discovery and exposed it through monitor getters.
- Added network identity discovery and caching using IP Helper APIs, with Wi-Fi SSID discovery through WLAN APIs.
- Added low-frequency network identity refresh cadence (10s) separate from high-frequency metric polling to reduce overhead.
- Updated HUD network row text to display throughput plus active adapter/SSID identity.
- Updated HUD GPU row text to display the active monitored GPU adapter name.
- Added required build links for `iphlpapi`, `wlanapi`, and `ws2_32`.

**Current State:**
- Overlay now shows which GPU adapter is being monitored and shows network identity with SSID when connected on Wi-Fi.

**Immediate Next Steps:**
- Validate behavior across Ethernet and Wi-Fi transitions and tune identity refresh timing if needed.


## 2026-04-05: Full GPU Modes, Targeted Disk IOPS, and Route-Aware Network Split

**Summary of Work Done:**
- Expanded GPU telemetry from single-adapter mode to full mode matrix:
	- `Targeted` (user-selected adapter)
	- `Highest-Load` (dynamic busiest adapter)
	- `Multi-GPU` (one row per adapter)
	- `Aggregate` (average utilization, summed memory usage)
- Refactored backend GPU model to enumerate and cache multiple DXGI adapters and track per-adapter utilization/memory snapshots.
- Implemented selected-drive disk telemetry rows with per-drive active-time and read/write IOPS output.
- Added drive-to-disk mapping pipeline that prioritizes `QueryDosDevice` and uses instance-aware PDH resolution, with explicit `_Total` fallback labeling when mapping is unresolved.
- Replaced network throughput total-only rendering with upload/download split using active adapter sampling.
- Added route-aware primary network source logic and optional secondary adapter tracking with independent throughput rows.
- Expanded tray control hub:
	- GPU display mode selector
	- GPU source selector
	- Network primary source mode (auto/manual)
	- Network secondary enable/selection
	- Network display mode selector
- Extended INI persistence and runtime settings application for all new GPU/network controls.
- Preserved low-resource cadence model: identity refresh remains slow cadence while metric polling remains interval-based.

**Current State:**
- Build succeeds and runtime process launches successfully with the new telemetry model active.
- Overlay now supports multi-mode GPU output, per-selected-drive disk activity/IOPS rows, and primary/secondary network split display modes.
- Tray controls can force GPU source/mode and network source/display behavior live at runtime with persisted settings.

**Immediate Next Steps:**
- Validate mode behavior under real mixed workloads (multi-GPU stress, per-drive read/write stress, VPN route changes).
- Tune tray UX labels and fallback wording if any ambiguity appears during long-run usage.


## 2026-04-05: HUD Label Placement and Split Network Mini-Boxes

**Summary of Work Done:**
- Updated network HUD presentation so the network name is rendered next to the `Network` label instead of inside the usage box.
- Changed network identity display policy to SSID-only when available (no adapter name in the displayed network label).
- Updated split network presentation to use two side-by-side mini bars with explicit down/up markers (`v` and `^`) and adaptive `Kbps`/`Mbps` labels.
- Updated GPU HUD presentation so adapter name is rendered next to the `GPU` label, while the progress box now shows only usage/engine/memory details.
- Reformatted disk read/write IOPS text to compact scaled units (`IOPS`, `KIOPS`, `MIOPS`) for better readability.

**Current State:**
- Network and GPU names now live in labels near the metric title rows.
- Network split bars render side-by-side as requested and keep throughput units readable at low/high rates.
- Disk activity rows still show active percentage plus read/write load with compact units.

**Immediate Next Steps:**
- Validate readability under different DPI/scales and tune mini-bar widths if needed.


## 2026-04-05: Disk Throughput Units and Split-Bar Layout Fix

**Summary of Work Done:**
- Reworked disk read/write metrics from operation counts to throughput-based values.
- Switched disk PDH counters to `Disk Read Bytes/sec` and `Disk Write Bytes/sec`.
- Converted disk read/write counter output to Mbps in monitor polling and surfaced adaptive `Kbps`/`Mbps` labels in HUD.
- Updated network split-bar width math to use ImGui style spacing so download/upload mini bars render side-by-side reliably.

**Current State:**
- Disk rows now report bandwidth-style read/write values (`Kbps` or `Mbps`) instead of IOPS units.
- Network split rows use spacing-aware layout and no longer stack unexpectedly under normal window width.

**Immediate Next Steps:**
- Validate with sustained disk copy workloads and confirm read/write throughput visually tracks expected ranges.


## 2026-04-05: Primary Network Always Split Bars

**Summary of Work Done:**
- Removed the remaining single-bar primary network render branch.
- Primary network now always renders as two side-by-side mini bars (`v` download, `^` upload).
- Kept spacing-aware width and same-line spacing logic so bars remain horizontal at scaled UI sizes.

**Current State:**
- Primary network metric no longer falls back to a single combined bar due display mode state.
- Secondary network row behavior remains mode-gated and unchanged.

**Immediate Next Steps:**
- Confirm visual alignment on the target display scale and adjust mini-bar width margins only if needed.


## 2026-04-05: CoreGaze Window Boundaries and Branding

**Summary of Work Done:**
- Rebranded runtime identity from TaskManagerOverlay to CoreGaze in the application class/title, HUD title, tray tooltip, and app-data config folder naming.
- Added one-time legacy config migration: if `%APPDATA%\\CoreGaze\\config.ini` is missing and `%APPDATA%\\TaskManagerOverlay\\config.ini` exists, legacy settings are copied forward.
- Switched initial overlay host bounds from full virtual-screen sizing to primary desktop work-area sizing via `SystemParametersInfoW(SPI_GETWORKAREA)` with virtual-screen fallback.
- Added runtime bounds refresh on `WM_SETTINGCHANGE` and `WM_DISPLAYCHANGE` so taskbar/display-layout changes reapply overlay window bounds.
- Added `WS_EX_TOOLWINDOW` to overlay window creation to keep the overlay out of standard taskbar/Alt+Tab presentation paths.
- Wired executable/tray icon loading to embedded resource id `101` and added fallback to `IDI_APPLICATION` if resource loading fails.
- Added `resource.rc` to the build graph and prepared CMake target naming for `CoreGaze.exe` output.
- Fixed MinGW compatibility blockers during validation:
	- replaced `LoadIconW(..., IDI_APPLICATION)` fallback usage with `LoadIcon(..., IDI_APPLICATION)` in this toolchain configuration.
	- scoped ImGui include directories to C++ compilation only so `windres` can compile `resource.rc` without space-splitting include path failures.

**Current State:**
- Source and docs are synchronized for CoreGaze naming and work-area boundary behavior.
- Build configuration includes icon resource embedding and renamed project target.

**Immediate Next Steps:**
- Rebuild and run CoreGaze, then validate taskbar/Alt+Tab visibility behavior, tray icon rendering, work-area placement, and settings persistence/migration.


## 2026-04-05: Release Hardening, Installer Upgrade Path, and Signing Readiness

**Summary of Work Done:**
- Added single-instance process guard in `WinMain` via named mutex (`Local\\CoreGaze.SingleInstance`) to prevent duplicate overlays.
- Added startup management support:
	- New tray command `Launch on Windows Startup`.
	- New persisted config key `General/StartWithWindows`.
	- HKCU Run-key integration at `Software\\Microsoft\\Windows\\CurrentVersion\\Run` value `CoreGaze`.
- Added crash diagnostics:
	- Created `%APPDATA%\\CoreGaze\\Diagnostics` path at startup.
	- Installed unhandled exception filter.
	- Wrote timestamped `.log` and `.dmp` crash artifacts using DbgHelp minidumps.
- Added config version markers (`Version/ConfigSchemaVersion`, `Version/LastLaunchedVersion`) and preserved legacy config migration behavior.
- Added one-time legacy executable cleanup for `TaskManagerOverlay.exe` in the current install directory.
- Expanded `resource.rc` with Windows `VERSIONINFO` metadata while keeping icon embedding.
- Added build-level version constants in CMake and linked `dbghelp`.
- Added optional code-signing build hook variables in CMake for post-build signing when certificate secrets are provided.
- Added installer and release tooling:
	- `installer/CoreGaze.iss` (Inno Setup) with stable AppId, process-close handling, and legacy binary cleanup during install.
	- `scripts/build-installer.ps1` and `scripts/sign-artifacts.ps1` helper scripts.
	- `.github/workflows/release-signing-template.yml` CI template for cert-based signing flow.
- Added dedicated feature docs:
	- `docs/single_instance.md`
	- `docs/startup_management.md`
	- `docs/crash_diagnostics.md`
	- `docs/installer_upgrade.md`
	- `docs/code_signing.md`

**Current State:**
- Core runtime now includes release-grade single-instance, startup, and crash diagnostics foundations.
- Installer and signing flows are scaffolded and ready for certificate-backed release execution.
- No certificate material is stored in-repo; signing remains opt-in via local/CI secrets.

**Immediate Next Steps:**
- Rebuild and run validation matrix:
	- second-launch exit behavior,
	- startup key toggle correctness,
	- forced-crash artifact generation,
	- installer compile and upgrade overwrite behavior,
	- optional signing run once certificate is available.


## 2026-04-07: Repository Bootstrap and One-Command Unsigned Release Flow

**Summary of Work Done:**
- Initialized git for the project workspace and prepared the default branch for first commit.
- Added a root `.gitignore` to exclude build artifacts, generated binaries, IDE state, and runtime dump/log files.
- Added release helper scripting to support one-command unsigned release packaging (build + installer) for ongoing local release generation.
- Upgraded installer metadata with publisher/support/update/contact URLs for richer Add/Remove Programs details.
- Added optional Visual C++ runtime bootstrap integration (`vc_redist.x64.exe`) with install-time missing-runtime checks.
- Connected repository remote to GitHub for publish/push readiness.

**Current State:**
- Workspace is ready for source-only commits without accidentally tracking local build output.
- Project is ready to generate future unsigned installers on demand via scripted release flow.
- Installer supports both default machine-wide mode and optional per-user mode to avoid UAC prompts.

**Immediate Next Steps:**
- Run the one-command release script for each new release build.
- Push the initial commit to the configured GitHub remote.
