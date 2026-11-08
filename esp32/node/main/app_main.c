#include <inttypes.h>

#include "cluster.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "jobs.h"
#include "logs.h"
#include "ota.h"
#include "store.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "app";

// Long enough for the hotspot fallback to kick in when the stored network is gone,
// which also counts as up: the page can be reached and a fix uploaded.
#define UP_TIMEOUT_S 240

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "%s %s (IDF %s)", app->project_name, app->version, app->idf_ver);
    ESP_LOGI(TAG, "chip %s rev v%d.%d, running from %s at 0x%" PRIx32,
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             running->label, running->address);

    logs_init();
    store_init();
    store_lock();
    wifi_apply_timezone(json_str(store_config(), "timezone", ""));
    store_unlock();

    wifi_start();
    jobs_start();
    cluster_start();
    bool healthy = web_start() == ESP_OK;
    console_start();
    healthy = wifi_wait_up(UP_TIMEOUT_S) && healthy;
    ota_settle(healthy);
    ESP_LOGI(TAG, "up, free heap %" PRIu32 " B, lowest %" PRIu32 " B, reset reason %d",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size(), esp_reset_reason());
}
