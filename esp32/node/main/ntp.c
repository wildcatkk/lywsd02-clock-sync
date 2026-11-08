#include "ntp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "store.h"

#define NTP_EPOCH 2208988800UL  // 1900-01-01 to 1970-01-01

static double stamp(const uint8_t *data)
{
    uint32_t seconds = (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    uint32_t fraction = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
    return (double)seconds - NTP_EPOCH + fraction / 4294967296.0;
}

bool ntp_query(const char *host, double *offset, char *error, size_t size)
{
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *found = NULL;
    if (getaddrinfo(host, "123", &hints, &found) != 0 || !found) {
        snprintf(error, size, "gaierror('%s')", host);
        return false;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        freeaddrinfo(found);
        snprintf(error, size, "OSError('socket')");
        return false;
    }
    struct timeval wait = {.tv_sec = 3};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) != 0
        || setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait)) != 0) {
        freeaddrinfo(found);
        close(sock);
        snprintf(error, size, "OSError('setsockopt')");
        return false;
    }
    int linked = connect(sock, found->ai_addr, found->ai_addrlen);
    freeaddrinfo(found);

    // Version 4, client mode. A random transmit stamp comes back as the originate
    // stamp, which ties the answer to this request.
    uint8_t request[48] = {0x23};
    esp_fill_random(request + 40, 8);
    uint8_t reply[68];
    double sent = store_now(), received = 0;
    bool ok = false;
    if (linked == 0 && send(sock, request, sizeof(request), 0) == sizeof(request)) {
        int64_t deadline = esp_timer_get_time() + 3000000;
        snprintf(error, size, "TimeoutError('%s')", host);
        for (int invalid = 0; invalid < 16; invalid++) {
            int64_t left = deadline - esp_timer_get_time();
            // lwIP rounds SO_RCVTIMEO down to milliseconds; zero means forever.
            if (left < 1000) {
                break;
            }
            wait.tv_sec = left / 1000000;
            wait.tv_usec = left % 1000000;
            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) != 0) {
                snprintf(error, size, "OSError('setsockopt')");
                break;
            }
            int length = recv(sock, reply, sizeof(reply), 0);
            received = store_now();
            if (length < 0 || esp_timer_get_time() >= deadline) {
                break;
            }
            if (length >= 48 && memcmp(reply + 24, request + 40, 8) == 0) {
                ok = true;
                break;
            }
        }
    } else {
        snprintf(error, size, "OSError('send')");
    }
    close(sock);
    if (!ok) {
        return false;
    }
    int leap = reply[0] >> 6, mode = reply[0] & 7, stratum = reply[1];
    if (mode != 4 || stratum < 1 || stratum > 15 || leap == 3) {
        snprintf(error, size, "OSError('unsynchronised reply (stratum %d)')", stratum);
        return false;
    }
    *offset = ((stamp(reply + 32) - sent) + (stamp(reply + 40) - received)) / 2;
    return true;
}
