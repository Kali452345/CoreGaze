# Process Monitor (Per-Process Collector)

`ProcessMonitor` (`ProcessMonitor.h` / `ProcessMonitor.cpp`) collects the data for the process window (`docs/process_window.md`). For every process it gathers CPU, memory, I/O, GPU, threads and handles, without opening a handle to any of them. It is the collector only; the table UI is `ProcessWindow`.

## Data Source
Each sample makes one `NtQuerySystemInformation(SystemProcessInformation)` call, resolved from `ntdll.dll` at runtime. The call returns every process, including protected ones, and needs no admin rights. `winternl.h` only exposes part of the `SYSTEM_PROCESS_INFORMATION` layout, so the full layout is declared locally.

Rejected alternatives, measured on the development machine (8 logical processors, about 370 processes, 5,400 threads):

| Source | Cost | Why not |
|---|---|---|
| PDH `\Process(*)\...` | about 15.6 ms CPU per poll for one counter | 4x the cost, and per-instance names need parsing |
| `OpenProcess` + `GetProcessTimes` / `GetProcessMemoryInfo` | one handle per process | a non-elevated process can open only about 60% of processes |
| Toolhelp snapshots | same kernel call underneath | no memory, I/O or CPU times |

## Columns
| Field | Source | Notes |
|---|---|---|
| `cpuPercent` | Δ(user + kernel time) / (Δelapsed × logical processors) | The same formula as Task Manager since KB5064081 |
| `privateWorkingSet` | `WorkingSetPrivateSize` | Task Manager's "Memory" column |
| `workingSet`, `commitBytes` | `WorkingSetSize`, `PrivatePageCount` | Details-page columns |
| `ioBytesPerSec` | Δ(read + write transfer bytes) / Δelapsed | **All** I/O (files, pipes, devices, network), not disk only. True per-process disk and network figures need an elevated ETW session (`docs/etw_disk_network.md`). |
| `diskRead/WriteBytesPerSec`, `networkSend/ReceiveBytesPerSec` | `EtwMonitor`, merged by `ApplyIoUsage()` | Zero unless CoreGaze runs elevated. `ProcessTotals` gets `diskBytesPerSec` and `networkBytesPerSec`. |
| `gpuPercent`, `gpuAdapterIndex`, `gpuEngineType` | `SystemMonitor` per-process GPU tracking | The process's busiest engine, as Task Manager shows it |
| `threadCount`, `handleCount`, `parentPid`, `sessionId`, `createTime` | snapshot | |

- The System Idle Process (pid 0) is not listed. Its time gives the CPU total: `100 - idle share`.
- Each process is matched to its previous sample by pid **and** creation time. A reused pid therefore starts fresh instead of producing a bogus delta.
- The image name is converted to UTF-8 once, when the process first appears. After that it is copied from the previous row.
- The elapsed time is stamped at the middle of the snapshot call, because the kernel reads the process times while it builds the snapshot.

## Why the denominator is elapsed × processors
Interrupt and DPC time is charged to no process, so the sum of all process times (Idle included) falls short of elapsed time × processors. Dividing by that sum made the total read 1-2 points below PDH `% Processor Time`. With elapsed × processors, the total matches PDH within sampling jitter (for example 31.5 vs 31.5 and 26.5 vs 26.9).

## GPU Merge
`SystemMonitor` already reads `\GPU Engine(*)\Utilization Percentage` for the HUD, and every instance name starts with `pid_<pid>_`. `SetProcessGpuTrackingEnabled(true)` makes `PollGpuMetrics` also do the following:
1. Record each non-zero instance as (pid, adapter, engine type, value).
2. Sort the records, sum the values per (pid, adapter, engine type), and keep each pid's maximum.
3. Expose the result, sorted by pid, through `GetProcessGpuUsage()`.

`ProcessMonitor::ApplyGpuUsage()` merges this list into the rows through the pid index. While tracking is on, the engine query runs even if the HUD's GPU row is hidden. Disabling tracking frees the buffers.

## Memory and Allocation
- The snapshot buffer starts at 1 MB. On `STATUS_INFO_LENGTH_MISMATCH` it grows to the required size plus 128 KB of headroom. It never shrinks while the monitor is in use.
- Rows are double-buffered: the current and previous arrays swap each sample. Once the capacity covers the process count (plus 64 rows of slack), a sample allocates nothing.
- The pid index uses open addressing over a power-of-two table that is at most half full. It is built once per sample for the current rows, and the next sample reuses it as the previous-row index.
- `Release()` frees everything. The process window calls it on close, so an idle CoreGaze holds none of this memory.

## Measured Cost
Measured on the development machine at 20-50% load, with Task Manager and browsers running:
- `Sample()` takes about 7-8 ms of CPU per call. Almost all of it is kernel time inside `NtQuerySystemInformation`; the raw call alone measures the same. The parse and delta pass is too small to measure. At an earlier, lighter moment the raw call cost 3.8 ms.
- Per-process GPU tracking adds about 1.2 ms of CPU per poll on top of `SystemMonitor`.
- At a 1-second refresh, that totals under 1% of one core, about 0.1% of the machine. On the same system, Task Manager's steady state measured 25% of one core (3.2% of the machine).
- The collector uses about 2.5 MB of private memory with 380 processes.

The kernel cost scales with the process and thread count and cannot be reduced from user mode. The refresh interval is the only lever.
