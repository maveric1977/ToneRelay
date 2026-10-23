/*
 * Device mode: the names the box shows the network. The default mode is ToneRelay's own. The "helrig" mode gives the
 * box the names a Helrig user expects (Wi-Fi setup network, host name, mDNS instance and service), so someone who bought
 * a Helrig does not have to learn a second product name. The mode is saved in NVS and applied at boot, so changing it
 * restarts the box. Nothing else in the firmware depends on it.
 */
#include "tonerelay.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#define TAG "tr_mode"
#define NVS_NS "tonerelay"
#define NVS_KEY "mode"

static const tonerelay_mode_t MODES[] = {
    /* name        host         instance       service       AP prefix     id in names */
    {"tonerelay", "tonerelay", "ToneRelay", "_tonerelay", "ToneRelay", false},
    {"helrig", "helrigbox", "Helrig Box", "_helrigbox", "HelrigBox", true},
};
#define N_MODES (sizeof(MODES) / sizeof(MODES[0]))

static int s_mode = -1;

static int find(const char *name)
{
    for (size_t i = 0; i < N_MODES; i++) {
        if (strcmp(MODES[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void load(void)
{
    s_mode = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char name[16] = {0};
    size_t n = sizeof(name);
    if (nvs_get_str(h, NVS_KEY, name, &n) == ESP_OK) {
        int m = find(name);
        if (m >= 0) {
            s_mode = m;
        }
    }
    nvs_close(h);
}

const tonerelay_mode_t *tonerelay_mode(void)
{
    if (s_mode < 0) {
        load();
    }
    return &MODES[s_mode];
}

int tonerelay_mode_set(const char *name)
{
    if (!name || find(name) < 0) {
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -2;
    }
    esp_err_t err = nvs_set_str(h, NVS_KEY, name);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return -2;
    }
    ESP_LOGI(TAG, "mode saved: %s (applied at the next boot)", name);
    return 0;
}

void tonerelay_mode_id(char *out, size_t cap)
{
    /* The last three bytes of the chip's base MAC. esp_read_mac(ESP_MAC_WIFI_STA) returned zeros on the P4,
     * where Wi-Fi is on the C6. */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, cap, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

void tonerelay_mode_ap_ssid(char *out, size_t cap)
{
    const tonerelay_mode_t *m = tonerelay_mode();
    if (m->with_id) {
        char id[8];
        tonerelay_mode_id(id, sizeof(id));
        snprintf(out, cap, "%s-%s", m->ap_prefix, id);
    } else {
        snprintf(out, cap, "%s", m->ap_prefix);
    }
}

void tonerelay_mode_instance(char *out, size_t cap)
{
    const tonerelay_mode_t *m = tonerelay_mode();
    if (m->with_id) {
        char id[8];
        tonerelay_mode_id(id, sizeof(id));
        snprintf(out, cap, "%s %s", m->instance, id);
    } else {
        snprintf(out, cap, "%s", m->instance);
    }
}
