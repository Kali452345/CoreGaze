#include "ProcessMonitor.h"
#include "SystemMonitor.h"
#include "EtwMonitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// SYSTEM_PROCESS_INFORMATION as returned for SystemProcessInformation (5). winternl.h only declares a
// few fields, so the full documented layout (stable since Windows 7) is spelled out here.
struct CountedUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct SystemProcessInformation {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    CountedUnicodeString ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};

typedef LONG (NTAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

const ULONG kSystemProcessInformationClass = 5;
const LONG kStatusInfoLengthMismatch = (LONG)0xC0000004L;
const ULONG kInitialSnapshotBytes = 1024 * 1024;
const UINT kRowSlack = 64;

NtQuerySystemInformationFn ResolveNtQuerySystemInformation() {
    static NtQuerySystemInformationFn fn = (NtQuerySystemInformationFn)(void*)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    return fn;
}

UINT NextPowerOfTwo(UINT value) {
    UINT result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

UINT HashPid(DWORD pid) {
    // Pids are multiples of 4; drop those bits before mixing.
    return (pid >> 2) * 2654435761u;
}

void CopyImageName(const SystemProcessInformation* info, char* output, int outputSize) {
    const DWORD pid = (DWORD)(ULONG_PTR)info->UniqueProcessId;
    if (info->ImageName.Buffer == NULL || info->ImageName.Length == 0) {
        if (pid == 0) {
            snprintf(output, outputSize, "System Idle Process");
        } else {
            snprintf(output, outputSize, "pid %lu", pid);
        }
        return;
    }

    char utf8[768];
    const int length = WideCharToMultiByte(CP_UTF8, 0, info->ImageName.Buffer, info->ImageName.Length / 2,
                                           utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (length <= 0) {
        snprintf(output, outputSize, "pid %lu", pid);
        return;
    }

    // Truncate on a UTF-8 character boundary.
    int copyLength = length;
    if (copyLength > outputSize - 1) {
        copyLength = outputSize - 1;
        while (copyLength > 0 && ((unsigned char)utf8[copyLength] & 0xC0) == 0x80) {
            --copyLength;
        }
    }
    memcpy(output, utf8, copyLength);
    output[copyLength] = '\0';
}

} // namespace

ProcessMonitor::ProcessMonitor()
    : m_rows(NULL),
      m_previousRows(NULL),
      m_rowCount(0),
      m_previousRowCount(0),
      m_rowCapacity(0),
      m_index(NULL),
      m_indexMask(0),
      m_snapshot(NULL),
      m_snapshotCapacity(0),
      m_previousIdleTime100ns(0),
      m_logicalProcessorCount(0),
      m_hasPrevious(false),
      m_hasRates(false) {
    m_previousQpc.QuadPart = 0;
    ZeroMemory(&m_totals, sizeof(m_totals));
}

ProcessMonitor::~ProcessMonitor() {
    Release();
}

void ProcessMonitor::Release() {
    free(m_rows);
    free(m_previousRows);
    free(m_index);
    free(m_snapshot);
    m_rows = NULL;
    m_previousRows = NULL;
    m_index = NULL;
    m_snapshot = NULL;
    m_rowCount = 0;
    m_previousRowCount = 0;
    m_rowCapacity = 0;
    m_indexMask = 0;
    m_snapshotCapacity = 0;
    m_hasPrevious = false;
    m_hasRates = false;
    ZeroMemory(&m_totals, sizeof(m_totals));
}

bool ProcessMonitor::QuerySnapshot() {
    NtQuerySystemInformationFn query = ResolveNtQuerySystemInformation();
    if (query == NULL) {
        return false;
    }

    if (m_snapshot == NULL) {
        m_snapshot = (BYTE*)malloc(kInitialSnapshotBytes);
        if (m_snapshot == NULL) {
            return false;
        }
        m_snapshotCapacity = kInitialSnapshotBytes;
    }

    // The list can grow between the size probe and the retry, so allow a few attempts.
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG requiredBytes = 0;
        const LONG status = query(kSystemProcessInformationClass, m_snapshot, m_snapshotCapacity, &requiredBytes);
        if (status >= 0) {
            return true;
        }
        if (status != kStatusInfoLengthMismatch) {
            return false;
        }

        const ULONG newCapacity = (requiredBytes > m_snapshotCapacity ? requiredBytes : m_snapshotCapacity) + 128 * 1024;
        BYTE* grown = (BYTE*)realloc(m_snapshot, newCapacity);
        if (grown == NULL) {
            return false;
        }
        m_snapshot = grown;
        m_snapshotCapacity = newCapacity;
    }
    return false;
}

bool ProcessMonitor::EnsureRowCapacity(UINT count) {
    if (count <= m_rowCapacity) {
        return true;
    }

    const UINT newCapacity = count + kRowSlack;
    ProcessRow* rows = (ProcessRow*)realloc(m_rows, newCapacity * sizeof(ProcessRow));
    if (rows == NULL) {
        return false;
    }
    m_rows = rows;
    ProcessRow* previousRows = (ProcessRow*)realloc(m_previousRows, newCapacity * sizeof(ProcessRow));
    if (previousRows == NULL) {
        return false;
    }
    m_previousRows = previousRows;

    // Load factor stays at or below 50%.
    const UINT indexSize = NextPowerOfTwo(newCapacity * 2);
    int* index = (int*)realloc(m_index, indexSize * sizeof(int));
    if (index == NULL) {
        return false;
    }
    m_index = index;
    m_indexMask = indexSize - 1;
    m_rowCapacity = newCapacity;
    return true;
}

void ProcessMonitor::BuildIndex() {
    if (m_index == NULL) {
        return;
    }
    ZeroMemory(m_index, (m_indexMask + 1) * sizeof(int));
    for (UINT i = 0; i < m_rowCount; ++i) {
        UINT slot = HashPid(m_rows[i].pid) & m_indexMask;
        while (m_index[slot] != 0) {
            slot = (slot + 1) & m_indexMask;
        }
        m_index[slot] = (int)i + 1;
    }
}

int ProcessMonitor::FindRowIndex(DWORD pid) const {
    if (m_index == NULL) {
        return -1;
    }
    for (UINT slot = HashPid(pid) & m_indexMask; m_index[slot] != 0; slot = (slot + 1) & m_indexMask) {
        const int rowIndex = m_index[slot] - 1;
        if (m_rows[rowIndex].pid == pid) {
            return rowIndex;
        }
    }
    return -1;
}

int ProcessMonitor::FindPreviousRowIndex(DWORD pid) const {
    // Valid only inside Sample(), after the buffers swap and before BuildIndex().
    if (m_index == NULL) {
        return -1;
    }
    for (UINT slot = HashPid(pid) & m_indexMask; m_index[slot] != 0; slot = (slot + 1) & m_indexMask) {
        const int rowIndex = m_index[slot] - 1;
        if (m_previousRows[rowIndex].pid == pid) {
            return rowIndex;
        }
    }
    return -1;
}

bool ProcessMonitor::Sample() {
    // The kernel reads each process's times while it builds the snapshot (several ms on a busy
    // machine), so time-stamp the middle of the call.
    LARGE_INTEGER qpcBefore, qpcAfter, qpcFrequency;
    QueryPerformanceCounter(&qpcBefore);
    if (!QuerySnapshot()) {
        return false;
    }
    QueryPerformanceCounter(&qpcAfter);
    QueryPerformanceFrequency(&qpcFrequency);
    LARGE_INTEGER qpc;
    qpc.QuadPart = qpcBefore.QuadPart + (qpcAfter.QuadPart - qpcBefore.QuadPart) / 2;
    if (m_logicalProcessorCount == 0) {
        m_logicalProcessorCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (m_logicalProcessorCount == 0) {
            m_logicalProcessorCount = 1;
        }
    }

    UINT entryCount = 0;
    for (const BYTE* cursor = m_snapshot;;) {
        const SystemProcessInformation* info = (const SystemProcessInformation*)cursor;
        ++entryCount;
        if (info->NextEntryOffset == 0) {
            break;
        }
        cursor += info->NextEntryOffset;
    }

    // The current rows become the previous rows; m_index keeps pointing at them.
    ProcessRow* swap = m_previousRows;
    m_previousRows = m_rows;
    m_rows = swap;
    m_previousRowCount = m_hasPrevious ? m_rowCount : 0;
    m_rowCount = 0;

    const UINT oldIndexMask = m_indexMask;
    if (!EnsureRowCapacity(entryCount)) {
        return false;
    }
    if (m_indexMask != oldIndexMask) {
        // The index grew and lost its contents; rebuild it over the previous rows.
        ZeroMemory(m_index, (m_indexMask + 1) * sizeof(int));
        for (UINT i = 0; i < m_previousRowCount; ++i) {
            UINT slot = HashPid(m_previousRows[i].pid) & m_indexMask;
            while (m_index[slot] != 0) {
                slot = (slot + 1) & m_indexMask;
            }
            m_index[slot] = (int)i + 1;
        }
    }

    const bool hasPrevious = m_hasPrevious;
    const double elapsedSeconds = hasPrevious
        ? (double)(qpc.QuadPart - m_previousQpc.QuadPart) / (double)qpcFrequency.QuadPart
        : 0.0;

    ProcessTotals totals;
    ZeroMemory(&totals, sizeof(totals));
    ULONGLONG idleTime100ns = 0;
    ULONGLONG totalDeltaIoBytes = 0;

    for (const BYTE* cursor = m_snapshot;;) {
        const SystemProcessInformation* info = (const SystemProcessInformation*)cursor;
        const DWORD pid = (DWORD)(ULONG_PTR)info->UniqueProcessId;
        const ULONGLONG cpuTime = (ULONGLONG)info->UserTime.QuadPart + (ULONGLONG)info->KernelTime.QuadPart;

        if (pid == 0) {
            // The System Idle Process's time is the idle time of every logical processor. It is used
            // for the CPU total but, as in Task Manager's Processes page, not listed.
            idleTime100ns = cpuTime;
        } else {
            ProcessRow* row = &m_rows[m_rowCount++];
            row->pid = pid;
            row->parentPid = (DWORD)(ULONG_PTR)info->InheritedFromUniqueProcessId;
            row->sessionId = info->SessionId;
            row->createTime = (ULONGLONG)info->CreateTime.QuadPart;
            row->privateWorkingSet = (ULONGLONG)info->WorkingSetPrivateSize.QuadPart;
            row->workingSet = info->WorkingSetSize;
            row->commitBytes = info->PrivatePageCount;
            row->threadCount = info->NumberOfThreads;
            row->handleCount = info->HandleCount;
            row->cpuTime100ns = cpuTime;
            row->ioBytes = (ULONGLONG)info->ReadTransferCount.QuadPart + (ULONGLONG)info->WriteTransferCount.QuadPart;
            row->cpuPercent = 0.0f;
            row->cpuPowerWatts = 0.0f;
            row->ioBytesPerSec = 0.0f;
            row->diskReadBytesPerSec = 0.0f;
            row->diskWriteBytesPerSec = 0.0f;
            row->networkSendBytesPerSec = 0.0f;
            row->networkReceiveBytesPerSec = 0.0f;
            row->gpuPercent = 0.0f;
            row->gpuAdapterIndex = 0;
            row->gpuEngineType = 0;

            const int previousIndex = hasPrevious ? FindPreviousRowIndex(pid) : -1;
            const ProcessRow* previous = (previousIndex >= 0) ? &m_previousRows[previousIndex] : NULL;
            if (previous != NULL && previous->createTime == row->createTime) {
                memcpy(row->name, previous->name, sizeof(row->name));
                if (cpuTime >= previous->cpuTime100ns) {
                    // Raw delta for now; converted to a percentage once the total is known.
                    const ULONGLONG deltaCpu = cpuTime - previous->cpuTime100ns;
                    row->cpuPercent = (float)deltaCpu;
                }
                if (row->ioBytes >= previous->ioBytes && elapsedSeconds > 0.0) {
                    const ULONGLONG deltaIo = row->ioBytes - previous->ioBytes;
                    row->ioBytesPerSec = (float)((double)deltaIo / elapsedSeconds);
                    totalDeltaIoBytes += deltaIo;
                }
            } else {
                // New process (or a reused pid): name it once; rates start on the next sample.
                CopyImageName(info, row->name, (int)sizeof(row->name));
            }

            totals.privateWorkingSet += row->privateWorkingSet;
            totals.threadCount += row->threadCount;
            totals.handleCount += row->handleCount;
        }

        if (info->NextEntryOffset == 0) {
            break;
        }
        cursor += info->NextEntryOffset;
    }

    // Task Manager's formula: delta process time / (delta elapsed x logical processors). The sum of all
    // process times (Idle included) is not used as the denominator: interrupt and DPC time is charged
    // to no process, so rows would read slightly high and the total 1-2 points low.
    bool ratesValid = false;
    if (hasPrevious && elapsedSeconds > 0.0) {
        const double denominator = elapsedSeconds * 1.0e7 * (double)m_logicalProcessorCount;
        for (UINT i = 0; i < m_rowCount; ++i) {
            float percent = (float)((double)m_rows[i].cpuPercent * 100.0 / denominator);
            m_rows[i].cpuPercent = (percent > 100.0f) ? 100.0f : percent;
        }
        const double idleDelta = (idleTime100ns >= m_previousIdleTime100ns) ? (double)(idleTime100ns - m_previousIdleTime100ns) : 0.0;
        double busyPercent = 100.0 - idleDelta * 100.0 / denominator;
        totals.cpuPercent = (float)(busyPercent < 0.0 ? 0.0 : (busyPercent > 100.0 ? 100.0 : busyPercent));
        ratesValid = true;
    } else {
        for (UINT i = 0; i < m_rowCount; ++i) {
            m_rows[i].cpuPercent = 0.0f;
        }
    }

    totals.processCount = m_rowCount;
    totals.ioBytesPerSec = (elapsedSeconds > 0.0) ? (float)((double)totalDeltaIoBytes / elapsedSeconds) : 0.0f;
    m_totals = totals;

    m_previousQpc = qpc;
    m_previousIdleTime100ns = idleTime100ns;
    m_hasPrevious = true;
    m_hasRates = ratesValid;

    BuildIndex();
    return ratesValid;
}

void ProcessMonitor::ApplyGpuUsage(const ProcessGpuUsage* entries, UINT count) {
    if (entries == NULL) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        const int rowIndex = FindRowIndex(entries[i].pid);
        if (rowIndex < 0) {
            continue;
        }
        ProcessRow* row = &m_rows[rowIndex];
        row->gpuPercent = entries[i].percent;
        row->gpuAdapterIndex = entries[i].adapterIndex;
        row->gpuEngineType = entries[i].engineType;
    }
}

void ProcessMonitor::ApplyIoUsage(const ProcessIoUsage* entries, UINT count) {
    m_totals.diskBytesPerSec = 0.0f;
    m_totals.networkBytesPerSec = 0.0f;
    if (entries == NULL) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        const ProcessIoUsage& usage = entries[i];
        // Totals include traffic of processes that exited during the interval.
        m_totals.diskBytesPerSec += usage.diskReadBytesPerSec + usage.diskWriteBytesPerSec;
        m_totals.networkBytesPerSec += usage.networkSendBytesPerSec + usage.networkReceiveBytesPerSec;

        const int rowIndex = FindRowIndex(usage.pid);
        if (rowIndex < 0) {
            continue;
        }
        ProcessRow* row = &m_rows[rowIndex];
        row->diskReadBytesPerSec = usage.diskReadBytesPerSec;
        row->diskWriteBytesPerSec = usage.diskWriteBytesPerSec;
        row->networkSendBytesPerSec = usage.networkSendBytesPerSec;
        row->networkReceiveBytesPerSec = usage.networkReceiveBytesPerSec;
    }
}

void ProcessMonitor::ApplyCpuPower(float totalPackageWatts) {
    m_totals.cpuPackageWatts = totalPackageWatts;
    if (totalPackageWatts <= 0.0f || m_totals.cpuPercent < 0.1f) {
        for (UINT i = 0; i < m_rowCount; ++i) {
            m_rows[i].cpuPowerWatts = 0.0f;
        }
        return;
    }

    const float totalCpu = m_totals.cpuPercent;
    for (UINT i = 0; i < m_rowCount; ++i) {
        if (m_rows[i].cpuPercent > 0.0f) {
            m_rows[i].cpuPowerWatts = totalPackageWatts * (m_rows[i].cpuPercent / totalCpu);
        } else {
            m_rows[i].cpuPowerWatts = 0.0f;
        }
    }
}
