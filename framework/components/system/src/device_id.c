#include "device_id.h"
#include <stdio.h>

void device_id_format(const uint8_t mac[6], char out[13]) {
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void device_instance_name_format(const char *device_id, const char *name,
                                 char *out, size_t out_size) {
    snprintf(out, out_size, "%s@%s", device_id, name);
}
