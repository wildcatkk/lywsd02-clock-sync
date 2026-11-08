#pragma once

#include <stdbool.h>
#include <stddef.h>

// One SNTP round trip to host. On success *offset is what to add to store_now()
// to get the server's time; on failure error holds a short reason.
bool ntp_query(const char *host, double *offset, char *error, size_t size);
