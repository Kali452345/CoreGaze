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

## Work-Area Boundaries
CoreGaze constrains the overlay host window to desktop work-area bounds instead of full virtual-screen bounds:

- Primary path: `SystemParametersInfoW(SPI_GETWORKAREA, ...)`.
- Fallback path: `SM_XVIRTUALSCREEN`, `SM_YVIRTUALSCREEN`, `SM_CXVIRTUALSCREEN`, and `SM_CYVIRTUALSCREEN` when work-area query fails.

This keeps the overlay from covering the taskbar while preserving safe fallback behavior on unusual shell states.

The bounds are reapplied on `WM_SETTINGCHANGE` and `WM_DISPLAYCHANGE`, so taskbar and display layout changes are reflected at runtime.

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