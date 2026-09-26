# PDH Integration and Polling Strategy

PDH remains the main source for CPU, GPU engine utilization, and disk activity counters. The implementation is optimized for low overhead with reusable buffers and selective counter registration.

## PDH Counter Coverage

## CPU
- `\Processor(_Total)\% Processor Time`: busy time over elapsed time across all logical processors. Since KB5064081 (Windows 11 24H2/25H2, August 2025) this is what Task Manager shows on every page. The frequency-scaled `% Processor Utility` it used before can differ by several points either way (measured 16.6% vs 24.0% at low clocks) and is now only in Task Manager's optional "CPU Utility" column.
- `\Processor Information(_Total)\% Processor Performance`: current clock relative to base, used for the GHz readout.

## GPU
- `\GPU Engine(*)\Utilization Percentage`
- Wildcard instances are parsed and filtered by cached adapter LUID patterns.

## Disk
- `\PhysicalDisk(*)\% Disk Time` is used only for instance discovery/mapping.
- For each selected drive, the monitor registers a dedicated counter triplet, preferring the drive's own `LogicalDisk(X:)` instance and falling back to the resolved `PhysicalDisk` instance, then `PhysicalDisk(_Total)`:
  - `% Idle Time`: active time is shown as `100 - % Idle Time`, matching Task Manager. (`% Disk Time` is a queue-length estimate that routinely exceeds 100% on SSDs.)
  - `Disk Read Bytes/sec`
  - `Disk Write Bytes/sec`

## Selective Registration for Disk
Disk handles are rebuilt only when disk selection changes.
- Selected drives: active counter triplet registered.
- Unselected drives: no handles, no polling.
- Mapping fallback: if resolved instance is unreliable/unavailable, monitor uses `_Total` and marks the row as fallback.

UI formatting note:
- Read/write disk throughput is displayed in byte-based adaptive units (`KB/s`/`MB/s`/`GB/s`) for readability.

## Buffer Reuse
Wildcard PDH paths (`GPU Engine(*)`, `PhysicalDisk(*)`) use reusable internal buffers:
- buffer grows only when required by PDH payload size
- no per-tick malloc/free churn in steady state

## Network Throughput Note
Network throughput is no longer sourced from PDH wildcard totals.
- Current implementation computes per-interface RX/TX Mbps using `GetIfEntry` octet deltas.
- This allows deterministic primary/secondary adapter splits and default-route adapter tracking.

## Polling Cadence
All PDH counters follow monitor cadence (`500/1000/2000 ms`) and are skipped when their category is disabled by runtime mask.
