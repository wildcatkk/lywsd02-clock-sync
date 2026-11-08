#include "lywsd02.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6 as it appears on air.
static const uint8_t SERVICE_UUID[16] = {
    0xa6, 0xa3, 0x7d, 0x99, 0xf2, 0x6f, 0x1a, 0x8a,
    0x0c, 0x4b, 0x0a, 0x7a, 0xb0, 0xcc, 0xe0, 0xeb,
};
static const int KNOWN_PRODUCT_IDS[] = {0x045B, 0x16E4, 0x2542};

static long floor_div(long a, long b)
{
    long q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Days since 1970-01-01 of a proleptic Gregorian date.
static long days_from_civil(long y, long m, long d)
{
    y -= m <= 2;
    long era = floor_div(y, 400);
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int lywsd02_utc_offset(time_t moment)
{
    struct tm local;
    localtime_r(&moment, &local);
    long days = days_from_civil(local.tm_year + 1900L, local.tm_mon + 1L, local.tm_mday);
    long long wall = days * 86400LL + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return (int)(wall - (long long)moment);
}

void lywsd02_encode_time(double now, uint8_t out[5])
{
    long whole = (long)floor(now);
    long offset = lywsd02_utc_offset((time_t)whole);
    // Whole hours go in the timezone byte; the remainder is folded into the stamp.
    long hours = floor_div(offset, 3600);
    uint32_t stamp = (uint32_t)(whole + (offset - hours * 3600));
    out[0] = stamp;
    out[1] = stamp >> 8;
    out[2] = stamp >> 16;
    out[3] = stamp >> 24;
    out[4] = (uint8_t)(int8_t)hours;
}

double lywsd02_shown_time(uint32_t stamp, int timezone, double now)
{
    return (double)stamp + timezone * 3600.0 - lywsd02_utc_offset((time_t)floor(now));
}

bool lywsd02_decode_time(const uint8_t *data, size_t length, uint32_t *stamp, int *timezone)
{
    if (length < 5) {
        return false;
    }
    *stamp = data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24;
    *timezone = (int8_t)data[4];
    return true;
}

bool lywsd02_decode_env(const uint8_t *data, size_t length, double *temperature,
                        int *humidity, double *voltage)
{
    if (length < 3) {
        return false;
    }
    // A whole number over 100 is already the double nearest the 2-decimal value,
    // which is what Python's round(x, 2) gives.
    *temperature = (int16_t)(data[0] | data[1] << 8) / 100.0;
    *humidity = data[2];
    *voltage = length >= 5 ? (uint16_t)(data[3] | data[4] << 8) / 1000.0 : NAN;
    return true;
}

void lywsd02_adv_init(lywsd02_adv_t *adv)
{
    memset(adv, 0, sizeof(*adv));
    adv->product_id = -1;
}

size_t lywsd02_utf8_char(const char *p, size_t left)
{
    const unsigned char *s = (const unsigned char *)p;
    if (!left || !s[0]) {
        return 0;
    }
    if (s[0] < 0x80) {
        return 1;
    }
    size_t more;
    uint32_t code, least;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        more = 1, code = s[0] & 0x1F, least = 0x80;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        more = 2, code = s[0] & 0x0F, least = 0x800;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        more = 3, code = s[0] & 0x07, least = 0x10000;
    } else {
        return 0;
    }
    if (left <= more) {
        return 0;
    }
    for (size_t i = 1; i <= more; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        code = code << 6 | (s[i] & 0x3F);
    }
    if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return more + 1;
}

void lywsd02_parse_adv(const uint8_t *data, size_t length, lywsd02_adv_t *adv)
{
    size_t at = 0;
    while (at < length) {
        size_t size = data[at];
        if (size == 0 || at + 1 + size > length) {
            return;
        }
        uint8_t type = data[at + 1];
        const uint8_t *value = data + at + 2;
        size_t count = size - 1;
        if ((type == 0x09 || (type == 0x08 && !adv->complete_name)) && count > 0) {
            // The name goes out in JSON, so stop before a byte that is not part of
            // a whole character - including one our buffer would cut in half.
            size_t limit = count < sizeof(adv->name) - 1 ? count : sizeof(adv->name) - 1;
            size_t copy = 0;
            for (size_t step; copy < limit
                    && (step = lywsd02_utf8_char((const char *)value + copy, limit - copy));
                    copy += step) {
            }
            memcpy(adv->name, value, copy);
            adv->name[copy] = 0;
            adv->complete_name = type == 0x09;
        } else if (type == 0x06 || type == 0x07) {
            for (size_t i = 0; i + 16 <= count; i += 16) {
                adv->has_service = adv->has_service || memcmp(value + i, SERVICE_UUID, 16) == 0;
            }
        } else if (type == 0x16 && count >= 2 + 4 && value[0] == 0x95 && value[1] == 0xfe) {
            adv->product_id = value[4] | value[5] << 8;
        }
        at += 1 + size;
    }
}

bool lywsd02_looks_like_clock(const lywsd02_adv_t *adv)
{
    char upper[sizeof(adv->name)];
    size_t i = 0;
    for (; adv->name[i]; i++) {
        upper[i] = toupper((unsigned char)adv->name[i]);
    }
    upper[i] = 0;
    if (strstr(upper, "LYWSD02") || adv->has_service) {
        return true;
    }
    for (size_t k = 0; k < sizeof(KNOWN_PRODUCT_IDS) / sizeof(KNOWN_PRODUCT_IDS[0]); k++) {
        if (adv->product_id == KNOWN_PRODUCT_IDS[k]) {
            return true;
        }
    }
    return false;
}

void lywsd02_key(const uint8_t addr[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

void lywsd02_address(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

bool lywsd02_parse_key(const char *text, uint8_t addr[6])
{
    int digits = 0;
    uint8_t value[6] = {0};
    for (; *text; text++) {
        if (!isalnum((unsigned char)*text)) {
            continue;
        }
        if (!isxdigit((unsigned char)*text) || digits == 12) {
            return false;
        }
        int nibble = isdigit((unsigned char)*text) ? *text - '0'
                                                   : toupper((unsigned char)*text) - 'A' + 10;
        value[digits / 2] = value[digits / 2] << 4 | nibble;
        digits++;
    }
    if (digits != 12) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        addr[i] = value[5 - i];
    }
    return true;
}
