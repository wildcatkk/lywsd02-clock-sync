#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// The radio-free half of lywsd02.py: time encoding, sensor decoding and telling a
// clock apart from everything else that advertises.

#define LYWSD02_SYNC_TOLERANCE 5

// Seconds east of UTC in the zone set with TZ, at `moment`.
int lywsd02_utc_offset(time_t moment);

// encode_time(): the 5 bytes written to ebe0ccb7.
void lywsd02_encode_time(double now, uint8_t out[5]);
// shown_time(): the instant the face displays, on the host's clock.
double lywsd02_shown_time(uint32_t stamp, int timezone, double now);
// decode_time(): false when fewer than 5 bytes came back.
bool lywsd02_decode_time(const uint8_t *data, size_t length, uint32_t *stamp, int *timezone);

// decode_env(): false for the empty answer the LYWSD02 gives to a plain read.
// voltage is NAN when the value carries none.
bool lywsd02_decode_env(const uint8_t *data, size_t length, double *temperature,
                        int *humidity, double *voltage);

typedef struct {
    char name[32];       // complete name, else shortened; empty when none was sent
    bool complete_name;
    bool has_service;    // ebe0ccb0 listed among the 128-bit service UUIDs
    int product_id;      // from fe95 service data, -1 when absent
} lywsd02_adv_t;

void lywsd02_adv_init(lywsd02_adv_t *adv);
// Folds one advertisement or scan response into `adv`; a field only changes when
// the packet carries it, the way bleak merges the two.
void lywsd02_parse_adv(const uint8_t *data, size_t length, lywsd02_adv_t *adv);
bool lywsd02_looks_like_clock(const lywsd02_adv_t *adv);

// Length of the well-formed UTF-8 character at `p`, or 0 when it is not one.
size_t lywsd02_utf8_char(const char *p, size_t left);

// NimBLE keeps the address least significant byte first.
void lywsd02_key(const uint8_t addr[6], char out[13]);
void lywsd02_address(const uint8_t addr[6], char out[18]);
// The inverse of lywsd02_key() for anything normalize_address() would accept.
bool lywsd02_parse_key(const char *text, uint8_t addr[6]);
