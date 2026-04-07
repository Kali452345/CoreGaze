#include "SystemMonitor.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <windot11.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const float kBytesToGB = 1.0f / (1024.0f * 1024.0f * 1024.0f);
static const DWORD kAllDriveSelectionMask = 0x03FFFFFFu;
static const ULONGLONG kNetworkIdentityRefreshMs = 10000;

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
    if (networkDisplayMode > NETWORK_DISPLAY_RX_TX_SECONDARY) {
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
      m_pollingIntervalMs(1000),
      m_enabledMetricsMask(SYSTEM_METRIC_ALL),
      m_diskSelectionMask(kAllDriveSelectionMask),
      m_gpuDisplayMode(GPU_DISPLAY_TARGETED),
      m_selectedGpuAdapterIndex(0),
      m_gpuActiveAdapterIndex(0),
      m_networkPrimaryMode(NETWORK_PRIMARY_AUTO),
      m_networkPrimaryIfIndex(0),
      m_networkSecondaryEnabled(FALSE),
      m_networkSecondaryIfIndex(0),
      m_networkDisplayMode(NETWORK_DISPLAY_RX_TX),
      m_cpuUsage(0.0f),
      m_cpuGHz(0.0f),
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
      m_pdhQuery(NULL),
      m_pdhCpuCounter(NULL),
      m_pdhCpuPerfCounter(NULL),
      m_pdhDiskInstanceCounter(NULL),
      m_pdhGpuEngineCounter(NULL),
      m_pdhDiskBuffer(NULL),
      m_pdhDiskBufferSize(0),
      m_pdhGpuBuffer(NULL),
      m_pdhGpuBufferSize(0)
{
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
        m_driveCounters[i].activeCounter = NULL;
        m_driveCounters[i].readCounter = NULL;
        m_driveCounters[i].writeCounter = NULL;
        m_driveCounters[i].activePercent = 0.0f;
        m_driveCounters[i].readMbps = 0.0f;
        m_driveCounters[i].writeMbps = 0.0f;
    }

    for (UINT i = 0; i < kMaxNetworkAdapters; ++i) {
        m_networkAdapters[i].valid = false;
        m_networkAdapters[i].ifIndex = 0;
        m_networkAdapters[i].interfaceLuidValue = 0;
        m_networkAdapters[i].connected = false;
        m_networkAdapters[i].hasGateway = false;
        m_networkAdapters[i].isWifi = false;
        m_networkAdapters[i].adapterName[0] = '\0';
        m_networkAdapters[i].ssid[0] = '\0';
        m_networkAdapters[i].displayName[0] = '\0';
    }

    ResetNetworkDeltaState(&m_primaryDeltaState);
    ResetNetworkDeltaState(&m_secondaryDeltaState);
    ClearNetworkSnapshots();

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

    RefreshNetworkIdentity();
    m_lastNetworkIdentityRefresh = GetTickCount64();

    m_lastUpdateTime = GetTickCount64();
    PollMetrics();
}

SystemMonitor::~SystemMonitor() {
    ClearDiskCounterHandles();

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

void SystemMonitor::RefreshNetworkIdentityNow() {
    RefreshNetworkIdentity();
    m_lastNetworkIdentityRefresh = GetTickCount64();
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
            outSnapshot->readMbps = m_driveCounters[i].readMbps;
            outSnapshot->writeMbps = m_driveCounters[i].writeMbps;
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
    const bool pollDisk = ((m_enabledMetricsMask & SYSTEM_METRIC_DISK) != 0) && (m_diskSelectionMask != 0);
    const bool pollNetwork = (m_enabledMetricsMask & SYSTEM_METRIC_NETWORK) != 0;

    if (pollNetwork) {
        ULONGLONG now = GetTickCount64();
        if (m_lastNetworkIdentityRefresh == 0 || (now - m_lastNetworkIdentityRefresh) >= kNetworkIdentityRefreshMs) {
            RefreshNetworkIdentity();
            m_lastNetworkIdentityRefresh = now;
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

    const bool needsPdh = pollCpu || pollDisk || pollGpu;
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
            for (UINT i = 0; i < kMaxDriveLetters; ++i) {
                if (!m_driveCounters[i].selected) {
                    continue;
                }

                m_driveCounters[i].activePercent = 0.0f;
                m_driveCounters[i].readMbps = 0.0f;
                m_driveCounters[i].writeMbps = 0.0f;

                if (m_driveCounters[i].activeCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].activeCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].activePercent = (float)counterVal.doubleValue;
                    if (m_driveCounters[i].activePercent < 0.0f) {
                        m_driveCounters[i].activePercent = 0.0f;
                    }
                    if (m_driveCounters[i].activePercent > 100.0f) {
                        m_driveCounters[i].activePercent = 100.0f;
                    }
                }

                if (m_driveCounters[i].readCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].readCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].readMbps = (float)(counterVal.doubleValue * 8.0 / 1000000.0);
                    if (m_driveCounters[i].readMbps < 0.0f) {
                        m_driveCounters[i].readMbps = 0.0f;
                    }
                }

                if (m_driveCounters[i].writeCounter != NULL && PdhGetFormattedCounterValue(m_driveCounters[i].writeCounter, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
                    m_driveCounters[i].writeMbps = (float)(counterVal.doubleValue * 8.0 / 1000000.0);
                    if (m_driveCounters[i].writeMbps < 0.0f) {
                        m_driveCounters[i].writeMbps = 0.0f;
                    }
                }

                if (m_driveCounters[i].activePercent > m_diskUsage) {
                    m_diskUsage = m_driveCounters[i].activePercent;
                }
            }
        }
    }

    if (!pollCpu) {
        m_cpuUsage = 0.0f;
        m_cpuGHz = 0.0f;
    }

    if (!pollDisk) {
        m_diskUsage = 0.0f;
        for (UINT i = 0; i < kMaxDriveLetters; ++i) {
            m_driveCounters[i].activePercent = 0.0f;
            m_driveCounters[i].readMbps = 0.0f;
            m_driveCounters[i].writeMbps = 0.0f;
        }
    }

    if (pollGpu) {
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

    enum {
        GPU_ENGINE_3D = 0,
        GPU_ENGINE_COMPUTE,
        GPU_ENGINE_COPY,
        GPU_ENGINE_DECODE,
        GPU_ENGINE_ENCODE,
        GPU_ENGINE_PROCESSING,
        GPU_ENGINE_GDI,
        GPU_ENGINE_COUNT
    };

    float engineTotals[kMaxGpuAdapters][GPU_ENGINE_COUNT];
    bool engineSeen[kMaxGpuAdapters][GPU_ENGINE_COUNT];
    ZeroMemory(engineTotals, sizeof(engineTotals));
    ZeroMemory(engineSeen, sizeof(engineSeen));

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
    }

    const char* engineLabels[GPU_ENGINE_COUNT] = {
        "3D",
        "Compute",
        "Copy",
        "Decode",
        "Encode",
        "Process",
        "GDI"
    };

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
                maxEngineLabel = engineLabels[engineIndex];
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

void SystemMonitor::ClearDiskCounterHandles() {
    for (UINT i = 0; i < kMaxDriveLetters; ++i) {
        if (m_driveCounters[i].activeCounter != NULL) {
            PdhRemoveCounter(m_driveCounters[i].activeCounter);
            m_driveCounters[i].activeCounter = NULL;
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
        m_driveCounters[i].readMbps = 0.0f;
        m_driveCounters[i].writeMbps = 0.0f;
    }
}

bool SystemMonitor::ResolveDriveToPhysicalDisk(wchar_t driveLetter, int* outPhysicalDiskIndex) const {
    if (outPhysicalDiskIndex == NULL) {
        return false;
    }

    *outPhysicalDiskIndex = -1;

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

    for (UINT driveIndex = 0; driveIndex < kMaxDriveLetters; ++driveIndex) {
        const DWORD driveBit = (1u << driveIndex);
        if ((m_diskSelectionMask & driveBit) == 0) {
            continue;
        }

        if ((driveMask & driveBit) == 0) {
            continue;
        }

        wchar_t rootPath[4] = { (wchar_t)(L'A' + driveIndex), L':', L'\\', L'\0' };
        if (GetDriveTypeW(rootPath) != DRIVE_FIXED) {
            continue;
        }

        DriveCounterSlot* slot = &m_driveCounters[driveIndex];
        slot->selected = true;
        slot->driveLetter = (char)('A' + driveIndex);
        slot->physicalDiskIndex = -1;
        slot->fallbackTotal = false;
        slot->activePercent = 0.0f;
        slot->readMbps = 0.0f;
        slot->writeMbps = 0.0f;

        const wchar_t driveLetter = (wchar_t)(L'A' + driveIndex);
        ResolveDriveToPhysicalDisk(driveLetter, &slot->physicalDiskIndex);

        wchar_t instanceName[64] = {};
        if (!ResolveDriveToPdhInstance(driveLetter, slot->physicalDiskIndex, instanceName, (int)(sizeof(instanceName) / sizeof(instanceName[0])))) {
            wcscpy_s(instanceName, L"_Total");
            slot->fallbackTotal = true;
        }

        wcscpy_s(slot->pdhInstance, instanceName);

        auto removePartialCounters = [&]() {
            if (slot->activeCounter != NULL) {
                PdhRemoveCounter(slot->activeCounter);
                slot->activeCounter = NULL;
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

        auto addCounterSet = [&](const wchar_t* pdhInstanceName) -> bool {
            wchar_t activePath[128] = {};
            wchar_t readPath[128] = {};
            wchar_t writePath[128] = {};
            swprintf_s(activePath, L"\\PhysicalDisk(%ls)\\%% Disk Time", pdhInstanceName);
            swprintf_s(readPath, L"\\PhysicalDisk(%ls)\\Disk Read Bytes/sec", pdhInstanceName);
            swprintf_s(writePath, L"\\PhysicalDisk(%ls)\\Disk Write Bytes/sec", pdhInstanceName);

            if (PdhAddEnglishCounterW(m_pdhQuery, activePath, 0, &slot->activeCounter) != ERROR_SUCCESS) {
                removePartialCounters();
                return false;
            }

            if (PdhAddEnglishCounterW(m_pdhQuery, readPath, 0, &slot->readCounter) != ERROR_SUCCESS) {
                removePartialCounters();
                return false;
            }

            if (PdhAddEnglishCounterW(m_pdhQuery, writePath, 0, &slot->writeCounter) != ERROR_SUCCESS) {
                removePartialCounters();
                return false;
            }

            return true;
        };

        if (!addCounterSet(slot->pdhInstance)) {
            if (_wcsicmp(slot->pdhInstance, L"_Total") != 0) {
                slot->fallbackTotal = true;
                slot->physicalDiskIndex = -1;
                wcscpy_s(slot->pdhInstance, L"_Total");
                if (!addCounterSet(slot->pdhInstance)) {
                    slot->selected = false;
                    continue;
                }
            } else {
                slot->selected = false;
                continue;
            }
        }

        if (_wcsicmp(slot->pdhInstance, L"_Total") == 0) {
            slot->fallbackTotal = true;
        }
    }

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

bool SystemMonitor::TryQueryWifiSsidForLuid(ULONGLONG interfaceLuidValue, char* ssidBuffer, int ssidBufferSize) const {
    if (ssidBuffer == NULL || ssidBufferSize <= 0) {
        return false;
    }

    ssidBuffer[0] = '\0';

    HANDLE wlanHandle = NULL;
    DWORD negotiatedVersion = 0;
    if (WlanOpenHandle(2, NULL, &negotiatedVersion, &wlanHandle) != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    PWLAN_INTERFACE_INFO_LIST interfaceList = NULL;
    if (WlanEnumInterfaces(wlanHandle, NULL, &interfaceList) == ERROR_SUCCESS && interfaceList != NULL) {
        for (DWORD i = 0; i < interfaceList->dwNumberOfItems; ++i) {
            WLAN_INTERFACE_INFO* interfaceInfo = &interfaceList->InterfaceInfo[i];
            if (interfaceInfo->isState != wlan_interface_state_connected) {
                continue;
            }

            if (interfaceLuidValue != 0) {
                // Keep interfaceLuidValue in the signature for forward compatibility; on older stacks
                // we select the currently connected WLAN interface when LUID mapping is unavailable.
            }

            DWORD dataSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcodeType = wlan_opcode_value_type_invalid;
            PWLAN_CONNECTION_ATTRIBUTES connectionAttributes = NULL;
            if (WlanQueryInterface(
                    wlanHandle,
                    &interfaceInfo->InterfaceGuid,
                    wlan_intf_opcode_current_connection,
                    NULL,
                    &dataSize,
                    (PVOID*)&connectionAttributes,
                    &opcodeType) == ERROR_SUCCESS && connectionAttributes != NULL) {
                ULONG ssidLength = connectionAttributes->wlanAssociationAttributes.dot11Ssid.uSSIDLength;
                if (ssidLength >= (ULONG)ssidBufferSize) {
                    ssidLength = (ULONG)(ssidBufferSize - 1);
                }

                memcpy(ssidBuffer, connectionAttributes->wlanAssociationAttributes.dot11Ssid.ucSSID, ssidLength);
                ssidBuffer[ssidLength] = '\0';
                success = ssidLength > 0;
                WlanFreeMemory(connectionAttributes);
                if (success) {
                    break;
                }
            }

            if (connectionAttributes != NULL) {
                WlanFreeMemory(connectionAttributes);
            }
        }

        WlanFreeMemory(interfaceList);
    }

    WlanCloseHandle(wlanHandle, NULL);
    return success;
}

void SystemMonitor::BuildNetworkDisplayName(const NetworkAdapterSlot* adapter, char* output, int outputSize) const {
    if (output == NULL || outputSize <= 0) {
        return;
    }

    output[0] = '\0';

    if (adapter == NULL || !adapter->valid) {
        snprintf(output, outputSize, "Disconnected");
        return;
    }

    if (adapter->ssid[0] != '\0') {
        snprintf(output, outputSize, "%s", adapter->ssid);
        return;
    }

    if (adapter->connected) {
        snprintf(output, outputSize, "Wired");
    } else {
        snprintf(output, outputSize, "Disconnected");
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
    if (primaryAdapter->isWifi) {
        TryQueryWifiSsidForLuid(primaryAdapter->interfaceLuidValue, primaryAdapter->ssid, (int)sizeof(primaryAdapter->ssid));
    }
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
        if (secondaryAdapter->isWifi) {
            TryQueryWifiSsidForLuid(secondaryAdapter->interfaceLuidValue, secondaryAdapter->ssid, (int)sizeof(secondaryAdapter->ssid));
        }
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
    m_primaryNetworkMetrics.rxMbps = 0.0f;
    m_primaryNetworkMetrics.txMbps = 0.0f;
    m_primaryNetworkMetrics.totalMbps = 0.0f;

    m_secondaryNetworkMetrics.rxMbps = 0.0f;
    m_secondaryNetworkMetrics.txMbps = 0.0f;
    m_secondaryNetworkMetrics.totalMbps = 0.0f;

    if (m_primaryNetworkSlot < 0 || m_primaryNetworkSlot >= (int)m_networkAdapterCount) {
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
            snapshot->rxMbps = 0.0f;
            snapshot->txMbps = 0.0f;
            snapshot->totalMbps = 0.0f;
            return;
        }

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

        const ULONGLONG deltaInOctets = ifRow.dwInOctets >= deltaState->lastInOctets ? (ifRow.dwInOctets - deltaState->lastInOctets) : 0;
        const ULONGLONG deltaOutOctets = ifRow.dwOutOctets >= deltaState->lastOutOctets ? (ifRow.dwOutOctets - deltaState->lastOutOctets) : 0;
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
        deltaState->lastInOctets = ifRow.dwInOctets;
        deltaState->lastOutOctets = ifRow.dwOutOctets;
        deltaState->lastSampleTimeMs = now;
    };

    updateSnapshot(&m_primaryNetworkMetrics, &m_primaryDeltaState);
    if (m_hasSecondaryNetworkMetrics) {
        updateSnapshot(&m_secondaryNetworkMetrics, &m_secondaryDeltaState);
    }
}
