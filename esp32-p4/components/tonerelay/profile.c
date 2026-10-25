/*
 * The build's identity (see tonerelay_profile_t). The profile itself is a folder chosen at build time (profiles/README.md); this file holds
 * what every profile shares: the box id and the names that carry it.
 */
#include "tonerelay.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"

const tonerelay_profile_t *tonerelay_profile(void)
{
    return &TONERELAY_PROFILE;
}

void tonerelay_profile_id(char *out, size_t cap)
{
    /* The last three bytes of the chip's base MAC. esp_read_mac(ESP_MAC_WIFI_STA) returned zeros on the P4, where Wi-Fi is on the C6. */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, cap, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

void tonerelay_profile_ap_ssid(char *out, size_t cap)
{
    const tonerelay_profile_t *p = tonerelay_profile();
    if (p->with_id) {
        char id[8];
        tonerelay_profile_id(id, sizeof(id));
        snprintf(out, cap, "%s %s", p->ap_prefix, id);
    } else {
        snprintf(out, cap, "%s", p->ap_prefix);
    }
}

void tonerelay_profile_instance(char *out, size_t cap)
{
    const tonerelay_profile_t *p = tonerelay_profile();
    if (p->with_id) {
        char id[8];
        tonerelay_profile_id(id, sizeof(id));
        snprintf(out, cap, "%s %s", p->instance, id);
    } else {
        snprintf(out, cap, "%s", p->instance);
    }
}
