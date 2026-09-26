#include "ProcessWindow.h"
#include "SystemMonitor.h"
#include "Elevation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

const wchar_t kClassName[] = L"CoreGazeProcessesClass";
const wchar_t kWindowTitle[] = L"CoreGaze - Processes";
const wchar_t kSection[] = L"Processes";

// Layout at 100% scaling (96 DPI); multiplied by the window's DPI scale.
const float kFontSize = 15.0f;
const float kDefaultWidth = 1000.0f;
const float kDefaultHeight = 640.0f;
const float kMinWidth = 560.0f;
const float kMinHeight = 320.0f;

const ULONGLONG kMinFrameIntervalMs = 16;   // at most ~60 frames per second, only while interacting
const ULONGLONG kLateFrameDelayMs = 700;    // one more frame once input stops (hover tooltips)
const int kInputFrames = 3;                 // ImGui needs a couple of frames to settle after input
const ULONGLONG kStatusDurationMs = 8000;

const DWORD kRefreshChoices[] = { 500, 1000, 2000, 0 };
const char* const kRefreshLabels[] = { "High (0.5 s)", "Normal (1 s)", "Low (2 s)", "Paused" };

struct ColumnInfo {
    const char* name;
    float width;
    ImGuiTableColumnFlags flags;
};

const ImGuiTableColumnFlags kNumeric = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;

// Order matches ProcessWindow::Column.
const ColumnInfo kColumns[] = {
    { "Name",        230.0f, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide },
    { "PID",          58.0f, ImGuiTableColumnFlags_WidthFixed },
    { "CPU",          62.0f, kNumeric | ImGuiTableColumnFlags_DefaultSort },
    { "Memory",       88.0f, kNumeric },
    { "Disk",         82.0f, kNumeric },
    { "Network",      86.0f, kNumeric },
    { "GPU",          56.0f, kNumeric },
    { "GPU engine",   78.0f, ImGuiTableColumnFlags_WidthFixed },
    { "I/O",          82.0f, kNumeric },
    { "Threads",      62.0f, kNumeric | ImGuiTableColumnFlags_DefaultHide },
    { "Handles",      66.0f, kNumeric | ImGuiTableColumnFlags_DefaultHide },
    { "Working set",  92.0f, kNumeric | ImGuiTableColumnFlags_DefaultHide },
    { "Commit",       92.0f, kNumeric | ImGuiTableColumnFlags_DefaultHide },
    { "Session",      58.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide },
    { "Parent PID",   72.0f, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide },
};

// Sort context for the qsort comparators (the UI is single-threaded).
const ProcessRow* s_sortRows = NULL;
int s_sortColumn = 0;
bool s_sortDescending = false;

DWORD HashNameLowercase(const char* text) {
    DWORD hash = 2166136261u;
    for (const unsigned char* c = (const unsigned char*)text; *c != 0; ++c) {
        unsigned char ch = *c;
        if (ch >= 'A' && ch <= 'Z') {
            ch = (unsigned char)(ch - 'A' + 'a');
        }
        hash = (hash ^ ch) * 16777619u;
    }
    return hash;
}

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// needle must already be lowercase.
bool ContainsNoCase(const char* haystack, const char* needle) {
    if (needle[0] == '\0') {
        return true;
    }
    for (const char* start = haystack; *start != '\0'; ++start) {
        const char* h = start;
        const char* n = needle;
        while (*h != '\0' && *n != '\0' && ToLowerAscii(*h) == *n) {
            ++h;
            ++n;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

void FormatPercent(float value, char* output, int outputSize) {
    if (value < 0.05f) {
        snprintf(output, outputSize, "0%%");
    } else {
        snprintf(output, outputSize, "%.1f%%", value);
    }
}

void FormatMegabytes(ULONGLONG bytes, char* output, int outputSize) {
    snprintf(output, outputSize, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

void FormatByteRate(float bytesPerSec, char* output, int outputSize) {
    const double kilobytes = bytesPerSec / 1024.0;
    if (kilobytes < 0.5) {
        snprintf(output, outputSize, "0 KB/s");
    } else if (kilobytes < 1024.0) {
        snprintf(output, outputSize, "%.0f KB/s", kilobytes);
    } else {
        snprintf(output, outputSize, "%.1f MB/s", kilobytes / 1024.0);
    }
}

void FormatBitRate(float bytesPerSec, char* output, int outputSize) {
    const double bits = bytesPerSec * 8.0;
    if (bits < 500.0) {
        snprintf(output, outputSize, "0 Kbps");
    } else if (bits < 1000000.0) {
        snprintf(output, outputSize, "%.0f Kbps", bits / 1000.0);
    } else {
        snprintf(output, outputSize, "%.1f Mbps", bits / 1000000.0);
    }
}

double RowSortValue(const ProcessRow& row, int column) {
    switch (column) {
    case 1: return row.pid;                                                        // COL_PID
    case 2: return row.cpuPercent;                                                 // COL_CPU
    case 3: return (double)row.privateWorkingSet;                                  // COL_MEMORY
    case 4: return (double)row.diskReadBytesPerSec + row.diskWriteBytesPerSec;     // COL_DISK
    case 5: return (double)row.networkSendBytesPerSec + row.networkReceiveBytesPerSec; // COL_NETWORK
    case 6: return row.gpuPercent;                                                 // COL_GPU
    case 7: return (row.gpuPercent > 0.0f) ? (double)row.gpuEngineType : -1.0;     // COL_GPU_ENGINE
    case 8: return row.ioBytesPerSec;                                              // COL_IO
    case 9: return row.threadCount;                                                // COL_THREADS
    case 10: return row.handleCount;                                               // COL_HANDLES
    case 11: return (double)row.workingSet;                                        // COL_WORKING_SET
    case 12: return (double)row.commitBytes;                                       // COL_COMMIT
    case 13: return row.sessionId;                                                 // COL_SESSION
    case 14: return row.parentPid;                                                 // COL_PARENT_PID
    default: return 0.0;
    }
}

// Background tint for a cell, strongest for the heaviest users, like Task Manager's heat map.
// fraction is the value relative to a "very busy" reference for that column.
ImU32 HeatColor(float fraction) {
    if (fraction <= 0.005f) {
        return 0;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    return ImGui::GetColorU32(ImVec4(1.0f, 0.62f, 0.10f, 0.10f + 0.50f * fraction));
}

// Right-aligns text in the current table cell; zero values are drawn dimmed.
void TextRight(const char* text, bool dim) {
    const float width = ImGui::CalcTextSize(text).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    }
    if (dim) {
        ImGui::TextDisabled("%s", text);
    } else {
        ImGui::TextUnformatted(text);
    }
}

typedef BOOL (WINAPI *IsProcessCriticalFn)(HANDLE, PBOOL);

IsProcessCriticalFn ResolveIsProcessCritical() {
    static IsProcessCriticalFn fn = (IsProcessCriticalFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical");
    return fn;
}

// Opens a process only if it is still the one that was captured (same creation time).
HANDLE OpenCapturedProcess(DWORD pid, ULONGLONG createTime, DWORD access, DWORD* outError) {
    HANDLE process = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == NULL) {
        *outError = GetLastError();
        return NULL;
    }
    FILETIME creation, exitTime, kernelTime, userTime;
    if (createTime != 0 && GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime)) {
        const ULONGLONG actual = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
        if (actual != createTime) {
            CloseHandle(process);
            *outError = ERROR_NOT_FOUND;
            return NULL;
        }
    }
    *outError = ERROR_SUCCESS;
    return process;
}

bool QueryImagePath(DWORD pid, ULONGLONG createTime, wchar_t* path, DWORD capacity) {
    DWORD error = 0;
    HANDLE process = OpenCapturedProcess(pid, createTime, 0, &error);
    if (process == NULL) {
        return false;
    }
    DWORD length = capacity;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    return ok && length > 0;
}

} // namespace

ProcessWindow::ProcessWindow()
    : m_hwnd(NULL),
      m_notifyWindow(NULL),
      m_instance(NULL),
      m_device(NULL),
      m_deviceContext(NULL),
      m_swapChain(NULL),
      m_renderTarget(NULL),
      m_imgui(NULL),
      m_dpiScale(1.0f),
      m_styleDirty(true),
      m_inFrame(false),
      m_pendingFrames(0),
      m_lastFrameTick(0),
      m_lateFrameTick(0),
      m_lastSampleTick(0),
      m_sampleRequested(false),
      m_systemMonitor(NULL),
      m_elevated(false),
      m_gpuTotalPercent(0.0f),
      m_filtered(NULL),
      m_members(NULL),
      m_groups(NULL),
      m_lines(NULL),
      m_viewCapacity(0),
      m_filteredCount(0),
      m_groupCount(0),
      m_lineCount(0),
      m_viewDirty(true),
      m_sortColumn(COL_CPU),
      m_sortDescending(true),
      m_expandedGroups(NULL),
      m_expandedCount(0),
      m_expandedCapacity(0),
      m_selectionIsGroup(false),
      m_selectedPid(0),
      m_selectedGroupHash(0),
      m_selectedLine(-1),
      m_scrollToSelection(false),
      m_focusFilter(false),
      m_visibleLines(20),
      m_actionPids(NULL),
      m_actionCreateTimes(NULL),
      m_actionPidCount(0),
      m_actionPidCapacity(0),
      m_actionIsGroup(false),
      m_actionGroupHash(0),
      m_actionPriority(0),
      m_openContextMenu(false),
      m_openEndTaskConfirm(false),
      m_statusExpireTick(0) {
    m_iniPath[0] = '\0';
    m_configPath[0] = L'\0';
    m_filter[0] = '\0';
    m_actionLabel[0] = '\0';
    m_statusText[0] = '\0';
    ZeroMemory(&m_memoryStatus, sizeof(m_memoryStatus));
    ZeroMemory(&m_settings, sizeof(m_settings));
    m_settings.refreshMs = 1000;
    m_settings.groupByName = TRUE;
}

ProcessWindow::~ProcessWindow() {
    Close();
}

// ---------------------------------------------------------------------------------------------
// Lifetime

bool ProcessWindow::Open(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                         SystemMonitor* systemMonitor, HWND notifyWindow, const wchar_t* configPath,
                         HICON largeIcon, HICON smallIcon) {
    if (m_hwnd != NULL) {
        if (IsIconic(m_hwnd)) {
            ShowWindow(m_hwnd, SW_RESTORE);
        }
        SetForegroundWindow(m_hwnd);
        return true;
    }
    if (device == NULL || deviceContext == NULL || systemMonitor == NULL) {
        return false;
    }

    m_instance = instance;
    m_device = device;
    m_deviceContext = deviceContext;
    m_systemMonitor = systemMonitor;
    m_notifyWindow = notifyWindow;
    wcsncpy_s(m_configPath, configPath != NULL ? configPath : L"", _TRUNCATE);
    m_elevated = IsProcessElevated();
    LoadSettings();

    if (!CreateWindowAndDevice(largeIcon, smallIcon) || !CreateImGuiContext()) {
        Close();
        return false;
    }

    if (m_systemMonitor != NULL) {
        m_systemMonitor->SetProcessGpuTrackingEnabled(true);
    }
    if (m_elevated) {
        m_etw.Start(); // On failure the toolbar shows the error code.
    }

    m_lastSampleTick = 0;
    m_sampleRequested = true;
    m_viewDirty = true;
    m_selectedLine = -1;

    WINDOWPLACEMENT placement;
    ZeroMemory(&placement, sizeof(placement));
    placement.length = sizeof(placement);
    placement.showCmd = m_settings.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    placement.rcNormalPosition = m_settings.hasPlacement ? m_settings.placement : DefaultPlacement();
    SetWindowPlacement(m_hwnd, &placement);
    m_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd); // the placement may be on another monitor
    m_styleDirty = true;
    ApplyAlwaysOnTop();
    SetForegroundWindow(m_hwnd);
    RequestFrames(2);
    return true;
}

void ProcessWindow::Close() {
    if (m_systemMonitor == NULL) {
        return; // never opened, or already closed
    }
    if (m_hwnd != NULL) {
        SaveSettings();
    }

    m_etw.Stop();
    m_systemMonitor->SetProcessGpuTrackingEnabled(false);
    m_systemMonitor = NULL;

    if (m_imgui != NULL) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(previous != m_imgui ? previous : NULL);
        m_imgui = NULL;
    }

    ReleaseRenderTarget();
    if (m_swapChain != NULL) {
        m_swapChain->Release();
        m_swapChain = NULL;
    }

    HWND hwnd = m_hwnd;
    m_hwnd = NULL; // Messages sent while the window is destroyed are no longer handled.
    if (hwnd != NULL) {
        DestroyWindow(hwnd);
    }

    m_monitor.Release();
    ReleaseView();
    free(m_expandedGroups);
    free(m_actionPids);
    free(m_actionCreateTimes);
    m_expandedGroups = NULL;
    m_actionPids = NULL;
    m_actionCreateTimes = NULL;
    m_expandedCount = 0;
    m_expandedCapacity = 0;
    m_actionPidCount = 0;
    m_actionPidCapacity = 0;
    m_pendingFrames = 0;
    m_lateFrameTick = 0;
    m_statusText[0] = '\0';
}

bool ProcessWindow::CreateWindowAndDevice(HICON largeIcon, HICON smallIcon) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = m_instance;
        wc.hIcon = largeIcon;
        wc.hIconSm = smallIcon;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            return false;
        }
        classRegistered = true;
    }

    // Created hidden; Open() applies the saved placement and shows it.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                NULL, NULL, m_instance, this);
    if (hwnd == NULL) {
        return false;
    }
    m_hwnd = hwnd;

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    m_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd);
    if (m_dpiScale <= 0.0f) {
        m_dpiScale = 1.0f;
    }
    return CreateSwapChain();
}

bool ProcessWindow::CreateSwapChain() {
    IDXGIDevice* dxgiDevice = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory2* factory = NULL;
    bool ok = SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
              SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = factory->CreateSwapChainForHwnd(m_device, m_hwnd, &desc, NULL, NULL, &m_swapChain);
        if (FAILED(hr)) {
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // Windows 8.x
            hr = factory->CreateSwapChainForHwnd(m_device, m_hwnd, &desc, NULL, NULL, &m_swapChain);
        }
        ok = SUCCEEDED(hr);
        if (ok) {
            factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
        }
    }
    if (factory != NULL) factory->Release();
    if (adapter != NULL) adapter->Release();
    if (dxgiDevice != NULL) dxgiDevice->Release();
    if (ok) {
        CreateRenderTarget();
    }
    return ok;
}

void ProcessWindow::CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = NULL;
    if (m_swapChain != NULL && SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer, NULL, &m_renderTarget);
        backBuffer->Release();
    }
}

void ProcessWindow::ReleaseRenderTarget() {
    if (m_renderTarget != NULL) {
        m_renderTarget->Release();
        m_renderTarget = NULL;
    }
}

void ProcessWindow::ResizeSwapChain(UINT width, UINT height) {
    if (m_swapChain == NULL || width == 0 || height == 0) {
        return;
    }
    ReleaseRenderTarget();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

bool ProcessWindow::CreateImGuiContext() {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    m_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imgui);

    ImGuiIO& io = ImGui::GetIO();
    // Column order, widths, visibility and sort order persist here (the HUD keeps no ImGui ini).
    wchar_t iniPathWide[MAX_PATH];
    wcsncpy_s(iniPathWide, m_configPath, _TRUNCATE);
    wchar_t* lastSlash = wcsrchr(iniPathWide, L'\\');
    if (lastSlash != NULL) {
        *(lastSlash + 1) = L'\0';
        wcsncat_s(iniPathWide, L"processes_table.ini", _TRUNCATE);
        if (WideCharToMultiByte(CP_UTF8, 0, iniPathWide, -1, m_iniPath, (int)sizeof(m_iniPath), NULL, NULL) <= 0) {
            m_iniPath[0] = '\0';
        }
    }
    io.IniFilename = (m_iniPath[0] != '\0') ? m_iniPath : NULL;
    io.ConfigInputTextCursorBlink = false; // a blinking caret would need a frame every 0.5 s

    char fontPath[MAX_PATH];
    const UINT windowsLength = GetWindowsDirectoryA(fontPath, MAX_PATH);
    ImFont* font = NULL;
    if (windowsLength > 0 && windowsLength < MAX_PATH - 32) {
        strcat_s(fontPath, "\\Fonts\\segoeui.ttf");
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 1;
        font = io.Fonts->AddFontFromFileTTF(fontPath, kFontSize, &fontConfig);
    }
    if (font == NULL) {
        io.Fonts->AddFontDefault();
    }

    m_styleDirty = true;
    const bool ok = ImGui_ImplWin32_Init(m_hwnd) && ImGui_ImplDX11_Init(m_device, m_deviceContext);
    ImGui::SetCurrentContext(previous);
    return ok;
}

void ProcessWindow::ApplyStyle() {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 2.0f);
    style.ScrollbarSize = 14.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.105f, 0.115f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.24f, 0.44f, 0.76f, 0.55f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.50f, 0.82f, 0.35f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.50f, 0.82f, 0.65f);
    style.ScaleAllSizes(m_dpiScale);
    style.FontScaleDpi = m_dpiScale;
    ImGui::GetStyle() = style;
    m_styleDirty = false;
}

RECT ProcessWindow::DefaultPlacement() const {
    // Workspace coordinates (relative to the primary work area), as WINDOWPLACEMENT expects.
    RECT workArea = { 0, 0, 1280, 720 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const POINT origin = { 0, 0 };
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    const LONG areaWidth = workArea.right - workArea.left;
    const LONG areaHeight = workArea.bottom - workArea.top;
    LONG width = (LONG)(kDefaultWidth * scale);
    LONG height = (LONG)(kDefaultHeight * scale);
    if (width > areaWidth) width = areaWidth;
    if (height > areaHeight) height = areaHeight;
    RECT rect;
    rect.left = (areaWidth - width) / 2;
    rect.top = (areaHeight - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

void ProcessWindow::LoadSettings() {
    const DWORD refresh = GetPrivateProfileIntW(kSection, L"RefreshMs", 1000, m_configPath);
    m_settings.refreshMs = 1000;
    for (UINT i = 0; i < sizeof(kRefreshChoices) / sizeof(kRefreshChoices[0]); ++i) {
        if (kRefreshChoices[i] == refresh) {
            m_settings.refreshMs = refresh;
        }
    }
    m_settings.groupByName = GetPrivateProfileIntW(kSection, L"GroupByName", 1, m_configPath) ? TRUE : FALSE;
    m_settings.alwaysOnTop = GetPrivateProfileIntW(kSection, L"AlwaysOnTop", 0, m_configPath) ? TRUE : FALSE;
    m_settings.maximized = GetPrivateProfileIntW(kSection, L"Maximized", 0, m_configPath) ? TRUE : FALSE;

    // GetPrivateProfileInt can't read negative numbers (monitors left of or above the primary).
    const wchar_t* keys[4] = { L"WindowLeft", L"WindowTop", L"WindowRight", L"WindowBottom" };
    LONG values[4];
    bool present = true;
    for (int i = 0; i < 4; ++i) {
        wchar_t buffer[32];
        GetPrivateProfileStringW(kSection, keys[i], L"", buffer, 32, m_configPath);
        present = present && buffer[0] != L'\0';
        values[i] = _wtol(buffer);
    }
    RECT rect = { values[0], values[1], values[2], values[3] };
    m_settings.hasPlacement = present && rect.right - rect.left >= 200 && rect.bottom - rect.top >= 150 &&
                              MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != NULL;
    m_settings.placement = rect;
}

void ProcessWindow::SaveSettings() {
    if (m_configPath[0] == L'\0') {
        return;
    }
    wchar_t buffer[32];
    swprintf_s(buffer, L"%lu", m_settings.refreshMs);
    WritePrivateProfileStringW(kSection, L"RefreshMs", buffer, m_configPath);
    WritePrivateProfileStringW(kSection, L"GroupByName", m_settings.groupByName ? L"1" : L"0", m_configPath);
    WritePrivateProfileStringW(kSection, L"AlwaysOnTop", m_settings.alwaysOnTop ? L"1" : L"0", m_configPath);

    WINDOWPLACEMENT placement;
    placement.length = sizeof(placement);
    if (m_hwnd != NULL && GetWindowPlacement(m_hwnd, &placement)) {
        m_settings.placement = placement.rcNormalPosition;
        m_settings.maximized = (placement.showCmd == SW_SHOWMAXIMIZED) ||
                               (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
        m_settings.hasPlacement = TRUE;
        const LONG values[4] = { placement.rcNormalPosition.left, placement.rcNormalPosition.top,
                                 placement.rcNormalPosition.right, placement.rcNormalPosition.bottom };
        const wchar_t* keys[4] = { L"WindowLeft", L"WindowTop", L"WindowRight", L"WindowBottom" };
        for (int i = 0; i < 4; ++i) {
            swprintf_s(buffer, L"%ld", values[i]);
            WritePrivateProfileStringW(kSection, keys[i], buffer, m_configPath);
        }
        WritePrivateProfileStringW(kSection, L"Maximized", m_settings.maximized ? L"1" : L"0", m_configPath);
    }
}

void ProcessWindow::ApplyAlwaysOnTop() {
    if (m_hwnd != NULL) {
        SetWindowPos(m_hwnd, m_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// ---------------------------------------------------------------------------------------------
// Scheduling

void ProcessWindow::RequestFrames(int count) {
    if (m_pendingFrames < count) {
        m_pendingFrames = count;
    }
}

void ProcessWindow::OnInput() {
    RequestFrames(kInputFrames);
    m_lateFrameTick = GetTickCount64() + kLateFrameDelayMs;
}

DWORD ProcessWindow::Tick() {
    if (m_hwnd == NULL) {
        return INFINITE;
    }

    const ULONGLONG now = GetTickCount64();
    if (IsIconic(m_hwnd)) {
        // Nothing is visible, so nothing is sampled or drawn; restoring sends WM_SIZE.
        m_pendingFrames = 0;
        m_lateFrameTick = 0;
        return INFINITE;
    }

    const bool sampleDue = m_settings.refreshMs != 0 &&
                           (m_lastSampleTick == 0 || now - m_lastSampleTick >= m_settings.refreshMs);
    if (sampleDue || m_sampleRequested) {
        SampleData();
        m_lastSampleTick = now;
        m_sampleRequested = false;
        RequestFrames(1);
    }

    if (m_lateFrameTick != 0 && now >= m_lateFrameTick) {
        m_lateFrameTick = 0;
        RequestFrames(1);
    }

    const ULONGLONG sinceFrame = now - m_lastFrameTick;
    if (m_pendingFrames > 0 && sinceFrame >= kMinFrameIntervalMs) {
        --m_pendingFrames;
        m_lastFrameTick = now;
        RenderFrame();
    }

    if (m_hwnd == NULL) {
        return INFINITE; // The frame closed the window.
    }

    ULONGLONG wait = INFINITE;
    if (m_settings.refreshMs != 0) {
        const ULONGLONG next = m_lastSampleTick + m_settings.refreshMs;
        wait = (next > now) ? next - now : 0;
    }
    if (m_pendingFrames > 0) {
        const ULONGLONG elapsed = now - m_lastFrameTick;
        const ULONGLONG frameWait = (elapsed >= kMinFrameIntervalMs) ? 0 : kMinFrameIntervalMs - elapsed;
        if (frameWait < wait) wait = frameWait;
    }
    if (m_lateFrameTick != 0) {
        const ULONGLONG lateWait = (m_lateFrameTick > now) ? m_lateFrameTick - now : 0;
        if (lateWait < wait) wait = lateWait;
    }
    return (wait == INFINITE) ? INFINITE : (DWORD)wait;
}

// ---------------------------------------------------------------------------------------------
// Data and view

void ProcessWindow::SampleData() {
    m_monitor.Sample();
    if (m_systemMonitor != NULL) {
        const ProcessGpuUsage* gpuUsage = NULL;
        const UINT gpuCount = m_systemMonitor->GetProcessGpuUsage(&gpuUsage);
        m_monitor.ApplyGpuUsage(gpuUsage, gpuCount);

        m_gpuTotalPercent = 0.0f;
        const UINT adapterRows = m_systemMonitor->GetDisplayedGPUCount();
        for (UINT i = 0; i < adapterRows; ++i) {
            GPUMetricsSnapshot snapshot;
            if (m_systemMonitor->GetDisplayedGPUSnapshot(i, &snapshot) && snapshot.utilizationAvailable &&
                snapshot.usagePercent > m_gpuTotalPercent) {
                m_gpuTotalPercent = snapshot.usagePercent;
            }
        }
    }
    if (m_etw.IsRunning()) {
        const ProcessIoUsage* ioUsage = NULL;
        const UINT ioCount = m_etw.Drain(&ioUsage);
        m_monitor.ApplyIoUsage(ioUsage, ioCount);
    }
    m_memoryStatus.dwLength = sizeof(m_memoryStatus);
    GlobalMemoryStatusEx(&m_memoryStatus);
    m_viewDirty = true;
    RebuildView();
}

bool ProcessWindow::EnsureViewCapacity(UINT rowCount) {
    if (rowCount <= m_viewCapacity) {
        return true;
    }
    const UINT capacity = rowCount + 64;
    int* filtered = (int*)realloc(m_filtered, capacity * sizeof(int));
    if (filtered != NULL) m_filtered = filtered;
    int* members = (int*)realloc(m_members, capacity * sizeof(int));
    if (members != NULL) m_members = members;
    Group* groups = (Group*)realloc(m_groups, capacity * sizeof(Group));
    if (groups != NULL) m_groups = groups;
    Line* lines = (Line*)realloc(m_lines, capacity * 2 * sizeof(Line)); // a group line plus its members
    if (lines != NULL) m_lines = lines;
    if (filtered == NULL || members == NULL || groups == NULL || lines == NULL) {
        return false;
    }
    m_viewCapacity = capacity;
    return true;
}

void ProcessWindow::ReleaseView() {
    free(m_filtered);
    free(m_members);
    free(m_groups);
    free(m_lines);
    m_filtered = NULL;
    m_members = NULL;
    m_groups = NULL;
    m_lines = NULL;
    m_viewCapacity = 0;
    m_filteredCount = 0;
    m_groupCount = 0;
    m_lineCount = 0;
    m_selectedLine = -1;
}

bool ProcessWindow::MatchesFilter(const ProcessRow& row, const char* loweredFilter, bool numericFilter) const {
    if (loweredFilter[0] == '\0' || ContainsNoCase(row.name, loweredFilter)) {
        return true;
    }
    if (numericFilter) {
        char pidText[16];
        snprintf(pidText, sizeof(pidText), "%lu", row.pid);
        return strstr(pidText, loweredFilter) != NULL;
    }
    return false;
}

int ProcessWindow::CompareRowIndicesByName(const void* left, const void* right) {
    const ProcessRow& a = s_sortRows[*(const int*)left];
    const ProcessRow& b = s_sortRows[*(const int*)right];
    const int byName = _stricmp(a.name, b.name);
    if (byName != 0) {
        return byName;
    }
    return (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
}

int ProcessWindow::CompareRowIndices(const void* left, const void* right) {
    const ProcessRow& a = s_sortRows[*(const int*)left];
    const ProcessRow& b = s_sortRows[*(const int*)right];
    int result = 0;
    if (s_sortColumn == COL_NAME) {
        result = _stricmp(a.name, b.name);
    } else {
        const double va = RowSortValue(a, s_sortColumn);
        const double vb = RowSortValue(b, s_sortColumn);
        result = (va < vb) ? -1 : (va > vb ? 1 : 0);
    }
    if (s_sortDescending) {
        result = -result;
    }
    if (result == 0) {
        result = _stricmp(a.name, b.name);
    }
    if (result == 0) {
        result = (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0);
    }
    return result;
}

int ProcessWindow::CompareGroups(const void* left, const void* right) {
    const Group& a = *(const Group*)left;
    const Group& b = *(const Group*)right;
    double va = 0.0;
    double vb = 0.0;
    int result = 0;
    switch (s_sortColumn) {
    case COL_NAME: result = _stricmp(a.name, b.name); break;
    case COL_PID: va = a.memberCount; vb = b.memberCount; break;
    case COL_CPU: va = a.cpuPercent; vb = b.cpuPercent; break;
    case COL_MEMORY: va = (double)a.privateWorkingSet; vb = (double)b.privateWorkingSet; break;
    case COL_DISK: va = a.diskBytesPerSec; vb = b.diskBytesPerSec; break;
    case COL_NETWORK: va = a.networkBytesPerSec; vb = b.networkBytesPerSec; break;
    case COL_GPU: va = a.gpuPercent; vb = b.gpuPercent; break;
    case COL_GPU_ENGINE: va = (a.gpuPercent > 0.0f) ? a.gpuEngineType : -1; vb = (b.gpuPercent > 0.0f) ? b.gpuEngineType : -1; break;
    case COL_IO: va = a.ioBytesPerSec; vb = b.ioBytesPerSec; break;
    case COL_THREADS: va = a.threadCount; vb = b.threadCount; break;
    case COL_HANDLES: va = a.handleCount; vb = b.handleCount; break;
    case COL_WORKING_SET: va = (double)a.workingSet; vb = (double)b.workingSet; break;
    case COL_COMMIT: va = (double)a.commitBytes; vb = (double)b.commitBytes; break;
    case COL_SESSION: va = a.sessionId; vb = b.sessionId; break;
    default: break;
    }
    if (s_sortColumn != COL_NAME) {
        result = (va < vb) ? -1 : (va > vb ? 1 : 0);
    }
    if (s_sortDescending) {
        result = -result;
    }
    if (result == 0) {
        result = _stricmp(a.name, b.name);
    }
    return result;
}

void ProcessWindow::SortRows(int* indices, UINT count) const {
    if (count > 1) {
        qsort(indices, count, sizeof(int), CompareRowIndices);
    }
}

void ProcessWindow::SortGroups() {
    if (m_groupCount > 1) {
        qsort(m_groups, m_groupCount, sizeof(Group), CompareGroups);
    }
}

void ProcessWindow::RebuildView() {
    m_viewDirty = false;
    const UINT rowCount = m_monitor.GetRowCount();
    const ProcessRow* rows = m_monitor.GetRows();
    if (!EnsureViewCapacity(rowCount)) {
        m_lineCount = 0;
        return;
    }

    s_sortRows = rows;
    s_sortColumn = m_sortColumn;
    s_sortDescending = m_sortDescending;

    char loweredFilter[sizeof(m_filter)];
    bool numericFilter = m_filter[0] != '\0';
    size_t length = 0;
    for (const char* c = m_filter; *c != '\0' && length + 1 < sizeof(loweredFilter); ++c) {
        if (*c == ' ' && length == 0) {
            continue; // ignore leading spaces
        }
        loweredFilter[length++] = ToLowerAscii(*c);
        numericFilter = numericFilter && (*c >= '0' && *c <= '9');
    }
    while (length > 0 && loweredFilter[length - 1] == ' ') {
        --length;
    }
    loweredFilter[length] = '\0';

    m_filteredCount = 0;
    for (UINT i = 0; i < rowCount; ++i) {
        if (MatchesFilter(rows[i], loweredFilter, numericFilter)) {
            m_filtered[m_filteredCount++] = (int)i;
        }
    }

    m_groupCount = 0;
    if (m_settings.groupByName) {
        memcpy(m_members, m_filtered, m_filteredCount * sizeof(int));
        if (m_filteredCount > 1) {
            qsort(m_members, m_filteredCount, sizeof(int), CompareRowIndicesByName);
        }
        UINT start = 0;
        while (start < m_filteredCount) {
            const ProcessRow& first = rows[m_members[start]];
            UINT end = start + 1;
            while (end < m_filteredCount && _stricmp(rows[m_members[end]].name, first.name) == 0) {
                ++end;
            }

            Group& group = m_groups[m_groupCount++];
            ZeroMemory(&group, sizeof(group));
            group.firstMember = (int)start;
            group.memberCount = (int)(end - start);
            group.name = first.name;
            group.nameHash = HashNameLowercase(first.name);
            group.sessionId = first.sessionId;
            float busiestGpu = -1.0f;
            for (UINT m = start; m < end; ++m) {
                const ProcessRow& row = rows[m_members[m]];
                group.cpuPercent += row.cpuPercent;
                group.privateWorkingSet += row.privateWorkingSet;
                group.workingSet += row.workingSet;
                group.commitBytes += row.commitBytes;
                group.diskBytesPerSec += row.diskReadBytesPerSec + row.diskWriteBytesPerSec;
                group.networkBytesPerSec += row.networkSendBytesPerSec + row.networkReceiveBytesPerSec;
                group.ioBytesPerSec += row.ioBytesPerSec;
                group.gpuPercent += row.gpuPercent;
                group.threadCount += row.threadCount;
                group.handleCount += row.handleCount;
                if (row.gpuPercent > busiestGpu) {
                    busiestGpu = row.gpuPercent;
                    group.gpuEngineType = row.gpuEngineType;
                }
            }
            if (group.gpuPercent > 100.0f) {
                group.gpuPercent = 100.0f;
            }
            if (group.cpuPercent > 100.0f) {
                group.cpuPercent = 100.0f;
            }
            SortRows(m_members + start, end - start);
            start = end;
        }
        SortGroups();
    } else {
        SortRows(m_filtered, m_filteredCount);
    }

    BuildLines();
    LocateSelection();
}

void ProcessWindow::BuildLines() {
    m_lineCount = 0;
    if (!m_settings.groupByName) {
        for (UINT i = 0; i < m_filteredCount; ++i) {
            m_lines[m_lineCount].kind = LINE_PROCESS;
            m_lines[m_lineCount].index = m_filtered[i];
            ++m_lineCount;
        }
        return;
    }

    for (UINT g = 0; g < m_groupCount; ++g) {
        const Group& group = m_groups[g];
        if (group.memberCount == 1) {
            m_lines[m_lineCount].kind = LINE_PROCESS;
            m_lines[m_lineCount].index = m_members[group.firstMember];
            ++m_lineCount;
            continue;
        }
        m_lines[m_lineCount].kind = LINE_GROUP;
        m_lines[m_lineCount].index = (int)g;
        ++m_lineCount;
        if (IsExpanded(group.nameHash)) {
            for (int m = 0; m < group.memberCount; ++m) {
                m_lines[m_lineCount].kind = LINE_MEMBER;
                m_lines[m_lineCount].index = m_members[group.firstMember + m];
                ++m_lineCount;
            }
        }
    }
}

void ProcessWindow::LocateSelection() {
    m_selectedLine = -1;
    if (m_selectedPid == 0 && m_selectedGroupHash == 0) {
        return;
    }
    const ProcessRow* rows = m_monitor.GetRows();
    for (UINT i = 0; i < m_lineCount; ++i) {
        const Line& line = m_lines[i];
        if (m_selectionIsGroup) {
            if (line.kind == LINE_GROUP && m_groups[line.index].nameHash == m_selectedGroupHash) {
                m_selectedLine = (int)i;
                return;
            }
        } else if (line.kind != LINE_GROUP && rows[line.index].pid == m_selectedPid) {
            m_selectedLine = (int)i;
            return;
        }
    }
}

bool ProcessWindow::IsExpanded(DWORD nameHash) const {
    for (UINT i = 0; i < m_expandedCount; ++i) {
        if (m_expandedGroups[i] == nameHash) {
            return true;
        }
    }
    return false;
}

void ProcessWindow::SetExpanded(DWORD nameHash, bool expanded) {
    for (UINT i = 0; i < m_expandedCount; ++i) {
        if (m_expandedGroups[i] == nameHash) {
            if (!expanded) {
                m_expandedGroups[i] = m_expandedGroups[--m_expandedCount];
            }
            m_viewDirty = true;
            return;
        }
    }
    if (!expanded) {
        return;
    }
    if (m_expandedCount == m_expandedCapacity) {
        const UINT capacity = m_expandedCapacity + 16;
        DWORD* grown = (DWORD*)realloc(m_expandedGroups, capacity * sizeof(DWORD));
        if (grown == NULL) {
            return;
        }
        m_expandedGroups = grown;
        m_expandedCapacity = capacity;
    }
    m_expandedGroups[m_expandedCount++] = nameHash;
    m_viewDirty = true;
}

void ProcessWindow::SelectLine(int lineIndex) {
    if (lineIndex < 0 || lineIndex >= (int)m_lineCount) {
        return;
    }
    const Line& line = m_lines[lineIndex];
    m_selectedLine = lineIndex;
    if (line.kind == LINE_GROUP) {
        m_selectionIsGroup = true;
        m_selectedGroupHash = m_groups[line.index].nameHash;
        m_selectedPid = 0;
    } else {
        m_selectionIsGroup = false;
        m_selectedPid = m_monitor.GetRows()[line.index].pid;
        m_selectedGroupHash = 0;
    }
}

// Captures the selected process (or every process of the selected group) as action targets.
bool ProcessWindow::CollectSelectionPids() {
    m_actionPidCount = 0;
    m_actionPriority = 0;
    if (m_selectedLine < 0) {
        return false;
    }
    const ProcessRow* rows = m_monitor.GetRows();
    const Line& line = m_lines[m_selectedLine];
    const int first = (line.kind == LINE_GROUP) ? m_groups[line.index].firstMember : 0;
    const UINT count = (line.kind == LINE_GROUP) ? (UINT)m_groups[line.index].memberCount : 1;

    if (count > m_actionPidCapacity) {
        DWORD* pids = (DWORD*)realloc(m_actionPids, count * sizeof(DWORD));
        if (pids != NULL) m_actionPids = pids;
        ULONGLONG* times = (ULONGLONG*)realloc(m_actionCreateTimes, count * sizeof(ULONGLONG));
        if (times != NULL) m_actionCreateTimes = times;
        if (pids == NULL || times == NULL) {
            return false;
        }
        m_actionPidCapacity = count;
    }

    for (UINT i = 0; i < count; ++i) {
        const ProcessRow& row = rows[(line.kind == LINE_GROUP) ? m_members[first + (int)i] : line.index];
        m_actionPids[i] = row.pid;
        m_actionCreateTimes[i] = row.createTime;
    }
    m_actionPidCount = count;
    m_actionIsGroup = (line.kind == LINE_GROUP);
    m_actionGroupHash = m_actionIsGroup ? m_groups[line.index].nameHash : 0;

    if (m_actionIsGroup) {
        snprintf(m_actionLabel, sizeof(m_actionLabel), "%s (%u processes)", m_groups[line.index].name, count);
    } else {
        const ProcessRow& row = rows[line.index];
        snprintf(m_actionLabel, sizeof(m_actionLabel), "%s (PID %lu)", row.name, row.pid);
        DWORD error = 0;
        HANDLE process = OpenCapturedProcess(row.pid, row.createTime, 0, &error);
        if (process != NULL) {
            m_actionPriority = GetPriorityClass(process);
            CloseHandle(process);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing

void ProcessWindow::RenderFrame() {
    if (m_hwnd == NULL || m_imgui == NULL || m_renderTarget == NULL || m_inFrame) {
        return;
    }
    RECT client;
    if (!GetClientRect(m_hwnd, &client) || client.right <= 0 || client.bottom <= 0) {
        return;
    }

    m_inFrame = true;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_imgui);

    if (m_styleDirty) {
        ApplyStyle();
    }
    if (m_viewDirty) {
        RebuildView();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();

    if (m_hwnd != NULL && m_renderTarget != NULL) {
        const float clearColor[4] = { 0.105f, 0.105f, 0.115f, 1.0f };
        m_deviceContext->OMSetRenderTargets(1, &m_renderTarget, NULL);
        m_deviceContext->ClearRenderTargetView(m_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_swapChain->Present(0, 0); // frames are already capped by Tick(); don't block the HUD loop
    }

    ImGui::SetCurrentContext(previous);
    m_inFrame = false;
}

void ProcessWindow::DrawUi() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("##processes", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);

    HandleKeyboard();
    DrawToolbar();
    DrawTable();
    DrawStatusBar();
    DrawContextMenu();
    DrawEndTaskConfirm();

    ImGui::End();
}

void ProcessWindow::DrawToolbar() {
    const float scale = m_dpiScale;

    if (m_focusFilter) {
        ImGui::SetKeyboardFocusHere();
        m_focusFilter = false;
    }
    ImGui::SetNextItemWidth(250.0f * scale);
    if (ImGui::InputTextWithHint("##filter", "Filter by name or PID (Ctrl+F)", m_filter, sizeof(m_filter))) {
        m_viewDirty = true;
    }

    ImGui::SameLine();
    bool groupByName = m_settings.groupByName != FALSE;
    if (ImGui::Checkbox("Group by name", &groupByName)) {
        m_settings.groupByName = groupByName ? TRUE : FALSE;
        m_viewDirty = true;
        SaveSettings();
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Update:");
    ImGui::SameLine();
    int currentChoice = 1;
    for (int i = 0; i < 4; ++i) {
        if (kRefreshChoices[i] == m_settings.refreshMs) {
            currentChoice = i;
        }
    }
    ImGui::SetNextItemWidth(118.0f * scale);
    if (ImGui::BeginCombo("##refresh", kRefreshLabels[currentChoice])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kRefreshLabels[i], i == currentChoice)) {
                m_settings.refreshMs = kRefreshChoices[i];
                SaveSettings();
            }
        }
        ImGui::EndCombo();
    }
    if (m_settings.refreshMs == 0) {
        ImGui::SameLine();
        if (ImGui::Button("Refresh (F5)")) {
            m_sampleRequested = true;
        }
    }

    ImGui::SameLine();
    bool alwaysOnTop = m_settings.alwaysOnTop != FALSE;
    if (ImGui::Checkbox("Always on top", &alwaysOnTop)) {
        m_settings.alwaysOnTop = alwaysOnTop ? TRUE : FALSE;
        ApplyAlwaysOnTop();
        SaveSettings();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(m_selectedLine < 0);
    if (ImGui::Button("End task")) {
        if (CollectSelectionPids()) {
            m_openEndTaskConfirm = true;
        }
    }
    ImGui::EndDisabled();

    // Administrator state, right-aligned when there is room.
    char adminText[128];
    ImVec4 adminColor(0.55f, 0.85f, 0.55f, 1.0f);
    bool showRestartButton = false;
    if (m_etw.IsRunning()) {
        snprintf(adminText, sizeof(adminText), "Administrator: disk and network on");
    } else if (m_elevated) {
        snprintf(adminText, sizeof(adminText), "Disk/network unavailable (ETW error %lu)", m_etw.GetStartError());
        adminColor = ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
    } else {
        snprintf(adminText, sizeof(adminText), "Restart as administrator");
        showRestartButton = true;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float adminWidth = ImGui::CalcTextSize(adminText).x + (showRestartButton ? style.FramePadding.x * 2.0f : 0.0f);
    ImGui::SameLine();
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > adminWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - adminWidth);
    } else {
        ImGui::NewLine();
    }
    if (showRestartButton) {
        if (ImGui::Button(adminText) && m_notifyWindow != NULL) {
            PostMessageW(m_notifyWindow, WM_COREGAZE_RESTART_ELEVATED, 0, 0);
        }
        ImGui::SetItemTooltip("Per-process Disk and Network need administrator rights\n"
                              "(Windows only lets administrators read the kernel's I/O events).\n"
                              "CoreGaze restarts elevated after a UAC prompt.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(adminColor, "%s", adminText);
        if (m_etw.IsRunning()) {
            ImGui::SetItemTooltip("Kernel ETW session active while this window is open.\nEvents lost: %lu, dropped: %llu",
                                  m_etw.GetLostEventCount(), m_etw.GetDroppedEventCount());
        } else if (m_elevated) {
            ImGui::SetItemTooltip("The kernel ETW session could not be started.\n"
                                  "Another tool may be using too many kernel sessions.");
        }
    }
}

void ProcessWindow::DrawTable() {
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_SizingFixedFit;
    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= statusHeight;
    if (size.y < ImGui::GetFrameHeight() * 3.0f) {
        size.y = ImGui::GetFrameHeight() * 3.0f;
    }
    if (!ImGui::BeginTable("processes", COL_COUNT, flags, size)) {
        return;
    }

    for (int c = 0; c < COL_COUNT; ++c) {
        ImGui::TableSetupColumn(kColumns[c].name, kColumns[c].flags, kColumns[c].width * m_dpiScale, (ImGuiID)c);
    }
    ImGui::TableSetupScrollFreeze(0, 2); // header + totals row stay visible

    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if (sortSpecs != NULL && sortSpecs->SpecsDirty) {
        if (sortSpecs->SpecsCount > 0) {
            m_sortColumn = (int)sortSpecs->Specs[0].ColumnUserID;
            m_sortDescending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        }
        sortSpecs->SpecsDirty = false;
        RebuildView();
    }

    // Header row with explanations for the columns whose meaning differs from Task Manager's.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < COL_COUNT; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) {
            continue;
        }
        ImGui::PushID(c);
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
        const char* tooltip = NULL;
        switch (c) {
        case COL_CPU: tooltip = "Share of all logical processors, computed like Task Manager."; break;
        case COL_MEMORY: tooltip = "Private working set (Task Manager's Memory column)."; break;
        case COL_DISK:
            tooltip = m_etw.IsRunning() ? "Disk reads + writes (kernel ETW)."
                                        : "Needs administrator rights: use 'Restart as administrator'.";
            break;
        case COL_NETWORK:
            tooltip = m_etw.IsRunning() ? "TCP + UDP sent and received (kernel ETW)."
                                        : "Needs administrator rights: use 'Restart as administrator'.";
            break;
        case COL_GPU: tooltip = "Busiest GPU engine used by the process."; break;
        case COL_IO: tooltip = "All I/O: files, pipes, devices and network. Works without administrator rights."; break;
        case COL_WORKING_SET: tooltip = "Private + shared memory currently in RAM."; break;
        case COL_COMMIT: tooltip = "Private bytes (commit charge)."; break;
        default: break;
        }
        if (tooltip != NULL) {
            ImGui::SetItemTooltip("%s", tooltip);
        }
        ImGui::PopID();
    }

    const float rowHeight = ImGui::GetTextLineHeight();
    DrawTotalsRow(rowHeight);

    ImGuiListClipper clipper;
    clipper.Begin((int)m_lineCount);
    if (m_scrollToSelection && m_selectedLine >= 0 && m_selectedLine < (int)m_lineCount) {
        clipper.IncludeItemByIndex(m_selectedLine);
    }
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            DrawLine(i, rowHeight);
        }
    }
    m_scrollToSelection = false;

    const float rowPitch = rowHeight + ImGui::GetStyle().CellPadding.y * 2.0f;
    m_visibleLines = (rowPitch > 0.0f) ? (int)(size.y / rowPitch) - 3 : 10;
    if (m_visibleLines < 1) {
        m_visibleLines = 1;
    }
    ImGui::EndTable();
}

void ProcessWindow::DrawTotalsRow(float rowHeight) {
    const ProcessTotals& totals = m_monitor.GetTotals();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.20f, 0.22f, 0.27f, 1.0f)));

    char text[64];
    for (int c = 0; c < COL_COUNT; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) {
            continue;
        }
        text[0] = '\0';
        switch (c) {
        case COL_NAME:
            if (m_filteredCount != totals.processCount) {
                snprintf(text, sizeof(text), "%u of %u processes", m_filteredCount, totals.processCount);
            } else {
                snprintf(text, sizeof(text), "%u processes", totals.processCount);
            }
            ImGui::TextUnformatted(text);
            continue;
        case COL_CPU: snprintf(text, sizeof(text), "%.0f%%", totals.cpuPercent); break;
        case COL_MEMORY: snprintf(text, sizeof(text), "%lu%%", m_memoryStatus.dwMemoryLoad); break;
        case COL_DISK:
            if (m_etw.IsRunning()) FormatByteRate(totals.diskBytesPerSec, text, sizeof(text));
            else snprintf(text, sizeof(text), "-");
            break;
        case COL_NETWORK:
            if (m_etw.IsRunning()) FormatBitRate(totals.networkBytesPerSec, text, sizeof(text));
            else snprintf(text, sizeof(text), "-");
            break;
        case COL_GPU: snprintf(text, sizeof(text), "%.0f%%", m_gpuTotalPercent); break;
        case COL_IO: FormatByteRate(totals.ioBytesPerSec, text, sizeof(text)); break;
        case COL_THREADS: snprintf(text, sizeof(text), "%u", totals.threadCount); break;
        case COL_HANDLES: snprintf(text, sizeof(text), "%u", totals.handleCount); break;
        case COL_COMMIT:
            snprintf(text, sizeof(text), "%.1f GB",
                     (double)(m_memoryStatus.ullTotalPageFile - m_memoryStatus.ullAvailPageFile) / (1024.0 * 1024.0 * 1024.0));
            break;
        default: break;
        }
        if (text[0] != '\0') {
            TextRight(text, false);
        }
    }
}

void ProcessWindow::DrawLine(int lineIndex, float rowHeight) {
    const Line& line = m_lines[lineIndex];
    const ProcessRow* rows = m_monitor.GetRows();
    const Group* group = (line.kind == LINE_GROUP) ? &m_groups[line.index] : NULL;
    const ProcessRow* row = (group == NULL) ? &rows[line.index] : NULL;
    const bool grouped = m_settings.groupByName != FALSE;
    const bool selected = (lineIndex == m_selectedLine);
    const bool etw = m_etw.IsRunning();

    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
    ImGui::PushID(line.kind);
    ImGui::PushID(group != NULL ? (int)group->nameHash : (int)row->pid);

    // Name cell: expand arrow for groups, then a selectable spanning the whole row.
    ImGui::TableSetColumnIndex(COL_NAME);
    const float arrowSize = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    if (grouped) {
        if (group != NULL) {
            const bool expanded = IsExpanded(group->nameHash);
            if (ImGui::InvisibleButton("##toggle", ImVec2(arrowSize, rowHeight))) {
                SetExpanded(group->nameHash, !expanded);
            }
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImU32 color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            ImGui::RenderArrow(ImGui::GetWindowDrawList(), ImVec2(min.x + arrowSize * 0.15f, min.y + arrowSize * 0.1f),
                               color, expanded ? ImGuiDir_Down : ImGuiDir_Right, 0.8f);
            ImGui::SameLine(0.0f, spacing);
        } else {
            const float indent = arrowSize + spacing + ((line.kind == LINE_MEMBER) ? arrowSize : 0.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        }
    }

    char label[160];
    if (group != NULL) {
        snprintf(label, sizeof(label), "%s (%d)", group->name, group->memberCount);
    } else {
        snprintf(label, sizeof(label), "%s", row->name);
    }
    if (ImGui::Selectable(label, selected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(0.0f, rowHeight))) {
        SelectLine(lineIndex);
        if (group != NULL && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            SetExpanded(group->nameHash, !IsExpanded(group->nameHash));
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        SelectLine(lineIndex);
        if (CollectSelectionPids()) {
            m_openContextMenu = true;
        }
    }
    if (selected && m_scrollToSelection) {
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    }

    // Values, from the group aggregate or the process.
    const float cpu = group ? group->cpuPercent : row->cpuPercent;
    const ULONGLONG memory = group ? group->privateWorkingSet : row->privateWorkingSet;
    const float disk = group ? group->diskBytesPerSec : row->diskReadBytesPerSec + row->diskWriteBytesPerSec;
    const float network = group ? group->networkBytesPerSec : row->networkSendBytesPerSec + row->networkReceiveBytesPerSec;
    const float gpu = group ? group->gpuPercent : row->gpuPercent;
    const BYTE gpuEngine = group ? group->gpuEngineType : row->gpuEngineType;
    const float io = group ? group->ioBytesPerSec : row->ioBytesPerSec;
    const double totalRam = (m_memoryStatus.ullTotalPhys > 0) ? (double)m_memoryStatus.ullTotalPhys : 1.0;

    char text[64];
    for (int c = 1; c < COL_COUNT; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) {
            continue;
        }
        switch (c) {
        case COL_PID:
            if (row != NULL) {
                snprintf(text, sizeof(text), "%lu", row->pid);
                TextRight(text, false);
            }
            break;
        case COL_CPU:
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor(cpu / 25.0f));
            FormatPercent(cpu, text, sizeof(text));
            TextRight(text, cpu < 0.05f);
            break;
        case COL_MEMORY:
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor((float)((double)memory / totalRam * 4.0)));
            FormatMegabytes(memory, text, sizeof(text));
            TextRight(text, false);
            break;
        case COL_DISK:
            if (!etw) {
                TextRight("-", true);
                break;
            }
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor(disk / (50.0f * 1024.0f * 1024.0f)));
            FormatByteRate(disk, text, sizeof(text));
            TextRight(text, disk < 512.0f);
            if (row != NULL && disk >= 512.0f && ImGui::IsItemHovered()) {
                char readText[32], writeText[32];
                FormatByteRate(row->diskReadBytesPerSec, readText, sizeof(readText));
                FormatByteRate(row->diskWriteBytesPerSec, writeText, sizeof(writeText));
                ImGui::SetTooltip("Read %s\nWrite %s", readText, writeText);
            }
            break;
        case COL_NETWORK:
            if (!etw) {
                TextRight("-", true);
                break;
            }
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor(network / (12.5f * 1024.0f * 1024.0f)));
            FormatBitRate(network, text, sizeof(text));
            TextRight(text, network < 64.0f);
            if (row != NULL && network >= 64.0f && ImGui::IsItemHovered()) {
                char sendText[32], receiveText[32];
                FormatBitRate(row->networkSendBytesPerSec, sendText, sizeof(sendText));
                FormatBitRate(row->networkReceiveBytesPerSec, receiveText, sizeof(receiveText));
                ImGui::SetTooltip("Send %s\nReceive %s", sendText, receiveText);
            }
            break;
        case COL_GPU:
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor(gpu / 50.0f));
            FormatPercent(gpu, text, sizeof(text));
            TextRight(text, gpu < 0.05f);
            break;
        case COL_GPU_ENGINE:
            if (gpu >= 0.05f) {
                ImGui::TextUnformatted(SystemMonitor::GetGpuEngineLabel(gpuEngine));
            }
            break;
        case COL_IO:
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, HeatColor(io / (50.0f * 1024.0f * 1024.0f)));
            FormatByteRate(io, text, sizeof(text));
            TextRight(text, io < 512.0f);
            break;
        case COL_THREADS:
            snprintf(text, sizeof(text), "%lu", group ? group->threadCount : row->threadCount);
            TextRight(text, false);
            break;
        case COL_HANDLES:
            snprintf(text, sizeof(text), "%lu", group ? group->handleCount : row->handleCount);
            TextRight(text, false);
            break;
        case COL_WORKING_SET:
            FormatMegabytes(group ? group->workingSet : row->workingSet, text, sizeof(text));
            TextRight(text, false);
            break;
        case COL_COMMIT:
            FormatMegabytes(group ? group->commitBytes : row->commitBytes, text, sizeof(text));
            TextRight(text, false);
            break;
        case COL_SESSION:
            snprintf(text, sizeof(text), "%lu", group ? group->sessionId : row->sessionId);
            TextRight(text, false);
            break;
        case COL_PARENT_PID:
            if (row != NULL) {
                snprintf(text, sizeof(text), "%lu", row->parentPid);
                TextRight(text, false);
            }
            break;
        default:
            break;
        }
    }

    ImGui::PopID();
    ImGui::PopID();
}

void ProcessWindow::DrawContextMenu() {
    if (m_openContextMenu) {
        ImGui::OpenPopup("##rowmenu");
        m_openContextMenu = false;
    }
    if (!ImGui::BeginPopup("##rowmenu")) {
        return;
    }

    ImGui::TextDisabled("%s", m_actionLabel);
    ImGui::Separator();
    if (m_actionIsGroup) {
        const bool expanded = IsExpanded(m_actionGroupHash);
        if (ImGui::MenuItem(expanded ? "Collapse" : "Expand", "Enter")) {
            SetExpanded(m_actionGroupHash, !expanded);
        }
    }
    if (ImGui::MenuItem(m_actionIsGroup ? "End task (all)" : "End task", "Del")) {
        m_openEndTaskConfirm = true;
    }
    if (ImGui::BeginMenu("Set priority")) {
        struct PriorityChoice { const char* label; DWORD value; };
        const PriorityChoice choices[] = {
            { "High", HIGH_PRIORITY_CLASS },
            { "Above normal", ABOVE_NORMAL_PRIORITY_CLASS },
            { "Normal", NORMAL_PRIORITY_CLASS },
            { "Below normal", BELOW_NORMAL_PRIORITY_CLASS },
            { "Low", IDLE_PRIORITY_CLASS },
        };
        for (int i = 0; i < 5; ++i) {
            if (ImGui::MenuItem(choices[i].label, NULL, !m_actionIsGroup && m_actionPriority == choices[i].value)) {
                SetSelectedPriority(choices[i].value);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open file location") && m_actionPidCount > 0) {
        OpenFileLocation(m_actionPids[0]);
    }
    if (ImGui::MenuItem("Properties") && m_actionPidCount > 0) {
        OpenProperties(m_actionPids[0]);
    }
    if (ImGui::MenuItem("Copy", "Ctrl+C")) {
        CopySelectionToClipboard();
    }
    ImGui::EndPopup();
}

void ProcessWindow::DrawEndTaskConfirm() {
    if (m_openEndTaskConfirm) {
        ImGui::OpenPopup("End task?");
        m_openEndTaskConfirm = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("End task?", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::Text("End %s?", m_actionLabel);
    ImGui::TextDisabled("Unsaved work in %s will be lost.", m_actionPidCount > 1 ? "these processes" : "this process");
    ImGui::Spacing();
    if (ImGui::Button("End task") || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        EndSelectedTasks();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ProcessWindow::DrawStatusBar() {
    const ProcessTotals& totals = m_monitor.GetTotals();
    const double gigabyte = 1024.0 * 1024.0 * 1024.0;
    const double usedGb = (double)(m_memoryStatus.ullTotalPhys - m_memoryStatus.ullAvailPhys) / gigabyte;
    const double totalGb = (double)m_memoryStatus.ullTotalPhys / gigabyte;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Processes %u   Threads %u   Handles %u   CPU %.0f%%   Memory %.1f / %.1f GB (%lu%%)%s",
                        totals.processCount, totals.threadCount, totals.handleCount, totals.cpuPercent,
                        usedGb, totalGb, m_memoryStatus.dwMemoryLoad,
                        m_settings.refreshMs == 0 ? "   Updates paused" : "");
    if (m_statusText[0] != '\0') {
        if (GetTickCount64() >= m_statusExpireTick) {
            m_statusText[0] = '\0';
        } else {
            ImGui::SameLine(0.0f, 24.0f * m_dpiScale);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s", m_statusText);
        }
    }
}

void ProcessWindow::HandleKeyboard() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        m_focusFilter = true;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        m_sampleRequested = true;
    }
    if (io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && m_filter[0] != '\0') {
            m_filter[0] = '\0';
            m_viewDirty = true;
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && m_filter[0] != '\0') {
        m_filter[0] = '\0';
        m_viewDirty = true;
        return;
    }

    int target = m_selectedLine;
    const int last = (int)m_lineCount - 1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) target = (target < 0) ? 0 : target + 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) target = (target < 0) ? 0 : target - 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) target = (target < 0) ? 0 : target + m_visibleLines;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) target = (target < 0) ? 0 : target - m_visibleLines;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) target = 0;
    else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) target = last;
    if (target != m_selectedLine && last >= 0) {
        if (target < 0) target = 0;
        if (target > last) target = last;
        SelectLine(target);
        m_scrollToSelection = true;
        return;
    }

    if (m_selectedLine < 0 || m_selectedLine > last) {
        return;
    }
    const Line& line = m_lines[m_selectedLine];
    if (line.kind == LINE_GROUP) {
        const DWORD hash = m_groups[line.index].nameHash;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) SetExpanded(hash, true);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) SetExpanded(hash, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            SetExpanded(hash, !IsExpanded(hash));
        }
    } else if (line.kind == LINE_MEMBER && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        for (int i = m_selectedLine - 1; i >= 0; --i) {
            if (m_lines[i].kind == LINE_GROUP) {
                SelectLine(i);
                m_scrollToSelection = true;
                break;
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && CollectSelectionPids()) {
        m_openEndTaskConfirm = true;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && CollectSelectionPids()) {
        CopySelectionToClipboard();
    }
}

// ---------------------------------------------------------------------------------------------
// Actions

void ProcessWindow::EndSelectedTasks() {
    IsProcessCriticalFn isProcessCritical = ResolveIsProcessCritical();
    const DWORD selfPid = GetCurrentProcessId();
    UINT ended = 0, denied = 0, critical = 0, gone = 0, skippedSelf = 0;

    for (UINT i = 0; i < m_actionPidCount; ++i) {
        const DWORD pid = m_actionPids[i];
        if (pid == selfPid) {
            ++skippedSelf; // use Exit in the tray menu
            continue;
        }
        if (pid == 0 || pid == 4) {
            ++critical;
            continue;
        }
        DWORD error = 0;
        HANDLE process = OpenCapturedProcess(pid, m_actionCreateTimes[i], PROCESS_TERMINATE, &error);
        if (process == NULL) {
            if (error == ERROR_ACCESS_DENIED) ++denied; else ++gone;
            continue;
        }
        // Ending a critical process (csrss, wininit, ...) crashes Windows; never do it.
        BOOL isCritical = FALSE;
        if (isProcessCritical != NULL && isProcessCritical(process, &isCritical) && isCritical) {
            ++critical;
        } else if (TerminateProcess(process, 1)) {
            ++ended;
        } else if (GetLastError() == ERROR_ACCESS_DENIED) {
            ++denied;
        } else {
            ++gone;
        }
        CloseHandle(process);
    }

    char text[256];
    int length = snprintf(text, sizeof(text), "Ended %u process%s.", ended, ended == 1 ? "" : "es");
    if (denied > 0 && length > 0 && length < (int)sizeof(text)) {
        length += snprintf(text + length, sizeof(text) - length, " %u denied%s.", denied,
                           m_elevated ? "" : " (try Restart as administrator)");
    }
    if (critical > 0 && length > 0 && length < (int)sizeof(text)) {
        length += snprintf(text + length, sizeof(text) - length, " Skipped %u critical system process%s.",
                           critical, critical == 1 ? "" : "es");
    }
    if (gone > 0 && length > 0 && length < (int)sizeof(text)) {
        length += snprintf(text + length, sizeof(text) - length, " %u already exited.", gone);
    }
    if (skippedSelf > 0 && length > 0 && length < (int)sizeof(text)) {
        snprintf(text + length, sizeof(text) - length, " CoreGaze itself was skipped (use Exit).");
    }
    SetStatus("%s", text);
    m_sampleRequested = true;
}

void ProcessWindow::SetSelectedPriority(DWORD priorityClass) {
    UINT changed = 0, failed = 0;
    for (UINT i = 0; i < m_actionPidCount; ++i) {
        DWORD error = 0;
        HANDLE process = OpenCapturedProcess(m_actionPids[i], m_actionCreateTimes[i], PROCESS_SET_INFORMATION, &error);
        if (process != NULL && SetPriorityClass(process, priorityClass)) {
            ++changed;
        } else {
            ++failed;
        }
        if (process != NULL) {
            CloseHandle(process);
        }
    }
    if (failed == 0) {
        SetStatus("Priority changed for %u process%s.", changed, changed == 1 ? "" : "es");
    } else {
        SetStatus("Priority changed for %u, failed for %u%s.", changed, failed,
                  m_elevated ? "" : " (try Restart as administrator)");
    }
}

void ProcessWindow::OpenFileLocation(DWORD pid) {
    wchar_t path[MAX_PATH * 2];
    if (!QueryImagePath(pid, m_actionPidCount > 0 ? m_actionCreateTimes[0] : 0, path, MAX_PATH * 2)) {
        SetStatus("The file location of this process is not available%s.", m_elevated ? "" : " without administrator rights");
        return;
    }
    wchar_t arguments[MAX_PATH * 2 + 16];
    swprintf_s(arguments, L"/select,\"%ls\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", arguments, NULL, SW_SHOWNORMAL);
}

void ProcessWindow::OpenProperties(DWORD pid) {
    wchar_t path[MAX_PATH * 2];
    if (!QueryImagePath(pid, m_actionPidCount > 0 ? m_actionCreateTimes[0] : 0, path, MAX_PATH * 2)) {
        SetStatus("The file of this process is not available%s.", m_elevated ? "" : " without administrator rights");
        return;
    }
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.lpVerb = L"properties";
    info.lpFile = path;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

void ProcessWindow::CopySelectionToClipboard() {
    if (m_actionPidCount == 0) {
        return;
    }
    // Name and PID of each target, one per line.
    const size_t capacity = (size_t)m_actionPidCount * 160 + 1;
    char* text = (char*)malloc(capacity);
    if (text == NULL) {
        return;
    }
    size_t length = 0;
    text[0] = '\0';
    const ProcessRow* rows = m_monitor.GetRows();
    for (UINT i = 0; i < m_actionPidCount; ++i) {
        const int rowIndex = m_monitor.FindRowIndex(m_actionPids[i]);
        const char* name = (rowIndex >= 0) ? rows[rowIndex].name : "";
        const int written = snprintf(text + length, capacity - length, "%s\t%lu\r\n", name, m_actionPids[i]);
        if (written <= 0 || (size_t)written >= capacity - length) {
            break;
        }
        length += (size_t)written;
    }
    ImGui::SetClipboardText(text);
    free(text);
    SetStatus("Copied %u line%s.", m_actionPidCount, m_actionPidCount == 1 ? "" : "s");
}

void ProcessWindow::SetStatus(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(m_statusText, sizeof(m_statusText), format, args);
    va_end(args);
    m_statusExpireTick = GetTickCount64() + kStatusDurationMs;
    m_lateFrameTick = m_statusExpireTick + 50; // redraw once more to clear it
    RequestFrames(1);
}

// ---------------------------------------------------------------------------------------------
// Window procedure

LRESULT WINAPI ProcessWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* create = (const CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    }
    ProcessWindow* self = (ProcessWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self == NULL || self->m_hwnd != hwnd) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT ProcessWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (m_imgui != NULL) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imgui);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(m_hwnd, msg, wParam, lParam);
        ImGui::SetCurrentContext(previous);
        if (handled) {
            OnInput();
            return handled;
        }
    }

    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_MOUSELEAVE: case WM_ACTIVATE:
        OnInput();
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
            // Draw now: while the user drags the border, Windows runs its own modal loop and the
            // main loop (and so Tick) doesn't run.
            RenderFrame();
            RequestFrames(1);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMinTrackSize.x = (LONG)(kMinWidth * m_dpiScale);
        info->ptMinTrackSize.y = (LONG)(kMinHeight * m_dpiScale);
        return 0;
    }
    case WM_DPICHANGED: {
        m_dpiScale = (float)HIWORD(wParam) / 96.0f;
        m_styleDirty = true;
        const RECT* suggested = (const RECT*)lParam;
        SetWindowPos(m_hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RequestFrames(2);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(m_hwnd, NULL);
        RequestFrames(1);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            return 0; // no ALT menu beep
        }
        break;
    case WM_CLOSE:
        Close();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
