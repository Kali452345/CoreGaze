# Per-Process Disk and Network (ETW)

`EtwMonitor` (`EtwMonitor.h` / `EtwMonitor.cpp`) supplies the process window's **Disk** and **Network** columns. Windows exposes no cheap, non-admin counter for per-process disk or network bytes:
- The `I/O` column (from `NtQuerySystemInformation`) mixes files, pipes, devices and network together.
- The `\Process(*)\IO ...` PDH counters have the same problem.

Task Manager, Resource Monitor and Process Explorer all use kernel ETW for these columns, and so does CoreGaze.

**Administrator rights are required.** Only administrators (or members of "Performance Log Users") can start a kernel trace session. A non-elevated CoreGaze doesn't try; its Disk and Network columns show "-", and the toolbar offers **Restart as administrator**.

## Session
A private **system logger** session (`EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_REAL_TIME_MODE`, Windows 8+) named `CoreGaze Kernel Trace`. It enables only these kernel flags:
- `EVENT_TRACE_FLAG_DISK_IO`: disk read and write completions;
- `EVENT_TRACE_FLAG_NETWORK_TCPIP`: TCP and UDP send and receive;
- `EVENT_TRACE_FLAG_NO_SYSCONFIG`: no hardware configuration dump at start.

It is not the shared "NT Kernel Logger", so it doesn't conflict with other tools (Process Explorer, xperf, ...) that use that one.

- **Buffers:** 64 KB each, 4-32 of them, flushed every second.
- **Consumer:** `OpenTraceW` + `ProcessTrace` on a dedicated thread with `PROCESS_TRACE_MODE_EVENT_RECORD`.
- **Lifetime:** the session exists only while the process window is open.
  - `Start()` runs when the window opens; `Stop()` runs when it closes, or when CoreGaze exits.
  - A session left behind by a crashed instance (`ERROR_ALREADY_EXISTS`) is stopped by name and started again.
- **Lost events:** `GetLostEventCount()` (events lost plus real-time buffers lost) appears in the toolbar tooltip, next to the events dropped because the counter table was full.

## Attribution
| Event | Process | Bytes |
|---|---|---|
| `DiskIo` read / write (opcodes 10 / 11) | Owner of `IssuingThreadId` (payload offset 48 on 64-bit, Windows 8+) | `TransferSize` (offset 8) |
| `TcpIp` / `UdpIp` send / receive, IPv4 (10 / 11) and IPv6 (26 / 27) | `PID` (offset 0) | `size` (offset 4) |

Disk completions run in arbitrary thread contexts, so the event header's process id is not the issuer. The issuing thread is resolved to its process with:
- `OpenThread(THREAD_QUERY_LIMITED_INFORMATION)` + `GetProcessIdOfThread`;
- a 4,096-slot cache (8-probe open addressing, 5 s expiry), so busy threads cost one lookup every 5 s.

Threads that have already exited can't be resolved; their bytes are dropped.

## Counters and Rates
- Events add bytes to a 2,048-slot open-addressing table keyed by PID (kept at most 3/4 full), guarded by an SRW lock.
- `Drain()` runs once per process-window sample. It converts the counts to bytes per second over the time since the previous drain (QPC), then empties the table.
- `ProcessMonitor::ApplyIoUsage()` merges the result into the rows and totals.
- Because buffers flush once per second, rates can trail real activity by up to a second.

## Cost
The consumer thread wakes only when a buffer is delivered, and does a hash-table update per event. Heavy network or disk traffic produces more events: roughly one per send or receive call, and one per disk I/O. The session costs nothing while the process window is closed, because it doesn't exist.

## Testing Status
The ETW path compiles, and the non-elevated fallback ("-" columns, restart button) is verified. The elevated path needs an administrator session to verify:
- the session starts;
- rates are plausible compared with Resource Monitor;
- there are no lost events under load;
- the session is cleaned up on window close and exit.
