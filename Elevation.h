#pragma once
#include <windows.h>

// Administrator support. Per-process disk and network need an elevated CoreGaze (see EtwMonitor).
//
// "Always run as administrator" registers an on-demand scheduled task that starts CoreGaze with the
// highest privileges. Registering it needs one UAC prompt; afterwards a normal (non-elevated)
// launch, including the "Launch on Windows Startup" Run entry, hands over to that task, which starts
// elevated without prompting.

bool IsProcessElevated();

// Command-line flags handled at startup.
bool HasCommandLineFlag(const wchar_t* flag);
// "--wait-for-pid <pid>": waits (up to 10 s) for the instance that relaunched us to exit, so the
// single-instance check doesn't see it.
void WaitForPreviousInstanceFromCommandLine();

// Starts this executable elevated (UAC prompt) with "--wait-for-pid <this pid>" plus
// extraArguments. Returns false if the user declined or the launch failed (*outError is set).
bool RelaunchElevated(const wchar_t* extraArguments, DWORD* outError);

// Registers or deletes the elevated task. Both need administrator rights, so a non-elevated caller
// gets a UAC prompt for schtasks.exe. Blocks until schtasks finishes (at most 60 s).
bool RegisterElevatedTask(DWORD* outError);
bool DeleteElevatedTask(DWORD* outError);

// Starts the elevated task (no prompt). Returns true when schtasks reports success.
bool RunElevatedTask();
