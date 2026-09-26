# Single-Instance Protection

CoreGaze now enforces a single running instance through a named Win32 mutex.

## How It Works
- On startup, `WinMain` calls `AcquireSingleInstanceMutex`, which calls `CreateMutexW` with `Local\\CoreGaze.SingleInstance`.
- If `GetLastError()` returns `ERROR_ALREADY_EXISTS`, the new process exits immediately.

## Handovers
Two startup paths hand over from one instance to another; see `docs/elevation.md`:
- `--wait-for-pid <pid>` ("Restart as administrator"): before touching the mutex, the new instance waits (up to 10 s) for the old one to exit.
- `--from-task` (the elevated scheduled task): the old instance releases the mutex before starting the task and exits right after. The new instance retries the mutex every 100 ms for up to 5 s.
- The non-elevated instance that hands over to the task closes its mutex first. If the task can't be started, it takes the mutex back (and exits if another instance got it in between).
- The mutex handle is closed during shutdown to keep lifecycle explicit.

## APIs Used
- `CreateMutexW`
- `GetLastError`
- `CloseHandle`

## Integration
- Implemented in `main.cpp` before monitor initialization.
- Prevents duplicate overlays, tray duplication, and config write races.
