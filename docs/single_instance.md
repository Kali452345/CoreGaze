# Single-Instance Protection

CoreGaze now enforces a single running instance through a named Win32 mutex.

## How It Works
- On startup, `WinMain` calls `CreateMutexW` with `Local\\CoreGaze.SingleInstance`.
- If `GetLastError()` returns `ERROR_ALREADY_EXISTS`, the new process exits immediately.
- The mutex handle is closed during shutdown to keep lifecycle explicit.

## APIs Used
- `CreateMutexW`
- `GetLastError`
- `CloseHandle`

## Integration
- Implemented in `main.cpp` before monitor initialization.
- Prevents duplicate overlays, tray duplication, and config write races.
