#pragma once
#include <windows.h>

struct ProcessGpuUsage;

// One process as shown in the process window. Rates cover the interval between the last two samples.
struct ProcessRow {
    DWORD pid;
    DWORD parentPid;
    DWORD sessionId;
    ULONGLONG createTime;        // (pid, createTime) identifies a process even when a pid is reused
    char name[128];              // UTF-8 image name, e.g. "msedge.exe"
    float cpuPercent;            // share of all logical processors, as Task Manager computes it
    ULONGLONG privateWorkingSet; // bytes; Task Manager's "Memory" column
    ULONGLONG workingSet;        // bytes
    ULONGLONG commitBytes;       // private bytes (commit charge)
    float ioBytesPerSec;         // read + write: all I/O (files, pipes, devices, network), not only disk
    float gpuPercent;
    BYTE gpuAdapterIndex;
    BYTE gpuEngineType;          // SystemMonitor::GetGpuEngineLabel
    DWORD threadCount;
    DWORD handleCount;
    ULONGLONG cpuTime100ns;      // running totals, kept for the next sample's deltas
    ULONGLONG ioBytes;
};

struct ProcessTotals {
    float cpuPercent;            // 100 - System Idle Process share of elapsed time x logical processors
    float ioBytesPerSec;
    ULONGLONG privateWorkingSet;
    UINT processCount;
    UINT threadCount;
    UINT handleCount;
};

// Per-process CPU, memory and I/O from one NtQuerySystemInformation(SystemProcessInformation) call
// per sample (about 4 ms for ~365 processes), with no per-process handles. All buffers are
// grow-only and are released by Release() when the process window closes.
class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    // Takes a snapshot and computes rates against the previous one. Returns true when rates are valid
    // (a previous sample existed); the first call after construction or Release() only primes them.
    bool Sample();
    // Merges SystemMonitor's per-process GPU load (sorted by pid) into the current rows.
    void ApplyGpuUsage(const ProcessGpuUsage* entries, UINT count);
    void Release();

    UINT GetRowCount() const { return m_rowCount; }
    const ProcessRow* GetRows() const { return m_rows; }
    const ProcessTotals& GetTotals() const { return m_totals; }
    bool HasRates() const { return m_hasRates; }
    int FindRowIndex(DWORD pid) const;

private:
    ProcessRow* m_rows;
    ProcessRow* m_previousRows;
    UINT m_rowCount;
    UINT m_previousRowCount;
    UINT m_rowCapacity;

    // Open-addressing pid -> row index + 1 (0 = empty) for m_rows. Built at the end of each sample;
    // after the row buffers swap it indexes the previous rows, so one build serves both lookups.
    int* m_index;
    UINT m_indexMask;

    BYTE* m_snapshot;
    ULONG m_snapshotCapacity;

    LARGE_INTEGER m_previousQpc;
    ULONGLONG m_previousIdleTime100ns;
    DWORD m_logicalProcessorCount;
    bool m_hasPrevious;
    bool m_hasRates;
    ProcessTotals m_totals;

    bool QuerySnapshot();
    bool EnsureRowCapacity(UINT count);
    void BuildIndex();
    int FindPreviousRowIndex(DWORD pid) const;
};
