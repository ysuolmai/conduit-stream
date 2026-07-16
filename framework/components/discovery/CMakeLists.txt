# discovery: advertise the AirPlay (_raop._tcp) service via IDF's managed mdns.
# NOTE: this component is named `discovery`, NOT `mdns`, on purpose — a local
# component named `mdns` would collide with (and override) the managed
# `espressif/mdns` component, which also registers under the name `mdns`.
idf_component_register(
    SRCS "src/mdns_service.c"
         "src/raop_txt.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mdns
)
