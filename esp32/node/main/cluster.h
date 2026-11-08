#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "logs.h"

// Port a hand-typed peer gets when none is written. The other nodes are mostly the
// Python program, which listens on 8787.
#define PEER_DEFAULT_PORT 8787

#define CLUSTER_LOCK_TTL 900.0
#define CLUSTER_BUCKET 6
#define CLUSTER_SIGHTING_TTL (7 * 86400.0)
#define CLUSTER_TAKEOVER_AFTER (26 * 3600.0)
#define CLUSTER_STATE_TIMEOUT 1.5
#define CLUSTER_ACTION_TIMEOUT 8.0

typedef struct {
    char url[96];
    char name[97];
} cluster_peer_t;

// Starts the beacon and the poll loop.
void cluster_start(void);

// normalize_peer() of cluster.py: "10.0.0.5", "10.0.0.5:8787" or
// "http://10.0.0.5:8787" -> "http://10.0.0.5:8787". False when it cannot be read.
bool cluster_normalize_peer(const char *entry, int default_port, char *out, size_t size);

// round(rssi / BUCKET), rounding half to even like Python.
int cluster_bucket(double rssi);

// config.node.cluster.
bool cluster_enabled(void);
// Seconds since boot; the clock every cluster timer runs on.
double cluster_clock(void);
void cluster_wake(void);
// A page asked for /api/state: poll fast for the next WATCH_WINDOW seconds.
void cluster_watch(void);
// Probe the peers now unless that was just done. Makes network calls.
void cluster_ensure_fresh(void);
// hear(): whoever probed us from `ipv4` (network order) listens on `port`.
void cluster_hear(uint32_t ipv4, int port);

// acquire(): hold the cluster still for `op` ({event, params}, not taken). False
// (and logged) when refused, with the name of the node in the way.
bool cluster_acquire(const cJSON *op, char token[33], char *blocker, size_t size);
void cluster_release(const char *token);
bool cluster_holds(const char *token);
// keep(): called between the steps of a long job so the lock does not expire.
void cluster_keep(const char *token);
// grant(): the answer to a peer's /api/node/lock. `claim` is borrowed.
cJSON *cluster_grant(const cJSON *claim);
// drop(): a peer's /api/node/unlock.
void cluster_drop(const char *token);
// lock_view(): {holder, holder_id, op, mine} or JSON null.
cJSON *cluster_lock_view(void);

// Validate JSON text without allocation, including escaped strings and depth <= 16.
// len excludes any trailing NUL; embedded NULs are rejected.
bool cluster_json_bounded(const char *text, size_t len);

// One request to another node: a POST of `payload` (or {}), or a GET when `get`.
// Returns the JSON of a 2xx answer. Otherwise NULL, with *status the HTTP status
// when the node answered with an error (0 for network failure or timeout, -1 for
// a local allocation/input limit failure) and, when `failure` is given, what
// Rejected.detail or str(error) would hold in Python.
cJSON *cluster_call(const char *url, const char *path, const cJSON *payload, bool get,
                    double timeout, int *status, cJSON **failure);
// POST `payload` (or {}) to every online peer, ignoring the answers.
void cluster_tell_all(const char *path, const cJSON *payload, double timeout);
// The online peer that is `node_id`; false when none is.
bool cluster_peer_of(const char *node_id, cluster_peer_t *out);
#define CLUSTER_MAX_PEERS 10
// -1 on metadata allocation failure; *metadata is NULL and copied lines are freed.
int cluster_log_source(int index, const cJSON *cursors, cJSON **metadata,
                       log_line_t *out, int limit);

// start_on_peers(): hand the job at `path` to every online peer; returns how many
// took it or may have started, listed in *started (free it). -1 on local setup OOM.
int cluster_start_on_peers(const char *path, const char *token, const char *event,
                           cluster_peer_t **started);
// wait_for_peers(): true once none of them is busy, false at `deadline`
// (cluster_clock() seconds). Keeps `token` alive meanwhile.
bool cluster_wait_for_peers(const cluster_peer_t *peers, int count, double deadline,
                            const char *token);

// merge(): one row per device from every node's list; `local_views` is borrowed.
cJSON *cluster_merge(const cJSON *local_views);
// my_share(): append scheduled keys; false on allocation failure.
bool cluster_my_share(const cJSON *merged, cJSON *mine, cJSON *taken);
bool cluster_my_work(const cJSON *merged);
bool cluster_online(const char *node_id);
// nodes_view(): the node rows of /api/state. `busy` is taken.
cJSON *cluster_nodes_view(const cJSON *merged, cJSON *busy, int queued);
