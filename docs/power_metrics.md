# CPU Power Metrics

CoreGaze captures live CPU power consumption (Watts) using native Windows interfaces without 3rd-party kernel drivers (e.g. no WinRing0) and without WMI. Sources that a machine or environment doesn't expose degrade gracefully to `-` without errors.

## Data Source: Windows Energy Meter Interface (EMI)

Modern Windows (Windows 10 1809+, Windows 11) includes the Energy Meter Interface (EMI) in the kernel, reading Intel and AMD Running Average Power Limit (RAPL) Model-Specific Registers (MSRs) and exposing them directly via user-mode Performance Data Helper (PDH).

| Interface | Object & Counter | Unit | Cadence |
|---|---|---|---|
| Primary | `\Energy Meter(*)\Power` | milliwatts (`mW`) | 1 s |
| Fallback | `\Power Meter(*)\Power` | milliwatts (`mW`) | 1 s |

Power is sampled on its own 1-second cadence timer inside `SystemMonitor::PollMetrics`, independent of the tray polling rate. Polling is gated on the CPU metric being visible and **Show CPU Power** being enabled.

### Domains & Aggregation
Intel and AMD processors expose power over several RAPL domains as counter instances:

- `RAPL_Package0_PKG` (or `_pkg`): Total CPU package/socket power. On multi-socket systems, all package instances are summed.
- `RAPL_Package0_PP0` (or `_PP0` / `Core`): Power consumed by CPU compute cores (IA).
- `RAPL_Package0_DRAM`: Power consumed by the integrated memory controller and attached DRAM.
- `RAPL_Package0_PP1`: Power consumed by integrated GPU / uncore.

If no `_PKG` counter instance exists, CoreGaze falls back to summing all active Energy Meter counters, or querying the ACPI platform `\Power Meter(*)\Power` counter.

### Polling Cost & Zero-Allocation Buffer
- Queries run on a dedicated PDH query handle (`m_pdhPowerQuery`).
- Instance data is retrieved via `PdhGetRawCounterArrayW` into double-buffered, grow-only memory (`m_pdhPowerBuffer`). In steady-state operation, power collection incurs **0 heap allocations** and consumes less than 0.05 ms of CPU per sample.

---

## Desktop HUD Overlay

- **Progress Bar Readout**: When CPU power is available and enabled, the CPU row displays:
  `XX.X% @ X.XX GHz (XX.X W)`
- **Hover Breakdown Tooltip**: Hovering over the CPU progress bar opens an interactive tooltip detailing domain power:
  - `CPU Package Power: XX.XX W`
  - `Cores (IA): XX.XX W` (when exposed by hardware)
  - `Memory (DRAM): XX.XX W` (when exposed by hardware)

---

## Process Window Telemetry

### Column: Power
- Placed directly next to the **CPU** column (`COL_POWER`, 68 px default width).
- Formats:
  - `<0.1 W` for active sub-decawatt usage
  - `%.1f W` for active usage
  - `-` for zero or unavailable usage
- **Sorting**: Fully sortable (descending first for numeric power). Group rows sum member power; expanding a group displays per-process child power.
- **Heat Tint**: Amber heat-map tinting matching Task Manager styling (`HeatColor(power / 15.0f)`).

### Proportional Power Allocation Formula
Because user-mode processes cannot directly read hardware MSR energy accounting for arbitrary thread context switches without kernel instrumentation, individual process wattage is calculated proportionally from total package power weighted by active CPU share:

$$P_{\text{proc}} = P_{\text{package}} \times \frac{\text{CPU}\%_{\text{proc}}}{\text{CPU}\%_{\text{total}}}$$

When $\text{CPU}\%_{\text{total}} < 0.1\%$, process wattage defaults to 0.

### Totals Row & Status Bar
- **Totals Row**: Displays total active package wattage (`%.1f W`) beneath the Power column header.
- **Status Bar**: Summarizes system-wide processor state: `CPU XX% (XX.X W)`.

---

## Settings & Persistence

Tray Menu: **CPU** submenu
- **Show CPU Power** (default on). Toggling this off stops the Energy Meter PDH collection in `SystemMonitor` and omits power figures from the HUD progress bar.

Persisted in `%APPDATA%\CoreGaze\config.ini`:
```ini
[CPU]
ShowPower=1
```
