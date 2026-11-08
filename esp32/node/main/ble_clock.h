#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "lywsd02.h"

// The radio half of lywsd02.py on NimBLE. Every call blocks and must come from the
// one job worker: the controller is only switched on for the length of a job.

typedef struct {
    uint8_t addr[6];     // least significant byte first, as NimBLE has it
    uint8_t addr_type;
    int8_t rssi;
    lywsd02_adv_t adv;
} ble_seen_t;

esp_err_t ble_clock_start(void);
void ble_clock_stop(void);

// scan(): everything heard in `seconds` that looks like a clock or is in `known`.
// Returns the count and a malloc'ed array in *out (NULL when none), or -1.
int ble_clock_scan(double seconds, const uint8_t (*known)[6], int known_count, ble_seen_t **out);

// resolve(): true once the address advertises within `seconds`, with its type.
bool ble_clock_find(const uint8_t addr[6], double seconds, uint8_t *addr_type);

typedef struct {
    bool write;
    bool env;
    double connect_timeout;
    double env_timeout;
    double offset;       // time server minus own clock, 0 for the own clock
} ble_visit_t;

// visit(): connect once, fill `report` with what lywsd02.visit() reports, and set
// the clock when asked. On failure returns false with a repr()-style error.
bool ble_clock_visit(const uint8_t addr[6], uint8_t addr_type, const ble_visit_t *how,
                     cJSON *report, char *error, size_t size);

// repr() of a float: "15.0", "2.5".
void ble_clock_float(double value, char *out, size_t size);
