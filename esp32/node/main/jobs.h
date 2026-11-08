#pragma once

#include <stdbool.h>

#include "cJSON.h"

// The Hub and the job functions of server.py: one worker drives the radio, one
// task fires the schedule.

typedef enum {
    JOB_SCAN,          // job_scan
    JOB_SCAN_CLUSTER,  // job_scan_cluster
    JOB_READ_OWNED,    // job_read_owned
    JOB_READ_CLUSTER,  // job_read_cluster
    JOB_VISIT,         // job_visit
    JOB_AUTO,          // job_auto
} job_kind_t;

void jobs_start(void);

// Diagnostic BLE cycles must hold this same lifecycle lock as the worker.
// Nonblocking; call outside store_lock(), and unlock only after a successful take.
bool jobs_ble_try_lock(void);
void jobs_ble_unlock(void);

// enqueue(): false on allocation/queue failure or when that key is already waiting
// or running. Takes ownership of `busy`, including on failure; NULL is refused.
// `token` is the cluster lock the job releases when it ends, or NULL.
bool jobs_enqueue(const char *key, cJSON *busy, job_kind_t kind, const char *device,
                  bool write, const char *token);

// hub.pending is not empty. Caller holds store_lock().
bool jobs_any_pending(void);
// A copy of hub.busy, or JSON null.
cJSON *jobs_busy(void);
int jobs_queued(void);

// Caller holds store_lock() for these three.
// record(): NULL on allocation failure or when adding beyond 200 devices;
// existing records are never removed by the limit.
cJSON *jobs_record(const char *key);
const char *jobs_name_of(const char *key);
// local_views(): every record sorted by key, or NULL on any construction failure.
cJSON *jobs_local_views(void);

// last_scheduled() and next_scheduled() of server.py. NAN when the schedule is off.
double jobs_last_scheduled(const cJSON *schedule, double now);
double jobs_next_scheduled(const cJSON *schedule, double now);
