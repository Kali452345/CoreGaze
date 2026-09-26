# DPI Scaling

CoreGaze is per-monitor DPI aware (v2), so the HUD renders at the display's native resolution instead of being bitmap-stretched (and blurred) by Windows on scaled displays.

## Awareness
- `ImGui_ImplWin32_EnableDpiAwareness()` runs in `WinMain` before any window exists. It selects `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` on Windows 10 1703+, falling back to older APIs on earlier systems.
- As a result, all Win32 coordinates (work area, window bounds, mouse, saved HUD position) are physical pixels.

## Scale Factor
- The HUD window is always clamped inside the primary monitor's work area, so one scale factor, `g_dpiScale`, taken from the primary monitor, applies to the whole HUD (`1.0` = 96 DPI = 100%).
- Layout constants are defined at 96 DPI and multiplied by `g_dpiScale`:
  - font size `kHudFontSize` (17px): applied through `style.FontScaleDpi`, so ImGui 1.92 rasterizes glyphs at the scaled size (sharp, not magnified)
  - bar size `kHudBarWidth` x `kHudBarHeight` (290 x 20)
  - default position offsets `kHudDefaultRightOffset` / `kHudDefaultTopOffset` (330 / 30 from the work area's top-right)
  - style paddings, spacing and rounding: `ApplyHudStyle()` builds the style from its 96 DPI values, then calls `ScaleAllSizes`
- The HUD therefore keeps the same physical size it had when Windows stretched it, just rendered sharply.

## Runtime Changes
- `WM_DPICHANGED` (the user changes scaling), `WM_DISPLAYCHANGE` and `WM_SETTINGCHANGE` (the primary monitor may have switched) call `RefreshDpiScale()`.
- A changed scale sets `g_dpiScaleChanged`; the main loop reapplies `ApplyHudStyle()` between frames. The style is always rebuilt from 96 DPI values, so repeated changes don't compound rounding.
- The suggested rectangle in `WM_DPICHANGED` is ignored: the restyled HUD resizes the window on the next frame, and `ApplyOverlayBounds()` re-clamps its position.

## Saved Position Migration
- Config schema 2 stores `[Window] PosX/PosY` as the HUD window's screen position in physical pixels.
- Earlier versions were DPI unaware and saved a position relative to the work-area-sized host window, divided by the display scale. When `LoadAppSettings()` sees `ConfigSchemaVersion < 2` with a saved position, it computes `pos * scale + workArea origin` and writes it back immediately.

## Verification
- At 125% on a 1920x1080 panel: bars measure 362px (290 x 1.25, identical to the old stretched size), text is crisp, and a saved `650,47` position migrated to `812.5,58.8`.
