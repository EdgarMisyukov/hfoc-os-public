#ifndef HFOC_APPLIANCE_TIME_SYNC_H
#define HFOC_APPLIANCE_TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-374 wall clock and SNTP time synchronization.
 *
 * The frozen baseline has no RTC and only a monotonic millisecond timer, so
 * TLS has no trustworthy clock.  This module owns a bounded SNTP unicast
 * client: it builds one request at a time, validates the reply against the
 * request it sent (mode, version, stratum, originate timestamp, sane epoch),
 * and anchors a wall clock on the monotonic timer only after a validated
 * reply.  Without a validated reply the clock is UNSYNCHRONIZED and never
 * fabricated; the service activation gate stays closed.
 */

#define APPLIANCE_TIME_SYNC_VERSION 1U
#define APPLIANCE_TIME_SYNC_PACKET_LENGTH 48U
#define APPLIANCE_TIME_SYNC_PORT 123U
#define APPLIANCE_TIME_SYNC_ATTEMPT_MAX 3U
#define APPLIANCE_TIME_SYNC_DEADLINE_MS 2000U
#define APPLIANCE_TIME_SYNC_NTP_UNIX_OFFSET UINT32_C(2208988800)
/* 2023-11-14T22:13:20Z and 2100-01-01T00:00:00Z keep a bogus reply out. */
#define APPLIANCE_TIME_SYNC_EPOCH_MIN UINT32_C(1700000000)
#define APPLIANCE_TIME_SYNC_EPOCH_MAX UINT32_C(4102444800)

enum appliance_time_sync_state {
    APPLIANCE_TIME_SYNC_STATE_NONE = 0,
    APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED = 1,
    APPLIANCE_TIME_SYNC_STATE_SYNCING = 2,
    APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED = 3,
    APPLIANCE_TIME_SYNC_STATE_FAILED = 4,
};

enum appliance_time_sync_status {
    APPLIANCE_TIME_SYNC_OK = 0,
    APPLIANCE_TIME_SYNC_NULL = 1,
    APPLIANCE_TIME_SYNC_BAD_STATE = 2,
    APPLIANCE_TIME_SYNC_BAD_VERSION = 3,
    APPLIANCE_TIME_SYNC_BAD_CONFIG = 4,
    APPLIANCE_TIME_SYNC_BAD_PACKET = 5,
    APPLIANCE_TIME_SYNC_STALE_PACKET = 6,
    APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED = 7,
    APPLIANCE_TIME_SYNC_NOT_SYNCHRONIZED = 8,
    APPLIANCE_TIME_SYNC_OUT_OF_RANGE = 9,
    APPLIANCE_TIME_SYNC_BUFFER_TOO_SMALL = 10,
};

struct appliance_time_sync_config {
    uint32_t version;
    uint8_t server[4U];
    uint16_t port;
    uint32_t max_attempts;
    uint32_t attempt_deadline_ms;
};

struct appliance_time_sync {
    uint32_t version;
    bool initialized;
    enum appliance_time_sync_state state;
    enum appliance_time_sync_status last_status;
    struct appliance_time_sync_config config;
    uint32_t attempt_count;
    uint32_t deadline_ms;
    bool deadline_set;
    uint8_t request[APPLIANCE_TIME_SYNC_PACKET_LENGTH];
    uint8_t request_length;
    uint8_t transaction_id;
    uint32_t origin_ntp_seconds;
    uint32_t origin_ntp_fraction;
    uint32_t base_epoch_seconds;
    uint32_t base_now_ms;
};

enum appliance_time_sync_status appliance_time_sync_init(
    struct appliance_time_sync *sync,
    const struct appliance_time_sync_config *config);
enum appliance_time_sync_status appliance_time_sync_start(
    struct appliance_time_sync *sync,
    uint32_t now_ms);
enum appliance_time_sync_status appliance_time_sync_tick(
    struct appliance_time_sync *sync,
    uint32_t now_ms);
enum appliance_time_sync_status appliance_time_sync_input(
    struct appliance_time_sync *sync,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms);
bool appliance_time_sync_ready(
    const struct appliance_time_sync *sync);
bool appliance_time_sync_deadline(
    const struct appliance_time_sync *sync,
    uint32_t *deadline_ms_out);
bool appliance_time_sync_server(
    const struct appliance_time_sync *sync,
    uint8_t server_out[4U],
    uint16_t *port_out);
enum appliance_time_sync_state appliance_time_sync_state(
    const struct appliance_time_sync *sync);
enum appliance_time_sync_status appliance_time_sync_last_status(
    const struct appliance_time_sync *sync);
uint32_t appliance_time_sync_epoch_seconds(
    const struct appliance_time_sync *sync,
    uint32_t now_ms);
const char *appliance_time_sync_state_string(
    enum appliance_time_sync_state state);
const char *appliance_time_sync_status_string(
    enum appliance_time_sync_status status);

#endif
