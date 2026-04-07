# PDH Integration and Polling Strategy

PDH remains the main source for CPU, GPU engine utilization, and disk activity counters. The implementation is optimized for low overhead with reusable buffers and selective counter registration.

## PDH Counter Coverage

## CPU
- `\Processor(_Total)\% Processor Time`
- `\Processor Information(_Total)\% Processor Performance`

## GPU
- `\GPU Engine(*)\Utilization Percentage`
- Wildcard instances are parsed and filtered by cached adapter LUID patterns.

## Disk
- `\PhysicalDisk(*)\% Disk Time` is used for instance discovery/mapping.
- For each selected drive, monitor registers dedicated counters against resolved instance:
  - `\PhysicalDisk(instance)\% Disk Time`
  - `\PhysicalDisk(instance)\Disk Read Bytes/sec`
  - `\PhysicalDisk(instance)\Disk Write Bytes/sec`

## Selective Registration for Disk
Disk handles are rebuilt only when disk selection changes.
- Selected drives: active counter triplet registered.
- Unselected drives: no handles, no polling.
- Mapping fallback: if resolved instance is unreliable/unavailable, monitor uses `_Total` and marks the row as fallback.

UI formatting note:
- Read/write disk throughput is displayed in adaptive units (`Kbps`/`Mbps`) for readability.

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
