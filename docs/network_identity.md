# Network Identity and Source Selection

Network telemetry now separates identity/source selection from throughput sampling to keep overhead low and behavior deterministic.

## Identity Refresh
Identity refresh is event-driven.

Refresh sources:
- legacy IP Helper address-change notifications through `NotifyAddrChange`
- throughput failure fallback when the active adapter becomes invalid, disconnects, or reconnects
- an adaptive safety timer (see below)

Refresh behavior:
- event callbacks only toggle atomic request flags and return immediately
- the main monitor thread performs the adapter enumeration and display rebuild
- event refreshes are debounced (`500ms`) so bursts of network events collapse into a single rebuild

## Adaptive Fallback Timer
Network List Manager (NLM) often lags behind the address-change event: right after a connect or Wi-Fi switch it can still report `Identifying...` or the previous network's name. The fallback timer therefore has two speeds:

- **Settling (`2s`)**: for `10s` after any event-triggered refresh (and after startup), so the name catches up within 1-2 seconds.
- **Unresolved Wi-Fi (`2s`)**: while the primary adapter is connected Wi-Fi without a resolved name, for up to `60s` after the last event. After that, networks NLM never names (for example `Unidentified network`) stop forcing fast refreshes.
- **Steady state (`30s`)**: otherwise. Each refresh enumerates adapters (`GetAdaptersInfo`) and creates an NLM COM instance, so this keeps idle overhead low.

Constants live at the top of `SystemMonitor.cpp` (`kNetworkIdentity*`); the decision is `SystemMonitor::IsNetworkIdentitySettling`.

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
Primary and secondary throughput are sampled independently via interface octet deltas (`GetIfEntry`):
- download (`rxMbps`)
- upload (`txMbps`)
- total (`rx + tx`)

## Network Name (SSID) Labeling
Network names come from the Network List Manager COM API (`INetworkListManager`), not the WLAN API, so Windows never shows the location-services indicator.

Per-adapter matching:
- `IP_ADAPTER_INFO::AdapterName` is the interface GUID string; it is parsed with `CLSIDFromString` and cached per adapter slot.
- `INetworkListManager::GetNetworkConnections` is enumerated and each connection's `GetAdapterId` is compared with that GUID, so each Wi-Fi adapter only gets the name of its own network. This keeps the label correct when Ethernet and Wi-Fi are both connected, and stops Wi-Fi Direct virtual adapters from inheriting the SSID.
- `Unidentified network` and `Identifying...` are treated as unresolved.
- If the adapter GUID cannot be parsed, the lookup falls back to the first connected network (`GetNetworks(NLM_ENUM_NETWORK_CONNECTED)`).

Display policy:
- Wi-Fi with a resolved name: show the name only
- Wi-Fi without a resolved name: show `Wi-Fi`
- Connected non-Wi-Fi: show `Wired`
- Not connected: show `Disconnected`

## Tray Display Modes
- Primary RX/TX Split
- Primary + Secondary RX/TX

The primary row always renders as side-by-side download/upload bars. The legacy `Primary Total Mbps` mode (config value `0`) was removed from the tray and is normalized to `Primary RX/TX Split` when loaded.

These modes only affect presentation; the backend still maintains per-source snapshots.
