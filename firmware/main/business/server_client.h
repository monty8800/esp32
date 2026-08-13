/**
 * @file server_client.h
 *
 * Client for the LAN server monitor (ct104 "dashboard" on Proxmox).
 *
 * Endpoint: GET http://192.168.9.206:8787/api/summary
 *   - no authentication (LAN-only service)
 *   - each request triggers live probing and takes ~1-3s to answer, so the
 *     poll interval must stay >= 15s and the timeout is generous (5s)
 *   - response: {"time","total","online","hosts":[{id,name,ip,online,
 *     latency_ms,probes:[{name,online},...]},...]}
 *
 * The URL can be overridden with the SERVER_SUMMARY_URL environment
 * variable. Implemented with libcurl + cJSON (the hosts/probes arrays are
 * nested, unlike flat HA state objects).
 *
 * Only ever called from the net_worker thread - never from the UI thread.
 */

#ifndef SERVER_CLIENT_H
#define SERVER_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "net_worker.h"

#define SERVER_SUMMARY_URL_DEFAULT "http://192.168.9.206:8787/api/summary"

/**
 * Fetch and parse /api/summary into @p out. On success fills
 * valid/time_str/total/online/hosts/host_count (valid=true) and returns
 * true. On any failure (connect, timeout, HTTP error, malformed JSON)
 * returns false and leaves @p out untouched, so the caller can keep the
 * previous data and mark it stale. The seq field is never touched here.
 */
bool server_client_fetch(server_snapshot_t * out);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CLIENT_H */
