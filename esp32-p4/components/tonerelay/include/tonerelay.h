#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start USB host, SoftAP/STA, HTTP, BLE, and the UART wifi console. */
void tonerelay_start(void);

/*
 * USB host Wire for hx-usb. Claim/release/claim interface 0, posted IN.
 * Return 0 on success.
 * helix_usb_recv: -1 timed out (must print as "read timed out"), -2 not open, -3 USB error.
 */
int helix_usb_start(void);
int helix_usb_open(uint16_t *pid_out);
int helix_usb_close(void);
int helix_usb_send(const uint8_t *data, size_t len, uint32_t timeout_ms);
int helix_usb_recv(uint8_t *buf, size_t buf_len, size_t *out_len, uint32_t timeout_ms);
bool helix_usb_present(void);
bool helix_usb_claimed(void);
uint16_t helix_usb_pid(void);
uint32_t helix_usb_enum_gen(void);
void helix_usb_stats(
    uint32_t *drops,
    uint32_t *in_ok,
    uint32_t *tx_ok,
    int32_t *last_n,
    int32_t *last_st,
    uint32_t *parked,
    uint32_t *queued
);
const char *helix_usb_last_error(void);
/* Product and serial string descriptors of the open pedal (ASCII, '?' for other characters). 0 on success. */
/* The product and serial strings read when the pedal enumerated (valid without a session). */
void helix_usb_cached_strings(char *product, size_t plen, char *serial, size_t slen);
int helix_usb_strings(char *product, size_t plen, char *serial, size_t slen);

/* The names the box shows the network (Wi-Fi setup network, host name, mDNS), chosen by a saved mode. `tonerelay` is the
 * default; `helrig` gives the names a Helrig user expects. Applied at boot, so changing the mode restarts the box. */
typedef struct {
    const char *name;      /* "tonerelay" or "helrig" */
    const char *hostname;  /* mDNS host name, without .local */
    const char *instance;  /* mDNS instance name (the box id is added when with_id) */
    const char *service;   /* mDNS service type, such as "_tonerelay" */
    const char *ap_prefix; /* Wi-Fi setup network name (the box id is added when with_id) */
    bool with_id;
} tonerelay_mode_t;
const tonerelay_mode_t *tonerelay_mode(void);
int tonerelay_mode_set(const char *name); /* 0 saved, -1 unknown mode, -2 could not save */
void tonerelay_mode_id(char *out, size_t cap);
void tonerelay_mode_ap_ssid(char *out, size_t cap);
void tonerelay_mode_instance(char *out, size_t cap);

/* Version of the raw relay contract (docs/raw-relay-protocol.md): sent in the /raw hello and advertised over mDNS. */
#define TONERELAY_RELAY_PROTO 0

void tonerelay_wifi_start(void);
void tonerelay_c6_sync(void);
int tonerelay_wifi_scan_json(char *buf, size_t len);
int tonerelay_wifi_join(const char *ssid, const char *pass);
int tonerelay_wifi_ap_password(const char *pass);
int tonerelay_wifi_status_json(char *buf, size_t len);
int tonerelay_wifi_forget(void);
bool tonerelay_wifi_setup_required(void);
bool tonerelay_wifi_ap_secured(void);
bool tonerelay_wifi_sta_configured(void);
bool tonerelay_wifi_sta_up(void);
bool tonerelay_ble_up(void);

void tonerelay_catalog_start(void);
bool tonerelay_catalog_ready(void);
const char *tonerelay_catalog_name_ok(const char *name);
int tonerelay_catalog_put(const char *name, const uint8_t *data, size_t len);
FILE *tonerelay_catalog_open_put(const char *name);
int tonerelay_catalog_status_json(char *buf, size_t len);

void tonerelay_http_start(void);

/* Raw relay (docs/raw-relay-protocol.md). While relay-only is on, the box opens no session of its own. */
void tonerelay_relay_start(void);
bool tonerelay_relay_only(void);
void tonerelay_ble_start(void);

#ifdef __cplusplus
}
#endif
