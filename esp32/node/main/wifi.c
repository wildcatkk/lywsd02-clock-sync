#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/_tz_structs.h>
#include "esp_netif.h"
#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logs.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "store.h"
#include "web.h"

// Everything that touches the radio's connection state runs in one task; the event
// handlers only record what happened and wake it. That keeps the retry, hotspot and
// linger rules in one place instead of spread over callbacks that race each other.

#define ATTEMPT_MS (20 * 1000)
#define PORTAL_AFTER_MS (180 * 1000)
#define PORTAL_LINGER_MS (60 * 1000)
#define UP_BIT BIT0

typedef enum { ST_IDLE, ST_CONNECTING, ST_CONNECTED, ST_FAILED } sta_state_t;

static const char *TAG = "net";
static const int BACKOFF_S[] = {0, 2, 4, 8, 16, 30};

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *sta_netif, *ap_netif;
static TaskHandle_t task;
static EventGroupHandle_t bits;
static SemaphoreHandle_t scan_mutex;
static dns_server_handle_t dns;

// Guarded by `lock`.
static char ssid[33];
static sta_state_t state = ST_IDLE;
static int reason;            // last disconnect reason of a failed attempt
static uint32_t sta_ip;
static bool ap_on;
static bool join_wanted;
static bool scanning;
static bool got_ip, lost_ip;  // edges for the task to act on
static bool synced, sync_logged;
static char ap_ssid[33];

// Only the task touches these.
static int64_t lost_since, connected_since, attempt_started, next_try, sntp_next_try;
static int failures;
static bool was_connected, sntp_started, retry_planned;
static char portal_uri[32];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void wake(void)
{
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        // Our own disconnect ahead of a new join says "leaving". The join has already
        // moved the state on to connecting, and that attempt must not be failed by it.
        bool leaving = event->reason == WIFI_REASON_ASSOC_LEAVE;
        if (!leaving) {
            ESP_LOGI(TAG, "network dropped or refused us, reason %d", event->reason);
        }
        taskENTER_CRITICAL(&lock);
        if (!leaving) {
            reason = event->reason;
        }
        if (state == ST_CONNECTED || (state == ST_CONNECTING && !leaving)) {
            state = ST_FAILED;
        }
        lost_ip = lost_ip || sta_ip != 0;
        sta_ip = 0;
        taskEXIT_CRITICAL(&lock);
        wake();
    } else if (id == WIFI_EVENT_STA_START) {
        wake();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        taskENTER_CRITICAL(&lock);
        sta_ip = event->ip_info.ip.addr;
        state = ST_CONNECTED;
        reason = 0;
        got_ip = true;
        taskEXIT_CRITICAL(&lock);
        xEventGroupSetBits(bits, UP_BIT);
        wake();
    }
}

static void on_sntp(struct timeval *tv)
{
    taskENTER_CRITICAL(&lock);
    synced = true;
    taskEXIT_CRITICAL(&lock);
    wake();
}

static bool start_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "ntp.aliyun.com", "ntp.tencent.com"));
    config.wait_for_sync = false;
    config.sync_cb = on_sntp;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void portal_on(void)
{
    taskENTER_CRITICAL(&lock);
    strcpy(ap_ssid, store_hostname());
    taskEXIT_CRITICAL(&lock);
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, ap_ssid, strlen(ap_ssid));
    config.ap.ssid_len = strlen(ap_ssid);
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.max_connection = 4;
    config.ap.channel = 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &config);

    // Option 114 tells a phone where the portal is, so it opens the page by itself
    // instead of waiting for its connectivity check to be caught by the DNS server.
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(ap_netif, &info);
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&info.ip));
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           portal_uri, strlen(portal_uri));
    esp_netif_dhcps_start(ap_netif);

    dns_server_config_t answer = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns = start_dns_server(&answer);

    taskENTER_CRITICAL(&lock);
    ap_on = true;
    taskEXIT_CRITICAL(&lock);
    xEventGroupSetBits(bits, UP_BIT);
    logs_put("wifi.portal", "ssid", cJSON_CreateString(ap_ssid), NULL);
}

static void portal_off(void)
{
    if (dns) {
        stop_dns_server(dns);
        dns = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    taskENTER_CRITICAL(&lock);
    ap_on = false;
    taskEXIT_CRITICAL(&lock);
    logs_put("wifi.portalClosed", NULL);
}

static void attempt(int64_t now)
{
    taskENTER_CRITICAL(&lock);
    state = ST_CONNECTING;
    taskEXIT_CRITICAL(&lock);
    attempt_started = now;
    retry_planned = false;
    if (esp_wifi_connect() != ESP_OK) {
        // Usually a scan from the page holds the radio; try again on the next tick.
        taskENTER_CRITICAL(&lock);
        state = ST_FAILED;
        taskEXIT_CRITICAL(&lock);
        next_try = now + 1000;
        retry_planned = true;
    }
}

static void manager(void *arg)
{
    lost_since = now_ms();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        web_portal_update();
        int64_t now = now_ms();

        taskENTER_CRITICAL(&lock);
        bool have_ssid = ssid[0] != 0;
        sta_state_t current = state;
        bool joining = join_wanted, busy = scanning;
        bool rose = got_ip, fell = lost_ip, sync_edge = synced && !sync_logged;
        bool portal = ap_on;
        uint32_t ip = sta_ip;
        char name[33];
        strcpy(name, ssid);
        // A join asked for during a scan waits for the scan to finish.
        join_wanted = joining && busy;
        got_ip = lost_ip = false;
        sync_logged = sync_logged || sync_edge;
        taskEXIT_CRITICAL(&lock);

        if (rose) {
            connected_since = now;
            failures = 0;
            was_connected = true;
            char text[16];
            esp_ip4addr_ntoa((const esp_ip4_addr_t *)&ip, text, sizeof(text));
            logs_put("wifi.connected", "ssid", cJSON_CreateString(name),
                     "ip", cJSON_CreateString(text), NULL);
        }
        if (current == ST_CONNECTED && !sntp_started && now >= sntp_next_try) {
            sntp_started = start_sntp();
            sntp_next_try = now + 5000;
        }
        if (fell || (current == ST_FAILED && was_connected)) {
            if (was_connected) {
                logs_put("wifi.lost", "ssid", cJSON_CreateString(name), NULL);
            }
            was_connected = false;
            lost_since = now;
        }
        if (sync_edge) {
            time_t clock = time(NULL);
            struct tm local;
            localtime_r(&clock, &local);
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
            logs_put("time.synced", "time", cJSON_CreateString(text), NULL);
        }

        if (joining && !busy) {
            failures = 0;
            esp_wifi_disconnect();
            attempt(now);
            continue;
        }

        if (have_ssid && !busy && !joining) {
            if (current == ST_CONNECTING && now - attempt_started > ATTEMPT_MS) {
                esp_wifi_disconnect();
                taskENTER_CRITICAL(&lock);
                state = current = ST_FAILED;
                // No reason came back, but the page still has to hear it failed.
                reason = reason ? reason : -1;
                taskEXIT_CRITICAL(&lock);
            }
            if (current == ST_FAILED && !retry_planned) {
                int step = failures < 5 ? failures : 5;
                failures++;
                next_try = now + BACKOFF_S[step] * 1000;
                retry_planned = true;
            }
            if ((current == ST_FAILED || current == ST_IDLE) && now >= next_try) {
                attempt(now);
            }
        }

        if (!portal && (!have_ssid || (current != ST_CONNECTED
                                       && now - lost_since > PORTAL_AFTER_MS))) {
            portal_on();
        } else if (portal && current == ST_CONNECTED
                   && now - connected_since > PORTAL_LINGER_MS) {
            portal_off();
        }
    }
}

void wifi_start(void)
{
    bits = xEventGroupCreate();
    scan_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(bits && scan_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_netif, store_hostname());

    const char *host = store_hostname();

    // The driver names the network at info level. Serial logs get pasted into
    // bug reports, so keep it quiet; our own lines under "net" leave the name out.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));

    // The driver keeps the station credentials in its own NVS namespace.
    wifi_config_t stored = {0};
    esp_wifi_get_config(WIFI_IF_STA, &stored);
    memcpy(ssid, stored.sta.ssid, sizeof(stored.sta.ssid));
    ssid[32] = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep holds every incoming packet until the next DTIM beacon, which on
    // a router with DTIM 3 adds hundreds of ms per round trip and makes the other
    // nodes' 1.5 s probes time out. Only a BLE job needs it, to share the radio.
    wifi_share_radio(false);
    // Cheap C3 boards reset or drop packets at full power: weak LDO, poor antenna match.
    esp_wifi_set_max_tx_power(34);

    char name[97];
    store_node_name(name, sizeof(name));
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set(name);
        mdns_service_add(NULL, "_http", "_tcp", 8787, NULL, 0);
    }
    ESP_LOGI(TAG, "hostname %s, %s", host, ssid[0] ? "network saved" : "no network saved");
    ESP_ERROR_CHECK(xTaskCreate(manager, "wifi", 4096, NULL, 5, &task) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}

void wifi_share_radio(bool shared)
{
    esp_err_t err = esp_wifi_set_ps(shared ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power save %d: %s", shared, esp_err_to_name(err));
    }
}

bool wifi_wait_up(int seconds)
{
    return xEventGroupWaitBits(bits, UP_BIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(seconds * 1000)) & UP_BIT;
}

bool wifi_connected(void)
{
    taskENTER_CRITICAL(&lock);
    bool up = state == ST_CONNECTED;
    taskEXIT_CRITICAL(&lock);
    return up;
}

bool wifi_time_synced(void)
{
    taskENTER_CRITICAL(&lock);
    bool done = synced;
    taskEXIT_CRITICAL(&lock);
    return done;
}

uint32_t wifi_ap_ip(void)
{
    taskENTER_CRITICAL(&lock);
    bool on = ap_on;
    taskEXIT_CRITICAL(&lock);
    if (!on) {
        return 0;
    }
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(ap_netif, &info);
    return info.ip.addr;
}

static const char *reason_key(int code)
{
    switch (code) {
    case 0:
        return NULL;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrongPassword";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "notFound";
    default:
        return "failed";
    }
}

cJSON *wifi_status_json(void)
{
    static const char *NAMES[] = {"idle", "connecting", "connected", "failed"};
    taskENTER_CRITICAL(&lock);
    char name[33];
    strcpy(name, ssid);
    sta_state_t current = state;
    int code = reason;
    uint32_t ip = sta_ip;
    char portal[33];
    strcpy(portal, ap_on ? ap_ssid : "");
    taskEXIT_CRITICAL(&lock);

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "ssid", name);
    cJSON_AddStringToObject(status, "state", name[0] ? NAMES[current] : "idle");
    const char *why = current == ST_CONNECTED ? NULL : reason_key(code);
    cJSON_AddItemToObject(status, "reason", why ? cJSON_CreateString(why) : cJSON_CreateNull());
    if (ip) {
        char text[16];
        esp_ip4addr_ntoa((const esp_ip4_addr_t *)&ip, text, sizeof(text));
        cJSON_AddStringToObject(status, "ip", text);
    } else {
        cJSON_AddNullToObject(status, "ip");
    }
    wifi_ap_record_t info;
    if (current == ST_CONNECTED && esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        cJSON_AddNumberToObject(status, "rssi", info.rssi);
    } else {
        cJSON_AddNullToObject(status, "rssi");
    }
    cJSON_AddStringToObject(status, "hostname", store_hostname());
    cJSON_AddStringToObject(status, "default_hostname", store_default_hostname());
    cJSON_AddItemToObject(status, "ap", portal[0] ? cJSON_CreateString(portal) : cJSON_CreateNull());
    return status;
}

static int by_signal(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

cJSON *wifi_scan_json(void)
{
    // HTTP and Improv share the driver-owned scan result list. Hold this mutex
    // through waiting, scanning, fetching/clearing results and building JSON.
    if (!scan_mutex || xSemaphoreTake(scan_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    // A scan cannot run while the station is in the middle of joining, so wait for
    // the attempt to end and keep the next one from starting until we are done.
    for (int waited = 0;; waited += 100) {
        taskENTER_CRITICAL(&lock);
        bool free_radio = state != ST_CONNECTING || waited >= 5000;
        if (free_radio) {
            scanning = true;
        }
        taskEXIT_CRITICAL(&lock);
        if (free_radio) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    uint16_t count = 20;
    wifi_ap_record_t *records = NULL;
    if (err == ESP_OK) {
        records = calloc(count, sizeof(*records));
        err = records ? esp_wifi_scan_get_ap_records(&count, records) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        esp_err_t clear_err = esp_wifi_clear_ap_list();
        if (clear_err != ESP_OK) {
            ESP_LOGW(TAG, "clearing scan results failed: %s", esp_err_to_name(clear_err));
        }
    }
    taskENTER_CRITICAL(&lock);
    scanning = false;
    taskEXIT_CRITICAL(&lock);
    wake();

    if (err != ESP_OK) {
        free(records);
        xSemaphoreGive(scan_mutex);
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return NULL;
    }
    qsort(records, count, sizeof(*records), by_signal);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; list && i < count; i++) {
        const char *name = (const char *)records[i].ssid;
        if (!name[0]) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(name, (const char *)records[j].ssid) == 0;
        }
        if (seen) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "ssid", name)
            || !cJSON_AddNumberToObject(item, "rssi", records[i].rssi)
            || !cJSON_AddBoolToObject(item, "secure", records[i].authmode != WIFI_AUTH_OPEN)
            || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            cJSON_Delete(list);
            list = NULL;
            break;
        }
    }
    free(records);
    xSemaphoreGive(scan_mutex);
    return list;
}

esp_err_t wifi_join(const char *name, const char *password, const char *hostname)
{
    size_t name_len = strlen(name), pass_len = strlen(password);
    if (name_len == 0 || name_len > 32 || (pass_len > 0 && pass_len < 8) || pass_len > 64
        || (hostname && hostname[0] && !store_hostname_valid(hostname))) {
        return ESP_ERR_INVALID_ARG;
    }
    // Set before the join so the DHCP request of the new attempt carries it.
    if (hostname && strcmp(hostname[0] ? hostname : store_default_hostname(), store_hostname())) {
        store_set_hostname(hostname);
        esp_netif_set_hostname(sta_netif, store_hostname());
        mdns_hostname_set(store_hostname());
        logs_put("wifi.hostname", "name", cJSON_CreateString(store_hostname()), NULL);
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, name, name_len);
    memcpy(config.sta.password, password, pass_len);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return err;
    }
    taskENTER_CRITICAL(&lock);
    memcpy(ssid, name, name_len);
    ssid[name_len] = 0;
    reason = 0;
    state = ST_CONNECTING;
    join_wanted = true;
    taskEXIT_CRITICAL(&lock);
    wake();
    return ESP_OK;
}

void __real__tzset_unlocked(void);

// picolibc reads a switch time like the "/-1" in Greenland's
// "<-02>2<-01>,M3.5.0/-1,M10.5.0/0" as an unsigned number. It parses TZ again
// inside every localtime() and mktime(), so the signed times are put back right
// after that parse (CMakeLists.txt wraps _tzset_unlocked).
void __wrap__tzset_unlocked(void)
{
    __real__tzset_unlocked();
    const char *posix = getenv("TZ");
    if (!posix || !strstr(posix, "/-")) {
        return;
    }
    __tzinfo_type *tz = __gettzinfo();
    const char *at = strchr(posix, ',');
    for (int i = 0; i < 2 && at; i++) {
        const char *next = strchr(at + 1, ',');
        const char *slash = strchr(at, '/');
        if (slash && (!next || slash < next)) {
            char *p = (char *)slash + 1;
            int sign = *p == '-' ? -1 : 1;
            if (*p == '-' || *p == '+') {
                p++;
            }
            int parts[3] = {0, 0, 0};
            for (int k = 0; k < 3; k++) {
                parts[k] = (int)strtol(p, &p, 10);
                if (*p != ':') {
                    break;
                }
                p++;
            }
            tz->__tzrule[i].s = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
        }
        at = next;
    }
    // The switch moments were worked out from the wrong times; this makes the
    // caller work them out again.
    tz->__tzyear = 0;
}

void wifi_apply_timezone(const char *posix)
{
    setenv("TZ", posix && posix[0] ? posix : "UTC0", 1);
    tzset();
}
