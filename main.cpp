#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <tchar.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <wchar.h>
#include "SystemMonitor.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "wtsapi32.lib")

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static bool                     g_isSuspended = false;
static HPOWERNOTIFY             g_powerNotify = NULL;

static const UINT WM_TRAYICON = WM_APP + 101;
static const UINT IDI_MAINICON = 101;
static const wchar_t* kWindowClassName = L"CoreGazeClass";
static const wchar_t* kWindowTitle = L"CoreGaze";
static const wchar_t* kConfigFolderName = L"CoreGaze";
static const wchar_t* kLegacyConfigFolderName = L"TaskManagerOverlay";
static const wchar_t* kTrayTooltip = L"CoreGaze";
static const char* kHudWindowTitle = "CoreGaze";
static const wchar_t* kSingleInstanceMutexName = L"Local\\CoreGaze.SingleInstance";
static const wchar_t* kStartupRegistryPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kStartupRegistryValueName = L"CoreGaze";
static const wchar_t* kCurrentAppVersion = L"1.0.0";
static const DWORD kConfigSchemaVersion = 1;
static const int kOverlayHotkeyId = 0x0C0E;
static const UINT kOverlayHotkeyModifiers = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
static const UINT kOverlayHotkeyVirtualKey = 'O';
// Thermal zones have no reported limits, so CPU thresholds are fixed. GPU and disk thresholds come
// from the device (or SystemMonitor defaults) through their snapshots.
static const float kCpuTemperatureWarningC = 85.0f;
static const float kCpuTemperatureCriticalC = 95.0f;

enum TrayCommandId : UINT {
    ID_TRAY_TOGGLE_OVERLAY = 5001,
    ID_TRAY_SHOW_CPU = 5002,
    ID_TRAY_SHOW_RAM = 5003,
    ID_TRAY_SHOW_GPU = 5004,
    ID_TRAY_SHOW_DISK = 5005,
    ID_TRAY_SHOW_NETWORK = 5006,
    ID_TRAY_RESET_POSITION = 5007,
    ID_TRAY_POLL_500 = 5101,
    ID_TRAY_POLL_1000 = 5102,
    ID_TRAY_POLL_2000 = 5103,
    ID_TRAY_DISK_SELECT_ALL = 5201,
    ID_TRAY_DISK_SELECT_NONE = 5202,
    ID_TRAY_DISK_DRIVE_BASE = 5300,
    ID_TRAY_GPU_MODE_TARGETED = 5401,
    ID_TRAY_GPU_MODE_HIGHEST_LOAD = 5402,
    ID_TRAY_GPU_MODE_MULTI_GPU = 5403,
    ID_TRAY_GPU_MODE_AGGREGATE = 5404,
    ID_TRAY_GPU_SHOW_VRAM = 5405,
    ID_TRAY_GPU_SELECT_BASE = 5450,
    ID_TRAY_NETWORK_PRIMARY_AUTO = 5601,
    ID_TRAY_NETWORK_PRIMARY_BASE = 5610,
    ID_TRAY_NETWORK_SECONDARY_ENABLE = 5630,
    ID_TRAY_NETWORK_SECONDARY_BASE = 5640,
    ID_TRAY_NETWORK_DISPLAY_RXTX = 5661,
    ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY = 5662,
    ID_TRAY_TEMPERATURE_SHOW = 5701,
    ID_TRAY_TEMPERATURE_CELSIUS = 5702,
    ID_TRAY_TEMPERATURE_FAHRENHEIT = 5703,
    ID_TRAY_STARTUP_TOGGLE = 5901,
    ID_TRAY_EXIT = 5999
};

struct AppSettings {
    DWORD visibleMetricsMask;
    DWORD pollingIntervalMs;
    DWORD diskSelectionMask;
    BOOL overlayVisible;
    DWORD gpuDisplayMode;
    DWORD selectedGpuAdapterIndex;
    BOOL showGpuVram;
    DWORD networkPrimaryMode;
    DWORD networkPrimaryIfIndex;
    BOOL networkSecondaryEnabled;
    DWORD networkSecondaryIfIndex;
    DWORD networkDisplayMode;
    BOOL showTemperatures;
    BOOL temperatureFahrenheit;
    BOOL startupEnabled;
    float overlayPosX;
    float overlayPosY;
    BOOL hasSavedPos;
};

static AppSettings g_appSettings = {
    SYSTEM_METRIC_ALL,
    1000,
    0x03FFFFFFu,
    TRUE,
    GPU_DISPLAY_TARGETED,
    0,
    TRUE,
    NETWORK_PRIMARY_AUTO,
    0,
    FALSE,
    0,
    NETWORK_DISPLAY_RX_TX,
    TRUE,
    FALSE,
    FALSE,
    0.0f,
    0.0f,
    FALSE
};
static NOTIFYICONDATAW g_trayIconData = {};
static wchar_t g_configPath[MAX_PATH] = {};
static wchar_t g_diagnosticsPath[MAX_PATH] = {};
static SystemMonitor* g_systemMonitor = nullptr;
static HANDLE g_singleInstanceMutex = NULL;
static bool g_resetPositionRequested = false;

static wchar_t g_discoveredGpuNames[8][128] = {};
static UINT g_discoveredGpuCount = 0;
static wchar_t g_discoveredDriveLabels[26][32] = {};
static UINT g_discoveredDriveCount = 0;
static DWORD g_discoveredNetworkIfIndices[16] = {};
static wchar_t g_discoveredNetworkNames[16][192] = {};
static UINT g_discoveredNetworkCount = 0;

struct NetworkBarScaleState {
    ULONG ifIndex;
    float downCeilingMbps;
    float upCeilingMbps;
    int downHighSamples;
    int upHighSamples;
};

static NetworkBarScaleState g_primaryNetworkBarScale = { 0, 100.0f, 100.0f, 0, 0 };
static NetworkBarScaleState g_secondaryNetworkBarScale = { 0, 100.0f, 100.0f, 0, 0 };

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void InitializeConfigPath();
void LoadAppSettings();
void SaveAppSettings();
void ApplyRuntimeSettings(HWND hwnd);
void InitializeTrayIcon(HWND hwnd, HINSTANCE hInstance);
void CleanupTrayIcon();
void DiscoverHardwareForMenu();
void ShowTrayContextMenu(HWND hwnd);
void HandleTrayCommand(HWND hwnd, UINT commandId);
static RECT GetOverlayWorkAreaBounds();
static void ApplyOverlayBounds(HWND hwnd);
static HICON LoadAppIcon(HINSTANCE hInstance, int width, int height);
static bool IsStartupEnabledInRegistry();
static void SetStartupEnabledInRegistry(BOOL enabled);
static void InitializeDiagnosticsPath();
static LONG WINAPI CoreGazeUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo);
static void WriteStringSetting(const wchar_t* section, const wchar_t* key, const wchar_t* value);
static void CleanupSingleInstanceMutex();
static void RemoveLegacyExecutableIfPresent();
static void FormatNetworkRateMbps(float mbps, char* output, int outputSize);
static void FormatDiskRateKBps(float kbps, char* output, int outputSize);
static ImVec4 ResolveUtilizationThresholdColor(float progressFraction, const ImVec4& baseColor);
static void DrawMetricTitle(const char* title, bool showTemperature, float temperatureC, float warningC, float criticalC, float rowWidth);
static void ResetNetworkBarScaleState(NetworkBarScaleState* state, ULONG ifIndex);
static float ComputeAdaptiveNetworkProgress(float mbps, float* ceilingMbps, int* highSamples);

// Main code
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    g_singleInstanceMutex = CreateMutexW(NULL, FALSE, kSingleInstanceMutexName);
    if (g_singleInstanceMutex == NULL) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = NULL;
        return 0;
    }

    InitializeConfigPath();
    InitializeDiagnosticsPath();
    SetUnhandledExceptionFilter(CoreGazeUnhandledExceptionFilter);
    RemoveLegacyExecutableIfPresent();

    // Initialize COM for Network List Manager queries (avoids WLAN API Location icon)
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    LoadAppSettings();

    // Create application window
    // WS_EX_TOPMOST: Always on top
    // WS_EX_LAYERED: Allows transparency
    // WS_EX_TRANSPARENT: Click-through (doesn't intercept mouse input)
    // WS_EX_TOOLWINDOW: Hidden from taskbar/ALT+TAB list for overlay behavior
    HICON appIconLarge = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    if (appIconLarge == NULL) {
        appIconLarge = LoadIcon(NULL, IDI_APPLICATION);
    }

    HICON appIconSmall = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (appIconSmall == NULL) {
        appIconSmall = appIconLarge;
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, appIconLarge, nullptr, nullptr, nullptr, kWindowClassName, appIconSmall };
    ::RegisterClassExW(&wc);

    const RECT overlayBounds = GetOverlayWorkAreaBounds();
    const int overlayWidth = overlayBounds.right - overlayBounds.left;
    const int overlayHeight = overlayBounds.bottom - overlayBounds.top;
    
    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        wc.lpszClassName, 
        kWindowTitle,
        WS_POPUP, // Borderless
        overlayBounds.left, overlayBounds.top,
        overlayWidth, overlayHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
        
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CleanupSingleInstanceMutex();
        CoUninitialize();
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls

    // Load custom font with high-clarity rasterization settings
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH = true;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 17.0f, &fontConfig);
    if (font == nullptr) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig);
    }
    if (font == nullptr) {
        io.Fonts->AddFontDefault();
    }

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Modern UI Styling - Sleek, compact HUD
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(6.0f, 3.0f);
    style.FramePadding = ImVec2(4.0f, 2.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.65f); // 65% opacity black

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    SystemMonitor sysMonitor(g_pd3dDevice);
    g_systemMonitor = &sysMonitor;
    ApplyRuntimeSettings(hwnd);
    InitializeTrayIcon(hwnd, hInstance);
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    g_powerNotify = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    const BOOL hotkeyRegistered = RegisterHotKey(
        hwnd,
        kOverlayHotkeyId,
        kOverlayHotkeyModifiers,
        kOverlayHotkeyVirtualKey);

    // Main loop
    bool done = false;
    while (!done)
    {
        // "Hold ALT to Move" Win32 Dynamic Swap Logic - check before sleep for instant responsiveness
        bool isAltHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        // If system display is asleep or workstation is locked, sleep without polling or rendering.
        // When ALT is held, poll at 16ms (60 FPS) for smooth mouse dragging.
        if (g_isSuspended) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2000, QS_ALLINPUT);
        } else if (isAltHeld) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        } else {
            // Active Idle Sleep check for iGPU optimization: 
            // Sleep until either a new window message arrives, or the polling loop hits.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, g_appSettings.pollingIntervalMs, QS_ALLINPUT);
        }

        // Poll and handle messages (inputs, window resize, etc.)
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            if (msg.message == WM_HOTKEY && msg.wParam == (WPARAM)kOverlayHotkeyId) {
                HandleTrayCommand(hwnd, ID_TRAY_TOGGLE_OVERLAY);
                continue;
            }

            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_isSuspended) {
            continue;
        }

        // Dynamic WS_EX_TRANSPARENT toggle with immediate frame update
        LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (isAltHeld) {
            // Remove WS_EX_TRANSPARENT so the window intercepts mouse clicks for ImGui
            if (exStyle & WS_EX_TRANSPARENT) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        } else {
            // Add WS_EX_TRANSPARENT back to resume click-through behavior
            if (!(exStyle & WS_EX_TRANSPARENT)) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }

        // Handle window resize (we don't resize directly, but just in case)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Update hardware metrics (internally throttled to polling interval)
        sysMonitor.Update();

        if (!g_appSettings.overlayVisible) {
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Minimalist HUD Configuration - Saved position or default top right
        if (g_resetPositionRequested) {
            const RECT workAreaBounds = GetOverlayWorkAreaBounds();
            g_appSettings.overlayPosX = (float)workAreaBounds.right - 330.0f;
            g_appSettings.overlayPosY = (float)workAreaBounds.top + 30.0f;
            g_appSettings.hasSavedPos = FALSE;
            ImGui::SetNextWindowPos(ImVec2(g_appSettings.overlayPosX, g_appSettings.overlayPosY), ImGuiCond_Always);
            g_resetPositionRequested = false;
        } else if (g_appSettings.hasSavedPos) {
            ImGui::SetNextWindowPos(ImVec2(g_appSettings.overlayPosX, g_appSettings.overlayPosY), ImGuiCond_FirstUseEver);
        } else {
            const RECT workAreaBounds = GetOverlayWorkAreaBounds();
            ImGui::SetNextWindowPos(ImVec2((float)workAreaBounds.right - 330.0f, (float)workAreaBounds.top + 30.0f), ImGuiCond_FirstUseEver);
        }

        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0)); // No border outline
        ImGui::Begin(kHudWindowTitle, nullptr,
            ImGuiWindowFlags_NoDecoration | 
            ImGuiWindowFlags_AlwaysAutoResize | 
            ImGuiWindowFlags_NoSavedSettings | 
            ImGuiWindowFlags_NoFocusOnAppearing | 
            ImGuiWindowFlags_NoNav |
            (isAltHeld ? 0 : ImGuiWindowFlags_NoMove));

        if (isAltHeld) {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 mouseDelta = io.MouseDelta;
                if (mouseDelta.x != 0.0f || mouseDelta.y != 0.0f) {
                    g_appSettings.overlayPosX += mouseDelta.x;
                    g_appSettings.overlayPosY += mouseDelta.y;
                    g_appSettings.hasSavedPos = TRUE;
                    ImGui::SetNextWindowPos(ImVec2(g_appSettings.overlayPosX, g_appSettings.overlayPosY), ImGuiCond_Always);
                }
            } else {
                ImVec2 curPos = ImGui::GetWindowPos();
                if (curPos.x != g_appSettings.overlayPosX || curPos.y != g_appSettings.overlayPosY) {
                    g_appSettings.overlayPosX = curPos.x;
                    g_appSettings.overlayPosY = curPos.y;
                    g_appSettings.hasSavedPos = TRUE;
                }
            }
        }

        float barWidth = 290.0f;
        float barHeight = 20.0f;

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_CPU) != 0) {
            // CPU Metrics (Blue ProgressBar)
            char cpuTitle[128];
            const char* cpuName = sysMonitor.GetCPUName();
            if (cpuName != NULL && cpuName[0] != '\0' && strcmp(cpuName, "CPU") != 0) {
                snprintf(cpuTitle, sizeof(cpuTitle), "CPU: %s", cpuName);
            } else {
                snprintf(cpuTitle, sizeof(cpuTitle), "CPU");
            }

            char cpuBuf[64];
            snprintf(cpuBuf, sizeof(cpuBuf), "%.1f%% @ %.2f GHz", sysMonitor.GetCPUUsage(), sysMonitor.GetCPUGHz());
            const float cpuProgress = sysMonitor.GetCPUUsage() / 100.0f;
            const ImVec4 cpuBaseColor(0.2f, 0.6f, 1.0f, 1.0f);
            DrawMetricTitle(cpuTitle,
                g_appSettings.showTemperatures && sysMonitor.IsCPUTemperatureAvailable(),
                sysMonitor.GetCPUTemperatureC(), kCpuTemperatureWarningC, kCpuTemperatureCriticalC, barWidth);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ResolveUtilizationThresholdColor(cpuProgress, cpuBaseColor));
            ImGui::ProgressBar(cpuProgress, ImVec2(barWidth, barHeight), cpuBuf);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_RAM) != 0) {
            // RAM Metrics (Green ProgressBar)
            char ramTitle[64];
            const DWORD ramSpeed = sysMonitor.GetRAMSpeedMHz();
            if (ramSpeed > 0) {
                snprintf(ramTitle, sizeof(ramTitle), "RAM (%lu MHz)", ramSpeed);
            } else {
                snprintf(ramTitle, sizeof(ramTitle), "RAM");
            }

            char ramBuf[64];
            snprintf(ramBuf, sizeof(ramBuf), "%.1f / %.1f GB (%.0f%%)", sysMonitor.GetRAMUsedGB(), sysMonitor.GetRAMTotalGB(), sysMonitor.GetRAMUsagePercent());
            const float ramProgress = sysMonitor.GetRAMUsagePercent() / 100.0f;
            const ImVec4 ramBaseColor(0.2f, 1.0f, 0.4f, 1.0f);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", ramTitle);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ResolveUtilizationThresholdColor(ramProgress, ramBaseColor));
            ImGui::ProgressBar(ramProgress, ImVec2(barWidth, barHeight), ramBuf);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_DISK) != 0) {
            const UINT diskRows = sysMonitor.GetSelectedDiskMetricCount();
            if (diskRows == 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Disk");
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.6f, 0.1f, 1.0f));
                ImGui::ProgressBar(0.0f, ImVec2(barWidth, barHeight), "No drives selected");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            } else {
                for (UINT row = 0; row < diskRows; ++row) {
                    DiskMetricsSnapshot diskSnapshot = {};
                    if (!sysMonitor.GetSelectedDiskMetric(row, &diskSnapshot)) {
                        continue;
                    }

                    char diskTitle[128];
                    if (diskSnapshot.capacityAvailable && diskSnapshot.totalGB > 0.0f) {
                        if (diskSnapshot.totalGB >= 1000.0f) {
                            snprintf(diskTitle, sizeof(diskTitle), "Disk %s (%.1f/%.1f TB - %.1f TB free)",
                                diskSnapshot.driveLabel,
                                diskSnapshot.usedGB / 1024.0f,
                                diskSnapshot.totalGB / 1024.0f,
                                diskSnapshot.freeGB / 1024.0f);
                        } else {
                            snprintf(diskTitle, sizeof(diskTitle), "Disk %s (%.0f/%.0f GB - %.0f GB free)",
                                diskSnapshot.driveLabel,
                                diskSnapshot.usedGB,
                                diskSnapshot.totalGB,
                                diskSnapshot.freeGB);
                        }
                    } else if (diskSnapshot.physicalDiskIndex >= 0) {
                        snprintf(diskTitle, sizeof(diskTitle), "Disk %s (PD%d)", diskSnapshot.driveLabel, diskSnapshot.physicalDiskIndex);
                    } else {
                        snprintf(diskTitle, sizeof(diskTitle), "Disk %s", diskSnapshot.driveLabel);
                    }

                    char diskBuf[192];
                    char readRateText[32];
                    char writeRateText[32];
                    FormatDiskRateKBps(diskSnapshot.readKBps, readRateText, (int)sizeof(readRateText));
                    FormatDiskRateKBps(diskSnapshot.writeKBps, writeRateText, (int)sizeof(writeRateText));
                    if (diskSnapshot.fallbackTotal) {
                        snprintf(diskBuf, sizeof(diskBuf), "%.1f%% | R %s | W %s | Fallback", diskSnapshot.activePercent, readRateText, writeRateText);
                    } else {
                        snprintf(diskBuf, sizeof(diskBuf), "%.1f%% | R %s | W %s", diskSnapshot.activePercent, readRateText, writeRateText);
                    }

                    float diskProgress = diskSnapshot.activePercent / 100.0f;
                    if (diskProgress < 0.0f) diskProgress = 0.0f;
                    if (diskProgress > 1.0f) diskProgress = 1.0f;

                    DrawMetricTitle(diskTitle,
                        g_appSettings.showTemperatures && diskSnapshot.temperatureAvailable,
                        diskSnapshot.temperatureC, diskSnapshot.temperatureWarningC, diskSnapshot.temperatureCriticalC, barWidth);
                    const ImVec4 diskBaseColor(1.0f, 0.6f, 0.1f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ResolveUtilizationThresholdColor(diskProgress, diskBaseColor));
                    ImGui::ProgressBar(diskProgress, ImVec2(barWidth, barHeight), diskBuf);
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
            }
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_NETWORK) != 0) {
            const NetworkMetricsSnapshot& primaryNetwork = sysMonitor.GetPrimaryNetworkMetrics();
            const NetworkMetricsSnapshot& secondaryNetwork = sysMonitor.GetSecondaryNetworkMetrics();
            const float miniSpacing = ImGui::GetStyle().ItemSpacing.x;
            const float miniBarWidth = (barWidth - miniSpacing - 1.0f) * 0.5f;

            const char* primaryName = primaryNetwork.displayName[0] != '\0' ? primaryNetwork.displayName : "Disconnected";
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Network: %s", primaryName);

            char downText[64];
            char upText[64];
            char downBuf[80];
            char upBuf[80];
            FormatNetworkRateMbps(primaryNetwork.rxMbps, downText, (int)sizeof(downText));
            FormatNetworkRateMbps(primaryNetwork.txMbps, upText, (int)sizeof(upText));
            snprintf(downBuf, sizeof(downBuf), "v %s", downText);
            snprintf(upBuf, sizeof(upBuf), "^ %s", upText);

            if (g_primaryNetworkBarScale.ifIndex != primaryNetwork.ifIndex) {
                ResetNetworkBarScaleState(&g_primaryNetworkBarScale, primaryNetwork.ifIndex);
            }

            const float downProgress = ComputeAdaptiveNetworkProgress(
                primaryNetwork.rxMbps,
                &g_primaryNetworkBarScale.downCeilingMbps,
                &g_primaryNetworkBarScale.downHighSamples);
            const float upProgress = ComputeAdaptiveNetworkProgress(
                primaryNetwork.txMbps,
                &g_primaryNetworkBarScale.upCeilingMbps,
                &g_primaryNetworkBarScale.upHighSamples);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 1.0f, 1.0f));
            ImGui::ProgressBar(downProgress, ImVec2(miniBarWidth, barHeight), downBuf);
            ImGui::PopStyleColor();

            ImGui::SameLine(0.0f, miniSpacing);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.45f, 1.0f, 1.0f));
            ImGui::ProgressBar(upProgress, ImVec2(miniBarWidth, barHeight), upBuf);
            ImGui::PopStyleColor();

            if (g_appSettings.networkDisplayMode == NETWORK_DISPLAY_RX_TX_SECONDARY && sysMonitor.HasSecondaryNetworkMetrics()) {
                const char* secondaryName = secondaryNetwork.displayName[0] != '\0' ? secondaryNetwork.displayName : "Secondary";
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Network 2: %s", secondaryName);

                char downText2[64];
                char upText2[64];
                char downBuf2[80];
                char upBuf2[80];
                FormatNetworkRateMbps(secondaryNetwork.rxMbps, downText2, (int)sizeof(downText2));
                FormatNetworkRateMbps(secondaryNetwork.txMbps, upText2, (int)sizeof(upText2));
                snprintf(downBuf2, sizeof(downBuf2), "v %s", downText2);
                snprintf(upBuf2, sizeof(upBuf2), "^ %s", upText2);

                if (g_secondaryNetworkBarScale.ifIndex != secondaryNetwork.ifIndex) {
                    ResetNetworkBarScaleState(&g_secondaryNetworkBarScale, secondaryNetwork.ifIndex);
                }

                const float downProgress2 = ComputeAdaptiveNetworkProgress(
                    secondaryNetwork.rxMbps,
                    &g_secondaryNetworkBarScale.downCeilingMbps,
                    &g_secondaryNetworkBarScale.downHighSamples);
                const float upProgress2 = ComputeAdaptiveNetworkProgress(
                    secondaryNetwork.txMbps,
                    &g_secondaryNetworkBarScale.upCeilingMbps,
                    &g_secondaryNetworkBarScale.upHighSamples);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 1.0f, 1.0f));
                ImGui::ProgressBar(downProgress2, ImVec2(miniBarWidth, barHeight), downBuf2);
                ImGui::PopStyleColor();

                ImGui::SameLine(0.0f, miniSpacing);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.45f, 1.0f, 1.0f));
                ImGui::ProgressBar(upProgress2, ImVec2(miniBarWidth, barHeight), upBuf2);
                ImGui::PopStyleColor();
            } else {
                ResetNetworkBarScaleState(&g_secondaryNetworkBarScale, 0);
            }

            ImGui::Spacing();
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_GPU) != 0) {
            const UINT gpuRows = sysMonitor.GetDisplayedGPUCount();
            if (gpuRows == 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "GPU");
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.1f, 0.9f, 0.9f, 1.0f));
                ImGui::ProgressBar(0.0f, ImVec2(barWidth, barHeight), "Unavailable");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            } else {
                for (UINT row = 0; row < gpuRows; ++row) {
                    GPUMetricsSnapshot gpuSnapshot = {};
                    if (!sysMonitor.GetDisplayedGPUSnapshot(row, &gpuSnapshot)) {
                        continue;
                    }

                    char gpuTitle[64];
                    if (g_appSettings.gpuDisplayMode == GPU_DISPLAY_MULTI_GPU && gpuSnapshot.adapterIndex != 0xFFFFFFFFu) {
                        snprintf(gpuTitle, sizeof(gpuTitle), "GPU %u", gpuSnapshot.adapterIndex);
                    } else {
                        snprintf(gpuTitle, sizeof(gpuTitle), "GPU");
                    }

                    char gpuRowTitle[192];
                    snprintf(gpuRowTitle, sizeof(gpuRowTitle), "%s: %s", gpuTitle, gpuSnapshot.adapterName);
                    DrawMetricTitle(gpuRowTitle,
                        g_appSettings.showTemperatures && gpuSnapshot.temperatureAvailable,
                        gpuSnapshot.temperatureC, gpuSnapshot.temperatureWarningC, gpuSnapshot.temperatureCriticalC, barWidth);

                    // GPU Core Utilization Bar
                    char gpuBuf[64];
                    if (gpuSnapshot.utilizationAvailable) {
                        snprintf(gpuBuf, sizeof(gpuBuf), "%.1f%% (%s)", gpuSnapshot.usagePercent, gpuSnapshot.engineLabel);
                    } else {
                        snprintf(gpuBuf, sizeof(gpuBuf), "N/A");
                    }
                    float gpuProgress = gpuSnapshot.utilizationAvailable ? (gpuSnapshot.usagePercent / 100.0f) : 0.0f;
                    if (gpuProgress > 1.0f) gpuProgress = 1.0f;
                    if (gpuProgress < 0.0f) gpuProgress = 0.0f;

                    const ImVec4 gpuBaseColor(0.1f, 0.9f, 0.9f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ResolveUtilizationThresholdColor(gpuProgress, gpuBaseColor));
                    ImGui::ProgressBar(gpuProgress, ImVec2(barWidth, barHeight), gpuBuf);
                    ImGui::PopStyleColor();

                    // Dedicated VRAM Progress Bar
                    if (g_appSettings.showGpuVram && gpuSnapshot.memoryAvailable && gpuSnapshot.dedicatedTotalGB > 0.0f) {
                        char vramBuf[96];
                        float vramPercent = (gpuSnapshot.dedicatedUsedGB / gpuSnapshot.dedicatedTotalGB) * 100.0f;
                        if (gpuSnapshot.sharedUsedGB > 0.05f) {
                            snprintf(vramBuf, sizeof(vramBuf), "VRAM: %.1f/%.1f GB (%.0f%%) +%.1fG",
                                gpuSnapshot.dedicatedUsedGB, gpuSnapshot.dedicatedTotalGB, vramPercent, gpuSnapshot.sharedUsedGB);
                        } else {
                            snprintf(vramBuf, sizeof(vramBuf), "VRAM: %.1f / %.1f GB (%.0f%%)",
                                gpuSnapshot.dedicatedUsedGB, gpuSnapshot.dedicatedTotalGB, vramPercent);
                        }
                        float vramProgress = gpuSnapshot.dedicatedUsedGB / gpuSnapshot.dedicatedTotalGB;
                        if (vramProgress > 1.0f) vramProgress = 1.0f;
                        if (vramProgress < 0.0f) vramProgress = 0.0f;

                        const ImVec4 vramBaseColor(0.65f, 0.35f, 1.0f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ResolveUtilizationThresholdColor(vramProgress, vramBaseColor));
                        ImGui::ProgressBar(vramProgress, ImVec2(barWidth, barHeight), vramBuf);
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::Spacing();
            }
        }

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), " *(Hold ALT to move)* ");

        ImGui::End();
        ImGui::PopStyleColor(); // Pop border color

        ImGui::Render();
        // Fully transparent clear color
        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present with vsync
        g_pSwapChain->Present(1, 0); 
    }

    // Cleanup
    SaveAppSettings();
    CleanupTrayIcon();
    if (hotkeyRegistered) {
        UnregisterHotKey(hwnd, kOverlayHotkeyId);
    }
    if (g_powerNotify != NULL) {
        UnregisterPowerSettingNotification(g_powerNotify);
        g_powerNotify = NULL;
    }
    WTSUnRegisterSessionNotification(hwnd);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_systemMonitor = nullptr;

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CleanupSingleInstanceMutex();
    CoUninitialize();

    return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static DWORD NormalizePollingRate(DWORD pollingRate) {
    if (pollingRate <= 750) return 500;
    if (pollingRate <= 1500) return 1000;
    return 2000;
}

static DWORD NormalizeGpuDisplayMode(DWORD gpuDisplayMode) {
    if (gpuDisplayMode > GPU_DISPLAY_AGGREGATE) {
        return GPU_DISPLAY_TARGETED;
    }
    return gpuDisplayMode;
}

static DWORD NormalizeNetworkPrimaryMode(DWORD networkPrimaryMode) {
    if (networkPrimaryMode > NETWORK_PRIMARY_MANUAL) {
        return NETWORK_PRIMARY_AUTO;
    }
    return networkPrimaryMode;
}

static DWORD NormalizeNetworkDisplayMode(DWORD networkDisplayMode) {
    // The primary row always renders as RX/TX split bars, so the legacy "total" mode maps to split.
    if (networkDisplayMode == NETWORK_DISPLAY_TOTAL || networkDisplayMode > NETWORK_DISPLAY_RX_TX_SECONDARY) {
        return NETWORK_DISPLAY_RX_TX;
    }
    return networkDisplayMode;
}

static bool IsStartupEnabledInRegistry() {
    HKEY keyHandle = NULL;
    LONG openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (openStatus != ERROR_SUCCESS) {
        return false;
    }

    wchar_t startupValue[MAX_PATH] = {};
    DWORD valueSize = sizeof(startupValue);
    LONG queryStatus = RegQueryValueExW(keyHandle, kStartupRegistryValueName, NULL, NULL, (LPBYTE)startupValue, &valueSize);
    RegCloseKey(keyHandle);
    return (queryStatus == ERROR_SUCCESS);
}

static void SetStartupEnabledInRegistry(BOOL enabled) {
    HKEY keyHandle = NULL;
    DWORD disposition = 0;
    LONG openStatus = RegCreateKeyExW(HKEY_CURRENT_USER, kStartupRegistryPath, 0, NULL, 0, KEY_SET_VALUE, NULL, &keyHandle, &disposition);
    if (openStatus != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        wchar_t modulePath[MAX_PATH] = {};
        DWORD modulePathLength = GetModuleFileNameW(NULL, modulePath, MAX_PATH);
        if (modulePathLength > 0 && modulePathLength < MAX_PATH) {
            wchar_t quotedPath[MAX_PATH * 2] = {};
            swprintf_s(quotedPath, L"\"%ls\"", modulePath);
            RegSetValueExW(keyHandle, kStartupRegistryValueName, 0, REG_SZ, (const BYTE*)quotedPath, (DWORD)((wcslen(quotedPath) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(keyHandle, kStartupRegistryValueName);
    }

    RegCloseKey(keyHandle);
}

static void InitializeDiagnosticsPath() {
    wchar_t appData[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        wcscpy_s(appData, L".");
    }

    wchar_t appConfigDir[MAX_PATH] = {};
    swprintf_s(appConfigDir, L"%ls\\%ls", appData, kConfigFolderName);
    CreateDirectoryW(appConfigDir, NULL);

    swprintf_s(g_diagnosticsPath, L"%ls\\Diagnostics", appConfigDir);
    CreateDirectoryW(g_diagnosticsPath, NULL);
}

static LONG WINAPI CoreGazeUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    if (g_diagnosticsPath[0] == L'\0') {
        InitializeDiagnosticsPath();
    }

    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);

    wchar_t logPath[MAX_PATH] = {};
    swprintf_s(logPath, L"%ls\\crash_%04u%02u%02u_%02u%02u%02u.log",
        g_diagnosticsPath,
        localTime.wYear,
        localTime.wMonth,
        localTime.wDay,
        localTime.wHour,
        localTime.wMinute,
        localTime.wSecond);

    HANDLE logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logFile != INVALID_HANDLE_VALUE) {
        DWORD exceptionCode = 0;
        void* exceptionAddress = NULL;
        if (exceptionInfo != NULL && exceptionInfo->ExceptionRecord != NULL) {
            exceptionCode = exceptionInfo->ExceptionRecord->ExceptionCode;
            exceptionAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;
        }

        char reportText[1024] = {};
        int textLength = snprintf(reportText, sizeof(reportText),
            "CoreGaze crash diagnostic\r\n"
            "Version: 1.0.0\r\n"
            "ExceptionCode: 0x%08lX\r\n"
            "ExceptionAddress: %p\r\n"
            "ThreadId: %lu\r\n",
            (unsigned long)exceptionCode,
            exceptionAddress,
            (unsigned long)GetCurrentThreadId());

        if (textLength > 0) {
            DWORD bytesWritten = 0;
            WriteFile(logFile, reportText, (DWORD)textLength, &bytesWritten, NULL);
        }

        CloseHandle(logFile);
    }

    if (exceptionInfo != NULL) {
        wchar_t dumpPath[MAX_PATH] = {};
        swprintf_s(dumpPath, L"%ls\\crash_%04u%02u%02u_%02u%02u%02u.dmp",
            g_diagnosticsPath,
            localTime.wYear,
            localTime.wMonth,
            localTime.wDay,
            localTime.wHour,
            localTime.wMinute,
            localTime.wSecond);

        HANDLE dumpFile = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (dumpFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION dumpInfo = {};
            dumpInfo.ThreadId = GetCurrentThreadId();
            dumpInfo.ExceptionPointers = exceptionInfo;
            dumpInfo.ClientPointers = FALSE;

            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                dumpFile,
                MiniDumpWithIndirectlyReferencedMemory,
                &dumpInfo,
                NULL,
                NULL);

            CloseHandle(dumpFile);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

static RECT GetOverlayWorkAreaBounds() {
    RECT bounds = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &bounds, 0) && bounds.right > bounds.left && bounds.bottom > bounds.top) {
        return bounds;
    }

    bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.right = bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.bottom = bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return bounds;
}

static void ApplyOverlayBounds(HWND hwnd) {
    if (hwnd == NULL) {
        return;
    }

    const RECT bounds = GetOverlayWorkAreaBounds();
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static HICON LoadAppIcon(HINSTANCE hInstance, int width, int height) {
    return (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MAINICON), IMAGE_ICON, width, height, LR_DEFAULTCOLOR | LR_SHARED);
}

static void FormatNetworkRateMbps(float mbps, char* output, int outputSize) {
    if (output == NULL || outputSize <= 0) {
        return;
    }

    float value = mbps;
    if (value < 0.0f) {
        value = 0.0f;
    }

    if (value >= 1.0f) {
        snprintf(output, outputSize, "%.1f Mbps", value);
    } else {
        snprintf(output, outputSize, "%.0f Kbps", value * 1000.0f);
    }
}

static void FormatDiskRateKBps(float kbps, char* output, int outputSize) {
    if (output == NULL || outputSize <= 0) {
        return;
    }

    float value = kbps;
    if (value < 0.0f) {
        value = 0.0f;
    }

    if (value >= (1024.0f * 1024.0f)) {
        snprintf(output, outputSize, "%.2f GB/s", value / (1024.0f * 1024.0f));
    } else if (value >= 1024.0f) {
        snprintf(output, outputSize, "%.1f MB/s", value / 1024.0f);
    } else {
        snprintf(output, outputSize, "%.0f KB/s", value);
    }
}

static ImVec4 ResolveUtilizationThresholdColor(float progressFraction, const ImVec4& baseColor) {
    const float kWarningThreshold = 0.80f;
    const float kCriticalThreshold = 0.95f;
    const ImVec4 kWarningColor(1.0f, 0.55f, 0.0f, 1.0f);
    const ImVec4 kCriticalColor(1.0f, 0.15f, 0.15f, 1.0f);

    float clampedFraction = progressFraction;
    if (clampedFraction < 0.0f) {
        clampedFraction = 0.0f;
    } else if (clampedFraction > 1.0f) {
        clampedFraction = 1.0f;
    }

    if (clampedFraction >= kCriticalThreshold) {
        return kCriticalColor;
    }

    if (clampedFraction >= kWarningThreshold) {
        return kWarningColor;
    }

    return baseColor;
}

// Copies `text` into `output`, shortened with "..." if it is wider than `maxWidth` pixels.
// Cuts only on UTF-8 character boundaries so adapter names with non-ASCII characters stay valid.
static void FitTextToWidth(const char* text, float maxWidth, char* output, int outputSize) {
    snprintf(output, outputSize, "%s", text);
    if (ImGui::CalcTextSize(output).x <= maxWidth) {
        return;
    }

    const char* kEllipsis = "...";
    const float ellipsisWidth = ImGui::CalcTextSize(kEllipsis).x;
    int length = (int)strlen(output);
    while (length > 0) {
        --length;
        while (length > 0 && (output[length] & 0xC0) == 0x80) {
            --length;
        }
        if (ImGui::CalcTextSize(output, output + length).x + ellipsisWidth <= maxWidth) {
            break;
        }
    }

    while (length > 0 && output[length - 1] == ' ') {
        --length;
    }
    snprintf(output + length, outputSize - length, "%s", kEllipsis);
}

// Draws a metric title, optionally with its temperature right-aligned to the bar edge. When a
// temperature is shown, a title too long to fit beside it is shortened so the HUD never widens.
static void DrawMetricTitle(const char* title, bool showTemperature, float temperatureC, float warningC, float criticalC, float rowWidth) {
    const ImVec4 kTitleColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (!showTemperature) {
        ImGui::TextColored(kTitleColor, "%s", title);
        return;
    }

    const ImVec4 kNormalColor(0.75f, 0.75f, 0.75f, 1.0f);
    const ImVec4 kWarningColor(1.0f, 0.55f, 0.0f, 1.0f);
    const ImVec4 kCriticalColor(1.0f, 0.15f, 0.15f, 1.0f);

    ImVec4 temperatureColor = kNormalColor;
    if (criticalC > 0.0f && temperatureC >= criticalC) {
        temperatureColor = kCriticalColor;
    } else if (warningC > 0.0f && temperatureC >= warningC) {
        temperatureColor = kWarningColor;
    }

    char temperatureText[24];
    if (g_appSettings.temperatureFahrenheit) {
        snprintf(temperatureText, sizeof(temperatureText), "%.0f\xC2\xB0""F", (temperatureC * 9.0f / 5.0f) + 32.0f);
    } else {
        snprintf(temperatureText, sizeof(temperatureText), "%.0f\xC2\xB0""C", temperatureC);
    }

    const float temperatureWidth = ImGui::CalcTextSize(temperatureText).x;
    const float maxTitleWidth = rowWidth - temperatureWidth - ImGui::GetStyle().ItemSpacing.x;

    char fittedTitle[192];
    FitTextToWidth(title, maxTitleWidth, fittedTitle, (int)sizeof(fittedTitle));
    ImGui::TextColored(kTitleColor, "%s", fittedTitle);

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + rowWidth - temperatureWidth);
    ImGui::TextColored(temperatureColor, "%s", temperatureText);
}

static void ResetNetworkBarScaleState(NetworkBarScaleState* state, ULONG ifIndex) {
    if (state == NULL) {
        return;
    }

    state->ifIndex = ifIndex;
    state->downCeilingMbps = 100.0f;
    state->upCeilingMbps = 100.0f;
    state->downHighSamples = 0;
    state->upHighSamples = 0;
}

static float ComputeAdaptiveNetworkProgress(float mbps, float* ceilingMbps, int* highSamples) {
    const float kMinCeilingMbps = 100.0f;
    const float kHighWatermarkRatio = 0.85f;
    const float kLowWatermarkRatio = 0.20f;
    const float kCeilingHeadroomFactor = 1.20f;
    const float kCeilingDecayFactor = 0.97f;
    const int kHighSamplesToGrow = 2;

    if (ceilingMbps == NULL || highSamples == NULL) {
        return 0.0f;
    }

    float value = mbps;
    if (value < 0.0f) {
        value = 0.0f;
    }

    if (*ceilingMbps < kMinCeilingMbps) {
        *ceilingMbps = kMinCeilingMbps;
    }

    if (value >= (*ceilingMbps * kHighWatermarkRatio)) {
        if (*highSamples < kHighSamplesToGrow) {
            ++(*highSamples);
        }

        if (*highSamples >= kHighSamplesToGrow) {
            const float expandedCeiling = value * kCeilingHeadroomFactor;
            if (expandedCeiling > *ceilingMbps) {
                *ceilingMbps = expandedCeiling;
            }
            *highSamples = 0;
        }
    } else if (value <= (*ceilingMbps * kLowWatermarkRatio)) {
        *highSamples = 0;
        *ceilingMbps *= kCeilingDecayFactor;
        if (*ceilingMbps < kMinCeilingMbps) {
            *ceilingMbps = kMinCeilingMbps;
        }
    } else if (*highSamples > 0) {
        --(*highSamples);
    }

    if (*ceilingMbps <= 0.0f) {
        return 0.0f;
    }

    float progress = value / *ceilingMbps;
    if (progress < 0.0f) {
        progress = 0.0f;
    } else if (progress > 1.0f) {
        progress = 1.0f;
    }

    return progress;
}

static UINT CheckedFlag(BOOL checked) {
    return checked ? MF_CHECKED : MF_UNCHECKED;
}

static void WriteUIntSetting(const wchar_t* section, const wchar_t* key, DWORD value) {
    wchar_t buffer[32];
    swprintf_s(buffer, L"%u", value);
    WritePrivateProfileStringW(section, key, buffer, g_configPath);
}

static void WriteStringSetting(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    if (value == NULL) {
        WritePrivateProfileStringW(section, key, L"", g_configPath);
        return;
    }
    WritePrivateProfileStringW(section, key, value, g_configPath);
}

static void CleanupSingleInstanceMutex() {
    if (g_singleInstanceMutex != NULL) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = NULL;
    }
}

static void RemoveLegacyExecutableIfPresent() {
    wchar_t modulePath[MAX_PATH] = {};
    DWORD pathLength = GetModuleFileNameW(NULL, modulePath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        return;
    }

    wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
    if (lastSlash == NULL) {
        return;
    }

    *(lastSlash + 1) = L'\0';

    wchar_t legacyExePath[MAX_PATH] = {};
    swprintf_s(legacyExePath, L"%lsTaskManagerOverlay.exe", modulePath);
    DWORD legacyAttributes = GetFileAttributesW(legacyExePath);
    if (legacyAttributes != INVALID_FILE_ATTRIBUTES && (legacyAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        DeleteFileW(legacyExePath);
    }
}

void InitializeConfigPath() {
    wchar_t appData[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        wcscpy_s(appData, L".");
    }

    wchar_t configDir[MAX_PATH] = {};
    swprintf_s(configDir, L"%ls\\%ls", appData, kConfigFolderName);
    CreateDirectoryW(configDir, NULL);

    swprintf_s(g_configPath, L"%ls\\config.ini", configDir);

    if (GetFileAttributesW(g_configPath) == INVALID_FILE_ATTRIBUTES) {
        wchar_t legacyConfigPath[MAX_PATH] = {};
        swprintf_s(legacyConfigPath, L"%ls\\%ls\\config.ini", appData, kLegacyConfigFolderName);
        DWORD legacyConfigAttributes = GetFileAttributesW(legacyConfigPath);
        if (legacyConfigAttributes != INVALID_FILE_ATTRIBUTES && (legacyConfigAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            CopyFileW(legacyConfigPath, g_configPath, TRUE);
        }
    }
}

void LoadAppSettings() {
    g_appSettings.visibleMetricsMask = GetPrivateProfileIntW(L"Display", L"VisibleMask", SYSTEM_METRIC_ALL, g_configPath) & SYSTEM_METRIC_ALL;
    g_appSettings.overlayVisible = GetPrivateProfileIntW(L"Display", L"OverlayVisible", 1, g_configPath) ? TRUE : FALSE;
    g_appSettings.diskSelectionMask = GetPrivateProfileIntW(L"Disk", L"SelectedMask", 0x03FFFFFFu, g_configPath) & 0x03FFFFFFu;
    g_appSettings.pollingIntervalMs = NormalizePollingRate(GetPrivateProfileIntW(L"Polling", L"IntervalMs", 1000, g_configPath));
    g_appSettings.gpuDisplayMode = NormalizeGpuDisplayMode(GetPrivateProfileIntW(L"GPU", L"DisplayMode", GPU_DISPLAY_TARGETED, g_configPath));
    g_appSettings.selectedGpuAdapterIndex = (DWORD)GetPrivateProfileIntW(L"GPU", L"SelectedAdapterIndex", 0, g_configPath);
    g_appSettings.showGpuVram = GetPrivateProfileIntW(L"GPU", L"ShowVram", 1, g_configPath) ? TRUE : FALSE;
    g_appSettings.networkPrimaryMode = NormalizeNetworkPrimaryMode(GetPrivateProfileIntW(L"Network", L"PrimaryMode", NETWORK_PRIMARY_AUTO, g_configPath));
    g_appSettings.networkPrimaryIfIndex = (DWORD)GetPrivateProfileIntW(L"Network", L"PrimaryIfIndex", 0, g_configPath);
    g_appSettings.networkSecondaryEnabled = GetPrivateProfileIntW(L"Network", L"SecondaryEnabled", 0, g_configPath) ? TRUE : FALSE;
    g_appSettings.networkSecondaryIfIndex = (DWORD)GetPrivateProfileIntW(L"Network", L"SecondaryIfIndex", 0, g_configPath);
    g_appSettings.networkDisplayMode = NormalizeNetworkDisplayMode(GetPrivateProfileIntW(L"Network", L"DisplayMode", NETWORK_DISPLAY_RX_TX, g_configPath));
    g_appSettings.showTemperatures = GetPrivateProfileIntW(L"Temperature", L"Show", 1, g_configPath) ? TRUE : FALSE;
    g_appSettings.temperatureFahrenheit = GetPrivateProfileIntW(L"Temperature", L"Fahrenheit", 0, g_configPath) ? TRUE : FALSE;
    BOOL startupRegistryEnabled = IsStartupEnabledInRegistry() ? TRUE : FALSE;
    g_appSettings.startupEnabled = GetPrivateProfileIntW(L"General", L"StartWithWindows", startupRegistryEnabled ? 1 : 0, g_configPath) ? TRUE : FALSE;
    g_appSettings.hasSavedPos = GetPrivateProfileIntW(L"Window", L"HasSavedPos", 0, g_configPath) ? TRUE : FALSE;
    wchar_t posXBuf[32] = {}, posYBuf[32] = {};
    GetPrivateProfileStringW(L"Window", L"PosX", L"0", posXBuf, 32, g_configPath);
    GetPrivateProfileStringW(L"Window", L"PosY", L"0", posYBuf, 32, g_configPath);
    g_appSettings.overlayPosX = (float)_wtof(posXBuf);
    g_appSettings.overlayPosY = (float)_wtof(posYBuf);
    WriteUIntSetting(L"Version", L"ConfigSchemaVersion", kConfigSchemaVersion);
    WriteStringSetting(L"Version", L"LastLaunchedVersion", kCurrentAppVersion);
    SetStartupEnabledInRegistry(g_appSettings.startupEnabled);
}

void SaveAppSettings() {
    WriteUIntSetting(L"Display", L"VisibleMask", g_appSettings.visibleMetricsMask & SYSTEM_METRIC_ALL);
    WriteUIntSetting(L"Display", L"OverlayVisible", g_appSettings.overlayVisible ? 1u : 0u);
    WriteUIntSetting(L"Disk", L"SelectedMask", g_appSettings.diskSelectionMask & 0x03FFFFFFu);
    WriteUIntSetting(L"Polling", L"IntervalMs", NormalizePollingRate(g_appSettings.pollingIntervalMs));
    WriteUIntSetting(L"GPU", L"DisplayMode", NormalizeGpuDisplayMode(g_appSettings.gpuDisplayMode));
    WriteUIntSetting(L"GPU", L"SelectedAdapterIndex", g_appSettings.selectedGpuAdapterIndex);
    WriteUIntSetting(L"GPU", L"ShowVram", g_appSettings.showGpuVram ? 1u : 0u);
    WriteUIntSetting(L"Network", L"PrimaryMode", NormalizeNetworkPrimaryMode(g_appSettings.networkPrimaryMode));
    WriteUIntSetting(L"Network", L"PrimaryIfIndex", g_appSettings.networkPrimaryIfIndex);
    WriteUIntSetting(L"Network", L"SecondaryEnabled", g_appSettings.networkSecondaryEnabled ? 1u : 0u);
    WriteUIntSetting(L"Network", L"SecondaryIfIndex", g_appSettings.networkSecondaryIfIndex);
    WriteUIntSetting(L"Network", L"DisplayMode", NormalizeNetworkDisplayMode(g_appSettings.networkDisplayMode));
    WriteUIntSetting(L"Temperature", L"Show", g_appSettings.showTemperatures ? 1u : 0u);
    WriteUIntSetting(L"Temperature", L"Fahrenheit", g_appSettings.temperatureFahrenheit ? 1u : 0u);
    WriteUIntSetting(L"General", L"StartWithWindows", g_appSettings.startupEnabled ? 1u : 0u);
    WriteUIntSetting(L"Window", L"HasSavedPos", g_appSettings.hasSavedPos ? 1u : 0u);
    if (g_appSettings.hasSavedPos) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f", g_appSettings.overlayPosX);
        WriteStringSetting(L"Window", L"PosX", buf);
        swprintf_s(buf, L"%.1f", g_appSettings.overlayPosY);
        WriteStringSetting(L"Window", L"PosY", buf);
    } else {
        WriteStringSetting(L"Window", L"PosX", L"");
        WriteStringSetting(L"Window", L"PosY", L"");
    }
    WriteUIntSetting(L"Version", L"ConfigSchemaVersion", kConfigSchemaVersion);
    WriteStringSetting(L"Version", L"LastLaunchedVersion", kCurrentAppVersion);
    SetStartupEnabledInRegistry(g_appSettings.startupEnabled);
}

void ApplyRuntimeSettings(HWND hwnd) {
    g_appSettings.pollingIntervalMs = NormalizePollingRate(g_appSettings.pollingIntervalMs);
    g_appSettings.visibleMetricsMask &= SYSTEM_METRIC_ALL;
    g_appSettings.diskSelectionMask &= 0x03FFFFFFu;
    g_appSettings.gpuDisplayMode = NormalizeGpuDisplayMode(g_appSettings.gpuDisplayMode);
    g_appSettings.networkPrimaryMode = NormalizeNetworkPrimaryMode(g_appSettings.networkPrimaryMode);
    g_appSettings.networkDisplayMode = NormalizeNetworkDisplayMode(g_appSettings.networkDisplayMode);
    g_appSettings.networkSecondaryEnabled = g_appSettings.networkSecondaryEnabled ? TRUE : FALSE;

    if (g_systemMonitor) {
        g_systemMonitor->SetPollingIntervalMs(g_appSettings.pollingIntervalMs);
        g_systemMonitor->SetEnabledMetricsMask(g_appSettings.visibleMetricsMask);
        g_systemMonitor->SetDiskSelectionMask(g_appSettings.diskSelectionMask);
        g_systemMonitor->SetGPUDisplayMode(g_appSettings.gpuDisplayMode);
        g_systemMonitor->SetSelectedGPUAdapterIndex(g_appSettings.selectedGpuAdapterIndex);
        g_systemMonitor->SetNetworkPrimaryMode(g_appSettings.networkPrimaryMode);
        g_systemMonitor->SetNetworkPrimaryIfIndex(g_appSettings.networkPrimaryIfIndex);
        g_systemMonitor->SetNetworkSecondaryEnabled(g_appSettings.networkSecondaryEnabled);
        g_systemMonitor->SetNetworkSecondaryIfIndex(g_appSettings.networkSecondaryIfIndex);
        g_systemMonitor->SetNetworkDisplayMode(g_appSettings.networkDisplayMode);
        g_systemMonitor->SetTemperaturesEnabled(g_appSettings.showTemperatures != FALSE);
    }

    ShowWindow(hwnd, g_appSettings.overlayVisible ? SW_SHOWNA : SW_HIDE);
}

void InitializeTrayIcon(HWND hwnd, HINSTANCE hInstance) {
    ZeroMemory(&g_trayIconData, sizeof(g_trayIconData));
    g_trayIconData.cbSize = sizeof(g_trayIconData);
    g_trayIconData.hWnd = hwnd;
    g_trayIconData.uID = 1;
    g_trayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIconData.uCallbackMessage = WM_TRAYICON;
    g_trayIconData.hIcon = LoadAppIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (g_trayIconData.hIcon == NULL) {
        g_trayIconData.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    wcscpy_s(g_trayIconData.szTip, kTrayTooltip);
    Shell_NotifyIconW(NIM_ADD, &g_trayIconData);
}

void CleanupTrayIcon() {
    if (g_trayIconData.hWnd != NULL) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIconData);
        g_trayIconData.hWnd = NULL;
    }
}

void DiscoverHardwareForMenu() {
    g_discoveredGpuCount = 0;
    if (g_systemMonitor != nullptr) {
        const UINT gpuCount = g_systemMonitor->GetGPUAdapterCount();
        for (UINT i = 0; i < gpuCount && i < 8; ++i) {
            const char* gpuNameUtf8 = g_systemMonitor->GetGPUAdapterNameByIndex(i);
            if (gpuNameUtf8 == nullptr || gpuNameUtf8[0] == '\0') {
                swprintf_s(g_discoveredGpuNames[g_discoveredGpuCount], L"GPU %u", i);
            } else if (MultiByteToWideChar(CP_UTF8, 0, gpuNameUtf8, -1, g_discoveredGpuNames[g_discoveredGpuCount], (int)(sizeof(g_discoveredGpuNames[g_discoveredGpuCount]) / sizeof(g_discoveredGpuNames[g_discoveredGpuCount][0]))) <= 0) {
                swprintf_s(g_discoveredGpuNames[g_discoveredGpuCount], L"GPU %u", i);
            }
            ++g_discoveredGpuCount;
        }
    }

    g_discoveredDriveCount = 0;
    DWORD driveMask = GetLogicalDrives();
    for (UINT i = 0; i < 26; ++i) {
        if ((driveMask & (1u << i)) == 0) {
            continue;
        }

        wchar_t rootPath[4] = { (wchar_t)(L'A' + i), L':', L'\\', L'\0' };
        UINT driveType = GetDriveTypeW(rootPath);
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) {
            continue;
        }

        if (driveType == DRIVE_REMOVABLE) {
            swprintf_s(g_discoveredDriveLabels[g_discoveredDriveCount], L"%c: (Removable)", (wchar_t)(L'A' + i));
        } else {
            swprintf_s(g_discoveredDriveLabels[g_discoveredDriveCount], L"%c:", (wchar_t)(L'A' + i));
        }
        ++g_discoveredDriveCount;
    }

    g_discoveredNetworkCount = 0;
    if (g_systemMonitor != nullptr) {
        g_systemMonitor->RefreshNetworkIdentityNow();
        const UINT networkCount = g_systemMonitor->GetNetworkAdapterCount();
        for (UINT i = 0; i < networkCount && i < 16; ++i) {
            const DWORD ifIndex = g_systemMonitor->GetNetworkAdapterIfIndex(i);
            const char* networkNameUtf8 = g_systemMonitor->GetNetworkAdapterNameByIndex(i);
            if (ifIndex == 0 || networkNameUtf8 == nullptr || networkNameUtf8[0] == '\0') {
                continue;
            }

            if (MultiByteToWideChar(CP_UTF8, 0, networkNameUtf8, -1, g_discoveredNetworkNames[g_discoveredNetworkCount], (int)(sizeof(g_discoveredNetworkNames[g_discoveredNetworkCount]) / sizeof(g_discoveredNetworkNames[g_discoveredNetworkCount][0]))) <= 0) {
                continue;
            }

            g_discoveredNetworkIfIndices[g_discoveredNetworkCount] = ifIndex;
            ++g_discoveredNetworkCount;
        }
    }
}

void HandleTrayCommand(HWND hwnd, UINT commandId) {
    bool settingsChanged = true;

    switch (commandId) {
    case ID_TRAY_TOGGLE_OVERLAY:
        g_appSettings.overlayVisible = g_appSettings.overlayVisible ? FALSE : TRUE;
        break;
    case ID_TRAY_RESET_POSITION:
        g_resetPositionRequested = true;
        break;
    case ID_TRAY_SHOW_CPU:
        g_appSettings.visibleMetricsMask ^= SYSTEM_METRIC_CPU;
        break;
    case ID_TRAY_SHOW_RAM:
        g_appSettings.visibleMetricsMask ^= SYSTEM_METRIC_RAM;
        break;
    case ID_TRAY_SHOW_GPU:
        g_appSettings.visibleMetricsMask ^= SYSTEM_METRIC_GPU;
        break;
    case ID_TRAY_SHOW_DISK:
        g_appSettings.visibleMetricsMask ^= SYSTEM_METRIC_DISK;
        break;
    case ID_TRAY_SHOW_NETWORK:
        g_appSettings.visibleMetricsMask ^= SYSTEM_METRIC_NETWORK;
        break;
    case ID_TRAY_POLL_500:
        g_appSettings.pollingIntervalMs = 500;
        break;
    case ID_TRAY_POLL_1000:
        g_appSettings.pollingIntervalMs = 1000;
        break;
    case ID_TRAY_POLL_2000:
        g_appSettings.pollingIntervalMs = 2000;
        break;
    case ID_TRAY_GPU_MODE_TARGETED:
        g_appSettings.gpuDisplayMode = GPU_DISPLAY_TARGETED;
        break;
    case ID_TRAY_GPU_MODE_HIGHEST_LOAD:
        g_appSettings.gpuDisplayMode = GPU_DISPLAY_HIGHEST_LOAD;
        break;
    case ID_TRAY_GPU_MODE_MULTI_GPU:
        g_appSettings.gpuDisplayMode = GPU_DISPLAY_MULTI_GPU;
        break;
    case ID_TRAY_GPU_MODE_AGGREGATE:
        g_appSettings.gpuDisplayMode = GPU_DISPLAY_AGGREGATE;
        break;
    case ID_TRAY_GPU_SHOW_VRAM:
        g_appSettings.showGpuVram = !g_appSettings.showGpuVram;
        break;
    case ID_TRAY_DISK_SELECT_ALL: {
        DWORD discoveredMask = 0;
        for (UINT i = 0; i < g_discoveredDriveCount; ++i) {
            wchar_t driveLetter = g_discoveredDriveLabels[i][0];
            if (driveLetter >= L'A' && driveLetter <= L'Z') {
                discoveredMask |= (1u << (driveLetter - L'A'));
            }
        }
        g_appSettings.diskSelectionMask = discoveredMask;
        break;
    }
    case ID_TRAY_DISK_SELECT_NONE:
        g_appSettings.diskSelectionMask = 0;
        break;
    case ID_TRAY_NETWORK_PRIMARY_AUTO:
        g_appSettings.networkPrimaryMode = NETWORK_PRIMARY_AUTO;
        g_appSettings.networkPrimaryIfIndex = 0;
        break;
    case ID_TRAY_NETWORK_SECONDARY_ENABLE:
        g_appSettings.networkSecondaryEnabled = g_appSettings.networkSecondaryEnabled ? FALSE : TRUE;
        if (!g_appSettings.networkSecondaryEnabled) {
            g_appSettings.networkSecondaryIfIndex = 0;
        }
        break;
    case ID_TRAY_NETWORK_DISPLAY_RXTX:
        g_appSettings.networkDisplayMode = NETWORK_DISPLAY_RX_TX;
        break;
    case ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY:
        g_appSettings.networkDisplayMode = NETWORK_DISPLAY_RX_TX_SECONDARY;
        break;
    case ID_TRAY_TEMPERATURE_SHOW:
        g_appSettings.showTemperatures = g_appSettings.showTemperatures ? FALSE : TRUE;
        break;
    case ID_TRAY_TEMPERATURE_CELSIUS:
        g_appSettings.temperatureFahrenheit = FALSE;
        break;
    case ID_TRAY_TEMPERATURE_FAHRENHEIT:
        g_appSettings.temperatureFahrenheit = TRUE;
        break;
    case ID_TRAY_STARTUP_TOGGLE:
        g_appSettings.startupEnabled = g_appSettings.startupEnabled ? FALSE : TRUE;
        break;
    case ID_TRAY_EXIT:
        settingsChanged = false;
        DestroyWindow(hwnd);
        break;
    default:
        if (commandId >= ID_TRAY_GPU_SELECT_BASE && commandId < (ID_TRAY_GPU_SELECT_BASE + 8)) {
            g_appSettings.selectedGpuAdapterIndex = commandId - ID_TRAY_GPU_SELECT_BASE;
        } else if (commandId >= ID_TRAY_DISK_DRIVE_BASE && commandId < (ID_TRAY_DISK_DRIVE_BASE + 26)) {
            UINT driveIndex = commandId - ID_TRAY_DISK_DRIVE_BASE;
            g_appSettings.diskSelectionMask ^= (1u << driveIndex);
        } else if (commandId >= ID_TRAY_NETWORK_PRIMARY_BASE && commandId < (ID_TRAY_NETWORK_PRIMARY_BASE + 16)) {
            const UINT adapterIndex = commandId - ID_TRAY_NETWORK_PRIMARY_BASE;
            if (adapterIndex < g_discoveredNetworkCount && g_discoveredNetworkIfIndices[adapterIndex] != 0) {
                g_appSettings.networkPrimaryMode = NETWORK_PRIMARY_MANUAL;
                g_appSettings.networkPrimaryIfIndex = g_discoveredNetworkIfIndices[adapterIndex];
                if (g_appSettings.networkSecondaryIfIndex == g_appSettings.networkPrimaryIfIndex) {
                    g_appSettings.networkSecondaryIfIndex = 0;
                }
            } else {
                settingsChanged = false;
            }
        } else if (commandId >= ID_TRAY_NETWORK_SECONDARY_BASE && commandId < (ID_TRAY_NETWORK_SECONDARY_BASE + 16)) {
            const UINT adapterIndex = commandId - ID_TRAY_NETWORK_SECONDARY_BASE;
            if (adapterIndex < g_discoveredNetworkCount && g_discoveredNetworkIfIndices[adapterIndex] != 0) {
                const DWORD requestedSecondaryIfIndex = g_discoveredNetworkIfIndices[adapterIndex];
                if (requestedSecondaryIfIndex == g_appSettings.networkPrimaryIfIndex && g_appSettings.networkPrimaryMode == NETWORK_PRIMARY_MANUAL) {
                    settingsChanged = false;
                    break;
                }
                g_appSettings.networkSecondaryEnabled = TRUE;
                g_appSettings.networkSecondaryIfIndex = requestedSecondaryIfIndex;
            } else {
                settingsChanged = false;
            }
        } else {
            settingsChanged = false;
        }
        break;
    }

    if (settingsChanged) {
        ApplyRuntimeSettings(hwnd);
        SaveAppSettings();
    }
}

void ShowTrayContextMenu(HWND hwnd) {
    DiscoverHardwareForMenu();

    HMENU rootMenu = CreatePopupMenu();
    HMENU pollingMenu = CreatePopupMenu();
    HMENU gpuMenu = CreatePopupMenu();
    HMENU gpuModeMenu = CreatePopupMenu();
    HMENU gpuSelectMenu = CreatePopupMenu();
    HMENU diskMenu = CreatePopupMenu();
    HMENU networkMenu = CreatePopupMenu();
    HMENU networkPrimaryMenu = CreatePopupMenu();
    HMENU networkSecondaryMenu = CreatePopupMenu();
    HMENU networkDisplayMenu = CreatePopupMenu();
    HMENU temperatureMenu = CreatePopupMenu();

    AppendMenuW(rootMenu, MF_STRING | CheckedFlag(g_appSettings.overlayVisible), ID_TRAY_TOGGLE_OVERLAY, L"Show Overlay");
    AppendMenuW(rootMenu, MF_STRING, ID_TRAY_RESET_POSITION, L"Reset Overlay Position");
    AppendMenuW(rootMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_CPU) != 0), ID_TRAY_SHOW_CPU, L"Show CPU");
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_RAM) != 0), ID_TRAY_SHOW_RAM, L"Show RAM");
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_GPU) != 0), ID_TRAY_SHOW_GPU, L"Show GPU(s)");
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_DISK) != 0), ID_TRAY_SHOW_DISK, L"Show Disk(s)");
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_NETWORK) != 0), ID_TRAY_SHOW_NETWORK, L"Show Network");

    AppendMenuW(gpuModeMenu, MF_STRING | CheckedFlag(g_appSettings.gpuDisplayMode == GPU_DISPLAY_TARGETED), ID_TRAY_GPU_MODE_TARGETED, L"Targeted (Single GPU)");
    AppendMenuW(gpuModeMenu, MF_STRING | CheckedFlag(g_appSettings.gpuDisplayMode == GPU_DISPLAY_HIGHEST_LOAD), ID_TRAY_GPU_MODE_HIGHEST_LOAD, L"Highest-Load (Dynamic)");
    AppendMenuW(gpuModeMenu, MF_STRING | CheckedFlag(g_appSettings.gpuDisplayMode == GPU_DISPLAY_MULTI_GPU), ID_TRAY_GPU_MODE_MULTI_GPU, L"Multi-GPU (All Rows)");
    AppendMenuW(gpuModeMenu, MF_STRING | CheckedFlag(g_appSettings.gpuDisplayMode == GPU_DISPLAY_AGGREGATE), ID_TRAY_GPU_MODE_AGGREGATE, L"Aggregate (Average Util)");

    AppendMenuW(gpuMenu, MF_STRING | CheckedFlag(g_appSettings.showGpuVram), ID_TRAY_GPU_SHOW_VRAM, L"Show Dedicated VRAM");
    AppendMenuW(gpuMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(gpuMenu, MF_POPUP, (UINT_PTR)gpuModeMenu, L"Display Mode");
    AppendMenuW(gpuMenu, MF_SEPARATOR, 0, NULL);

    if (g_discoveredGpuCount == 0) {
        AppendMenuW(gpuSelectMenu, MF_STRING | MF_GRAYED, 0, L"No adapters detected");
    } else {
        for (UINT i = 0; i < g_discoveredGpuCount; ++i) {
            wchar_t gpuLabel[192] = {};
            swprintf_s(gpuLabel, L"GPU %u: %ls", i, g_discoveredGpuNames[i]);
            AppendMenuW(gpuSelectMenu, MF_STRING | CheckedFlag(g_appSettings.selectedGpuAdapterIndex == i), ID_TRAY_GPU_SELECT_BASE + i, gpuLabel);
        }
    }
    AppendMenuW(gpuMenu, MF_POPUP, (UINT_PTR)gpuSelectMenu, L"Select GPU Source");
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)gpuMenu, L"GPUs");

    if (g_discoveredDriveCount == 0) {
        AppendMenuW(diskMenu, MF_STRING | MF_GRAYED, 0, L"No drives detected");
    } else {
        for (UINT i = 0; i < g_discoveredDriveCount; ++i) {
            wchar_t driveLetter = g_discoveredDriveLabels[i][0];
            UINT driveIndex = (driveLetter >= L'A' && driveLetter <= L'Z') ? (UINT)(driveLetter - L'A') : 0;
            BOOL selected = ((g_appSettings.diskSelectionMask & (1u << driveIndex)) != 0);
            AppendMenuW(diskMenu, MF_STRING | CheckedFlag(selected), ID_TRAY_DISK_DRIVE_BASE + driveIndex, g_discoveredDriveLabels[i]);
        }
        AppendMenuW(diskMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(diskMenu, MF_STRING, ID_TRAY_DISK_SELECT_ALL, L"Select All");
        AppendMenuW(diskMenu, MF_STRING, ID_TRAY_DISK_SELECT_NONE, L"Select None");
    }
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)diskMenu, L"Disks");

    AppendMenuW(networkPrimaryMenu, MF_STRING | CheckedFlag(g_appSettings.networkPrimaryMode == NETWORK_PRIMARY_AUTO), ID_TRAY_NETWORK_PRIMARY_AUTO, L"Auto (Default Route)");
    AppendMenuW(networkPrimaryMenu, MF_SEPARATOR, 0, NULL);
    if (g_discoveredNetworkCount == 0) {
        AppendMenuW(networkPrimaryMenu, MF_STRING | MF_GRAYED, 0, L"No adapters detected");
    } else {
        for (UINT i = 0; i < g_discoveredNetworkCount; ++i) {
            const BOOL isChecked = (g_appSettings.networkPrimaryMode == NETWORK_PRIMARY_MANUAL) && (g_appSettings.networkPrimaryIfIndex == g_discoveredNetworkIfIndices[i]);
            AppendMenuW(networkPrimaryMenu, MF_STRING | CheckedFlag(isChecked), ID_TRAY_NETWORK_PRIMARY_BASE + i, g_discoveredNetworkNames[i]);
        }
    }

    AppendMenuW(networkSecondaryMenu, MF_STRING | CheckedFlag(g_appSettings.networkSecondaryEnabled), ID_TRAY_NETWORK_SECONDARY_ENABLE, L"Enable Secondary Adapter");
    AppendMenuW(networkSecondaryMenu, MF_SEPARATOR, 0, NULL);
    if (g_discoveredNetworkCount == 0) {
        AppendMenuW(networkSecondaryMenu, MF_STRING | MF_GRAYED, 0, L"No adapters detected");
    } else {
        for (UINT i = 0; i < g_discoveredNetworkCount; ++i) {
            UINT secondaryFlags = MF_STRING;
            if (!g_appSettings.networkSecondaryEnabled) {
                secondaryFlags |= MF_GRAYED;
            }
            if (g_appSettings.networkPrimaryMode == NETWORK_PRIMARY_MANUAL && g_discoveredNetworkIfIndices[i] == g_appSettings.networkPrimaryIfIndex) {
                secondaryFlags |= MF_GRAYED;
            }
            if (g_appSettings.networkSecondaryEnabled && g_appSettings.networkSecondaryIfIndex == g_discoveredNetworkIfIndices[i]) {
                secondaryFlags |= MF_CHECKED;
            }
            AppendMenuW(networkSecondaryMenu, secondaryFlags, ID_TRAY_NETWORK_SECONDARY_BASE + i, g_discoveredNetworkNames[i]);
        }
    }

    AppendMenuW(networkDisplayMenu, MF_STRING | CheckedFlag(g_appSettings.networkDisplayMode == NETWORK_DISPLAY_RX_TX), ID_TRAY_NETWORK_DISPLAY_RXTX, L"Primary RX/TX Split");
    AppendMenuW(networkDisplayMenu, MF_STRING | CheckedFlag(g_appSettings.networkDisplayMode == NETWORK_DISPLAY_RX_TX_SECONDARY), ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY, L"Primary + Secondary RX/TX");

    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkPrimaryMenu, L"Primary Adapter");
    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkSecondaryMenu, L"Secondary Adapter");
    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkDisplayMenu, L"Display Mode");
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)networkMenu, L"Network");

    const UINT unitFlags = g_appSettings.showTemperatures ? 0 : MF_GRAYED;
    AppendMenuW(temperatureMenu, MF_STRING | CheckedFlag(g_appSettings.showTemperatures), ID_TRAY_TEMPERATURE_SHOW, L"Show Temperatures");
    AppendMenuW(temperatureMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(temperatureMenu, MF_STRING | unitFlags | CheckedFlag(!g_appSettings.temperatureFahrenheit), ID_TRAY_TEMPERATURE_CELSIUS, L"Celsius (\u00B0C)");
    AppendMenuW(temperatureMenu, MF_STRING | unitFlags | CheckedFlag(g_appSettings.temperatureFahrenheit), ID_TRAY_TEMPERATURE_FAHRENHEIT, L"Fahrenheit (\u00B0F)");
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)temperatureMenu, L"Temperatures");

    AppendMenuW(rootMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(rootMenu, MF_STRING | CheckedFlag(g_appSettings.startupEnabled), ID_TRAY_STARTUP_TOGGLE, L"Launch on Windows Startup");
    AppendMenuW(rootMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pollingMenu, MF_STRING | CheckedFlag(g_appSettings.pollingIntervalMs == 500), ID_TRAY_POLL_500, L"500ms (High precision)");
    AppendMenuW(pollingMenu, MF_STRING | CheckedFlag(g_appSettings.pollingIntervalMs == 1000), ID_TRAY_POLL_1000, L"1000ms (Balanced)");
    AppendMenuW(pollingMenu, MF_STRING | CheckedFlag(g_appSettings.pollingIntervalMs == 2000), ID_TRAY_POLL_2000, L"2000ms (Power saver)");
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)pollingMenu, L"Polling Rate / Refresh Speed");

    AppendMenuW(rootMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(rootMenu, MF_STRING, ID_TRAY_EXIT, L"Exit / Quit");

    POINT cursorPos;
    GetCursorPos(&cursorPos);
    SetForegroundWindow(hwnd);

    UINT selectedCommand = TrackPopupMenu(rootMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursorPos.x, cursorPos.y, 0, hwnd, NULL);
    if (selectedCommand != 0) {
        HandleTrayCommand(hwnd, selectedCommand);
    }

    DestroyMenu(rootMenu);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayContextMenu(hWnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            g_appSettings.overlayVisible = g_appSettings.overlayVisible ? FALSE : TRUE;
            ApplyRuntimeSettings(hWnd);
            SaveAppSettings();
        }
        return 0;
    case WM_DEVICECHANGE:
        if (g_systemMonitor != nullptr) {
            g_systemMonitor->RequestDiskTopologyRefresh();
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            g_isSuspended = true;
        } else if (wParam == WTS_SESSION_UNLOCK) {
            g_isSuspended = false;
            if (g_systemMonitor != nullptr) {
                g_systemMonitor->RefreshNetworkIdentityNow();
                g_systemMonitor->RequestDiskTopologyRefresh();
            }
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            g_isSuspended = true;
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            g_isSuspended = false;
            if (g_systemMonitor != nullptr) {
                g_systemMonitor->RefreshNetworkIdentityNow();
                g_systemMonitor->RequestDiskTopologyRefresh();
            }
        } else if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0) {
            POWERBROADCAST_SETTING* pbs = (POWERBROADCAST_SETTING*)lParam;
            if (pbs->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && pbs->DataLength >= sizeof(DWORD)) {
                DWORD displayState = *(DWORD*)pbs->Data;
                // 0 = off, 1 = on, 2 = dimmed
                g_isSuspended = (displayState == 0);
                if (!g_isSuspended && g_systemMonitor != nullptr) {
                    g_systemMonitor->RefreshNetworkIdentityNow();
                    g_systemMonitor->RequestDiskTopologyRefresh();
                }
            }
        }
        return TRUE;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
        ApplyOverlayBounds(hWnd);
        return 0;
    case WM_DESTROY:
        CleanupTrayIcon();
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}