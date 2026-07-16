// Pure-C identity string formatters. No ESP-IDF deps, so host-unit-testable.
// The device id is derived from the base MAC at boot (not stored); the RAOP
// service instance name is "<deviceid>@<name>" (e.g. "E83DC1F2AC6C@Conduit").
#pragma once

#include <stddef.h>
#include <stdint.h>

// A 6-byte MAC -> 12 uppercase hex chars + NUL. `out` must hold >= 13 bytes.
// e.g. {0xE8,0x3D,0xC1,0xF2,0xAC,0x6C} -> "E83DC1F2AC6C".
void device_id_format(const uint8_t mac[6], char out[13]);

// "<device_id>@<name>" into `out` (truncated to out_size, always NUL-terminated).
void device_instance_name_format(const char *device_id, const char *name,
                                 char *out, size_t out_size);
