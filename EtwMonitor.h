#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

// Per-process disk and network rates over the interval since the previous EtwMonitor::Drain().
struct ProcessIoUsage {
    DWORD pid;
    float diskReadBytesPerSec;
    float diskWriteBytesPerSec;
    float networkSendBytesPerSec;
    float networkReceiveBytesPerSec;
};

// Per-process disk and network I/O from a private kernel ETW session ("system logger" mode,
// Windows 8+) with the DiskIo and TcpIp/UdpIp event groups. Windows only allows administrators to
// start kernel sessions, so Start() fails with ERROR_ACCESS_DENIED in a non-elevated CoreGaze and
// the process window then shows only the handle-free "I/O" column.
//
// The session runs only while the process window is open. Events are consumed on a dedicated
// thread and summed per pid into a fixed-size table; the UI thread drains it once per refresh.
class EtwMonitor {
public:
    EtwMonitor();
    ~EtwMonitor();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_consumerThread != NULL; }
    // Win32 error from the last failed Start() (ERROR_ACCESS_DENIED when not elevated).
    DWORD GetStartError() const { return m_startError; }

    // Converts the bytes counted since the previous call into per-second rates and resets the
    // counters. The returned entries stay valid until the next call.
    UINT Drain(const ProcessIoUsage** outEntries);

    // Events dropped because the per-pid table was full, and events ETW itself lost.
    ULONGLONG GetDroppedEventCount() const { return m_droppedEvents; }
    ULONG GetLostEventCount() const;

private:
    struct Counter {
        DWORD pid;               // kEmptyPid when unused
        ULONGLONG diskReadBytes;
        ULONGLONG diskWriteBytes;
        ULONGLONG networkSendBytes;
        ULONGLONG networkReceiveBytes;
    };
    struct ThreadOwner {
        DWORD tid;               // 0 when unused
        DWORD pid;
        ULONGLONG expiresTick;
    };

    static const UINT kCounterSlots = 2048;      // power of two
    static const UINT kThreadOwnerSlots = 4096;  // power of two
    static const DWORD kEmptyPid = 0xFFFFFFFFu;

    TRACEHANDLE m_sessionHandle;
    TRACEHANDLE m_consumerHandle;
    HANDLE m_consumerThread;
    BYTE* m_properties;          // EVENT_TRACE_PROPERTIES + session name
    DWORD m_startError;

    SRWLOCK m_lock;
    Counter* m_counters;
    UINT m_counterCount;
    volatile ULONGLONG m_droppedEvents;

    // Only touched by the consumer thread.
    ThreadOwner* m_threadOwners;

    ProcessIoUsage* m_output;
    LARGE_INTEGER m_lastDrainQpc;

    EVENT_TRACE_PROPERTIES* PrepareProperties();
    void StopSessionByName();
    void OnEvent(const EVENT_RECORD* record);
    void AddBytes(DWORD pid, int kind, ULONG bytes);
    DWORD ResolveThreadProcessId(DWORD tid);
    void ReleaseBuffers();

    static VOID WINAPI EventRecordCallback(PEVENT_RECORD record);
    static DWORD WINAPI ConsumerThreadProc(LPVOID parameter);
};
