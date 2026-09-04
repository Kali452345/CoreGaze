# Global Hotkey Toggle

CoreGaze now supports a global visibility toggle hotkey using Win32 hotkey registration.

## Hotkey Binding

Default binding:
- `Ctrl + Shift + O`

Registration values:
- Modifiers: `MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT`
- Virtual key: `'O'`

## Integration Points

The hotkey is integrated entirely through the existing application message flow in `main.cpp`.

Startup integration:
- `RegisterHotKey` is called during application initialization after runtime settings and tray icon initialization.
- Registration result is tracked so cleanup only unregisters when registration succeeded.

Message pump integration:
- The existing `MsgWaitForMultipleObjects` + `PeekMessage` loop checks for `WM_HOTKEY`.
- When the registered hotkey ID arrives, CoreGaze calls:
  - `HandleTrayCommand(hwnd, ID_TRAY_TOGGLE_OVERLAY)`
- This reuses the same overlay visibility path as the tray menu toggle.

Shutdown integration:
- `UnregisterHotKey` is called during application cleanup when hotkey registration was successful.

## Behavior and Overhead

This design is OS message-driven:
- no key-state polling loop was added
- no per-frame keyboard scanning was added
- idle overhead is effectively zero beyond normal Windows message handling

Because the hotkey routes through the tray command handler, overlay visibility, runtime window state updates, and settings persistence stay consistent with existing tray behavior.
