#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_server.h"

// POST /api/ota: the raw application image as the request body.
esp_err_t ota_upload(httpd_req_t *req);
// Runs once at boot: reports an image the bootloader had to roll back from, and
// settles the running one. `healthy` is whether the network and the page came up.
void ota_settle(bool healthy);
// {version, partition, idf}
cJSON *ota_firmware_json(void);
