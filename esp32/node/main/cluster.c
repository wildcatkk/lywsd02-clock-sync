#include "cluster.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jobs.h"
#include "logs.h"
#include "lwip/sockets.h"
#include "lywsd02.h"
#include "store.h"
#include "wifi.h"

#define MAX_PEERS CLUSTER_MAX_PEERS
#define MAX_SOURCES (MAX_PEERS + 1)
#define BEACON_PORT 8788
#define BEACON_FAST 10.0
#define BEACON_SLOW 60.0
#define BEACON_GAP 2.0
#define BEACON_TTL 150.0
#define POLL_WATCHED 2.0
#define POLL_IDLE 60.0
#define POLL_ALONE 300.0
#define WATCH_WINDOW 15.0
#define FRESH_ENOUGH 3.0
#define DOWN_GRACE 30.0
#define DOWN_BACKOFF 60.0
#define OWN_PORT PEER_DEFAULT_PORT
#define REPLY_MAX (32 * 1024)
#define LOCAL_FAILURE (-1)
#define PEER_LOGS 60
#define LOG_FETCH 20
#define LONG_AGO (-1e9)
#define POLL_BIT BIT0
#define BEACON_BIT BIT1

static const char *TAG = "cluster";

bool cluster_normalize_peer(const char *entry, int default_port, char *out, size_t size)
{
    while (isspace((unsigned char)*entry)) {
        entry++;
    }
    const char *scheme = strstr(entry, "://");
    if (scheme) {
        entry = scheme + 3;
    }
    const char *end = entry + strlen(entry);
    while (end > entry && isspace((unsigned char)end[-1])) {
        end--;
    }
    while (entry < end && *entry == '/') {
        entry++;
    }
    while (end > entry && end[-1] == '/') {
        end--;
    }
    if (entry == end) {
        return false;
    }

    const char *colon = memchr(entry, ':', end - entry);
    const char *host_end = colon ? colon : end;
    if (host_end == entry || host_end - entry > 253) {
        return false;
    }
    for (const char *c = entry; c < host_end; c++) {
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') {
            return false;
        }
    }

    long port = default_port;
    if (colon && colon + 1 < end) {
        char digits[8];
        size_t length = end - colon - 1;
        if (length >= sizeof(digits)) {
            return false;
        }
        memcpy(digits, colon + 1, length);
        digits[length] = 0;
        char *rest;
        port = strtol(digits, &rest, 10);
        if (*rest || !isdigit((unsigned char)digits[0])) {
            return false;
        }
    }
    if (port < 1 || port > 65535) {
        return false;
    }
    int written = snprintf(out, size, "http://%.*s:%ld", (int)(host_end - entry), entry, port);
    return written > 0 && (size_t)written < size;
}

int cluster_bucket(double rssi)
{
    if (!isfinite(rssi)) {
        return 0;
    }
    double bucket = rint(rssi / CLUSTER_BUCKET);
    // Ownership compares negated buckets, so INT_MIN is never a valid bucket.
    return bucket >= INT_MAX ? INT_MAX : bucket <= -INT_MAX ? -INT_MAX : (int)bucket;
}

// The first `chars` characters of `text`, cut where a whole one fits in `size`.
static void copy_text(char *out, size_t size, const char *text, size_t chars)
{
    size_t used = 0, count = 0;
    while (text[used]) {
        size_t length = 1;
        while (((unsigned char)text[used + length] & 0xC0) == 0x80) {
            length++;
        }
        if (count == chars || used + length >= size) {
            break;
        }
        used += length;
        count++;
    }
    memcpy(out, text, used);
    out[used] = 0;
}

// ---------------------------------------------------------------- peers

typedef struct {
    bool used;
    bool manual;
    char url[96];
    char node_id[65];
    char name[97];
    double last_beacon;
    double probed;
    double attempted;
    double down_since;
    bool online;
    bool has_state;
    cJSON *error;     // a string, or the {event, params} a peer refused with
    cJSON *devices;   // its device_view() list
    cJSON *busy;      // NULL when idle
    int queued;
    log_line_t *logs;
    int log_count;
    char log_boot[17];
    uint32_t log_cursor;
} peer_t;

// Guarded by store_lock(), as are `refreshed` and `watched_at`.
static peer_t peers[MAX_PEERS];
static double refreshed = LONG_AGO;
static double watched_at = LONG_AGO;
static EventGroupHandle_t nudges;
// One round of probes at a time; whoever asks during one waits for it instead.
static SemaphoreHandle_t refreshing;
static SemaphoreHandle_t http_gate;

double cluster_clock(void)
{
    return esp_timer_get_time() / 1e6;
}

static peer_t *find_peer(const char *url)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].used && strcmp(peers[i].url, url) == 0) {
            return &peers[i];
        }
    }
    return NULL;
}

static void drop_logs(peer_t *peer)
{
    if (peer->logs) {
        logs_free(peer->logs, peer->log_count);
        free(peer->logs);
    }
    peer->logs = NULL;
    peer->log_count = 0;
    peer->log_boot[0] = 0;
    peer->log_cursor = 0;
}

static void forget(peer_t *peer)
{
    cJSON_Delete(peer->error);
    cJSON_Delete(peer->devices);
    cJSON_Delete(peer->busy);
    drop_logs(peer);
    memset(peer, 0, sizeof(*peer));
}

static peer_t *touch(const char *url, bool manual)
{
    peer_t *peer = find_peer(url);
    if (peer) {
        // A hand-typed peer outlives its beacons, so the stricter origin wins.
        peer->manual |= manual;
        return peer;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].used) {
            peer = &peers[i];
            memset(peer, 0, sizeof(*peer));
            peer->used = true;
            peer->manual = manual;
            snprintf(peer->url, sizeof(peer->url), "%s", url);
            peer->last_beacon = LONG_AGO;
            peer->probed = LONG_AGO;
            peer->attempted = LONG_AGO;
            peer->down_since = cluster_clock();
            return peer;
        }
    }
    return NULL;
}

static bool setting(const char *key)
{
    store_lock();
    bool on = json_bool(cJSON_GetObjectItem(store_config(), "node"), key, true);
    store_unlock();
    return on;
}

bool cluster_enabled(void)
{
    return setting("cluster");
}

static bool discovering(void)
{
    return cluster_enabled() && setting("discovery");
}

void cluster_wake(void)
{
    xEventGroupSetBits(nudges, POLL_BIT | BEACON_BIT);
}

static void nap(EventBits_t bit, double seconds)
{
    xEventGroupWaitBits(nudges, bit, pdTRUE, pdFALSE, pdMS_TO_TICKS((uint32_t)(seconds * 1000)));
}

static bool watched(void)
{
    store_lock();
    bool watching = cluster_clock() - watched_at < WATCH_WINDOW;
    store_unlock();
    return watching;
}

void cluster_watch(void)
{
    // Only the first request wakes the loops; every one of them doing it would
    // send a beacon every two seconds.
    bool was = watched();
    store_lock();
    watched_at = cluster_clock();
    store_unlock();
    if (!was) {
        cluster_wake();
    }
}

// It just spoke to us, so whatever backoff it was in is over. A peer we already
// woke the loops for is not woken for again until it has been probed, or two
// nodes that cannot reach each other would answer each other's beacons forever.
static bool heard(peer_t *peer, bool news)
{
    bool wake = news && peer->attempted != LONG_AGO;
    if (news) {
        peer->attempted = LONG_AGO;
    }
    return wake;
}

// ---------------------------------------------------------------- HTTP

static cJSON *parse_bounded(const char *text, size_t len);

static void transport_error(esp_http_client_handle_t client, esp_err_t err, char *out,
                            size_t size)
{
    int code = esp_http_client_get_errno(client);
    if (err == ESP_ERR_HTTP_EAGAIN || err == -ESP_ERR_HTTP_EAGAIN || code == EAGAIN
            || code == ETIMEDOUT) {
        snprintf(out, size, "timed out");
    } else if (code) {
        snprintf(out, size, "[Errno %d] %s", code, strerror(code));
    } else {
        snprintf(out, size, "%s", esp_err_to_name(err));
    }
}

static int remaining_ms(double deadline)
{
    double left = ceil((deadline - cluster_clock()) * 1000);
    return left <= 0 ? 0 : left >= INT_MAX ? INT_MAX : (int)left;
}

static bool http_remaining(esp_http_client_handle_t client, double deadline,
                           char *error, size_t size)
{
    int left = remaining_ms(deadline);
    if (!left) {
        snprintf(error, size, "timed out");
        return false;
    }
    esp_http_client_set_timeout_ms(client, left);
    return true;
}

static void fetch_error(esp_http_client_handle_t client, esp_err_t err, int *status,
                        char *error, size_t size)
{
    if (err == ESP_ERR_NO_MEM || err == -ESP_ERR_NO_MEM
            || esp_http_client_get_errno(client) == ENOMEM) {
        *status = LOCAL_FAILURE;
    }
    transport_error(client, err, error, size);
}

// The gate and all transport stages share one monotonic deadline.
static char *fetch(const char *url, const char *path, const char *body, double timeout,
                   int *status, char *error, size_t error_size)
{
    *status = 0;
    error[0] = 0;
    if (!isfinite(timeout) || timeout <= 0) {
        snprintf(error, error_size, "timed out");
        return NULL;
    }
    double deadline = cluster_clock() + timeout;
    int left = remaining_ms(deadline);
    if (!left || xSemaphoreTake(http_gate, pdMS_TO_TICKS((uint32_t)left)) != pdTRUE) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "timed out waiting for local HTTP slot");
        return NULL;
    }
    char target[192];
    int written = snprintf(target, sizeof(target), "%s%s", url, path);
    size_t body_length = body ? strlen(body) : 0;
    if (written < 0 || (size_t)written >= sizeof(target) || body_length > INT_MAX) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "HTTP request exceeds local limit");
        xSemaphoreGive(http_gate);
        return NULL;
    }
    left = remaining_ms(deadline);
    if (!left) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "local HTTP admission deadline");
        xSemaphoreGive(http_gate);
        return NULL;
    }
    esp_http_client_config_t config = {
        .url = target,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = left,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "out of memory initializing HTTP");
        xSemaphoreGive(http_gate);
        return NULL;
    }
    char *text = NULL;
    int length = (int)body_length;
    if (body) {
        esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err != ESP_OK) {
            fetch_error(client, err, status, error, error_size);
            goto done;
        }
    }
    if (!http_remaining(client, deadline, error, error_size)) {
        goto done;
    }
    esp_err_t err = esp_http_client_open(client, length);
    if (err != ESP_OK) {
        fetch_error(client, err, status, error, error_size);
        goto done;
    }
    for (int sent = 0; sent < length;) {
        if (!http_remaining(client, deadline, error, error_size)) {
            goto done;
        }
        int n = esp_http_client_write(client, body + sent, length - sent);
        if (n <= 0) {
            fetch_error(client, n < 0 ? n : ESP_FAIL, status, error, error_size);
            goto done;
        }
        sent += n;
    }
    if (!http_remaining(client, deadline, error, error_size)) {
        goto done;
    }
    int64_t content = esp_http_client_fetch_headers(client);
    if (content < 0) {
        fetch_error(client, (esp_err_t)content, status, error, error_size);
        goto done;
    }
    if (!remaining_ms(deadline)) {
        snprintf(error, error_size, "timed out");
        goto done;
    }
    if (content > REPLY_MAX) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "answer exceeds local limit");
        goto done;
    }
    int capacity = content > 0 ? (int)content + 1 : 2048;
    int have = 0;
    text = malloc(capacity);
    // Not "until complete": a body that came in with the headers is already
    // counted as received while it still waits in the client's cache.
    while (text && !(content > 0 && have == content)) {
        if (!http_remaining(client, deadline, error, error_size)) {
            free(text);
            text = NULL;
            goto done;
        }
        if (have + 1 >= capacity) {
            if (capacity >= REPLY_MAX + 1) {
                char extra;
                int got = esp_http_client_read(client, &extra, 1);
                if (!got && esp_http_client_is_complete_data_received(client)) {
                    break;
                }
                if (got < 0) {
                    fetch_error(client, got, status, error, error_size);
                } else if (!got) {
                    snprintf(error, error_size, "incomplete answer");
                } else {
                    *status = LOCAL_FAILURE;
                    snprintf(error, error_size, "answer exceeds local limit");
                }
                free(text);
                text = NULL;
                goto done;
            }
            capacity = capacity * 2 > REPLY_MAX + 1 ? REPLY_MAX + 1 : capacity * 2;
            char *grown = realloc(text, capacity);
            if (!grown) {
                free(text);
                text = NULL;
                break;
            }
            text = grown;
        }
        if (!http_remaining(client, deadline, error, error_size)) {
            free(text);
            text = NULL;
            goto done;
        }
        int got = esp_http_client_read(client, text + have, capacity - have - 1);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            fetch_error(client, got, status, error, error_size);
            free(text);
            text = NULL;
            goto done;
        }
        have += got;
    }
    if (!text) {
        *status = LOCAL_FAILURE;
        if (!error[0]) {
            snprintf(error, error_size, "out of memory receiving HTTP");
        }
        goto done;
    }
    if (!remaining_ms(deadline) || !esp_http_client_is_complete_data_received(client)) {
        snprintf(error, error_size, "%s", remaining_ms(deadline) ? "incomplete answer" : "timed out");
        free(text);
        text = NULL;
        goto done;
    }
    if (memchr(text, 0, have)) {
        *status = LOCAL_FAILURE;
        snprintf(error, error_size, "answer contains embedded NUL");
        free(text);
        text = NULL;
        goto done;
    }
    text[have] = 0;
    *status = esp_http_client_get_status_code(client);
done:
    esp_http_client_cleanup(client);
    xSemaphoreGive(http_gate);
    return text;
}

// Rejected.detail, or str(error), the way cluster.py would hold it.
static cJSON *failure_of(int status, const char *reply, const char *error)
{
    if (status <= 0) {
        return cJSON_CreateString(error);
    }
    cJSON *body = reply ? parse_bounded(reply, strlen(reply)) : NULL;
    cJSON *detail = cJSON_DetachItemFromObject(body, "detail");
    cJSON_Delete(body);
    bool usable = (cJSON_IsString(detail) && detail->valuestring[0])
                  || ((cJSON_IsObject(detail) || cJSON_IsArray(detail)) && detail->child);
    if (usable) {
        return detail;
    }
    cJSON_Delete(detail);
    char text[16];
    snprintf(text, sizeof(text), "HTTP %d", status);
    return cJSON_CreateString(text);
}

// The text of a 2xx answer, or NULL with *status the HTTP status of a refusal (0
// when nobody answered) and, when asked for, *failure.
static char *request(const char *url, const char *path, const char *body, double timeout,
                     int *status, cJSON **failure)
{
    char error[96] = "";
    char *text = fetch(url, path, body, timeout, status, error, sizeof(error));
    if (text && *status >= 200 && *status < 300) {
        return text;
    }
    if (failure) {
        *failure = failure_of(*status, text, error);
    }
    free(text);
    return NULL;
}

cJSON *cluster_call(const char *url, const char *path, const cJSON *payload, bool get,
                    double timeout, int *status, cJSON **failure)
{
    char *body = NULL;
    if (!get) {
        body = payload ? cJSON_PrintUnformatted(payload) : strdup("{}");
        if (!body) {
            if (status) {
                *status = LOCAL_FAILURE;
            }
            if (failure) {
                *failure = cJSON_CreateString("out of memory encoding HTTP");
            }
            return NULL;
        }
    }
    int code;
    char *text = request(url, path, body, timeout, &code, failure);
    free(body);
    if (status) {
        *status = code;
    }
    if (!text) {
        return NULL;
    }
    const char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    cJSON *answer = *start ? parse_bounded(start, strlen(start)) : cJSON_CreateObject();
    free(text);
    if (!answer) {
        if (status) {
            *status = LOCAL_FAILURE;
        }
        if (failure) {
            *failure = cJSON_CreateString("cannot parse answer");
        }
    }
    return answer;
}

// What a failure says, for a log line: the event of a refusal, or the text.
static const char *text_of(const cJSON *failure)
{
    if (cJSON_IsObject(failure)) {
        return json_str(failure, "event", "");
    }
    return cJSON_IsString(failure) ? failure->valuestring : "";
}

// ---------------------------------------------------------------- JSON scanning
// Logs are validated and copied one entry at a time, without a whole log tree.

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static const char *scan_string(const char *p, const char *end)
{
    if (p >= end || *p++ != '"') {
        return NULL;
    }
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            return p;
        }
        if (c < 0x20) {
            return NULL;
        }
        // cJSON passes any byte through, and a stored string that is not UTF-8
        // would make every later answer of ours unreadable to the other nodes.
        if (c >= 0x80) {
            size_t step = lywsd02_utf8_char(p - 1, end - p + 1);
            if (!step) {
                return NULL;
            }
            p += step - 1;
            continue;
        }
        if (c == '\\') {
            if (p == end) {
                return NULL;
            }
            c = (unsigned char)*p++;
            if (c == 'u') {
                if (end - p < 4) {
                    return NULL;
                }
                bool zero = true;
                for (int n = 0; n < 4; n++) {
                    if (!isxdigit((unsigned char)p[n])) {
                        return NULL;
                    }
                    zero &= p[n] == '0';
                }
                // cJSON strings cannot represent embedded NULs safely.
                if (zero) {
                    return NULL;
                }
                p += 4;
            } else if (!c || !strchr("\"\\/bfnrt", c)) {
                return NULL;
            }
        }
    }
    return NULL;
}

bool cluster_json_bounded(const char *text, size_t len)
{
    if (!text || !len) {
        return false;
    }
    const char *p = text, *end = text + len;
    // Object: 0 first key, 1 colon, 2 value, 3 delimiter, 4 next key.
    // Array: 5 first value, 6 delimiter, 7 next value.
    unsigned char state[16];
    int depth = 0;
    bool root = false;
    for (;;) {
        p = skip_space(p, end);
        if (!depth && root) {
            return p == end;
        }
        if (p == end) {
            return false;
        }
        if (depth) {
            unsigned char *s = &state[depth - 1];
            if ((*s == 0 || *s == 3) && *p == '}') {
                depth--;
                p++;
                continue;
            }
            if ((*s == 5 || *s == 6) && *p == ']') {
                depth--;
                p++;
                continue;
            }
            if (*s == 0 || *s == 4) {
                p = scan_string(p, end);
                if (!p) {
                    return false;
                }
                *s = 1;
                continue;
            }
            if (*s == 1) {
                if (*p++ != ':') {
                    return false;
                }
                *s = 2;
                continue;
            }
            if (*s == 3 || *s == 6) {
                if (*p++ != ',') {
                    return false;
                }
                *s = *s == 3 ? 4 : 7;
                continue;
            }
            *s = *s == 2 ? 3 : 6;
        } else {
            root = true;
        }
        if (*p == '{' || *p == '[') {
            if (depth == 16) {
                return false;
            }
            state[depth++] = *p++ == '{' ? 0 : 5;
        } else if (*p == '"') {
            p = scan_string(p, end);
            if (!p) {
                return false;
            }
        } else if (*p == 'n' || *p == 't' || *p == 'f') {
            const char *word = *p == 'n' ? "null" : *p == 't' ? "true" : "false";
            size_t n = strlen(word);
            if ((size_t)(end - p) < n || memcmp(p, word, n)) {
                return false;
            }
            p += n;
        } else {
            if (*p == '-') {
                p++;
            }
            if (p == end || *p < '0' || *p > '9') {
                return false;
            }
            if (*p++ != '0') {
                while (p < end && *p >= '0' && *p <= '9') {
                    p++;
                }
            }
            if (p < end && *p == '.') {
                p++;
                const char *first = p;
                while (p < end && *p >= '0' && *p <= '9') {
                    p++;
                }
                if (p == first) {
                    return false;
                }
            }
            if (p < end && (*p == 'e' || *p == 'E')) {
                p++;
                if (p < end && (*p == '+' || *p == '-')) {
                    p++;
                }
                const char *first = p;
                while (p < end && *p >= '0' && *p <= '9') {
                    p++;
                }
                if (p == first) {
                    return false;
                }
            }
        }
    }
}

static cJSON *parse_bounded(const char *text, size_t len)
{
    return cluster_json_bounded(text, len) ? cJSON_ParseWithLength(text, len) : NULL;
}

// Just past the JSON value at `p`, or NULL when it is cut short.
static const char *skip_value(const char *p, const char *end)
{
    p = skip_space(p, end);
    if (p >= end) {
        return NULL;
    }
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '\\') {
                p++;
            } else if (*p == '"') {
                return p + 1;
            }
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p = skip_value(p, end);
                if (!p) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if ((*p == '}' || *p == ']') && --depth == 0) {
                return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (p < end && *p && !strchr(",}] \t\r\n", *p)) {
        p++;
    }
    return p;
}

// The value of `key` in the object at `p`.
static bool member(const char *p, const char *end, const char *key, const char **start,
                   const char **stop)
{
    p = skip_space(p, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;
    size_t length = strlen(key);
    bool found = false;
    for (;;) {
        p = skip_space(p, end);
        if (p < end && *p == '}') {
            return found;
        }
        if (p >= end || *p != '"') {
            return false;
        }
        const char *name = p + 1;
        const char *after = skip_value(p, end);
        if (!after) {
            return false;
        }
        bool match = (size_t)(after - 1 - name) == length && memcmp(name, key, length) == 0;
        p = skip_space(after, end);
        if (p >= end || *p != ':') {
            return false;
        }
        const char *value = skip_space(p + 1, end);
        const char *value_end = skip_value(value, end);
        if (!value_end) {
            return false;
        }
        if (match) {
            if (found) {
                return false;
            }
            *start = value;
            *stop = value_end;
            found = true;
        }
        p = skip_space(value_end, end);
        if (p < end && *p == '}') {
            return found;
        }
        if (p >= end || *p != ',') {
            return false;
        }
        p++;
    }
}

// Walks an array: *cursor starts on its '[' and each call yields one element.
static bool next_element(const char **cursor, const char *end, const char **start,
                         const char **stop)
{
    const char *p = skip_space(*cursor, end);
    if (p >= end || (*p != '[' && *p != ',')) {
        return false;
    }
    p = skip_space(p + 1, end);
    if (p >= end || *p == ']') {
        return false;
    }
    const char *after = skip_value(p, end);
    if (!after) {
        return false;
    }
    *start = p;
    *stop = after;
    *cursor = after;
    return true;
}

static void local_problem(const char *url, const char *error)
{
    static double last = LONG_AGO;
    double now = cluster_clock();
    ESP_LOGW(TAG, "%s: %s; heap %u, largest %u", url, error,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    store_lock();
    if (now - last >= 30) {
        last = now;
        char text[256];
        snprintf(text, sizeof(text), "%s: %s", url, error);
        logs_put("cluster.localResource", "url", cJSON_CreateString(url),
                 "error", cJSON_CreateString(text), NULL);
    }
    store_unlock();
}

static bool uint32_value(const cJSON *value, uint32_t *out);
static bool boot_value(const cJSON *value);
static bool bounded_string(const cJSON *value, size_t max, bool nonempty);

static cJSON *parse_member(const char *text, const char *end, const char *key)
{
    const char *a, *b;
    return member(text, end, key, &a, &b) ? parse_bounded(a, b - a) : NULL;
}

static void poll_logs(const char *url, const char *epoch, uint32_t newest)
{
    char boot[17], node_id[65], name[97];
    uint32_t after;
    store_lock();
    peer_t *peer = find_peer(url);
    if (!peer || !watched()) {
        store_unlock();
        return;
    }
    if (strcmp(peer->log_boot, epoch) == 0 && peer->log_cursor == newest) {
        store_unlock();
        return;
    }
    snprintf(boot, sizeof(boot), "%s", peer->log_boot);
    snprintf(node_id, sizeof(node_id), "%s", peer->node_id);
    snprintf(name, sizeof(name), "%s", peer->name);
    after = strcmp(peer->log_boot, epoch) == 0 && peer->log_cursor <= newest
            ? peer->log_cursor : 0;
    store_unlock();

    char path[96];
    snprintf(path, sizeof(path), "/api/node/logs?boot=%s&after=%lu", boot,
             (unsigned long)after);
    int status;
    cJSON *failure = NULL;
    char *text = request(url, path, NULL, CLUSTER_STATE_TIMEOUT, &status, &failure);
    if (!text) {
        if (status == LOCAL_FAILURE) {
            local_problem(url, text_of(failure));
        }
        cJSON_Delete(failure);
        return;
    }
    const char *end = text + strlen(text), *start, *stop;
    if (!cluster_json_bounded(text, end - text)) {
        free(text);
        local_problem(url, "invalid log JSON");
        return;
    }
    cJSON *identity = parse_member(text, end, "id");
    cJSON *source_name = parse_member(text, end, "name");
    cJSON *version = parse_member(text, end, "boot");
    cJSON *head = parse_member(text, end, "cursor");
    uint32_t cursor = 0;
    // A cursor past `newest` is fine: the node may have logged since it answered.
    bool valid = bounded_string(identity, 64, true) && !strcmp(identity->valuestring, node_id)
                 && bounded_string(source_name, 96, false)
                 && !strcmp(source_name->valuestring[0] ? source_name->valuestring : url, name)
                 && boot_value(version) && !strcmp(version->valuestring, epoch)
                 && uint32_value(head, &cursor) && cursor >= after;
    cJSON_Delete(identity);
    cJSON_Delete(source_name);
    cJSON_Delete(version);
    cJSON_Delete(head);
    if (!valid || !member(text, end, "logs", &start, &stop) || *start != '[') {
        free(text);
        local_problem(url, "invalid log source");
        return;
    }
    const char *position = start, *a, *b;
    const char *entries[LOG_FETCH], *ends[LOG_FETCH];
    uint32_t ids[LOG_FETCH], previous = 0;
    double times[LOG_FETCH];
    int received = 0;
    while (next_element(&position, stop, &a, &b)) {
        if (received == LOG_FETCH || b - a > 1024) {
            valid = false;
            break;
        }
        cJSON *entry = parse_bounded(a, b - a);
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(entry, "at");
        uint32_t id = 0;
        valid = cJSON_IsObject(entry)
                && uint32_value(cJSON_GetObjectItemCaseSensitive(entry, "id"), &id)
                && id > previous && id <= cursor && cJSON_IsNumber(at)
                && isfinite(at->valuedouble)
                && bounded_string(cJSON_GetObjectItemCaseSensitive(entry, "event"), 31, false)
                && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(entry, "params"))
                && cJSON_GetArraySize(entry) == 4;
        if (valid) {
            entries[received] = a;
            ends[received] = b;
            ids[received] = id;
            times[received++] = at->valuedouble;
            previous = id;
        }
        cJSON_Delete(entry);
        if (!valid) {
            break;
        }
    }
    if (!valid || (received && previous != cursor) || (!received && cursor != after)) {
        free(text);
        local_problem(url, "invalid or unparseable log batch");
        return;
    }
    snprintf(boot, sizeof(boot), "%s", epoch);
    store_lock();
    peer = find_peer(url);
    bool oom = false;
    if (peer && watched() && !strcmp(peer->node_id, node_id)) {
        log_line_t *buffer = peer->logs;
        if (!buffer) {
            buffer = calloc(PEER_LOGS, sizeof(*buffer));
        }
        if (!buffer) {
            oom = true;
        } else {
            if (strcmp(peer->log_boot, boot) != 0) {
                if (peer->logs) {
                    logs_free(peer->logs, peer->log_count);
                }
                peer->log_count = 0;
                peer->log_cursor = 0;
            }
            peer->logs = buffer;
            snprintf(peer->log_boot, sizeof(peer->log_boot), "%s", boot);
            bool complete = true;
            for (int n = 0; n < received; n++) {
                if (ids[n] <= peer->log_cursor) {
                    continue;
                }
                size_t length = ends[n] - entries[n];
                char *copy = malloc(length + 1);
                if (!copy) {
                    complete = false;
                    oom = true;
                    break;
                }
                memcpy(copy, entries[n], length);
                copy[length] = 0;
                if (peer->log_count == PEER_LOGS) {
                    free(peer->logs[0].text);
                    memmove(peer->logs, peer->logs + 1, (--peer->log_count) * sizeof(*peer->logs));
                }
                peer->logs[peer->log_count++] = (log_line_t){times[n], copy, ids[n]};
                peer->log_cursor = ids[n];
            }
            if (complete && peer->log_cursor < cursor) {
                peer->log_cursor = cursor;
            }
        }
    }
    store_unlock();
    free(text);
    if (oom) {
        local_problem(url, "out of memory copying logs");
    }
}

static bool uint32_value(const cJSON *value, uint32_t *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)
            || value->valuedouble < 0 || value->valuedouble > UINT32_MAX
            || value->valuedouble != floor(value->valuedouble)) {
        return false;
    }
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool boot_value(const cJSON *value)
{
    if (!cJSON_IsString(value) || strlen(value->valuestring) != 16) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!isxdigit((unsigned char)value->valuestring[i])) {
            return false;
        }
    }
    return true;
}

static bool bounded_string(const cJSON *value, size_t max, bool nonempty)
{
    return cJSON_IsString(value) && (!nonempty || value->valuestring[0])
           && strlen(value->valuestring) <= max;
}

static bool unique_member(const cJSON *object, const char *key)
{
    const cJSON *first = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!first) {
        return false;
    }
    for (const cJSON *item = first->next; item; item = item->next) {
        if (!strcmp(item->string, key)) {
            return false;
        }
    }
    return true;
}

static bool valid_busy(const cJSON *busy)
{
    return cJSON_IsNull(busy) || cJSON_IsFalse(busy)
           || (cJSON_IsObject(busy) && cJSON_GetArraySize(busy) == 2
               && bounded_string(cJSON_GetObjectItemCaseSensitive(busy, "event"), 31, false)
               && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(busy, "params")));
}

typedef struct {
    const char *name;
    char type;
    size_t max;
} device_field_t;

static const device_field_t DEVICE_FIELDS[] = {
    {"key", 's', 23}, {"address", 's', 40}, {"label", 's', 160},
    {"owner_pin", 's', 64}, {"name", 's', 96}, {"model", 's', 128},
    {"firmware", 's', 128}, {"serial", 's', 128}, {"error", 's', 512},
    {"auto", 'b', 0}, {"pending", 'b', 0},
    {"label_at", 'n', 0}, {"auto_at", 'n', 0}, {"pin_at", 'n', 0},
    {"seen_at", 'n', 0}, {"updated_at", 'n', 0}, {"error_at", 'n', 0},
    {"rssi", 'n', 0}, {"battery", 'n', 0}, {"voltage", 'n', 0},
    {"temperature", 'n', 0}, {"humidity", 'n', 0}, {"device_time", 'n', 0},
    {"timezone", 'n', 0}, {"drift", 'n', 0}, {"product_id", 'n', 0},
};

static const device_field_t *device_field(const char *name)
{
    for (size_t i = 0; i < sizeof(DEVICE_FIELDS) / sizeof(DEVICE_FIELDS[0]); i++) {
        if (!strcmp(name, DEVICE_FIELDS[i].name)) {
            return &DEVICE_FIELDS[i];
        }
    }
    return NULL;
}

static bool valid_devices(const cJSON *devices)
{
    if (!cJSON_IsArray(devices) || cJSON_GetArraySize(devices) > 200) {
        return false;
    }
    const cJSON *view;
    cJSON_ArrayForEach(view, devices) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(view, "key");
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(view, "address");
        uint8_t addr[6];
        if (!cJSON_IsObject(view) || !bounded_string(key, 23, true)
                || !lywsd02_parse_key(key->valuestring, addr)
                || !bounded_string(address, 40, true)
                || !lywsd02_parse_key(address->valuestring, addr)) {
            return false;
        }
        for (const cJSON *previous = devices->child; previous != view; previous = previous->next) {
            if (!strcmp(json_str(previous, "key", ""), key->valuestring)) {
                return false;
            }
        }
        const cJSON *item;
        cJSON_ArrayForEach(item, view) {
            const device_field_t *field = device_field(item->string);
            if (!field) {
                continue;
            }
            for (const cJSON *previous = view->child; previous != item; previous = previous->next) {
                if (!strcmp(previous->string, item->string)) {
                    return false;
                }
            }
            if (cJSON_IsNull(item) && field->type != 'b') {
                continue;
            }
            if ((field->type == 's' && !bounded_string(item, field->max, false))
                    || (field->type == 'b' && !cJSON_IsBool(item))
                    || (field->type == 'n'
                        && (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)))) {
                return false;
            }
        }
    }
    return true;
}

static void trim_device_fields(cJSON *devices)
{
    cJSON *view;
    cJSON_ArrayForEach(view, devices) {
        cJSON *item = view->child;
        while (item) {
            cJSON *next = item->next;
            if (!device_field(item->string)) {
                cJSON_Delete(cJSON_DetachItemViaPointer(view, item));
            }
            item = next;
        }
    }
}

// ---------------------------------------------------------------- probing

static void remember(const char *node_id, const char *name, bool *save_state)
{
    cJSON *nodes = cJSON_GetObjectItem(store_state(), "nodes");
    if (!nodes) {
        nodes = cJSON_AddObjectToObject(store_state(), "nodes");
    }
    const char *known = json_str(nodes, node_id, NULL);
    if (node_id[0] && name[0] && !(known && strcmp(known, name) == 0)
            && (known || cJSON_GetArraySize(nodes) < 64)
            && json_set(nodes, node_id, cJSON_CreateString(name))) {
        *save_state = true;
    }
}

// Take over the fields a peer may have edited more recently than we did.
static bool adopt(const cJSON *devices)
{
    static const char *const FIELDS[][2] = {
        {"label", "label_at"}, {"auto", "auto_at"}, {"owner_pin", "pin_at"}};
    bool changed = false;
    const cJSON *view;
    cJSON_ArrayForEach(view, devices) {
        const char *key = json_str(view, "key", NULL);
        if (!key || !key[0] || strlen(key) >= 24) {
            continue;
        }
        bool is_new = !cJSON_GetObjectItemCaseSensitive(store_devices(), key);
        cJSON *record = jobs_record(key);
        if (!record) {
            continue;
        }
        cJSON *candidate = cJSON_Duplicate(record, true);
        bool edited = is_new;
        bool ok = candidate != NULL;
        if (ok && is_new) {
            ok = json_set(candidate, "address", cJSON_CreateString(json_str(view, "address", key)))
                 != NULL;
        }
        for (int f = 0; ok && f < 3; f++) {
            double theirs = json_num(view, FIELDS[f][1], 0);
            if (theirs > json_num(record, FIELDS[f][1], 0)) {
                const cJSON *value = cJSON_GetObjectItem(view, FIELDS[f][0]);
                ok = json_set(candidate, FIELDS[f][0],
                              value ? cJSON_Duplicate(value, true) : cJSON_CreateNull())
                     && json_set(candidate, FIELDS[f][1], cJSON_CreateNumber(theirs));
                edited = true;
            }
        }
        if (ok && edited) {
            ok = json_set(store_devices(), key, candidate) != NULL;
            candidate = NULL;
            changed |= ok;
        }
        cJSON_Delete(candidate);
        if (!ok && is_new) {
            cJSON_DeleteItemFromObjectCaseSensitive(store_devices(), key);
        }
    }
    return changed;
}

static void lock_dropped_for(const char *node_id, double down_since);

// Caller holds store_lock().
static bool reachable_elsewhere(const peer_t *peer)
{
    if (!peer->node_id[0]) {
        return false;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (&peers[i] != peer && peers[i].used && peers[i].online
                && strcmp(peers[i].node_id, peer->node_id) == 0) {
            return true;
        }
    }
    return false;
}

static void lost(const char *url, cJSON *failure)
{
    bool was_online = false;
    char name[97] = "";
    store_lock();
    peer_t *peer = find_peer(url);
    if (!peer) {
        store_unlock();
        cJSON_Delete(failure);
        return;
    }
    bool elsewhere = reachable_elsewhere(peer);
    if (elsewhere && !peer->manual) {
        // The node moved to another address (a port or IP change), so this record
        // is stale rather than down; keeping it would list the node twice.
        forget(peer);
        store_unlock();
        cJSON_Delete(failure);
        return;
    }
    double now = cluster_clock();
    if (peer->online) {
        was_online = true;
        peer->down_since = now;
        snprintf(name, sizeof(name), "%s", peer->name[0] ? peer->name : peer->url);
    }
    // The device list is kept: a node that is down still owns the devices it is
    // nearest to, and forgetting that would hand them to a radio that can barely
    // hear them.
    peer->online = false;
    drop_logs(peer);
    cJSON_Delete(peer->error);
    peer->error = failure;
    if (was_online) {
        logs_put("cluster.peerDown", "node", cJSON_CreateString(name),
                 "error", cJSON_CreateString(text_of(failure)), NULL);
    }
    if (!elsewhere) {
        lock_dropped_for(peer->node_id, peer->down_since);
    }
    store_unlock();
}

static void probe(const char *url)
{
    store_lock();
    peer_t *peer = find_peer(url);
    if (!peer) {
        store_unlock();
        return;
    }
    peer->attempted = cluster_clock();
    bool was_online = peer->online;
    store_unlock();
    // Our own port goes along, so the answering node can add us to its list if it
    // has not heard our broadcast.
    char path[48];
    snprintf(path, sizeof(path), "/api/node/state?port=%d", OWN_PORT);
    int status;
    cJSON *failure = NULL;
    char *text = request(url, path, NULL, CLUSTER_STATE_TIMEOUT, &status, &failure);
    if (!text && was_online && status == 0) {
        // One reply lost while the radio is shared with a BLE scan is not a node
        // going away; asking again saves a peerDown/peerUp pair in the log.
        cJSON_Delete(failure);
        failure = NULL;
        text = request(url, path, NULL, CLUSTER_STATE_TIMEOUT, &status, &failure);
    }
    if (!text) {
        // A node whose cluster switch is off has left, it is not misbehaving.
        bool left = !strcmp(text_of(failure), "error.clusterOff");
        if (!left && (status == LOCAL_FAILURE || status >= 500)) {
            local_problem(url, text_of(failure));
            cJSON_Delete(failure);
        } else {
            lost(url, failure);
        }
        return;
    }
    cJSON *state = parse_bounded(text, strlen(text));
    free(text);
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(state, "node");
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(node, "id");
    const cJSON *node_name_value = cJSON_GetObjectItemCaseSensitive(node, "name");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(state, "log_boot");
    const cJSON *busy_value = cJSON_GetObjectItemCaseSensitive(state, "busy");
    const cJSON *device_values = cJSON_GetObjectItemCaseSensitive(state, "devices");
    const cJSON *host_time = cJSON_GetObjectItemCaseSensitive(state, "host_time");
    uint32_t queued_value = 0, log_cursor = 0;
    if (!cJSON_IsObject(state) || !cJSON_IsObject(node)
            || !unique_member(state, "node") || !unique_member(state, "busy")
            || !unique_member(state, "queued") || !unique_member(state, "devices")
            || !unique_member(state, "log_boot") || !unique_member(state, "log_cursor")
            || !unique_member(node, "id") || !unique_member(node, "name")
            || (host_time && (!cJSON_IsNumber(host_time) || !isfinite(host_time->valuedouble)))
            || !bounded_string(identity, 64, true) || !bounded_string(node_name_value, 96, false)
            || !valid_busy(busy_value)
            || !uint32_value(cJSON_GetObjectItemCaseSensitive(state, "queued"), &queued_value)
            || queued_value > 32 || !valid_devices(device_values) || !boot_value(version)
            || !uint32_value(cJSON_GetObjectItemCaseSensitive(state, "log_cursor"), &log_cursor)) {
        cJSON_Delete(state);
        local_problem(url, "invalid or unparseable node state");
        return;
    }
    char node_id[65], name[97], log_boot[17];
    snprintf(node_id, sizeof(node_id), "%s", identity->valuestring);
    const char *said = node_name_value->valuestring;
    snprintf(name, sizeof(name), "%.96s", said[0] ? said : url);
    snprintf(log_boot, sizeof(log_boot), "%s", version->valuestring);

    if (strcmp(node_id, store_node_id()) == 0) {
        // Our own address, typed into our own peer list. Harmless, but drop it.
        cJSON_Delete(state);
        store_lock();
        peer = find_peer(url);
        if (peer) {
            forget(peer);
        }
        store_unlock();
        return;
    }

    cJSON *devices = cJSON_DetachItemFromObjectCaseSensitive(state, "devices");
    trim_device_fields(devices);
    cJSON *busy = cJSON_DetachItemFromObjectCaseSensitive(state, "busy");
    if (cJSON_IsNull(busy) || cJSON_IsFalse(busy)) {
        cJSON_Delete(busy);
        busy = NULL;
    } else {
        cJSON *item = busy->child;
        while (item) {
            cJSON *next = item->next;
            if (strcmp(item->string, "event") && strcmp(item->string, "params")) {
                cJSON_Delete(cJSON_DetachItemViaPointer(busy, item));
            }
            item = next;
        }
    }
    int queued = (int)queued_value;
    cJSON_Delete(state);

    bool came_up = false, save_state = false, save_devices = false;
    store_lock();
    peer = find_peer(url);
    if (!peer) {
        store_unlock();
        cJSON_Delete(devices);
        cJSON_Delete(busy);
        return;
    }
    came_up = !peer->online;
    snprintf(peer->node_id, sizeof(peer->node_id), "%s", node_id);
    snprintf(peer->name, sizeof(peer->name), "%s", name);
    peer->online = true;
    peer->has_state = true;
    peer->probed = cluster_clock();
    cJSON_Delete(peer->error);
    peer->error = NULL;
    cJSON_Delete(peer->devices);
    peer->devices = devices;
    cJSON_Delete(peer->busy);
    peer->busy = busy;
    peer->queued = queued;
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *other = &peers[i];
        if (other != peer && other->used && !other->online && !other->manual
                && strcmp(other->node_id, node_id) == 0) {
            forget(other);  // its old address; see lost()
        }
    }
    if (!watched()) {
        drop_logs(peer);
    }
    remember(node_id, name, &save_state);
    save_devices = adopt(devices);
    store_unlock();

    if (save_state) {
        store_save_state();
    }
    if (save_devices) {
        store_save_devices();
    }
    if (came_up) {
        logs_put("cluster.peerUp", "node", cJSON_CreateString(name),
                 "url", cJSON_CreateString(url), NULL);
    }
    if (strlen(log_boot) == 16) {
        poll_logs(url, log_boot, log_cursor);
    }
}

// A probe is due for a peer that is up or has just gone quiet - a restart is the
// usual reason and it will be back in seconds - but one that has been gone a
// while is only tried once a minute: each try blocks for the whole timeout.
static bool due(const peer_t *peer, double now)
{
    if (peer->online || now - peer->down_since <= DOWN_GRACE) {
        return true;
    }
    return now - peer->attempted >= DOWN_BACKOFF;
}

static void refresh(void)
{
    double asked = cluster_clock();
    xSemaphoreTake(refreshing, portMAX_DELAY);
    store_lock();
    bool done = refreshed >= asked;
    store_unlock();
    if (done) {
        // A round finished while we waited for it; that one is ours too.
        xSemaphoreGive(refreshing);
        return;
    }

    static char targets[MAX_PEERS][96];
    int count = 0;
    double now = cluster_clock();
    store_lock();
    if (!cluster_enabled()) {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (peers[i].used) {
                forget(&peers[i]);
            }
        }
    } else {
        bool listed[MAX_PEERS] = {0};
        const cJSON *entry;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItem(cJSON_GetObjectItem(store_config(), "node"),
                                                      "peers")) {
            char url[96];
            if (cJSON_IsString(entry)
                    && cluster_normalize_peer(entry->valuestring, PEER_DEFAULT_PORT, url,
                                              sizeof(url))) {
                peer_t *peer = touch(url, true);
                if (peer) {
                    listed[peer - peers] = true;
                }
            }
        }
        for (int i = 0; i < MAX_PEERS; i++) {
            peer_t *peer = &peers[i];
            if (!peer->used) {
                continue;
            }
            if (peer->manual && !listed[i]) {
                // Taken off the list: from now on it lives by its beacons alone.
                peer->manual = false;
            }
            // A beacon we never managed to talk to is noise and goes quickly. One
            // we did talk to is a real node, so it stays listed - as offline - long
            // enough to keep owning its devices while it is down for repair.
            if (!peer->manual && now - peer->last_beacon > BEACON_TTL
                    && now - peer->probed > CLUSTER_SIGHTING_TTL) {
                forget(peer);
                continue;
            }
            if (due(peer, now)) {
                snprintf(targets[count++], sizeof(targets[0]), "%s", peer->url);
            }
        }
    }
    store_unlock();

    if (wifi_connected()) {
        for (int i = 0; i < count; i++) {
            probe(targets[i]);
        }
    }
    store_lock();
    refreshed = cluster_clock();
    store_unlock();
    xSemaphoreGive(refreshing);
}

void cluster_ensure_fresh(void)
{
    store_lock();
    bool stale = cluster_clock() - refreshed > FRESH_ENOUGH;
    store_unlock();
    if (stale) {
        refresh();
    }
}

static double cadence(void)
{
    if (!cluster_enabled()) {
        return POLL_IDLE;
    }
    if (watched()) {
        return POLL_WATCHED;
    }
    // Nothing to ask and nobody asking: the beacon is what finds a peer, and that
    // wakes this loop, so there is no reason to keep knocking.
    bool any = false;
    store_lock();
    for (int i = 0; i < MAX_PEERS; i++) {
        any |= peers[i].used;
    }
    store_unlock();
    return any ? POLL_IDLE : POLL_ALONE;
}

static void poll_task(void *arg)
{
    for (;;) {
        if (!wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        refresh();
        nap(POLL_BIT, cadence());
    }
}

// ---------------------------------------------------------------- beacon

static int open_beacon(int *code)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        *code = errno;
        return -1;
    }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        *code = errno;
        close(sock);
        return -1;
    }
    return sock;
}

static void announce(int sock)
{
    char name[97];
    store_node_name(name, sizeof(name));
    cJSON *payload = cJSON_CreateObject();
    if (!payload || !json_set(payload, "id", cJSON_CreateString(store_node_id()))
            || !json_set(payload, "name", cJSON_CreateString(name))
            || !json_set(payload, "port", cJSON_CreateNumber(OWN_PORT))) {
        cJSON_Delete(payload);
        return;
    }
    char *text = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!text) {
        return;
    }
    struct sockaddr_in everyone = {
        .sin_family = AF_INET,
        .sin_port = htons(BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(sock, text, strlen(text), 0, (struct sockaddr *)&everyone, sizeof(everyone));
    free(text);
}

static void on_beacon(const char *data, int length, const struct sockaddr_in *from)
{
    cJSON *payload = length > 0 ? parse_bounded(data, (size_t)length) : NULL;
    if (!cJSON_IsObject(payload) || strcmp(json_str(payload, "id", ""), store_node_id()) == 0) {
        cJSON_Delete(payload);
        return;
    }
    const cJSON *port = cJSON_GetObjectItem(payload, "port");
    if (!cJSON_IsNumber(port) || port->valuedouble != floor(port->valuedouble)
            || port->valuedouble < 1 || port->valuedouble > 65535) {
        cJSON_Delete(payload);
        return;
    }
    const uint8_t *ip = (const uint8_t *)&from->sin_addr.s_addr;
    char url[96];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%d", ip[0], ip[1], ip[2], ip[3],
             (int)port->valuedouble);

    store_lock();
    peer_t *peer = find_peer(url);
    // A node we have never seen, and one we had given up on, are both worth
    // probing now: the second is how a machine that was switched off gets back on
    // the page before its backoff would have noticed.
    bool news = !peer || !peer->online;
    bool wake = false;
    peer = touch(url, false);
    if (peer) {
        peer->last_beacon = cluster_clock();
        const char *name = json_str(payload, "name", NULL);
        if (name && name[0]) {
            copy_text(peer->name, sizeof(peer->name), name, 24);
        }
        wake = heard(peer, news);
    }
    store_unlock();
    cJSON_Delete(payload);
    if (wake) {
        cluster_wake();
    }
}

static void beacon_task(void *arg)
{
    int sock = -1;
    bool blocked = false;
    double announced = LONG_AGO;
    static char buffer[512];
    for (;;) {
        if (!discovering()) {
            if (sock >= 0) {
                // Give the port back, so the switch really means "off".
                close(sock);
                sock = -1;
            }
            nap(BEACON_BIT, POLL_IDLE);
            continue;
        }
        if (!wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (sock < 0) {
            int code = 0;
            sock = open_beacon(&code);
            if (sock < 0) {
                // Discovery is a convenience; the hand-typed list still works.
                if (!blocked) {
                    char error[64];
                    snprintf(error, sizeof(error), "[Errno %d] %s", code, strerror(code));
                    logs_put("cluster.beaconFailed", "port", cJSON_CreateNumber(BEACON_PORT),
                             "error", cJSON_CreateString(error), NULL);
                    blocked = true;
                }
                vTaskDelay(pdMS_TO_TICKS((uint32_t)(BEACON_SLOW * 1000)));
                continue;
            }
            blocked = false;
        }
        if (cluster_clock() - announced >= BEACON_GAP) {
            announce(sock);
            announced = cluster_clock();
        }
        double until = announced + (watched() ? BEACON_FAST : BEACON_SLOW);
        while (sock >= 0 && cluster_clock() < until) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(sock, &ready);
            struct timeval wait = {.tv_sec = 0, .tv_usec = 500 * 1000};
            int count = select(sock + 1, &ready, NULL, NULL, &wait);
            if (count > 0) {
                struct sockaddr_in from;
                socklen_t size = sizeof(from);
                int got = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from,
                                   &size);
                if (got > 0) {
                    on_beacon(buffer, got, &from);
                }
            } else if (count < 0) {
                close(sock);
                sock = -1;
            }
            if ((xEventGroupClearBits(nudges, BEACON_BIT) & BEACON_BIT)
                    && cluster_clock() - announced >= BEACON_GAP) {
                break;
            }
            if (!discovering()) {
                break;
            }
        }
    }
}

void cluster_hear(uint32_t ipv4, int port)
{
    if (port < 1 || port > 65535 || !ipv4 || !cluster_enabled()) {
        return;
    }
    const uint8_t *ip = (const uint8_t *)&ipv4;
    char url[96];
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%d", ip[0], ip[1], ip[2], ip[3], port);
    store_lock();
    peer_t *peer = find_peer(url);
    bool news = !peer || !peer->online;
    bool wake = false;
    peer = touch(url, false);
    if (peer) {
        // Counts as a beacon for expiry too, or the next refresh would drop it as
        // noise before it had been probed once.
        peer->last_beacon = cluster_clock();
        wake = heard(peer, news);
    }
    store_unlock();
    if (wake) {
        cluster_wake();
    }
}

// ---------------------------------------------------------------- peer lists

// The online peers, copied out so the network can be used without the lock.
static int online_peers(cluster_peer_t **out)
{
    *out = NULL;
    store_lock();
    int count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        count += peers[i].used && peers[i].online;
    }
    cluster_peer_t *list = count ? calloc(count, sizeof(*list)) : NULL;
    int n = 0;
    for (int i = 0; list && i < MAX_PEERS; i++) {
        if (peers[i].used && peers[i].online) {
            snprintf(list[n].url, sizeof(list[n].url), "%s", peers[i].url);
            snprintf(list[n].name, sizeof(list[n].name), "%s",
                     peers[i].name[0] ? peers[i].name : peers[i].url);
            n++;
        }
    }
    store_unlock();
    *out = list;
    return count && !list ? -1 : n;
}

void cluster_tell_all(const char *path, const cJSON *payload, double timeout)
{
    bool clearing = !strcmp(path, "/api/node/logs/clear");
    if (clearing) {
        xSemaphoreTake(refreshing, portMAX_DELAY);
    }
    cluster_peer_t *list;
    int count = online_peers(&list);
    if (count < 0) {
        if (clearing) {
            xSemaphoreGive(refreshing);
        }
        local_problem("cluster", "out of memory listing peers");
        return;
    }
    for (int i = 0; i < count; i++) {
        cJSON *answer = cluster_call(list[i].url, path, payload, false, timeout, NULL, NULL);
        if (clearing && answer) {
            store_lock();
            peer_t *peer = find_peer(list[i].url);
            if (peer) {
                drop_logs(peer);
            }
            store_unlock();
        }
        cJSON_Delete(answer);
    }
    free(list);
    if (clearing) {
        xSemaphoreGive(refreshing);
        cluster_wake();
    }
}

bool cluster_peer_of(const char *node_id, cluster_peer_t *out)
{
    bool found = false;
    store_lock();
    for (int i = 0; i < MAX_PEERS && !found; i++) {
        const peer_t *peer = &peers[i];
        if (peer->used && peer->online && node_id && strcmp(peer->node_id, node_id) == 0) {
            snprintf(out->url, sizeof(out->url), "%s", peer->url);
            snprintf(out->name, sizeof(out->name), "%s", peer->name[0] ? peer->name : peer->url);
            found = true;
        }
    }
    store_unlock();
    return found;
}

int cluster_log_source(int index, const cJSON *cursors, cJSON **metadata,
                       log_line_t *out, int limit)
{
    *metadata = NULL;
    if (index < 0 || index >= MAX_PEERS) {
        return 0;
    }
    store_lock();
    peer_t *peer = &peers[index];
    if (!peer->used || !peer->online || !peer->log_boot[0]) {
        store_unlock();
        return 0;
    }
    const cJSON *held_cursor = cJSON_GetObjectItemCaseSensitive(cursors, peer->node_id);
    uint32_t after = 0;
    if (strcmp(json_str(held_cursor, "boot", ""), peer->log_boot) == 0) {
        uint32_value(cJSON_GetObjectItemCaseSensitive(held_cursor, "after"), &after);
    }
    if (after > peer->log_cursor) {
        after = 0;
    }
    int made = 0;
    uint32_t cursor = after;
    int skip = !after && peer->log_count > limit ? peer->log_count - limit : 0;
    for (int n = skip; n < peer->log_count && made < limit; n++) {
        if (peer->logs[n].id <= after) {
            continue;
        }
        char *copy = strdup(peer->logs[n].text);
        if (!copy) {
            break;
        }
        out[made] = peer->logs[n];
        out[made++].text = copy;
        cursor = peer->logs[n].id;
    }
    if (!peer->log_count) {
        cursor = peer->log_cursor;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body || !json_set(body, "id", cJSON_CreateString(peer->node_id))
            || !json_set(body, "name", cJSON_CreateString(peer->name))
            || !json_set(body, "boot", cJSON_CreateString(peer->log_boot))
            || !json_set(body, "cursor", cJSON_CreateNumber(cursor))) {
        cJSON_Delete(body);
        logs_free(out, made);
        store_unlock();
        return -1;
    }
    *metadata = body;
    store_unlock();
    return made;
}

// ---------------------------------------------------------------- lock

// Guarded by store_lock(). Times are cluster_clock() seconds.
static struct {
    bool held;
    char token[65];
    char holder_id[65];
    char holder_name[97];
    cJSON *op;
    double at;
    double claimed_at;
} lock;

static void clear_lock(void)
{
    lock.held = false;
    cJSON_Delete(lock.op);
    lock.op = NULL;
}

static bool held(void)
{
    if (lock.held && cluster_clock() - lock.at > CLUSTER_LOCK_TTL) {
        logs_put("cluster.lockExpired", "node", cJSON_CreateString(lock.holder_name), NULL);
        clear_lock();
    }
    return lock.held;
}

// A node that holds our lock and then disappears would otherwise block everyone
// until the TTL runs out. Caller holds store_lock().
static void lock_dropped_for(const char *node_id, double down_since)
{
    if (lock.held && node_id[0] && strcmp(lock.holder_id, node_id) == 0
            && cluster_clock() - down_since > DOWN_GRACE) {
        logs_put("cluster.lockDropped", "node", cJSON_CreateString(lock.holder_name), NULL);
        clear_lock();
    }
}

static bool take(const char *token, const char *holder_id, const char *holder_name,
                 const cJSON *op)
{
    if (held() || jobs_any_pending()) {
        return false;
    }
    cJSON *copy = op ? cJSON_Duplicate(op, true) : cJSON_CreateNull();
    if (!copy) {
        return false;
    }
    lock.held = true;
    snprintf(lock.token, sizeof(lock.token), "%s", token);
    snprintf(lock.holder_id, sizeof(lock.holder_id), "%s", holder_id);
    snprintf(lock.holder_name, sizeof(lock.holder_name), "%s", holder_name);
    lock.op = copy;
    lock.at = lock.claimed_at = cluster_clock();
    return true;
}

static void drop(const char *token)
{
    if (token && lock.held && strcmp(lock.token, token) == 0) {
        clear_lock();
    }
}

void cluster_drop(const char *token)
{
    store_lock();
    drop(token);
    store_unlock();
}

cJSON *cluster_grant(const cJSON *claim)
{
    const char *token = json_str(claim, "token", "");
    char name[97];
    store_node_name(name, sizeof(name));
    store_lock();
    bool granted;
    if (held() && token[0] && strcmp(lock.token, token) == 0) {
        lock.at = cluster_clock();  // the holder renewing a long job
        granted = true;
    } else {
        granted = take(token, json_str(claim, "holder_id", ""), json_str(claim, "holder_name", ""),
                       cJSON_GetObjectItem(claim, "op"));
    }
    cJSON *answer = cJSON_CreateObject();
    if (!answer || !json_set(answer, "granted", cJSON_CreateBool(granted))
            || !json_set(answer, "holder", granted ? cJSON_CreateNull()
                        : cJSON_CreateString(held() ? lock.holder_name : name))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    store_unlock();
    return answer;
}

// True when every peer that answered granted the claim; otherwise false, with the
// node in the way in `blocker`.
static bool claim_peers(const char *token, const cJSON *op, char *blocker, size_t size)
{
    cluster_peer_t *list;
    int count = online_peers(&list);
    if (!count) {
        return true;
    }
    char name[97];
    store_node_name(name, sizeof(name));
    if (count < 0) {
        snprintf(blocker, size, "%s", name);
        return false;
    }
    cJSON *payload = cJSON_CreateObject();
    if (!payload || !json_set(payload, "token", cJSON_CreateString(token))
            || !json_set(payload, "holder_id", cJSON_CreateString(store_node_id()))
            || !json_set(payload, "holder_name", cJSON_CreateString(name))
            || !json_set(payload, "op", op ? cJSON_Duplicate(op, true) : cJSON_CreateNull())) {
        cJSON_Delete(payload);
        free(list);
        snprintf(blocker, size, "%s", name);
        return false;
    }
    bool granted = true;
    for (int i = 0; i < count && granted; i++) {
        int status;
        cJSON *answer = cluster_call(list[i].url, "/api/node/lock", payload, false,
                                     CLUSTER_STATE_TIMEOUT, &status, NULL);
        if (!answer && (status == LOCAL_FAILURE || status != 0)) {
            snprintf(blocker, size, "%s", list[i].name);
            granted = false;
        }
        // An unreachable peer cannot be driving a radio we care about, and letting
        // it veto would mean one sleeping machine freezes the whole house.
        const cJSON *holder_value = cJSON_GetObjectItemCaseSensitive(answer, "holder");
        if (answer && (!cJSON_IsObject(answer) || cJSON_GetArraySize(answer) != 2
                       || !unique_member(answer, "granted") || !unique_member(answer, "holder")
                       || !cJSON_IsNull(holder_value)
                       || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "granted")))) {
            const char *holder = json_str(answer, "holder", NULL);
            snprintf(blocker, size, "%s", holder && holder[0] ? holder : list[i].name);
            granted = false;
        }
        cJSON_Delete(answer);
    }
    cJSON_Delete(payload);
    free(list);
    return granted;
}

static void release_peers(const char *token)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload || !json_set(payload, "token", cJSON_CreateString(token))) {
        cJSON_Delete(payload);
        local_problem("cluster", "out of memory releasing peer locks");
        return;
    }
    cluster_tell_all("/api/node/unlock", payload, CLUSTER_STATE_TIMEOUT);
    cJSON_Delete(payload);
}

bool cluster_acquire(const cJSON *op, char token[33], char *blocker, size_t size)
{
    // The peer list may be minutes old when nobody is watching, and a node missing
    // from it would not be asked for the lock.
    cluster_ensure_fresh();
    char name[97];
    store_node_name(name, sizeof(name));
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (int i = 0; i < 16; i++) {
            sprintf(token + i * 2, "%02x", random[i]);
        }
        store_lock();
        bool taken = take(token, store_node_id(), name, op);
        if (!taken) {
            snprintf(blocker, size, "%s", held() ? lock.holder_name : name);
        }
        store_unlock();
        if (taken) {
            if (claim_peers(token, op, blocker, size)) {
                return true;
            }
            release_peers(token);
            cluster_drop(token);
        }
        if (!attempt) {
            // Two nodes can refuse each other at the same instant; let one win.
            vTaskDelay(pdMS_TO_TICKS(100 + esp_random() % 301));
        }
    }
    logs_put("cluster.lockRefused", "node", cJSON_CreateString(blocker), NULL);
    return false;
}

void cluster_release(const char *token)
{
    cluster_drop(token);
    release_peers(token);
}

bool cluster_holds(const char *token)
{
    store_lock();
    bool holds = token && token[0] && held() && strcmp(lock.token, token) == 0;
    store_unlock();
    return holds;
}

void cluster_keep(const char *token)
{
    // LOCK_TTL is there for a holder that crashed, not for one that is merely slow:
    // a scheduled run over many devices with retries easily outlasts it, and a peer
    // whose copy expired would start its own radio in the middle.
    cJSON *op = NULL;
    store_lock();
    if (token && held() && strcmp(lock.token, token) == 0) {
        lock.at = cluster_clock();
        if (lock.at - lock.claimed_at >= CLUSTER_LOCK_TTL / 3) {
            op = cJSON_Duplicate(lock.op, true);
            if (op) {
                lock.claimed_at = lock.at;
            }
        }
    }
    store_unlock();
    if (op) {
        // The same claim again: a peer still holding it renews, one that came
        // online after we started takes it now.
        char blocker[97];
        claim_peers(token, op, blocker, sizeof(blocker));
        cJSON_Delete(op);
    }
}

cJSON *cluster_lock_view(void)
{
    store_lock();
    cJSON *view;
    if (!held()) {
        view = cJSON_CreateNull();
    } else {
        view = cJSON_CreateObject();
        if (!view || !json_set(view, "holder", cJSON_CreateString(lock.holder_name))
                || !json_set(view, "holder_id", cJSON_CreateString(lock.holder_id))
                || !json_set(view, "op", cJSON_Duplicate(lock.op, true))
                || !json_set(view, "mine", cJSON_CreateBool(!strcmp(lock.holder_id, store_node_id())))) {
            cJSON_Delete(view);
            view = NULL;
        }
    }
    store_unlock();
    return view;
}

// ---------------------------------------------------------------- cluster jobs

int cluster_start_on_peers(const char *path, const char *token, const char *event,
                           cluster_peer_t **started)
{
    cluster_peer_t *list;
    int count = online_peers(&list);
    *started = NULL;
    if (count <= 0) {
        return count;
    }
    cJSON *payload = cJSON_CreateObject();
    if (!payload || !json_set(payload, "token", cJSON_CreateString(token))) {
        cJSON_Delete(payload);
        free(list);
        return -1;
    }
    *started = list;
    logs_put(event, "count", cJSON_CreateNumber(count), NULL);
    int took = 0;
    for (int i = 0; i < count; i++) {
        cJSON *failure = NULL;
        int status;
        cJSON *answer = cluster_call(list[i].url, path, payload, false, CLUSTER_ACTION_TIMEOUT,
                                     &status, &failure);
        const cJSON *queued = cJSON_GetObjectItemCaseSensitive(answer, "queued");
        bool certain = cJSON_IsObject(answer) && unique_member(answer, "queued")
                       && cJSON_IsBool(queued);
        if (!certain || cJSON_IsFalse(queued)) {
            logs_put("cluster.peerFailed", "node", cJSON_CreateString(list[i].name),
                     "error", cJSON_CreateString(certain ? "error.alreadyQueued"
                                                 : answer ? "invalid start answer"
                                                 : text_of(failure)), NULL);
        }
        // A lost/invalid reply can follow an already-started action. Keep waiting
        // for that node rather than immediately releasing its radio lock.
        if ((certain && cJSON_IsTrue(queued))
                || (!certain && (answer || status <= 0 || status >= 500))) {
            list[took++] = list[i];
        }
        cJSON_Delete(answer);
        cJSON_Delete(failure);
    }
    cJSON_Delete(payload);
    return took;
}

// Has a job running or queued, by the raw text of its /api/node/state.
static bool still_working(const char *text)
{
    cJSON *state = parse_bounded(text, strlen(text));
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(state, "node");
    const cJSON *busy = cJSON_GetObjectItemCaseSensitive(state, "busy");
    uint32_t queued = 0;
    bool valid = cJSON_IsObject(state) && cJSON_IsObject(node)
                 && unique_member(state, "node") && unique_member(state, "busy")
                 && unique_member(state, "queued")
                 && unique_member(node, "id") && unique_member(node, "name")
                 && bounded_string(cJSON_GetObjectItemCaseSensitive(node, "id"), 64, true)
                 && bounded_string(cJSON_GetObjectItemCaseSensitive(node, "name"), 96, false)
                 && valid_busy(busy)
                 && uint32_value(cJSON_GetObjectItemCaseSensitive(state, "queued"), &queued)
                 && queued <= 32;
    bool working = !valid || queued != 0 || cJSON_IsObject(busy);
    cJSON_Delete(state);
    return working;
}

bool cluster_wait_for_peers(const cluster_peer_t *list, int count, double deadline,
                            const char *token)
{
    if (count < 0 || count > MAX_PEERS || (count && !list)) {
        return false;
    }
    double failed_since[MAX_PEERS] = {0};
    while (cluster_clock() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        cluster_keep(token);
        bool working = false;
        for (int i = 0; i < count; i++) {
            int status;
            char *text = request(list[i].url, "/api/node/status", NULL, CLUSTER_STATE_TIMEOUT,
                                 &status, NULL);
            if (text) {
                failed_since[i] = 0;
                working |= still_working(text);
                free(text);
            } else if (status != 0) {
                failed_since[i] = 0;
                working = true;
            } else {
                if (!failed_since[i]) {
                    failed_since[i] = cluster_clock();
                }
                working |= cluster_clock() - failed_since[i] < DOWN_GRACE;
            }
        }
        if (!working) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------- merge and ownership

typedef struct {
    const char *node_id;
    const char *name;
    const cJSON *views;
    bool online;
} source_t;

// One device as the sources see it: at most one view per source.
typedef struct {
    int count;
    const source_t *source[MAX_SOURCES];
    const cJSON *view[MAX_SOURCES];
} slot_t;

// Every node's device list, including nodes that are down. Caller holds the lock.
static int sources(const cJSON *local_views, const char *self_name, source_t *out)
{
    out[0] = (source_t){store_node_id(), self_name, local_views, true};
    int count = 1;
    for (int i = 0; i < MAX_PEERS; i++) {
        const peer_t *peer = &peers[i];
        if (peer->used && peer->has_state && peer->node_id[0]) {
            out[count++] = (source_t){peer->node_id, peer->name[0] ? peer->name : peer->url,
                                      peer->devices, peer->online};
        }
    }
    return count;
}

static double activity(const cJSON *view)
{
    double updated = json_num(view, "updated_at", 0);
    double failed = json_num(view, "error_at", 0);
    return updated > failed ? updated : failed;
}

static bool has_rssi(const cJSON *view)
{
    return cJSON_IsNumber(cJSON_GetObjectItem(view, "rssi"));
}

static bool fresh(const cJSON *view, double now)
{
    return now - json_num(view, "seen_at", 0) < CLUSTER_SIGHTING_TTL;
}

static bool online(const source_t *all, int count, const char *node_id)
{
    for (int i = 0; i < count; i++) {
        if (all[i].online && strcmp(all[i].node_id, node_id) == 0) {
            return true;
        }
    }
    return false;
}

static const char *owner(const slot_t *slot, const char *pin, double now)
{
    if (pin && pin[0]) {
        return pin;
    }
    // Quantising the signal first means every node computes the same owner and a
    // peer has to be clearly closer, not one noisy dBm closer, to take over.
    int best = -1;
    for (int i = 0; i < slot->count; i++) {
        if (!has_rssi(slot->view[i]) || !fresh(slot->view[i], now)) {
            continue;
        }
        if (best < 0) {
            best = i;
            continue;
        }
        int mine = -cluster_bucket(json_num(slot->view[i], "rssi", 0));
        int theirs = -cluster_bucket(json_num(slot->view[best], "rssi", 0));
        if (mine < theirs || (mine == theirs
                && strcmp(slot->source[i]->node_id, slot->source[best]->node_id) < 0)) {
            best = i;
        }
    }
    if (best >= 0) {
        return slot->source[best]->node_id;
    }
    best = 0;
    for (int i = 1; i < slot->count; i++) {
        double mine = -activity(slot->view[i]);
        double theirs = -activity(slot->view[best]);
        if (mine < theirs || (mine == theirs
                && strcmp(slot->source[i]->node_id, slot->source[best]->node_id) < 0)) {
            best = i;
        }
    }
    return slot->source[best]->node_id;
}

static const char *node_name(const char *node_id, const char *self_name)
{
    if (strcmp(node_id, store_node_id()) == 0) {
        return self_name;
    }
    return json_str(cJSON_GetObjectItem(store_state(), "nodes"), node_id, NULL);
}

// Takes ownership on both success and failure, like json_set().
static bool append_json(cJSON *array, cJSON *value)
{
    if (value && cJSON_IsArray(array) && cJSON_AddItemToArray(array, value)) {
        return true;
    }
    cJSON_Delete(value);
    return false;
}

static cJSON *row(const char *key, const slot_t *slot, const source_t *all, int count,
                  const char *self_name, double now)
{
    // Whatever happened most recently, anywhere, is what the row should show.
    int base = 0;
    for (int i = 1; i < slot->count; i++) {
        if (activity(slot->view[i]) > activity(slot->view[base])) {
            base = i;
        }
    }
    cJSON *device = cJSON_Duplicate(slot->view[base], true);
    if (!device || !json_set(device, "key", cJSON_CreateString(key))) {
        goto failed;
    }

    static const char *const FIELDS[][2] = {
        {"label", "label_at"}, {"auto", "auto_at"}, {"owner_pin", "pin_at"}};
    for (int f = 0; f < 3; f++) {
        // Records written before stamps existed carry none, so a value beats a
        // blank at the same timestamp instead of the sources' order.
        int newest = 0;
        for (int i = 1; i < slot->count; i++) {
            double stamp = json_num(slot->view[i], FIELDS[f][1], 0);
            double best = json_num(slot->view[newest], FIELDS[f][1], 0);
            const cJSON *value = cJSON_GetObjectItem(slot->view[i], FIELDS[f][0]);
            const cJSON *held_value = cJSON_GetObjectItem(slot->view[newest], FIELDS[f][0]);
            bool set = value && !cJSON_IsNull(value);
            bool best_set = held_value && !cJSON_IsNull(held_value);
            if (stamp > best || (stamp == best && set && !best_set)) {
                newest = i;
            }
        }
        for (int part = 0; part < 2; part++) {
            const cJSON *value = cJSON_GetObjectItem(slot->view[newest], FIELDS[f][part]);
            if (!json_set(device, FIELDS[f][part],
                          value ? cJSON_Duplicate(value, true) : cJSON_CreateNull())) {
                goto failed;
            }
        }
    }
    if (cJSON_IsNull(cJSON_GetObjectItem(device, "auto"))
            && !json_set(device, "auto", cJSON_CreateTrue())) {
        goto failed;
    }

    const char *pin = json_str(device, "owner_pin", NULL);
    const char *owner_id = owner(slot, pin, now);
    // owner_id can point into `device` itself; copy it before the object changes.
    char owner_copy[65];
    snprintf(owner_copy, sizeof(owner_copy), "%s", owner_id);
    const char *name = node_name(owner_copy, self_name);
    if (!json_set(device, "node_id", cJSON_CreateString(owner_copy))
            || !json_set(device, "node", name ? cJSON_CreateString(name) : cJSON_CreateNull())
            || !json_set(device, "node_online", cJSON_CreateBool(online(all, count, owner_copy)))) {
        goto failed;
    }

    // One sighting per node: a node listed twice keeps its first place and its
    // last report, as a dict keyed by node would.
    int pick[MAX_SOURCES];
    int picked = 0;
    for (int i = 0; i < slot->count; i++) {
        if (!has_rssi(slot->view[i])) {
            continue;
        }
        int at = 0;
        while (at < picked && strcmp(slot->source[pick[at]]->node_id, slot->source[i]->node_id)) {
            at++;
        }
        pick[at] = i;
        if (at == picked) {
            picked++;
        }
    }
    // Only a node that heard it lately can be offered as a pin: anything else
    // would just fail to connect. Stable sort by signal, strongest first.
    int order[MAX_SOURCES];
    for (int n = 0; n < picked; n++) {
        int i = pick[n];
        double rssi = json_num(slot->view[i], "rssi", 0);
        int at = n;
        while (at > 0 && json_num(slot->view[order[at - 1]], "rssi", 0) < rssi) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    cJSON *sightings = cJSON_CreateArray();
    if (!json_set(device, "sightings", sightings)) {
        goto failed;
    }
    int owner_sighting = -1;
    for (int n = 0; n < picked; n++) {
        int i = order[n];
        const cJSON *seen = cJSON_GetObjectItem(slot->view[i], "seen_at");
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !json_set(entry, "node_id", cJSON_CreateString(slot->source[i]->node_id))
                || !json_set(entry, "node", slot->source[i]->name
                            ? cJSON_CreateString(slot->source[i]->name) : cJSON_CreateNull())
                || !json_set(entry, "rssi", cJSON_CreateNumber(json_num(slot->view[i], "rssi", 0)))
                || !json_set(entry, "seen_at", seen ? cJSON_Duplicate(seen, true) : cJSON_CreateNull())
                || !json_set(entry, "fresh", cJSON_CreateBool(fresh(slot->view[i], now)))) {
            cJSON_Delete(entry);
            goto failed;
        }
        if (!append_json(sightings, entry)) {
            goto failed;
        }
        if (strcmp(slot->source[i]->node_id, owner_copy) == 0) {
            owner_sighting = i;
        }
    }

    // Only a running node can still be working on it.
    bool pending = false;
    for (int i = 0; i < slot->count; i++) {
        pending |= online(all, count, slot->source[i]->node_id)
                   && json_bool(slot->view[i], "pending", false);
    }
    if (!json_set(device, "pending", cJSON_CreateBool(pending))) {
        goto failed;
    }

    if (owner_sighting >= 0) {
        const cJSON *view = slot->view[owner_sighting];
        const cJSON *seen = cJSON_GetObjectItem(view, "seen_at");
        if (!json_set(device, "rssi", cJSON_CreateNumber(json_num(view, "rssi", 0)))
                || !json_set(device, "seen_at", seen ? cJSON_Duplicate(seen, true) : cJSON_CreateNull())) {
            goto failed;
        }
    }
    return device;
failed:
    cJSON_Delete(device);
    return NULL;
}

static int by_text(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

cJSON *cluster_merge(const cJSON *local_views)
{
    if (!cJSON_IsArray(local_views) || cJSON_GetArraySize(local_views) > 200) {
        return NULL;
    }
    char self_name[97];
    store_node_name(self_name, sizeof(self_name));
    cJSON *merged = cJSON_CreateArray();
    if (!merged) {
        return NULL;
    }

    store_lock();
    source_t all[MAX_SOURCES];
    int count = sources(local_views, self_name, all);

    int total = 0;
    for (int s = 0; s < count; s++) {
        total += cJSON_GetArraySize(all[s].views);
    }
    const char **keys = total ? malloc((size_t)total * sizeof(*keys)) : NULL;
    if (total && !keys) {
        store_unlock();
        cJSON_Delete(merged);
        return NULL;
    }
    int unique = 0;
    for (int s = 0; s < count; s++) {
        const cJSON *view;
        cJSON_ArrayForEach(view, all[s].views) {
            const char *key = json_str(view, "key", NULL);
            if (!key || !keys) {
                continue;
            }
            bool known = false;
            for (int k = 0; k < unique && !known; k++) {
                known = strcmp(keys[k], key) == 0;
            }
            if (!known) {
                keys[unique++] = key;
            }
        }
    }
    if (keys) {
        qsort(keys, unique, sizeof(*keys), by_text);
    }

    double now = store_now();
    for (int k = 0; k < unique; k++) {
        slot_t slot = {0};
        for (int s = 0; s < count; s++) {
            const cJSON *view;
            cJSON_ArrayForEach(view, all[s].views) {
                if (strcmp(json_str(view, "key", ""), keys[k]) == 0) {
                    slot.source[slot.count] = &all[s];
                    slot.view[slot.count++] = view;
                    break;
                }
            }
        }
        if (!append_json(merged, row(keys[k], &slot, all, count, self_name, now))) {
            store_unlock();
            free(keys);
            cJSON_Delete(merged);
            return NULL;
        }
    }
    store_unlock();
    free(keys);
    return merged;
}

static bool heard_here(const cJSON *device)
{
    const cJSON *sighting;
    cJSON_ArrayForEach(sighting, cJSON_GetObjectItem(device, "sightings")) {
        if (strcmp(json_str(sighting, "node_id", ""), store_node_id()) == 0) {
            return true;
        }
    }
    return false;
}

bool cluster_online(const char *node_id)
{
    if (!node_id) {
        return false;
    }
    if (strcmp(node_id, store_node_id()) == 0) {
        return true;
    }
    bool up = false;
    store_lock();
    for (int i = 0; i < MAX_PEERS && !up; i++) {
        up = peers[i].used && peers[i].online && strcmp(peers[i].node_id, node_id) == 0;
    }
    store_unlock();
    return up;
}

bool cluster_my_share(const cJSON *merged, cJSON *mine, cJSON *taken)
{
    if (!cJSON_IsArray(merged) || !cJSON_IsArray(mine) || !cJSON_IsArray(taken)) {
        if (cJSON_IsArray(mine)) {
            while (mine->child) {
                cJSON_Delete(cJSON_DetachItemViaPointer(mine, mine->child));
            }
        }
        if (cJSON_IsArray(taken)) {
            while (taken->child) {
                cJSON_Delete(cJSON_DetachItemViaPointer(taken, taken->child));
            }
        }
        return false;
    }
    double now = store_now();
    const cJSON *device;
    cJSON_ArrayForEach(device, merged) {
        if (!json_bool(device, "auto", true)) {
            continue;
        }
        const char *key = json_str(device, "key", "");
        const char *node_id = json_str(device, "node_id", "");
        const char *pin = json_str(device, "owner_pin", NULL);
        cJSON *target = NULL;
        if (strcmp(node_id, store_node_id()) == 0) {
            target = mine;
        } else if (!(pin && pin[0]) && !cluster_online(node_id)
                   && now - activity(device) > CLUSTER_TAKEOVER_AFTER && heard_here(device)) {
            // Cover for an absent owner only when this radio has heard the device.
            target = taken;
        }
        if (target && !append_json(target, cJSON_CreateString(key))) {
            while (mine && mine->child) {
                cJSON_Delete(cJSON_DetachItemViaPointer(mine, mine->child));
            }
            while (taken && taken->child) {
                cJSON_Delete(cJSON_DetachItemViaPointer(taken, taken->child));
            }
            return false;
        }
    }
    return true;
}

bool cluster_my_work(const cJSON *merged)
{
    if (!cJSON_IsArray(merged)) {
        return true;
    }
    double now = store_now();
    bool heard_auto = false;
    const cJSON *device;
    cJSON_ArrayForEach(device, merged) {
        if (!json_bool(device, "auto", true)) {
            continue;
        }
        bool heard = heard_here(device);
        heard_auto |= heard;
        const char *node_id = json_str(device, "node_id", "");
        const char *pin = json_str(device, "owner_pin", NULL);
        if (!strcmp(node_id, store_node_id())
                || (!(pin && pin[0]) && !cluster_online(node_id)
                    && now - activity(device) > CLUSTER_TAKEOVER_AFTER && heard)) {
            return true;
        }
    }
    // A scan is still needed when no automatic device was ever heard here.
    return !heard_auto;
}

static int owned_by(const cJSON *merged, const char *node_id)
{
    int owned = 0;
    const cJSON *device;
    cJSON_ArrayForEach(device, merged) {
        owned += strcmp(json_str(device, "node_id", ""), node_id) == 0;
    }
    return owned;
}

static int by_url(const void *a, const void *b)
{
    return strcmp((*(const peer_t *const *)a)->url, (*(const peer_t *const *)b)->url);
}

cJSON *cluster_nodes_view(const cJSON *merged, cJSON *busy, int queued)
{
    char name[97];
    store_node_name(name, sizeof(name));
    cJSON *self = cJSON_CreateObject();
    cJSON *rows = cJSON_CreateArray();
    if (!self || !rows || !busy || !cJSON_IsArray(merged)) {
        cJSON_Delete(self);
        cJSON_Delete(rows);
        cJSON_Delete(busy);
        return NULL;
    }
    bool ok = json_set(self, "busy", busy) != NULL;
    if (!ok || !json_set(self, "id", cJSON_CreateString(store_node_id()))
            || !json_set(self, "name", cJSON_CreateString(name))
            || !json_set(self, "url", cJSON_CreateNull())
            || !json_set(self, "origin", cJSON_CreateString("self"))
            || !json_set(self, "own", cJSON_CreateTrue())
            || !json_set(self, "online", cJSON_CreateTrue())
            || !json_set(self, "queued", cJSON_CreateNumber(queued))
            || !json_set(self, "error", cJSON_CreateNull())
            || !json_set(self, "devices", cJSON_CreateNumber(owned_by(merged, store_node_id())))) {
        cJSON_Delete(self);
        cJSON_Delete(rows);
        return NULL;
    }
    if (!append_json(rows, self)) {
        cJSON_Delete(rows);
        return NULL;
    }

    store_lock();
    const peer_t *sorted[MAX_PEERS];
    int count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].used) {
            sorted[count++] = &peers[i];
        }
    }
    qsort(sorted, count, sizeof(*sorted), by_url);
    for (int i = 0; i < count; i++) {
        const peer_t *peer = sorted[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !json_set(entry, "id", peer->node_id[0] ? cJSON_CreateString(peer->node_id)
                                                            : cJSON_CreateNull())
                || !json_set(entry, "name", cJSON_CreateString(peer->name[0] ? peer->name : peer->url))
                || !json_set(entry, "url", cJSON_CreateString(peer->url))
                || !json_set(entry, "origin", cJSON_CreateString(peer->manual ? "manual" : "beacon"))
                || !json_set(entry, "own", cJSON_CreateFalse())
                || !json_set(entry, "online", cJSON_CreateBool(peer->online))
                || !json_set(entry, "busy", peer->online && peer->busy
                            ? cJSON_Duplicate(peer->busy, true) : cJSON_CreateNull())
                || !json_set(entry, "queued", cJSON_CreateNumber(peer->online ? peer->queued : 0))
                || !json_set(entry, "error", peer->error ? cJSON_Duplicate(peer->error, true)
                                                        : cJSON_CreateNull())
                || !json_set(entry, "devices", cJSON_CreateNumber(
                            peer->node_id[0] ? owned_by(merged, peer->node_id) : 0))) {
            cJSON_Delete(entry);
            store_unlock();
            cJSON_Delete(rows);
            return NULL;
        }
        if (!append_json(rows, entry)) {
            store_unlock();
            cJSON_Delete(rows);
            return NULL;
        }
    }
    store_unlock();
    return rows;
}

// ---------------------------------------------------------------- start

void cluster_start(void)
{
    nudges = xEventGroupCreate();
    refreshing = xSemaphoreCreateMutex();
    http_gate = xSemaphoreCreateMutex();
    configASSERT(nudges && refreshing && http_gate);
    // A peer that is switched off is normal; its every failed probe is not news.
    static const char *const QUIET[] = {"HTTP_CLIENT", "transport", "transport_base", "esp-tls"};
    for (size_t i = 0; i < sizeof(QUIET) / sizeof(QUIET[0]); i++) {
        esp_log_level_set(QUIET[i], ESP_LOG_NONE);
    }
    char name[97];
    store_node_name(name, sizeof(name));
    logs_put("cluster.node", "node", cJSON_CreateString(name), NULL);
    ESP_LOGI(TAG, "node %s", store_node_id());
    BaseType_t created = xTaskCreate(poll_task, "poll", 6144, NULL, 4, NULL);
    configASSERT(created == pdPASS);
    created = xTaskCreate(beacon_task, "beacon", 4096, NULL, 4, NULL);
    configASSERT(created == pdPASS);
}
