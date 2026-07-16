# discovery

Zero-config discovery so the device shows up on the LAN automatically (part of
the "be discoverable automatically" design goal). Thin wrapper over ESP-IDF's
managed `espressif/mdns` component (pulled in by `idf_component.yml`).

> The component is named `discovery`, **not** `mdns`, on purpose: a local
> component named `mdns` would collide with and override the managed
> `espressif/mdns` component (which also registers under the name `mdns`). The
> public wrapper API still lives in `mdns_service.h`.

## `_raop._tcp` advertise (Phase 1)

`mdns_advertise_raop(hostname, instance_name, port)` inits mDNS, sets the
hostname (e.g. `conduit` -> `conduit.local`), the service instance name
(`<deviceid>@<name>`, e.g. `E83DC1F2AC6C@Conduit`), and advertises `_raop._tcp`
on `port` with the RAOP TXT records (`tp`, `sr`, `ss`, `ch`, `cn`, `et`, `sv`,
`da`, `vn`, `md`) that make the speaker appear in the iOS/macOS AirPlay menu.

The RAOP TXT set is built by the pure, host-tested `src/raop_txt.c` (zero
ESP-IDF deps); `src/mdns_service.c` copies it into IDF's `mdns_txt_item_t` and
is verified by the target build.

**Note:** the advertised RTSP port (`RAOP_RTSP_PORT = 5000`) has **no listener**
until Phase 2 (the RTSP state machine). In Phase 1 the advert alone is what makes
the device discoverable; selecting it in the AirPlay menu won't play yet.
