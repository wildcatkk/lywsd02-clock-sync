#include "console.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "selftest.h"
#include "store.h"
#include "wifi.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart_vfs.h"
#endif

// Improv Serial: "IMPROV", version 1, type, length, data, then the low byte of the
// sum of everything before it. https://www.improv-wifi.com/serial/

#define FIRMWARE "LYWSD02 Clock Sync"
#define JOIN_WAIT_MS (40 * 1000)
#define PACKET_IDLE_US (1000 * 1000)

enum { TYPE_STATE = 1, TYPE_ERROR = 2, TYPE_RPC = 3, TYPE_RESULT = 4 };
enum { CMD_WIFI = 1, CMD_STATE = 2, CMD_INFO = 3, CMD_NETWORKS = 4 };
enum { STATE_READY = 2, STATE_PROVISIONING = 3, STATE_PROVISIONED = 4 };
enum { ERR_INVALID = 1, ERR_UNKNOWN_CMD = 2, ERR_CONNECT = 3, ERR_OTHER = 0xFF };

static const char HEADER[] = "IMPROV";

static uint8_t packet[9 + 255 + 1];
static size_t got;
static int64_t last_byte;
static char line[64];
static size_t line_length;

typedef struct {
    uint8_t data[255];
    size_t length;
} result_t;

static void set_rx_raw(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
#elif CONFIG_ESP_CONSOLE_UART
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_LF);
#endif
}

// A packet byte that happens to be '\n' must not grow a '\r' on the way out.
static void set_tx_raw(bool raw)
{
    esp_line_endings_t mode = raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CRLF;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_tx_line_endings(mode);
#elif CONFIG_ESP_CONSOLE_UART
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, mode);
#else
    (void)mode;
#endif
}

static void send_packet(uint8_t type, const uint8_t *data, size_t length)
{
    uint8_t out[sizeof(packet) + 1];
    memcpy(out, HEADER, 6);
    out[6] = 1;
    out[7] = type;
    out[8] = (uint8_t)length;
    memcpy(out + 9, data, length);
    uint8_t sum = 0;
    for (size_t i = 0; i < 9 + length; i++) {
        sum += out[i];
    }
    out[9 + length] = sum;
    // The trailing newline makes the USB FIFO go out now instead of on the next line.
    out[10 + length] = '\n';
    fflush(stdout);
    set_tx_raw(true);
    write(STDOUT_FILENO, out, 11 + length);
    set_tx_raw(false);
}

static void send_byte(uint8_t type, uint8_t value)
{
    send_packet(type, &value, 1);
}

static void result_begin(result_t *result, uint8_t command)
{
    result->data[0] = command;
    result->data[1] = 0;
    result->length = 2;
}

static void result_add(result_t *result, const char *text)
{
    size_t length = strlen(text);
    if (result->length + 1 + length > sizeof(result->data)) {
        return;
    }
    result->data[result->length++] = (uint8_t)length;
    memcpy(result->data + result->length, text, length);
    result->length += length;
}

static void result_send(result_t *result)
{
    result->data[1] = (uint8_t)(result->length - 2);
    send_packet(TYPE_RESULT, result->data, result->length);
}

// The page address once the station has one, else an empty string.
static void station_url(char *out, size_t size)
{
    cJSON *status = wifi_status_json();
    const char *ip = json_str(status, "ip", NULL);
    if (ip && strcmp(json_str(status, "state", ""), "connected") == 0) {
        snprintf(out, size, "http://%s:8787/", ip);
    } else {
        out[0] = 0;
    }
    cJSON_Delete(status);
}

static void send_provisioned(uint8_t command, const char *url)
{
    send_byte(TYPE_STATE, STATE_PROVISIONED);
    result_t result;
    result_begin(&result, command);
    result_add(&result, url);
    result_send(&result);
}

static void rpc_state(void)
{
    char url[32];
    station_url(url, sizeof(url));
    if (url[0]) {
        send_provisioned(CMD_STATE, url);
    } else {
        send_byte(TYPE_STATE, STATE_READY);
    }
}

static void rpc_info(void)
{
    char name[97];
    store_node_name(name, sizeof(name));
    result_t result;
    result_begin(&result, CMD_INFO);
    result_add(&result, FIRMWARE);
    result_add(&result, esp_app_get_description()->version);
    result_add(&result, store_chip());
    result_add(&result, name);
    result_send(&result);
}

static void rpc_networks(void)
{
    cJSON *networks = wifi_scan_json();
    const cJSON *network;
    cJSON_ArrayForEach(network, networks) {
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", (int)json_num(network, "rssi", 0));
        result_t result;
        result_begin(&result, CMD_NETWORKS);
        result_add(&result, json_str(network, "ssid", ""));
        result_add(&result, rssi);
        result_add(&result, json_bool(network, "secure", false) ? "YES" : "NO");
        result_send(&result);
    }
    cJSON_Delete(networks);
    result_t end;
    result_begin(&end, CMD_NETWORKS);
    result_send(&end);
}

static void rpc_wifi(const uint8_t *payload, size_t length)
{
    char ssid[33], password[65];
    size_t ssid_len = length > 0 ? payload[0] : 0;
    size_t pass_at = 1 + ssid_len;
    size_t pass_len = pass_at < length ? payload[pass_at] : 0;
    if (length < 2 || pass_at >= length || pass_at + 1 + pass_len != length
        || ssid_len >= sizeof(ssid) || pass_len >= sizeof(password)) {
        send_byte(TYPE_ERROR, ERR_INVALID);
        return;
    }
    memcpy(ssid, payload + 1, ssid_len);
    ssid[ssid_len] = 0;
    memcpy(password, payload + pass_at + 1, pass_len);
    password[pass_len] = 0;

    esp_err_t err = wifi_join(ssid, password, NULL);
    memset(password, 0, sizeof(password));
    memset(packet, 0, sizeof(packet));
    if (err != ESP_OK) {
        send_byte(TYPE_ERROR, err == ESP_ERR_INVALID_ARG ? ERR_CONNECT : ERR_OTHER);
        send_byte(TYPE_STATE, STATE_READY);
        return;
    }
    send_byte(TYPE_STATE, STATE_PROVISIONING);

    // Same rule as the page: a failure that came with a reason is final.
    for (int waited = 0; waited < JOIN_WAIT_MS; waited += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
        cJSON *status = wifi_status_json();
        const char *state = json_str(status, "state", "");
        bool up = strcmp(state, "connected") == 0
                  && strcmp(json_str(status, "ssid", ""), ssid) == 0;
        bool failed = strcmp(state, "failed") == 0 && cJSON_IsString(cJSON_GetObjectItem(status, "reason"));
        cJSON_Delete(status);
        if (up) {
            char url[32];
            station_url(url, sizeof(url));
            send_provisioned(CMD_WIFI, url);
            return;
        }
        if (failed) {
            break;
        }
    }
    send_byte(TYPE_ERROR, ERR_CONNECT);
    send_byte(TYPE_STATE, STATE_READY);
}

static void handle_rpc(const uint8_t *data, size_t length)
{
    if (length < 2 || data[1] != length - 2) {
        send_byte(TYPE_ERROR, ERR_INVALID);
        return;
    }
    switch (data[0]) {
    case CMD_WIFI:
        rpc_wifi(data + 2, data[1]);
        break;
    case CMD_STATE:
        rpc_state();
        break;
    case CMD_INFO:
        rpc_info();
        break;
    case CMD_NETWORKS:
        rpc_networks();
        break;
    default:
        send_byte(TYPE_ERROR, ERR_UNKNOWN_CMD);
    }
}

static void print_info(void)
{
    cJSON *status = wifi_status_json();
    const char *state = json_str(status, "state", "idle");
    const char *ip = json_str(status, "ip", NULL);
    const char *ap = json_str(status, "ap", NULL);
    const char *reason = json_str(status, "reason", NULL);
    const char *host = store_hostname();
    char name[97];
    store_node_name(name, sizeof(name));

    printf("firmware  %s %s (%s)\n", FIRMWARE, esp_app_get_description()->version, store_chip());
    printf("hostname  %s\n", host);
    printf("node      %s (%s)\n", name, store_node_id());
    if (strcmp(state, "connected") == 0 && ip) {
        printf("wifi      connected, signal %d dBm\n", (int)json_num(status, "rssi", 0));
        printf("page      http://%s:8787/  http://%s.local:8787/\n", ip, host);
    } else if (strcmp(state, "idle") == 0) {
        printf("wifi      no network saved\n");
    } else {
        printf("wifi      %s%s%s\n", state, reason ? ", " : "", reason ? reason : "");
    }
    if (ap) {
        uint32_t address = wifi_ap_ip();
        char text[16];
        esp_ip4addr_ntoa((const esp_ip4_addr_t *)&address, text, sizeof(text));
        printf("hotspot   %s, page http://%s/\n", ap, text);
    } else {
        printf("hotspot   off\n");
    }
    if (wifi_time_synced()) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char text[32];
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
        printf("time      %s\n", text);
    } else {
        printf("time      not synced yet\n");
    }
    printf("uptime    %lld s, free heap %u B\n", (long long)(esp_timer_get_time() / 1000000),
           (unsigned)esp_get_free_heap_size());
    cJSON_Delete(status);
}

static void run_command(const char *text)
{
    if (strcmp(text, "info") == 0) {
        print_info();
    } else if (strcmp(text, "reboot") == 0) {
        printf("rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!selftest_command(text)) {
        printf("commands: info, reboot, selftest, heap, blecycle N, ntp [server]\n");
    }
}

static void text_byte(uint8_t c)
{
    if (c == '\n' || c == '\r') {
        line[line_length] = 0;
        line_length = 0;
        if (line[0]) {
            run_command(line);
        }
    } else if (line_length < sizeof(line) - 1) {
        line[line_length++] = (char)c;
    }
}

// Bytes that turn out not to start a packet belong to a typed line.
static void give_back(void)
{
    if (got <= 6) {
        for (size_t i = 0; i < got; i++) {
            text_byte(packet[i]);
        }
    }
    got = 0;
}

static void feed(uint8_t c)
{
    if (got < 6) {
        if (c == (uint8_t)HEADER[got]) {
            packet[got++] = c;
            return;
        }
        give_back();
        if (c == (uint8_t)HEADER[0]) {
            packet[got++] = c;
        } else {
            text_byte(c);
        }
        return;
    }
    packet[got++] = c;
    if (got == 7 && c != 1) {
        got = 0;
        return;
    }
    if (got < 9 || got < 9 + (size_t)packet[8] + 1) {
        return;
    }
    size_t length = packet[8];
    got = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < 9 + length; i++) {
        sum += packet[i];
    }
    if (sum != packet[9 + length]) {
        send_byte(TYPE_ERROR, ERR_INVALID);
    } else if (packet[7] == TYPE_RPC) {
        handle_rpc(packet + 9, length);
    }
}

static void console(void *arg)
{
    set_rx_raw();
    uint8_t chunk[64];
    for (;;) {
        ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        int64_t now = esp_timer_get_time();
        if (count <= 0) {
            if (got && now - last_byte > PACKET_IDLE_US) {
                give_back();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_byte = now;
        for (ssize_t i = 0; i < count; i++) {
            feed(chunk[i]);
        }
    }
}

void console_start(void)
{
    xTaskCreate(console, "console", 5120, NULL, 1, NULL);
}
