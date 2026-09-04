# Network Identity and Source Selection

Network telemetry now separates identity/source selection from throughput sampling to keep overhead low and behavior deterministic.

## Identity Refresh
Identity refresh is event-driven.

Refresh sources:
- legacy IP Helper address-change notifications through `NotifyAddrChange`
- WLAN connection notifications through `WlanRegisterNotification`
- throughput failure fallback when the active adapter becomes invalid or disconnects
- a slow periodic safety refresh if notifications are missed

Refresh behavior:
- event callbacks only toggle atomic request flags and return immediately
- the main monitor thread performs the adapter enumeration and display rebuild
- refreshes are debounced so bursts of network events collapse into a single rebuild
- the fallback timer is intentionally long so it does not recreate the old steady polling pattern

## Primary Adapter Selection
Two modes are supported:
- Auto mode prefers the default-route interface through `GetBestRoute`, then falls back to a scored connected-adapter selection.
- Manual mode uses the user-selected interface index from the tray menu.

## Secondary Adapter
Secondary monitoring is disabled by default.

When enabled:
- a manual secondary source can be pinned by interface index
- otherwise the best non-primary connected adapter is used

## Throughput Metrics
Primary and secondary throughput are sampled independently via interface octet deltas:
- download (`rxMbps`)
- upload (`txMbps`)
- total (`rx + tx`)

## SSID Labeling
When the selected adapter is Wi-Fi, the monitor queries WLAN state and uses the SSID as the display label.

SSID query policy:
- reuse the cached SSID when the same adapter is still current and the cache is fresh
- force a new lookup when WLAN notifications indicate a Wi-Fi state change
- avoid repeating the WLAN query on every refresh burst during reconnect churn

Display policy:
- Wi-Fi with SSID: show SSID only
- Connected non-Wi-Fi: show Wired
- Not connected: show Disconnected

Display caveat:
- SSID queries still touch WLAN APIs, so Windows may briefly surface the location indicator when a fresh lookup is required; the event-driven path only reduces how often that happens.

## Tray Display Modes
- Primary Total Mbps
- Primary RX/TX Split
- Primary + Secondary RX/TX

These modes only affect presentation; backend still maintains per-source snapshots.

## Fallback Timer Policy
- The fallback refresh exists only to recover from missed notifications or stale state.
- It is intentionally slow and should not produce regular SSID polling.
- The goal is to keep network identity current without reintroducing a steady WLAN lookup cadence.
