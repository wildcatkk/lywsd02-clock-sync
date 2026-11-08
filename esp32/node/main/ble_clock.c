#include "ble_clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store.h"
#include "wifi.h"

static const char *TAG = "ble";

#define SYNCED BIT0
#define SCAN_DONE BIT1
#define FOUND BIT2
#define CONNECTED BIT3
#define CONNECT_FAILED BIT4
#define DISCONNECTED BIT5
#define OP_DONE BIT6
#define NOTIFIED BIT7
#define HOST_EXITED BIT8

#define OP_TIMEOUT_MS 10000
#define SCAN_CAPACITY 200
#define VALUE_MAX 64
// 2025-01-01: a clock earlier than this has not been set by SNTP.
#define SANE_EPOCH 1735689600.0

static EventGroupHandle_t bits;
static SemaphoreHandle_t guard;
static bool running, host_stopped, stopping;
static uint8_t own_addr_type;

// Callbacks run on the NimBLE host task. Each operation gets a fresh generation,
// and a callback whose generation is not the current one belongs to an operation
// that has already timed out and is ignored.
static uint32_t generation;

static struct {
    ble_seen_t *table;
    int count;
    const uint8_t (*known)[6];
    int known_count;
    uint8_t target[6];
    uint8_t found_type;
} scan;

static struct {
    uint32_t conn_gen;
    uint16_t conn;
    int connect_status;
    int disconnect_reason;
    int status;
    uint8_t data[VALUE_MAX];
    size_t length;
    uint16_t env_handle;
    uint8_t note[VALUE_MAX];
    size_t note_length;
    // characteristic discovery
    uint16_t model, firmware, serial, battery, env, env_end, clock;
    bool after_env;
    uint16_t cccd;
} link;

static void lock(void) { xSemaphoreTake(guard, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(guard); }

static void *tag(uint32_t gen) { return (void *)(uintptr_t)gen; }

static bool current(void *arg) { return (uintptr_t)arg == generation; }

static uint32_t next_generation(void)
{
    lock();
    uint32_t gen = ++generation;
    unlock();
    return gen;
}

void ble_clock_float(double value, char *out, size_t size)
{
    snprintf(out, size, "%.15g", value);
    if (!strpbrk(out, ".en")) {
        strncat(out, ".0", size - strlen(out) - 1);
    }
}

// ---------------------------------------------------------------- host

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    xEventGroupSetBits(bits, SYNCED);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    xEventGroupClearBits(bits, SYNCED);
}

static void host_task(void *param)
{
    nimble_port_run();
    // nimble_port_stop() returns before this task is deleted. Let the caller
    // delete it through esp_nimble_disable() before freeing or reinitializing
    // the stack, so an old task cannot delete a newly created host task.
    xEventGroupSetBits(bits, HOST_EXITED);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

esp_err_t ble_clock_start(void)
{
    if (running) {
        return !stopping && !host_stopped && (xEventGroupGetBits(bits) & SYNCED)
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!bits) {
        bits = xEventGroupCreate();
        if (!bits) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!guard) {
        guard = xSemaphoreCreateMutex();
        if (!guard) {
            vEventGroupDelete(bits);
            bits = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(bits, 0xFF | HOST_EXITED);
    wifi_share_radio(true);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        wifi_share_radio(false);
        return err;
    }
    running = true;
    host_stopped = true;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // The public enable API reports task allocation failure instead of aborting.
    err = esp_nimble_enable(host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "host task creation failed: %s", esp_err_to_name(err));
        ble_clock_stop();
        return err;
    }
    host_stopped = false;
    if (!(xEventGroupWaitBits(bits, SYNCED, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000)) & SYNCED)) {
        ESP_LOGE(TAG, "host did not sync");
        ble_clock_stop();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void ble_clock_stop(void)
{
    if (!running) {
        return;
    }
    // A failed stop/deinit must block both re-init and reuse of a partial stack.
    stopping = true;
    next_generation();
    if (!host_stopped) {
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "host did not stop: %d", rc);
            return;
        }
        if (!(xEventGroupWaitBits(bits, HOST_EXITED, pdFALSE, pdTRUE,
                                  pdMS_TO_TICKS(5000)) & HOST_EXITED)) {
            ESP_LOGE(TAG, "host task did not exit");
            return;
        }
        esp_err_t err = esp_nimble_disable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "host task deletion failed: %s", esp_err_to_name(err));
            return;
        }
        host_stopped = true;
        xEventGroupClearBits(bits, SYNCED);
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deinit failed: %s", esp_err_to_name(err));
        return;
    }
    running = false;
    stopping = false;
    wifi_share_radio(false);
}

// ---------------------------------------------------------------- scan

static bool is_known(const uint8_t addr[6])
{
    for (int i = 0; i < scan.known_count; i++) {
        if (memcmp(scan.known[i], addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

static int on_scan(struct ble_gap_event *event, void *arg)
{
    lock();
    if (!current(arg)) {
        unlock();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        xEventGroupSetBits(bits, SCAN_DONE);
        unlock();
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) {
        unlock();
        return 0;
    }
    const struct ble_gap_disc_desc *disc = &event->disc;
    if (!scan.table) {
        // ble_clock_find(): only one address matters.
        if (memcmp(disc->addr.val, scan.target, 6) == 0) {
            scan.found_type = disc->addr.type;
            xEventGroupSetBits(bits, FOUND);
            ble_gap_disc_cancel();
        }
        unlock();
        return 0;
    }

    ble_seen_t *slot = NULL;
    for (int i = 0; i < scan.count; i++) {
        if (memcmp(scan.table[i].addr, disc->addr.val, 6) == 0) {
            slot = &scan.table[i];
            break;
        }
    }
    lywsd02_adv_t adv;
    if (slot) {
        adv = slot->adv;
    } else {
        lywsd02_adv_init(&adv);
    }
    lywsd02_parse_adv(disc->data, disc->length_data, &adv);
    bool wanted = lywsd02_looks_like_clock(&adv) || is_known(disc->addr.val);
    if (!slot && scan.count < SCAN_CAPACITY) {
        slot = &scan.table[scan.count++];
    } else if (!slot && wanted) {
        // A busy room fills the table with phones and earbuds; a clock that turns
        // up late still gets a place.
        for (int i = 0; i < scan.count && !slot; i++) {
            if (!lywsd02_looks_like_clock(&scan.table[i].adv) && !is_known(scan.table[i].addr)) {
                slot = &scan.table[i];
            }
        }
    }
    if (slot) {
        memcpy(slot->addr, disc->addr.val, 6);
        slot->addr_type = disc->addr.type;
        if (disc->rssi != 127) {
            slot->rssi = disc->rssi;
        }
        slot->adv = adv;
    }
    unlock();
    return 0;
}

static int discover(double seconds, uint32_t gen)
{
    struct ble_gap_disc_params params = {
        // Half the air time: WiFi shares the antenna and must keep answering.
        .itvl = 96,
        .window = 48,
        .passive = 0,
        .filter_duplicates = 0,
    };
    return ble_gap_disc(own_addr_type, (int32_t)(seconds * 1000), &params, on_scan, tag(gen));
}

int ble_clock_scan(double seconds, const uint8_t (*known)[6], int known_count, ble_seen_t **out)
{
    *out = NULL;
    ble_seen_t *table = calloc(SCAN_CAPACITY, sizeof(ble_seen_t));
    if (!table) {
        return -1;
    }
    lock();
    uint32_t gen = ++generation;
    scan.table = table;
    scan.count = 0;
    scan.known = known;
    scan.known_count = known_count;
    unlock();
    xEventGroupClearBits(bits, SCAN_DONE);
    int rc = discover(seconds, gen);
    if (rc == 0 && !(xEventGroupWaitBits(bits, SCAN_DONE, pdTRUE, pdTRUE,
                                         pdMS_TO_TICKS(seconds * 1000 + 3000)) & SCAN_DONE)) {
        ble_gap_disc_cancel();
    }
    lock();
    generation++;
    int heard = scan.count;
    scan.table = NULL;
    scan.known = NULL;
    scan.known_count = 0;
    unlock();
    if (rc != 0) {
        ESP_LOGE(TAG, "scan did not start: %d", rc);
        free(table);
        return -1;
    }

    int kept = 0;
    for (int i = 0; i < heard; i++) {
        bool wanted = lywsd02_looks_like_clock(&table[i].adv);
        for (int k = 0; k < known_count && !wanted; k++) {
            wanted = memcmp(known[k], table[i].addr, 6) == 0;
        }
        if (wanted) {
            table[kept++] = table[i];
        }
    }
    ESP_LOGI(TAG, "heard %d device(s), %d of interest", heard, kept);
    if (!kept) {
        free(table);
        return 0;
    }
    ble_seen_t *smaller = realloc(table, kept * sizeof(ble_seen_t));
    *out = smaller ? smaller : table;
    return kept;
}

bool ble_clock_find(const uint8_t addr[6], double seconds, uint8_t *addr_type)
{
    lock();
    uint32_t gen = ++generation;
    scan.table = NULL;
    memcpy(scan.target, addr, 6);
    unlock();
    xEventGroupClearBits(bits, SCAN_DONE | FOUND);
    if (discover(seconds, gen) != 0) {
        return false;
    }
    EventBits_t got = xEventGroupWaitBits(bits, SCAN_DONE | FOUND, pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(seconds * 1000 + 3000));
    if (!got) {
        ble_gap_disc_cancel();
    }
    next_generation();
    if (got & FOUND) {
        *addr_type = scan.found_type;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- connection

static int on_link(struct ble_gap_event *event, void *arg)
{
    lock();
    if ((uintptr_t)arg != link.conn_gen) {
        unlock();
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        link.connect_status = event->connect.status;
        if (event->connect.status == 0) {
            link.conn = event->connect.conn_handle;
            xEventGroupSetBits(bits, CONNECTED);
        } else {
            xEventGroupSetBits(bits, CONNECT_FAILED);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        link.disconnect_reason = event->disconnect.reason;
        xEventGroupSetBits(bits, DISCONNECTED);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == link.env_handle && link.env_handle) {
            uint16_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (length > sizeof(link.note)) {
                length = sizeof(link.note);
            }
            os_mbuf_copydata(event->notify_rx.om, 0, length, link.note);
            link.note_length = length;
            xEventGroupSetBits(bits, NOTIFIED);
        }
        break;
    default:
        break;
    }
    unlock();
    return 0;
}

static uint32_t begin(void)
{
    lock();
    uint32_t gen = ++generation;
    link.status = 0;
    link.length = 0;
    xEventGroupClearBits(bits, OP_DONE);
    unlock();
    return gen;
}

static int finish(int rc)
{
    if (rc == 0) {
        EventBits_t got = xEventGroupWaitBits(bits, OP_DONE | DISCONNECTED, pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(OP_TIMEOUT_MS));
        rc = (got & OP_DONE) ? link.status : (got & DISCONNECTED) ? BLE_HS_ENOTCONN
                                                                  : BLE_HS_ETIMEOUT;
    }
    next_generation();
    return rc;
}

static void done(int status)
{
    link.status = status == BLE_HS_EDONE ? 0 : status;
    xEventGroupSetBits(bits, OP_DONE);
}

static int on_read(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                   void *arg)
{
    lock();
    if (current(arg)) {
        if (error->status == 0 && attr) {
            size_t length = OS_MBUF_PKTLEN(attr->om);
            if (attr->offset < sizeof(link.data)) {
                size_t room = sizeof(link.data) - attr->offset;
                size_t copy = length < room ? length : room;
                os_mbuf_copydata(attr->om, 0, copy, link.data + attr->offset);
                if (attr->offset + copy > link.length) {
                    link.length = attr->offset + copy;
                }
            }
        } else {
            done(error->status);
        }
    }
    unlock();
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                    void *arg)
{
    lock();
    if (current(arg)) {
        done(error->status);
    }
    unlock();
    return 0;
}

static const uint8_t BASE[16] = {
    0xa6, 0xa3, 0x7d, 0x99, 0xf2, 0x6f, 0x1a, 0x8a, 0x0c, 0x4b, 0x0a, 0x7a, 0x00, 0xcc, 0xe0, 0xeb,
};

// ebe0ccXX-7a0a-4b0c-8a1a-6ff2997da3a6 -> XX, or 0 for any other UUID.
static int xiaomi_short(const ble_uuid_any_t *uuid)
{
    if (uuid->u.type != BLE_UUID_TYPE_128 || memcmp(uuid->u128.value, BASE, 12) != 0
            || memcmp(uuid->u128.value + 13, BASE + 13, 3) != 0) {
        return 0;
    }
    return uuid->u128.value[12];
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                  void *arg)
{
    lock();
    if (current(arg)) {
        if (error->status == 0 && chr) {
            if (link.after_env) {
                link.env_end = chr->def_handle - 1;
                link.after_env = false;
            }
            int id16 = chr->uuid.u.type == BLE_UUID_TYPE_16 ? chr->uuid.u16.value : 0;
            int xiaomi = xiaomi_short(&chr->uuid);
            if (id16 == 0x2a24) {
                link.model = chr->val_handle;
            } else if (id16 == 0x2a26) {
                link.firmware = chr->val_handle;
            } else if (id16 == 0x2a25) {
                link.serial = chr->val_handle;
            } else if (xiaomi == 0xc4) {
                link.battery = chr->val_handle;
            } else if (xiaomi == 0xc1) {
                link.env = chr->val_handle;
                link.env_end = 0xffff;
                link.after_env = true;
            } else if (xiaomi == 0xb7) {
                link.clock = chr->val_handle;
            }
        } else {
            done(error->status);
        }
    }
    unlock();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    lock();
    if (current(arg)) {
        if (error->status == 0 && dsc) {
            if (!link.cccd && dsc->uuid.u.type == BLE_UUID_TYPE_16
                    && dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
                link.cccd = dsc->handle;
            }
        } else {
            done(error->status);
        }
    }
    unlock();
    return 0;
}

static int read_value(uint16_t handle, uint8_t *out, size_t *length)
{
    if (!handle) {
        return BLE_HS_ENOENT;
    }
    uint32_t gen = begin();
    int rc = finish(ble_gattc_read_long(link.conn, handle, 0, on_read, tag(gen)));
    *length = link.length;
    memcpy(out, link.data, link.length);
    return rc;
}

static int write_value(uint16_t handle, const uint8_t *data, size_t length)
{
    uint32_t gen = begin();
    return finish(ble_gattc_write_flat(link.conn, handle, data, length, on_write, tag(gen)));
}

static void describe(char *out, size_t size, const char *what, int rc)
{
    if (rc == BLE_HS_ETIMEOUT) {
        snprintf(out, size, "TimeoutError('%s timed out')", what);
    } else if (rc == BLE_HS_ENOTCONN) {
        snprintf(out, size, "ConnectionError('disconnected during %s, reason 0x%x')", what,
                 link.disconnect_reason);
    } else if (rc == BLE_HS_ENOENT) {
        snprintf(out, size, "BleakError('%s: characteristic not found')", what);
    } else {
        snprintf(out, size, "BleakError('%s failed, status 0x%x')", what, rc);
    }
}

// _read_text(): ASCII with U+FFFD for anything else, NULs and spaces trimmed.
static cJSON *read_text(uint16_t handle)
{
    uint8_t raw[VALUE_MAX];
    size_t length;
    if (read_value(handle, raw, &length) != 0) {
        return cJSON_CreateNull();
    }
    size_t start = 0, end = length;
    while (start < end && raw[start] == 0) {
        start++;
    }
    while (end > start && raw[end - 1] == 0) {
        end--;
    }
    while (start < end && (raw[start] == ' ' || (raw[start] >= '\t' && raw[start] <= '\r'))) {
        start++;
    }
    while (end > start && (raw[end - 1] == ' ' || (raw[end - 1] >= '\t' && raw[end - 1] <= '\r'))) {
        end--;
    }
    if (start == end) {
        return cJSON_CreateNull();
    }
    char text[VALUE_MAX * 3 + 1];
    size_t at = 0;
    for (size_t i = start; i < end; i++) {
        if (raw[i] < 0x80) {
            text[at++] = raw[i];
        } else {
            memcpy(text + at, "\xef\xbf\xbd", 3);
            at += 3;
        }
    }
    text[at] = 0;
    return cJSON_CreateString(text);
}

static void hex(const uint8_t *data, size_t length, char *out)
{
    for (size_t i = 0; i < length; i++) {
        sprintf(out + i * 2, "%02x", data[i]);
    }
    out[length * 2] = 0;
}

static bool env_report(const uint8_t *data, size_t length, cJSON *report)
{
    double temperature, voltage;
    int humidity;
    if (!lywsd02_decode_env(data, length, &temperature, &humidity, &voltage)) {
        return false;
    }
    json_set(report, "temperature", cJSON_CreateNumber(temperature));
    json_set(report, "humidity", cJSON_CreateNumber(humidity));
    if (!isnan(voltage)) {
        json_set(report, "voltage", cJSON_CreateNumber(voltage));
    }
    return true;
}

// read_env(): the LYWSD02 answers a read of ccc1 with nothing and only ever sends
// the values as a notification; the LYWSD02MMC answers the read.
static bool read_env(double timeout, cJSON *report, char *error, size_t size)
{
    uint8_t raw[VALUE_MAX];
    size_t length;
    if (read_value(link.env, raw, &length) == 0 && env_report(raw, length, report)) {
        return true;
    }
    if (!link.env) {
        describe(error, size, "sensor", BLE_HS_ENOENT);
        return false;
    }
    uint32_t gen = begin();
    int rc = finish(ble_gattc_disc_all_dscs(link.conn, link.env, link.env_end, on_dsc, tag(gen)));
    if (rc != 0 || !link.cccd) {
        describe(error, size, "sensor subscription", rc ? rc : BLE_HS_ENOENT);
        return false;
    }
    lock();
    link.env_handle = link.env;
    link.note_length = 0;
    xEventGroupClearBits(bits, NOTIFIED);
    unlock();
    static const uint8_t on[2] = {1, 0}, off[2] = {0, 0};
    rc = write_value(link.cccd, on, sizeof(on));
    if (rc != 0) {
        describe(error, size, "sensor subscription", rc);
        return false;
    }

    bool ok = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout * 1000);
    while (!ok) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            char seconds[24];
            ble_clock_float(timeout, seconds, sizeof(seconds));
            snprintf(error, size, "TimeoutError('no sensor notification within %ss')", seconds);
            break;
        }
        EventBits_t got = xEventGroupWaitBits(bits, NOTIFIED | DISCONNECTED, pdFALSE, pdFALSE,
                                              deadline - now);
        if (got & DISCONNECTED) {
            describe(error, size, "sensor notification", BLE_HS_ENOTCONN);
            break;
        }
        if (got & NOTIFIED) {
            lock();
            xEventGroupClearBits(bits, NOTIFIED);
            length = link.note_length;
            memcpy(raw, link.note, length);
            unlock();
            ok = env_report(raw, length, report);
        }
    }
    lock();
    link.env_handle = 0;
    unlock();
    write_value(link.cccd, off, sizeof(off));
    return ok;
}

static bool clock_report(const uint8_t *data, size_t length, double offset, cJSON *report)
{
    uint32_t stamp;
    int timezone;
    if (!lywsd02_decode_time(data, length, &stamp, &timezone)) {
        return false;
    }
    double now = store_now() + offset;
    double shown = lywsd02_shown_time(stamp, timezone, now);
    json_set(report, "device_time", cJSON_CreateNumber(shown));
    json_set(report, "timezone", cJSON_CreateNumber(timezone));
    json_set(report, "drift", cJSON_CreateNumber(shown - floor(now)));
    return true;
}

static bool session(const ble_visit_t *how, cJSON *report, char *error, size_t size)
{
    uint32_t gen = begin();
    int rc = finish(ble_gattc_disc_all_chrs(link.conn, 1, 0xffff, on_chr, tag(gen)));
    if (rc != 0) {
        describe(error, size, "service discovery", rc);
        return false;
    }

    json_set(report, "model", read_text(link.model));
    json_set(report, "firmware", read_text(link.firmware));
    json_set(report, "serial", read_text(link.serial));
    uint8_t raw[VALUE_MAX];
    size_t length;
    rc = read_value(link.battery, raw, &length);
    json_set(report, "battery", rc == 0 && length ? cJSON_CreateNumber(raw[0]) : cJSON_CreateNull());

    if (how->env) {
        char env_error[160];
        if (!read_env(how->env_timeout, report, env_error, sizeof(env_error))) {
            json_set(report, "env_error", cJSON_CreateString(env_error));
        }
    }

    uint8_t current[VALUE_MAX];
    size_t current_length;
    rc = read_value(link.clock, current, &current_length);
    if (rc != 0) {
        describe(error, size, "time read", rc);
        return false;
    }
    char text[VALUE_MAX * 2 + 1];
    hex(current, current_length, text);
    json_set(report, "raw_time", cJSON_CreateString(text));
    if (!clock_report(current, current_length, how->offset, report)) {
        snprintf(error, size, "error('unpack requires a buffer of 5 bytes')");
        return false;
    }
    if (!how->write) {
        return true;
    }

    // Never write a clock from a board that has not been told the time.
    double now = store_now();
    if (!wifi_time_synced() || now < SANE_EPOCH) {
        snprintf(error, size, "RuntimeError('time not set, refusing to write')");
        return false;
    }
    // LYWSD02MMC reports 7 bytes; whatever follows the timezone byte goes back as it was.
    uint8_t payload[VALUE_MAX];
    lywsd02_encode_time(now + how->offset, payload);
    memcpy(payload + 5, current + 5, current_length - 5);
    rc = write_value(link.clock, payload, current_length);
    if (rc != 0) {
        describe(error, size, "time write", rc);
        return false;
    }
    rc = read_value(link.clock, raw, &length);
    if (rc != 0) {
        describe(error, size, "time read", rc);
        return false;
    }
    if (!clock_report(raw, length, how->offset, report)) {
        snprintf(error, size, "error('unpack requires a buffer of 5 bytes')");
        return false;
    }
    hex(payload, current_length, text);
    json_set(report, "wrote", cJSON_CreateString(text));
    double drift = json_num(report, "drift", NAN);
    json_set(report, "synced", cJSON_CreateBool(fabs(drift) <= LYWSD02_SYNC_TOLERANCE));
    return true;
}

bool ble_clock_visit(const uint8_t addr[6], uint8_t addr_type, const ble_visit_t *how,
                     cJSON *report, char *error, size_t size)
{
    lock();
    memset(&link, 0, sizeof(link));
    link.conn_gen = ++generation;
    uint32_t conn_gen = link.conn_gen;
    unlock();
    xEventGroupClearBits(bits, CONNECTED | CONNECT_FAILED | DISCONNECTED | OP_DONE | NOTIFIED);

    ble_addr_t peer = {.type = addr_type};
    memcpy(peer.val, addr, 6);
    // A longer supervision timeout than the default 2.56 s: WiFi takes the antenna
    // for whole beacons at a time.
    struct ble_gap_conn_params params = {
        .scan_itvl = 16,
        .scan_window = 16,
        .itvl_min = 24,
        .itvl_max = 40,
        .latency = 0,
        .supervision_timeout = 600,
    };
    int ms = (int)(how->connect_timeout * 1000);
    int rc = ble_gap_connect(own_addr_type, &peer, ms, &params, on_link, tag(conn_gen));
    if (rc != 0) {
        describe(error, size, "connect", rc);
        return false;
    }
    EventBits_t got = xEventGroupWaitBits(bits, CONNECTED | CONNECT_FAILED, pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(ms + 3000));
    if (!(got & CONNECTED)) {
        if (!got) {
            ble_gap_conn_cancel();
        }
        lock();
        link.conn_gen = 0;
        int status = link.connect_status;
        unlock();
        if (!got || status == BLE_HS_ETIMEOUT) {
            snprintf(error, size, "TimeoutError()");
        } else {
            describe(error, size, "connect", status);
        }
        return false;
    }

    bool ok = session(how, report, error, size);

    if (!(xEventGroupGetBits(bits) & DISCONNECTED)) {
        ble_gap_terminate(link.conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(bits, DISCONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    }
    lock();
    link.conn_gen = 0;
    unlock();
    return ok;
}
