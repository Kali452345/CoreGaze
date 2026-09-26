# Administrator Mode (Elevation)

CoreGaze runs without administrator rights by default. Elevation (`Elevation.h` / `Elevation.cpp`) unlocks:
- the process window's **Disk** and **Network** columns (kernel ETW, see `docs/etw_disk_network.md`);
- **End task**, **Set priority** and **Open file location** for elevated processes and services.

There are two ways to get it.

## Restart as Administrator (One-Off)
Available from the tray menu (**Restart as Administrator**, shown only when not elevated) and from the process window's toolbar button:
1. `RelaunchElevated` starts `CoreGaze.exe --wait-for-pid <current pid> [--background]` with the `runas` verb, which shows a UAC prompt.
2. If the user accepts, the current instance closes (`DestroyWindow`) and the new one waits up to 10 s for it to exit (`WaitForPreviousInstanceFromCommandLine`) before taking the single-instance mutex.
3. The elevated copy opens the process window, as any start does. `--background` is added when the window was closed, so it stays closed.

Declining the prompt (`ERROR_CANCELLED`) changes nothing. Other failures show a message box with the error code.

## Always Run as Administrator (Persistent)
The tray item **Always Run as Administrator** registers a scheduled task named `CoreGaze Elevated Startup`:
- **Run level:** highest (`HighestAvailable`, which is elevated for an administrator account).
- **Logon:** `InteractiveToken`, so it runs in the user's desktop session.
- **Triggers:** none; it runs only on demand.
- **Action:** `"<path to CoreGaze.exe>" --from-task`.
- **Settings:**
  - no time limit (`PT0S`);
  - runs on battery and doesn't stop on battery;
  - `IgnoreNew` for multiple instances.

The task is created from a UTF-16 XML file with `schtasks /Create /XML`, run through `runas`, so it costs one UAC prompt. Windows allows the task to start elevated without prompting because it was registered by an administrator.

At startup (`WinMain`), when `[General] AlwaysElevated=1` and CoreGaze is not elevated:
1. It releases the single-instance mutex and runs `schtasks /Run /TN "CoreGaze Elevated Startup"` (no prompt).
2. On success, it exits. The task's instance (`--from-task`) retries the mutex for up to 5 s while the first instance exits.
3. If the task can't be started (deleted, or Task Scheduler unavailable), it takes the mutex back and continues without elevation.

This covers every launch path: the **Launch on Windows Startup** Run key, Start menu and desktop shortcuts, and the installer's "Launch CoreGaze".

`schtasks /Run` can't pass arguments to the task. So before starting it, the handing-over instance records in `[General] HandoffBackground` whether it was started with `--background`. The task instance reads that value and clears it, so a sign-in start stays in the background and a Start menu start opens the process window. "Restart it as administrator now?" (below) sets the value from whether the window is open.

- `--no-elevate` skips the handoff (for troubleshooting).
- A `--from-task` instance never hands off again. For an account that isn't an administrator, `HighestAvailable` yields a normal token, and without this rule the two would loop.

When the setting is turned on from a non-elevated instance, CoreGaze offers to restart elevated immediately, through the task, falling back to `runas`.

Turning it off deletes the task (another UAC prompt when not elevated). If the prompt is declined, the setting stays on. Any other failure (most likely the task is already gone) turns the setting off.

### Moved Executables
The task stores the executable path. The path used when registering is saved as `[General] AlwaysElevatedPath`. An elevated instance whose path differs (for example, a portable copy that was moved) re-registers the task with its own path. It can do this without a prompt because it is already elevated.

### Uninstall
The installer's `CurUninstallStepChanged` checks for the task with `schtasks /Query`. If it exists, it deletes it with `runas`, which shows a UAC prompt only if the uninstaller isn't already elevated.

## Elevated Behaviour Notes
- `ChangeWindowMessageFilterEx(WM_TRAYICON)` lets the non-elevated Explorer's tray callbacks reach an elevated CoreGaze (UIPI would otherwise drop them).
- Config, the HUD position and the process window layout stay in the same `%APPDATA%\CoreGaze` folder, because elevation keeps the same user profile.

## Settings
| Key | Meaning |
|---|---|
| `[General] AlwaysElevated` | 1 = hand non-elevated starts over to the elevated task |
| `[General] AlwaysElevatedPath` | Executable path the task was registered with |
| `[General] HandoffBackground` | Set only between a handoff and the task instance's start: 1 = the original start was `--background` |

## Command-Line Flags
| Flag | Meaning |
|---|---|
| `--wait-for-pid <pid>` | Wait (up to 10 s) for that process to exit before starting |
| `--from-task` | Started by the elevated task: retry the single-instance mutex for 5 s; never hand off |
| `--background` | Start without opening the process window (the Run key uses it) |
| `--no-elevate` | Don't hand off to the elevated task this time |

## Testing Status
Needs an interactive administrator session with someone present to accept the UAC prompts:
- the UAC restart;
- task registration and the handoff at sign-in;
- task deletion;
- uninstall cleanup;
- elevated tray callbacks.

The non-elevated paths are verified: no task, the setting off, the button shown.
