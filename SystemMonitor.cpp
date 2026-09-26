#include "SystemMonitor.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <winioctl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const float kBytesToGB = 1.0f / (1024.0f * 1024.0f * 1024.0f);
static const DWORD kAllDriveSelectionMask = 0x03FFFFFFu;
// Identity refresh is event-driven; the timer is only a safety net. After a network event,
// Network List Manager can lag behind (stale name or "Identifying..."), so the fallback runs
// fast for a short settling window and then drops to the slow steady-state cadence.
static const ULONGLONG kNetworkIdentityFallbackRefreshMs = 30000;
static const ULONGLONG kNetworkIdentitySettlingRefreshMs = 2000;
static const ULONGLONG kNetworkIdentitySettlingWindowMs = 10000;
static const ULONGLONG kNetworkIdentityUnresolvedWindowMs = 60000;
static const ULONGLONG kNetworkIdentityEventDebounceMs = 500;

// Temperatures change slowly, so they are sampled on their own cadence instead of every poll.
// Disk reads are the slowest so NVMe drives can still reach their low-power states.
static const ULONGLONG kCpuTemperaturePollMs = 2000;
static const ULONGLONG kGpuTemperaturePollMs = 2000;
static const ULONGLONG kDiskTemperaturePollMs = 10000;
// Free space rarely changes and GetDiskFreeSpaceExW can touch removable/USB media, so it is
// refreshed on its own slow cadence (and immediately on every disk topology rebuild).
static const ULONGLONG kDiskCapacityPollMs = 30000;
static const float kMinValidTemperatureC = 1.0f;
static const float kMaxValidCpuTemperatureC = 125.0f;
static const float kMaxValidGpuTemperatureC = 150.0f;
static const float kMaxValidDiskTemperatureC = 125.0f;
static const float kDefaultGpuTemperatureWarningC = 80.0f;
static const float kDefaultGpuTemperatureCriticalC = 90.0f;
static const float kDefaultDiskTemperatureWarningC = 70.0f;
static const float kDefaultDiskTemperatureCriticalC = 80.0f;
static const double kKelvinToCelsiusOffset = 273.15;

// MinGW ships no d3dkmthk.h, so the few D3DKMT types used for GPU temperature are declared here
// and the entry points are resolved from gdi32.dll at runtime (x64 layout, 8-byte aligned).
typedef UINT CG_D3DKMT_HANDLE;

struct CG_D3DKMT_OPENADAPTERFROMLUID {
    LUID AdapterLuid;
    CG_D3DKMT_HANDLE hAdapter;
};

struct CG_D3DKMT_QUERYADAPTERINFO {
    CG_D3DKMT_HANDLE hAdapter;
    UINT Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};

struct CG_D3DKMT_CLOSEADAPTER {
    CG_D3DKMT_HANDLE hAdapter;
};

struct CG_D3DKMT_ADAPTER_PERFDATA {
    UINT32 PhysicalAdapterIndex;
    ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG FanRPM;
    ULONG Power;       // tenths of a percent
    ULONG Temperature; // tenths of a degree Celsius
    UCHAR PowerStateOverride;
};

struct CG_D3DKMT_ADAPTER_PERFDATACAPS {
    UINT32 PhysicalAdapterIndex;
    ULONGLONG MaxMemoryBandwidth;
    ULONGLONG MaxPCIEBandwidth;
    ULONG MaxFanRPM;
    ULONG TemperatureMax;     // tenths of a degree Celsius
    ULONG TemperatureWarning; // tenths of a degree Celsius
};

static_assert(sizeof(CG_D3DKMT_ADAPTER_PERFDATA) == 64, "D3DKMT_ADAPTER_PERFDATA layout mismatch");
static_assert(sizeof(CG_D3DKMT_ADAPTER_PERFDATACAPS) == 40, "D3DKMT_ADAPTER_PERFDATACAPS layout mismatch");

static const UINT kKmtQueryAdapterPerfData = 62;     // KMTQAITYPE_ADAPTERPERFDATA
static const UINT kKmtQueryAdapterPerfDataCaps = 63; // KMTQAITYPE_ADAPTERPERFDATA_CAPS

typedef LONG (APIENTRY* PFN_CG_D3DKMTOpenAdapterFromLuid)(CG_D3DKMT_OPENADAPTERFROMLUID*);
typedef LONG (APIENTRY* PFN_CG_D3DKMTQueryAdapterInfo)(const CG_D3DKMT_QUERYADAPTERINFO*);
typedef LONG (APIENTRY* PFN_CG_D3DKMTCloseAdapter)(const CG_D3DKMT_CLOSEADAPTER*);

struct D3dkmtApi {
    PFN_CG_D3DKMTOpenAdapterFromLuid openAdapterFromLuid;
    PFN_CG_D3DKMTQueryAdapterInfo queryAdapterInfo;
    PFN_CG_D3DKMTCloseAdapter closeAdapter;
};

static const D3dkmtApi* GetD3dkmtApi() {
    static bool attempted = false;
    static D3dkmtApi api = {};
    if (!attempted) {
        attempted = true;
        HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
        if (gdi32 == NULL) {
            gdi32 = LoadLibraryW(L"gdi32.dll");
        }
        if (gdi32 != NULL) {
            api.openAdapterFromLuid = (PFN_CG_D3DKMTOpenAdapterFromLuid)(void*)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
            api.queryAdapterInfo = (PFN_CG_D3DKMTQueryAdapterInfo)(void*)GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
            api.closeAdapter = (PFN_CG_D3DKMTCloseAdapter)(void*)GetProcAddress(gdi32, "D3DKMTCloseAdapter");
        }
    }

    if (api.openAdapterFromLuid == NULL || api.queryAdapterInfo == NULL || api.closeAdapter == NULL) {
        return NULL;
    }
    return &api;
}

static bool IsIntervalDue(ULONGLONG lastTick, ULONGLONG now, ULONGLONG intervalMs) {
    return lastTick == 0 || (now - lastTick) >= intervalMs;
}

enum GpuEngineType {
    GPU_ENGINE_3D = 0,
    GPU_ENGINE_COMPUTE,
    GPU_ENGINE_COPY,
    GPU_ENGINE_DECODE,
    GPU_ENGINE_ENCODE,
    GPU_ENGINE_PROCESSING,
    GPU_ENGINE_GDI,
    GPU_ENGINE_COUNT
};

static const char* const kGpuEngineLabels[GPU_ENGINE_COUNT] = {
    "3D",
    "Compute",
    "Copy",
    "Decode",
    "Encode",
    "Process",
    "GDI"
};

static DWORD ClampPollingIntervalMs(DWORD pollingIntervalMs) {
    if (pollingIntervalMs < 500) {
        return 500;
    }
    if (pollingIntervalMs > 2000) {
        return 2000;
    }
    return pollingIntervalMs;
}

static DWORD ClampGpuDisplayMode(DWORD gpuDisplayMode) {
    if (gpuDisplayMode > GPU_DISPLAY_AGGREGATE) {
        return GPU_DISPLAY_TARGETED;
    }
    return gpuDisplayMode;
}

static DWORD ClampNetworkPrimaryMode(DWORD networkPrimaryMode) {
    if (networkPrimaryMode > NETWORK_PRIMARY_MANUAL) {
        return NETWORK_PRIMARY_AUTO;
    }
    return networkPrimaryMode;
}

static DWORD ClampNetworkDisplayMode(DWORD networkDisplayMode) {
    if (networkDisplayMode == NETWORK_DISPLAY_TOTAL || networkDisplayMode > NETWORK_DISPLAY_RX_TX_SECONDARY) {
        return NETWORK_DISPLAY_RX_TX;
    }
    return networkDisplayMode;
}

static int ParseLeadingDiskIndex(const char* instanceName) {
    if (instanceName == NULL || !isdigit((unsigned char)instanceName[0])) {
        return -1;
    }

    int parsedIndex = 0;
    size_t i = 0;
    while (instanceName[i] != '\0' && isdigit((unsigned char)instanceName[i])) {
        parsedIndex = (parsedIndex * 10) + (instanceName[i] - '0');
        ++i;
    }

    if (i == 0) {
        return -1;
    }

    if (instanceName[i] != '\0' && instanceName[i] != ' ') {
        return -1;
    }

    return parsedIndex;
}

void SystemMonitor::CopyWideToUtf8(const wchar_t* source, char* destination, int destinationSize) {
    if (destination == NULL || destinationSize <= 0) {
        return;
    }

    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, source, -1, destination, destinationSize, NULL, NULL) <= 0) {
        destination[0] = '\0';
    }
}

bool SystemMonitor::ContainsIgnoreCase(const char* haystack, const char* needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    const size_t needleLen = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; ++i) {
        size_t j = 0;
        while (j < needleLen && haystack[i + j] != '\0') {
            const unsigned char h = (unsigned char)haystack[i + j];
            const unsigned char n = (unsigned char)needle[j];
            if (tolower(h) != tolower(n)) {
                break;
            }
            ++j;
        }

        if (j == needleLen) {
            return true;
        }
    }

    return false;
}

bool SystemMonitor::EnsurePdhBuffer(BYTE*& buffer, DWORD& bufferCapacity, DWORD requiredSize) {
    if (requiredSize == 0) {
        return false;
    }

    if (buffer != NULL && bufferCapacity >= requiredSize) {
        return true;
    }

    BYTE* resized = (BYTE*)realloc(buffer, requiredSize);
    if (resized == NULL) {
        return false;
    }

    buffer = resized;
    bufferCapacity = requiredSize;
    return true;
}

SystemMonitor::SystemMonitor(ID3D11Device* d3dDevice)
    : m_lastUpdateTime(0),
      m_lastNetworkIdentityRefresh(0),
      m_lastNetworkIdentityEventTick(0),
      m_networkIdentityRefreshRequested(false),
      m_networkSsidRefreshRequested(false),
      m_networkCallbacksEnabled(false),
      m_diskTopologyRefreshRequested(false),
      m_pollingIntervalMs(1000),
      m_enabledMetricsMask(SYSTEM_METRIC_ALL),
      m_diskSelectionMask(kAllDriveSelectionMask),
      m_cachedLogicalDrivesMask(0),
      m_gpuDisplayMode(GPU_DISPLAY_TARGETED),
      m_selectedGpuAdapterIndex(0),
      m_gpuActiveAdapterIndex(0),
      m_networkPrimaryMode(NETWORK_PRIMARY_AUTO),
      m_networkPrimaryIfIndex(0),
      m_networkSecondaryEnabled(FALSE),
      m_networkSecondaryIfIndex(0),
      m_networkDisplayMode(NETWORK_DISPLAY_RX_TX),
      m_temperaturesEnabled(true),
      m_lastCpuTemperaturePoll(0),
      m_lastGpuTemperaturePoll(0),
      m_lastDiskTemperaturePoll(0),
      m_lastDiskCapacityPoll(0),
      m_cpuUsage(0.0f),
      m_cpuGHz(0.0f),
      m_cpuTemperatureAvailable(false),
      m_cpuTemperatureC(0.0f),
      m_ramSpeedMHz(0),
      m_ramUsedGB(0.0f),
      m_ramTotalGB(0.0f),
      m_ramUsagePercent(0.0f),
      m_diskUsage(0.0f),
      m_cpuBaseFreqMHz(0),
      m_gpuAdapterCount(0),
      m_networkAdapterCount(0),
      m_primaryNetworkSlot(-1),
      m_secondaryNetworkSlot(-1),
      m_hasSecondaryNetworkMetrics(false),
    m_networkAddressChangeHandle(NULL),
    m_networkAddressChangeWaitHandle(NULL),
    m_networkAddressChangeOverlapped{},
      m_pdhQuery(NULL),
      m_pdhCpuCounter(NULL),
      m_pdhCpuPerfCounter(NULL),
      m_pdhDiskInstanceCounter(NULL),
      m_pdhGpuEngineCounter(NULL),
      m_pdhDiskBuffer(NULL),
      m_pdhDiskBufferSize(0),
      m_pdhGpuBuffer(NULL),
      m_pdhGpuBufferSize(0),
      m_processGpuTracking(false),
      m_processGpuSamples(NULL),
      m_processGpuSampleCapacity(0),
      m_processGpuUsage(NULL),
      m_processGpuUsageCapacity(0),
      m_processGpuUsageCount(0),
      m_pdhThermalQuery(NULL),
      m_pdhThermalCounter(NULL),
      m_pdhThermalHighPrecision(false),
      m_pdhThermalBuffer(NULL),
      m_pdhThermalBufferSize(0)
{
    m_cpuName[0] = '\0';
    QueryCpuName(m_cpuName, sizeof(m_cpuName));
    m_ramSpeedMHz = QueryRamSpeedMHz();

    ZeroMemory(&m_legacyGpuDisplay, sizeof(m_legacyGpuDisplay));
    m_legacyGpuDisplay.adapterIndex = 0;
    snprintf(m_legacyGpuDisplay.adapterName, sizeof(m_legacyGpuDisplay.adapterName), "Unknown GPU");
    snprintf(m_legacyGpuDisplay.engineLabel, sizeof(m_legacyGpuDisplay.engineLabel), "N/A");

    snprintf(m_networkAdapterName, sizeof(m_networkAdapterName), "Disconnected");
    m_networkSSID[0] = '\0';
    snprintf(m_networkDisplayName, sizeof(m_networkDisplayName), "Disconnected");

    for (UINT i = 0; i < kMaxGpuAdapters; ++i) {
        m_gpuAdapters[i].valid = false;
        ZeroMemory(&m_gpuAdapters[i].adapterLuid, sizeof(LUID));
        m_gpuAdapters[i].adapterName[0] = '\0';
        m_gpuAdapters[i].luidPatternHL[0] = '\0';
        m_gpuAdapters[i].luidPatternLH[0] = '\0';
        m_gpuAdapters[i].adapter3 = NULL;
        m_gpuAdapters[i].kmtAdapterHandle = 0;
        m_gpuAdapters[i].kmtTemperatureSupported = false;

        ZeroMemory(&m_gpuSnapshots[i], sizeof(GPUMetricsSnapshot));
        m_gpuSnapshots[i].adapterIndex = i;
        snprintf(m_gpuSnapshots[i].adapterName, sizeof(m_gpuSnapshots[i].adapterName), "GPU %u", i);
        snprintf(m_gpuSnapshots[i].engineLabel, sizeof(m_gpuSnapshots[i].engineLabel), "N/A");
    }

    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        m_driveCounters[i].selected = false;
        m_driveCounters[i].driveLetter = (char)('A' + i);
        m_driveCounters[i].physicalDiskIndex = -1;
        m_driveCounters[i].fallbackTotal = false;
        m_driveCounters[i].pdhInstance[0] = L'\0';
        m_driveCounters[i].idleCounter = NULL;
        m_driveCounters[i].readCounter = NULL;
        m_driveCounters[i].writeCounter = NULL;
        m_driveCounters[i].activePercent = 0.0f;
        m_driveCounters[i].readKBps = 0.0f;
        m_driveCounters[i].writeKBps = 0.0f;
        m_driveCounters[i].temperatureSupported = false;
        m_driveCounters[i].temperatureAvailable = false;
        m_driveCounters[i].temperatureC = 0.0f;
        m_driveCounters[i].temperatureWarningC = 0.0f;
        m_driveCounters[i].temperatureCriticalC = 0.0f;
    }

    for (UINT i = 0; i < kMaxNetworkAdapters; ++i) {
        m_networkAdapters[i].valid = false;
        m_networkAdapters[i].ifIndex = 0;
        m_networkAdapters[i].interfaceLuidValue = 0;
        m_networkAdapters[i].connected = false;
        m_networkAdapters[i].hasGateway = false;
        m_networkAdapters[i].isWifi = false;
        m_networkAdapters[i].hasAdapterGuid = false;
        ZeroMemory(&m_networkAdapters[i].adapterGuid, sizeof(GUID));
        m_networkAdapters[i].adapterName[0] = '\0';
        m_networkAdapters[i].ssid[0] = '\0';
        m_networkAdapters[i].displayName[0] = '\0';
        m_networkAdapters[i].lastSsidQueryTick = 0;
    }

    ResetNetworkDeltaState(&m_primaryDeltaState);
    ResetNetworkDeltaState(&m_secondaryDeltaState);
    ClearNetworkSnapshots();

    InitializeNetworkNotifications();

    InitializeGpuMonitoring(d3dDevice);

    // Read static CPU base clock from registry to avoid higher-cost WMI queries.
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dataSize = sizeof(DWORD);
        RegQueryValueExA(hKey, "~MHz", NULL, NULL, (LPBYTE)&m_cpuBaseFreqMHz, &dataSize);
        RegCloseKey(hKey);
    }
    if (m_cpuBaseFreqMHz == 0) {
        m_cpuBaseFreqMHz = 2000;
    }

    if (PdhOpenQueryA(NULL, 0, &m_pdhQuery) == ERROR_SUCCESS) {
        // % Processor Time (busy time / elapsed time across all logical processors) is what Task Manager
        // shows since KB5064081 (Windows 11 24H2/25H2, Aug 2025). The older frequency-scaled
        // % Processor Utility now only appears in Task Manager's optional "CPU Utility" column.
        PdhAddEnglishCounterW(m_pdhQuery, L"\\Processor(_Total)\\% Processor Time", 0, &m_pdhCpuCounter);
        PdhAddEnglishCounterW(m_pdhQuery, L"\\Processor Information(_Total)\\% Processor Performance", 0, &m_pdhCpuPerfCounter);
        if (PdhAddEnglishCounterW(m_pdhQuery, L"\\PhysicalDisk(*)\\% Disk Time", 0, &m_pdhDiskInstanceCounter) != ERROR_SUCCESS) {
            m_pdhDiskInstanceCounter = NULL;
        }
        if (PdhAddEnglishCounterW(m_pdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_pdhGpuEngineCounter) != ERROR_SUCCESS) {
            m_pdhGpuEngineCounter = NULL;
        }

        PdhCollectQueryData(m_pdhQuery);
        RebuildDiskCounters();
        PdhCollectQueryData(m_pdhQuery);
    }

    InitializeThermalZoneQuery();

    RefreshNetworkIdentityNow();
    // Treat startup like a network event: at sign-in NLM often still reports "Identifying...".
    m_lastNetworkIdentityEventTick = GetTickCount64();

    m_lastUpdateTime = GetTickCount64();
    PollMetrics();
}

SystemMonitor::~SystemMonitor() {
    ShutdownNetworkNotifications();

    ClearDiskCounterHandles();

    CloseGpuKmtHandles();
    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        if (m_gpuAdapters[i].adapter3 != NULL) {
            m_gpuAdapters[i].adapter3->Release();
            m_gpuAdapters[i].adapter3 = NULL;
        }
    }

    if (m_pdhQuery != NULL) {
        PdhCloseQuery(m_pdhQuery);
        m_pdhQuery = NULL;
    }

    if (m_pdhThermalQuery != NULL) {
        PdhCloseQuery(m_pdhThermalQuery);
        m_pdhThermalQuery = NULL;
        m_pdhThermalCounter = NULL;
    }

    if (m_pdhThermalBuffer != NULL) {
        free(m_pdhThermalBuffer);
        m_pdhThermalBuffer = NULL;
        m_pdhThermalBufferSize = 0;
    }

    if (m_pdhDiskBuffer != NULL) {
        free(m_pdhDiskBuffer);
        m_pdhDiskBuffer = NULL;
        m_pdhDiskBufferSize = 0;
    }

    if (m_pdhGpuBuffer != NULL) {
        free(m_pdhGpuBuffer);
        m_pdhGpuBuffer = NULL;
        m_pdhGpuBufferSize = 0;
    }

    SetProcessGpuTrackingEnabled(false);
}

void SystemMonitor::SetProcessGpuTrackingEnabled(bool enabled) {
    m_processGpuTracking = enabled;
    if (!enabled) {
        // Only the process window needs these; give the memory back when it closes.
        free(m_processGpuSamples);
        free(m_processGpuUsage);
        m_processGpuSamples = NULL;
        m_processGpuUsage = NULL;
        m_processGpuSampleCapacity = 0;
        m_processGpuUsageCapacity = 0;
        m_processGpuUsageCount = 0;
    }
}

UINT SystemMonitor::GetProcessGpuUsage(const ProcessGpuUsage** outEntries) const {
    if (outEntries != NULL) {
        *outEntries = m_processGpuUsage;
    }
    return m_processGpuUsageCount;
}

void SystemMonitor::SetPollingIntervalMs(DWORD pollingIntervalMs) {
    m_pollingIntervalMs = ClampPollingIntervalMs(pollingIntervalMs);
}

void SystemMonitor::SetEnabledMetricsMask(DWORD enabledMask) {
    m_enabledMetricsMask = enabledMask & SYSTEM_METRIC_ALL;
}

void SystemMonitor::SetDiskSelectionMask(DWORD diskSelectionMask) {
    DWORD normalizedMask = diskSelectionMask & kAllDriveSelectionMask;
    if (normalizedMask == m_diskSelectionMask) {
        return;
    }

    m_diskSelectionMask = normalizedMask;
    RebuildDiskCounters();
}

void SystemMonitor::SetGPUDisplayMode(DWORD gpuDisplayMode) {
    m_gpuDisplayMode = ClampGpuDisplayMode(gpuDisplayMode);
    SyncLegacyGpuFields();
}

void SystemMonitor::SetSelectedGPUAdapterIndex(DWORD gpuAdapterIndex) {
    m_selectedGpuAdapterIndex = gpuAdapterIndex;
    SyncLegacyGpuFields();
}

void SystemMonitor::SetNetworkPrimaryMode(DWORD networkPrimaryMode) {
    DWORD normalized = ClampNetworkPrimaryMode(networkPrimaryMode);
    if (normalized == m_networkPrimaryMode) {
        return;
    }

    m_networkPrimaryMode = normalized;
    RefreshNetworkIdentityNow();
}

void SystemMonitor::SetNetworkPrimaryIfIndex(DWORD networkPrimaryIfIndex) {
    if (networkPrimaryIfIndex == m_networkPrimaryIfIndex) {
        return;
    }

    m_networkPrimaryIfIndex = networkPrimaryIfIndex;
    if (m_networkPrimaryMode == NETWORK_PRIMARY_MANUAL) {
        RefreshNetworkIdentityNow();
    }
}

void SystemMonitor::SetNetworkSecondaryEnabled(BOOL enabled) {
    BOOL normalized = enabled ? TRUE : FALSE;
    if (normalized == m_networkSecondaryEnabled) {
        return;
    }

    m_networkSecondaryEnabled = normalized;
    RefreshNetworkIdentityNow();
}

void SystemMonitor::SetNetworkSecondaryIfIndex(DWORD networkSecondaryIfIndex) {
    if (networkSecondaryIfIndex == m_networkSecondaryIfIndex) {
        return;
    }

    m_networkSecondaryIfIndex = networkSecondaryIfIndex;
    if (m_networkSecondaryEnabled) {
        RefreshNetworkIdentityNow();
    }
}

void SystemMonitor::SetNetworkDisplayMode(DWORD networkDisplayMode) {
    m_networkDisplayMode = ClampNetworkDisplayMode(networkDisplayMode);
}

void SystemMonitor::SetTemperaturesEnabled(bool enabled) {
    if (enabled == m_temperaturesEnabled) {
        return;
    }

    m_temperaturesEnabled = enabled;
    // Clearing also resets the cadence timers, so re-enabling reads fresh values on the next poll.
    m_cpuTemperatureAvailable = false;
    m_lastCpuTemperaturePoll = 0;
    ClearGpuTemperatures();
    ClearDiskTemperatures();
    SyncLegacyGpuFields();
}

void SystemMonitor::RefreshNetworkIdentityNow() {
    RefreshNetworkIdentity();
    m_lastNetworkIdentityRefresh = GetTickCount64();
    m_networkIdentityRefreshRequested.store(false, std::memory_order_relaxed);
    m_networkSsidRefreshRequested.store(false, std::memory_order_relaxed);
    ArmNetworkAddressChangeNotification();
}

void SystemMonitor::RefreshDiskTopologyNow() {
    m_diskTopologyRefreshRequested.store(false, std::memory_order_relaxed);
    RebuildDiskCounters();
}

void SystemMonitor::RequestDiskTopologyRefresh() {
    m_diskTopologyRefreshRequested.store(true, std::memory_order_relaxed);
}

void SystemMonitor::InitializeNetworkNotifications() {
    m_networkCallbacksEnabled.store(true, std::memory_order_relaxed);

    ZeroMemory(&m_networkAddressChangeOverlapped, sizeof(m_networkAddressChangeOverlapped));
    m_networkAddressChangeHandle = NULL;
    m_networkAddressChangeWaitHandle = NULL;
    m_networkAddressChangeOverlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (m_networkAddressChangeOverlapped.hEvent != NULL) {
        if (RegisterWaitForSingleObject(
                &m_networkAddressChangeWaitHandle,
                m_networkAddressChangeOverlapped.hEvent,
                &SystemMonitor::OnNetworkAddressChangeWaitCallback,
                this,
                INFINITE,
                0) == FALSE) {
            CloseHandle(m_networkAddressChangeOverlapped.hEvent);
            m_networkAddressChangeOverlapped.hEvent = NULL;
        } else {
            ArmNetworkAddressChangeNotification();
        }
    }
}

void SystemMonitor::ShutdownNetworkNotifications() {
    m_networkCallbacksEnabled.store(false, std::memory_order_relaxed);

    if (m_networkAddressChangeWaitHandle != NULL) {
        UnregisterWaitEx(m_networkAddressChangeWaitHandle, INVALID_HANDLE_VALUE);
        m_networkAddressChangeWaitHandle = NULL;
    }

    if (m_networkAddressChangeHandle != NULL && m_networkAddressChangeOverlapped.hEvent != NULL) {
        CancelIPChangeNotify(&m_networkAddressChangeOverlapped);
        m_networkAddressChangeHandle = NULL;
    }

    if (m_networkAddressChangeOverlapped.hEvent != NULL) {
        CloseHandle(m_networkAddressChangeOverlapped.hEvent);
        m_networkAddressChangeOverlapped.hEvent = NULL;
    }
}

void SystemMonitor::ArmNetworkAddressChangeNotification() {
    if (m_networkAddressChangeOverlapped.hEvent == NULL) {
        return;
    }

    HANDLE addressChangeEvent = m_networkAddressChangeOverlapped.hEvent;
    if (m_networkAddressChangeHandle != NULL) {
        CancelIPChangeNotify(&m_networkAddressChangeOverlapped);
        m_networkAddressChangeHandle = NULL;
    }

    ZeroMemory(&m_networkAddressChangeOverlapped, sizeof(m_networkAddressChangeOverlapped));
    m_networkAddressChangeOverlapped.hEvent = addressChangeEvent;
    ResetEvent(addressChangeEvent);

    DWORD notifyResult = NotifyAddrChange(&m_networkAddressChangeHandle, &m_networkAddressChangeOverlapped);
    if (notifyResult != NO_ERROR && notifyResult != ERROR_IO_PENDING) {
        m_networkAddressChangeHandle = NULL;
    }
}

void SystemMonitor::RequestNetworkIdentityRefresh() {
    m_networkIdentityRefreshRequested.store(true, std::memory_order_relaxed);
}

bool SystemMonitor::IsNetworkIdentitySettling(ULONGLONG now) const {
    const ULONGLONG sinceEventMs = now - m_lastNetworkIdentityEventTick;
    if (sinceEventMs < kNetworkIdentitySettlingWindowMs) {
        return true;
    }

    // Connected Wi-Fi without a resolved name yet: keep retrying a while longer, but give up on
    // fast polling for networks NLM never names (for example "Unidentified network").
    if (m_primaryNetworkSlot >= 0 && m_primaryNetworkSlot < (int)m_networkAdapterCount) {
        const NetworkAdapterSlot* primary = &m_networkAdapters[m_primaryNetworkSlot];
        if (primary->isWifi && primary->connected && primary->ssid[0] == '\0') {
            return sinceEventMs < kNetworkIdentityUnresolvedWindowMs;
        }
    }

    return false;
}

VOID CALLBACK SystemMonitor::OnNetworkAddressChangeWaitCallback(PVOID context, BOOLEAN) {
    SystemMonitor* monitor = (SystemMonitor*)context;
    if (monitor == NULL || !monitor->m_networkCallbacksEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    monitor->m_networkIdentityRefreshRequested.store(true, std::memory_order_relaxed);
}

const char* SystemMonitor::GetGPUAdapterNameByIndex(UINT index) const {
    if (index >= m_gpuAdapterCount || !m_gpuAdapters[index].valid) {
        return "";
    }
    return m_gpuAdapters[index].adapterName;
}

UINT SystemMonitor::GetDisplayedGPUCount() const {
    if (m_gpuAdapterCount == 0) {
        return 0;
    }

    if (ClampGpuDisplayMode(m_gpuDisplayMode) == GPU_DISPLAY_MULTI_GPU) {
        return m_gpuAdapterCount;
    }

    return 1;
}

bool SystemMonitor::GetDisplayedGPUSnapshot(UINT rowIndex, GPUMetricsSnapshot* outSnapshot) const {
    if (outSnapshot == NULL || m_gpuAdapterCount == 0) {
        return false;
    }

    const DWORD displayMode = ClampGpuDisplayMode(m_gpuDisplayMode);
    if (displayMode == GPU_DISPLAY_MULTI_GPU) {
        if (rowIndex >= m_gpuAdapterCount) {
            return false;
        }

        *outSnapshot = m_gpuSnapshots[rowIndex];
        return true;
    }

    if (rowIndex != 0) {
        return false;
    }

    if (displayMode == GPU_DISPLAY_TARGETED) {
        int index = FindTargetedGpuIndex();
        if (index < 0) {
            return false;
        }

        *outSnapshot = m_gpuSnapshots[index];
        return true;
    }

    if (displayMode == GPU_DISPLAY_HIGHEST_LOAD) {
        int index = FindHighestLoadGpuIndex();
        if (index < 0) {
            return false;
        }

        *outSnapshot = m_gpuSnapshots[index];
        return true;
    }

    if (displayMode == GPU_DISPLAY_AGGREGATE) {
        BuildAggregateGpuSnapshot(outSnapshot);
        return true;
    }

    return false;
}

UINT SystemMonitor::GetSelectedDiskMetricCount() const {
    UINT count = 0;
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        if (m_driveCounters[i].selected) {
            ++count;
        }
    }
    return count;
}

bool SystemMonitor::GetSelectedDiskMetric(UINT rowIndex, DiskMetricsSnapshot* outSnapshot) const {
    if (outSnapshot == NULL) {
        return false;
    }

    UINT currentRow = 0;
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        if (!m_driveCounters[i].selected) {
            continue;
        }

        if (currentRow == rowIndex) {
            outSnapshot->driveLabel[0] = m_driveCounters[i].driveLetter;
            outSnapshot->driveLabel[1] = ':';
            outSnapshot->driveLabel[2] = '\0';
            outSnapshot->physicalDiskIndex = m_driveCounters[i].physicalDiskIndex;
            outSnapshot->fallbackTotal = m_driveCounters[i].fallbackTotal;
            outSnapshot->activePercent = m_driveCounters[i].activePercent;
            outSnapshot->readKBps = m_driveCounters[i].readKBps;
            outSnapshot->writeKBps = m_driveCounters[i].writeKBps;
            outSnapshot->usedGB = m_driveCounters[i].usedGB;
            outSnapshot->totalGB = m_driveCounters[i].totalGB;
            outSnapshot->freeGB = m_driveCounters[i].freeGB;
            outSnapshot->capacityAvailable = m_driveCounters[i].capacityAvailable;
            outSnapshot->temperatureAvailable = m_driveCounters[i].temperatureAvailable;
            outSnapshot->temperatureC = m_driveCounters[i].temperatureC;
            outSnapshot->temperatureWarningC = m_driveCounters[i].temperatureWarningC;
            outSnapshot->temperatureCriticalC = m_driveCounters[i].temperatureCriticalC;
            return true;
        }

        ++currentRow;
    }

    return false;
}

DWORD SystemMonitor::GetNetworkAdapterIfIndex(UINT index) const {
    if (index >= m_networkAdapterCount) {
        return 0;
    }
    return m_networkAdapters[index].ifIndex;
}

const char* SystemMonitor::GetNetworkAdapterNameByIndex(UINT index) const {
    if (index >= m_networkAdapterCount) {
        return "";
    }

    return m_networkAdapters[index].displayName[0] != '\0'
        ? m_networkAdapters[index].displayName
        : m_networkAdapters[index].adapterName;
}

void SystemMonitor::Update() {
    const ULONGLONG currentTime = GetTickCount64();
    if (currentTime - m_lastUpdateTime >= m_pollingIntervalMs) {
        PollMetrics();
        m_lastUpdateTime = currentTime;
    }
}

void SystemMonitor::PollMetrics() {
    const bool pollCpu = (m_enabledMetricsMask & SYSTEM_METRIC_CPU) != 0;
    const bool pollRam = (m_enabledMetricsMask & SYSTEM_METRIC_RAM) != 0;
    const bool pollGpu = (m_enabledMetricsMask & SYSTEM_METRIC_GPU) != 0;
    const bool pollGpuEngines = pollGpu || m_processGpuTracking;
    const bool pollDisk = ((m_enabledMetricsMask & SYSTEM_METRIC_DISK) != 0) && (m_diskSelectionMask != 0);
    const bool pollNetwork = (m_enabledMetricsMask & SYSTEM_METRIC_NETWORK) != 0;

    if (pollNetwork) {
        ULONGLONG now = GetTickCount64();
        const ULONGLONG fallbackIntervalMs = IsNetworkIdentitySettling(now)
            ? kNetworkIdentitySettlingRefreshMs
            : kNetworkIdentityFallbackRefreshMs;
        const bool periodicRefreshDue = (m_lastNetworkIdentityRefresh == 0) || ((now - m_lastNetworkIdentityRefresh) >= fallbackIntervalMs);
        const bool eventRefreshPending = m_networkIdentityRefreshRequested.load(std::memory_order_relaxed) ||
            m_networkSsidRefreshRequested.load(std::memory_order_relaxed);
        const bool quickRefreshDue = eventRefreshPending &&
            (m_lastNetworkIdentityRefresh == 0 || (now - m_lastNetworkIdentityRefresh) >= kNetworkIdentityEventDebounceMs);

        if (quickRefreshDue) {
            m_lastNetworkIdentityEventTick = now;
        }

        if (periodicRefreshDue || quickRefreshDue) {
            RefreshNetworkIdentityNow();
        }
        PollNetworkThroughput();
    } else {
        m_primaryNetworkMetrics.rxMbps = 0.0f;
        m_primaryNetworkMetrics.txMbps = 0.0f;
        m_primaryNetworkMetrics.totalMbps = 0.0f;
        m_secondaryNetworkMetrics.rxMbps = 0.0f;
        m_secondaryNetworkMetrics.txMbps = 0.0f;
        m_secondaryNetworkMetrics.totalMbps = 0.0f;
    }

    if (pollRam) {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            const DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
            const DWORDLONG physMemUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys;

            m_ramTotalGB = (float)((double)totalPhysMem * kBytesToGB);
            m_ramUsedGB = (float)((double)physMemUsed * kBytesToGB);
            m_ramUsagePercent = (float)memInfo.dwMemoryLoad;
        }
    } else {
        m_ramTotalGB = 0.0f;
        m_ramUsedGB = 0.0f;
        m_ramUsagePercent = 0.0f;
    }

    const bool needsPdh = pollCpu || pollDisk || pollGpuEngines;
    if (pollDisk) {
        const DWORD currentDriveMask = GetLogicalDrives();
        const bool topologyRefreshRequested = m_diskTopologyRefreshRequested.exchange(false, std::memory_order_relaxed);
        if (topologyRefreshRequested || currentDriveMask != m_cachedLogicalDrivesMask) {
            RebuildDiskCounters();
        }
    }

    if (m_pdhQuery != NULL && needsPdh) {
        PdhCollectQueryData(m_pdhQuery);

        PDH_FMT_COUNTERVALUE counterVal;

        if (pollCpu && m_pdhCpuCounter != NULL && PdhGetFormattedCounterValue(m_pdhCpuCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
            m_cpuUsage = (float)counterVal.doubleValue;
            if (m_cpuUsage < 0.0f) {
                m_cpuUsage = 0.0f;
            }
            if (m_cpuUsage > 100.0f) {
                m_cpuUsage = 100.0f;
            }
        }

        if (pollCpu && m_pdhCpuPerfCounter != NULL && PdhGetFormattedCounterValue(m_pdhCpuPerfCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
            m_cpuGHz = (m_cpuBaseFreqMHz * ((float)counterVal.doubleValue / 100.0f)) / 1000.0f;
            if (m_cpuGHz < 0.0f) {
                m_cpuGHz = 0.0f;
            }
        }

        if (pollDisk) {
            m_diskUsage = 0.0f;
            const ULONGLONG capacityNow = GetTickCount64();
            const bool refreshCapacity = IsIntervalDue(m_lastDiskCapacityPoll, capacityNow, kDiskCapacityPollMs);
            if (refreshCapacity) {
                m_lastDiskCapacityPoll = capacityNow;
            }

            for (UINT i = 0; i < kMaxDriveLetters; ++i) {
                if (!m_driveCounters[i].selected) {
                    continue;
                }

                m_driveCounters[i].activePercent = 0.0f;
                m_driveCounters[i].readKBps = 0.0f;
                m_driveCounters[i].writeKBps = 0.0f;

                // Active time is 100 - % Idle Time, as in Task Manager. % Disk Time is a queue-length
                // estimate that routinely exceeds 100% on SSDs.
                if (m_driveCounters[i].idleCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].idleCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].activePercent = 100.0f - (float)counterVal.doubleValue;
                    if (m_driveCounters[i].activePercent < 0.0f) {
                        m_driveCounters[i].activePercent = 0.0f;
                    }
                    if (m_driveCounters[i].activePercent > 100.0f) {
                        m_driveCounters[i].activePercent = 100.0f;
                    }
                }

                if (m_driveCounters[i].readCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].readCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].readKBps = (float)(counterVal.doubleValue / 1024.0);
                    if (m_driveCounters[i].readKBps < 0.0f) {
                        m_driveCounters[i].readKBps = 0.0f;
                    }
                }

                if (m_driveCounters[i].writeCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].writeCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].writeKBps = (float)(counterVal.doubleValue / 1024.0);
                    if (m_driveCounters[i].writeKBps < 0.0f) {
                        m_driveCounters[i].writeKBps = 0.0f;
                    }
                }

                if (refreshCapacity) {
                    wchar_t rootPath[4] = { (wchar_t)(L'A' + i), L':', L'\\', L'\0' };
                    ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFreeBytes = {};
                    if (GetDiskFreeSpaceExW(rootPath, &freeBytesAvail, &totalBytes, &totalFreeBytes) && totalBytes.QuadPart > 0) {
                        m_driveCounters[i].totalGB = (float)((double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0));
                        m_driveCounters[i].freeGB = (float)((double)totalFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0));
                        m_driveCounters[i].usedGB = m_driveCounters[i].totalGB - m_driveCounters[i].freeGB;
                        m_driveCounters[i].capacityAvailable = true;
                    }
                }

                if (m_driveCounters[i].activePercent > m_diskUsage) {
                    m_diskUsage = m_driveCounters[i].activePercent;
                }
            }
        }
    }

    const ULONGLONG temperatureNow = GetTickCount64();

    if (pollCpu && m_temperaturesEnabled) {
        if (IsIntervalDue(m_lastCpuTemperaturePoll, temperatureNow, kCpuTemperaturePollMs)) {
            PollCpuTemperature();
            m_lastCpuTemperaturePoll = temperatureNow;
        }
    } else {
        m_cpuTemperatureAvailable = false;
        m_lastCpuTemperaturePoll = 0;
    }

    if (pollDisk && m_temperaturesEnabled) {
        if (IsIntervalDue(m_lastDiskTemperaturePoll, temperatureNow, kDiskTemperaturePollMs)) {
            PollDiskTemperatures();
            m_lastDiskTemperaturePoll = temperatureNow;
        }
    } else {
        ClearDiskTemperatures();
    }

    if (!pollCpu) {
        m_cpuUsage = 0.0f;
        m_cpuGHz = 0.0f;
    }

    if (!pollDisk) {
        m_diskUsage = 0.0f;
        for (UINT i = 0; i < kMaxDriveLetters; ++i) {
            m_driveCounters[i].activePercent = 0.0f;
            m_driveCounters[i].readKBps = 0.0f;
            m_driveCounters[i].writeKBps = 0.0f;
        }
    }

    if (pollGpu && m_temperaturesEnabled) {
        if (IsIntervalDue(m_lastGpuTemperaturePoll, temperatureNow, kGpuTemperaturePollMs)) {
            PollGpuTemperatures();
            m_lastGpuTemperaturePoll = temperatureNow;
        }
    } else {
        ClearGpuTemperatures();
    }

    if (pollGpuEngines) {
        PollGpuMetrics();
    } else {
        for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
            m_gpuSnapshots[i].usagePercent = 0.0f;
            m_gpuSnapshots[i].dedicatedUsedGB = 0.0f;
            m_gpuSnapshots[i].sharedUsedGB = 0.0f;
            m_gpuSnapshots[i].utilizationAvailable = false;
            m_gpuSnapshots[i].memoryAvailable = false;
            snprintf(m_gpuSnapshots[i].engineLabel, sizeof(m_gpuSnapshots[i].engineLabel), "N/A");
        }
        SyncLegacyGpuFields();
    }
}

void SystemMonitor::InitializeGpuMonitoring(ID3D11Device* d3dDevice) {
    CloseGpuKmtHandles();
    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        if (m_gpuAdapters[i].adapter3 != NULL) {
            m_gpuAdapters[i].adapter3->Release();
            m_gpuAdapters[i].adapter3 = NULL;
        }
    }

    m_gpuAdapterCount = 0;
    m_gpuActiveAdapterIndex = 0;

    LUID activeAdapterLuid = {};
    bool hasActiveAdapterLuid = false;

    if (d3dDevice != NULL) {
        IDXGIDevice* dxgiDevice = NULL;
        if (SUCCEEDED(d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = NULL;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                IDXGIAdapter1* adapter1 = NULL;
                if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1)))) {
                    DXGI_ADAPTER_DESC1 desc1;
                    ZeroMemory(&desc1, sizeof(desc1));
                    if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                        activeAdapterLuid = desc1.AdapterLuid;
                        hasActiveAdapterLuid = true;
                    }
                    adapter1->Release();
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
    }

    IDXGIFactory1* factory = NULL;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        SyncLegacyGpuFields();
        return;
    }

    for (UINT adapterIndex = 0; adapterIndex < kMaxGpuAdapters; ++adapterIndex) {
        IDXGIAdapter1* adapter1 = NULL;
        if (factory->EnumAdapters1(adapterIndex, &adapter1) != S_OK) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc1;
        ZeroMemory(&desc1, sizeof(desc1));
        if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
            GPUAdapterSlot* gpuSlot = &m_gpuAdapters[m_gpuAdapterCount];
            gpuSlot->valid = true;
            gpuSlot->adapterLuid = desc1.AdapterLuid;
            CopyWideToUtf8(desc1.Description, gpuSlot->adapterName, (int)sizeof(gpuSlot->adapterName));

            const unsigned int luidHigh = (unsigned int)(DWORD)gpuSlot->adapterLuid.HighPart;
            const unsigned int luidLow = (unsigned int)gpuSlot->adapterLuid.LowPart;
            snprintf(gpuSlot->luidPatternHL, sizeof(gpuSlot->luidPatternHL), "luid_0x%08x_0x%08x", luidHigh, luidLow);
            snprintf(gpuSlot->luidPatternLH, sizeof(gpuSlot->luidPatternLH), "luid_0x%08x_0x%08x", luidLow, luidHigh);

            gpuSlot->adapter3 = NULL;
            adapter1->QueryInterface(IID_PPV_ARGS(&gpuSlot->adapter3));

            GPUMetricsSnapshot* snapshot = &m_gpuSnapshots[m_gpuAdapterCount];
            ZeroMemory(snapshot, sizeof(GPUMetricsSnapshot));
            snapshot->adapterIndex = m_gpuAdapterCount;
            snprintf(snapshot->adapterName, sizeof(snapshot->adapterName), "%s", gpuSlot->adapterName[0] != '\0' ? gpuSlot->adapterName : "Unknown GPU");
            snapshot->dedicatedTotalGB = (float)((double)desc1.DedicatedVideoMemory * kBytesToGB);
            snapshot->usagePercent = 0.0f;
            snapshot->dedicatedUsedGB = 0.0f;
            snapshot->sharedUsedGB = 0.0f;
            snapshot->utilizationAvailable = false;
            snapshot->memoryAvailable = false;
            snprintf(snapshot->engineLabel, sizeof(snapshot->engineLabel), "N/A");
            snapshot->temperatureWarningC = kDefaultGpuTemperatureWarningC;
            snapshot->temperatureCriticalC = kDefaultGpuTemperatureCriticalC;

            // A kernel-mode adapter handle is kept open for the temperature query. Software adapters
            // (Microsoft Basic Render Driver) have no sensor.
            gpuSlot->kmtAdapterHandle = 0;
            gpuSlot->kmtTemperatureSupported = false;
            const D3dkmtApi* kmt = GetD3dkmtApi();
            if (kmt != NULL && (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                CG_D3DKMT_OPENADAPTERFROMLUID openAdapter = {};
                openAdapter.AdapterLuid = desc1.AdapterLuid;
                if (kmt->openAdapterFromLuid(&openAdapter) == 0 && openAdapter.hAdapter != 0) {
                    gpuSlot->kmtAdapterHandle = openAdapter.hAdapter;
                    gpuSlot->kmtTemperatureSupported = true;

                    CG_D3DKMT_ADAPTER_PERFDATACAPS caps = {};
                    CG_D3DKMT_QUERYADAPTERINFO capsQuery = {};
                    capsQuery.hAdapter = openAdapter.hAdapter;
                    capsQuery.Type = kKmtQueryAdapterPerfDataCaps;
                    capsQuery.pPrivateDriverData = &caps;
                    capsQuery.PrivateDriverDataSize = sizeof(caps);
                    if (kmt->queryAdapterInfo(&capsQuery) == 0) {
                        const float capsWarningC = (float)caps.TemperatureWarning / 10.0f;
                        const float capsMaxC = (float)caps.TemperatureMax / 10.0f;
                        if (capsWarningC >= kMinValidTemperatureC && capsWarningC <= kMaxValidGpuTemperatureC) {
                            snapshot->temperatureWarningC = capsWarningC;
                        }
                        if (capsMaxC >= snapshot->temperatureWarningC && capsMaxC <= kMaxValidGpuTemperatureC) {
                            snapshot->temperatureCriticalC = capsMaxC;
                        }
                        if (snapshot->temperatureCriticalC < snapshot->temperatureWarningC) {
                            snapshot->temperatureCriticalC = snapshot->temperatureWarningC;
                        }
                    }
                }
            }

            if (hasActiveAdapterLuid && memcmp(&gpuSlot->adapterLuid, &activeAdapterLuid, sizeof(LUID)) == 0) {
                m_gpuActiveAdapterIndex = m_gpuAdapterCount;
            }

            ++m_gpuAdapterCount;
        }

        adapter1->Release();
    }

    factory->Release();

    if (m_gpuAdapterCount > 0) {
        if (m_selectedGpuAdapterIndex >= m_gpuAdapterCount) {
            m_selectedGpuAdapterIndex = m_gpuActiveAdapterIndex < m_gpuAdapterCount ? m_gpuActiveAdapterIndex : 0;
        }
    }

    SyncLegacyGpuFields();
}

int SystemMonitor::FindGpuAdapterIndexForInstance(const char* instanceName) const {
    if (instanceName == NULL) {
        return -1;
    }

    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        if (!m_gpuAdapters[i].valid) {
            continue;
        }

        if ((m_gpuAdapters[i].luidPatternHL[0] != '\0' && ContainsIgnoreCase(instanceName, m_gpuAdapters[i].luidPatternHL)) ||
            (m_gpuAdapters[i].luidPatternLH[0] != '\0' && ContainsIgnoreCase(instanceName, m_gpuAdapters[i].luidPatternLH))) {
            return (int)i;
        }
    }

    if (m_gpuAdapterCount == 1) {
        return 0;
    }

    return -1;
}

int SystemMonitor::FindTargetedGpuIndex() const {
    if (m_gpuAdapterCount == 0) {
        return -1;
    }

    if (m_selectedGpuAdapterIndex < m_gpuAdapterCount) {
        return (int)m_selectedGpuAdapterIndex;
    }

    if (m_gpuActiveAdapterIndex < m_gpuAdapterCount) {
        return (int)m_gpuActiveAdapterIndex;
    }

    return 0;
}

int SystemMonitor::FindHighestLoadGpuIndex() const {
    if (m_gpuAdapterCount == 0) {
        return -1;
    }

    int bestIndex = -1;
    float bestUsage = -1.0f;
    float bestMemory = -1.0f;

    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        const GPUMetricsSnapshot* snapshot = &m_gpuSnapshots[i];
        if (!snapshot->utilizationAvailable) {
            continue;
        }

        if (snapshot->usagePercent > bestUsage ||
            (snapshot->usagePercent == bestUsage && snapshot->dedicatedUsedGB > bestMemory)) {
            bestUsage = snapshot->usagePercent;
            bestMemory = snapshot->dedicatedUsedGB;
            bestIndex = (int)i;
        }
    }

    if (bestIndex >= 0) {
        return bestIndex;
    }

    bestMemory = -1.0f;
    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        if (m_gpuSnapshots[i].dedicatedUsedGB > bestMemory) {
            bestMemory = m_gpuSnapshots[i].dedicatedUsedGB;
            bestIndex = (int)i;
        }
    }

    if (bestIndex >= 0) {
        return bestIndex;
    }

    return FindTargetedGpuIndex();
}

void SystemMonitor::BuildAggregateGpuSnapshot(GPUMetricsSnapshot* outSnapshot) const {
    if (outSnapshot == NULL) {
        return;
    }

    ZeroMemory(outSnapshot, sizeof(GPUMetricsSnapshot));
    outSnapshot->adapterIndex = 0xFFFFFFFFu;
    snprintf(outSnapshot->adapterName, sizeof(outSnapshot->adapterName), "Aggregate (%u GPUs)", m_gpuAdapterCount);
    snprintf(outSnapshot->engineLabel, sizeof(outSnapshot->engineLabel), "N/A");

    double utilizationSum = 0.0;
    UINT utilizationCount = 0;

    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        const GPUMetricsSnapshot* snapshot = &m_gpuSnapshots[i];

        if (snapshot->utilizationAvailable) {
            utilizationSum += snapshot->usagePercent;
            ++utilizationCount;
        }

        outSnapshot->dedicatedUsedGB += snapshot->dedicatedUsedGB;
        outSnapshot->dedicatedTotalGB += snapshot->dedicatedTotalGB;
        outSnapshot->sharedUsedGB += snapshot->sharedUsedGB;
        if (snapshot->memoryAvailable) {
            outSnapshot->memoryAvailable = true;
        }

        // Aggregate shows the hottest adapter, with that adapter's own thresholds.
        if (snapshot->temperatureAvailable &&
            (!outSnapshot->temperatureAvailable || snapshot->temperatureC > outSnapshot->temperatureC)) {
            outSnapshot->temperatureAvailable = true;
            outSnapshot->temperatureC = snapshot->temperatureC;
            outSnapshot->temperatureWarningC = snapshot->temperatureWarningC;
            outSnapshot->temperatureCriticalC = snapshot->temperatureCriticalC;
        }
    }

    if (utilizationCount > 0) {
        outSnapshot->usagePercent = (float)(utilizationSum / (double)utilizationCount);
        if (outSnapshot->usagePercent > 100.0f) {
            outSnapshot->usagePercent = 100.0f;
        }
        outSnapshot->utilizationAvailable = true;
        snprintf(outSnapshot->engineLabel, sizeof(outSnapshot->engineLabel), "Avg");
    }
}

void SystemMonitor::SyncLegacyGpuFields() {
    GPUMetricsSnapshot displaySnapshot;
    if (GetDisplayedGPUSnapshot(0, &displaySnapshot)) {
        m_legacyGpuDisplay = displaySnapshot;
        return;
    }

    ZeroMemory(&m_legacyGpuDisplay, sizeof(m_legacyGpuDisplay));
    m_legacyGpuDisplay.adapterIndex = 0;
    snprintf(m_legacyGpuDisplay.adapterName, sizeof(m_legacyGpuDisplay.adapterName), "Unknown GPU");
    snprintf(m_legacyGpuDisplay.engineLabel, sizeof(m_legacyGpuDisplay.engineLabel), "N/A");
}

void SystemMonitor::PollGpuMetrics() {
    m_processGpuUsageCount = 0;
    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        m_gpuSnapshots[i].usagePercent = 0.0f;
        m_gpuSnapshots[i].dedicatedUsedGB = 0.0f;
        m_gpuSnapshots[i].sharedUsedGB = 0.0f;
        m_gpuSnapshots[i].utilizationAvailable = false;
        m_gpuSnapshots[i].memoryAvailable = false;
        snprintf(m_gpuSnapshots[i].engineLabel, sizeof(m_gpuSnapshots[i].engineLabel), "N/A");
    }

    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        if (m_gpuAdapters[i].adapter3 == NULL) {
            continue;
        }

        bool localAvailable = false;

        DXGI_QUERY_VIDEO_MEMORY_INFO localInfo;
        ZeroMemory(&localInfo, sizeof(localInfo));
        if (SUCCEEDED(m_gpuAdapters[i].adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo))) {
            localAvailable = true;
            m_gpuSnapshots[i].dedicatedUsedGB = (float)((double)localInfo.CurrentUsage * kBytesToGB);
            if (m_gpuSnapshots[i].dedicatedTotalGB <= 0.001f && localInfo.Budget > 0) {
                m_gpuSnapshots[i].dedicatedTotalGB = (float)((double)localInfo.Budget * kBytesToGB);
            }
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO sharedInfo;
        ZeroMemory(&sharedInfo, sizeof(sharedInfo));
        if (SUCCEEDED(m_gpuAdapters[i].adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sharedInfo))) {
            m_gpuSnapshots[i].sharedUsedGB = (float)((double)sharedInfo.CurrentUsage * kBytesToGB);
        }

        m_gpuSnapshots[i].memoryAvailable = localAvailable;
    }

    if (m_pdhQuery == NULL || m_pdhGpuEngineCounter == NULL || m_gpuAdapterCount == 0) {
        SyncLegacyGpuFields();
        return;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS gpuStatus = PdhGetFormattedCounterArrayA(m_pdhGpuEngineCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
    if (!((gpuStatus == (PDH_STATUS)PDH_MORE_DATA || gpuStatus == (PDH_STATUS)ERROR_SUCCESS) && bufferSize > 0 && itemCount > 0)) {
        SyncLegacyGpuFields();
        return;
    }

    if (!EnsurePdhBuffer(m_pdhGpuBuffer, m_pdhGpuBufferSize, bufferSize)) {
        SyncLegacyGpuFields();
        return;
    }

    PDH_FMT_COUNTERVALUE_ITEM_A* itemArray = (PDH_FMT_COUNTERVALUE_ITEM_A*)m_pdhGpuBuffer;
    if (PdhGetFormattedCounterArrayA(m_pdhGpuEngineCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, itemArray) != ERROR_SUCCESS) {
        SyncLegacyGpuFields();
        return;
    }

    float engineTotals[kMaxGpuAdapters][GPU_ENGINE_COUNT];
    bool engineSeen[kMaxGpuAdapters][GPU_ENGINE_COUNT];
    ZeroMemory(engineTotals, sizeof(engineTotals));
    ZeroMemory(engineSeen, sizeof(engineSeen));

    // Instance names start with "pid_<pid>_", so the same items give per-process load for free.
    UINT processSampleCount = 0;
    bool collectProcessSamples = m_processGpuTracking;
    if (collectProcessSamples && itemCount > m_processGpuSampleCapacity) {
        ProcessGpuSample* grown = (ProcessGpuSample*)realloc(m_processGpuSamples, itemCount * sizeof(ProcessGpuSample));
        if (grown != NULL) {
            m_processGpuSamples = grown;
            m_processGpuSampleCapacity = itemCount;
        } else {
            collectProcessSamples = false;
        }
    }

    for (DWORD i = 0; i < itemCount; ++i) {
        const char* instanceName = itemArray[i].szName;
        if (instanceName == NULL || itemArray[i].FmtValue.CStatus != ERROR_SUCCESS) {
            continue;
        }

        const double rawValue = itemArray[i].FmtValue.doubleValue;
        if (rawValue < 0.0) {
            continue;
        }

        const int gpuIndex = FindGpuAdapterIndexForInstance(instanceName);
        if (gpuIndex < 0 || gpuIndex >= (int)m_gpuAdapterCount) {
            continue;
        }

        int engineType = -1;
        if (ContainsIgnoreCase(instanceName, "engtype_3D")) {
            engineType = GPU_ENGINE_3D;
        } else if (ContainsIgnoreCase(instanceName, "engtype_Compute")) {
            engineType = GPU_ENGINE_COMPUTE;
        } else if (ContainsIgnoreCase(instanceName, "engtype_Copy")) {
            engineType = GPU_ENGINE_COPY;
        } else if (ContainsIgnoreCase(instanceName, "engtype_VideoDecode")) {
            engineType = GPU_ENGINE_DECODE;
        } else if (ContainsIgnoreCase(instanceName, "engtype_VideoEncode")) {
            engineType = GPU_ENGINE_ENCODE;
        } else if (ContainsIgnoreCase(instanceName, "engtype_VideoProcessing")) {
            engineType = GPU_ENGINE_PROCESSING;
        } else if (ContainsIgnoreCase(instanceName, "engtype_GDI Render")) {
            engineType = GPU_ENGINE_GDI;
        }

        if (engineType < 0) {
            continue;
        }

        engineTotals[gpuIndex][engineType] += (float)rawValue;
        engineSeen[gpuIndex][engineType] = true;

        if (collectProcessSamples && rawValue > 0.0 && strncmp(instanceName, "pid_", 4) == 0) {
            ProcessGpuSample* sample = &m_processGpuSamples[processSampleCount++];
            sample->pid = (DWORD)strtoul(instanceName + 4, NULL, 10);
            sample->adapterIndex = (BYTE)gpuIndex;
            sample->engineType = (BYTE)engineType;
            sample->value = (float)rawValue;
        }
    }

    if (collectProcessSamples) {
        BuildProcessGpuUsage(processSampleCount);
    }

    for (UINT gpuIndex = 0; gpuIndex < m_gpuAdapterCount; ++gpuIndex) {
        float maxEngineValue = -1.0f;
        const char* maxEngineLabel = "N/A";

        for (UINT engineIndex = 0; engineIndex < GPU_ENGINE_COUNT; ++engineIndex) {
            if (!engineSeen[gpuIndex][engineIndex]) {
                continue;
            }

            if (engineTotals[gpuIndex][engineIndex] > 100.0f) {
                engineTotals[gpuIndex][engineIndex] = 100.0f;
            }

            if (engineTotals[gpuIndex][engineIndex] > maxEngineValue) {
                maxEngineValue = engineTotals[gpuIndex][engineIndex];
                maxEngineLabel = kGpuEngineLabels[engineIndex];
            }
        }

        if (maxEngineValue >= 0.0f) {
            m_gpuSnapshots[gpuIndex].usagePercent = maxEngineValue;
            m_gpuSnapshots[gpuIndex].utilizationAvailable = true;
            snprintf(m_gpuSnapshots[gpuIndex].engineLabel, sizeof(m_gpuSnapshots[gpuIndex].engineLabel), "%s", maxEngineLabel);
        }
    }

    SyncLegacyGpuFields();
}

const char* SystemMonitor::GetGpuEngineLabel(UINT engineType) {
    return (engineType < GPU_ENGINE_COUNT) ? kGpuEngineLabels[engineType] : "N/A";
}

int SystemMonitor::CompareProcessGpuSamples(const void* left, const void* right) {
    const ProcessGpuSample* a = (const ProcessGpuSample*)left;
    const ProcessGpuSample* b = (const ProcessGpuSample*)right;
    if (a->pid != b->pid) {
        return (a->pid < b->pid) ? -1 : 1;
    }
    const int keyA = (a->adapterIndex << 8) | a->engineType;
    const int keyB = (b->adapterIndex << 8) | b->engineType;
    return keyA - keyB;
}

void SystemMonitor::BuildProcessGpuUsage(UINT sampleCount) {
    m_processGpuUsageCount = 0;
    if (sampleCount == 0) {
        return;
    }

    // At most one result per sample, so sizing to the sample count is always enough.
    if (sampleCount > m_processGpuUsageCapacity) {
        ProcessGpuUsage* grown = (ProcessGpuUsage*)realloc(m_processGpuUsage, sampleCount * sizeof(ProcessGpuUsage));
        if (grown == NULL) {
            return;
        }
        m_processGpuUsage = grown;
        m_processGpuUsageCapacity = sampleCount;
    }

    qsort(m_processGpuSamples, sampleCount, sizeof(ProcessGpuSample), CompareProcessGpuSamples);

    // Sum engine instances per (pid, adapter, engine type), then keep each pid's busiest engine.
    UINT i = 0;
    while (i < sampleCount) {
        const DWORD pid = m_processGpuSamples[i].pid;
        ProcessGpuUsage* usage = &m_processGpuUsage[m_processGpuUsageCount++];
        usage->pid = pid;
        usage->percent = 0.0f;
        usage->adapterIndex = m_processGpuSamples[i].adapterIndex;
        usage->engineType = m_processGpuSamples[i].engineType;

        while (i < sampleCount && m_processGpuSamples[i].pid == pid) {
            const BYTE adapterIndex = m_processGpuSamples[i].adapterIndex;
            const BYTE engineType = m_processGpuSamples[i].engineType;
            float engineSum = 0.0f;
            while (i < sampleCount && m_processGpuSamples[i].pid == pid &&
                   m_processGpuSamples[i].adapterIndex == adapterIndex && m_processGpuSamples[i].engineType == engineType) {
                engineSum += m_processGpuSamples[i].value;
                ++i;
            }
            if (engineSum > 100.0f) {
                engineSum = 100.0f;
            }
            if (engineSum > usage->percent) {
                usage->percent = engineSum;
                usage->adapterIndex = adapterIndex;
                usage->engineType = engineType;
            }
        }
    }
}

void SystemMonitor::ClearDiskCounterHandles() {
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        if (m_driveCounters[i].idleCounter != NULL) {
            PdhRemoveCounter(m_driveCounters[i].idleCounter);
            m_driveCounters[i].idleCounter = NULL;
        }
        if (m_driveCounters[i].readCounter != NULL) {
            PdhRemoveCounter(m_driveCounters[i].readCounter);
            m_driveCounters[i].readCounter = NULL;
        }
        if (m_driveCounters[i].writeCounter != NULL) {
            PdhRemoveCounter(m_driveCounters[i].writeCounter);
            m_driveCounters[i].writeCounter = NULL;
        }

        m_driveCounters[i].selected = false;
        m_driveCounters[i].physicalDiskIndex = -1;
        m_driveCounters[i].fallbackTotal = false;
        m_driveCounters[i].pdhInstance[0] = L'\0';
        m_driveCounters[i].activePercent = 0.0f;
        m_driveCounters[i].readKBps = 0.0f;
        m_driveCounters[i].writeKBps = 0.0f;
        m_driveCounters[i].usedGB = 0.0f;
        m_driveCounters[i].totalGB = 0.0f;
        m_driveCounters[i].freeGB = 0.0f;
        m_driveCounters[i].capacityAvailable = false;
        m_driveCounters[i].temperatureSupported = false;
        m_driveCounters[i].temperatureAvailable = false;
        m_driveCounters[i].temperatureC = 0.0f;
        m_driveCounters[i].temperatureWarningC = 0.0f;
        m_driveCounters[i].temperatureCriticalC = 0.0f;
    }
}

bool SystemMonitor::ResolveDriveToPhysicalDisk(wchar_t driveLetter, int* outPhysicalDiskIndex) const {
    if (outPhysicalDiskIndex == NULL) {
        return false;
    }

    *outPhysicalDiskIndex = -1;

    // Ask the volume which disk it lives on; access 0 is enough and needs no elevation. Volumes
    // spanning several disks fail here and fall through to the device-path parse below.
    wchar_t volumePath[8] = { L'\\', L'\\', L'.', L'\\', driveLetter, L':', L'\0' };
    HANDLE volumeHandle = CreateFileW(volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (volumeHandle != INVALID_HANDLE_VALUE) {
        STORAGE_DEVICE_NUMBER deviceNumber = {};
        DWORD bytesReturned = 0;
        const BOOL queried = DeviceIoControl(volumeHandle, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
            &deviceNumber, sizeof(deviceNumber), &bytesReturned, NULL);
        CloseHandle(volumeHandle);
        if (queried && deviceNumber.DeviceType == FILE_DEVICE_DISK) {
            *outPhysicalDiskIndex = (int)deviceNumber.DeviceNumber;
            return true;
        }
    }

    wchar_t driveName[3] = { driveLetter, L':', L'\0' };
    wchar_t targetPath[512] = {};
    if (QueryDosDeviceW(driveName, targetPath, (DWORD)(sizeof(targetPath) / sizeof(targetPath[0]))) == 0) {
        return false;
    }

    const wchar_t* harddisk = wcsstr(targetPath, L"Harddisk");
    if (harddisk == NULL) {
        return false;
    }

    harddisk += 8; // skip "Harddisk"
    if (_wcsnicmp(harddisk, L"Volume", 6) == 0) {
        return false;
    }

    if (!iswdigit(*harddisk)) {
        return false;
    }

    int parsedDisk = 0;
    while (*harddisk != L'\0' && iswdigit(*harddisk)) {
        parsedDisk = (parsedDisk * 10) + (*harddisk - L'0');
        ++harddisk;
    }

    if (parsedDisk < 0) {
        return false;
    }

    *outPhysicalDiskIndex = parsedDisk;
    return true;
}

bool SystemMonitor::ResolveDriveToPdhInstance(wchar_t driveLetter, int preferredDiskIndex, wchar_t* outInstance, int outInstanceLength) {
    if (outInstance == NULL || outInstanceLength <= 0) {
        return false;
    }

    outInstance[0] = L'\0';

    bool matched = false;
    char selectedInstance[64] = {};

    if (m_pdhDiskInstanceCounter != NULL) {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS diskStatus = PdhGetFormattedCounterArrayA(m_pdhDiskInstanceCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if ((diskStatus == (PDH_STATUS)PDH_MORE_DATA || diskStatus == (PDH_STATUS)ERROR_SUCCESS) && bufferSize > 0 && itemCount > 0) {
            if (EnsurePdhBuffer(m_pdhDiskBuffer, m_pdhDiskBufferSize, bufferSize)) {
                PDH_FMT_COUNTERVALUE_ITEM_A* itemArray = (PDH_FMT_COUNTERVALUE_ITEM_A*)m_pdhDiskBuffer;
                if (PdhGetFormattedCounterArrayA(m_pdhDiskInstanceCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, itemArray) == ERROR_SUCCESS) {
                    char driveToken[4] = { (char)driveLetter, ':', '\0' };
                    for (DWORD i = 0; i < itemCount; ++i) {
                        const char* instanceName = itemArray[i].szName;
                        if (instanceName == NULL || ContainsIgnoreCase(instanceName, "_Total")) {
                            continue;
                        }

                        const int leadingDiskIndex = ParseLeadingDiskIndex(instanceName);
                        if (preferredDiskIndex >= 0 && leadingDiskIndex == preferredDiskIndex) {
                            snprintf(selectedInstance, sizeof(selectedInstance), "%s", instanceName);
                            matched = true;
                            break;
                        }

                        if (!matched && ContainsIgnoreCase(instanceName, driveToken)) {
                            snprintf(selectedInstance, sizeof(selectedInstance), "%s", instanceName);
                            matched = true;
                        }
                    }
                }
            }
        }
    }

    if (matched) {
        if (MultiByteToWideChar(CP_ACP, 0, selectedInstance, -1, outInstance, outInstanceLength) <= 0) {
            outInstance[0] = L'\0';
            return false;
        }
        return true;
    }

    if (preferredDiskIndex >= 0) {
        swprintf_s(outInstance, outInstanceLength, L"%d", preferredDiskIndex);
        return true;
    }

    return false;
}

void SystemMonitor::RebuildDiskCounters() {
    ClearDiskCounterHandles();

    if (m_pdhQuery == NULL) {
        return;
    }

    const DWORD driveMask = GetLogicalDrives();
    m_cachedLogicalDrivesMask = driveMask;

    for (UINT driveIndex = 0; driveIndex < kMaxDriveLetters; ++driveIndex) {
        const DWORD driveBit = (1u << driveIndex);
        if ((m_diskSelectionMask & driveBit) == 0) {
            continue;
        }

        if ((driveMask & driveBit) == 0) {
            continue;
        }

        wchar_t rootPath[4] = { (wchar_t)(L'A' + driveIndex), L':', L'\\', L'\0' };
        const UINT driveType = GetDriveTypeW(rootPath);
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) {
            continue;
        }

        DriveCounterSlot* slot = &m_driveCounters[driveIndex];
        slot->selected = true;
        slot->driveLetter = (char)('A' + driveIndex);
        slot->physicalDiskIndex = -1;
        slot->fallbackTotal = false;
        slot->activePercent = 0.0f;
        slot->readKBps = 0.0f;
        slot->writeKBps = 0.0f;
        slot->totalGB = 0.0f;
        slot->usedGB = 0.0f;
        slot->freeGB = 0.0f;
        slot->capacityAvailable = false;

        ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFreeBytes = {};
        if (GetDiskFreeSpaceExW(rootPath, &freeBytesAvail, &totalBytes, &totalFreeBytes) && totalBytes.QuadPart > 0) {
            slot->totalGB = (float)((double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0));
            slot->freeGB = (float)((double)totalFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0));
            slot->usedGB = slot->totalGB - slot->freeGB;
            slot->capacityAvailable = true;
        }

        const wchar_t driveLetter = (wchar_t)(L'A' + driveIndex);
        ResolveDriveToPhysicalDisk(driveLetter, &slot->physicalDiskIndex);

        // Spinning disks are skipped: a temperature query can wake a drive that has spun down.
        slot->temperatureSupported = slot->physicalDiskIndex >= 0 && !IsSeekPenaltyDisk(slot->physicalDiskIndex);
        slot->temperatureAvailable = false;

        wchar_t instanceName[64] = {};
        if (!ResolveDriveToPdhInstance(driveLetter, slot->physicalDiskIndex, instanceName, (int)(sizeof(instanceName) / sizeof(instanceName[0])))) {
            wcscpy_s(instanceName, L"_Total");
            slot->fallbackTotal = true;
        }

        wcscpy_s(slot->pdhInstance, instanceName);

        auto removePartialCounters = [&]() {
            if (slot->idleCounter != NULL) {
                PdhRemoveCounter(slot->idleCounter);
                slot->idleCounter = NULL;
            }
            if (slot->readCounter != NULL) {
                PdhRemoveCounter(slot->readCounter);
                slot->readCounter = NULL;
            }
            if (slot->writeCounter != NULL) {
                PdhRemoveCounter(slot->writeCounter);
                slot->writeCounter = NULL;
            }
        };

        auto tryAddLogicalCounters = [&](wchar_t dl) -> bool {
            wchar_t idlePath[128] = {};
            wchar_t readPath[128] = {};
            wchar_t writePath[128] = {};
            swprintf_s(idlePath, L"\\LogicalDisk(%c:)\\%% Idle Time", dl);
            swprintf_s(readPath, L"\\LogicalDisk(%c:)\\Disk Read Bytes/sec", dl);
            swprintf_s(writePath, L"\\LogicalDisk(%c:)\\Disk Write Bytes/sec", dl);

            if (PdhAddEnglishCounterW(m_pdhQuery, idlePath, 0, &slot->idleCounter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(m_pdhQuery, readPath, 0, &slot->readCounter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(m_pdhQuery, writePath, 0, &slot->writeCounter) == ERROR_SUCCESS) {
                swprintf_s(slot->pdhInstance, L"%c:", dl);
                slot->fallbackTotal = false;
                return true;
            }
            removePartialCounters();
            return false;
        };

        auto tryAddPhysicalCounters = [&](const wchar_t* pdhInstanceName) -> bool {
            wchar_t idlePath[128] = {};
            wchar_t readPath[128] = {};
            wchar_t writePath[128] = {};
            swprintf_s(idlePath, L"\\PhysicalDisk(%ls)\\%% Idle Time", pdhInstanceName);
            swprintf_s(readPath, L"\\PhysicalDisk(%ls)\\Disk Read Bytes/sec", pdhInstanceName);
            swprintf_s(writePath, L"\\PhysicalDisk(%ls)\\Disk Write Bytes/sec", pdhInstanceName);

            if (PdhAddEnglishCounterW(m_pdhQuery, idlePath, 0, &slot->idleCounter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(m_pdhQuery, readPath, 0, &slot->readCounter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(m_pdhQuery, writePath, 0, &slot->writeCounter) == ERROR_SUCCESS) {
                return true;
            }
            removePartialCounters();
            return false;
        };

        if (tryAddLogicalCounters(driveLetter)) {
            // Successfully bound to dedicated LogicalDisk counter for this drive letter
            slot->fallbackTotal = false;
        } else if (slot->pdhInstance[0] != L'\0' && _wcsicmp(slot->pdhInstance, L"_Total") != 0 && tryAddPhysicalCounters(slot->pdhInstance)) {
            // Successfully bound to specific PhysicalDisk counter
            slot->fallbackTotal = false;
        } else if (tryAddPhysicalCounters(L"_Total")) {
            // Generic fallback
            wcscpy_s(slot->pdhInstance, L"_Total");
            slot->fallbackTotal = true;
        } else {
            slot->selected = false;
            continue;
        }
    }

    // New topology: read temperatures on the next poll instead of waiting out the interval.
    // Capacity was just read above for every selected drive, so its timer restarts now.
    m_lastDiskTemperaturePoll = 0;
    m_lastDiskCapacityPoll = GetTickCount64();

    if (m_pdhQuery != NULL) {
        PdhCollectQueryData(m_pdhQuery);
    }
}

int SystemMonitor::FindNetworkAdapterSlotByIfIndex(ULONG ifIndex) const {
    if (ifIndex == 0) {
        return -1;
    }

    for (UINT i = 0; i < m_networkAdapterCount; ++i) {
        if (m_networkAdapters[i].valid && m_networkAdapters[i].ifIndex == ifIndex) {
            return (int)i;
        }
    }

    return -1;
}

int SystemMonitor::ResolveAutoPrimaryNetworkSlot() const {
    MIB_IPFORWARDROW route;
    ZeroMemory(&route, sizeof(route));
    if (GetBestRoute(0, 0x08080808u, &route) == NO_ERROR && route.dwForwardIfIndex != 0) {
        int routeMatch = FindNetworkAdapterSlotByIfIndex(route.dwForwardIfIndex);
        if (routeMatch >= 0 && m_networkAdapters[routeMatch].connected) {
            return routeMatch;
        }
    }

    int bestSlot = -1;
    int bestScore = -100000;

    for (UINT i = 0; i < m_networkAdapterCount; ++i) {
        const NetworkAdapterSlot* adapter = &m_networkAdapters[i];
        if (!adapter->valid) {
            continue;
        }

        int score = 0;
        if (adapter->connected) {
            score += 200;
        }
        if (adapter->hasGateway) {
            score += 100;
        }
        if (adapter->isWifi) {
            score += 25;
        } else if (adapter->ifIndex != 0) {
            score += 10;
        }

        if (score > bestScore) {
            bestScore = score;
            bestSlot = (int)i;
        }
    }

    return bestSlot;
}

int SystemMonitor::ResolveAutoSecondaryNetworkSlot(int primarySlot) const {
    int bestSlot = -1;
    int bestScore = -100000;

    for (UINT i = 0; i < m_networkAdapterCount; ++i) {
        if ((int)i == primarySlot) {
            continue;
        }

        const NetworkAdapterSlot* adapter = &m_networkAdapters[i];
        if (!adapter->valid || !adapter->connected) {
            continue;
        }

        int score = 0;
        if (adapter->hasGateway) {
            score += 100;
        }
        if (adapter->isWifi) {
            score += 20;
        } else {
            score += 10;
        }

        if (score > bestScore) {
            bestScore = score;
            bestSlot = (int)i;
        }
    }

    return bestSlot;
}

void SystemMonitor::ResetNetworkDeltaState(NetworkDeltaState* state) {
    if (state == NULL) {
        return;
    }

    state->valid = false;
    state->ifIndex = 0;
    state->lastInOctets = 0;
    state->lastOutOctets = 0;
    state->lastSampleTimeMs = 0;
}

bool SystemMonitor::TryQueryConnectedNetworkName(const GUID* adapterGuid, char* nameBuffer, int nameBufferSize) const {
    if (nameBuffer == NULL || nameBufferSize <= 0) {
        return false;
    }

    nameBuffer[0] = '\0';

    INetworkListManager* pNLM = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_NetworkListManager, NULL, CLSCTX_ALL,
        IID_INetworkListManager, (void**)&pNLM);
    if (FAILED(hr) || pNLM == NULL) {
        return false;
    }

    bool success = false;

    // Method 1 (adapter GUID unknown only): first connected network. With a GUID we must not use
    // this, because it can return another adapter's network (e.g. Ethernet while Wi-Fi is also up).
    IEnumNetworks* pEnumNetworks = NULL;
    hr = adapterGuid == NULL ? pNLM->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &pEnumNetworks) : E_NOTIMPL;
    if (SUCCEEDED(hr) && pEnumNetworks != NULL) {
        INetwork* pNetwork = NULL;
        ULONG fetched = 0;
        while (pEnumNetworks->Next(1, &pNetwork, &fetched) == S_OK && fetched > 0 && pNetwork != NULL) {
            BSTR bstrName = NULL;
            hr = pNetwork->GetName(&bstrName);
            if (SUCCEEDED(hr) && bstrName != NULL) {
                char temp[128] = {};
                CopyWideToUtf8(bstrName, temp, sizeof(temp));
                SysFreeString(bstrName);

                if (temp[0] != '\0' &&
                    _stricmp(temp, "Unidentified network") != 0 &&
                    _stricmp(temp, "Identifying...") != 0) {
                    snprintf(nameBuffer, nameBufferSize, "%s", temp);
                    success = true;
                }
            }
            pNetwork->Release();
            if (success) {
                break;
            }
        }
        pEnumNetworks->Release();
    }

    // Method 2: per-connection lookup. NLM adapter ids match IP_ADAPTER_INFO::AdapterName GUIDs.
    if (!success) {
        IEnumNetworkConnections* pEnumConnections = NULL;
        hr = pNLM->GetNetworkConnections(&pEnumConnections);
        if (SUCCEEDED(hr) && pEnumConnections != NULL) {
            INetworkConnection* pConnection = NULL;
            ULONG fetched = 0;
            while (pEnumConnections->Next(1, &pConnection, &fetched) == S_OK && fetched > 0 && pConnection != NULL) {
                if (adapterGuid != NULL) {
                    GUID connectionAdapterGuid = {};
                    if (FAILED(pConnection->GetAdapterId(&connectionAdapterGuid)) ||
                        !IsEqualGUID(connectionAdapterGuid, *adapterGuid)) {
                        pConnection->Release();
                        continue;
                    }
                }

                INetwork* pNetwork = NULL;
                hr = pConnection->GetNetwork(&pNetwork);
                if (SUCCEEDED(hr) && pNetwork != NULL) {
                    BSTR bstrName = NULL;
                    hr = pNetwork->GetName(&bstrName);
                    if (SUCCEEDED(hr) && bstrName != NULL) {
                        char temp[128] = {};
                        CopyWideToUtf8(bstrName, temp, sizeof(temp));
                        SysFreeString(bstrName);

                        if (temp[0] != '\0' &&
                            _stricmp(temp, "Unidentified network") != 0 &&
                            _stricmp(temp, "Identifying...") != 0) {
                            snprintf(nameBuffer, nameBufferSize, "%s", temp);
                            success = true;
                        }
                    }
                    pNetwork->Release();
                }
                pConnection->Release();
                if (success) {
                    break;
                }
            }
            pEnumConnections->Release();
        }
    }

    pNLM->Release();
    return success;
}

void SystemMonitor::BuildNetworkDisplayName(const NetworkAdapterSlot* adapter, char* output, int outputSize) const {
    if (output == NULL || outputSize <= 0) {
        return;
    }

    output[0] = '\0';

    if (adapter == NULL || !adapter->valid || !adapter->connected) {
        snprintf(output, outputSize, "Disconnected");
        return;
    }

    if (adapter->ssid[0] != '\0') {
        snprintf(output, outputSize, "%s", adapter->ssid);
        return;
    }

    if (adapter->isWifi) {
        snprintf(output, outputSize, "Wi-Fi");
    } else {
        snprintf(output, outputSize, "Wired");
    }
}

void SystemMonitor::ClearNetworkSnapshots() {
    ZeroMemory(&m_primaryNetworkMetrics, sizeof(m_primaryNetworkMetrics));
    ZeroMemory(&m_secondaryNetworkMetrics, sizeof(m_secondaryNetworkMetrics));

    snprintf(m_primaryNetworkMetrics.displayName, sizeof(m_primaryNetworkMetrics.displayName), "Disconnected");
    m_primaryNetworkMetrics.connected = false;

    snprintf(m_secondaryNetworkMetrics.displayName, sizeof(m_secondaryNetworkMetrics.displayName), "Unavailable");
    m_secondaryNetworkMetrics.connected = false;

    m_hasSecondaryNetworkMetrics = false;

    snprintf(m_networkAdapterName, sizeof(m_networkAdapterName), "Disconnected");
    m_networkSSID[0] = '\0';
    snprintf(m_networkDisplayName, sizeof(m_networkDisplayName), "Disconnected");
}

void SystemMonitor::RefreshNetworkIdentity() {
    m_networkSsidRefreshRequested.store(false, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    const ULONG previousPrimaryIfIndex = m_primaryNetworkMetrics.ifIndex;
    const ULONG previousSecondaryIfIndex = m_secondaryNetworkMetrics.ifIndex;

    m_networkAdapterCount = 0;
    m_primaryNetworkSlot = -1;
    m_secondaryNetworkSlot = -1;

    for (UINT i = 0; i < kMaxNetworkAdapters; ++i) {
        m_networkAdapters[i].valid = false;
        m_networkAdapters[i].ifIndex = 0;
        m_networkAdapters[i].interfaceLuidValue = 0;
        m_networkAdapters[i].connected = false;
        m_networkAdapters[i].hasGateway = false;
        m_networkAdapters[i].isWifi = false;
        m_networkAdapters[i].hasAdapterGuid = false;
        ZeroMemory(&m_networkAdapters[i].adapterGuid, sizeof(GUID));
        m_networkAdapters[i].adapterName[0] = '\0';
        m_networkAdapters[i].ssid[0] = '\0';
        m_networkAdapters[i].displayName[0] = '\0';
    }

    ULONG bufferSize = 0;
    DWORD adapterStatus = GetAdaptersInfo(NULL, &bufferSize);

    if (adapterStatus == ERROR_BUFFER_OVERFLOW && bufferSize > 0) {
        IP_ADAPTER_INFO* adapters = (IP_ADAPTER_INFO*)malloc(bufferSize);
        if (adapters != NULL) {
            adapterStatus = GetAdaptersInfo(adapters, &bufferSize);
            if (adapterStatus == NO_ERROR) {
                for (IP_ADAPTER_INFO* adapter = adapters; adapter != NULL && m_networkAdapterCount < kMaxNetworkAdapters; adapter = adapter->Next) {
                    if (adapter->Type == 24) { // Loopback
                        continue;
                    }

                    NetworkAdapterSlot* slot = &m_networkAdapters[m_networkAdapterCount];
                    slot->valid = true;
                    slot->ifIndex = adapter->Index;
                    slot->interfaceLuidValue = 0;
                    slot->hasGateway = adapter->GatewayList.IpAddress.String[0] != '\0' && strcmp(adapter->GatewayList.IpAddress.String, "0.0.0.0") != 0;
                    slot->isWifi = (adapter->Type == 71);

                    // AdapterName is the interface GUID string, e.g. "{2BC46643-...}".
                    wchar_t adapterGuidText[64] = {};
                    slot->hasAdapterGuid =
                        MultiByteToWideChar(CP_ACP, 0, adapter->AdapterName, -1, adapterGuidText, (int)(sizeof(adapterGuidText) / sizeof(adapterGuidText[0]))) > 0 &&
                        SUCCEEDED(CLSIDFromString(adapterGuidText, &slot->adapterGuid));

                    MIB_IFROW ifRow;
                    ZeroMemory(&ifRow, sizeof(ifRow));
                    ifRow.dwIndex = adapter->Index;
                    if (GetIfEntry(&ifRow) == NO_ERROR) {
                        // MIB_IFROW status values: 4=connected, 5=operational
                        slot->connected = ifRow.dwOperStatus >= 4;
                    } else {
                        slot->connected = slot->hasGateway;
                    }

                    if (adapter->Description[0] != '\0') {
                        snprintf(slot->adapterName, sizeof(slot->adapterName), "%s", adapter->Description);
                    } else if (adapter->AdapterName[0] != '\0') {
                        snprintf(slot->adapterName, sizeof(slot->adapterName), "%s", adapter->AdapterName);
                    } else {
                        snprintf(slot->adapterName, sizeof(slot->adapterName), "Adapter %lu", adapter->Index);
                    }

                    slot->ssid[0] = '\0';
                    slot->lastSsidQueryTick = 0;
                    if (slot->isWifi && slot->connected) {
                        if (TryQueryConnectedNetworkName(slot->hasAdapterGuid ? &slot->adapterGuid : NULL, slot->ssid, (int)sizeof(slot->ssid))) {
                            slot->lastSsidQueryTick = now;
                        }
                    }
                    BuildNetworkDisplayName(slot, slot->displayName, (int)sizeof(slot->displayName));

                    ++m_networkAdapterCount;
                }
            }

            free(adapters);
        }
    }

    if (m_networkAdapterCount == 0) {
        ClearNetworkSnapshots();
        ResetNetworkDeltaState(&m_primaryDeltaState);
        ResetNetworkDeltaState(&m_secondaryDeltaState);
        return;
    }

    int primarySlot = -1;
    if (m_networkPrimaryMode == NETWORK_PRIMARY_MANUAL && m_networkPrimaryIfIndex != 0) {
        primarySlot = FindNetworkAdapterSlotByIfIndex((ULONG)m_networkPrimaryIfIndex);
    }

    if (primarySlot < 0) {
        primarySlot = ResolveAutoPrimaryNetworkSlot();
    }

    if (primarySlot < 0) {
        primarySlot = 0;
    }

    m_primaryNetworkSlot = primarySlot;

    NetworkAdapterSlot* primaryAdapter = &m_networkAdapters[m_primaryNetworkSlot];
    BuildNetworkDisplayName(primaryAdapter, primaryAdapter->displayName, (int)sizeof(primaryAdapter->displayName));

    if (primaryAdapter->adapterName[0] != '\0') {
        snprintf(m_networkAdapterName, sizeof(m_networkAdapterName), "%s", primaryAdapter->adapterName);
    } else {
        snprintf(m_networkAdapterName, sizeof(m_networkAdapterName), "Unknown Adapter");
    }

    snprintf(m_networkSSID, sizeof(m_networkSSID), "%s", primaryAdapter->ssid);
    snprintf(m_networkDisplayName, sizeof(m_networkDisplayName), "%s", primaryAdapter->displayName);

    m_primaryNetworkMetrics.ifIndex = primaryAdapter->ifIndex;
    snprintf(m_primaryNetworkMetrics.displayName, sizeof(m_primaryNetworkMetrics.displayName), "%s", primaryAdapter->displayName);
    m_primaryNetworkMetrics.connected = primaryAdapter->connected;

    int secondarySlot = -1;
    if (m_networkSecondaryEnabled) {
        if (m_networkSecondaryIfIndex != 0) {
            int manualSecondarySlot = FindNetworkAdapterSlotByIfIndex((ULONG)m_networkSecondaryIfIndex);
            if (manualSecondarySlot >= 0 && manualSecondarySlot != m_primaryNetworkSlot) {
                secondarySlot = manualSecondarySlot;
            }
        }

        if (secondarySlot < 0) {
            secondarySlot = ResolveAutoSecondaryNetworkSlot(m_primaryNetworkSlot);
        }
    }

    if (secondarySlot >= 0 && secondarySlot < (int)m_networkAdapterCount) {
        m_secondaryNetworkSlot = secondarySlot;
        NetworkAdapterSlot* secondaryAdapter = &m_networkAdapters[m_secondaryNetworkSlot];
        BuildNetworkDisplayName(secondaryAdapter, secondaryAdapter->displayName, (int)sizeof(secondaryAdapter->displayName));

        m_secondaryNetworkMetrics.ifIndex = secondaryAdapter->ifIndex;
        snprintf(m_secondaryNetworkMetrics.displayName, sizeof(m_secondaryNetworkMetrics.displayName), "%s", secondaryAdapter->displayName);
        m_secondaryNetworkMetrics.connected = secondaryAdapter->connected;
        m_hasSecondaryNetworkMetrics = true;
    } else {
        m_secondaryNetworkSlot = -1;
        m_secondaryNetworkMetrics.ifIndex = 0;
        m_secondaryNetworkMetrics.connected = false;
        m_secondaryNetworkMetrics.rxMbps = 0.0f;
        m_secondaryNetworkMetrics.txMbps = 0.0f;
        m_secondaryNetworkMetrics.totalMbps = 0.0f;
        snprintf(m_secondaryNetworkMetrics.displayName, sizeof(m_secondaryNetworkMetrics.displayName), "Unavailable");
        m_hasSecondaryNetworkMetrics = false;
    }

    if (previousPrimaryIfIndex != m_primaryNetworkMetrics.ifIndex) {
        ResetNetworkDeltaState(&m_primaryDeltaState);
    }

    if (previousSecondaryIfIndex != m_secondaryNetworkMetrics.ifIndex) {
        ResetNetworkDeltaState(&m_secondaryDeltaState);
    }

    if (!m_hasSecondaryNetworkMetrics) {
        ResetNetworkDeltaState(&m_secondaryDeltaState);
    }
}

void SystemMonitor::PollNetworkThroughput() {
    static const ULONGLONG kOctetCounterWrap = 0x100000000ULL;

    m_primaryNetworkMetrics.rxMbps = 0.0f;
    m_primaryNetworkMetrics.txMbps = 0.0f;
    m_primaryNetworkMetrics.totalMbps = 0.0f;

    m_secondaryNetworkMetrics.rxMbps = 0.0f;
    m_secondaryNetworkMetrics.txMbps = 0.0f;
    m_secondaryNetworkMetrics.totalMbps = 0.0f;

    if (m_primaryNetworkSlot < 0 || m_primaryNetworkSlot >= (int)m_networkAdapterCount) {
        RequestNetworkIdentityRefresh();
        return;
    }

    const ULONGLONG now = GetTickCount64();

    auto updateSnapshot = [&](NetworkMetricsSnapshot* snapshot, NetworkDeltaState* deltaState) {
        if (snapshot == NULL || deltaState == NULL || snapshot->ifIndex == 0) {
            return;
        }

        MIB_IFROW ifRow;
        ZeroMemory(&ifRow, sizeof(ifRow));
        ifRow.dwIndex = snapshot->ifIndex;
        if (GetIfEntry(&ifRow) != NO_ERROR) {
            ResetNetworkDeltaState(deltaState);
            snapshot->connected = false;
            snapshot->rxMbps = 0.0f;
            snapshot->txMbps = 0.0f;
            snapshot->totalMbps = 0.0f;
            RequestNetworkIdentityRefresh();
            return;
        }

        const bool connectedNow = ifRow.dwOperStatus >= 4;
        if (!connectedNow) {
            if (snapshot->connected) {
                RequestNetworkIdentityRefresh();
            }

            snapshot->connected = false;
            ResetNetworkDeltaState(deltaState);
            snapshot->rxMbps = 0.0f;
            snapshot->txMbps = 0.0f;
            snapshot->totalMbps = 0.0f;
            return;
        }

        if (!snapshot->connected) {
            RequestNetworkIdentityRefresh();
        }
        snapshot->connected = true;

        if (!deltaState->valid || deltaState->ifIndex != snapshot->ifIndex || now <= deltaState->lastSampleTimeMs) {
            deltaState->valid = true;
            deltaState->ifIndex = snapshot->ifIndex;
            deltaState->lastInOctets = ifRow.dwInOctets;
            deltaState->lastOutOctets = ifRow.dwOutOctets;
            deltaState->lastSampleTimeMs = now;
            snapshot->rxMbps = 0.0f;
            snapshot->txMbps = 0.0f;
            snapshot->totalMbps = 0.0f;
            return;
        }

        const ULONGLONG elapsedMs = now - deltaState->lastSampleTimeMs;
        if (elapsedMs == 0) {
            snapshot->rxMbps = 0.0f;
            snapshot->txMbps = 0.0f;
            snapshot->totalMbps = 0.0f;
            return;
        }

        const ULONGLONG currentInOctets = (ULONGLONG)ifRow.dwInOctets;
        const ULONGLONG currentOutOctets = (ULONGLONG)ifRow.dwOutOctets;

        const ULONGLONG deltaInOctets = currentInOctets >= deltaState->lastInOctets
            ? (currentInOctets - deltaState->lastInOctets)
            : (kOctetCounterWrap - deltaState->lastInOctets + currentInOctets);
        const ULONGLONG deltaOutOctets = currentOutOctets >= deltaState->lastOutOctets
            ? (currentOutOctets - deltaState->lastOutOctets)
            : (kOctetCounterWrap - deltaState->lastOutOctets + currentOutOctets);
        const double elapsedSeconds = (double)elapsedMs / 1000.0;

        snapshot->rxMbps = (float)((deltaInOctets * 8.0) / 1000000.0 / elapsedSeconds);
        snapshot->txMbps = (float)((deltaOutOctets * 8.0) / 1000000.0 / elapsedSeconds);
        if (snapshot->rxMbps < 0.0f) {
            snapshot->rxMbps = 0.0f;
        }
        if (snapshot->txMbps < 0.0f) {
            snapshot->txMbps = 0.0f;
        }

        snapshot->totalMbps = snapshot->rxMbps + snapshot->txMbps;

        deltaState->valid = true;
        deltaState->ifIndex = snapshot->ifIndex;
        deltaState->lastInOctets = currentInOctets;
        deltaState->lastOutOctets = currentOutOctets;
        deltaState->lastSampleTimeMs = now;
    };

    updateSnapshot(&m_primaryNetworkMetrics, &m_primaryDeltaState);
    if (m_hasSecondaryNetworkMetrics) {
        updateSnapshot(&m_secondaryNetworkMetrics, &m_secondaryDeltaState);
    }
}

void SystemMonitor::InitializeThermalZoneQuery() {
    if (PdhOpenQueryW(NULL, 0, &m_pdhThermalQuery) != ERROR_SUCCESS) {
        m_pdhThermalQuery = NULL;
        return;
    }

    // "High Precision Temperature" is in tenths of Kelvin; older builds only have whole Kelvin.
    if (PdhAddEnglishCounterW(m_pdhThermalQuery, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &m_pdhThermalCounter) == ERROR_SUCCESS) {
        m_pdhThermalHighPrecision = true;
        return;
    }

    if (PdhAddEnglishCounterW(m_pdhThermalQuery, L"\\Thermal Zone Information(*)\\Temperature", 0, &m_pdhThermalCounter) == ERROR_SUCCESS) {
        m_pdhThermalHighPrecision = false;
        return;
    }

    PdhCloseQuery(m_pdhThermalQuery);
    m_pdhThermalQuery = NULL;
    m_pdhThermalCounter = NULL;
}

void SystemMonitor::PollCpuTemperature() {
    m_cpuTemperatureAvailable = false;
    if (m_pdhThermalQuery == NULL || m_pdhThermalCounter == NULL) {
        return;
    }

    if (PdhCollectQueryData(m_pdhThermalQuery) != ERROR_SUCCESS) {
        return;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(m_pdhThermalCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
    if (!((status == (PDH_STATUS)PDH_MORE_DATA || status == (PDH_STATUS)ERROR_SUCCESS) && bufferSize > 0 && itemCount > 0)) {
        return;
    }

    if (!EnsurePdhBuffer(m_pdhThermalBuffer, m_pdhThermalBufferSize, bufferSize)) {
        return;
    }

    PDH_FMT_COUNTERVALUE_ITEM_W* itemArray = (PDH_FMT_COUNTERVALUE_ITEM_W*)m_pdhThermalBuffer;
    if (PdhGetFormattedCounterArrayW(m_pdhThermalCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, itemArray) != ERROR_SUCCESS) {
        return;
    }

    // Report the hottest ACPI thermal zone. Zones reading 0 K or absurd values are firmware placeholders.
    const double kelvinScale = m_pdhThermalHighPrecision ? 10.0 : 1.0;
    float hottestC = -1.0f;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (itemArray[i].FmtValue.CStatus != ERROR_SUCCESS) {
            continue;
        }

        const float celsius = (float)((itemArray[i].FmtValue.doubleValue / kelvinScale) - kKelvinToCelsiusOffset);
        if (celsius < kMinValidTemperatureC || celsius > kMaxValidCpuTemperatureC) {
            continue;
        }

        if (celsius > hottestC) {
            hottestC = celsius;
        }
    }

    if (hottestC >= kMinValidTemperatureC) {
        m_cpuTemperatureC = hottestC;
        m_cpuTemperatureAvailable = true;
    }
}

void SystemMonitor::PollGpuTemperatures() {
    const D3dkmtApi* kmt = GetD3dkmtApi();

    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        GPUAdapterSlot* slot = &m_gpuAdapters[i];
        GPUMetricsSnapshot* snapshot = &m_gpuSnapshots[i];
        snapshot->temperatureAvailable = false;

        if (kmt == NULL || !slot->kmtTemperatureSupported || slot->kmtAdapterHandle == 0) {
            continue;
        }

        CG_D3DKMT_ADAPTER_PERFDATA perfData = {};
        CG_D3DKMT_QUERYADAPTERINFO query = {};
        query.hAdapter = slot->kmtAdapterHandle;
        query.Type = kKmtQueryAdapterPerfData;
        query.pPrivateDriverData = &perfData;
        query.PrivateDriverDataSize = sizeof(perfData);
        if (kmt->queryAdapterInfo(&query) != 0) {
            // The driver does not implement perf data at all; stop asking.
            slot->kmtTemperatureSupported = false;
            continue;
        }

        // Drivers without a sensor (most integrated GPUs) answer with 0. Keep retrying anyway,
        // because a dedicated GPU can also report 0 while it is powered down.
        const float celsius = (float)perfData.Temperature / 10.0f;
        if (celsius < kMinValidTemperatureC || celsius > kMaxValidGpuTemperatureC) {
            continue;
        }

        snapshot->temperatureC = celsius;
        snapshot->temperatureAvailable = true;
    }

    SyncLegacyGpuFields();
}

void SystemMonitor::PollDiskTemperatures() {
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        DriveCounterSlot* slot = &m_driveCounters[i];
        if (!slot->selected || !slot->temperatureSupported) {
            slot->temperatureAvailable = false;
            continue;
        }

        // Several volumes on one disk share a single reading.
        bool copied = false;
        for (UINT j = 0; j < i; ++j) {
            const DriveCounterSlot* earlier = &m_driveCounters[j];
            if (earlier->selected && earlier->temperatureSupported && earlier->physicalDiskIndex == slot->physicalDiskIndex) {
                slot->temperatureAvailable = earlier->temperatureAvailable;
                slot->temperatureC = earlier->temperatureC;
                slot->temperatureWarningC = earlier->temperatureWarningC;
                slot->temperatureCriticalC = earlier->temperatureCriticalC;
                copied = true;
                break;
            }
        }
        if (copied) {
            continue;
        }

        float temperatureC = 0.0f;
        float warningC = 0.0f;
        float criticalC = 0.0f;
        if (QueryStorageTemperature(slot->physicalDiskIndex, &temperatureC, &warningC, &criticalC)) {
            slot->temperatureAvailable = true;
            slot->temperatureC = temperatureC;
            slot->temperatureWarningC = warningC > 0.0f ? warningC : kDefaultDiskTemperatureWarningC;
            slot->temperatureCriticalC = criticalC > 0.0f ? criticalC : kDefaultDiskTemperatureCriticalC;
            if (slot->temperatureCriticalC < slot->temperatureWarningC) {
                slot->temperatureCriticalC = slot->temperatureWarningC;
            }
        } else {
            // A drive that has never answered (USB bridges, RAID, some SATA controllers) is not
            // queried again until the next topology rebuild. One that answered before gets another try.
            if (!slot->temperatureAvailable) {
                slot->temperatureSupported = false;
            }
            slot->temperatureAvailable = false;
        }
    }
}

void SystemMonitor::CloseGpuKmtHandles() {
    const D3dkmtApi* kmt = GetD3dkmtApi();
    for (UINT i = 0; i < kMaxGpuAdapters; ++i) {
        if (m_gpuAdapters[i].kmtAdapterHandle != 0 && kmt != NULL) {
            CG_D3DKMT_CLOSEADAPTER closeAdapter = {};
            closeAdapter.hAdapter = m_gpuAdapters[i].kmtAdapterHandle;
            kmt->closeAdapter(&closeAdapter);
        }
        m_gpuAdapters[i].kmtAdapterHandle = 0;
        m_gpuAdapters[i].kmtTemperatureSupported = false;
    }
}

void SystemMonitor::ClearGpuTemperatures() {
    for (UINT i = 0; i < m_gpuAdapterCount; ++i) {
        m_gpuSnapshots[i].temperatureAvailable = false;
    }
    m_lastGpuTemperaturePoll = 0;
}

void SystemMonitor::ClearDiskTemperatures() {
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        m_driveCounters[i].temperatureAvailable = false;
    }
    m_lastDiskTemperaturePoll = 0;
}

bool SystemMonitor::IsSeekPenaltyDisk(int physicalDiskIndex) {
    if (physicalDiskIndex < 0) {
        return false;
    }

    wchar_t diskPath[32] = {};
    swprintf_s(diskPath, L"\\\\.\\PhysicalDrive%d", physicalDiskIndex);
    HANDLE diskHandle = CreateFileW(diskPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (diskHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = {};
    DWORD bytesReturned = 0;
    const BOOL queried = DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &descriptor, sizeof(descriptor), &bytesReturned, NULL);
    CloseHandle(diskHandle);

    return queried && bytesReturned >= sizeof(descriptor) && descriptor.IncursSeekPenalty;
}

bool SystemMonitor::QueryStorageTemperature(int physicalDiskIndex, float* outTemperatureC, float* outWarningC, float* outCriticalC) {
    if (physicalDiskIndex < 0 || outTemperatureC == NULL || outWarningC == NULL || outCriticalC == NULL) {
        return false;
    }

    // Opened and closed per read: holding the handle would block safe removal of external drives.
    wchar_t diskPath[32] = {};
    swprintf_s(diskPath, L"\\\\.\\PhysicalDrive%d", physicalDiskIndex);
    HANDLE diskHandle = CreateFileW(diskPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (diskHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceTemperatureProperty;
    query.QueryType = PropertyStandardQuery;

    // Room for the descriptor plus several sensor entries.
    union {
        STORAGE_TEMPERATURE_DATA_DESCRIPTOR descriptor;
        BYTE raw[512];
    } output;
    ZeroMemory(&output, sizeof(output));

    DWORD bytesReturned = 0;
    const BOOL queried = DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &output, sizeof(output), &bytesReturned, NULL);
    CloseHandle(diskHandle);

    const DWORD minimumBytes = (DWORD)(FIELD_OFFSET(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo) + sizeof(STORAGE_TEMPERATURE_INFO));
    if (!queried || bytesReturned < minimumBytes || output.descriptor.InfoCount == 0) {
        return false;
    }

    // Entry 0 is the composite temperature the drive reports for itself.
    const float celsius = (float)output.descriptor.TemperatureInfo[0].Temperature;
    if (celsius < kMinValidTemperatureC || celsius > kMaxValidDiskTemperatureC) {
        return false;
    }

    *outTemperatureC = celsius;
    *outWarningC = (float)output.descriptor.WarningTemperature;
    *outCriticalC = (float)output.descriptor.CriticalTemperature;
    if (*outWarningC < kMinValidTemperatureC || *outWarningC > kMaxValidDiskTemperatureC) {
        *outWarningC = 0.0f;
    }
    if (*outCriticalC < kMinValidTemperatureC || *outCriticalC > kMaxValidDiskTemperatureC) {
        *outCriticalC = 0.0f;
    }
    return true;
}

void SystemMonitor::QueryCpuName(char* outName, int outSize) {
    if (outName == NULL || outSize <= 0) {
        return;
    }
    outName[0] = '\0';

    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char rawName[128] = {};
        DWORD dataSize = sizeof(rawName);
        if (RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)rawName, &dataSize) == ERROR_SUCCESS) {
            const char* src = rawName;
            while (*src == ' ' || *src == '\t') {
                src++;
            }

            int dstIdx = 0;
            bool lastWasSpace = false;
            while (*src != '\0' && dstIdx < outSize - 1) {
                if (*src == ' ' || *src == '\t') {
                    if (!lastWasSpace) {
                        outName[dstIdx++] = ' ';
                        lastWasSpace = true;
                    }
                } else {
                    outName[dstIdx++] = *src;
                    lastWasSpace = false;
                }
                src++;
            }

            while (dstIdx > 0 && outName[dstIdx - 1] == ' ') {
                dstIdx--;
            }
            outName[dstIdx] = '\0';
        }
        RegCloseKey(hKey);
    }

    if (outName[0] == '\0') {
        snprintf(outName, outSize, "CPU");
    }
}

DWORD SystemMonitor::QueryRamSpeedMHz() {
    const DWORD signature = 0x52534D42; // 'RSMB'
    const DWORD size = GetSystemFirmwareTable(signature, 0, NULL, 0);
    if (size == 0) {
        return 0;
    }

    BYTE* buffer = (BYTE*)malloc(size);
    if (buffer == NULL) {
        return 0;
    }

    if (GetSystemFirmwareTable(signature, 0, buffer, size) != size) {
        free(buffer);
        return 0;
    }

    DWORD maxRamSpeed = 0;
    if (size > 8) {
        const BYTE* ptr = buffer + 8;
        const BYTE* end = buffer + size;
        while (ptr + 4 <= end) {
            BYTE type = ptr[0];
            BYTE length = ptr[1];
            if (length < 4 || ptr + length > end) {
                break;
            }

            if (type == 17) { // Type 17: Memory Device (SMBIOS)
                WORD speed = 0;
                WORD configuredSpeed = 0;
                if (length >= 23) {
                    speed = *(const WORD*)(ptr + 0x15);
                }
                if (length >= 34) {
                    configuredSpeed = *(const WORD*)(ptr + 0x20);
                }
                WORD effectiveSpeed = (configuredSpeed > 0 && configuredSpeed < 65535) ? configuredSpeed : speed;
                if (effectiveSpeed > maxRamSpeed && effectiveSpeed < 65535) {
                    maxRamSpeed = effectiveSpeed;
                }
            }

            const BYTE* strPtr = ptr + length;
            while (strPtr < end - 1 && !(strPtr[0] == 0 && strPtr[1] == 0)) {
                strPtr++;
            }
            ptr = strPtr + 2;
        }
    }

    free(buffer);
    return maxRamSpeed;
}

