/*
 * Raw relay (prototype): carries USB bulk transfers between one WebSocket client on /raw and the
 * pedal, without interpreting them. See docs/raw-relay-protocol.md (draft v0).
 *
 * Prototype limits, all deliberate: no stale_session handling, and a failed upgrade is
 * reported with an `error` text frame and a close instead of HTTP 409. The mode is turned on with
 * POST /api/relay {"on":true}, which fails with 409 while the box's own session holds the pedal:
 * turn it on BEFORE attaching the pedal.
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
#include "esp_random.h"
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
#define PROTO TONERELAY_RELAY_PROTO
/* A client that has sent nothing for this long is gone (the engine's own keepalives alone arrive about once a second, so a live
 * client is never this quiet). Wi-Fi loss, a closed laptop or a crashed app leave no goodbye, and without this the box held the
 * pedal for its dead client until TCP gave up. */
#define CLIENT_SILENT_MS 6000
/* The longest a single send to the /raw client may block. */
#define SEND_TIMEOUT_S 2
/* The box's token: 128 random bits as 32 hex characters, made on first boot and kept in NVS. A client must send it as
 * `Authorization: Bearer <token>` on the /raw upgrade and on POST /api/relay. */
#define TOKEN_LEN 32
#define NVS_TOKEN_KEY "token"

static volatile bool s_relay_only;
static char s_token[TOKEN_LEN + 1];
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

/* Breadcrumbs that survive a software reset, so the next boot can say what happened last (it logs them at start-up).
 * Lifecycle steps only (a binary frame would flush the ring). */
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
    uint8_t v = tonerelay_profile()->relay_only_default ? 1 : 0; /* until someone changes it */
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY, &v) != ESP_OK) {
            v = tonerelay_profile()->relay_only_default ? 1 : 0;
        }
        nvs_close(h);
    }
    s_relay_only = v != 0;
}

static void token_init(void)
{
    nvs_handle_t h;
    bool have_nvs = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (have_nvs) {
        size_t len = sizeof(s_token);
        if (nvs_get_str(h, NVS_TOKEN_KEY, s_token, &len) == ESP_OK && len == sizeof(s_token)) {
            nvs_close(h);
            return;
        }
    }
    uint8_t raw[TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(s_token + 2 * i, 3, "%02x", raw[i]);
    }
    if (have_nvs) {
        nvs_set_str(h, NVS_TOKEN_KEY, s_token);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "made this box's token");
    } else {
        ESP_LOGE(TAG, "could not save the token: it is good until the next restart");
    }
}

void tonerelay_relay_token(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_token);
}

/* Compares in time that depends only on the length, not on where the strings first differ. */
static bool same_secret(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < alen; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* `Authorization: Bearer <token>`. A native client can set the header; a browser cannot on a WebSocket, and is kept out by the Origin check. */
static bool token_ok(httpd_req_t *req)
{
    char v[64];
    size_t n = httpd_req_get_hdr_value_len(req, "Authorization");
    if (n == 0 || n >= sizeof(v) || httpd_req_get_hdr_value_str(req, "Authorization", v, sizeof(v)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(v, "Bearer ", 7) != 0) {
        return false;
    }
    const char *t = v + 7;
    return same_secret(t, strlen(t), s_token, TOKEN_LEN);
}

void tonerelay_relay_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_send_mu = xSemaphoreCreateMutex();
    s_task_done = xSemaphoreCreateBinary();
    nvs_load();
    token_init();
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
    if (s_prev_n > 0) {
        char list[CRUMBS * 4 + 1];
        size_t o = 0;
        uint32_t cnt = s_prev_n < CRUMBS ? s_prev_n : CRUMBS;
        for (uint32_t i = 0; i < cnt && o + 4 < sizeof(list); i++) {
            o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%u", i ? "," : "", (unsigned)s_prev[(s_prev_n - cnt + i) % CRUMBS]);
        }
        ESP_LOGI(TAG, "breadcrumbs from the previous run (last %u of %u): %s", (unsigned)cnt, (unsigned)s_prev_n, list);
    }
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
        if (!token_ok(req)) {
            return refuse(req, "unauthorized");
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
        /* httpd gives every socket send_wait_timeout (60 s, see http.c). A send to a client that has stopped reading would hold the relay
         * task, and with it the pedal, far longer than the 6 s silence check can act: it only runs between USB reads. A failed send drops
         * the client (relay_task breaks out of its loop). */
        struct timeval send_to = {.tv_sec = SEND_TIMEOUT_S, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_to, sizeof(send_to));
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

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') {
        (*p)++;
    }
}

/* Accepts exactly {"on":true} or {"on":false} (whitespace allowed). Returns 1 or 0, or -1 for anything else. */
static int parse_on(const char *p)
{
    skip_ws(&p);
    if (*p++ != '{') {
        return -1;
    }
    skip_ws(&p);
    if (strncmp(p, "\"on\"", 4) != 0) {
        return -1;
    }
    p += 4;
    skip_ws(&p);
    if (*p++ != ':') {
        return -1;
    }
    skip_ws(&p);
    int v;
    if (strncmp(p, "true", 4) == 0) {
        v = 1;
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        v = 0;
        p += 5;
    } else {
        return -1;
    }
    skip_ws(&p);
    if (*p++ != '}') {
        return -1;
    }
    skip_ws(&p);
    return *p == 0 ? v : -1;
}

static esp_err_t json_reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t relay_api(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        if (!token_ok(req)) {
            return json_reply(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"unauthorized\"}");
        }
        char body[64];
        size_t want = req->content_len;
        if (want == 0 || want >= sizeof(body)) {
            return json_reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad_request\"}");
        }
        size_t got = 0;
        while (got < want) {
            int n = httpd_req_recv(req, body + got, want - got);
            if (n <= 0) {
                return json_reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad_request\"}");
            }
            got += (size_t)n;
        }
        body[got] = 0;
        int on = parse_on(body);
        if (on < 0) {
            return json_reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"bad_request\"}");
        }
        if (on && !s_relay_only && helix_usb_claimed()) {
            /* The box's own session holds the pedal: relay mode would take it over without a session close, which can wedge the pedal. */
            return json_reply(req, "409 Conflict", "{\"ok\":false,\"error\":\"busy\"}");
        }
        if ((on != 0) != s_relay_only) {
            s_relay_only = on != 0;
            nvs_save(s_relay_only);
            if (!s_relay_only && s_active) {
                httpd_sess_trigger_close(s_hd, s_fd);
            }
        }
    }
    char product[48], serial[48], pe[48], se[48];
    helix_usb_cached_strings(product, sizeof(product), serial, sizeof(serial));
    clean(pe, sizeof(pe), product);
    clean(se, sizeof(se), serial);
    char out[400];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"relay_only\":%s,\"client\":%s,\"proto\":%d,\"profile\":\"%s\",\"auth\":\"bearer\","
             "\"pedal\":{\"present\":%s,\"pid\":\"0x%04x\",\"vid\":\"0x0e41\",\"product\":\"%s\",\"serial\":\"%s\"}}",
             s_relay_only ? "true" : "false", s_active ? "true" : "false", PROTO, tonerelay_profile()->name,
             helix_usb_present() ? "true" : "false", (unsigned)helix_usb_pid(), pe, se);
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
