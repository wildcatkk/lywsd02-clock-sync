#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

void wifi_start(void);
// Blocks until the page is reachable: an STA address, or the hotspot running.
// False when that did not happen within the timeout.
bool wifi_wait_up(int seconds);

bool wifi_connected(void);
// WiFi and BLE share one antenna, which needs modem sleep; otherwise stay awake.
void wifi_share_radio(bool shared);
// SNTP has answered since boot. No clock may be written before this.
bool wifi_time_synced(void);
// Network byte order; 0 while the hotspot is off.
uint32_t wifi_ap_ip(void);

// {ssid, state: idle|connecting|connected|failed, reason, ip, rssi, hostname,
//  default_hostname, ap}
cJSON *wifi_status_json(void);
// [{ssid, rssi, secure}], strongest first; NULL when the radio refused to scan.
cJSON *wifi_scan_json(void);
// A NULL hostname leaves it alone, an empty one restores the default.
// ESP_ERR_INVALID_ARG when any field is bad.
esp_err_t wifi_join(const char *ssid, const char *password, const char *hostname);

void wifi_apply_timezone(const char *posix);
