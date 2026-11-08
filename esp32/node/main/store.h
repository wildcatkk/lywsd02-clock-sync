#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

// The three trees mirror config.json, state.json and devices.json of the Python
// node, so views built from them come out in the same shape. Every access goes
// through store_lock(); the lock is recursive and must never be held across a
// network call or a radio operation.

void store_init(void);
void store_lock(void);
void store_unlock(void);

cJSON *store_config(void);
cJSON *store_state(void);
cJSON *store_devices(void);

void store_save_config(void);
void store_save_state(void);
void store_save_devices(void);

const char *store_node_id(void);
// The chosen hostname, or the default one: chip plus the last two MAC bytes,
// e.g. ESP32-C3-E7DC. The returned string stays valid across a later change.
const char *store_hostname(void);
const char *store_default_hostname(void);
// "ESP32-C3" for esp32c3, "ESP32" for esp32.
const char *store_chip(void);
// RFC 1123 label: 1-32 letters, digits and '-', not starting or ending with '-'.
bool store_hostname_valid(const char *name);
// An empty name goes back to the default. Returns false for an invalid name.
bool store_set_hostname(const char *name);
// config.node.name, or the hostname when that is blank. Takes the lock itself.
void store_node_name(char *out, size_t size);

double store_now(void);

// Small cJSON conveniences used everywhere.
const char *json_str(const cJSON *object, const char *key, const char *fallback);
double json_num(const cJSON *object, const char *key, double fallback);
bool json_bool(const cJSON *object, const char *key, bool fallback);
// Takes ownership of value, including on failure; returns NULL if insertion fails.
cJSON *json_set(cJSON *object, const char *key, cJSON *value);
cJSON *json_num_or_null(double value);
