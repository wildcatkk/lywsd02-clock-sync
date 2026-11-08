/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "dns_server.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)
#define OPCODE_MASK (0x7800)
#define QR_FLAG (0x8000)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

static const char *TAG = "example_dns_redirect_server";

// DNS Header Packet
typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

// DNS Question Packet
typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

// DNS Answer Packet
typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

// The handle owns the socket until the worker has acknowledged its exit.
struct dns_server_handle {
    TaskHandle_t task;
    SemaphoreHandle_t done;
    atomic_bool stopping;
    int sock;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

static const uint8_t *parse_dns_name(const uint8_t *raw, const uint8_t *end,
                                     char *name, size_t capacity)
{
    size_t used = 0;
    const uint8_t *start = raw;
    while (raw < end) {
        unsigned length = *raw++;
        if (length == 0) {
            if ((size_t)(raw - start) > 255) {
                return NULL;
            }
            name[used] = 0;
            return raw;
        }
        // Compression pointers and reserved label encodings are unsupported.
        if (length > 63 || (size_t)(end - raw) < length
            || (size_t)(raw - start) + length > 254) {
            return NULL;
        }
        size_t separator = used != 0;
        if (used + separator + length >= capacity) {
            return NULL;
        }
        if (separator) {
            name[used++] = '.';
        }
        memcpy(name + used, raw, length);
        used += length;
        raw += length;
    }
    return NULL;
}

// Only one uncompressed question is supported; optional additional records are
// omitted from the response so the answer always follows the question.
static int parse_dns_request(const char *req, size_t req_len, char *reply,
                             size_t capacity, dns_server_handle_t handle)
{
    if (req_len < sizeof(dns_header_t) || req_len > DNS_MAX_LEN || req_len > capacity) {
        return -1;
    }
    dns_header_t header;
    memcpy(&header, req, sizeof(header));
    uint16_t flags = ntohs(header.flags);
    if ((flags & (OPCODE_MASK | QR_FLAG)) || ntohs(header.qd_count) != 1
        || header.an_count || header.ns_count) {
        return -1;
    }

    const uint8_t *end = (const uint8_t *)req + req_len;
    char name[254];
    const uint8_t *name_end = parse_dns_name((const uint8_t *)req + sizeof(header),
                                            end, name, sizeof(name));
    if (!name_end || (size_t)(end - name_end) < sizeof(dns_question_t)) {
        return -1;
    }
    dns_question_t question;
    memcpy(&question, name_end, sizeof(question));
    size_t length = (size_t)(name_end - (const uint8_t *)req) + sizeof(question);
    memcpy(reply, req, length);
    dns_header_t *response = (dns_header_t *)reply;
    response->flags = htons(flags | QR_FLAG);
    response->an_count = response->ns_count = response->ar_count = 0;

    esp_ip4_addr_t ip = {.addr = IPADDR_ANY};
    if (ntohs(question.type) == QD_TYPE_A && ntohs(question.class) == 1) {
        for (int i = 0; i < handle->num_of_entries; i++) {
            const dns_entry_pair_t *entry = &handle->entry[i];
            if (strcmp(entry->name, "*") && strcmp(entry->name, name)) {
                continue;
            }
            if (entry->if_key) {
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey(entry->if_key);
                esp_netif_ip_info_t info;
                if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
                    ip = info.ip;
                }
            } else {
                ip = entry->ip;
            }
            if (ip.addr != IPADDR_ANY) {
                break;
            }
        }
    }
    if (ip.addr != IPADDR_ANY) {
        if (capacity - length < sizeof(dns_answer_t)) {
            return -1;
        }
        dns_answer_t answer = {
            .ptr_offset = htons(0xC000 | sizeof(header)),
            .type = question.type,
            .class = question.class,
            .ttl = htonl(ANS_TTL_SEC),
            .addr_len = htons(sizeof(ip.addr)),
            .ip_addr = ip.addr,
        };
        memcpy(reply + length, &answer, sizeof(answer));
        length += sizeof(answer);
        response->an_count = htons(1);
    }
    return (int)length;
}

static void dns_server_task(void *arg)
{
    dns_server_handle_t handle = arg;
    // The extra byte lets us reject oversized datagrams rather than parse a
    // silently truncated packet as a complete request.
    char request[DNS_MAX_LEN + 1];
    char reply[DNS_MAX_LEN];
    while (!atomic_load(&handle->stopping)) {
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        int length = recvfrom(handle->sock, request, sizeof(request), 0,
                              (struct sockaddr *)&source, &source_len);
        if (atomic_load(&handle->stopping)) {
            break;
        }
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }
        int reply_len = parse_dns_request(request, (size_t)length, reply, sizeof(reply), handle);
        if (reply_len > 0 && sendto(handle->sock, reply, reply_len, 0,
                                    (struct sockaddr *)&source, source_len) < 0) {
            ESP_LOGW(TAG, "sendto failed: errno %d", errno);
        }
    }
    // Semaphore give has no object access after waking the waiter (unlike
    // xEventGroupSetBits), so the caller may safely free it on completion.
    xSemaphoreGive(handle->done);
    vTaskDelete(NULL);
}

dns_server_handle_t start_dns_server(dns_server_config_t *config)
{
    if (!config || config->num_of_entries <= 0
        || (size_t)config->num_of_entries > (SIZE_MAX - sizeof(struct dns_server_handle))
                                           / sizeof(dns_entry_pair_t)) {
        return NULL;
    }
    for (int i = 0; i < config->num_of_entries; i++) {
        if (!config->item[i].name) {
            return NULL;
        }
    }
    dns_server_handle_t handle = calloc(1, sizeof(*handle)
                                        + config->num_of_entries * sizeof(dns_entry_pair_t));
    if (!handle) {
        return NULL;
    }
    handle->sock = -1;
    atomic_init(&handle->stopping, false);
    handle->done = xSemaphoreCreateBinary();
    if (!handle->done) {
        goto fail;
    }
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, config->num_of_entries * sizeof(dns_entry_pair_t));
    handle->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle->sock < 0) {
        goto fail;
    }
    // shutdown() wakes recvfrom; this timeout is a fallback if shutdown fails.
    const struct timeval wait = {.tv_usec = 250000};
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(handle->sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) != 0
        || setsockopt(handle->sock, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait)) != 0
        || bind(handle->sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
        goto fail;
    }
    if (xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task) != pdPASS) {
        goto fail;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", DNS_PORT);
    return handle;

fail:
    ESP_LOGE(TAG, "Failed to start DNS server");
    if (handle->sock >= 0) {
        close(handle->sock);
    }
    if (handle->done) {
        vSemaphoreDelete(handle->done);
    }
    free(handle);
    return NULL;
}

void stop_dns_server(dns_server_handle_t handle)
{
    if (!handle) {
        return;
    }
    atomic_store(&handle->stopping, true);
    shutdown(handle->sock, SHUT_RD);
    xSemaphoreTake(handle->done, portMAX_DELAY);
    close(handle->sock);
    vSemaphoreDelete(handle->done);
    free(handle);
}
