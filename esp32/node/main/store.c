#include "store.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";
static const char *NAMESPACE = "lywsd02";

// Same defaults as DEFAULT_CONFIG in server.py, minus host and port (the board
// always listens on port 8787) and plus the time zone the board cannot guess: its
// IANA name for the page, and the POSIX rule the C library runs on.
static const char *DEFAULT_CONFIG =
    "{\"language\":\"zh-CN\",\"zone\":\"\",\"timezone\":\"\",\"time_server\":\"\","
    "\"node\":{\"name\":\"\",\"peers\":[],\"cluster\":true,\"discovery\":true},"
    "\"schedule\":{\"mode\":\"weekly\",\"weekday\":6,\"hour\":4,\"minute\":0},"
    "\"bluetooth\":{\"scan_timeout\":25.0,\"connect_timeout\":20.0,"
    "\"env_timeout\":15.0,\"retries\":3}}";

static SemaphoreHandle_t mutex;
static cJSON *config, *state, *devices;
static char node_id[13];
// Readers hold on to the pointer without the lock, so a change is written to the
// spare buffer and then published by flipping the index.
static char hostnames[2][33];
static volatile int hostname_at;
static char default_hostname[33];
static char chip[16];

static cJSON *load(nvs_handle_t nvs, const char *key, const char *fallback)
{
    size_t size = 0;
    esp_err_t err = nvs_get_blob(nvs, key, NULL, &size);
    cJSON *value = NULL;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = cJSON_Parse(fallback);
    } else if (err == ESP_OK && size > 0 && size < SIZE_MAX) {
        char *text = malloc(size + 1);
        if (text) {
            err = nvs_get_blob(nvs, key, text, &size);
            if (err == ESP_OK) {
                text[size] = 0;
                value = cJSON_ParseWithLength(text, size + 1);
            }
            free(text);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    // cJSON does not distinguish malformed input from parse OOM. Fail closed
    // for both, rather than overwrite a stored tree with defaults at startup.
    if (!cJSON_IsObject(value)) {
        ESP_LOGE(TAG, "loading %s failed: %s", key,
                 err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND
                     ? "invalid JSON object or out of memory" : esp_err_to_name(err));
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

static void save(const char *key, const cJSON *value)
{
    store_lock();
    char *text = cJSON_PrintUnformatted(value);
    store_unlock();
    if (!text) {
        ESP_LOGE(TAG, "out of memory saving %s", key);
        return;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, key, text, strlen(text));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    free(text);
}

// load_config() in server.py: the nested blocks are filled in one level deep, so a
// setting added in a later version gets its default without touching the rest.
static bool fill_defaults(cJSON *target)
{
    cJSON *defaults = cJSON_Parse(DEFAULT_CONFIG);
    if (!defaults) {
        return false;
    }
    bool ok = true;
    cJSON *item;
    cJSON_ArrayForEach(item, defaults) {
        cJSON *have = cJSON_GetObjectItem(target, item->string);
        if (!have || (cJSON_IsObject(item) && !cJSON_IsObject(have))) {
            if (!json_set(target, item->string, cJSON_Duplicate(item, true))) {
                ok = false;
                break;
            }
        } else if (cJSON_IsObject(item)) {
            cJSON *inner;
            cJSON_ArrayForEach(inner, item) {
                if (!cJSON_GetObjectItem(have, inner->string)
                    && !json_set(have, inner->string, cJSON_Duplicate(inner, true))) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }
    }
    cJSON_Delete(defaults);
    return ok;
}

void store_init(void)
{
    esp_err_t err = nvs_flash_init();
    ESP_ERROR_CHECK(err);
    mutex = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(mutex ? ESP_OK : ESP_ERR_NO_MEM);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    size_t at = 0;
    for (const char *c = CONFIG_IDF_TARGET; *c && at < sizeof(chip) - 2; c++) {
        if (c - CONFIG_IDF_TARGET == 5) {
            chip[at++] = '-';
        }
        chip[at++] = (char)toupper((unsigned char)*c);
    }
    chip[at] = 0;
    snprintf(default_hostname, sizeof(default_hostname), "%s-%02X%02X", chip, mac[4], mac[5]);

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NAMESPACE, NVS_READWRITE, &nvs));
    size_t size = sizeof(hostnames[0]);
    if (nvs_get_str(nvs, "hostname", hostnames[0], &size) != ESP_OK
        || !store_hostname_valid(hostnames[0])) {
        strcpy(hostnames[0], default_hostname);
    }
    config = load(nvs, "config", "{}");
    state = load(nvs, "state", "{\"last_auto_run\":0}");
    devices = load(nvs, "devices", "{}");
    nvs_close(nvs);
    if (!config || !state || !devices) {
        cJSON_Delete(config);
        cJSON_Delete(state);
        cJSON_Delete(devices);
        config = state = devices = NULL;
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_ERROR_CHECK(fill_defaults(config) ? ESP_OK : ESP_ERR_NO_MEM);

    // The identity is the board's own and survives a settings reset; it is what
    // the other nodes key ownership on.
    const char *stored = json_str(state, "node_id", "");
    if (strlen(stored) == 12) {
        strcpy(node_id, stored);
    } else {
        uint8_t raw[6];
        esp_fill_random(raw, sizeof(raw));
        for (int i = 0; i < 6; i++) {
            sprintf(node_id + i * 2, "%02x", raw[i]);
        }
        ESP_ERROR_CHECK(json_set(state, "node_id", cJSON_CreateString(node_id))
                        ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(state, "nodes"))) {
        ESP_ERROR_CHECK(json_set(state, "nodes", cJSON_CreateObject())
                        ? ESP_OK : ESP_ERR_NO_MEM);
    }
    char name[97];
    store_node_name(name, sizeof(name));
    ESP_ERROR_CHECK(json_set(cJSON_GetObjectItem(state, "nodes"), node_id, cJSON_CreateString(name))
                    ? ESP_OK : ESP_ERR_NO_MEM);
    store_save_state();
    ESP_LOGI(TAG, "node %s (%s), %d device(s) on record", node_id, name,
             cJSON_GetArraySize(devices));
}

void store_lock(void)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
}

void store_unlock(void)
{
    xSemaphoreGiveRecursive(mutex);
}

cJSON *store_config(void) { return config; }
cJSON *store_state(void) { return state; }
cJSON *store_devices(void) { return devices; }

void store_save_config(void) { save("config", config); }
void store_save_state(void) { save("state", state); }
void store_save_devices(void) { save("devices", devices); }

const char *store_node_id(void) { return node_id; }
const char *store_hostname(void) { return hostnames[hostname_at]; }
const char *store_default_hostname(void) { return default_hostname; }
const char *store_chip(void) { return chip; }

bool store_hostname_valid(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length > 32 || name[0] == '-' || name[length - 1] == '-') {
        return false;
    }
    for (const char *c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-') {
            return false;
        }
    }
    return true;
}

bool store_set_hostname(const char *name)
{
    bool reset = name[0] == 0;
    if (!reset && !store_hostname_valid(name)) {
        return false;
    }
    store_lock();
    int spare = !hostname_at;
    strcpy(hostnames[spare], reset ? default_hostname : name);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = reset ? nvs_erase_key(nvs, "hostname") : nvs_set_str(nvs, "hostname", name);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving hostname failed: %s", esp_err_to_name(err));
    }
    hostname_at = spare;
    store_unlock();
    return true;
}

void store_node_name(char *out, size_t size)
{
    store_lock();
    const char *name = json_str(cJSON_GetObjectItem(config, "node"), "name", "");
    snprintf(out, size, "%s", name[0] ? name : store_hostname());
    store_unlock();
}

double store_now(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec + now.tv_usec / 1e6;
}

const char *json_str(const cJSON *object, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

double json_num(const cJSON *object, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

bool json_bool(const cJSON *object, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

cJSON *json_set(cJSON *object, const char *key, cJSON *value)
{
    bool ok = false;
    if (cJSON_IsObject(object) && key && value) {
        ok = cJSON_GetObjectItem(object, key)
            ? cJSON_ReplaceItemInObject(object, key, value)
            : cJSON_AddItemToObject(object, key, value);
    }
    if (!ok) {
        cJSON_Delete(value);
        return NULL;
    }
    return value;
}

cJSON *json_num_or_null(double value)
{
    return isnan(value) ? cJSON_CreateNull() : cJSON_CreateNumber(value);
}
