# Crash Diagnostics

CoreGaze writes crash diagnostics for unhandled exceptions.

## How It Works
- On startup, the app creates `%APPDATA%\\CoreGaze\\Diagnostics`.
- `SetUnhandledExceptionFilter` installs a crash handler.
- On crash, the handler writes:
  - a timestamped `.log` summary (exception code, address, thread id)
  - a timestamped `.dmp` minidump for debugger analysis

## APIs Used
- `SetUnhandledExceptionFilter`
- `CreateFileW`
- `WriteFile`
- `MiniDumpWriteDump` (DbgHelp)
- `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`

## Integration
- Handler and diagnostics path initialization are in `main.cpp`.
- Build links against `dbghelp` in CMake.
