#pragma once
#include <atomic>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

struct ID3D11Device;
struct IDXGIAdapter3;

enum SystemMetricMask : DWORD {
    SYSTEM_METRIC_CPU     = 1u << 0,
    SYSTEM_METRIC_RAM     = 1u << 1,
    SYSTEM_METRIC_GPU     = 1u << 2,
    SYSTEM_METRIC_DISK    = 1u << 3,
    SYSTEM_METRIC_NETWORK = 1u << 4,
    SYSTEM_METRIC_ALL     = SYSTEM_METRIC_CPU | SYSTEM_METRIC_RAM | SYSTEM_METRIC_GPU | SYSTEM_METRIC_DISK | SYSTEM_METRIC_NETWORK
};

enum GPUDisplayMode : DWORD {
    GPU_DISPLAY_TARGETED = 0,
    GPU_DISPLAY_HIGHEST_LOAD = 1,
    GPU_DISPLAY_MULTI_GPU = 2,
    GPU_DISPLAY_AGGREGATE = 3
};

enum NetworkPrimaryMode : DWORD {
    NETWORK_PRIMARY_AUTO = 0,
    NETWORK_PRIMARY_MANUAL = 1
};

enum NetworkDisplayMode : DWORD {
    NETWORK_DISPLAY_TOTAL = 0,
    NETWORK_DISPLAY_RX_TX = 1,
    NETWORK_DISPLAY_RX_TX_SECONDARY = 2
};

struct GPUMetricsSnapshot {
    UINT adapterIndex;
    char adapterName[128];
    float usagePercent;
    float dedicatedUsedGB;
    float dedicatedTotalGB;
    float sharedUsedGB;
    bool utilizationAvailable;
    bool memoryAvailable;
    char engineLabel[16];
};

struct DiskMetricsSnapshot {
    char driveLabel[4];
    int physicalDiskIndex;
    bool fallbackTotal;
    float activePercent;
    float readKBps;
    float writeKBps;
    float usedGB;
    float totalGB;
    float freeGB;
    bool capacityAvailable;
};

struct NetworkMetricsSnapshot {
    ULONG ifIndex;
    char displayName[192];
    float rxMbps;
    float txMbps;
    float totalMbps;
    bool connected;
};

class SystemMonitor {
public:
    explicit SystemMonitor(ID3D11Device* d3dDevice = nullptr);
    ~SystemMonitor();
    
    // Call every frame; updates internally only once per 1000ms
    void Update();
    void SetPollingIntervalMs(DWORD pollingIntervalMs);
    DWORD GetPollingIntervalMs() const { return m_pollingIntervalMs; }
    void SetEnabledMetricsMask(DWORD enabledMask);
    DWORD GetEnabledMetricsMask() const { return m_enabledMetricsMask; }
    void SetDiskSelectionMask(DWORD diskSelectionMask);
    DWORD GetDiskSelectionMask() const { return m_diskSelectionMask; }

    void SetGPUDisplayMode(DWORD gpuDisplayMode);
    DWORD GetGPUDisplayMode() const { return m_gpuDisplayMode; }
    void SetSelectedGPUAdapterIndex(DWORD gpuAdapterIndex);
    DWORD GetSelectedGPUAdapterIndex() const { return m_selectedGpuAdapterIndex; }

    void SetNetworkPrimaryMode(DWORD networkPrimaryMode);
    DWORD GetNetworkPrimaryMode() const { return m_networkPrimaryMode; }
    void SetNetworkPrimaryIfIndex(DWORD networkPrimaryIfIndex);
    DWORD GetNetworkPrimaryIfIndex() const { return m_networkPrimaryIfIndex; }
    void SetNetworkSecondaryEnabled(BOOL enabled);
    BOOL IsNetworkSecondaryEnabled() const { return m_networkSecondaryEnabled; }
    void SetNetworkSecondaryIfIndex(DWORD networkSecondaryIfIndex);
    DWORD GetNetworkSecondaryIfIndex() const { return m_networkSecondaryIfIndex; }
    void SetNetworkDisplayMode(DWORD networkDisplayMode);
    DWORD GetNetworkDisplayMode() const { return m_networkDisplayMode; }
    void RefreshNetworkIdentityNow();
    void RefreshDiskTopologyNow();
    void RequestDiskTopologyRefresh();

    UINT GetGPUAdapterCount() const { return m_gpuAdapterCount; }
    const char* GetGPUAdapterNameByIndex(UINT index) const;
    UINT GetDisplayedGPUCount() const;
    bool GetDisplayedGPUSnapshot(UINT rowIndex, GPUMetricsSnapshot* outSnapshot) const;

    UINT GetSelectedDiskMetricCount() const;
    bool GetSelectedDiskMetric(UINT rowIndex, DiskMetricsSnapshot* outSnapshot) const;

    UINT GetNetworkAdapterCount() const { return m_networkAdapterCount; }
    DWORD GetNetworkAdapterIfIndex(UINT index) const;
    const char* GetNetworkAdapterNameByIndex(UINT index) const;
    const NetworkMetricsSnapshot& GetPrimaryNetworkMetrics() const { return m_primaryNetworkMetrics; }
    const NetworkMetricsSnapshot& GetSecondaryNetworkMetrics() const { return m_secondaryNetworkMetrics; }
    bool HasSecondaryNetworkMetrics() const { return m_hasSecondaryNetworkMetrics; }

    // Getters for the HUD
    const char* GetCPUName() const { return m_cpuName; }
    float GetCPUUsage() const { return m_cpuUsage; }
    float GetCPUGHz() const { return m_cpuGHz; }
    DWORD GetRAMSpeedMHz() const { return m_ramSpeedMHz; }
    float GetRAMUsedGB() const { return m_ramUsedGB; }
    float GetRAMTotalGB() const { return m_ramTotalGB; }
    float GetRAMUsagePercent() const { return m_ramUsagePercent; }
    float GetDiskUsagePercent() const { return m_diskUsage; }
    float GetNetworkTotalMbps() const { return m_primaryNetworkMetrics.totalMbps; }
    const char* GetNetworkAdapterName() const { return m_networkAdapterName; }
    const char* GetNetworkSSID() const { return m_networkSSID; }
    const char* GetNetworkDisplayName() const { return m_networkDisplayName; }
    float GetNetworkRxMbps() const { return m_primaryNetworkMetrics.rxMbps; }
    float GetNetworkTxMbps() const { return m_primaryNetworkMetrics.txMbps; }
    float GetGPUUsagePercent() const { return m_legacyGpuDisplay.usagePercent; }
    float GetGPUDedicatedUsedGB() const { return m_legacyGpuDisplay.dedicatedUsedGB; }
    float GetGPUDedicatedTotalGB() const { return m_legacyGpuDisplay.dedicatedTotalGB; }
    float GetGPUSharedUsedGB() const { return m_legacyGpuDisplay.sharedUsedGB; }
    const char* GetGPUAdapterName() const { return m_legacyGpuDisplay.adapterName; }
    const char* GetGPUEngineLabel() const { return m_legacyGpuDisplay.engineLabel; }
    bool IsGPUMemoryAvailable() const { return m_legacyGpuDisplay.memoryAvailable; }
    bool IsGPUUtilizationAvailable() const { return m_legacyGpuDisplay.utilizationAvailable; }

private:
    static const UINT kMaxGpuAdapters = 8;
    static const UINT kMaxDriveLetters = 26;
    static const UINT kMaxNetworkAdapters = 16;

    struct GPUAdapterSlot {
        bool valid;
        LUID adapterLuid;
        char adapterName[128];
        char luidPatternHL[40];
        char luidPatternLH[40];
        IDXGIAdapter3* adapter3;
    };

    struct DriveCounterSlot {
        bool selected;
        char driveLetter;
        int physicalDiskIndex;
        bool fallbackTotal;
        wchar_t pdhInstance[64];
        PDH_HCOUNTER activeCounter;
        PDH_HCOUNTER readCounter;
        PDH_HCOUNTER writeCounter;
        float activePercent;
        float readKBps;
        float writeKBps;
        float usedGB;
        float totalGB;
        float freeGB;
        bool capacityAvailable;
    };

    struct NetworkAdapterSlot {
        bool valid;
        ULONG ifIndex;
        ULONGLONG interfaceLuidValue;
        bool connected;
        bool hasGateway;
        bool isWifi;
        char adapterName[128];
        char ssid[64];
        char displayName[192];
        ULONGLONG lastSsidQueryTick;
    };

    struct NetworkDeltaState {
        bool valid;
        ULONG ifIndex;
        ULONGLONG lastInOctets;
        ULONGLONG lastOutOctets;
        ULONGLONG lastSampleTimeMs;
    };

    // Timing mechanics
    ULONGLONG m_lastUpdateTime;
    ULONGLONG m_lastNetworkIdentityRefresh;
    std::atomic<bool> m_networkIdentityRefreshRequested;
    std::atomic<bool> m_networkSsidRefreshRequested;
    std::atomic<bool> m_networkCallbacksEnabled;
    std::atomic<bool> m_diskTopologyRefreshRequested;
    DWORD m_pollingIntervalMs;
    DWORD m_enabledMetricsMask;
    DWORD m_diskSelectionMask;
    DWORD m_cachedLogicalDrivesMask;

    DWORD m_gpuDisplayMode;
    DWORD m_selectedGpuAdapterIndex;
    UINT m_gpuActiveAdapterIndex;

    DWORD m_networkPrimaryMode;
    DWORD m_networkPrimaryIfIndex;
    BOOL m_networkSecondaryEnabled;
    DWORD m_networkSecondaryIfIndex;
    DWORD m_networkDisplayMode;

    // Cache metrics
    char m_cpuName[64];
    float m_cpuUsage;
    float m_cpuGHz;
    DWORD m_ramSpeedMHz;
    float m_ramUsedGB;
    float m_ramTotalGB;
    float m_ramUsagePercent;
    float m_diskUsage;
    char m_networkAdapterName[128];
    char m_networkSSID[64];
    char m_networkDisplayName[192];
    GPUMetricsSnapshot m_legacyGpuDisplay;

    DWORD m_cpuBaseFreqMHz;

    GPUAdapterSlot m_gpuAdapters[kMaxGpuAdapters];
    GPUMetricsSnapshot m_gpuSnapshots[kMaxGpuAdapters];
    UINT m_gpuAdapterCount;

    DriveCounterSlot m_driveCounters[kMaxDriveLetters];

    NetworkAdapterSlot m_networkAdapters[kMaxNetworkAdapters];
    UINT m_networkAdapterCount;
    int m_primaryNetworkSlot;
    int m_secondaryNetworkSlot;
    NetworkMetricsSnapshot m_primaryNetworkMetrics;
    NetworkMetricsSnapshot m_secondaryNetworkMetrics;
    bool m_hasSecondaryNetworkMetrics;
    NetworkDeltaState m_primaryDeltaState;
    NetworkDeltaState m_secondaryDeltaState;

    HANDLE m_networkAddressChangeHandle;
    HANDLE m_networkAddressChangeWaitHandle;
    OVERLAPPED m_networkAddressChangeOverlapped;

    // PDH Handles
    PDH_HQUERY m_pdhQuery;
    PDH_HCOUNTER m_pdhCpuCounter;
    PDH_HCOUNTER m_pdhCpuPerfCounter;
    PDH_HCOUNTER m_pdhDiskInstanceCounter;
    PDH_HCOUNTER m_pdhGpuEngineCounter;
    BYTE* m_pdhDiskBuffer;
    DWORD m_pdhDiskBufferSize;
    BYTE* m_pdhGpuBuffer;
    DWORD m_pdhGpuBufferSize;

    void PollMetrics();
    void InitializeGpuMonitoring(ID3D11Device* d3dDevice);
    void PollGpuMetrics();
    void RefreshNetworkIdentity();
    void PollNetworkThroughput();
    void SyncLegacyGpuFields();
    void InitializeNetworkNotifications();
    void ShutdownNetworkNotifications();
    void ArmNetworkAddressChangeNotification();
    void RequestNetworkIdentityRefresh();

    static VOID CALLBACK OnNetworkAddressChangeWaitCallback(PVOID context, BOOLEAN timerOrWaitFired);

    void RebuildDiskCounters();
    void ClearDiskCounterHandles();
    bool ResolveDriveToPhysicalDisk(wchar_t driveLetter, int* outPhysicalDiskIndex) const;
    bool ResolveDriveToPdhInstance(wchar_t driveLetter, int preferredDiskIndex, wchar_t* outInstance, int outInstanceLength);

    int FindGpuAdapterIndexForInstance(const char* instanceName) const;
    int FindTargetedGpuIndex() const;
    int FindHighestLoadGpuIndex() const;
    void BuildAggregateGpuSnapshot(GPUMetricsSnapshot* outSnapshot) const;

    int FindNetworkAdapterSlotByIfIndex(ULONG ifIndex) const;
    int ResolveAutoPrimaryNetworkSlot() const;
    int ResolveAutoSecondaryNetworkSlot(int primarySlot) const;
    void ResetNetworkDeltaState(NetworkDeltaState* state);
    bool TryQueryConnectedNetworkName(char* nameBuffer, int nameBufferSize) const;
    void BuildNetworkDisplayName(const NetworkAdapterSlot* adapter, char* output, int outputSize) const;
    void ClearNetworkSnapshots();
    bool EnsurePdhBuffer(BYTE*& buffer, DWORD& bufferCapacity, DWORD requiredSize);
    static void CopyWideToUtf8(const wchar_t* source, char* destination, int destinationSize);
    static bool ContainsIgnoreCase(const char* haystack, const char* needle);
    static void QueryCpuName(char* outName, int outSize);
    static DWORD QueryRamSpeedMHz();
};
