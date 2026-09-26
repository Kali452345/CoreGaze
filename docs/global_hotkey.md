# Global Hotkey Toggle

CoreGaze now supports a global visibility toggle hotkey using Win32 hotkey registration.

## Hotkey Binding

Default bindings:
- `Ctrl + Shift + O`: toggle the overlay.
  - Modifiers: `MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT`
  - Virtual key: `'O'`
  - Id: `kOverlayHotkeyId` (`0x0C0E`)
- `Ctrl + Shift + Alt + P`: open (or bring to front) the process window (`docs/process_window.md`).
  - Modifiers: `MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT`
  - Virtual key: `'P'`
  - Id: `kProcessesHotkeyId` (`0x0C0F`)
  - `Ctrl + Shift + P` alone was avoided because editors (command palette) and browsers (private window) use it, and a registered hotkey would take it from them system-wide.

## Integration Points

The hotkey is integrated entirely through the existing application message flow in `main.cpp`.

Startup integration:
- `RegisterHotKey` is called during application initialization after runtime settings and tray icon initialization.
- Registration result is tracked so cleanup only unregisters when registration succeeded.

Message pump integration:
- The existing `MsgWaitForMultipleObjects` + `PeekMessage` loop checks for `WM_HOTKEY`.
- When the overlay hotkey ID arrives, CoreGaze calls `HandleTrayCommand(hwnd, ID_TRAY_TOGGLE_OVERLAY)`. This reuses the same overlay visibility path as the tray menu toggle.
- When the processes hotkey ID arrives, CoreGaze calls `OpenProcessWindow(hwnd)`, the same path as the tray's `Processes...` item.

Shutdown integration:
- `UnregisterHotKey` is called during application cleanup for each hotkey whose registration succeeded. If another program already owns a combination, that hotkey is simply unavailable; the tray menu still works.

## Behavior and Overhead

This design is OS message-driven:
- no key-state polling loop was added
- no per-frame keyboard scanning was added
- idle overhead is effectively zero beyond normal Windows message handling

Because the hotkey routes through the tray command handler, overlay visibility, runtime window state updates, and settings persistence stay consistent with existing tray behavior.
