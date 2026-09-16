/**
 * @file panel_client.h
 *
 * Client for the LAN "panel hub" (LXC 117 panel-hub on Proxmox) — the single
 * backend of the desk panel.
 *
 * Endpoint: GET http://192.168.9.216:8790/api/panel
 *   - no authentication (LAN-only service; optional X-Panel-Token if the
 *     operator sets PANEL_API_TOKEN on the hub)
 *   - the hub serves a PRE-AGGREGATED json from a state file, so the answer
 *     is fast (~10ms) and does NOT depend on upstream health: the hub keeps
 *     polling LixingXing / mail-ai-service on its own schedule and flags
 *     stale data in the payload.
 *   - payload carries three independent blocks, each with its own "ok":
 *       ops  -> LixingXing sales + FBA stockout   (core, drives top-level stale)
 *       mail -> mail-ai-service pending drafts
 *       dev  -> Mac push heartbeat (cron jobs + projects)
 *     The UI must consult the PER-BLOCK ok, never only the top-level stale.
 *
 * The URL can be overridden with the PANEL_URL environment variable (fed from
 * the "panel_url" NVS key by env_shim.c).
 *
 * Only ever called from the net_worker thread - never from the UI thread.
 */

#ifndef PANEL_CLIENT_H
#define PANEL_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "net_worker.h"

#define PANEL_API_URL_DEFAULT "http://192.168.9.216:8790/api/panel"

/**
 * Fetch and parse /api/panel into @p out. On success fills the whole
 * snapshot (valid=true) and returns true. On any failure (connect, timeout,
 * HTTP error, malformed JSON) returns false and leaves @p out untouched, so
 * the caller can keep the previous data and mark it stale. The seq field is
 * never touched here.
 */
bool panel_client_fetch(panel_snapshot_t * out);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_CLIENT_H */
