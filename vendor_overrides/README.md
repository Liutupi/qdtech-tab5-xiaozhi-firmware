# Local IDF component overrides

`78__esp-wifi-connect` is based on `78/esp-wifi-connect` 3.3.1 (MIT license).
The source is pinned through `main/idf_component.yml` so component-manager
refreshes do not discard two local Wi-Fi selection fixes:

- Scan results follow saved SSID priority, using RSSI within each SSID.
- Re-provisioning an existing SSID moves it to the front of the saved list.

When updating the upstream component, compare `wifi_station.cc` and
`ssid_manager.cc` against the registry release and carry these fixes forward.
Do not copy component-manager checksum files or build output into this folder.

`78__esp-ml307` is based on `78/esp-ml307` 3.7.4 (Apache-2.0 license).
It fixes connection task shutdown and concurrent send/close behavior used by
Tab5 WebSocket, HTTP radio and lyric requests. See its `LOCAL_OVERRIDE.md`
for the affected paths and the host regression tests.
