#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <tchar.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <stdio.h>
#include <wchar.h>
#include "SystemMonitor.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dbghelp.lib")

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

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

enum TrayCommandId : UINT {
    ID_TRAY_TOGGLE_OVERLAY = 5001,
    ID_TRAY_SHOW_CPU = 5002,
    ID_TRAY_SHOW_RAM = 5003,
    ID_TRAY_SHOW_GPU = 5004,
    ID_TRAY_SHOW_DISK = 5005,
    ID_TRAY_SHOW_NETWORK = 5006,
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
    ID_TRAY_GPU_SELECT_BASE = 5450,
    ID_TRAY_NETWORK_PRIMARY_AUTO = 5601,
    ID_TRAY_NETWORK_PRIMARY_BASE = 5610,
    ID_TRAY_NETWORK_SECONDARY_ENABLE = 5630,
    ID_TRAY_NETWORK_SECONDARY_BASE = 5640,
    ID_TRAY_NETWORK_DISPLAY_TOTAL = 5660,
    ID_TRAY_NETWORK_DISPLAY_RXTX = 5661,
    ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY = 5662,
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
    DWORD networkPrimaryMode;
    DWORD networkPrimaryIfIndex;
    BOOL networkSecondaryEnabled;
    DWORD networkSecondaryIfIndex;
    DWORD networkDisplayMode;
    BOOL startupEnabled;
};

static AppSettings g_appSettings = {
    SYSTEM_METRIC_ALL,
    1000,
    0x03FFFFFFu,
    TRUE,
    GPU_DISPLAY_TARGETED,
    0,
    NETWORK_PRIMARY_AUTO,
    0,
    FALSE,
    0,
    NETWORK_DISPLAY_RX_TX,
    FALSE
};
static NOTIFYICONDATAW g_trayIconData = {};
static wchar_t g_configPath[MAX_PATH] = {};
static wchar_t g_diagnosticsPath[MAX_PATH] = {};
static SystemMonitor* g_systemMonitor = nullptr;
static HANDLE g_singleInstanceMutex = NULL;

static wchar_t g_discoveredGpuNames[8][128] = {};
static UINT g_discoveredGpuCount = 0;
static wchar_t g_discoveredDriveLabels[26][8] = {};
static UINT g_discoveredDriveCount = 0;
static DWORD g_discoveredNetworkIfIndices[16] = {};
static wchar_t g_discoveredNetworkNames[16][192] = {};
static UINT g_discoveredNetworkCount = 0;

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
static void FormatMbpsValue(float mbps, char* output, int outputSize);

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

    // Load custom font
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 22.0f);
    if (font == nullptr) {
        // Fallback
        io.Fonts->AddFontDefault();
    }

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Modern UI Styling Overhaul
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f); // 60% opacity black
    style.ScaleAllSizes(1.2f); // Global UI scaling

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    SystemMonitor sysMonitor(g_pd3dDevice);
    g_systemMonitor = &sysMonitor;
    ApplyRuntimeSettings(hwnd);
    InitializeTrayIcon(hwnd, hInstance);

    // Main loop
    bool done = false;
    while (!done)
    {
        // Active Idle Sleep check for iGPU optimization: 
        // Sleep until either a new window message arrives, or the 1000ms loop hits.
        // Doing this limits our application to ~1 FPS of internal execution 
        // unless you're moving your mouse/window, saving 99% logic CPU overhead.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, g_appSettings.pollingIntervalMs, QS_ALLINPUT);

        // Poll and handle messages (inputs, window resize, etc.)
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window resize (we don't resize directly, but just in case)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Update hardware metrics (internally throttled to 1000ms delta)
        sysMonitor.Update();

        if (!g_appSettings.overlayVisible) {
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // "Hold ALT to Move" Win32 Dynamic Swap Logic
        bool isAltHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

        if (isAltHeld) {
            // Remove WS_EX_TRANSPARENT so the window intercepts mouse clicks for ImGui
            if (exStyle & WS_EX_TRANSPARENT) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
            }
        } else {
            // Add WS_EX_TRANSPARENT back to resume click-through behavior
            if (!(exStyle & WS_EX_TRANSPARENT)) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
            }
        }

        // Minimalist HUD Configuration - Default top right
        const RECT workAreaBounds = GetOverlayWorkAreaBounds();
        ImGui::SetNextWindowPos(ImVec2((float)workAreaBounds.right - 400.0f, (float)workAreaBounds.top + 50.0f), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0)); // No border outline
        ImGui::Begin(kHudWindowTitle, nullptr,
            ImGuiWindowFlags_NoDecoration | 
            ImGuiWindowFlags_AlwaysAutoResize | 
            ImGuiWindowFlags_NoSavedSettings | 
            ImGuiWindowFlags_NoFocusOnAppearing | 
            ImGuiWindowFlags_NoNav |
            (isAltHeld ? 0 : ImGuiWindowFlags_NoMove));

        float barWidth = 350.0f;
        float barHeight = 24.0f;

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_CPU) != 0) {
            // CPU Metrics (Blue ProgressBar)
            char cpuBuf[64];
            snprintf(cpuBuf, sizeof(cpuBuf), "%.1f%% @ %.2f GHz", sysMonitor.GetCPUUsage(), sysMonitor.GetCPUGHz());
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "CPU");
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 1.0f, 1.0f)); // Blue
            ImGui::ProgressBar(sysMonitor.GetCPUUsage() / 100.0f, ImVec2(barWidth, barHeight), cpuBuf);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_RAM) != 0) {
            // RAM Metrics (Green ProgressBar)
            char ramBuf[64];
            snprintf(ramBuf, sizeof(ramBuf), "%.1f / %.1f GB (%.0f%%)", sysMonitor.GetRAMUsedGB(), sysMonitor.GetRAMTotalGB(), sysMonitor.GetRAMUsagePercent());
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RAM");
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 1.0f, 0.4f, 1.0f)); // Green
            ImGui::ProgressBar(sysMonitor.GetRAMUsagePercent() / 100.0f, ImVec2(barWidth, barHeight), ramBuf);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        if ((g_appSettings.visibleMetricsMask & SYSTEM_METRIC_DISK) != 0) {
            const UINT diskRows = sysMonitor.GetSelectedDiskMetricCount();
            if (diskRows == 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Disk");
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.6f, 0.1f, 1.0f));
                ImGui::ProgressBar(0.0f, ImVec2(barWidth, barHeight), "No fixed drives selected");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            } else {
                for (UINT row = 0; row < diskRows; ++row) {
                    DiskMetricsSnapshot diskSnapshot = {};
                    if (!sysMonitor.GetSelectedDiskMetric(row, &diskSnapshot)) {
                        continue;
                    }

                    char diskTitle[64];
                    if (diskSnapshot.physicalDiskIndex >= 0) {
                        snprintf(diskTitle, sizeof(diskTitle), "Disk %s (PD%d)", diskSnapshot.driveLabel, diskSnapshot.physicalDiskIndex);
                    } else {
                        snprintf(diskTitle, sizeof(diskTitle), "Disk %s", diskSnapshot.driveLabel);
                    }

                    char diskBuf[192];
                    char readRateText[32];
                    char writeRateText[32];
                    FormatMbpsValue(diskSnapshot.readMbps, readRateText, (int)sizeof(readRateText));
                    FormatMbpsValue(diskSnapshot.writeMbps, writeRateText, (int)sizeof(writeRateText));
                    if (diskSnapshot.fallbackTotal) {
                        snprintf(diskBuf, sizeof(diskBuf), "%.1f%% | R %s | W %s | Fallback", diskSnapshot.activePercent, readRateText, writeRateText);
                    } else {
                        snprintf(diskBuf, sizeof(diskBuf), "%.1f%% | R %s | W %s", diskSnapshot.activePercent, readRateText, writeRateText);
                    }

                    float diskProgress = diskSnapshot.activePercent / 100.0f;
                    if (diskProgress < 0.0f) diskProgress = 0.0f;
                    if (diskProgress > 1.0f) diskProgress = 1.0f;

                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", diskTitle);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.6f, 0.1f, 1.0f));
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
            FormatMbpsValue(primaryNetwork.rxMbps, downText, (int)sizeof(downText));
            FormatMbpsValue(primaryNetwork.txMbps, upText, (int)sizeof(upText));
            snprintf(downBuf, sizeof(downBuf), "v %s", downText);
            snprintf(upBuf, sizeof(upBuf), "^ %s", upText);

            float downProgress = primaryNetwork.rxMbps / 1000.0f;
            float upProgress = primaryNetwork.txMbps / 1000.0f;
            if (downProgress < 0.0f) downProgress = 0.0f;
            if (upProgress < 0.0f) upProgress = 0.0f;
            if (downProgress > 1.0f) downProgress = 1.0f;
            if (upProgress > 1.0f) upProgress = 1.0f;

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
                FormatMbpsValue(secondaryNetwork.rxMbps, downText2, (int)sizeof(downText2));
                FormatMbpsValue(secondaryNetwork.txMbps, upText2, (int)sizeof(upText2));
                snprintf(downBuf2, sizeof(downBuf2), "v %s", downText2);
                snprintf(upBuf2, sizeof(upBuf2), "^ %s", upText2);

                float downProgress2 = secondaryNetwork.rxMbps / 1000.0f;
                float upProgress2 = secondaryNetwork.txMbps / 1000.0f;
                if (downProgress2 < 0.0f) downProgress2 = 0.0f;
                if (upProgress2 < 0.0f) upProgress2 = 0.0f;
                if (downProgress2 > 1.0f) downProgress2 = 1.0f;
                if (upProgress2 > 1.0f) upProgress2 = 1.0f;

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 1.0f, 1.0f));
                ImGui::ProgressBar(downProgress2, ImVec2(miniBarWidth, barHeight), downBuf2);
                ImGui::PopStyleColor();

                ImGui::SameLine(0.0f, miniSpacing);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.45f, 1.0f, 1.0f));
                ImGui::ProgressBar(upProgress2, ImVec2(miniBarWidth, barHeight), upBuf2);
                ImGui::PopStyleColor();
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

                    char gpuBuf[256];
                    if (gpuSnapshot.utilizationAvailable) {
                        snprintf(gpuBuf, sizeof(gpuBuf), "%.1f%% (%s) | %.1f/%.1f GB + %.1f GB shared",
                            gpuSnapshot.usagePercent,
                            gpuSnapshot.engineLabel,
                            gpuSnapshot.dedicatedUsedGB,
                            gpuSnapshot.dedicatedTotalGB,
                            gpuSnapshot.sharedUsedGB);
                    } else if (gpuSnapshot.memoryAvailable) {
                        snprintf(gpuBuf, sizeof(gpuBuf), "N/A (Memory) | %.1f/%.1f GB + %.1f GB shared",
                            gpuSnapshot.dedicatedUsedGB,
                            gpuSnapshot.dedicatedTotalGB,
                            gpuSnapshot.sharedUsedGB);
                    } else {
                        snprintf(gpuBuf, sizeof(gpuBuf), "Unavailable");
                    }

                    float gpuProgress = 0.0f;
                    if (gpuSnapshot.utilizationAvailable) {
                        gpuProgress = gpuSnapshot.usagePercent / 100.0f;
                    } else if (gpuSnapshot.dedicatedTotalGB > 0.0f) {
                        gpuProgress = gpuSnapshot.dedicatedUsedGB / gpuSnapshot.dedicatedTotalGB;
                    }

                    if (gpuProgress > 1.0f) gpuProgress = 1.0f;
                    if (gpuProgress < 0.0f) gpuProgress = 0.0f;

                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s: %s", gpuTitle, gpuSnapshot.adapterName);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.1f, 0.9f, 0.9f, 1.0f));
                    ImGui::ProgressBar(gpuProgress, ImVec2(barWidth, barHeight), gpuBuf);
                    ImGui::PopStyleColor();
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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_systemMonitor = nullptr;

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CleanupSingleInstanceMutex();

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
    if (networkDisplayMode > NETWORK_DISPLAY_RX_TX_SECONDARY) {
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

static void FormatMbpsValue(float mbps, char* output, int outputSize) {
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
    g_appSettings.networkPrimaryMode = NormalizeNetworkPrimaryMode(GetPrivateProfileIntW(L"Network", L"PrimaryMode", NETWORK_PRIMARY_AUTO, g_configPath));
    g_appSettings.networkPrimaryIfIndex = (DWORD)GetPrivateProfileIntW(L"Network", L"PrimaryIfIndex", 0, g_configPath);
    g_appSettings.networkSecondaryEnabled = GetPrivateProfileIntW(L"Network", L"SecondaryEnabled", 0, g_configPath) ? TRUE : FALSE;
    g_appSettings.networkSecondaryIfIndex = (DWORD)GetPrivateProfileIntW(L"Network", L"SecondaryIfIndex", 0, g_configPath);
    g_appSettings.networkDisplayMode = NormalizeNetworkDisplayMode(GetPrivateProfileIntW(L"Network", L"DisplayMode", NETWORK_DISPLAY_RX_TX, g_configPath));
    BOOL startupRegistryEnabled = IsStartupEnabledInRegistry() ? TRUE : FALSE;
    g_appSettings.startupEnabled = GetPrivateProfileIntW(L"General", L"StartWithWindows", startupRegistryEnabled ? 1 : 0, g_configPath) ? TRUE : FALSE;
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
    WriteUIntSetting(L"Network", L"PrimaryMode", NormalizeNetworkPrimaryMode(g_appSettings.networkPrimaryMode));
    WriteUIntSetting(L"Network", L"PrimaryIfIndex", g_appSettings.networkPrimaryIfIndex);
    WriteUIntSetting(L"Network", L"SecondaryEnabled", g_appSettings.networkSecondaryEnabled ? 1u : 0u);
    WriteUIntSetting(L"Network", L"SecondaryIfIndex", g_appSettings.networkSecondaryIfIndex);
    WriteUIntSetting(L"Network", L"DisplayMode", NormalizeNetworkDisplayMode(g_appSettings.networkDisplayMode));
    WriteUIntSetting(L"General", L"StartWithWindows", g_appSettings.startupEnabled ? 1u : 0u);
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
        if (driveType != DRIVE_FIXED) {
            continue;
        }

        swprintf_s(g_discoveredDriveLabels[g_discoveredDriveCount], L"%c:", (wchar_t)(L'A' + i));
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
    case ID_TRAY_NETWORK_DISPLAY_TOTAL:
        g_appSettings.networkDisplayMode = NETWORK_DISPLAY_TOTAL;
        break;
    case ID_TRAY_NETWORK_DISPLAY_RXTX:
        g_appSettings.networkDisplayMode = NETWORK_DISPLAY_RX_TX;
        break;
    case ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY:
        g_appSettings.networkDisplayMode = NETWORK_DISPLAY_RX_TX_SECONDARY;
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

    AppendMenuW(rootMenu, MF_STRING | CheckedFlag(g_appSettings.overlayVisible), ID_TRAY_TOGGLE_OVERLAY, L"Show Overlay");
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
        AppendMenuW(diskMenu, MF_STRING | MF_GRAYED, 0, L"No fixed drives detected");
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

    AppendMenuW(networkDisplayMenu, MF_STRING | CheckedFlag(g_appSettings.networkDisplayMode == NETWORK_DISPLAY_TOTAL), ID_TRAY_NETWORK_DISPLAY_TOTAL, L"Primary Total Mbps");
    AppendMenuW(networkDisplayMenu, MF_STRING | CheckedFlag(g_appSettings.networkDisplayMode == NETWORK_DISPLAY_RX_TX), ID_TRAY_NETWORK_DISPLAY_RXTX, L"Primary RX/TX Split");
    AppendMenuW(networkDisplayMenu, MF_STRING | CheckedFlag(g_appSettings.networkDisplayMode == NETWORK_DISPLAY_RX_TX_SECONDARY), ID_TRAY_NETWORK_DISPLAY_RXTX_SECONDARY, L"Primary + Secondary RX/TX");

    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkPrimaryMenu, L"Primary Adapter");
    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkSecondaryMenu, L"Secondary Adapter");
    AppendMenuW(networkMenu, MF_POPUP, (UINT_PTR)networkDisplayMenu, L"Display Mode");
    AppendMenuW(rootMenu, MF_POPUP, (UINT_PTR)networkMenu, L"Network");

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