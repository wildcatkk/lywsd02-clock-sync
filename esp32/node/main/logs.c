#include "logs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "store.h"

// server.py keeps 200; a page only ever shows the last 60 per node, and the RAM is
// better spent on peer states.
#define CAPACITY 80
#define PARAMS_MAX 320

// Entries keep boot time and are put on the wall clock when read, so the ones
// written before SNTP answered come out right once it has.
typedef struct {
    int64_t at_us;
    uint32_t id;
    char event[32];
    char *params;  // compact JSON object
} entry_t;

static const char *TAG = "log";
static entry_t ring[CAPACITY];
static int head, count;
static uint32_t sequence;
static char boot[17];
static SemaphoreHandle_t mutex;

static void new_epoch(void)
{
    snprintf(boot, sizeof(boot), "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());
    sequence = 0;
}

void logs_init(void)
{
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex);
    new_epoch();
}

void logs_put(const char *event, ...)
{
    cJSON *params = cJSON_CreateObject();
    va_list args;
    va_start(args, event);
    for (const char *key = va_arg(args, const char *); key; key = va_arg(args, const char *)) {
        cJSON *value = va_arg(args, cJSON *);
        if (!json_set(params, key, value ? value : cJSON_CreateNull())) {
            cJSON_Delete(params);
            params = NULL;
        }
    }
    va_end(args);

    char *text = cJSON_PrintUnformatted(params);
    // The page may show the network name; serial output ends up in bug reports.
    char *serial = NULL;
    bool redacted = cJSON_HasObjectItem(params, "ssid");
    if (redacted) {
        cJSON_DeleteItemFromObject(params, "ssid");
        serial = cJSON_PrintUnformatted(params);
    }
    cJSON_Delete(params);
    if (!text) {
        free(serial);
        return;
    }
    if (strlen(text) > PARAMS_MAX) {
        // An error string can be a whole stack of reprs; the page has no use for
        // more than the start of it.
        free(text);
        text = strdup("{\"truncated\":true}");
    }
    if (!text) {
        free(serial);
        return;
    }
    ESP_LOGI(TAG, "%s %s", event, redacted ? (serial ? serial : "{}") : text);
    free(serial);

    xSemaphoreTake(mutex, portMAX_DELAY);
    entry_t *slot = &ring[(head + count) % CAPACITY];
    if (count == CAPACITY) {
        free(slot->params);
        head = (head + 1) % CAPACITY;
    } else {
        count++;
    }
    slot->at_us = esp_timer_get_time();
    slot->id = ++sequence;
    snprintf(slot->event, sizeof(slot->event), "%s", event);
    slot->params = text;
    xSemaphoreGive(mutex);
}

void logs_version(char epoch[17], uint32_t *cursor)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    memcpy(epoch, boot, sizeof(boot));
    *cursor = sequence;
    xSemaphoreGive(mutex);
}

int logs_read(log_line_t *out, int limit, const char *epoch, uint32_t after,
              char version[17], uint32_t *cursor)
{
    double offset = store_now() - esp_timer_get_time() / 1e6;
    int made = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (strcmp(epoch, boot) != 0 || after > sequence) {
        after = 0;
    }
    memcpy(version, boot, sizeof(boot));
    *cursor = after;
    int skip = !after && count > limit ? count - limit : 0;
    for (int i = skip; i < count && made < limit; i++) {
        const entry_t *entry = &ring[(head + i) % CAPACITY];
        if (entry->id <= after) {
            continue;
        }
        double at = entry->at_us / 1e6 + offset;
        size_t size = strlen(entry->event) + strlen(entry->params) + 96;
        char *text = malloc(size);
        if (!text) {
            break;
        }
        snprintf(text, size, "{\"id\":%lu,\"at\":%.6f,\"event\":\"%s\",\"params\":%s}",
                 (unsigned long)entry->id, at, entry->event, entry->params);
        out[made++] = (log_line_t){at, text, entry->id};
        *cursor = entry->id;
    }
    xSemaphoreGive(mutex);
    return made;
}

void logs_free(log_line_t *lines, int count)
{
    for (int i = 0; i < count; i++) {
        free(lines[i].text);
    }
}

void logs_clear(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        free(ring[(head + i) % CAPACITY].params);
    }
    head = count = 0;
    new_epoch();
    xSemaphoreGive(mutex);
}
