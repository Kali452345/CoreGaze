# Temperature Metrics

CoreGaze shows CPU, GPU and disk temperatures next to each metric title. It uses only documented, user-mode Windows APIs: no kernel driver, no admin rights, no WMI. Sources that a machine doesn't expose are simply not shown.

## Sources

| Row | Source | Units | Cadence |
| --- | --- | --- | --- |
| CPU | PDH `\Thermal Zone Information(*)\High Precision Temperature` (fallback: `\Temperature`) | tenths of Kelvin (fallback: Kelvin) | 2s |
| GPU | `D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERPERFDATA)` on a per-adapter D3DKMT handle | tenths of °C | 2s |
| Disk | `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceTemperatureProperty` on `\\.\PhysicalDriveN` | °C | 10s |

Temperatures are sampled on their own timers inside `SystemMonitor::PollMetrics`, independent of the tray polling rate. A row's temperature is only polled while that metric is visible and **Show Temperatures** is on.

### CPU (ACPI thermal zones)
- A dedicated PDH query (`m_pdhThermalQuery`) is used so thermal zones never add to the main query's collection cost.
- The hottest zone in the valid range `1-125 °C` is shown. Zones reporting 0 K or absurd values are firmware placeholders and are skipped.
- **Limitation:** this is the ACPI thermal zone, not the per-core sensor that HWiNFO or Core Temp read through a kernel driver. On most laptops it tracks the CPU package closely. On many desktops it is missing, static, or a motherboard sensor. When no zone reports a valid value, the CPU row shows no temperature.

### GPU (D3DKMT adapter perf data)
- MinGW has no `d3dkmthk.h`, so the required structs are declared in `SystemMonitor.cpp`, size-checked with `static_assert`, and `D3DKMTOpenAdapterFromLuid` / `D3DKMTQueryAdapterInfo` / `D3DKMTCloseAdapter` are resolved from `gdi32.dll` at runtime.
- A handle is opened per DXGI adapter LUID in `InitializeGpuMonitoring` (software adapters are skipped) and closed in the destructor or on re-initialization.
- Warning/critical thresholds come from `KMTQAITYPE_ADAPTERPERFDATA_CAPS` (`TemperatureWarning`, `TemperatureMax`) when the driver reports them; otherwise `80 °C` / `90 °C`.
- A driver that rejects the query is not queried again. A reading of `0` means "no sensor" (most integrated GPUs, for example Intel UHD) or a powered-down dedicated GPU, so it is retried on the next cycle. This is the same data source Task Manager uses for GPU temperature. Whether querying it wakes a sleeping dedicated GPU on hybrid laptops has not been verified.
- Aggregate mode shows the hottest adapter together with that adapter's own thresholds.

### Disk (storage temperature property)
- Physical disk numbers come from `IOCTL_STORAGE_GET_DEVICE_NUMBER` on `\\.\X:`, which works for NVMe volumes where the old `QueryDosDevice` parse failed.
- Handles are opened with access `0` (no elevation required), then closed after each read, so the app never blocks safe removal of an external drive.
- Drives that report a seek penalty (spinning HDDs) are skipped, because a SMART read can spin up a sleeping disk.
- A drive that has never answered (USB bridges, card readers, RAID, some SATA controllers) is not queried again until the next disk topology rebuild. A drive that answered before and then fails once gets another try.
- Several volumes on one physical disk share one reading per cycle.
- The reads run on a thread-pool thread (`PollDiskTemperatures` submits, `ApplyDiskTemperatureResults` applies on a later poll), never on the UI thread.
  - Some drives take about 2 s to reject the query. On the development machine, two USB/SATA drives did.
  - Read synchronously, that froze the HUD, and delayed the first process-window sample at startup, by 2 s at every topology rebuild: startup, unlock, resume, display on.
  - Readings therefore appear up to one poll after the read finishes, about 2 s after start.
  - At exit, the destructor waits for a read in progress.
- Thresholds use the drive's own `WarningTemperature` / `CriticalTemperature` (NVMe WCTEMP/CCTEMP) when valid; otherwise `70 °C` / `80 °C`.

## HUD
- The temperature is drawn on the title line, right-aligned to the bar edge (`DrawMetricTitle` in `main.cpp`). A title too long to fit beside it (for example the full CPU brand string) is shortened with `...` (`FitTextToWidth`, cut on UTF-8 boundaries), so every temperature lines up and the HUD never widens. With temperatures off, titles are drawn in full.
- Colors: grey below the warning threshold, orange (`1.0, 0.55, 0.0`) at warning, red (`1.0, 0.15, 0.15`) at critical. These match the utilization threshold colors in `ui_thresholds.md`.
- CPU thresholds are fixed at `85 °C` warning and `95 °C` critical, because thermal zones don't report limits.
- Values are rounded to whole degrees and shown as °C or °F.

## Settings
Tray: **Temperatures** submenu
- **Show Temperatures** (default on). Turning it off also stops all temperature polling (`SystemMonitor::SetTemperaturesEnabled`).
- **Celsius (°C)** / **Fahrenheit (°F)**. Conversion is display-only; the monitor always works in °C.

Persisted in `config.ini`:
```ini
[Temperature]
Show=1
Fahrenheit=0
```

## Verified Hardware
Measured on an i5-8350U laptop (Windows 11, no elevation):
- CPU thermal zone `\_tz.thm0`: ~75-84 °C, reported with high precision.
- UMIS NVMe SSD: 40 °C (warning 75, critical 85).
- Netac USB SSD and the Realtek PCIe card reader: no temperature. They are marked unsupported after the first attempt.
- Intel UHD 620: the perf data query succeeds but reports 0, so no GPU temperature is shown.
