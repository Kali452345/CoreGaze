# UI Overlay Behavior

The HUD is rendered with Dear ImGui on a transparent, click-through DX11 surface. Rendering consumes cached monitor snapshots and does not execute heavy metric collection per frame.

## Interaction Model
- Default state: click-through (`WS_EX_TRANSPARENT`).
- Hold `ALT`: overlay becomes interactable and movable.
- Release `ALT`: click-through restored.
- Host window identity uses CoreGaze branding (`CoreGazeClass`, `CoreGaze`).
- Overlay host bounds are initialized from primary monitor work area (`SPI_GETWORKAREA`) with virtual-screen fallback.
- App process enforces single-instance startup; duplicate launches exit immediately.

## Rendering Principles
- All rows are visibility-gated by tray mask.
- Progress bars are bounded to `[0, 1]` before rendering.
- Row payloads are compact and identity-rich.
- When temperatures are enabled, CPU, disk and GPU titles show a right-aligned, threshold-colored temperature (see `temperature_metrics.md`).

## Metric Rows

## CPU
- Single row: utilization + GHz.

## RAM
- Single row: used/total GB + utilization percent.

## Disk
- One row per selected drive.
- Row payload includes:
  - active percent
  - read throughput (adaptive `KB/s` / `MB/s` / `GB/s`)
  - write throughput (adaptive `KB/s` / `MB/s` / `GB/s`)
  - fallback marker when drive->physical mapping falls back to `_Total`

## Network
- Primary row always shown when network section is enabled.
- Network name is rendered next to the `Network` label (not inside the progress bar payload).
- Primary payload is always shown as spacing-aware RX/TX side-by-side mini bars (`v` for download and `^` for upload).
- Optional secondary row is shown only when enabled and available.
- Network mini bars now use adaptive hysteresis scaling per stream (primary/secondary down/up) instead of a fixed 1000 Mbps denominator, improving visibility for common ranges like 25-200 Mbps while limiting jitter.

## GPU
- Row layout depends on GPU display mode:
  - Targeted: one selected adapter
  - Highest-Load: one dynamic adapter
  - Multi-GPU: one row per adapter
  - Aggregate: one combined row
- Adapter name is rendered next to the `GPU` label.
- Progress payload includes utilization/engine when available, and memory usage.

## Performance Notes
- HUD rendering uses monitor-provided snapshots only.
- Identity refreshes are cadence-limited in backend and not tied to frame loop.
- Hidden categories are not drawn and can be fully disabled in backend polling.
