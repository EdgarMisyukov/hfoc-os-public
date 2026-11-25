#ifndef HFOC_APPLIANCE_PRODUCTION_PANEL_VIEW_H
#define HFOC_APPLIANCE_PRODUCTION_PANEL_VIEW_H

#include "appliance_management_web_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-505 increment 2: the live panel view of the production appliance.
 *
 * The ITEM-384 panel reads one bounded view for every generic section
 * (Dashboard, Services, Storage, System, Diagnostics, Updates, Recovery): the
 * hostname, the management MAC, the service name, a detail line and a recent
 * log line.  Before this module the only publisher of that view was the QEMU
 * fixture (`virtio_net_frame_qemu.c`), so on a real appliance every one of
 * those fields rendered the literal `unavailable` although the owners - the
 * loaded configuration, the production network composition and the serving
 * path - were all present in the same kernel.
 *
 * This module composes the view from those owners and from nothing else:
 *
 *   * every field is optional, and a NULL owner publishes `unavailable`
 *     exactly as the renderer already renders a NULL field;
 *   * each value has its own bound, and a value that does not fit its bound is
 *     published as `unavailable` rather than cut: a truncated hostname or MAC
 *     would name a machine that does not exist;
 *   * the module allocates nothing and owns nothing: the caller owns the
 *     storage and every pointer in the view points into it.
 *
 * The bounds are the ones the recorded management-panel page budgets cover
 * (see `APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_*`): the production view can never
 * grow a rendered section page past the buffer the panel renders into.
 */

#define APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION 1U

/* Bound of the hostname copy, NUL included. */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_HOSTNAME_MAX 64U
/* Bound of the rendered `aa:bb:cc:dd:ee:ff` MAC address, NUL included. */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_MAX 18U
/* Bound of the service name copy, NUL included. */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_SERVICE_MAX 16U
/* Bound of the `state=... reason=...` detail line, NUL included. */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_DETAIL_MAX 96U
/*
 * Bound of the recent-log line copy, NUL included.  It is the renderer's own
 * per-value bound (`APPLIANCE_MANAGEMENT_WEB_UI_VALUE_MAX`): the renderer
 * shows a value whole or stops at that bound, so a longer line would be cut
 * silently.  The production line is composed to fit, and a line that does not
 * fit is published as `unavailable` rather than cut.
 */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_LOG_MAX 96U
/* The MAC address length every owner publishes. */
#define APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH 6U

enum appliance_production_panel_view_status {
    APPLIANCE_PRODUCTION_PANEL_VIEW_OK = 0,
    APPLIANCE_PRODUCTION_PANEL_VIEW_NULL = 1,
    APPLIANCE_PRODUCTION_PANEL_VIEW_BAD_VERSION = 2,
};

/*
 * The owners a production panel view reads.  Every pointer is borrowed for the
 * duration of the compose call and copied into the caller's storage; the
 * module keeps none of them.
 */
struct appliance_production_panel_view_owners {
    uint32_t version;
    /* The persistent hostname, or the ITEM-302 identity fallback. */
    const char *hostname;
    /* `APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH` bytes, or NULL. */
    const uint8_t *mac_address;
    /* The service the listener serves, or NULL. */
    const char *service_name;
    /* The serving state string, or NULL. */
    const char *serving_state;
    /* The serving state's own reason, or NULL. */
    const char *serving_reason;
    /* The newest bounded `[PRODUCTION-SERVING]` panel line, or NULL. */
    const char *recent_log;
};

/*
 * The caller-owned storage the view points into.  A field whose owner was NULL
 * or whose value did not fit stays zeroed and the view names it NULL, which
 * the renderer publishes as `unavailable`.
 */
struct appliance_production_panel_view_storage {
    char hostname[APPLIANCE_PRODUCTION_PANEL_VIEW_HOSTNAME_MAX];
    char mac_address[APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_MAX];
    char service_name[APPLIANCE_PRODUCTION_PANEL_VIEW_SERVICE_MAX];
    char detail[APPLIANCE_PRODUCTION_PANEL_VIEW_DETAIL_MAX];
    char log_line[APPLIANCE_PRODUCTION_PANEL_VIEW_LOG_MAX];
    struct appliance_management_web_ui_view view;
};

/*
 * Fill `storage` from `owners` and point `storage->view` at the values that
 * were published.  The result is deterministic and total: missing owners and
 * oversized values are published as NULL fields, never invented and never cut.
 */
enum appliance_production_panel_view_status
appliance_production_panel_view_compose(
    const struct appliance_production_panel_view_owners *owners,
    struct appliance_production_panel_view_storage *storage);

#endif
