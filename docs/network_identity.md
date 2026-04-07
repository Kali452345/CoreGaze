# Network Identity and Source Selection

Network telemetry now separates identity/source selection from throughput sampling to keep overhead low and behavior deterministic.

## Identity Refresh
Identity refresh runs on a slower cadence (about 10 seconds) and updates:
- discovered adapter list
- primary adapter selection
- optional secondary adapter selection
- display names and SSID labels

## Primary Adapter Selection
Two modes are supported:
1. Auto mode:
- prefers default-route interface (`GetBestRoute`)
- falls back to scored selection (connected state, gateway presence, adapter type)

2. Manual mode:
- uses user-selected interface index from tray menu

## Secondary Adapter (Optional)
- Secondary monitoring is disabled by default.
- When enabled:
  - manual secondary source can be pinned by interface index
  - otherwise best non-primary connected adapter is used

## Throughput Metrics
Primary and secondary throughput are sampled independently via interface octet deltas:
- download (`rxMbps`)
- upload (`txMbps`)
- total (`rx + tx`)

## SSID Labeling
When selected adapter is Wi-Fi, monitor queries WLAN connection state and uses SSID as the display label.

Display policy:
- Wi-Fi with SSID: show SSID only.
- Connected non-Wi-Fi: show `Wired`.
- Not connected: show `Disconnected`.

Display format examples:
- `OfficeNet`
- `Wired`

## Tray Display Modes
- `Primary Total Mbps`
- `Primary RX/TX Split`
- `Primary + Secondary RX/TX`

These modes only affect presentation; backend still maintains per-source snapshots.
