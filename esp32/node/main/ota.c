#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "logs.h"
#include "store.h"
#include "web.h"

#define CHUNK 4096
#define UPLOAD_TIMEOUT_US (120LL * 1000 * 1000)
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static const char *TAG = "ota";

static void restart(void *arg)
{
    esp_restart();
}

// The image header and the app description sit in the first few hundred bytes, so
// a file for another chip, another project, or a full flash image (bootloader at
// offset 0) is caught before anything is erased.
static bool fits(const uint8_t *data, size_t size, esp_app_desc_t *desc)
{
    if (size < DESC_OFFSET + sizeof(esp_app_desc_t)) {
        return false;
    }
    const esp_image_header_t *header = (const esp_image_header_t *)data;
    memcpy(desc, data + DESC_OFFSET, sizeof(*desc));
    return header->magic == ESP_IMAGE_HEADER_MAGIC
        && header->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID
        && desc->magic_word == ESP_APP_DESC_MAGIC_WORD
        && strncmp(desc->project_name, esp_app_get_description()->project_name,
                   sizeof(desc->project_name)) == 0;
}

// Fills the buffer up to `want` bytes or the end of the body. -1 on a dead socket.
static int receive(httpd_req_t *req, char *buffer, int want, int64_t deadline)
{
    int have = 0, stalls = 0;
    while (have < want) {
        if (esp_timer_get_time() >= deadline) {
            return -1;
        }
        int got = httpd_req_recv(req, buffer + have, want - have);
        if (esp_timer_get_time() >= deadline) {
            return -1;
        }
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 6) {
            continue;
        }
        if (got <= 0) {
            return -1;
        }
        have += got;
        stalls = 0;
    }
    return have;
}

static esp_err_t upload(httpd_req_t *req, char *buffer)
{
    int64_t deadline = esp_timer_get_time() + UPLOAD_TIMEOUT_US;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    size_t remaining = req->content_len;
    if (!target || remaining == 0 || remaining > target->size) {
        return web_send_error(req, "400 Bad Request", "error.otaWrongImage", NULL);
    }

    int first = receive(req, buffer, remaining < CHUNK ? (int)remaining : CHUNK, deadline);
    esp_app_desc_t desc;
    if (first < 0) {
        return ESP_FAIL;
    }
    if (!fits((uint8_t *)buffer, first, &desc)) {
        return web_send_error(req, "400 Bad Request", "error.otaWrongImage", NULL);
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    bool begun = err == ESP_OK;
    int got = first;
    while (err == ESP_OK) {
        if (esp_timer_get_time() >= deadline) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        err = esp_ota_write(handle, buffer, got);
        remaining -= got;
        if (err != ESP_OK || remaining == 0) {
            break;
        }
        got = receive(req, buffer, remaining < CHUNK ? (int)remaining : CHUNK, deadline);
        if (got < 0) {
            esp_ota_abort(handle);
            return ESP_FAIL;
        }
    }
    if (err == ESP_OK && esp_timer_get_time() >= deadline) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        // Checks the whole image, including its hash, before it can be booted.
        // esp_ota_end releases the handle even if validation fails.
        err = esp_ota_end(handle);
    } else if (begun) {
        esp_ota_abort(handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "error", esp_err_to_name(err));
        return web_send_error(req, "500 Internal Server Error", "error.otaFailed", params);
    }

    // A new attempt deserves its own report if it too is rolled back.
    store_lock();
    json_set(store_state(), "rolled_back", cJSON_CreateString(""));
    store_unlock();
    store_save_state();
    logs_put("ota.done", "version", cJSON_CreateString(desc.version), NULL);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    cJSON_AddStringToObject(body, "version", desc.version);
    web_send_json(req, "200 OK", body);

    // Late enough for the reply to leave, early enough that the page is still
    // waiting for the restart rather than wondering what happened.
    const esp_timer_create_args_t args = {.callback = restart, .name = "restart"};
    esp_timer_handle_t timer = NULL;
    err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(timer, 1500 * 1000);
    }
    if (err != ESP_OK) {
        if (timer) {
            esp_timer_delete(timer);
        }
        ESP_LOGE(TAG, "restart timer failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_upload(httpd_req_t *req)
{
    char *buffer = malloc(CHUNK);
    if (!buffer) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "error", esp_err_to_name(ESP_ERR_NO_MEM));
        return web_send_error(req, "503 Service Unavailable", "error.otaFailed", params);
    }
    esp_err_t result = upload(req, buffer);
    free(buffer);
    return result;
}

void ota_settle(bool healthy)
{
    // The bootloader only falls back once the image it tried has failed to confirm
    // itself, so a rollback is seen here, on the boot after it happened.
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        store_lock();
        bool reported = strcmp(json_str(store_state(), "rolled_back", ""), desc.version) == 0;
        if (!reported) {
            json_set(store_state(), "rolled_back", cJSON_CreateString(desc.version));
        }
        store_unlock();
        if (!reported) {
            store_save_state();
            logs_put("ota.rolledBack", "version", cJSON_CreateString(desc.version), NULL);
        }
    }

    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK
            || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (healthy) {
        esp_ota_mark_app_valid_cancel_rollback();
        logs_put("ota.confirmed", "version",
                 cJSON_CreateString(esp_app_get_description()->version), NULL);
    } else {
        ESP_LOGE(TAG, "new image did not come up, going back to the previous one");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

cJSON *ota_firmware_json(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *firmware = cJSON_CreateObject();
    cJSON_AddStringToObject(firmware, "version", app->version);
    cJSON_AddStringToObject(firmware, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(firmware, "idf", app->idf_ver);
    cJSON_AddStringToObject(firmware, "chip", CONFIG_IDF_TARGET);
    return firmware;
}
