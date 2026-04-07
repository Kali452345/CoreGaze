# GPU Metrics Monitoring

GPU telemetry now supports multi-adapter collection and four display modes while preserving low-overhead update cadence.

## Data Sources
1. DXGI memory (`IDXGIAdapter3::QueryVideoMemoryInfo`)
- local/dedicated usage
- non-local/shared usage

2. PDH utilization (`\GPU Engine(*)\Utilization Percentage`)
- wildcard engine instances
- adapter filtering by cached adapter LUID patterns
- per-engine grouping (`3D`, `Compute`, `Copy`, `Decode`, `Encode`, `VideoProcessing`, `GDI Render`)
- busiest engine becomes adapter utilization label/value

## Adapter Model
At startup, monitor enumerates up to 8 DXGI adapters and caches:
- adapter name
- adapter LUID
- two LUID text patterns for PDH instance matching
- optional `IDXGIAdapter3` pointer for memory queries

Each adapter has its own `GPUMetricsSnapshot`.

## Display Modes
Tray-selectable modes:
1. `Targeted`: one adapter selected by user index.
2. `Highest-Load`: dynamic adapter with highest utilization (memory tie-break).
3. `Multi-GPU`: one HUD row per adapter.
4. `Aggregate`: average utilization across adapters with utilization data, memory totals summed.

## Tray Integration
GPU tray menu now includes:
- `Display Mode` submenu
- `Select GPU Source` submenu (detected adapters)

Both settings are persisted in config and applied live without restart.

## Fallback Behavior
- If utilization is unavailable for an adapter, memory telemetry still renders.
- If memory query is unavailable, adapter row still renders utilization when present.
- If no adapters are available, HUD shows unavailable state without crashing.
