#include "EtwMonitor.h"
#include <stdlib.h>
#include <string.h>

namespace {

const wchar_t kSessionName[] = L"CoreGaze Kernel Trace";

// A private GUID identifies CoreGaze's system logger session.
const GUID kSessionGuid = { 0x7c1f9e2a, 0x4b3d, 0x4e8f, { 0x9a, 0x61, 0x2d, 0x5c, 0x8b, 0x0e, 0x3f, 0x47 } };

// Classic kernel event classes (MOF). Their events arrive with these as the provider id.
const GUID kDiskIoGuid = { 0x3d6fa8d4, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
const GUID kTcpIpGuid = { 0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2 } };
const GUID kUdpIpGuid = { 0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80 } };

const UCHAR kOpcodeDiskRead = 10;         // DiskIo_TypeGroup1, completion of a read
const UCHAR kOpcodeDiskWrite = 11;
const UCHAR kOpcodeSendIPv4 = 10;         // TcpIp/UdpIp send and receive; +16 for IPv6
const UCHAR kOpcodeReceiveIPv4 = 11;
const UCHAR kOpcodeSendIPv6 = 26;
const UCHAR kOpcodeReceiveIPv6 = 27;

// DiskIo_TypeGroup1 on 64-bit Windows: DiskNumber, IrpFlags, TransferSize, Reserved (4 x ULONG),
// ByteOffset, FileObject, Irp, HighResResponseTime (4 x 8 bytes), IssuingThreadId (Windows 8+).
// The header's process and thread ids are those of whatever context completed the I/O, so the
// issuing thread is what identifies the process.
const ULONG kDiskTransferSizeOffset = 8;
const ULONG kDiskIssuingThreadIdOffset = 48;

// TcpIp/UdpIp send and receive payloads start with PID and size (both ULONG) for IPv4 and IPv6.
const ULONG kNetworkPidOffset = 0;
const ULONG kNetworkSizeOffset = 4;

const ULONGLONG kThreadOwnerLifetimeMs = 5000;

enum ByteKind {
    BYTES_DISK_READ = 0,
    BYTES_DISK_WRITE,
    BYTES_NETWORK_SEND,
    BYTES_NETWORK_RECEIVE
};

UINT HashId(DWORD id) {
    return (id >> 2) * 2654435761u;
}

ULONG ReadUlong(const EVENT_RECORD* record, ULONG offset) {
    ULONG value;
    memcpy(&value, (const BYTE*)record->UserData + offset, sizeof(value));
    return value;
}

} // namespace

EtwMonitor::EtwMonitor()
    : m_sessionHandle(0),
      m_consumerHandle(INVALID_PROCESSTRACE_HANDLE),
      m_consumerThread(NULL),
      m_properties(NULL),
      m_startError(ERROR_SUCCESS),
      m_counters(NULL),
      m_counterCount(0),
      m_droppedEvents(0),
      m_threadOwners(NULL),
      m_output(NULL) {
    InitializeSRWLock(&m_lock);
    m_lastDrainQpc.QuadPart = 0;
}

EtwMonitor::~EtwMonitor() {
    Stop();
}

EVENT_TRACE_PROPERTIES* EtwMonitor::PrepareProperties() {
    const ULONG size = (ULONG)(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName));
    ZeroMemory(m_properties, size);
    EVENT_TRACE_PROPERTIES* properties = (EVENT_TRACE_PROPERTIES*)m_properties;
    properties->Wnode.BufferSize = size;
    properties->Wnode.Guid = kSessionGuid;
    properties->Wnode.ClientContext = 1; // QueryPerformanceCounter timestamps
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties->EnableFlags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_NO_SYSCONFIG;
    properties->BufferSize = 64;      // KB
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 32;
    properties->FlushTimer = 1;       // seconds; bounds how late events reach the consumer
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return properties;
}

// A session outlives a crashed CoreGaze, so a leftover one with the same name is stopped first.
void EtwMonitor::StopSessionByName() {
    EVENT_TRACE_PROPERTIES* properties = PrepareProperties();
    ControlTraceW(0, kSessionName, properties, EVENT_TRACE_CONTROL_STOP);
}

bool EtwMonitor::Start() {
    if (IsRunning()) {
        return true;
    }

    m_startError = ERROR_SUCCESS;
    if (m_properties == NULL) {
        m_properties = (BYTE*)malloc(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName));
    }
    if (m_counters == NULL) {
        m_counters = (Counter*)malloc(kCounterSlots * sizeof(Counter));
    }
    if (m_threadOwners == NULL) {
        m_threadOwners = (ThreadOwner*)malloc(kThreadOwnerSlots * sizeof(ThreadOwner));
    }
    if (m_output == NULL) {
        m_output = (ProcessIoUsage*)malloc(kCounterSlots * sizeof(ProcessIoUsage));
    }
    if (m_properties == NULL || m_counters == NULL || m_threadOwners == NULL || m_output == NULL) {
        m_startError = ERROR_NOT_ENOUGH_MEMORY;
        ReleaseBuffers();
        return false;
    }
    for (UINT i = 0; i < kCounterSlots; ++i) {
        m_counters[i].pid = kEmptyPid;
    }
    m_counterCount = 0;
    ZeroMemory(m_threadOwners, kThreadOwnerSlots * sizeof(ThreadOwner));
    m_droppedEvents = 0;

    ULONG status = StartTraceW(&m_sessionHandle, kSessionName, PrepareProperties());
    if (status == ERROR_ALREADY_EXISTS) {
        StopSessionByName();
        status = StartTraceW(&m_sessionHandle, kSessionName, PrepareProperties());
    }
    if (status != ERROR_SUCCESS) {
        m_startError = status;
        m_sessionHandle = 0;
        ReleaseBuffers();
        return false;
    }

    EVENT_TRACE_LOGFILEW logFile;
    ZeroMemory(&logFile, sizeof(logFile));
    logFile.LoggerName = (LPWSTR)kSessionName;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EventRecordCallback;
    logFile.Context = this;
    m_consumerHandle = OpenTraceW(&logFile);
    if (m_consumerHandle == INVALID_PROCESSTRACE_HANDLE) {
        m_startError = GetLastError();
        ControlTraceW(m_sessionHandle, NULL, PrepareProperties(), EVENT_TRACE_CONTROL_STOP);
        m_sessionHandle = 0;
        ReleaseBuffers();
        return false;
    }

    QueryPerformanceCounter(&m_lastDrainQpc);
    m_consumerThread = CreateThread(NULL, 0, ConsumerThreadProc, this, 0, NULL);
    if (m_consumerThread == NULL) {
        m_startError = GetLastError();
        CloseTrace(m_consumerHandle);
        m_consumerHandle = INVALID_PROCESSTRACE_HANDLE;
        ControlTraceW(m_sessionHandle, NULL, PrepareProperties(), EVENT_TRACE_CONTROL_STOP);
        m_sessionHandle = 0;
        ReleaseBuffers();
        return false;
    }
    return true;
}

void EtwMonitor::Stop() {
    if (m_sessionHandle != 0) {
        ControlTraceW(m_sessionHandle, NULL, PrepareProperties(), EVENT_TRACE_CONTROL_STOP);
        m_sessionHandle = 0;
    }
    if (m_consumerHandle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(m_consumerHandle);
        m_consumerHandle = INVALID_PROCESSTRACE_HANDLE;
    }
    if (m_consumerThread != NULL) {
        WaitForSingleObject(m_consumerThread, 5000);
        CloseHandle(m_consumerThread);
        m_consumerThread = NULL;
    }
    ReleaseBuffers();
}

void EtwMonitor::ReleaseBuffers() {
    free(m_counters);
    free(m_threadOwners);
    free(m_output);
    free(m_properties);
    m_counters = NULL;
    m_threadOwners = NULL;
    m_output = NULL;
    m_properties = NULL;
    m_counterCount = 0;
}

ULONG EtwMonitor::GetLostEventCount() const {
    if (!IsRunning()) {
        return 0;
    }
    BYTE buffer[sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName)];
    ZeroMemory(buffer, sizeof(buffer));
    EVENT_TRACE_PROPERTIES* properties = (EVENT_TRACE_PROPERTIES*)buffer;
    properties->Wnode.BufferSize = sizeof(buffer);
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    if (ControlTraceW(m_sessionHandle, NULL, properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_SUCCESS) {
        return 0;
    }
    return properties->EventsLost + properties->RealTimeBuffersLost;
}

UINT EtwMonitor::Drain(const ProcessIoUsage** outEntries) {
    if (outEntries != NULL) {
        *outEntries = m_output;
    }
    if (!IsRunning() || m_counters == NULL) {
        return 0;
    }

    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double elapsedSeconds = (double)(now.QuadPart - m_lastDrainQpc.QuadPart) / (double)frequency.QuadPart;
    m_lastDrainQpc = now;
    const double scale = (elapsedSeconds > 0.0) ? 1.0 / elapsedSeconds : 0.0;

    UINT count = 0;
    AcquireSRWLockExclusive(&m_lock);
    if (m_counterCount > 0) {
        for (UINT i = 0; i < kCounterSlots; ++i) {
            Counter& counter = m_counters[i];
            if (counter.pid == kEmptyPid) {
                continue;
            }
            ProcessIoUsage& usage = m_output[count++];
            usage.pid = counter.pid;
            usage.diskReadBytesPerSec = (float)((double)counter.diskReadBytes * scale);
            usage.diskWriteBytesPerSec = (float)((double)counter.diskWriteBytes * scale);
            usage.networkSendBytesPerSec = (float)((double)counter.networkSendBytes * scale);
            usage.networkReceiveBytesPerSec = (float)((double)counter.networkReceiveBytes * scale);
            counter.pid = kEmptyPid;
        }
        m_counterCount = 0;
    }
    ReleaseSRWLockExclusive(&m_lock);
    return count;
}

DWORD WINAPI EtwMonitor::ConsumerThreadProc(LPVOID parameter) {
    EtwMonitor* monitor = (EtwMonitor*)parameter;
    // Returns once the session is stopped or the consumer handle is closed.
    ProcessTrace(&monitor->m_consumerHandle, 1, NULL, NULL);
    return 0;
}

VOID WINAPI EtwMonitor::EventRecordCallback(PEVENT_RECORD record) {
    EtwMonitor* monitor = (EtwMonitor*)record->UserContext;
    if (monitor != NULL) {
        monitor->OnEvent(record);
    }
}

void EtwMonitor::OnEvent(const EVENT_RECORD* record) {
    const GUID& provider = record->EventHeader.ProviderId;
    const UCHAR opcode = record->EventHeader.EventDescriptor.Opcode;

    if (IsEqualGUID(provider, kDiskIoGuid)) {
        if ((opcode != kOpcodeDiskRead && opcode != kOpcodeDiskWrite) ||
            record->UserDataLength < kDiskIssuingThreadIdOffset + sizeof(ULONG)) {
            return;
        }
        const DWORD pid = ResolveThreadProcessId(ReadUlong(record, kDiskIssuingThreadIdOffset));
        if (pid != kEmptyPid) {
            AddBytes(pid, (opcode == kOpcodeDiskRead) ? BYTES_DISK_READ : BYTES_DISK_WRITE,
                     ReadUlong(record, kDiskTransferSizeOffset));
        }
        return;
    }

    if (IsEqualGUID(provider, kTcpIpGuid) || IsEqualGUID(provider, kUdpIpGuid)) {
        int kind;
        if (opcode == kOpcodeSendIPv4 || opcode == kOpcodeSendIPv6) {
            kind = BYTES_NETWORK_SEND;
        } else if (opcode == kOpcodeReceiveIPv4 || opcode == kOpcodeReceiveIPv6) {
            kind = BYTES_NETWORK_RECEIVE;
        } else {
            return;
        }
        if (record->UserDataLength < kNetworkSizeOffset + sizeof(ULONG)) {
            return;
        }
        AddBytes(ReadUlong(record, kNetworkPidOffset), kind, ReadUlong(record, kNetworkSizeOffset));
    }
}

void EtwMonitor::AddBytes(DWORD pid, int kind, ULONG bytes) {
    if (pid == kEmptyPid || bytes == 0) {
        return;
    }

    AcquireSRWLockExclusive(&m_lock);
    const UINT mask = kCounterSlots - 1;
    UINT slot = HashId(pid) & mask;
    Counter* counter = NULL;
    for (UINT probe = 0; probe < kCounterSlots; ++probe, slot = (slot + 1) & mask) {
        Counter& candidate = m_counters[slot];
        if (candidate.pid == pid) {
            counter = &candidate;
            break;
        }
        if (candidate.pid == kEmptyPid) {
            // Keep the table at most 3/4 full so probes stay short; beyond that, drop the event.
            if (m_counterCount < (kCounterSlots / 4) * 3) {
                candidate.pid = pid;
                candidate.diskReadBytes = 0;
                candidate.diskWriteBytes = 0;
                candidate.networkSendBytes = 0;
                candidate.networkReceiveBytes = 0;
                ++m_counterCount;
                counter = &candidate;
            }
            break;
        }
    }

    if (counter == NULL) {
        m_droppedEvents = m_droppedEvents + 1; // under m_lock; volatile only for the UI read
    } else if (kind == BYTES_DISK_READ) {
        counter->diskReadBytes += bytes;
    } else if (kind == BYTES_DISK_WRITE) {
        counter->diskWriteBytes += bytes;
    } else if (kind == BYTES_NETWORK_SEND) {
        counter->networkSendBytes += bytes;
    } else {
        counter->networkReceiveBytes += bytes;
    }
    ReleaseSRWLockExclusive(&m_lock);
}

// Maps a thread id to its process with a small cache; a miss costs one OpenThread call. Entries
// expire so a reused thread id is not charged to the wrong process for long.
DWORD EtwMonitor::ResolveThreadProcessId(DWORD tid) {
    if (tid == 0 || m_threadOwners == NULL) {
        return kEmptyPid;
    }

    const ULONGLONG now = GetTickCount64();
    const UINT mask = kThreadOwnerSlots - 1;
    const UINT home = HashId(tid) & mask;
    UINT freeSlot = home;
    bool haveFreeSlot = false;
    for (UINT probe = 0; probe < 8; ++probe) {
        ThreadOwner& entry = m_threadOwners[(home + probe) & mask];
        if (entry.tid == tid && entry.expiresTick > now) {
            return entry.pid;
        }
        if (!haveFreeSlot && (entry.tid == 0 || entry.tid == tid || entry.expiresTick <= now)) {
            freeSlot = (home + probe) & mask;
            haveFreeSlot = true;
        }
    }

    DWORD pid = kEmptyPid;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (thread != NULL) {
        const DWORD ownerPid = GetProcessIdOfThread(thread);
        CloseHandle(thread);
        if (ownerPid != 0) {
            pid = ownerPid;
        }
    }

    // A full probe window overwrites the home slot.
    ThreadOwner& entry = m_threadOwners[freeSlot];
    entry.tid = tid;
    entry.pid = pid;
    entry.expiresTick = now + kThreadOwnerLifetimeMs;
    return pid;
}
