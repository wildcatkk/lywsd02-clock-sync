#pragma once

#include <stdint.h>

#include "cJSON.h"

// The node's own log, kept in memory only, like hub.logs in server.py. Entries are
// {at, event, params}; the page translates "log.<event>" with the params.

// One entry as JSON text, so the cluster's merged log never has to be a cJSON tree.
typedef struct {
    double at;
    char *text;
    uint32_t id;
} log_line_t;

void logs_init(void);
// Pairs of (const char *key, cJSON *value), terminated by NULL. Takes ownership of
// every value.
void logs_put(const char *event, ...);
void logs_version(char boot[17], uint32_t *cursor);
int logs_read(log_line_t *out, int limit, const char *boot, uint32_t after,
              char epoch[17], uint32_t *cursor);
void logs_free(log_line_t *lines, int count);
void logs_clear(void);
