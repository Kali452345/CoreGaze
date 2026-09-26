# System Monitor Engine

The `SystemMonitor` component is the native telemetry backend for the overlay. It uses low-level Win32, PDH, DXGI, IP Helper, and Network List Manager APIs with cadence gating so monitoring overhead stays low while still exposing richer per-source metrics.

## Polling Model

## Cadence Separation
- Fast cadence (`500ms`, `1000ms`, `2000ms`): numeric counters (CPU, RAM, GPU utilization/memory, disk active/read/write throughput, network RX/TX rates).
- Disk capacity (used/free GB via `GetDiskFreeSpaceExW`): every `30s`, and immediately on a disk topology rebuild. Free space rarely changes and the call can touch removable media.
- Temperatures: own timers (`2s` CPU/GPU, `10s` disk), only while the metric is visible and temperatures are enabled. See `temperature_metrics.md`.
- Network identity (active network source, SSID labels, adapter lists): event-driven, with an adaptive safety timer (`2s` while settling after a network event, `30s` otherwise). See `network_identity.md`.

This split avoids expensive identity/mapping work in the hot polling path.

## Runtime Gating
The monitor supports runtime control from tray settings:
- `SetEnabledMetricsMask(...)`: collector category enable/disable (CPU, RAM, GPU, Disk, Network).
- `SetPollingIntervalMs(...)`: bounded polling cadence.
- `SetDiskSelectionMask(...)`: polls only selected fixed drives.
- `SetGPUDisplayMode(...)`, `SetSelectedGPUAdapterIndex(...)`: GPU output mode/source selection.
- `SetNetworkPrimaryMode(...)`, `SetNetworkPrimaryIfIndex(...)`: auto/manual primary network source.
- `SetNetworkSecondaryEnabled(...)`, `SetNetworkSecondaryIfIndex(...)`: optional secondary adapter.
- `SetTemperaturesEnabled(...)`: CPU/GPU/disk temperature polling on or off.
- `SetNetworkDisplayMode(...)`: primary split vs primary + secondary display (legacy total mode normalizes to split).

## CPU and RAM Collection
- CPU utilization: `\Processor(_Total)\% Processor Time` via PDH, which is what Task Manager shows since KB5064081 (Windows 11 24H2/25H2).
- CPU GHz: registry base MHz + PDH `\Processor Information(_Total)\% Processor Performance`.
- RAM: `GlobalMemoryStatusEx`.

## GPU Collection

## Adapter Enumeration
- Enumerates up to 8 DXGI adapters and caches:
  - adapter name
  - LUID patterns for PDH GPU Engine instance matching
  - optional `IDXGIAdapter3` for memory queries

## Per-Adapter Metrics
For each detected adapter:
- Utilization from `\GPU Engine(*)\Utilization Percentage` grouped by engine type.
- Memory from `QueryVideoMemoryInfo` (local and non-local segments).

## Mode Output
Mode selection is resolved in monitor helpers:
- `Targeted`: selected adapter index.
- `Highest-Load`: dynamic busiest adapter by utilization (memory tie-break).
- `Multi-GPU`: one snapshot per adapter.
- `Aggregate`: average utilization across adapters with utilization data; memory summed.

## Disk Collection

## Targeted Per-Drive Polling
Disk telemetry is now selected-drive aware:
- Each selected fixed drive gets dedicated PDH counters (`LogicalDisk(X:)` first, `PhysicalDisk` fallback) for:
  - `% Idle Time` (active time = `100 - idle`, as in Task Manager)
  - `Disk Read Bytes/sec`
  - `Disk Write Bytes/sec`
- Unselected drives have no active counter handles and incur no polling cost.

## Drive Mapping Strategy
- First-pass mapping uses `IOCTL_STORAGE_GET_DEVICE_NUMBER` on `\\.\X:` (works for NVMe), falling back to `QueryDosDevice`.
- PDH instance resolution then attempts physical-disk instance matching (index-aware when available, drive-token-aware otherwise).
- If mapping confidence is low, counters fall back to `_Total` and snapshot is marked fallback.

## Network Collection

## Source Selection
- Primary adapter:
  - auto mode uses default-route preference (`GetBestRoute`) with scored fallback
  - manual mode pins by interface index
- Secondary adapter:
  - optional, user-enabled
  - manual pin or best non-primary fallback

## Throughput
- RX/TX split is sampled per selected interface index via `GetIfEntry` delta calculations.
- Primary and secondary snapshots are stored independently.
- Legacy total Mbps is derived from primary (`rx + tx`).

## Identity
- Adapter display names are cached from IP Helper enumeration.
- Wi-Fi network names come from Network List Manager, matched to each adapter by interface GUID (no WLAN API, so no location indicator).

## Allocation Strategy
- Wildcard PDH parsing buffers are reused and grown only when needed.
- No per-frame allocations are introduced in rendering or metric update paths.
- Any dynamic allocation in monitor is outside tight loops (startup, identity refresh, counter rebuild).
