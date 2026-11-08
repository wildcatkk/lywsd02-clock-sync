#include "web.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cluster.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jobs.h"
#include "logs.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "ota.h"
#include "store.h"
#include "wifi.h"

#define BODY_MAX 4096
#define LOG_BATCH 1536
#define LOG_FETCH 20
#define HOST_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-"

static const char *TAG = "web";

typedef struct {
    const char *path;
    const char *type;
    const uint8_t *start;
    const uint8_t *end;
} asset_t;

extern const uint8_t index_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_end[] asm("_binary_index_html_gz_end");
extern const uint8_t i18n_start[] asm("_binary_i18n_js_gz_start");
extern const uint8_t i18n_end[] asm("_binary_i18n_js_gz_end");
extern const uint8_t icon_start[] asm("_binary_icon_png_gz_start");
extern const uint8_t icon_end[] asm("_binary_icon_png_gz_end");
extern const uint8_t timezones_start[] asm("_binary_timezones_js_gz_start");
extern const uint8_t timezones_end[] asm("_binary_timezones_js_gz_end");
extern const uint8_t zones_start[] asm("_binary_zones_js_gz_start");
extern const uint8_t zones_end[] asm("_binary_zones_js_gz_end");
extern const uint8_t config_start[] asm("_binary_config_html_gz_start");
extern const uint8_t config_end[] asm("_binary_config_html_gz_end");

static const asset_t INDEX = {"/static/index.html", "text/html; charset=utf-8", index_start, index_end};
static const asset_t CONFIG = {"/config", "text/html; charset=utf-8", config_start, config_end};
static const asset_t STATIC[] = {
    {"/static/index.html", "text/html; charset=utf-8", index_start, index_end},
    {"/static/i18n.js", "text/javascript; charset=utf-8", i18n_start, i18n_end},
    {"/static/icon.png", "image/png", icon_start, icon_end},
    {"/static/timezones.js", "text/javascript; charset=utf-8", timezones_start, timezones_end},
    {"/static/zones.js", "text/javascript; charset=utf-8", zones_start, zones_end},
};

static char etag[20];

// ---------------------------------------------------------------- helpers

static esp_err_t unavailable(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"detail\":\"temporarily unavailable\"}");
}

esp_err_t web_send_json(httpd_req_t *req, const char *status, cJSON *body)
{
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!text) {
        return unavailable(req);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *event,
                         cJSON *params)
{
    cJSON *detail = cJSON_CreateObject();
    cJSON_AddStringToObject(detail, "event", event);
    json_set(detail, "params", params ? params : cJSON_CreateObject());
    cJSON *body = cJSON_CreateObject();
    json_set(body, "detail", detail);
    return web_send_json(req, status, body);
}

static esp_err_t send_log_source(httpd_req_t *req, cJSON *body, const log_line_t *lines,
                                 int count)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!text) {
        return ESP_FAIL;
    }
    size_t length = strlen(text);
    bool empty = length <= 2;
    esp_err_t err = httpd_resp_send_chunk(req, text, length - 1);
    free(text);
    char batch[LOG_BATCH];
    size_t used = 0;
    if (err == ESP_OK) {
        used = snprintf(batch, LOG_BATCH, "%s\"logs\":[", empty ? "" : ",");
    }
    for (int i = 0; i < count && err == ESP_OK; i++) {
        size_t size = strlen(lines[i].text) + 1;
        if (used + size >= LOG_BATCH) {
            err = httpd_resp_send_chunk(req, batch, used);
            used = 0;
        }
        if (err != ESP_OK) {
            break;
        }
        if (size >= LOG_BATCH) {
            err = httpd_resp_send_chunk(req, i ? "," : "", i ? 1 : 0);
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, lines[i].text, size - 1);
            }
            continue;
        }
        used += snprintf(batch + used, LOG_BATCH - used, "%s%s", i ? "," : "", lines[i].text);
    }
    if (err == ESP_OK) {
        if (used + 3 > LOG_BATCH) {
            err = httpd_resp_send_chunk(req, batch, used);
            used = 0;
        }
        memcpy(batch + used, "]}", 2);
        used += 2;
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, batch, used);
        }
    }
    return err;
}

// ---------------------------------------------------------------- relay

// Anything that talks to another node runs here instead of on the server's own
// task: two nodes asking each other at once would otherwise each wait for an
// answer the other cannot give until its own request has timed out.
typedef esp_err_t (*deferred_fn)(httpd_req_t *req);

typedef struct {
    httpd_req_t *req;
    deferred_fn fn;
} deferred_t;

static QueueHandle_t deferred;

static void relay_task(void *arg)
{
    for (;;) {
        deferred_t job;
        xQueueReceive(deferred, &job, portMAX_DELAY);
        job.fn(job.req);
        httpd_req_async_handler_complete(job.req);
    }
}

// The request body is gone once the handler returns, so `fn` must not need it.
static esp_err_t defer(httpd_req_t *req, deferred_fn fn)
{
    httpd_req_t *copy;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return unavailable(req);
    }
    deferred_t job = {copy, fn};
    if (xQueueSend(deferred, &job, 0) != pdTRUE) {
        unavailable(copy);
        httpd_req_async_handler_complete(copy);
    }
    return ESP_OK;
}

// The IPv4 address the request came from, network order; 0 when it has none.
static uint32_t client_ipv4(httpd_req_t *req)
{
    struct sockaddr_storage peer;
    socklen_t size = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &size) != 0) {
        return 0;
    }
    uint32_t addr = 0;
    if (peer.ss_family == AF_INET) {
        addr = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    } else if (peer.ss_family == AF_INET6) {
        const uint8_t *bytes = ((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr;
        static const uint8_t MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(bytes, MAPPED, sizeof(MAPPED)) == 0) {
            memcpy(&addr, bytes + 12, 4);
        }
    }
    return addr;
}

static const char *status_line(int status, char *buffer, size_t size)
{
    static const struct {
        int code;
        const char *text;
    } KNOWN[] = {
        {400, "Bad Request"}, {404, "Not Found"}, {405, "Method Not Allowed"},
        {409, "Conflict"}, {422, "Unprocessable Entity"}, {500, "Internal Server Error"},
        {503, "Service Unavailable"},
    };
    const char *text = "Error";
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (KNOWN[i].code == status) {
            text = KNOWN[i].text;
        }
    }
    snprintf(buffer, size, "%d %s", status, text);
    return buffer;
}

static esp_err_t bad_field(httpd_req_t *req, const char *field)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "field", field);
    return web_send_error(req, "400 Bad Request", "error.badField", params);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) {
        return NULL;
    }
    char *text = malloc(req->content_len + 1);
    if (!text) {
        return NULL;
    }
    int have = 0;
    int64_t deadline = esp_timer_get_time() + 10 * 1000000;
    while (have < req->content_len) {
        if (esp_timer_get_time() >= deadline) {
            free(text);
            return NULL;
        }
        int got = httpd_req_recv(req, text + have, req->content_len - have);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            free(text);
            return NULL;
        }
        have += got;
    }
    text[have] = 0;
    cJSON *body = cluster_json_bounded(text, have) ? cJSON_Parse(text) : NULL;
    free(text);
    if (body && !cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

// The hotspot and the home network are told apart by the address the request
// arrived on. The server listens on IPv6, so IPv4 shows up mapped.
static bool on_hotspot(httpd_req_t *req)
{
    uint32_t ap = wifi_ap_ip();
    if (!ap) {
        return false;
    }
    struct sockaddr_storage local;
    socklen_t size = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0) {
        return false;
    }
    uint32_t addr = 0;
    if (local.ss_family == AF_INET) {
        addr = ((struct sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        const uint8_t *bytes = ((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr;
        memcpy(&addr, bytes + 12, 4);
    }
    return addr == ap;
}

static esp_err_t send_asset(httpd_req_t *req, const asset_t *asset)
{
    char seen[sizeof(etag) + 4];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", seen, sizeof(seen)) == ESP_OK
            && strcmp(seen, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, asset->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // The files only change with the firmware, whose build hash is the tag.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    return httpd_resp_send(req, (const char *)asset->start, asset->end - asset->start);
}

static size_t utf8_length(const char *text)
{
    size_t count = 0;
    for (; *text; text++) {
        count += ((unsigned char)*text & 0xC0) != 0x80;
    }
    return count;
}

static char *strip(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = 0;
    }
    return text;
}

// A number field the way pydantic reads it: an int must be whole.
static bool number(const cJSON *object, const char *key, double low, double high,
                   bool whole, double *out)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsNumber(item) || isnan(item->valuedouble)) {
        return false;
    }
    double value = item->valuedouble;
    if ((whole && value != floor(value)) || value < low || value > high) {
        return false;
    }
    *out = value;
    return true;
}

static bool boolean(const cJSON *object, const char *key, bool fallback, bool *out)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    if (!item) {
        *out = fallback;
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool valid_timezone(const char *text)
{
    if (strlen(text) > 64) {
        return false;
    }
    for (; *text; text++) {
        if (*text <= ' ' || *text > '~') {
            return false;
        }
    }
    return true;
}

// An IANA name such as America/Argentina/Buenos_Aires; the page owns the list.
static bool valid_zone(const char *text)
{
    if (!text[0] || strlen(text) > 40) {
        return false;
    }
    for (; *text; text++) {
        if (!isalnum((unsigned char)*text) && !strchr("/_+-", *text)) {
            return false;
        }
    }
    return true;
}

// The page sends the zone's name and its rule together; reads both off `body`.
static const char *read_zone(const cJSON *body, char *zone, size_t zone_size,
                             char *rule, size_t rule_size)
{
    const char *name = json_str(body, "zone", NULL);
    const char *posix = json_str(body, "timezone", NULL);
    if (!name || !valid_zone(name)) {
        return "zone";
    }
    if (!posix || !posix[0] || !valid_timezone(posix)) {
        return "timezone";
    }
    snprintf(zone, zone_size, "%s", name);
    snprintf(rule, rule_size, "%s", posix);
    return NULL;
}

static bool apply_zone(const char *zone, const char *rule)
{
    cJSON *updates = cJSON_CreateObject();
    if (!json_set(updates, "zone", cJSON_CreateString(zone))
            || !json_set(updates, "timezone", cJSON_CreateString(rule))) {
        cJSON_Delete(updates);
        return false;
    }
    cJSON *config = store_config();
    while (updates->child) {
        cJSON *item = cJSON_DetachItemViaPointer(updates, updates->child);
        bool replaced = cJSON_ReplaceItemViaPointer(config,
            cJSON_GetObjectItemCaseSensitive(config, item->string), item);
        configASSERT(replaced);
    }
    cJSON_Delete(updates);
    wifi_apply_timezone(rule);
    return true;
}

static void announce_name(void)
{
    char name[97];
    store_node_name(name, sizeof(name));
    mdns_instance_name_set(name);
    store_lock();
    json_set(cJSON_GetObjectItem(store_state(), "nodes"), store_node_id(),
             cJSON_CreateString(name));
    store_unlock();
    store_save_state();
}

static const char *language(void)
{
    store_lock();
    const char *chosen = json_str(store_config(), "language", "zh-CN");
    const char *known = "zh-CN";
    if (strcmp(chosen, "zh-TW") == 0) {
        known = "zh-TW";
    } else if (strcmp(chosen, "en") == 0) {
        known = "en";
    }
    store_unlock();
    return known;
}

// ---------------------------------------------------------------- pages

static esp_err_t get_root(httpd_req_t *req)
{
    // A phone that has just joined the hotspot asks for "/" to see the portal.
    return send_asset(req, on_hotspot(req) ? &CONFIG : &INDEX);
}

static esp_err_t get_config(httpd_req_t *req)
{
    return send_asset(req, &CONFIG);
}

static esp_err_t get_static(httpd_req_t *req)
{
    size_t length = strcspn(req->uri, "?");
    for (size_t i = 0; i < sizeof(STATIC) / sizeof(STATIC[0]); i++) {
        if (strlen(STATIC[i].path) == length && strncmp(req->uri, STATIC[i].path, length) == 0) {
            return send_asset(req, &STATIC[i]);
        }
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    // Every name resolves to us on the hotspot, so a phone's connectivity check
    // lands here; sending it to the portal is what makes the portal pop up.
    uint32_t ap = wifi_ap_ip();
    if (ap && on_hotspot(req)) {
        char location[48];
        snprintf(location, sizeof(location), "http://" IPSTR ":8787/config",
                 IP2STR((esp_ip4_addr_t *)&ap));
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_send(req, NULL, 0);
    }
    return web_send_error(req, "404 Not Found", "error.notFound", NULL);
}

// ---------------------------------------------------------------- state

static cJSON *node_view(void)
{
    char name[97];
    store_node_name(name, sizeof(name));
    cJSON *node = cJSON_CreateObject();
    if (!json_set(node, "id", cJSON_CreateString(store_node_id()))
            || !json_set(node, "name", cJSON_CreateString(name))) {
        cJSON_Delete(node);
        return NULL;
    }
    return node;
}

static cJSON *merged_devices(void)
{
    store_lock();
    cJSON *local = jobs_local_views();
    cJSON *merged = local ? cluster_merge(local) : NULL;
    store_unlock();
    cJSON_Delete(local);
    return merged;
}

static int local_log_source(const cJSON *held, cJSON **metadata, log_line_t *lines)
{
    char boot[17], name[97];
    uint32_t cursor;
    int count = logs_read(lines, LOG_FETCH, json_str(held, "boot", ""),
                          (uint32_t)json_num(held, "after", 0), boot, &cursor);
    store_node_name(name, sizeof(name));
    cJSON *body = cJSON_CreateObject();
    if (!json_set(body, "id", cJSON_CreateString(store_node_id()))
            || !json_set(body, "name", cJSON_CreateString(name))
            || !json_set(body, "boot", cJSON_CreateString(boot))
            || !json_set(body, "cursor", cJSON_CreateNumber(cursor))) {
        cJSON_Delete(body);
        *metadata = NULL;
        logs_free(lines, count);
        return -1;
    }
    *metadata = body;
    return count;
}

static bool valid_log_boot(const char *text)
{
    return text && (!text[0] || (strlen(text) == 16
           && strspn(text, "0123456789abcdef") == 16));
}

static esp_err_t post_logs(httpd_req_t *req)
{
    cJSON *cursors = read_json(req);
    if (!cursors) {
        return bad_field(req, "cursors");
    }
    if (!cJSON_IsObject(cursors) || cJSON_GetArraySize(cursors) > CLUSTER_MAX_PEERS + 1) {
        cJSON_Delete(cursors);
        return bad_field(req, "cursors");
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, cursors) {
        double after;
        if (!item->string || !item->string[0] || strlen(item->string) > 64
                || !cJSON_IsObject(item) || cJSON_GetArraySize(item) > 2
                || !valid_log_boot(json_str(item, "boot", NULL))
                || !number(item, "after", 0, UINT32_MAX, true, &after)) {
            cJSON_Delete(cursors);
            return bad_field(req, "cursors");
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr_chunk(req, "{\"sources\":[");
    bool first = true;
    for (int i = -1; i < CLUSTER_MAX_PEERS && err == ESP_OK; i++) {
        log_line_t lines[LOG_FETCH];
        cJSON *metadata;
        int count = i < 0
                    ? local_log_source(cJSON_GetObjectItemCaseSensitive(cursors, store_node_id()),
                                       &metadata, lines)
                    : cluster_log_source(i, cursors, &metadata, lines, LOG_FETCH);
        if (count < 0) {
            err = ESP_FAIL;
            break;
        }
        if (metadata) {
            if (!first) {
                err = httpd_resp_sendstr_chunk(req, ",");
            }
            first = false;
            if (err == ESP_OK) {
                err = send_log_source(req, metadata, lines, count);
            } else {
                cJSON_Delete(metadata);
            }
        }
        logs_free(lines, count);
    }
    cJSON_Delete(cursors);
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, "]}");
    }
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t get_state(httpd_req_t *req)
{
    // The page is open, so the cluster view should stay current. Probing happens
    // on the poll loop: this task must never wait on another node.
    cluster_watch();
    double now = store_now();
    cJSON *body = cJSON_CreateObject();
    bool ok = json_set(body, "language", cJSON_CreateString(language())) != NULL;
    store_lock();
    const cJSON *config = store_config();
    ok = json_set(body, "cluster", cJSON_CreateBool(json_bool(
                      cJSON_GetObjectItem(config, "node"), "cluster", true))) && ok;
    ok = json_set(body, "platform", cJSON_CreateString("esp32")) && ok;
    ok = json_set(body, "zone", cJSON_CreateString(json_str(config, "zone", ""))) && ok;
    ok = json_set(body, "timezone", cJSON_CreateString(json_str(config, "timezone", ""))) && ok;
    cJSON *schedule = cJSON_Duplicate(cJSON_GetObjectItem(config, "schedule"), true);
    store_unlock();

    cJSON *merged = merged_devices();
    ok = json_set(body, "node", node_view()) && ok;
    ok = json_set(body, "nodes", merged
                  ? cluster_nodes_view(merged, jobs_busy(), jobs_queued()) : NULL) && ok;
    ok = json_set(body, "lock", cluster_lock_view()) && ok;
    ok = json_set(body, "busy", jobs_busy()) && ok;
    ok = json_set(body, "queued", cJSON_CreateNumber(jobs_queued())) && ok;
    ok = json_set(body, "host_time", cJSON_CreateNumber(now)) && ok;
    ok = json_set(body, "next_run", json_num_or_null(jobs_next_scheduled(schedule, now))) && ok;
    ok = json_set(body, "schedule", schedule) && ok;
    ok = json_set(body, "devices", merged) && ok;
    if (!ok) {
        cJSON_Delete(body);
        body = NULL;
    }
    return web_send_json(req, "200 OK", body);
}

static esp_err_t get_ping(httpd_req_t *req)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    return web_send_json(req, "200 OK", body);
}

// ---------------------------------------------------------------- jobs

static cJSON *op(const char *event, const char *target)
{
    cJSON *body = cJSON_CreateObject();
    cJSON *params = json_set(body, "params", cJSON_CreateObject());
    if (!params || !json_set(body, "event", cJSON_CreateString(event))
            || (target && !json_set(params, "target", cJSON_CreateString(target)))) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

// Takes the cluster lock and queues the job; `event` is taken.
static esp_err_t start_job(httpd_req_t *req, const char *key, cJSON *event, job_kind_t kind,
                           const char *device, bool write)
{
    if (!event) {
        return unavailable(req);
    }
    // Every radio job stamps what it hears with the time, and a write would put
    // 1970 on the clock face.
    if (!wifi_time_synced()) {
        cJSON_Delete(event);
        return web_send_error(req, "503 Service Unavailable", "error.timeNotSet", NULL);
    }
    char token[33], blocker[97];
    if (!cluster_acquire(event, token, blocker, sizeof(blocker))) {
        cJSON_Delete(event);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "node", blocker);
        return web_send_error(req, "409 Conflict", "error.clusterBusy", params);
    }
    bool queued = jobs_enqueue(key, event, kind, device, write, token);
    if (!queued) {
        cluster_release(token);
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "queued", queued);
    return web_send_json(req, "200 OK", reply);
}

static esp_err_t run_scan(httpd_req_t *req)
{
    return start_job(req, "scan", op("busy.clusterScan", NULL), JOB_SCAN_CLUSTER, NULL, false);
}

static esp_err_t post_scan(httpd_req_t *req)
{
    return defer(req, run_scan);
}

static esp_err_t run_read(httpd_req_t *req)
{
    return start_job(req, "readAll", op("busy.clusterRead", NULL), JOB_READ_CLUSTER, NULL, false);
}

static esp_err_t post_read(httpd_req_t *req)
{
    return defer(req, run_read);
}

static esp_err_t no_logs(httpd_req_t *req)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddArrayToObject(reply, "logs");
    return web_send_json(req, "200 OK", reply);
}

static esp_err_t run_clear_logs(httpd_req_t *req)
{
    logs_clear();
    cluster_tell_all("/api/node/logs/clear", NULL, CLUSTER_ACTION_TIMEOUT);
    return no_logs(req);
}

static esp_err_t post_clear_logs(httpd_req_t *req)
{
    return defer(req, run_clear_logs);
}

// The {key} of {prefix}{key}[/action], or false when the path has none.
static bool device_key(httpd_req_t *req, const char *prefix, char key[24], char action[12])
{
    const char *start = req->uri + strlen(prefix);
    size_t length = strcspn(start, "/?");
    if (!length || length >= 24) {
        return false;
    }
    memcpy(key, start, length);
    key[length] = 0;
    action[0] = 0;
    if (start[length] == '/') {
        const char *rest = start + length + 1;
        size_t size = strcspn(rest, "?");
        if (size >= 12) {
            return false;
        }
        memcpy(action, rest, size);
        action[size] = 0;
    }
    return true;
}

// find(): the merged row of `key`, or NULL. The caller deletes it.
static cJSON *find(const char *key)
{
    cJSON *merged = merged_devices();
    cJSON *found = NULL;
    cJSON *device;
    cJSON_ArrayForEach(device, merged) {
        if (strcmp(json_str(device, "key", ""), key) == 0) {
            found = cJSON_DetachItemViaPointer(merged, device);
            break;
        }
    }
    cJSON_Delete(merged);
    return found;
}

// The device and action of a refresh or sync, or false (and answered) when the
// path names neither.
static bool visit_target(httpd_req_t *req, const char *prefix, char key[24], char action[12],
                         bool *write)
{
    if (!device_key(req, prefix, key, action)
            || (strcmp(action, "sync") && strcmp(action, "refresh"))) {
        not_found(req, HTTPD_404_NOT_FOUND);
        return false;
    }
    *write = strcmp(action, "sync") == 0;
    return true;
}

// start_visit(): the job on this node's own radio.
static esp_err_t visit_here(httpd_req_t *req, const char *key, bool write)
{
    store_lock();
    bool known = cJSON_GetObjectItemCaseSensitive(store_devices(), key) != NULL;
    cJSON *event = known ? op(write ? "busy.sync" : "busy.read", jobs_name_of(key)) : NULL;
    store_unlock();
    if (!known) {
        return web_send_error(req, "404 Not Found", "error.unknownDevice", NULL);
    }
    return start_job(req, key, event, JOB_VISIT, key, write);
}

// dispatch(): run it here, or hand it to whoever owns the device.
static esp_err_t run_device(httpd_req_t *req)
{
    char key[24], action[12];
    bool write;
    if (!visit_target(req, "/api/devices/", key, action, &write)) {
        return ESP_OK;
    }
    // Who owns it, and whether that node is up, comes from the peer states, so
    // they must not be minutes old.
    cluster_ensure_fresh();
    cJSON *device = find(key);
    if (!device) {
        return web_send_error(req, "404 Not Found", "error.unknownDevice", NULL);
    }
    const char *owner = json_str(device, "node_id", "");
    if (strcmp(owner, store_node_id()) == 0) {
        cJSON_Delete(device);
        return visit_here(req, key, write);
    }
    cluster_peer_t peer;
    if (!cluster_peer_of(owner, &peer)) {
        cJSON *params = cJSON_CreateObject();
        json_set(params, "node",
                              cJSON_Duplicate(cJSON_GetObjectItem(device, "node"), true));
        cJSON_Delete(device);
        return web_send_error(req, "503 Service Unavailable", "error.nodeOffline", params);
    }
    cJSON_Delete(device);

    char path[64];
    snprintf(path, sizeof(path), "/api/node/devices/%s/%s", key, action);
    int status;
    cJSON *failure = NULL;
    cJSON *answer = cluster_call(peer.url, path, NULL, false, CLUSTER_ACTION_TIMEOUT,
                                 &status, &failure);
    if (answer) {
        return web_send_json(req, "200 OK", answer);
    }
    if (status > 0) {
        char line[40];
        cJSON *body = cJSON_CreateObject();
        json_set(body, "detail", failure);
        return web_send_json(req, status_line(status, line, sizeof(line)), body);
    }
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "node", peer.name);
    cJSON_AddStringToObject(params, "error", cJSON_IsString(failure) ? failure->valuestring : "");
    cJSON_Delete(failure);
    return web_send_error(req, "503 Service Unavailable", "error.nodeFailed", params);
}

static esp_err_t post_device(httpd_req_t *req)
{
    return defer(req, run_device);
}

// The first `limit` characters, counted the way Python counts them.
static void truncate_utf8(char *text, size_t limit)
{
    size_t count = 0;
    for (char *c = text; *c; c++) {
        if (((unsigned char)*c & 0xC0) != 0x80 && count++ == limit) {
            *c = 0;
            return;
        }
    }
}

static esp_err_t patch_device(httpd_req_t *req)
{
    char key[24], action[12];
    if (!device_key(req, "/api/devices/", key, action) || action[0]) {
        return not_found(req, HTTPD_404_NOT_FOUND);
    }
    cJSON *body = read_json(req);
    if (!body) {
        return bad_field(req, "body");
    }
    const cJSON *label = cJSON_GetObjectItem(body, "label");
    const cJSON *automatic = cJSON_GetObjectItem(body, "auto");
    const cJSON *pin = cJSON_GetObjectItem(body, "owner_pin");
    const char *bad = NULL;
    if (label && !cJSON_IsNull(label) && !cJSON_IsString(label)) {
        bad = "label";
    } else if (automatic && !cJSON_IsNull(automatic) && !cJSON_IsBool(automatic)) {
        bad = "auto";
    } else if (pin && !cJSON_IsNull(pin) && !cJSON_IsString(pin)) {
        bad = "owner_pin";
    }
    if (bad) {
        cJSON_Delete(body);
        return bad_field(req, bad);
    }

    cJSON *device = find(key);
    if (!device) {
        cJSON_Delete(body);
        return web_send_error(req, "404 Not Found", "error.unknownDevice", NULL);
    }
    esp_err_t result;
    char *new_label = NULL;
    if (cJSON_IsString(label)) {
        char *text = strip(label->valuestring);
        truncate_utf8(text, 40);
        if (!text[0]) {
            result = web_send_error(req, "400 Bad Request", "error.emptyLabel", NULL);
            goto done;
        }
        new_label = text;
    }
    char *new_pin = NULL;
    if (pin) {
        new_pin = cJSON_IsString(pin) ? strip(pin->valuestring) : NULL;
        if (new_pin && !new_pin[0]) {
            new_pin = NULL;
        }
        const char *current = json_str(device, "owner_pin", NULL);
        bool heard = false;
        const cJSON *sighting;
        cJSON_ArrayForEach(sighting, cJSON_GetObjectItem(device, "sightings")) {
            heard |= new_pin && strcmp(json_str(sighting, "node_id", ""), new_pin) == 0
                     && json_bool(sighting, "fresh", false);
        }
        if (new_pin && !(current && strcmp(current, new_pin) == 0) && !heard) {
            result = web_send_error(req, "400 Bad Request", "error.nodeNotHeard", NULL);
            goto done;
        }
    }

    // Labels, the auto flag and the pin spread to the other nodes by timestamp.
    double now = store_now();
    store_lock();
    const cJSON *original = jobs_record(key);
    cJSON *record = original ? cJSON_Duplicate(original, true) : NULL;
    bool saved = record != NULL;
    if (saved && !cJSON_GetObjectItem(record, "address")) {
        saved = json_set(record, "address", cJSON_CreateString(json_str(device, "address", key)))
            != NULL;
    }
    if (saved && new_label) {
        saved = json_set(record, "label", cJSON_CreateString(new_label))
            && json_set(record, "label_at", cJSON_CreateNumber(now));
    }
    if (saved && cJSON_IsBool(automatic)) {
        saved = json_set(record, "auto", cJSON_CreateBool(cJSON_IsTrue(automatic)))
            && json_set(record, "auto_at", cJSON_CreateNumber(now));
    }
    if (saved && pin) {
        saved = json_set(record, "owner_pin", new_pin ? cJSON_CreateString(new_pin) : cJSON_CreateNull())
            && json_set(record, "pin_at", cJSON_CreateNumber(now));
    }
    if (saved) {
        saved = json_set(store_devices(), key, record) != NULL;
        record = NULL;
    }
    cJSON_Delete(record);
    store_unlock();
    if (!saved) {
        result = unavailable(req);
        goto done;
    }
    store_save_devices();
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    result = web_send_json(req, "200 OK", reply);

done:
    cJSON_Delete(device);
    cJSON_Delete(body);
    return result;
}

// ---------------------------------------------------------------- node to node
//
// What a peer calls. These act on this node's own radio and never forward
// anywhere else, so a request can only ever travel one hop.

static bool path_is(httpd_req_t *req, const char *path)
{
    size_t length = strcspn(req->uri, "?");
    return length == strlen(path) && strncmp(req->uri, path, length) == 0;
}

static esp_err_t cluster_off(httpd_req_t *req)
{
    return web_send_error(req, "503 Service Unavailable", "error.clusterOff", NULL);
}

static esp_err_t get_node(httpd_req_t *req)
{
    if (!cluster_enabled()) {
        return cluster_off(req);
    }
    if (path_is(req, "/api/node/logs")) {
        char query[96] = "", boot[17] = "", after[16] = "0";
        size_t length = httpd_req_get_url_query_len(req);
        if (length >= sizeof(query) || (length &&
                httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)) {
            return bad_field(req, "cursor");
        }
        esp_err_t boot_result = httpd_query_key_value(query, "boot", boot, sizeof(boot));
        esp_err_t after_result = httpd_query_key_value(query, "after", after, sizeof(after));
        char *rest;
        errno = 0;
        unsigned long cursor = strtoul(after, &rest, 10);
        if ((boot_result != ESP_OK && boot_result != ESP_ERR_NOT_FOUND)
                || (after_result != ESP_OK && after_result != ESP_ERR_NOT_FOUND)
                || !valid_log_boot(boot) || !after[0] || *rest || errno == ERANGE
                || strspn(after, "0123456789") != strlen(after) || cursor > UINT32_MAX) {
            return bad_field(req, "cursor");
        }
        cJSON *held = cJSON_CreateObject();
        if (!json_set(held, "boot", cJSON_CreateString(boot))
                || !json_set(held, "after", cJSON_CreateNumber(cursor))) {
            cJSON_Delete(held);
            return web_send_json(req, "200 OK", NULL);
        }
        log_line_t lines[LOG_FETCH];
        cJSON *metadata;
        int count = local_log_source(held, &metadata, lines);
        cJSON_Delete(held);
        if (count < 0) {
            return web_send_json(req, "200 OK", NULL);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t err = send_log_source(req, metadata, lines, count);
        logs_free(lines, count);
        return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
    }
    bool status_only = path_is(req, "/api/node/status");
    if (!status_only && !path_is(req, "/api/node/state")) {
        return not_found(req, HTTPD_404_NOT_FOUND);
    }
    // A probe is also an announcement: on a network where our broadcast never
    // arrives, this is the only thing that tells us the caller is out there.
    char query[48], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
            && httpd_query_key_value(query, "port", value, sizeof(value)) == ESP_OK) {
        char *rest;
        long port = strtol(value, &rest, 10);
        if (!*rest && port > 0) {
            cluster_hear(client_ipv4(req), (int)port);
        }
    }

    cJSON *body = cJSON_CreateObject();
    bool ok = json_set(body, "node", node_view()) != NULL;
    ok = json_set(body, "busy", jobs_busy()) && ok;
    ok = json_set(body, "queued", cJSON_CreateNumber(jobs_queued())) && ok;
    if (!status_only) {
        ok = json_set(body, "host_time", cJSON_CreateNumber(store_now())) && ok;
        store_lock();
        ok = json_set(body, "devices", jobs_local_views()) && ok;
        store_unlock();
        char boot[17];
        uint32_t cursor;
        logs_version(boot, &cursor);
        ok = json_set(body, "log_boot", cJSON_CreateString(boot)) && ok;
        ok = json_set(body, "log_cursor", cJSON_CreateNumber(cursor)) && ok;
    }
    if (!ok) {
        cJSON_Delete(body);
        body = NULL;
    }
    return web_send_json(req, "200 OK", body);
}

static esp_err_t run_node_device(httpd_req_t *req)
{
    char key[24], action[12];
    bool write;
    if (!visit_target(req, "/api/node/devices/", key, action, &write)) {
        return ESP_OK;
    }
    return visit_here(req, key, write);
}

static bool text_field(const cJSON *body, const char *key, size_t low, size_t high)
{
    const cJSON *item = cJSON_GetObjectItem(body, key);
    if (!cJSON_IsString(item)) {
        return false;
    }
    size_t length = utf8_length(item->valuestring);
    return length >= low && length <= high;
}

static esp_err_t node_lock(httpd_req_t *req, const cJSON *body)
{
    const cJSON *claim_op = cJSON_GetObjectItem(body, "op");
    const char *bad = !cJSON_IsObject(body) ? "body"
                      : !text_field(body, "token", 8, 64) ? "token"
                      : !text_field(body, "holder_id", 1, 64) ? "holder_id"
                      : !text_field(body, "holder_name", 1, 64) ? "holder_name"
                      : claim_op && !cJSON_IsObject(claim_op) && !cJSON_IsNull(claim_op) ? "op"
                      : NULL;
    if (bad) {
        return bad_field(req, bad);
    }
    return web_send_json(req, "200 OK", cluster_grant(body));
}

// A cluster scan or read is one operation: the coordinator holds the lock for
// all of us, so this must not try to take it again.
static esp_err_t node_job(httpd_req_t *req, const cJSON *body, const char *key,
                          const char *event, job_kind_t kind)
{
    if (!cluster_holds(json_str(body, "token", NULL))) {
        return web_send_error(req, "409 Conflict", "error.staleToken", NULL);
    }
    if (!wifi_time_synced()) {
        return web_send_error(req, "503 Service Unavailable", "error.timeNotSet", NULL);
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "queued", jobs_enqueue(key, op(event, NULL), kind, NULL,
                                                        false, NULL));
    return web_send_json(req, "200 OK", reply);
}

static esp_err_t post_node(httpd_req_t *req)
{
    if (!cluster_enabled()) {
        return cluster_off(req);
    }
    if (strncmp(req->uri, "/api/node/devices/", strlen("/api/node/devices/")) == 0) {
        // Taking the lock means asking every peer, so this cannot run here.
        return defer(req, run_node_device);
    }
    cJSON *body = read_json(req);
    esp_err_t result;
    if (path_is(req, "/api/node/lock")) {
        result = node_lock(req, body);
    } else if (path_is(req, "/api/node/unlock")) {
        cluster_drop(json_str(body, "token", NULL));
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(reply, "released", true);
        result = web_send_json(req, "200 OK", reply);
    } else if (path_is(req, "/api/node/scan")) {
        result = node_job(req, body, "scan", "busy.scan", JOB_SCAN);
    } else if (path_is(req, "/api/node/read")) {
        result = node_job(req, body, "readOwned", "busy.readOwned", JOB_READ_OWNED);
    } else if (path_is(req, "/api/node/logs/clear")) {
        logs_clear();
        result = no_logs(req);
    } else {
        result = not_found(req, HTTPD_404_NOT_FOUND);
    }
    cJSON_Delete(body);
    return result;
}

// ---------------------------------------------------------------- settings

static cJSON *settings_view(void)
{
    cJSON *view = cJSON_CreateObject();
    char name[97];
    store_node_name(name, sizeof(name));
    store_lock();
    const cJSON *config = store_config();
    cJSON *node = cJSON_Duplicate(cJSON_GetObjectItem(config, "node"), true);
    bool ok = json_set(node, "name", cJSON_CreateString(name)) != NULL;
    ok = json_set(view, "node", node) && ok;
    ok = json_set(view, "host", cJSON_CreateString("0.0.0.0")) && ok;
    ok = json_set(view, "port", cJSON_CreateNumber(PEER_DEFAULT_PORT)) && ok;
    ok = json_set(view, "schedule",
                  cJSON_Duplicate(cJSON_GetObjectItem(config, "schedule"), true)) && ok;
    ok = json_set(view, "bluetooth",
                  cJSON_Duplicate(cJSON_GetObjectItem(config, "bluetooth"), true)) && ok;
    ok = json_set(view, "time_server", cJSON_CreateString(json_str(config, "time_server", ""))) && ok;
    ok = json_set(view, "zone", cJSON_CreateString(json_str(config, "zone", ""))) && ok;
    ok = json_set(view, "timezone", cJSON_CreateString(json_str(config, "timezone", ""))) && ok;
    store_unlock();
    ok = json_set(view, "platform", cJSON_CreateString("esp32")) && ok;
    ok = json_set(view, "firmware", ota_firmware_json()) && ok;
    ok = json_set(view, "wifi", wifi_status_json()) && ok;
    if (!ok) {
        cJSON_Delete(view);
        return NULL;
    }
    return view;
}

static esp_err_t get_settings(httpd_req_t *req)
{
    return web_send_json(req, "200 OK", settings_view());
}

static esp_err_t put_settings(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    if (!body) {
        return bad_field(req, "body");
    }
    esp_err_t result;
    const char *bad = NULL;
    cJSON *node = cJSON_CreateObject(), *schedule = cJSON_CreateObject();
    cJSON *radio = cJSON_CreateObject(), *updates = cJSON_CreateObject();
    if (!node || !schedule || !radio || !updates) {
        result = unavailable(req);
        goto done;
    }

    // ---- node
    const cJSON *in_node = cJSON_GetObjectItem(body, "node");
    const cJSON *in_name = cJSON_GetObjectItem(in_node, "name");
    const cJSON *in_peers = cJSON_GetObjectItem(in_node, "peers");
    bool cluster_on = true, discovery_on = true;
    if (!cJSON_IsObject(in_node)) {
        bad = "node";
    } else if (!cJSON_IsString(in_name) || utf8_length(in_name->valuestring) < 1
               || utf8_length(in_name->valuestring) > 24) {
        bad = "name";
    } else if (in_peers && (!cJSON_IsArray(in_peers) || cJSON_GetArraySize(in_peers) > 8)) {
        bad = "peers";
    } else if (!boolean(in_node, "cluster", true, &cluster_on)) {
        bad = "cluster";
    } else if (!boolean(in_node, "discovery", true, &discovery_on)) {
        bad = "discovery";
    }

    // ---- schedule
    const cJSON *in_schedule = cJSON_GetObjectItem(body, "schedule");
    const char *mode = json_str(in_schedule, "mode", "");
    double weekday, hour, minute;
    if (bad) {
    } else if (strcmp(mode, "weekly") && strcmp(mode, "daily") && strcmp(mode, "off")) {
        bad = "mode";
    } else if (!number(in_schedule, "weekday", 0, 6, true, &weekday)) {
        bad = "weekday";
    } else if (!number(in_schedule, "hour", 0, 23, true, &hour)) {
        bad = "hour";
    } else if (!number(in_schedule, "minute", 0, 59, true, &minute)) {
        bad = "minute";
    }

    // ---- bluetooth
    const cJSON *in_radio = cJSON_GetObjectItem(body, "bluetooth");
    double scan, connect, env, retries;
    if (bad) {
    } else if (!number(in_radio, "scan_timeout", 5, 120, false, &scan)) {
        bad = "scan_timeout";
    } else if (!number(in_radio, "connect_timeout", 5, 120, false, &connect)) {
        bad = "connect_timeout";
    } else if (!number(in_radio, "env_timeout", 1, 60, false, &env)) {
        bad = "env_timeout";
    } else if (!number(in_radio, "retries", 1, 10, true, &retries)) {
        bad = "retries";
    }

    const cJSON *in_server = cJSON_GetObjectItem(body, "time_server");
    const char *server = "";
    if (!bad && in_server) {
        server = cJSON_IsString(in_server) ? in_server->valuestring : NULL;
        if (!server || strlen(server) > 64 || strspn(server, HOST_CHARS) != strlen(server)) {
            bad = "time_server";
        }
    }
    if (bad) {
        result = bad_field(req, bad);
        goto done;
    }

    char *name = strip(in_name->valuestring);
    if (!name[0]) {
        result = web_send_error(req, "400 Bad Request", "error.emptyNodeName", NULL);
        goto done;
    }
    // Same as server.py: the field is pre-filled with the hostname, and writing
    // that back must not freeze it into the settings.
    if (!json_set(node, "name", cJSON_CreateString(strcmp(name, store_hostname()) ? name : ""))) {
        result = unavailable(req);
        goto done;
    }

    cJSON *peers = json_set(node, "peers", cJSON_CreateArray());
    if (!peers) {
        result = unavailable(req);
        goto done;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, in_peers) {
        if (!cJSON_IsString(entry)) {
            result = bad_field(req, "peers");
            goto done;
        }
        char url[300];
        char *text = strip(entry->valuestring);
        if (!text[0]) {
            continue;
        }
        if (!cluster_normalize_peer(text, PEER_DEFAULT_PORT, url, sizeof(url))) {
            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "peer", text);
            result = web_send_error(req, "400 Bad Request", "error.badPeer", params);
            goto done;
        }
        bool seen = false;
        const cJSON *have;
        cJSON_ArrayForEach(have, peers) {
            seen = seen || strcmp(have->valuestring, url) == 0;
        }
        if (!seen) {
            cJSON *item = cJSON_CreateString(url);
            if (!item || !cJSON_AddItemToArray(peers, item)) {
                cJSON_Delete(item);
                result = unavailable(req);
                goto done;
            }
        }
    }
    if (!json_set(node, "cluster", cJSON_CreateBool(cluster_on))
            || !json_set(node, "discovery", cJSON_CreateBool(discovery_on))
            || !json_set(schedule, "mode", cJSON_CreateString(mode))
            || !json_set(schedule, "weekday", cJSON_CreateNumber(weekday))
            || !json_set(schedule, "hour", cJSON_CreateNumber(hour))
            || !json_set(schedule, "minute", cJSON_CreateNumber(minute))
            || !json_set(radio, "scan_timeout", cJSON_CreateNumber(scan))
            || !json_set(radio, "connect_timeout", cJSON_CreateNumber(connect))
            || !json_set(radio, "env_timeout", cJSON_CreateNumber(env))
            || !json_set(radio, "retries", cJSON_CreateNumber(retries))) {
        result = unavailable(req);
        goto done;
    }
    bool complete = json_set(updates, "node", node) != NULL;
    node = NULL;
    complete = json_set(updates, "schedule", schedule) && complete;
    schedule = NULL;
    complete = json_set(updates, "bluetooth", radio) && complete;
    radio = NULL;
    complete = json_set(updates, "time_server", cJSON_CreateString(server)) && complete;
    if (!complete) {
        result = unavailable(req);
        goto done;
    }

    store_lock();
    cJSON *config = store_config();
    // Every key exists after defaults; replacing prepared children cannot allocate.
    while (updates->child) {
        cJSON *item = cJSON_DetachItemViaPointer(updates, updates->child);
        bool replaced = cJSON_ReplaceItemViaPointer(config,
            cJSON_GetObjectItemCaseSensitive(config, item->string), item);
        configASSERT(replaced);
    }
    store_unlock();
    store_save_config();
    announce_name();
    // Either switch may just have been flipped, and both loops are asleep.
    cluster_wake();
    logs_put("settings.saved", NULL);

    cJSON *reply = cJSON_CreateObject();
    json_set(reply, "settings", settings_view());
    cJSON_AddBoolToObject(reply, "restart_required", false);
    result = web_send_json(req, "200 OK", reply);

done:
    cJSON_Delete(node);
    cJSON_Delete(schedule);
    cJSON_Delete(radio);
    cJSON_Delete(updates);
    cJSON_Delete(body);
    return result;
}

static esp_err_t put_language(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    const char *chosen = json_str(body, "language", "");
    if (strcmp(chosen, "zh-CN") && strcmp(chosen, "zh-TW") && strcmp(chosen, "en")) {
        cJSON_Delete(body);
        return bad_field(req, "language");
    }
    store_lock();
    bool saved = json_set(store_config(), "language", cJSON_CreateString(chosen)) != NULL;
    store_unlock();
    if (!saved) {
        cJSON_Delete(body);
        return unavailable(req);
    }
    store_save_config();
    logs_put("settings.saved", NULL);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "language", chosen);
    cJSON_Delete(body);
    return web_send_json(req, "200 OK", reply);
}

// The page sends the browser's own zone the first time it finds none, so the
// schedule runs on local time before anybody opens the settings.
static esp_err_t put_timezone(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    char zone[41], rule[65];
    const char *bad = read_zone(body, zone, sizeof(zone), rule, sizeof(rule));
    cJSON_Delete(body);
    if (bad) {
        return bad_field(req, bad);
    }
    store_lock();
    bool saved = apply_zone(zone, rule);
    store_unlock();
    if (!saved) {
        return unavailable(req);
    }
    store_save_config();
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "zone", zone);
    cJSON_AddStringToObject(reply, "timezone", rule);
    return web_send_json(req, "200 OK", reply);
}

// ---------------------------------------------------------------- wifi

static esp_err_t get_wifi(httpd_req_t *req)
{
    cJSON *status = wifi_status_json();
    cJSON_AddStringToObject(status, "language", language());
    // Tells /config whether to offer only the network form or every setting.
    cJSON_AddBoolToObject(status, "hotspot", on_hotspot(req));
    return web_send_json(req, "200 OK", status);
}

static esp_err_t get_wifi_scan(httpd_req_t *req)
{
    cJSON *networks = wifi_scan_json();
    cJSON *body = cJSON_CreateObject();
    if (!json_set(body, "networks", networks)) {
        cJSON_Delete(body);
        body = NULL;
    }
    return web_send_json(req, "200 OK", body);
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    cJSON *body = read_json(req);
    const char *ssid = json_str(body, "ssid", NULL);
    const char *password = json_str(body, "password", "");
    const char *hostname = json_str(body, "hostname", NULL);
    esp_err_t err = ssid ? wifi_join(ssid, password, hostname) : ESP_ERR_INVALID_ARG;
    esp_err_t result;
    if (err == ESP_ERR_INVALID_ARG) {
        const char *field = !ssid || !ssid[0] || strlen(ssid) > 32 ? "ssid"
                            : hostname && hostname[0] && !store_hostname_valid(hostname)
                                ? "hostname" : "password";
        result = bad_field(req, field);
    } else if (err != ESP_OK) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "error", esp_err_to_name(err));
        result = web_send_error(req, "500 Internal Server Error", "error.wifiFailed", params);
    } else {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(reply, "ok", true);
        result = web_send_json(req, "200 OK", reply);
    }
    cJSON_Delete(body);
    return result;
}

// ---------------------------------------------------------------- server

static atomic_bool ready;

static esp_err_t portal_redirect(httpd_req_t *req)
{
    uint32_t ap = wifi_ap_ip();
    if (!ap || !on_hotspot(req)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    char location[48];
    snprintf(location, sizeof(location), "http://" IPSTR ":8787/config",
             IP2STR((esp_ip4_addr_t *)&ap));
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_not_found(httpd_req_t *req, httpd_err_code_t code)
{
    return portal_redirect(req);
}

void web_portal_update(void)
{
    static httpd_handle_t portal;
    static int64_t retry_at;
    if (!atomic_load(&ready)) {
        return;
    }
    if (!wifi_ap_ip()) {
        if (portal && httpd_stop(portal) == ESP_OK) {
            portal = NULL;
        }
        return;
    }
    if (portal || esp_timer_get_time() < retry_at) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port++;
    config.max_uri_handlers = 1;
    config.max_open_sockets = 1;
    config.stack_size = 3072;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    const httpd_uri_t route = {.uri = "/", .method = HTTP_GET, .handler = portal_redirect};
    esp_err_t err = httpd_start(&portal, &config);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(portal, &route);
    }
    if (err == ESP_OK) {
        err = httpd_register_err_handler(portal, HTTPD_404_NOT_FOUND, portal_not_found);
    }
    if (err != ESP_OK) {
        if (portal) {
            httpd_stop(portal);
            portal = NULL;
        }
        retry_at = esp_timer_get_time() + 30 * 1000000;
        ESP_LOGW(TAG, "portal listener failed: %s", esp_err_to_name(err));
    }
}

esp_err_t web_start(void)
{
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    snprintf(etag, sizeof(etag), "\"%s\"", sha);

    deferred = xQueueCreate(4, sizeof(deferred_t));
    configASSERT(deferred);
    configASSERT(xTaskCreate(relay_task, "relay", 8192, NULL, 5, NULL) == pdPASS);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PEER_DEFAULT_PORT;
    config.max_uri_handlers = 32;
    // Leave sockets for the hotspot redirector, DNS, beacon and one HTTP client.
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.stack_size = 10240;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 3;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server did not start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_root},
        {.uri = "/config", .method = HTTP_GET, .handler = get_config},
        {.uri = "/static/*", .method = HTTP_GET, .handler = get_static},
        {.uri = "/api/state", .method = HTTP_GET, .handler = get_state},
        {.uri = "/api/ping", .method = HTTP_GET, .handler = get_ping},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = post_scan},
        {.uri = "/api/read", .method = HTTP_POST, .handler = post_read},
        {.uri = "/api/logs", .method = HTTP_POST, .handler = post_logs},
        {.uri = "/api/logs/clear", .method = HTTP_POST, .handler = post_clear_logs},
        {.uri = "/api/devices/*", .method = HTTP_POST, .handler = post_device},
        {.uri = "/api/devices/*", .method = HTTP_PATCH, .handler = patch_device},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = get_settings},
        {.uri = "/api/settings", .method = HTTP_PUT, .handler = put_settings},
        {.uri = "/api/language", .method = HTTP_PUT, .handler = put_language},
        {.uri = "/api/timezone", .method = HTTP_PUT, .handler = put_timezone},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = get_wifi_scan},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_upload},
        {.uri = "/api/node/*", .method = HTTP_GET, .handler = get_node},
        {.uri = "/api/node/*", .method = HTTP_POST, .handler = post_node},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            httpd_stop(server);
            return err;
        }
    }
    err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }
    atomic_store(&ready, true);
    return ESP_OK;
}
