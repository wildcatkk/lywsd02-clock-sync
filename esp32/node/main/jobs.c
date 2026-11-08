#include "jobs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_clock.h"
#include "cluster.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "logs.h"
#include "ntp.h"
#include "store.h"
#include "wifi.h"

#define QUEUE_DEPTH 32
#define MAX_DEVICES 200
#define RETRY_DELAY_MS 5000
// A server answer is reused for this long; the offset of a sane clock barely moves.
#define TIME_SERVER_CACHE 600

static const char *TAG = "jobs";

typedef struct {
    char key[24];
    cJSON *busy;
    job_kind_t kind;
    char device[24];
    bool write;
    bool has_token;
    char token[33];
} job_t;

static QueueHandle_t queue;
// Serialises the worker and diagnostic start/stop cycles, never under store_lock().
static SemaphoreHandle_t radio_lifecycle;
// Guarded by store_lock(): hub.pending and hub.busy.
static cJSON *pending;
static cJSON *busy;
// The lock the running job holds, renewed between its steps.
static char running_token[33];

bool jobs_ble_try_lock(void)
{
    return radio_lifecycle && xSemaphoreTake(radio_lifecycle, 0) == pdTRUE;
}

void jobs_ble_unlock(void)
{
    xSemaphoreGive(radio_lifecycle);
}

static void job_failed(const char *error)
{
    // Keep a diagnostic even if there is too little memory for a JSON log event.
    ESP_LOGE(TAG, "%s", error);
    logs_put("job.crashed", "error", cJSON_CreateString(error), NULL);
}

// ---------------------------------------------------------------- schedule

static bool parse(const cJSON *schedule, int *days, int *hour, int *minute, int *weekday)
{
    const char *mode = json_str(schedule, "mode", "weekly");
    if (strcmp(mode, "daily") == 0) {
        *days = 1;
    } else if (strcmp(mode, "weekly") == 0) {
        *days = 7;
    } else {
        return false;
    }
    *hour = (int)json_num(schedule, "hour", 4);
    *minute = (int)json_num(schedule, "minute", 0);
    *weekday = (int)json_num(schedule, "weekday", 6);
    return true;
}

// The local calendar day of `day` shifted by `shift` days, at hour:minute. mktime
// normalises the day of the month, so this is calendar arithmetic: stepping by
// 86400 s would land an hour off across a DST change.
static double at(struct tm day, int shift, int hour, int minute)
{
    day.tm_mday += shift;
    day.tm_hour = hour;
    day.tm_min = minute;
    day.tm_sec = 0;
    day.tm_isdst = -1;
    return (double)mktime(&day);
}

double jobs_last_scheduled(const cJSON *schedule, double now)
{
    int days, hour, minute, weekday;
    if (!parse(schedule, &days, &hour, &minute, &weekday)) {
        return NAN;
    }
    time_t clock = (time_t)now;
    struct tm day;
    localtime_r(&clock, &day);
    int shift = 0;
    if (days == 7) {
        // tm_wday counts from Sunday, Python's weekday() from Monday.
        int today = (day.tm_wday + 6) % 7;
        shift = -((((today - weekday) % 7) + 7) % 7);
    }
    double moment = at(day, shift, hour, minute);
    if (moment > now) {
        moment = at(day, shift - days, hour, minute);
    }
    return moment;
}

double jobs_next_scheduled(const cJSON *schedule, double now)
{
    double previous = jobs_last_scheduled(schedule, now);
    if (isnan(previous)) {
        return NAN;
    }
    int days, hour, minute, weekday;
    parse(schedule, &days, &hour, &minute, &weekday);
    time_t clock = (time_t)previous;
    struct tm day;
    localtime_r(&clock, &day);
    return at(day, days, hour, minute);
}

// ---------------------------------------------------------------- hub

cJSON *jobs_record(const char *key)
{
    cJSON *devices = store_devices();
    if (!key || !cJSON_IsObject(devices)) {
        return NULL;
    }
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(devices, key);
    if (existing) {
        return existing;
    }
    // Keep all existing records, including stores already above the limit.
    if (cJSON_GetArraySize(devices) >= MAX_DEVICES) {
        ESP_LOGW(TAG, "device limit reached, cannot add %s", key);
        return NULL;
    }
    // No label is stored, so the UI can show a default in the chosen language.
    existing = cJSON_CreateObject();
    if (!existing || !json_set(existing, "auto", cJSON_CreateTrue())) {
        cJSON_Delete(existing);
        return NULL;
    }
    return json_set(devices, key, existing);
}

const char *jobs_name_of(const char *key)
{
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(store_devices(), key);
    const char *label = json_str(record, "label", NULL);
    if (label && label[0]) {
        return label;
    }
    const char *address = json_str(record, "address", NULL);
    return address && address[0] ? address : key;
}

static bool is_pending(const char *key)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, pending) {
        if (strcmp(item->valuestring, key) == 0) {
            return true;
        }
    }
    return false;
}

bool jobs_any_pending(void)
{
    return cJSON_GetArraySize(pending) > 0;
}

cJSON *jobs_busy(void)
{
    store_lock();
    cJSON *copy = busy ? cJSON_Duplicate(busy, true) : cJSON_CreateNull();
    store_unlock();
    return copy;
}

int jobs_queued(void)
{
    return (int)uxQueueMessagesWaiting(queue);
}

static cJSON *copy_of(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return item ? cJSON_Duplicate(item, true) : cJSON_CreateNull();
}

static cJSON *device_view(const char *key, const cJSON *record)
{
    const cJSON *last = cJSON_GetObjectItemCaseSensitive(record, "last");
    cJSON *view = cJSON_CreateObject();
    if (!view
            || !json_set(view, "key", cJSON_CreateString(key))
            || !json_set(view, "address", cJSON_CreateString(json_str(record, "address", key)))
            || !json_set(view, "label", copy_of(record, "label"))
            || !json_set(view, "label_at", copy_of(record, "label_at"))
            || !json_set(view, "auto", cJSON_GetObjectItemCaseSensitive(record, "auto")
                         ? copy_of(record, "auto") : cJSON_CreateTrue())) {
        goto failed;
    }
    static const char *const RECORD[] = {"auto_at", "owner_pin", "pin_at", "name", "rssi",
                                         "seen_at"};
    for (size_t i = 0; i < sizeof(RECORD) / sizeof(RECORD[0]); i++) {
        if (!json_set(view, RECORD[i], copy_of(record, RECORD[i]))) {
            goto failed;
        }
    }
    static const char *const LAST[] = {"model", "firmware", "serial", "battery", "voltage",
                                       "temperature", "humidity", "device_time", "timezone",
                                       "drift"};
    for (size_t i = 0; i < sizeof(LAST) / sizeof(LAST[0]); i++) {
        if (!json_set(view, LAST[i], copy_of(last, LAST[i]))) {
            goto failed;
        }
    }
    if (!json_set(view, "updated_at", copy_of(last, "at"))
            || !json_set(view, "error", copy_of(record, "error"))
            || !json_set(view, "error_at", copy_of(record, "error_at"))
            || !json_set(view, "pending", cJSON_CreateBool(is_pending(key)))) {
        goto failed;
    }
    return view;

failed:
    cJSON_Delete(view);
    return NULL;
}

static int by_key(const void *a, const void *b)
{
    return strcmp((*(const cJSON *const *)a)->string, (*(const cJSON *const *)b)->string);
}

cJSON *jobs_local_views(void)
{
    const cJSON *devices = store_devices();
    if (!cJSON_IsObject(devices)) {
        return NULL;
    }
    int count = cJSON_GetArraySize(devices);
    cJSON *views = cJSON_CreateArray();
    if (!views || !count) {
        return views;
    }
    const cJSON **records = malloc((size_t)count * sizeof(*records));
    if (!records) {
        cJSON_Delete(views);
        return NULL;
    }
    int n = 0;
    const cJSON *record;
    cJSON_ArrayForEach(record, devices) {
        records[n++] = record;
    }
    qsort(records, n, sizeof(*records), by_key);
    for (int i = 0; i < n; i++) {
        cJSON *view = device_view(records[i]->string, records[i]);
        if (!view || !cJSON_AddItemToArray(views, view)) {
            cJSON_Delete(view);
            cJSON_Delete(views);
            free(records);
            return NULL;
        }
    }
    free(records);
    return views;
}

bool jobs_enqueue(const char *key, cJSON *busy_event, job_kind_t kind, const char *device,
                  bool write, const char *token)
{
    if (!key || strlen(key) >= sizeof(((job_t *)0)->key)
            || !busy_event || !queue || !pending) {
        cJSON_Delete(busy_event);
        job_failed("RuntimeError('job key, queue or event unavailable')");
        return false;
    }
    store_lock();
    if (is_pending(key)) {
        logs_put("queue.duplicate", "target", cJSON_CreateString(jobs_name_of(key)), NULL);
        store_unlock();
        cJSON_Delete(busy_event);
        return false;
    }
    job_t *job = calloc(1, sizeof(*job));
    cJSON *marker = job ? cJSON_CreateString(key) : NULL;
    if (!job || !marker || !cJSON_AddItemToArray(pending, marker)) {
        store_unlock();
        cJSON_Delete(marker);
        cJSON_Delete(busy_event);
        free(job);
        job_failed("MemoryError('job queue allocation')");
        return false;
    }
    snprintf(job->key, sizeof(job->key), "%s", key);
    job->busy = busy_event;
    job->kind = kind;
    snprintf(job->device, sizeof(job->device), "%s", device ? device : "");
    job->write = write;
    job->has_token = token != NULL;
    snprintf(job->token, sizeof(job->token), "%s", token ? token : "");
    // Publish the pending marker first while the worker is blocked on store_lock().
    bool queued = xQueueSend(queue, &job, 0) == pdTRUE;
    if (!queued) {
        cJSON_Delete(cJSON_DetachItemViaPointer(pending, marker));
    }
    store_unlock();
    if (!queued) {
        cJSON_Delete(job->busy);
        free(job);
    }
    return queued;
}

// ---------------------------------------------------------------- jobs

static void keep(void)
{
    if (running_token[0]) {
        cluster_keep(running_token);
    }
}

static double radio_setting(const char *key, double fallback)
{
    store_lock();
    double value = json_num(cJSON_GetObjectItem(store_config(), "bluetooth"), key, fallback);
    store_unlock();
    return value;
}

static bool job_scan(void)
{
    double timeout = radio_setting("scan_timeout", 25);
    logs_put("scan.start", "seconds", cJSON_CreateNumber(rint(timeout)), NULL);

    store_lock();
    const cJSON *devices = store_devices();
    int known_count = 0;
    uint8_t (*known)[6] = malloc(((size_t)cJSON_GetArraySize(devices) + 1) * sizeof(*known));
    if (!known) {
        store_unlock();
        job_failed("MemoryError('known devices')");
        return false;
    }
    const cJSON *record;
    cJSON_ArrayForEach(record, devices) {
        if (lywsd02_parse_key(record->string, known[known_count])) {
            known_count++;
        }
    }
    store_unlock();

    ble_seen_t *seen = NULL;
    int count = ble_clock_scan(timeout, (const uint8_t (*)[6])known, known_count, &seen);
    free(known);
    keep();
    if (count < 0 || (count && !seen)) {
        free(seen);
        job_failed("BleakError('scan failed')");
        return false;
    }

    double now = store_now();
    store_lock();
    cJSON *heard = cJSON_CreateObject();
    bool complete = heard != NULL;
    // Build the entire heard set before changing records or clearing any RSSI.
    for (int i = 0; complete && i < count; i++) {
        char key[13];
        lywsd02_key(seen[i].addr, key);
        complete = json_set(heard, key, cJSON_CreateTrue()) != NULL;
    }
    if (!complete) {
        cJSON_Delete(heard);
        store_unlock();
        free(seen);
        job_failed("MemoryError('scan heard set')");
        return false;
    }
    for (int i = 0; i < count; i++) {
        char key[13], address[18];
        lywsd02_key(seen[i].addr, key);
        lywsd02_address(seen[i].addr, address);
        // Ignoring a new record at the limit must not prevent refreshing old ones.
        if (!cJSON_GetObjectItemCaseSensitive(devices, key)
                && cJSON_GetArraySize(devices) >= MAX_DEVICES) {
            ESP_LOGW(TAG, "device limit reached, skipping %s", key);
            continue;
        }
        cJSON *entry = jobs_record(key);
        if (!entry
                || !json_set(entry, "address", cJSON_CreateString(address))
                || !json_set(entry, "name", seen[i].adv.name[0]
                             ? cJSON_CreateString(seen[i].adv.name) : cJSON_CreateNull())
                || !json_set(entry, "rssi", cJSON_CreateNumber(seen[i].rssi))
                || !json_set(entry, "product_id", seen[i].adv.product_id >= 0
                             ? cJSON_CreateNumber(seen[i].adv.product_id) : cJSON_CreateNull())
                || !json_set(entry, "seen_at", cJSON_CreateNumber(now))) {
            complete = false;
            break;
        }
    }
    // An incomplete scan must not invalidate the previous signal reports.
    if (complete) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, devices) {
            if (!cJSON_GetObjectItemCaseSensitive(heard, entry->string)
                    && !json_set(entry, "rssi", cJSON_CreateNull())) {
                complete = false;
                break;
            }
        }
    }
    cJSON_Delete(heard);
    store_unlock();
    free(seen);
    store_save_devices();
    if (!complete) {
        job_failed("MemoryError('scan device records')");
        return false;
    }
    logs_put("scan.done", "count", cJSON_CreateNumber(count), NULL);
    return true;
}

static uint8_t locate(const uint8_t addr[6], const char *name, const char *address)
{
    uint8_t type;
    if (ble_clock_find(addr, radio_setting("scan_timeout", 25), &type)) {
        return type;
    }
    logs_put("device.notAdvertising", "target", cJSON_CreateString(name),
             "address", cJSON_CreateString(address), NULL);
    return BLE_ADDR_PUBLIC;
}

// Seconds to add to our own clock to get the configured reference. Only the job
// worker calls this, so the cache needs no lock.
static bool time_offset(const char *server, double *offset, char *error, size_t size)
{
    static char cached[65];
    static double cached_offset, cached_at;
    if (!server[0]) {
        *offset = 0;
        return true;
    }
    double now = cluster_clock();
    if (strcmp(cached, server) == 0 && now - cached_at < TIME_SERVER_CACHE) {
        *offset = cached_offset;
        return true;
    }
    if (!ntp_query(server, offset, error, size)) {
        return false;
    }
    snprintf(cached, sizeof(cached), "%s", server);
    cached_offset = *offset;
    cached_at = cluster_clock();
    logs_put("time.offset", "server", cJSON_CreateString(server),
             "offset", cJSON_CreateNumber(rint(*offset * 1000) / 1000 + 0.0), NULL);  // no "-0"
    return true;
}

// False when local memory/record failures leave work incomplete, not for a
// completed attempt that records an ordinary device or time-server error.
static bool job_visit(const char *key, bool write)
{
    char name[128], address[40], server[65];
    store_lock();
    cJSON *record = jobs_record(key);
    if (!record) {
        store_unlock();
        job_failed("RuntimeError('device record unavailable')");
        return false;
    }
    snprintf(name, sizeof(name), "%s", jobs_name_of(key));
    snprintf(address, sizeof(address), "%s", json_str(record, "address", key));
    snprintf(server, sizeof(server), "%s", json_str(store_config(), "time_server", ""));
    const cJSON *radio = cJSON_GetObjectItem(store_config(), "bluetooth");
    int retries = (int)json_num(radio, "retries", 3);
    ble_visit_t how = {
        .write = write,
        .env = true,
        .connect_timeout = json_num(radio, "connect_timeout", 20),
        .env_timeout = json_num(radio, "env_timeout", 15),
    };
    store_unlock();

    logs_put(write ? "device.syncing" : "device.reading", "target", cJSON_CreateString(name),
             NULL);
    char time_error[160];
    if (!time_offset(server, &how.offset, time_error, sizeof(time_error))) {
        if (write) {
            // Writing our own time when somebody asked for a server would be quietly
            // doing the wrong thing; a read only reports, so it carries on.
            char error[240];
            snprintf(error, sizeof(error), "%s: %s", server, time_error);
            store_lock();
            record = jobs_record(key);
            bool saved = record && json_set(record, "error", cJSON_CreateString(error))
                && json_set(record, "error_at", cJSON_CreateNumber(store_now()));
            store_unlock();
            store_save_devices();
            if (!saved) {
                job_failed("MemoryError('device error record')");
            }
            logs_put("time.serverFailed", "target", cJSON_CreateString(name),
                     "server", cJSON_CreateString(server),
                     "error", cJSON_CreateString(time_error), NULL);
            return saved;
        }
        logs_put("time.serverFallback", "server", cJSON_CreateString(server),
                 "error", cJSON_CreateString(time_error), NULL);
        how.offset = 0;
    }
    char last_error[200] = "";
    cJSON *report = NULL;
    uint8_t addr[6];
    if (!lywsd02_parse_key(address, addr) && !lywsd02_parse_key(key, addr)) {
        snprintf(last_error, sizeof(last_error), "ValueError('not a Bluetooth address')");
        retries = 0;
    } else {
        uint8_t type = locate(addr, name, address);
        for (int attempt = 1; attempt <= retries; attempt++) {
            keep();
            report = cJSON_CreateObject();
            if (!report) {
                job_failed("MemoryError('device report')");
                return false;
            }
            if (ble_clock_visit(addr, type, &how, report, last_error, sizeof(last_error))) {
                break;
            }
            cJSON_Delete(report);
            report = NULL;
            logs_put("device.retry", "target", cJSON_CreateString(name),
                     "attempt", cJSON_CreateNumber(attempt), "total", cJSON_CreateNumber(retries),
                     "error", cJSON_CreateString(last_error), NULL);
            if (attempt < retries) {
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                keep();
                type = locate(addr, name, address);
            }
        }
    }

    double now = store_now();
    if (!report) {
        char error[260];
        if (retries) {
            snprintf(error, sizeof(error), "gave up after %d attempts: %s", retries, last_error);
        } else {
            snprintf(error, sizeof(error), "%s", last_error);
        }
        store_lock();
        record = jobs_record(key);
        bool saved = record && json_set(record, "error", cJSON_CreateString(error))
            && json_set(record, "error_at", cJSON_CreateNumber(now));
        store_unlock();
        store_save_devices();
        if (!saved) {
            job_failed("MemoryError('device error record')");
        }
        logs_put("device.failed", "target", cJSON_CreateString(name),
                 "error", cJSON_CreateString(error), NULL);
        return saved;
    }

    cJSON *last = cJSON_Duplicate(report, true);
    if (!last || !json_set(last, "at", cJSON_CreateNumber(now))) {
        cJSON_Delete(last);
        cJSON_Delete(report);
        job_failed("MemoryError('device history')");
        return false;
    }
    store_lock();
    record = jobs_record(key);
    if (!record) {
        store_unlock();
        cJSON_Delete(last);
        cJSON_Delete(report);
        job_failed("RuntimeError('device record unavailable')");
        return false;
    }
    bool saved = json_set(record, "last", last)
        && json_set(record, "error", cJSON_CreateNull())
        && json_set(record, "error_at", cJSON_CreateNull());
    store_unlock();
    store_save_devices();
    if (!saved) {
        cJSON_Delete(report);
        job_failed("MemoryError('device history record')");
        return false;
    }

    const char *env_error = json_str(report, "env_error", NULL);
    if (env_error) {
        logs_put("device.envFailed", "target", cJSON_CreateString(name),
                 "error", cJSON_CreateString(env_error), NULL);
    }
    if (write) {
        logs_put(json_bool(report, "synced", false) ? "device.synced" : "device.syncedOff",
                 "target", cJSON_CreateString(name), "payload", copy_of(report, "wrote"),
                 "drift", copy_of(report, "drift"), NULL);
    } else {
        logs_put("device.read", "target", cJSON_CreateString(name),
                 "drift", copy_of(report, "drift"),
                 "temperature", copy_of(report, "temperature"),
                 "humidity", copy_of(report, "humidity"), NULL);
    }
    cJSON_Delete(report);
    return true;
}

static cJSON *merged_now(void)
{
    store_lock();
    cJSON *local = jobs_local_views();
    cJSON *merged = local ? cluster_merge(local) : NULL;
    store_unlock();
    cJSON_Delete(local);
    return merged;
}

static bool append_key(cJSON *keys, const char *key)
{
    cJSON *item = cJSON_CreateString(key);
    if (!item || !cJSON_AddItemToArray(keys, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static bool job_read_owned(void)
{
    // Ownership is worked out from the peers' signal reports, and an idle node's
    // copy of those can be a minute old.
    cluster_ensure_fresh();
    cJSON *merged = merged_now();
    cJSON *keys = merged ? cJSON_CreateArray() : NULL;
    if (!keys) {
        cJSON_Delete(merged);
        job_failed("MemoryError('owned device views')");
        return false;
    }
    bool complete = true;
    store_lock();
    const cJSON *device;
    cJSON_ArrayForEach(device, merged) {
        const char *key = json_str(device, "key", "");
        if (strcmp(json_str(device, "node_id", ""), store_node_id()) == 0
                && cJSON_GetObjectItemCaseSensitive(store_devices(), key)
                && !append_key(keys, key)) {
            complete = false;
            break;
        }
    }
    store_unlock();
    cJSON_Delete(merged);
    if (!complete) {
        cJSON_Delete(keys);
        job_failed("MemoryError('owned device keys')");
        return false;
    }
    if (!cJSON_GetArraySize(keys)) {
        logs_put("read.nothingHere", NULL);
    }
    const cJSON *key;
    cJSON_ArrayForEach(key, keys) {
        if (!job_visit(key->valuestring, false)) {
            complete = false;
            break;
        }
    }
    cJSON_Delete(keys);
    return complete;
}

static bool job_auto(void)
{
    if (!job_scan()) {
        return false;
    }
    cJSON *merged = merged_now();
    cJSON *mine = merged ? cJSON_CreateArray() : NULL;
    cJSON *taken = mine ? cJSON_CreateArray() : NULL;
    bool complete = taken && cluster_my_share(merged, mine, taken);
    cJSON_Delete(merged);
    if (!complete) {
        cJSON_Delete(mine);
        cJSON_Delete(taken);
        job_failed("MemoryError('scheduled device keys')");
        return false;
    }
    const cJSON *key;
    cJSON_ArrayForEach(key, taken) {
        store_lock();
        cJSON *target = cJSON_CreateString(jobs_name_of(key->valuestring));
        store_unlock();
        logs_put("cluster.takeover", "target", target, NULL);
    }
    bool none = !cJSON_GetArraySize(mine) && !cJSON_GetArraySize(taken);
    cJSON_ArrayForEach(key, mine) {
        if (!job_visit(key->valuestring, true)) {
            complete = false;
            break;
        }
    }
    if (complete) {
        cJSON_ArrayForEach(key, taken) {
            if (!job_visit(key->valuestring, true)) {
                complete = false;
                break;
            }
        }
    }
    if (none) {
        logs_put("schedule.nothingHere", NULL);
    }
    cJSON_Delete(mine);
    cJSON_Delete(taken);
    return complete;
}

static bool start_radio(void)
{
    esp_err_t err = ble_clock_start();
    if (err == ESP_OK) {
        return true;
    }
    char error[96];
    snprintf(error, sizeof(error), "RuntimeError('bluetooth did not start: %s')",
             esp_err_to_name(err));
    job_failed(error);
    return false;
}

// job_scan_cluster() and job_read_cluster(): every node at once, and the lock
// held until the last one is done.
static bool job_cluster(const char *token, const char *path, const char *event,
                        const char *timeout, bool (*local)(void))
{
    double started = cluster_clock();
    cluster_peer_t *peers = NULL;
    int count = cluster_start_on_peers(path, token, event, &peers);
    if (count < 0 || (count > 0 && !peers)) {
        free(peers);
        job_failed("MemoryError('cluster peers')");
        return false;  // Do not start the local radio with an incomplete peer list.
    }
    bool complete = start_radio();
    if (complete) {
        complete = local();
    }
    // Free the local BLE stack before remote retries/waits consume network memory.
    // stop() logs failures and preserves its running guard; the worker retries it.
    ble_clock_stop();
    ESP_LOGI(TAG, "local BLE stop requested, heap %u, lowest %u, largest %u",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // A peer may scan longer or retry more than we do, so wait as long as the lock
    // allows; this returns as soon as every peer has gone idle.
    if (count > 0
            && !cluster_wait_for_peers(peers, count, started + CLUSTER_LOCK_TTL - 60, token)) {
        logs_put(timeout, NULL);
    }
    free(peers);
    return complete;
}

static bool run(const job_t *job)
{
    switch (job->kind) {
    case JOB_SCAN:
        return job_scan();
    case JOB_SCAN_CLUSTER:
        return job_cluster(job->token, "/api/node/scan", "cluster.scanPeers", "cluster.scanTimeout",
                           job_scan);
    case JOB_READ_OWNED:
        return job_read_owned();
    case JOB_READ_CLUSTER:
        return job_cluster(job->token, "/api/node/read", "cluster.readPeers", "cluster.readTimeout",
                           job_read_owned);
    case JOB_VISIT:
        return job_visit(job->device, job->write);
    case JOB_AUTO:
        return job_auto();
    }
    return false;
}

static void worker(void *arg)
{
    for (;;) {
        job_t *job;
        xQueueReceive(queue, &job, portMAX_DELAY);
        store_lock();
        busy = job->busy;
        job->busy = NULL;
        snprintf(running_token, sizeof(running_token), "%s", job->token);
        store_unlock();

        // Held for all local BLE operations and their cleanup, including failed
        // starts. Retaining this small mutex during peer waits is intentional.
        BaseType_t locked = xSemaphoreTake(radio_lifecycle, portMAX_DELAY);
        configASSERT(locked == pdTRUE);
        bool complete = false;
        if (locked == pdTRUE) {
            if (job->kind == JOB_SCAN_CLUSTER || job->kind == JOB_READ_CLUSTER) {
                complete = run(job);  // Cluster jobs check peer allocation before start.
            } else if (start_radio()) {
                complete = run(job);
            }
            // A second stop is a no-op after success, or retries a failed teardown.
            ble_clock_stop();
            jobs_ble_unlock();
        }
        ESP_LOGI(TAG, "%s done, heap %u, lowest %u, largest %u", job->key,
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        store_lock();
        bool save_state = job->kind == JOB_AUTO && complete;
        if (save_state) {
            save_state = json_set(store_state(), "last_auto_run", cJSON_CreateNumber(store_now()))
                != NULL;
        }
        cJSON_Delete(busy);
        busy = NULL;
        running_token[0] = 0;
        cJSON *item = pending->child;
        for (int i = 0; item; item = item->next, i++) {
            if (strcmp(item->valuestring, job->key) == 0) {
                cJSON_DeleteItemFromArray(pending, i);
                break;
            }
        }
        store_unlock();
        if (save_state) {
            store_save_state();
        } else if (job->kind == JOB_AUTO && complete) {
            job_failed("MemoryError('schedule completion')");
        }
        if (job->has_token) {
            cluster_release(job->token);
        }
        free(job);
    }
}

static void scheduler(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        if (!wifi_time_synced()) {
            continue;
        }
        double now = store_now();
        store_lock();
        if (is_pending("auto")) {
            store_unlock();
            continue;  // A running job alone decides whether its slot completed.
        }
        cJSON *state = store_state();
        double last_run = json_num(state, "last_auto_run", 0);
        if (!last_run) {
            // A fresh board has no history, and "no history" reads as "the last
            // slot was missed". Start the clock now, as server.py does at startup.
            if (!json_set(state, "last_auto_run", cJSON_CreateNumber(now))) {
                store_unlock();
                job_failed("MemoryError('schedule initial state')");
                continue;
            }
            last_run = now;
            store_save_state();
        }
        double due = jobs_last_scheduled(cJSON_GetObjectItem(store_config(), "schedule"), now);
        store_unlock();
        if (isnan(due) || last_run >= due) {
            continue;
        }

        cJSON *merged = merged_now();
        if (!merged) {
            job_failed("MemoryError('schedule device views')");
            continue;  // An unavailable view is not proof that the slot has no work.
        }
        bool work = cluster_my_work(merged);
        cJSON_Delete(merged);
        if (!work) {
            store_lock();
            bool saved = json_set(store_state(), "last_auto_run", cJSON_CreateNumber(store_now()))
                != NULL;
            store_unlock();
            if (saved) {
                store_save_state();
                logs_put("schedule.nothingHere", NULL);
            } else {
                job_failed("MemoryError('schedule state')");
            }
            continue;
        }

        cJSON *event = cJSON_CreateObject();
        if (!event || !json_set(event, "event", cJSON_CreateString("busy.scheduled"))
                || !json_set(event, "params", cJSON_CreateObject())) {
            cJSON_Delete(event);
            job_failed("MemoryError('scheduled event')");
            continue;
        }
        char token[33], blocker[97];
        if (!cluster_acquire(event, token, blocker, sizeof(blocker))) {
            cJSON_Delete(event);
            continue;  // someone is on the radio; the slot stays due, retry in a minute
        }
        if (!jobs_enqueue("auto", event, JOB_AUTO, NULL, false, token)) {
            cluster_release(token);
            continue;
        }
        // The worker advances last_auto_run only after the job completes. OOM or
        // queue failures leave this slot due rather than silently losing the run.
        time_t moment = (time_t)due;
        struct tm local;
        localtime_r(&moment, &local);
        char text[24];
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        logs_put("schedule.triggered", "due", cJSON_CreateString(text), NULL);
    }
}

void jobs_start(void)
{
    pending = cJSON_CreateArray();
    configASSERT(pending);
    if (!pending) {
        return;
    }
    queue = xQueueCreate(QUEUE_DEPTH, sizeof(job_t *));
    configASSERT(queue);
    if (!queue) {
        cJSON_Delete(pending);
        pending = NULL;
        return;
    }
    radio_lifecycle = xSemaphoreCreateMutex();
    configASSERT(radio_lifecycle);
    if (!radio_lifecycle) {
        vQueueDelete(queue);
        queue = NULL;
        cJSON_Delete(pending);
        pending = NULL;
        return;
    }
    BaseType_t result = xTaskCreate(worker, "jobs", 8192, NULL, 4, NULL);
    configASSERT(result == pdPASS);
    if (result != pdPASS) {
        vSemaphoreDelete(radio_lifecycle);
        radio_lifecycle = NULL;
        vQueueDelete(queue);
        queue = NULL;
        cJSON_Delete(pending);
        pending = NULL;
        return;
    }
    result = xTaskCreate(scheduler, "schedule", 6144, NULL, 3, NULL);
    configASSERT(result == pdPASS);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "scheduler task allocation failed");
    }
}
