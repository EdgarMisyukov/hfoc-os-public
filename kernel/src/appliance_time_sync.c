#include "appliance_time_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TIME_SYNC_MODE_MASK 0x07U
#define TIME_SYNC_VERSION_MASK 0x38U
#define TIME_SYNC_VERSION_SHIFT 3U
#define TIME_SYNC_LI_MASK 0xC0U
#define TIME_SYNC_LI_SHIFT 6U
#define TIME_SYNC_MODE_SERVER 4U
#define TIME_SYNC_LI_UNSYNCHRONIZED 3U
#define TIME_SYNC_STRATUM_MIN 1U
#define TIME_SYNC_STRATUM_MAX 15U
#define TIME_SYNC_OFFSET_ORIGINATE 24U
#define TIME_SYNC_OFFSET_TRANSMIT 40U
#define TIME_SYNC_MAX_DEADLINE_MS 60000U

static void time_sync_store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t time_sync_load_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static bool time_sync_server_zero(const uint8_t server[4U])
{
    return server[0] == 0U && server[1] == 0U && server[2] == 0U &&
           server[3] == 0U;
}

static void time_sync_build_request(
    struct appliance_time_sync *sync,
    uint32_t now_ms)
{
    uint32_t transmit_seconds = 0U;

    memset(sync->request, 0, sizeof(sync->request));
    sync->request[0] = 0x23U; /* LI=0, VN=4, Mode=3 (client). */
    sync->request[2] = 4U;
    sync->request[3] = 0xECU;
    if (sync->base_epoch_seconds != 0U) {
        transmit_seconds =
            sync->base_epoch_seconds +
            (now_ms - sync->base_now_ms) / 1000U;
    }
    time_sync_store_be32(
        sync->request + TIME_SYNC_OFFSET_TRANSMIT, transmit_seconds);
    time_sync_store_be32(
        sync->request + TIME_SYNC_OFFSET_TRANSMIT + 4U,
        (uint32_t)sync->transaction_id);
    sync->origin_ntp_seconds = transmit_seconds;
    sync->origin_ntp_fraction = (uint32_t)sync->transaction_id;
    sync->request_length = APPLIANCE_TIME_SYNC_PACKET_LENGTH;
}

static void time_sync_arm_attempt(
    struct appliance_time_sync *sync,
    uint32_t now_ms)
{
    ++sync->transaction_id;
    if (sync->transaction_id == 0U) {
        sync->transaction_id = 1U;
    }
    sync->deadline_ms = now_ms + sync->config.attempt_deadline_ms;
    sync->deadline_set = true;
    time_sync_build_request(sync, now_ms);
}

enum appliance_time_sync_status appliance_time_sync_init(
    struct appliance_time_sync *sync,
    const struct appliance_time_sync_config *config)
{
    if (sync == NULL || config == NULL) {
        return APPLIANCE_TIME_SYNC_NULL;
    }
    if (config->version != APPLIANCE_TIME_SYNC_VERSION) {
        return APPLIANCE_TIME_SYNC_BAD_VERSION;
    }
    if (time_sync_server_zero(config->server) || config->port == 0U ||
        config->max_attempts == 0U ||
        config->max_attempts > APPLIANCE_TIME_SYNC_ATTEMPT_MAX ||
        config->attempt_deadline_ms == 0U ||
        config->attempt_deadline_ms > TIME_SYNC_MAX_DEADLINE_MS) {
        return APPLIANCE_TIME_SYNC_BAD_CONFIG;
    }

    memset(sync, 0, sizeof(*sync));
    sync->version = APPLIANCE_TIME_SYNC_VERSION;
    sync->initialized = true;
    sync->state = APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED;
    sync->last_status = APPLIANCE_TIME_SYNC_OK;
    sync->config = *config;
    sync->request_length = APPLIANCE_TIME_SYNC_PACKET_LENGTH;
    time_sync_build_request(sync, 0U);
    return APPLIANCE_TIME_SYNC_OK;
}

enum appliance_time_sync_status appliance_time_sync_start(
    struct appliance_time_sync *sync,
    uint32_t now_ms)
{
    if (sync == NULL) {
        return APPLIANCE_TIME_SYNC_NULL;
    }
    if (!sync->initialized ||
        sync->state == APPLIANCE_TIME_SYNC_STATE_NONE ||
        sync->state == APPLIANCE_TIME_SYNC_STATE_SYNCING) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    sync->state = APPLIANCE_TIME_SYNC_STATE_SYNCING;
    sync->last_status = APPLIANCE_TIME_SYNC_OK;
    sync->attempt_count = 1U;
    time_sync_arm_attempt(sync, now_ms);
    return APPLIANCE_TIME_SYNC_OK;
}

enum appliance_time_sync_status appliance_time_sync_tick(
    struct appliance_time_sync *sync,
    uint32_t now_ms)
{
    if (sync == NULL) {
        return APPLIANCE_TIME_SYNC_NULL;
    }
    if (!sync->initialized ||
        sync->state != APPLIANCE_TIME_SYNC_STATE_SYNCING ||
        !sync->deadline_set) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    if ((int32_t)(now_ms - sync->deadline_ms) < 0) {
        return APPLIANCE_TIME_SYNC_OK;
    }
    if (sync->attempt_count >= sync->config.max_attempts) {
        sync->state = APPLIANCE_TIME_SYNC_STATE_FAILED;
        sync->last_status = APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED;
        sync->deadline_set = false;
        return APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED;
    }
    ++sync->attempt_count;
    time_sync_arm_attempt(sync, now_ms);
    return APPLIANCE_TIME_SYNC_OK;
}

enum appliance_time_sync_status appliance_time_sync_input(
    struct appliance_time_sync *sync,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms)
{
    uint32_t ntp_seconds;
    uint32_t epoch_seconds;

    if (sync == NULL || frame == NULL) {
        return APPLIANCE_TIME_SYNC_NULL;
    }
    if (!sync->initialized) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    if (sync->state != APPLIANCE_TIME_SYNC_STATE_SYNCING) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    if (frame_length != APPLIANCE_TIME_SYNC_PACKET_LENGTH) {
        return APPLIANCE_TIME_SYNC_BAD_PACKET;
    }
    if ((frame[0] & TIME_SYNC_LI_MASK) >> TIME_SYNC_LI_SHIFT ==
            TIME_SYNC_LI_UNSYNCHRONIZED ||
        (frame[0] & TIME_SYNC_MODE_MASK) != TIME_SYNC_MODE_SERVER) {
        return APPLIANCE_TIME_SYNC_BAD_PACKET;
    }
    {
        const uint8_t version =
            (uint8_t)((frame[0] & TIME_SYNC_VERSION_MASK) >>
                      TIME_SYNC_VERSION_SHIFT);

        if (version != 3U && version != 4U) {
            return APPLIANCE_TIME_SYNC_BAD_PACKET;
        }
    }
    if (frame[1] < TIME_SYNC_STRATUM_MIN ||
        frame[1] > TIME_SYNC_STRATUM_MAX) {
        return APPLIANCE_TIME_SYNC_BAD_PACKET;
    }
    if (memcmp(frame + TIME_SYNC_OFFSET_ORIGINATE,
               sync->request + TIME_SYNC_OFFSET_TRANSMIT, 8U) != 0) {
        return APPLIANCE_TIME_SYNC_STALE_PACKET;
    }

    ntp_seconds = time_sync_load_be32(
        frame + TIME_SYNC_OFFSET_TRANSMIT);
    if (ntp_seconds <= APPLIANCE_TIME_SYNC_NTP_UNIX_OFFSET) {
        sync->last_status = APPLIANCE_TIME_SYNC_OUT_OF_RANGE;
        return APPLIANCE_TIME_SYNC_OUT_OF_RANGE;
    }
    epoch_seconds = ntp_seconds - APPLIANCE_TIME_SYNC_NTP_UNIX_OFFSET;
    if (epoch_seconds < APPLIANCE_TIME_SYNC_EPOCH_MIN ||
        epoch_seconds > APPLIANCE_TIME_SYNC_EPOCH_MAX) {
        sync->last_status = APPLIANCE_TIME_SYNC_OUT_OF_RANGE;
        return APPLIANCE_TIME_SYNC_OUT_OF_RANGE;
    }

    sync->base_epoch_seconds = epoch_seconds;
    sync->base_now_ms = now_ms;
    sync->state = APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED;
    sync->last_status = APPLIANCE_TIME_SYNC_OK;
    sync->deadline_set = false;
    sync->attempt_count = 0U;
    return APPLIANCE_TIME_SYNC_OK;
}

bool appliance_time_sync_ready(const struct appliance_time_sync *sync)
{
    return sync != NULL && sync->initialized &&
           sync->base_epoch_seconds != 0U;
}

bool appliance_time_sync_deadline(
    const struct appliance_time_sync *sync,
    uint32_t *deadline_ms_out)
{
    if (sync == NULL || deadline_ms_out == NULL) {
        return false;
    }
    if (!sync->initialized || !sync->deadline_set ||
        sync->state != APPLIANCE_TIME_SYNC_STATE_SYNCING) {
        return false;
    }
    *deadline_ms_out = sync->deadline_ms;
    return true;
}

bool appliance_time_sync_server(
    const struct appliance_time_sync *sync,
    uint8_t server_out[4U],
    uint16_t *port_out)
{
    if (sync == NULL || server_out == NULL || port_out == NULL ||
        !sync->initialized) {
        return false;
    }
    memcpy(server_out, sync->config.server, 4U);
    *port_out = sync->config.port;
    return true;
}

enum appliance_time_sync_state appliance_time_sync_state(
    const struct appliance_time_sync *sync)
{
    if (sync == NULL || !sync->initialized) {
        return APPLIANCE_TIME_SYNC_STATE_NONE;
    }
    return sync->state;
}

enum appliance_time_sync_status appliance_time_sync_last_status(
    const struct appliance_time_sync *sync)
{
    if (sync == NULL || !sync->initialized) {
        return APPLIANCE_TIME_SYNC_BAD_STATE;
    }
    return sync->last_status;
}

uint32_t appliance_time_sync_epoch_seconds(
    const struct appliance_time_sync *sync,
    uint32_t now_ms)
{
    if (!appliance_time_sync_ready(sync)) {
        return 0U;
    }
    return sync->base_epoch_seconds +
           (now_ms - sync->base_now_ms) / 1000U;
}

const char *appliance_time_sync_state_string(
    enum appliance_time_sync_state state)
{
    switch (state) {
    case APPLIANCE_TIME_SYNC_STATE_NONE:
        return "NONE";
    case APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED:
        return "UNSYNCHRONIZED";
    case APPLIANCE_TIME_SYNC_STATE_SYNCING:
        return "SYNCING";
    case APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case APPLIANCE_TIME_SYNC_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_time_sync_status_string(
    enum appliance_time_sync_status status)
{
    switch (status) {
    case APPLIANCE_TIME_SYNC_OK:
        return "OK";
    case APPLIANCE_TIME_SYNC_NULL:
        return "NULL";
    case APPLIANCE_TIME_SYNC_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_TIME_SYNC_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_TIME_SYNC_BAD_CONFIG:
        return "BAD_CONFIG";
    case APPLIANCE_TIME_SYNC_BAD_PACKET:
        return "BAD_PACKET";
    case APPLIANCE_TIME_SYNC_STALE_PACKET:
        return "STALE_PACKET";
    case APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED:
        return "ATTEMPTS_EXHAUSTED";
    case APPLIANCE_TIME_SYNC_NOT_SYNCHRONIZED:
        return "NOT_SYNCHRONIZED";
    case APPLIANCE_TIME_SYNC_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case APPLIANCE_TIME_SYNC_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
