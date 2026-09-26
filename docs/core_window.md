# Core Window & DirectX 11 Setup

This document describes the core Win32 application and transparent windowing layer for CoreGaze.

## Overview
The goal of the core execution layer is to provide an invisible, click-through canvas onto which Dear ImGui can render hardware-accelerated user interfaces using DirectX 11. It needs to remain anchored over fullscreen applications and games without stealing mouse focus or taking up OS window chrome framing.

## Required Windows APIs

The window is constructed using standard `CreateWindowExW` calls with specific extended parameter requirements:

- `WS_POPUP`: Removes standard OS window bounds (title bar, borders, sizing controls). 
- `WS_EX_TOPMOST`: Instructs the OS window manager to place the application strictly on top of the z-order above standard application windows (including games).
- `WS_EX_LAYERED`: Declares the window as a layered window. This allows us to designate transparency and alpha blending over other applications.
- `WS_EX_TRANSPARENT`: Makes the window completely click-through. Hit-testing is disabled, and all mouse messages fall straight through the OS window manager to the target underneath.
- `WS_EX_TOOLWINDOW`: Marks the overlay as a tool window so it stays out of normal taskbar and Alt+Tab presentation paths.

### Desktop Window Manager (DWM) Integration
To facilitate proper alpha composition from the DirectX 11 clear color to the desktop background, we rely on the Desktop Window Manager (DWM):

```cpp
MARGINS margins = { -1 };
DwmExtendFrameIntoClientArea(hwnd, &margins);
```
Passing `-1` to `DwmExtendFrameIntoClientArea` removes the opaque backing of the Win32 window's client space entirely.

## DPI Awareness
The process opts into per-monitor v2 DPI awareness before creating the window, so the overlay renders at native resolution and all coordinates are physical pixels. See `dpi_scaling.md`.

## HUD-Sized Host Window
The host window is exactly as large as the HUD, not a full-screen canvas, so the swap chain, clear and present, and DWM composition only cover the HUD's pixels.

- The window starts at a rough size estimate. After every `ImGui::Render()`, the HUD's measured size (`ImGui::GetWindowSize()`) is compared with the client rect; on a mismatch the window is resized and the swap chain is resized in the same frame, before drawing, so a new size never shows stretched.
- ImGui clamps auto-resizing windows to the display size, so `io.DisplaySize` is overridden with the work-area size each frame; otherwise the HUD could never grow past the current window.
- The ImGui window is pinned at `(0, 0)` with `NoMove`. ALT + drag moves the Win32 window instead, using screen-space cursor positions (`GetCursorPos`), because ImGui's client-space mouse delta cancels out when the window moves under the cursor.

## Work-Area Boundaries
`ApplyOverlayBounds()` places the window at the saved position (or the default top-right spot) and clamps it so it stays fully inside the primary work area:

- Primary path: `SystemParametersInfoW(SPI_GETWORKAREA, ...)`.
- Fallback path: `SM_XVIRTUALSCREEN`, `SM_YVIRTUALSCREEN`, `SM_CXVIRTUALSCREEN`, and `SM_CYVIRTUALSCREEN` when work-area query fails.

This keeps the overlay from covering the taskbar while preserving safe fallback behavior on unusual shell states.

It runs at startup, while dragging, after the HUD resizes (a taller HUD near the bottom edge is pushed back up), on `Reset Overlay Position`, and on `WM_SETTINGCHANGE`, `WM_DISPLAYCHANGE` and `WM_DPICHANGED`. The clamp does not overwrite the saved position, so a HUD pushed in by a temporary resolution change returns once the space is back. Each call also re-asserts `HWND_TOPMOST`.

## Single-Instance Guard
CoreGaze enforces single-instance behavior at process startup with a named mutex (`Local\\CoreGaze.SingleInstance`).
If an existing instance is detected, the new process exits early to prevent duplicate overlays/tray icons and concurrent config writes.

## Crash Diagnostics
CoreGaze installs an unhandled exception filter during startup and writes diagnostics under `%APPDATA%\\CoreGaze\\Diagnostics`:

- `crash_*.log`: compact exception summary.
- `crash_*.dmp`: minidump generated through DbgHelp (`MiniDumpWriteDump`).

This enables post-mortem debugging without adding overhead to normal metric polling.

## Icon Resource Integration
CoreGaze embeds its executable icon through a resource script (`resource.rc`) that maps resource id `101` to `CoreGaze.ico`.
The class icon and tray icon load from this resource at runtime, with `IDI_APPLICATION` fallback if resource loading fails.

`resource.rc` also embeds Windows `VERSIONINFO` metadata so file properties and installer versioning remain aligned with the current build.

## DirectX 11 Initialization
We construct an `ID3D11Device`, `ID3D11DeviceContext`, and `IDXGISwapChain`. 
The application's render loop issues a frame clear using an absolutely transparent color standard:
`const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };`
This results in only the Dear ImGui drawn vertex buffers being opaque when rendering the frame data.

## Integration
Dear ImGui relies entirely on `.lib` linkages mapping its `backends/imgui_impl_dx11.cpp` and `backends/imgui_impl_win32.cpp` logic to our created handles, allowing drawing via normal context methods.