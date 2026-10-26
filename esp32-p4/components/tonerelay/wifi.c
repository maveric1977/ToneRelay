#include "tonerelay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "tr_wifi"
#define NVS_NS "tonerelay"
#define MAX_NETS 3
#define AP_PASS_MIN 8
#define AP_PASS_MAX 63
#define RETRY_CAP_US (30ULL * 1000000ULL)

static bool s_sta_up;
static bool s_sta_configured;
static bool s_ap_secured;
static char s_sta_ip[16];
static char s_sta_ssid[33];
static int s_sta_rssi;
static int s_retry;
static esp_timer_handle_t s_sta_retry_timer;
static esp_timer_handle_t s_ap_timer;
static bool s_ap_on;
static bool s_choosing; /* a scan is running to pick which saved network to join */
static bool s_leaving; /* the box let go of its network on purpose: that disconnect is not a failed attempt */
static char s_skip[33]; /* the network whose last attempt failed: tried again only when nothing else is in range */
/* For a profile whose setup network is only for setup (setup_ap_only): how long after the box has joined a network the setup
 * network goes off (time for the setup page to say so), and how long a box without Wi-Fi waits before it comes back. */
#define AP_OFF_DELAY_US (45ULL * 1000000ULL)
#define AP_BACK_DELAY_US (40ULL * 1000000ULL)

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= dst_len) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = 0;
}

/* Up to MAX_NETS networks are kept, the one joined last first. Slot 0 is the `ssid`/`pass` pair an older firmware wrote, so a box that
 * is updated keeps its network; the others are `ssid1`/`pass1` and so on. */
typedef struct {
    char ssid[33];
    char pass[65];
} net_t;

static void slot_keys(int i, char *ks, char *kp, size_t cap)
{
    if (i == 0) {
        snprintf(ks, cap, "ssid");
        snprintf(kp, cap, "pass");
    } else {
        snprintf(ks, cap, "ssid%d", i);
        snprintf(kp, cap, "pass%d", i);
    }
}

/* Reads the saved networks into nets (MAX_NETS entries) and returns how many there are. */
static int nvs_load_nets(net_t *nets)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        char ks[8], kp[8];
        slot_keys(i, ks, kp, sizeof(ks));
        net_t t = {0};
        size_t sl = sizeof(t.ssid);
        size_t pl = sizeof(t.pass);
        if (nvs_get_str(h, ks, t.ssid, &sl) == ESP_OK && t.ssid[0] != 0 && nvs_get_str(h, kp, t.pass, &pl) == ESP_OK) {
            nets[n++] = t;
        }
    }
    nvs_close(h);
    return n;
}

/* Writes nets[0..n) to slots 0..n) and clears the rest. */
static esp_err_t nvs_store_nets(const net_t *nets, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < MAX_NETS && err == ESP_OK; i++) {
        char ks[8], kp[8];
        slot_keys(i, ks, kp, sizeof(ks));
        if (i < n) {
            err = nvs_set_str(h, ks, nets[i].ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(h, kp, nets[i].pass);
            }
        } else {
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool nvs_load_ap_pass(char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t pl = pass_len;
    esp_err_t err = nvs_get_str(h, "ap_pass", pass, &pl);
    nvs_close(h);
    return err == ESP_OK && pass[0] != 0;
}

/* Saves a network as the newest. A name that is already saved takes the new password and moves to the front; when the list is full the
 * one joined longest ago is dropped. */
static esp_err_t nvs_save_sta(const char *ssid, const char *pass)
{
    net_t old[MAX_NETS];
    int n = nvs_load_nets(old);
    net_t all[MAX_NETS];
    int m = 0;
    snprintf(all[m].ssid, sizeof(all[m].ssid), "%s", ssid);
    snprintf(all[m].pass, sizeof(all[m].pass), "%s", pass ? pass : "");
    m++;
    for (int i = 0; i < n && m < MAX_NETS; i++) {
        if (strcmp(old[i].ssid, ssid) != 0) {
            all[m++] = old[i];
        }
    }
    esp_err_t err = nvs_store_nets(all, m);
    memset(old, 0, sizeof(old));
    memset(all, 0, sizeof(all));
    return err;
}

/* The network that just got an address becomes the first one tried next time. */
static void nvs_promote(const char *ssid)
{
    net_t nets[MAX_NETS];
    int n = nvs_load_nets(nets);
    if (n < 2 || strcmp(nets[0].ssid, ssid) == 0) {
        memset(nets, 0, sizeof(nets));
        return;
    }
    for (int i = 1; i < n; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) {
            net_t t = nets[i];
            for (int j = i; j > 0; j--) {
                nets[j] = nets[j - 1];
            }
            nets[0] = t;
            nvs_store_nets(nets, n);
            break;
        }
    }
    memset(nets, 0, sizeof(nets));
}

static esp_err_t nvs_save_ap_pass(const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ap_pass", pass);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void nvs_erase_sta(void)
{
    nvs_store_nets(NULL, 0);
}

static void start_mdns(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed");
        return;
    }
    const tonerelay_profile_t *profile = tonerelay_profile();
    char instance[40];
    tonerelay_profile_instance(instance, sizeof(instance));
    mdns_hostname_set(profile->hostname);
    mdns_instance_name_set(instance);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    /* Lets a client find a box by service type. `id` is the last three bytes of the chip's base MAC. */
    char id[8];
    tonerelay_profile_id(id, sizeof(id));
    char proto[8];
    snprintf(proto, sizeof(proto), "%d", TONERELAY_RELAY_PROTO);
    mdns_txt_item_t txt[] = {{"proto", proto}, {"path", "/raw"}, {"id", id}, {"profile", profile->name}};
    size_t n_txt = sizeof(txt) / sizeof(txt[0]);
    /* The contract's own service type is always advertised; a profile with names of its own adds its type. */
    mdns_service_add(NULL, "_tonerelay", "_tcp", 80, txt, n_txt);
    if (strcmp(profile->service, "_tonerelay") != 0) {
        mdns_service_add(NULL, profile->service, "_tcp", 80, txt, n_txt);
    }
    started = true;
    ESP_LOGI(TAG, "mDNS %s.local (%s)", profile->hostname, instance);
}

static void start_ap(void)
{
    wifi_config_t ap = {0};
    char ap_ssid[33];
    tonerelay_profile_ap_ssid(ap_ssid, sizeof(ap_ssid));
    strncpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.pmf_cfg.required = false;
    char ap_pass[64] = {0};
    if (nvs_load_ap_pass(ap_pass, sizeof(ap_pass)) && strlen(ap_pass) >= AP_PASS_MIN) {
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password) - 1);
        s_ap_secured = true;
        ESP_LOGI(TAG, "SoftAP %s WPA2 at 192.168.4.1", ap_ssid);
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
        s_ap_secured = false;
        ESP_LOGI(TAG, "SoftAP %s open at 192.168.4.1", ap_ssid);
    }
    memset(ap_pass, 0, sizeof(ap_pass));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_on = true;
}

static void ap_timer_cb(void *arg)
{
    (void)arg;
    if (!tonerelay_profile()->setup_ap_only) {
        return;
    }
    if (s_sta_up && s_ap_on) {
        ESP_LOGI(TAG, "joined a network: setup network off");
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            s_ap_on = false;
        }
    } else if (!s_sta_up && !s_ap_on) {
        ESP_LOGW(TAG, "no Wi-Fi: setup network back on");
        start_ap();
    }
}

static void ap_schedule(uint64_t us)
{
    if (!s_ap_timer || !tonerelay_profile()->setup_ap_only) {
        return;
    }
    esp_timer_stop(s_ap_timer);
    esp_timer_start_once(s_ap_timer, us);
}

/* Arm the setup-network timer once: every failed attempt lands here, and restarting it each time (the retries are 2 to 30 s apart) would
 * push the setup network back for ever. */
static void arm_ap_fallback(void)
{
    if (!s_ap_on && s_ap_timer && !esp_timer_is_active(s_ap_timer)) {
        ap_schedule(AP_BACK_DELAY_US);
    }
}

static void start_sta(const char *ssid, const char *pass, bool fresh)
{
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    if (fresh) {
        s_retry = 0;
    }
    ESP_LOGI(TAG, "STA joining ssid=%s", ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void schedule_sta_retry(void);

/* Choosing a network reads flash and a list of scan results, which does not fit the stacks of the event loop and of the timer task, so
 * those only post a request and this task does the work. */
enum { REQ_JOIN_FRESH, REQ_JOIN_RETRY, REQ_SCAN_DONE, REQ_PROMOTE };
static QueueHandle_t s_req_q;

/* With one saved network the box just joins it. With several it looks first, so a network that is not here is not tried (a failed attempt
 * takes seconds), and joins the one joined last if it is here, else the strongest of the others. */
static void join_saved(bool fresh)
{
    net_t nets[MAX_NETS];
    int n = nvs_load_nets(nets);
    if (n == 0) {
        return;
    }
    s_sta_configured = true;
    if (n == 1) {
        start_sta(nets[0].ssid, nets[0].pass, fresh);
    } else {
        wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
        s_choosing = true;
        if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
            /* A scan is already running (the setup page asked for one): do not wait for it. */
            s_choosing = false;
            start_sta(nets[0].ssid, nets[0].pass, fresh);
        }
    }
    memset(nets, 0, sizeof(nets));
}

static void on_scan_done(void)
{
    s_choosing = false;
    net_t nets[MAX_NETS];
    int n = nvs_load_nets(nets);
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > 24) {
        found = 24;
    }
    wifi_ap_record_t *recs = calloc(24, sizeof(*recs));
    uint16_t got = found;
    if (!recs || n == 0 || esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        got = 0;
    }
    int pick = -1;
    int best_rssi = -128;
    int skipped = -1; /* the network that just failed, if it is in range: the last resort */
    for (int i = 0; i < n; i++) {
        int rssi = 0;
        bool here = false;
        for (uint16_t k = 0; k < got; k++) {
            if (strcmp((const char *)recs[k].ssid, nets[i].ssid) == 0) {
                here = true;
                rssi = recs[k].rssi;
                break;
            }
        }
        if (!here) {
            continue;
        }
        if (s_skip[0] && strcmp(s_skip, nets[i].ssid) == 0) {
            skipped = i;
        } else if (pick < 0 || (pick != 0 && (i == 0 || rssi > best_rssi))) {
            /* the one joined last wins whenever it is here, else the strongest */
            pick = i;
            best_rssi = rssi;
        }
    }
    if (pick < 0) {
        pick = skipped;
    }
    if (pick >= 0) {
        ESP_LOGI(TAG, "%d saved networks: joining %s", n, nets[pick].ssid);
        start_sta(nets[pick].ssid, nets[pick].pass, false);
    } else if (n > 0) {
        ESP_LOGW(TAG, "none of the %d saved networks is in range", n);
        schedule_sta_retry();
        arm_ap_fallback();
    }
    free(recs);
    memset(nets, 0, sizeof(nets));
}

static void wifi_worker(void *arg)
{
    (void)arg;
    uint8_t req;
    while (xQueueReceive(s_req_q, &req, portMAX_DELAY) == pdTRUE) {
        switch (req) {
        case REQ_JOIN_FRESH:
            join_saved(true);
            break;
        case REQ_JOIN_RETRY:
            if (s_sta_configured && !s_sta_up && !s_choosing) {
                join_saved(false);
            }
            break;
        case REQ_SCAN_DONE:
            if (s_choosing) {
                on_scan_done();
            }
            break;
        case REQ_PROMOTE:
            nvs_promote(s_sta_ssid);
            break;
        }
    }
}

static void post_req(uint8_t req)
{
    if (s_req_q) {
        xQueueSend(s_req_q, &req, 0);
    }
}

static void sta_retry_cb(void *arg)
{
    (void)arg;
    if (s_sta_configured && !s_sta_up) {
        ESP_LOGW(TAG, "STA retry %d", s_retry);
        post_req(REQ_JOIN_RETRY);
    }
}

static uint64_t retry_delay_us(int attempt)
{
    uint64_t sec = 1ULL << (attempt > 8 ? 8 : attempt);
    if (sec > 30) {
        sec = 30;
    }
    return sec * 1000000ULL;
}

static void schedule_sta_retry(void)
{
    if (!s_sta_retry_timer || !s_sta_configured) {
        return;
    }
    s_retry++;
    uint64_t us = retry_delay_us(s_retry);
    if (us > RETRY_CAP_US) {
        us = RETRY_CAP_US;
    }
    esp_timer_stop(s_sta_retry_timer);
    esp_timer_start_once(s_sta_retry_timer, us);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        post_req(REQ_JOIN_FRESH);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_choosing) {
            post_req(REQ_SCAN_DONE);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_sta_up && !s_leaving) {
            /* An attempt that did not get an address. */
            strncpy(s_skip, s_sta_ssid, sizeof(s_skip) - 1);
        }
        s_leaving = false;
        s_sta_up = false;
        s_sta_ip[0] = 0;
        schedule_sta_retry();
        arm_ap_fallback();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_up = true;
        s_retry = 0;
        if (s_sta_retry_timer) {
            esp_timer_stop(s_sta_retry_timer);
        }
        ESP_LOGI(TAG, "STA ip=%s", s_sta_ip);
        s_skip[0] = 0;
        post_req(REQ_PROMOTE);
        if (s_ap_on) {
            ap_schedule(AP_OFF_DELAY_US);
        }
    }
}

void tonerelay_wifi_start(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = sta_retry_cb,
        .name = "sta_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sta_retry_timer));
    const esp_timer_create_args_t ap_timer_args = {
        .callback = ap_timer_cb,
        .name = "ap_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ap_timer_args, &s_ap_timer));

    s_req_q = xQueueCreate(8, sizeof(uint8_t));
    ESP_ERROR_CHECK(s_req_q ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(wifi_worker, "wifi_worker", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    net_t nets[MAX_NETS];
    s_sta_configured = nvs_load_nets(nets) > 0;
    if (s_sta_configured) {
        strncpy(s_sta_ssid, nets[0].ssid, sizeof(s_sta_ssid) - 1);
    }
    memset(nets, 0, sizeof(nets));
    if (tonerelay_profile()->setup_ap_only && s_sta_configured) {
        /* A box that has a network starts as a station only; its setup network comes up only if that network does not. */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_ap_on = false;
        ap_schedule(AP_BACK_DELAY_US);
    } else {
        start_ap();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* The default (modem sleep) puts the radio to sleep between beacons: replies after an idle gap come late, and group
     * (multicast) frames, such as an mDNS question to everyone, can be missed. The box has mains power. */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "could not turn Wi-Fi power-save off: %s", esp_err_to_name(ps));
    }
    start_mdns();
}

int tonerelay_wifi_scan_json(char *buf, size_t len)
{
    if (!buf || len < 32) {
        return -1;
    }
    wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return snprintf(buf, len, "{\"ok\":false,\"op\":\"wifi_scan\",\"error\":\"scan failed\"}");
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) {
        n = 24;
    }
    wifi_ap_record_t recs[24];
    uint16_t got = n;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        got = 0;
    }

    size_t o = 0;
    o += (size_t)snprintf(buf + o, len - o, "{\"ok\":true,\"op\":\"wifi_scan\",\"aps\":[");
    bool first = true;
    for (uint16_t i = 0; i < got && o + 80 < len; i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        char ssid[65];
        json_escape(ssid, sizeof(ssid), (const char *)recs[i].ssid);
        const char *auth = recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "psk";
        o += (size_t)snprintf(
            buf + o,
            len - o,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
            first ? "" : ",",
            ssid,
            recs[i].rssi,
            auth
        );
        first = false;
    }
    o += (size_t)snprintf(buf + o, len - o, "]}");
    return (int)o;
}

int tonerelay_wifi_join(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32) {
        return -1;
    }
    const char *pw = pass ? pass : "";
    if (strlen(pw) > 64) {
        return -1;
    }
    if (nvs_save_sta(ssid, pw) != ESP_OK) {
        return -2;
    }
    bool was_up = s_sta_up;
    s_sta_configured = true;
    s_sta_up = false;
    s_sta_ip[0] = 0;
    s_retry = 0;
    if (s_sta_retry_timer) {
        esp_timer_stop(s_sta_retry_timer);
    }
    s_choosing = false;
    s_skip[0] = 0;
    if (was_up) {
        /* A connected radio does not move to another network by itself: let go, and the retry that follows picks among the saved ones. */
        s_leaving = true;
        esp_wifi_disconnect();
    } else {
        start_sta(ssid, pw, true);
    }
    return 0;
}

int tonerelay_wifi_ap_password(const char *pass)
{
    if (!pass) {
        return -1;
    }
    size_t n = strlen(pass);
    if (n < AP_PASS_MIN || n > AP_PASS_MAX) {
        return -1;
    }
    if (nvs_save_ap_pass(pass) != ESP_OK) {
        return -2;
    }
    s_ap_secured = true;
    start_ap();
    ESP_LOGI(TAG, "SoftAP password set; clients must reconnect");
    return 0;
}

int tonerelay_wifi_status_json(char *buf, size_t len)
{
    if (!buf || len < 32) {
        return -1;
    }
    wifi_ap_record_t rec = {0};
    if (s_sta_up && esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        s_sta_rssi = rec.rssi;
    }
    char ssid[65];
    json_escape(ssid, sizeof(ssid), s_sta_ssid);
    char ap_ssid[33];
    tonerelay_profile_ap_ssid(ap_ssid, sizeof(ap_ssid));
    /* The names of the saved networks, never their passwords. */
    char saved[MAX_NETS * 70 + 4];
    size_t so = 0;
    net_t nets[MAX_NETS];
    int n = nvs_load_nets(nets);
    for (int i = 0; i < n; i++) {
        char esc[65];
        json_escape(esc, sizeof(esc), nets[i].ssid);
        so += (size_t)snprintf(saved + so, sizeof(saved) - so, "%s\"%s\"", i ? "," : "", esc);
    }
    saved[so < sizeof(saved) ? so : sizeof(saved) - 1] = 0;
    memset(nets, 0, sizeof(nets));
    return snprintf(
        buf,
        len,
        "{\"ok\":true,\"op\":\"wifi_status\",\"setup_required\":%s,\"sta\":%s,"
        "\"sta_configured\":%s,\"ap_secured\":%s,\"catalog_ready\":%s,"
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ap\":\"%s\","
        "\"ap_ip\":\"192.168.4.1\",\"mdns\":\"%s.local\",\"saved\":[%s]}",
        tonerelay_wifi_setup_required() ? "true" : "false",
        s_sta_up ? "true" : "false",
        s_sta_configured ? "true" : "false",
        s_ap_secured ? "true" : "false",
        tonerelay_catalog_ready() ? "true" : "false",
        ssid,
        s_sta_ip,
        s_sta_rssi,
        ap_ssid,
        tonerelay_profile()->hostname,
        saved
    );
}

int tonerelay_wifi_forget(void)
{
    nvs_erase_sta();
    s_sta_configured = false;
    s_choosing = false;
    s_sta_up = false;
    s_sta_ssid[0] = 0;
    s_sta_ip[0] = 0;
    s_retry = 0;
    if (s_sta_retry_timer) {
        esp_timer_stop(s_sta_retry_timer);
    }
    esp_wifi_disconnect();
    start_ap();
    ESP_LOGI(TAG, "STA credentials cleared");
    return 0;
}

int tonerelay_wifi_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    for (int i = 0; i < MAX_NETS; i++) {
        char ks[8], kp[8];
        slot_keys(i, ks, kp, sizeof(ks));
        nvs_erase_key(h, ks);
        nvs_erase_key(h, kp);
    }
    nvs_erase_key(h, "ap_pass");
    nvs_erase_key(h, "token"); /* a new token is made at the next start: a reset box must be paired again */
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "factory reset: Wi-Fi networks and hotspot password erased");
    return err == ESP_OK ? 0 : -1;
}

bool tonerelay_wifi_setup_required(void)
{
    return !s_ap_secured || !tonerelay_catalog_ready();
}

bool tonerelay_wifi_ap_secured(void)
{
    return s_ap_secured;
}

bool tonerelay_wifi_sta_configured(void)
{
    return s_sta_configured;
}

bool tonerelay_wifi_sta_up(void)
{
    return s_sta_up;
}
