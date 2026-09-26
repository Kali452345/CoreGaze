#include "Elevation.h"
#include <shellapi.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

namespace {

const wchar_t kTaskName[] = L"CoreGaze Elevated Startup";
const DWORD kSchtasksTimeoutMs = 60000;

bool GetModulePath(wchar_t* path, DWORD capacity) {
    const DWORD length = GetModuleFileNameW(NULL, path, capacity);
    return length > 0 && length < capacity;
}

bool GetSystemToolPath(const wchar_t* toolName, wchar_t* path, UINT capacity) {
    const UINT length = GetSystemDirectoryW(path, capacity);
    if (length == 0 || length + wcslen(toolName) + 2 > capacity) {
        return false;
    }
    wcscat_s(path, capacity, L"\\");
    wcscat_s(path, capacity, toolName);
    return true;
}

// Runs a program without a window and waits for it. With elevate = true it goes through
// ShellExecuteEx "runas" (UAC prompt unless we are already elevated).
bool RunHidden(const wchar_t* program, const wchar_t* arguments, bool elevate, DWORD* outExitCode, DWORD* outError) {
    *outExitCode = (DWORD)-1;
    *outError = ERROR_SUCCESS;

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = elevate ? L"runas" : L"open";
    info.lpFile = program;
    info.lpParameters = arguments;
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        *outError = GetLastError(); // ERROR_CANCELLED when the UAC prompt is declined
        return false;
    }
    if (info.hProcess == NULL) {
        *outError = ERROR_INVALID_HANDLE;
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(info.hProcess, kSchtasksTimeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(info.hProcess, outExitCode);
    } else {
        *outError = ERROR_TIMEOUT;
    }
    CloseHandle(info.hProcess);
    return waitResult == WAIT_OBJECT_0;
}

bool RunSchtasks(const wchar_t* arguments, bool elevate, DWORD* outError) {
    wchar_t schtasksPath[MAX_PATH];
    if (!GetSystemToolPath(L"schtasks.exe", schtasksPath, MAX_PATH)) {
        *outError = ERROR_FILE_NOT_FOUND;
        return false;
    }
    DWORD exitCode = 0;
    if (!RunHidden(schtasksPath, arguments, elevate, &exitCode, outError)) {
        return false;
    }
    if (exitCode != 0) {
        *outError = ERROR_GEN_FAILURE;
        return false;
    }
    return true;
}

// Appends text with the five XML special characters escaped. Returns false if it doesn't fit.
bool AppendXmlEscaped(wchar_t* output, size_t capacity, const wchar_t* text) {
    size_t length = wcslen(output);
    for (const wchar_t* c = text; *c != L'\0'; ++c) {
        const wchar_t* replacement = NULL;
        switch (*c) {
        case L'&': replacement = L"&amp;"; break;
        case L'<': replacement = L"&lt;"; break;
        case L'>': replacement = L"&gt;"; break;
        case L'"': replacement = L"&quot;"; break;
        case L'\'': replacement = L"&apos;"; break;
        default: break;
        }
        const size_t needed = replacement ? wcslen(replacement) : 1;
        if (length + needed + 1 > capacity) {
            return false;
        }
        if (replacement) {
            wcscpy_s(output + length, capacity - length, replacement);
        } else {
            output[length] = *c;
            output[length + 1] = L'\0';
        }
        length += needed;
    }
    return true;
}

bool AppendText(wchar_t* output, size_t capacity, const wchar_t* text) {
    return wcscat_s(output, capacity, text) == 0;
}

// Writes the task definition as UTF-16 with a BOM, which is what schtasks /XML expects.
bool WriteTaskXml(const wchar_t* xmlPath, DWORD* outError) {
    wchar_t modulePath[MAX_PATH];
    wchar_t userName[512];
    ULONG userNameLength = 512;
    if (!GetModulePath(modulePath, MAX_PATH) || !GetUserNameExW(NameSamCompatible, userName, &userNameLength)) {
        *outError = GetLastError();
        return false;
    }

    const size_t capacity = 8192;
    wchar_t* xml = (wchar_t*)malloc(capacity * sizeof(wchar_t));
    if (xml == NULL) {
        *outError = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    xml[0] = L'\0';

    // No trigger: the task only runs on demand (schtasks /Run), started by a non-elevated launch.
    // The defaults would stop CoreGaze after 3 days and on battery power, so those are disabled.
    bool ok = AppendText(xml, capacity,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Description>Starts CoreGaze with administrator rights without a UAC prompt, for per-process disk and network monitoring. Created by CoreGaze's \"Always Run as Administrator\" option.</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>");
    ok = ok && AppendXmlEscaped(xml, capacity, userName);
    ok = ok && AppendText(xml, capacity,
        L"</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>5</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>\"");
    ok = ok && AppendXmlEscaped(xml, capacity, modulePath);
    ok = ok && AppendText(xml, capacity,
        L"\"</Command>\r\n"
        L"      <Arguments>--from-task</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n");
    if (!ok) {
        free(xml);
        *outError = ERROR_INSUFFICIENT_BUFFER;
        return false;
    }

    HANDLE file = CreateFileW(xmlPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *outError = GetLastError();
        free(xml);
        return false;
    }
    const WCHAR bom = 0xFEFF;
    DWORD written = 0;
    BOOL writeOk = WriteFile(file, &bom, sizeof(bom), &written, NULL);
    writeOk = writeOk && WriteFile(file, xml, (DWORD)(wcslen(xml) * sizeof(wchar_t)), &written, NULL);
    CloseHandle(file);
    free(xml);
    if (!writeOk) {
        *outError = GetLastError();
        DeleteFileW(xmlPath);
        return false;
    }
    return true;
}

} // namespace

bool IsProcessElevated() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool HasCommandLineFlag(const wchar_t* flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        return false;
    }
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) {
        found = (_wcsicmp(argv[i], flag) == 0);
    }
    LocalFree(argv);
    return found;
}

void WaitForPreviousInstanceFromCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        return;
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--wait-for-pid") == 0) {
            const DWORD pid = (DWORD)wcstoul(argv[i + 1], NULL, 10);
            HANDLE process = (pid != 0) ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
            if (process != NULL) {
                WaitForSingleObject(process, 10000);
                CloseHandle(process);
            }
            break;
        }
    }
    LocalFree(argv);
}

bool RelaunchElevated(const wchar_t* extraArguments, DWORD* outError) {
    wchar_t modulePath[MAX_PATH];
    if (!GetModulePath(modulePath, MAX_PATH)) {
        *outError = GetLastError();
        return false;
    }
    wchar_t arguments[256];
    swprintf_s(arguments, L"--wait-for-pid %lu %ls", GetCurrentProcessId(), extraArguments ? extraArguments : L"");

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = modulePath;
    info.lpParameters = arguments;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        *outError = GetLastError();
        return false;
    }
    *outError = ERROR_SUCCESS;
    return true;
}

bool RegisterElevatedTask(DWORD* outError) {
    wchar_t tempDirectory[MAX_PATH];
    wchar_t xmlPath[MAX_PATH];
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (tempLength == 0 || tempLength >= MAX_PATH ||
        swprintf_s(xmlPath, L"%lsCoreGazeElevatedTask_%lu.xml", tempDirectory, GetCurrentProcessId()) < 0) {
        *outError = ERROR_PATH_NOT_FOUND;
        return false;
    }
    if (!WriteTaskXml(xmlPath, outError)) {
        return false;
    }

    wchar_t arguments[MAX_PATH + 128];
    swprintf_s(arguments, L"/Create /TN \"%ls\" /XML \"%ls\" /F", kTaskName, xmlPath);
    const bool ok = RunSchtasks(arguments, true, outError);
    DeleteFileW(xmlPath);
    return ok;
}

bool DeleteElevatedTask(DWORD* outError) {
    wchar_t arguments[128];
    swprintf_s(arguments, L"/Delete /TN \"%ls\" /F", kTaskName);
    return RunSchtasks(arguments, true, outError);
}

bool RunElevatedTask() {
    wchar_t arguments[128];
    swprintf_s(arguments, L"/Run /TN \"%ls\"", kTaskName);
    DWORD error = ERROR_SUCCESS;
    return RunSchtasks(arguments, false, &error);
}
