/*
 * Raw relay (prototype): carries USB bulk transfers between one WebSocket client on /raw and the
 * pedal, without interpreting them. See docs/raw-relay-protocol.md (draft v0).
 *
 * Prototype limits, all deliberate: no token, no mDNS record, no stale_session handling, a failed
 * upgrade is reported with an `error` text frame and a close instead of HTTP 409, and the only way
 * to turn the mode on is POST /api/relay {"on":true}. Turn it on BEFORE attaching the pedal: the
 * box's own session, if open, is dropped without a session close.
 */
#include "tonerelay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define TAG "tr_relay"
#define NVS_NS "tonerelay"
#define NVS_KEY "relay_only"
#define RAW_OUT_MAX 4096
#define RAW_IN_MAX 4096
#define IN_MPS 512
#define PROTO 0
/* A client that has sent nothing for this long is gone (the engine's own keepalives alone arrive about once a second, so a live
 * client is never this quiet). Wi-Fi loss, a closed laptop or a crashed app leave no goodbye, and without this the box held the
 * pedal for its dead client until TCP gave up. */
#define CLIENT_SILENT_MS 6000

static volatile bool s_relay_only;
static SemaphoreHandle_t s_mu;      /* guards the client fields below */
static SemaphoreHandle_t s_send_mu; /* one frame on the socket at a time */
static SemaphoreHandle_t s_task_done;
static httpd_handle_t s_hd;
static volatile int s_fd = -1;
static volatile bool s_active;
static volatile bool s_claimed; /* we opened the pedal for the current client */
static volatile int64_t s_last_rx_ms; /* when the client last sent a frame */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}
static volatile uint32_t s_gen;
static uint8_t s_in_buf[RAW_IN_MAX];

/* Breadcrumbs that survive a software reset, so the next boot can say what happened last. They are
 * exposed by GET /api/relay. Lifecycle steps only (a binary frame would flush the ring). */
#define CRUMBS 32
#define CR_MAGIC 0x52454c59u
static RTC_NOINIT_ATTR uint32_t s_cr_magic;
static RTC_NOINIT_ATTR uint32_t s_cr[CRUMBS];
static RTC_NOINIT_ATTR uint32_t s_cr_n;
static int s_boot_reason;
static uint32_t s_prev[CRUMBS];
static uint32_t s_prev_n;

static void crumb(uint32_t id)
{
    s_cr[s_cr_n % CRUMBS] = id;
    s_cr_n++;
}

bool tonerelay_relay_only(void)
{
    return s_relay_only;
}

static void nvs_save(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY, &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(h);
    }
    s_relay_only = v != 0;
}

void tonerelay_relay_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_send_mu = xSemaphoreCreateMutex();
    s_task_done = xSemaphoreCreateBinary();
    nvs_load();
    s_boot_reason = (int)esp_reset_reason();
    if (s_cr_magic == CR_MAGIC) {
        memcpy(s_prev, s_cr, sizeof(s_prev));
        s_prev_n = s_cr_n;
    } else {
        s_cr_magic = CR_MAGIC;
        s_prev_n = 0;
    }
    memset(s_cr, 0, sizeof(s_cr));
    s_cr_n = 0;
    ESP_LOGI(TAG, "relay-only is %s, reset reason %d", s_relay_only ? "ON" : "off", s_boot_reason);
}

/* -- sending ------------------------------------------------------------------------------- */

static esp_err_t send_frame(httpd_ws_type_t type, const uint8_t *data, size_t len)
{
    httpd_ws_frame_t f = {.final = true, .fragmented = false, .type = type, .payload = (uint8_t *)data, .len = len};
    xSemaphoreTake(s_send_mu, portMAX_DELAY);
    esp_err_t err = ESP_FAIL;
    int fd = s_fd;
    if (fd >= 0) {
        err = httpd_ws_send_frame_async(s_hd, fd, &f);
    }
    xSemaphoreGive(s_send_mu);
    return err;
}

static esp_err_t send_text(const char *text)
{
    return send_frame(HTTPD_WS_TYPE_TEXT, (const uint8_t *)text, strlen(text));
}

static void clean(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) {
        char c = src[i];
        dst[i] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
    }
    dst[i] = 0;
}

/* -- client lifecycle ---------------------------------------------------------------------- */

/* Stop the relay task, release the pedal, forget the client. Safe to call twice. */
static void detach(void)
{
    crumb(21);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool claimed = s_claimed;
    s_claimed = false;
    s_active = false;
    s_gen++;
    xSemaphoreGive(s_mu);
    if (!claimed) {
        return;
    }
    /* The task notices s_active and signals; bounded wait so a stuck USB call cannot hang httpd. */
    xSemaphoreTake(s_task_done, pdMS_TO_TICKS(600));
    crumb(22);
    helix_usb_close();
    crumb(23);
    s_fd = -1;
    ESP_LOGI(TAG, "client gone; pedal released (no session close sent)");
}

void tonerelay_relay_on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    crumb(20);
    if (fd == s_fd) {
        detach();
    }
    close(fd);
    crumb(24);
}

static void relay_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    while (s_active && gen == s_gen) {
        if (now_ms() - s_last_rx_ms > CLIENT_SILENT_MS) {
            ESP_LOGW(TAG, "client silent for %d ms: dropping it", (int)(now_ms() - s_last_rx_ms));
            crumb(33);
            send_text("{\"t\":\"closing\",\"reason\":\"client_silent\"}");
            break;
        }
        size_t total = 0, n = 0;
        int rc = helix_usb_recv(s_in_buf, IN_MPS, &n, 100);
        if (rc == -1) {
            if (!helix_usb_present()) {
                send_text("{\"t\":\"pedal_detached\"}");
                break;
            }
            continue;
        }
        if (rc != 0) {
            send_text("{\"t\":\"error\",\"code\":\"usb_error\"}");
            break;
        }
        total = n;
        /* A transfer ends at a short packet: join full packets until one is short. */
        while (n == IN_MPS && total + IN_MPS <= RAW_IN_MAX) {
            rc = helix_usb_recv(s_in_buf + total, IN_MPS, &n, 20);
            if (rc != 0) {
                break;
            }
            total += n;
        }
        if (send_frame(HTTPD_WS_TYPE_BINARY, s_in_buf, total) != ESP_OK) {
            break;
        }
    }
    crumb(30);
    bool mine = (gen == s_gen);
    xSemaphoreGive(s_task_done);
    if (mine && s_active) {
        /* The pedal went away or the socket failed: end the session from here. */
        int fd = s_fd;
        s_active = false;
        if (fd >= 0) {
            crumb(31);
            httpd_sess_trigger_close(s_hd, fd);
        }
    }
    crumb(32);
    vTaskDelete(NULL);
}

static void hello_json(char *out, size_t cap, bool have, uint16_t pid, const char *product, const char *serial)
{
    if (!have) {
        snprintf(out, cap, "{\"t\":\"hello\",\"proto\":%d,\"fw\":\"relay-proto\",\"pid\":null}", PROTO);
        return;
    }
    char p[48], s[48];
    clean(p, sizeof(p), product);
    clean(s, sizeof(s), serial);
    snprintf(out, cap,
             "{\"t\":\"hello\",\"proto\":%d,\"fw\":\"relay-proto\",\"pid\":\"0x%04x\",\"vid\":\"0x0e41\","
             "\"product\":\"%s\",\"serial\":\"%s\",\"mps\":%d}",
             PROTO, pid, p, s, IN_MPS);
}

static esp_err_t refuse(httpd_req_t *req, const char *code)
{
    char msg[80];
    snprintf(msg, sizeof(msg), "{\"t\":\"error\",\"code\":\"%s\"}", code);
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg)};
    httpd_ws_send_frame(req, &f);
    return ESP_FAIL; /* httpd closes the socket */
}

/* A browser sends Origin on every WebSocket upgrade, a native client does not. Accept a request without one, and
 * one from a page this box served itself (Origin and Host name the same authority). Anything else is a page from
 * elsewhere trying to drive the pedal through the visitor's browser. DNS rebinding is not covered: the rebound
 * name shows up in both headers. */
static bool origin_allowed(httpd_req_t *req)
{
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (olen == 0) {
        return true;
    }
    char origin[96], host[64];
    if (olen >= sizeof(origin) || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return false;
    }
    size_t hlen = httpd_req_get_hdr_value_len(req, "Host");
    if (hlen == 0 || hlen >= sizeof(host) || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    const char *authority = strstr(origin, "://");
    return authority && strcasecmp(authority + 3, host) == 0;
}

static esp_err_t raw_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        crumb(1);
        if (!origin_allowed(req)) {
            return refuse(req, "origin_not_allowed");
        }
        if (!s_relay_only) {
            return refuse(req, "relay_unavailable");
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_active) {
            xSemaphoreGive(s_mu);
            return refuse(req, "busy");
        }
        xSemaphoreGive(s_mu);

        if (helix_usb_claimed()) {
            return refuse(req, "box_session_open");
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        /* Second line against a vanished client: probe an idle connection after 5 s, every 2 s, give up after 3. */
        int idle = 5, intvl = 2, cnt = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

        uint16_t pid = 0;
        char product[48] = "", serial[48] = "";
        bool have = helix_usb_present() && helix_usb_open(&pid) == 0;
        char hello[200];
        if (have) {
            helix_usb_strings(product, sizeof(product), serial, sizeof(serial));
        }
        crumb(2);
        hello_json(hello, sizeof(hello), have, pid, product, serial);
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)hello, .len = strlen(hello)};
        httpd_ws_send_frame(req, &f);
        if (!have) {
            return ESP_FAIL;
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_hd = req->handle;
        s_fd = fd;
        s_last_rx_ms = now_ms();
        s_active = true;
        s_claimed = true;
        uint32_t gen = ++s_gen;
        xSemaphoreGive(s_mu);
        xSemaphoreTake(s_task_done, 0); /* clear a stale token */
        if (xTaskCreate(relay_task, "raw_relay", 6144, (void *)(uintptr_t)gen, 5, NULL) != pdPASS) {
            s_active = false;
            s_claimed = false;
            helix_usb_close();
            s_fd = -1;
            return ESP_FAIL;
        }
        crumb(4);
        ESP_LOGI(TAG, "client attached, pid=%04x", pid);
        return ESP_OK;
    }

    if (fd != s_fd) {
        return ESP_FAIL;
    }
    s_last_rx_ms = now_ms();
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        crumb(12);
        return ESP_OK;
    }
    if (frame.len > RAW_OUT_MAX) {
        send_text("{\"t\":\"error\",\"code\":\"oversize\"}");
        return ESP_FAIL;
    }
    uint8_t *buf = NULL;
    if (frame.len > 0) {
        buf = malloc(frame.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = buf;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        buf[frame.len] = 0;
    }
    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (frame.len > 0 && helix_usb_send(buf, frame.len, 2000) != 0) {
            send_text("{\"t\":\"error\",\"code\":\"usb_error\"}");
            free(buf);
            return ESP_FAIL;
        }
    } else if (frame.type == HTTPD_WS_TYPE_TEXT) {
        if (buf && strstr((char *)buf, "\"ping\"")) {
            send_text("{\"t\":\"pong\"}");
        }
    } else if (frame.type == HTTPD_WS_TYPE_PING) {
        send_frame(HTTPD_WS_TYPE_PONG, buf, frame.len);
    }
    free(buf);
    return ESP_OK;
}

/* -- /api/relay ---------------------------------------------------------------------------- */

static esp_err_t relay_api(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char body[64] = {0};
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n > 0) {
            if (strstr(body, "true")) {
                s_relay_only = true;
            } else if (strstr(body, "false")) {
                s_relay_only = false;
                if (s_active) {
                    int fd = s_fd;
                    httpd_sess_trigger_close(s_hd, fd);
                }
            }
            nvs_save(s_relay_only);
        }
    }
    char product[48], serial[48], pe[48], se[48];
    helix_usb_cached_strings(product, sizeof(product), serial, sizeof(serial));
    clean(pe, sizeof(pe), product);
    clean(se, sizeof(se), serial);
    char out[640];
    int o = snprintf(out, sizeof(out),
                     "{\"ok\":true,\"relay_only\":%s,\"client\":%s,\"proto\":%d,"
                     "\"pedal\":{\"present\":%s,\"pid\":\"0x%04x\",\"vid\":\"0x0e41\",\"product\":\"%s\",\"serial\":\"%s\"},"
                     "\"reset_reason\":%d,\"prev_crumbs\":[",
                     s_relay_only ? "true" : "false", s_active ? "true" : "false", PROTO,
                     helix_usb_present() ? "true" : "false", (unsigned)helix_usb_pid(), pe, se, s_boot_reason);
    uint32_t cnt = s_prev_n < CRUMBS ? s_prev_n : CRUMBS;
    for (uint32_t i = 0; i < cnt && o < (int)sizeof(out) - 16; i++) {
        uint32_t idx = (s_prev_n - cnt + i) % CRUMBS;
        o += snprintf(out + o, sizeof(out) - (size_t)o, "%s%u", i ? "," : "", (unsigned)s_prev[idx]);
    }
    snprintf(out + o, sizeof(out) - (size_t)o, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

void tonerelay_relay_register(httpd_handle_t srv)
{
    const httpd_uri_t raw = {.uri = "/raw", .method = HTTP_GET, .handler = raw_handler, .is_websocket = true,
                             .handle_ws_control_frames = true};
    const httpd_uri_t api_get = {.uri = "/api/relay", .method = HTTP_GET, .handler = relay_api};
    const httpd_uri_t api_post = {.uri = "/api/relay", .method = HTTP_POST, .handler = relay_api};
    httpd_register_uri_handler(srv, &raw);
    httpd_register_uri_handler(srv, &api_get);
    httpd_register_uri_handler(srv, &api_post);
}
