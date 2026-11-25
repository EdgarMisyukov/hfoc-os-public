#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_WIRE_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_WIRE_H

#include "appliance_network_config.h"
#include "appliance_network_mode.h"
#include "appliance_network_wire.h"
#include "appliance_time_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-362B ordinary-boot DHCP/DNS wire owner. It binds the ITEM-362A
 * production NIC/lwIP composition to the reused appliance network lifecycle
 * transport seam, drives the bounded DHCP and DNS wire protocols, and feeds
 * the received frames back into the lifecycle. It never claims an address or
 * answer that the wire did not actually publish.
 */
enum appliance_production_network_wire_status {
    APPLIANCE_PRODUCTION_NETWORK_WIRE_OK = 0,
    APPLIANCE_PRODUCTION_NETWORK_WIRE_UNAVAILABLE = 1,
    APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED = 2,
};

/*
 * ITEM-654: the SNTP source the operator declared for this boot.  `declared`
 * false keeps the pre-item rule (the active gateway).  A declared but invalid
 * address is published as INVALID and leaves the clock unsynchronized: the
 * source is never silently changed and the network itself still comes up.
 */
struct appliance_production_network_time_config {
    bool declared;
    bool valid;
    struct network_ipv4_address server;
};

enum appliance_production_network_time_source {
    APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE = 0,
    APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_GATEWAY = 1,
    APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_EXPLICIT = 2,
    APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID = 3,
};

enum appliance_production_network_wire_status
appliance_production_network_wire_boot_init(
    const struct appliance_network_config *desired,
    const struct appliance_production_network_time_config *time);
void appliance_production_network_wire_boot_poll(uint32_t now_ms);
bool appliance_production_network_wire_boot_input(
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms);
const struct appliance_network_wire *
appliance_production_network_wire_boot_wire(void);
enum appliance_production_network_wire_status
appliance_production_network_wire_boot_status(void);
/* ITEM-374: the decided mode, the validated wall clock gate and the derived
 * epoch for a bounded time source. */
enum appliance_network_mode_action
appliance_production_network_wire_boot_mode(void);
bool appliance_production_network_time_ready(void);
/* ITEM-654: the selected source, its kind and the bounded client's own
 * state/status, so `network`, the journal and the diag export name the
 * address and the result instead of only READY/UNSYNCHRONIZED. */
bool appliance_production_network_time_server(
    struct network_ipv4_address *server_out,
    enum appliance_production_network_time_source *source_out);
enum appliance_time_sync_state appliance_production_network_time_state(void);
enum appliance_time_sync_status appliance_production_network_time_status(void);
bool appliance_production_network_time_deadline(uint32_t *deadline_ms_out);
uint32_t appliance_production_network_time_epoch_seconds(uint32_t now_ms);

#endif
