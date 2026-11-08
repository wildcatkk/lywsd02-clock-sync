#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_start(void);
void web_portal_update(void);

// Both take ownership of the JSON they are given.
esp_err_t web_send_json(httpd_req_t *req, const char *status, cJSON *body);
// {"detail": {"event", "params"}}, the shape of detail() in server.py, so the page
// translates it the same way whichever node refused.
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *event,
                         cJSON *params);
