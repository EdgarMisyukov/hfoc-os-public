/*
 * ITEM-374 focused host proof: bounded SNTP time synchronization.
 *
 * The proof builds a real 48-byte SNTP request and a matching reply on the
 * wire shape the production network owner feeds in, and it refuses every
 * malformed, stale or out-of-range reply without ever publishing a clock.
 */

#ifdef HFOC_TIME_SYNC_HOST_TEST

#include "appliance_time_sync.h"
#include "appliance_time_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

static const uint8_t test_server[4U] = { 10U, 0U, 2U, 2U };
static const uint32_t test_epoch = UINT32_C(1750000000);

static void host_store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static struct appliance_time_sync_config host_config(void)
{
    struct appliance_time_sync_config config;

    memset(&config, 0, sizeof(config));
    config.version = APPLIANCE_TIME_SYNC_VERSION;
    memcpy(config.server, test_server, sizeof(test_server));
    config.port = APPLIANCE_TIME_SYNC_PORT;
    config.max_attempts = APPLIANCE_TIME_SYNC_ATTEMPT_MAX;
    config.attempt_deadline_ms = APPLIANCE_TIME_SYNC_DEADLINE_MS;
    return config;
}

static uint32_t host_ntp_seconds(uint32_t epoch_seconds)
{
    return epoch_seconds + APPLIANCE_TIME_SYNC_NTP_UNIX_OFFSET;
}

/* Builds a 48-byte SNTP server reply that answers the request in sync. */
static void host_build_reply(
    const struct appliance_time_sync *sync,
    uint32_t epoch_seconds,
    uint32_t fraction,
    uint8_t reply[APPLIANCE_TIME_SYNC_PACKET_LENGTH])
{
    memset(reply, 0, APPLIANCE_TIME_SYNC_PACKET_LENGTH);
    reply[0] = 0x24U; /* LI=0, VN=4, Mode=4 (server). */
    reply[1] = 2U;    /* Stratum 2. */
    reply[2] = 4U;
    reply[3] = 0xECU;
    memcpy(reply + 24U, sync->request + 40U, 8U);
    host_store_be32(reply + 32U, host_ntp_seconds(epoch_seconds));
    host_store_be32(reply + 36U, fraction);
    host_store_be32(reply + 40U, host_ntp_seconds(epoch_seconds));
    host_store_be32(reply + 44U, fraction);
}

static void host_time_sync_contract(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();
    struct appliance_time_sync_config bad = host_config();
    uint8_t server[4U];
    uint16_t port = 0U;

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(NULL, &config) ==
              APPLIANCE_TIME_SYNC_NULL,
          "time sync refuses a null handle");
    CHECK(appliance_time_sync_init(&sync, NULL) ==
              APPLIANCE_TIME_SYNC_NULL,
          "time sync refuses a null configuration");

    bad.version = 99U;
    CHECK(appliance_time_sync_init(&sync, &bad) ==
              APPLIANCE_TIME_SYNC_BAD_VERSION,
          "time sync refuses an unsupported configuration version");
    bad = host_config();
    bad.server[0] = 0U;
    bad.server[1] = 0U;
    bad.server[2] = 0U;
    bad.server[3] = 0U;
    CHECK(appliance_time_sync_init(&sync, &bad) ==
              APPLIANCE_TIME_SYNC_BAD_CONFIG,
          "time sync refuses an unspecified server");
    bad = host_config();
    bad.port = 0U;
    CHECK(appliance_time_sync_init(&sync, &bad) ==
              APPLIANCE_TIME_SYNC_BAD_CONFIG,
          "time sync refuses an invalid port");
    bad = host_config();
    bad.max_attempts = 0U;
    CHECK(appliance_time_sync_init(&sync, &bad) ==
              APPLIANCE_TIME_SYNC_BAD_CONFIG,
          "time sync refuses a zero attempt budget");
    bad = host_config();
    bad.attempt_deadline_ms = 0U;
    CHECK(appliance_time_sync_init(&sync, &bad) ==
              APPLIANCE_TIME_SYNC_BAD_CONFIG,
          "time sync refuses a zero attempt deadline");

    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_state(&sync) ==
                  APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED &&
              !appliance_time_sync_ready(&sync),
          "an initialized client is unsynchronized and not ready");
    CHECK(appliance_time_sync_epoch_seconds(&sync, 1000U) == 0U,
          "an unsynchronized clock publishes no epoch");
    CHECK(appliance_time_sync_server(&sync, server, &port) &&
              memcmp(server, test_server, sizeof(server)) == 0 &&
              port == APPLIANCE_TIME_SYNC_PORT,
          "the client exposes the configured server");
}

static void host_time_sync_request(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK,
          "the request fixture initializes");
    CHECK(appliance_time_sync_tick(&sync, 0U) ==
              APPLIANCE_TIME_SYNC_BAD_STATE,
          "tick before start is refused");
    CHECK(appliance_time_sync_input(
              &sync, NULL, APPLIANCE_TIME_SYNC_PACKET_LENGTH, 0U) ==
              APPLIANCE_TIME_SYNC_NULL,
          "a null reply is refused");
    CHECK(appliance_time_sync_start(&sync, 100U) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_state(&sync) ==
                  APPLIANCE_TIME_SYNC_STATE_SYNCING,
          "start moves the client to SYNCING");
    CHECK(sync.request_length == APPLIANCE_TIME_SYNC_PACKET_LENGTH &&
              sync.request[0] == 0x23U &&
              sync.transaction_id != 0U,
          "the client builds an SNTP client request");
    CHECK(appliance_time_sync_start(&sync, 200U) ==
              APPLIANCE_TIME_SYNC_BAD_STATE,
          "a second start is refused");
}

static void host_time_sync_valid_reply(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();
    uint8_t reply[APPLIANCE_TIME_SYNC_PACKET_LENGTH];

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_start(&sync, 1000U) ==
                  APPLIANCE_TIME_SYNC_OK,
          "the valid-reply fixture starts");
    host_build_reply(&sync, test_epoch, 0x80000000U, reply);
    CHECK(appliance_time_sync_input(
              &sync, reply, sizeof(reply), 1500U) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_state(&sync) ==
                  APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED &&
              appliance_time_sync_ready(&sync),
          "a matching server reply synchronizes the clock");
    CHECK(appliance_time_sync_epoch_seconds(&sync, 1500U) ==
              test_epoch &&
              appliance_time_sync_epoch_seconds(&sync, 2500U) ==
                  test_epoch + 1U,
          "the wall clock advances with the monotonic timer");
    CHECK(appliance_time_sync_last_status(&sync) ==
              APPLIANCE_TIME_SYNC_OK,
          "the synchronized client publishes OK");
}

static void host_time_sync_bad_replies(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();
    uint8_t reply[APPLIANCE_TIME_SYNC_PACKET_LENGTH];

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_start(&sync, 1000U) ==
                  APPLIANCE_TIME_SYNC_OK,
          "the bad-reply fixture starts");
    host_build_reply(&sync, test_epoch, 0U, reply);

    {
        uint8_t broken[APPLIANCE_TIME_SYNC_PACKET_LENGTH];

        memcpy(broken, reply, sizeof(broken));
        broken[0] = 0x43U; /* Mode 3 (client) is not a server reply. */
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "a client-mode packet is not a server reply");
        memcpy(broken, reply, sizeof(broken));
        broken[0] = 0x13U; /* VN=1. */
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "an unsupported SNTP version is refused");
        memcpy(broken, reply, sizeof(broken));
        broken[0] = 0xE4U; /* LI=3 (unsynchronized server). */
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "an unsynchronized server is refused");
        memcpy(broken, reply, sizeof(broken));
        broken[1] = 0U;
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "stratum zero is refused");
        memcpy(broken, reply, sizeof(broken));
        broken[1] = 16U;
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "an out-of-range stratum is refused");
        memcpy(broken, reply, sizeof(broken));
        broken[27U] ^= 0x01U; /* Corrupt the originate timestamp. */
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_STALE_PACKET,
              "a reply to another request is stale");
        CHECK(appliance_time_sync_input(&sync, reply, 20U, 1500U) ==
                  APPLIANCE_TIME_SYNC_BAD_PACKET,
              "a truncated packet is refused");
        memcpy(broken, reply, sizeof(broken));
        host_store_be32(broken + 40U, 100U); /* 1900-01-01. */
        CHECK(appliance_time_sync_input(&sync, broken, sizeof(broken),
                                        1500U) ==
                  APPLIANCE_TIME_SYNC_OUT_OF_RANGE,
              "an out-of-range epoch is refused");
    }
    CHECK(!appliance_time_sync_ready(&sync) &&
              appliance_time_sync_state(&sync) ==
                  APPLIANCE_TIME_SYNC_STATE_SYNCING,
          "no bad reply ever publishes a clock");
}

static void host_time_sync_attempts(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();
    uint32_t deadline = 0U;

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_start(&sync, 1000U) ==
                  APPLIANCE_TIME_SYNC_OK,
          "the attempt fixture starts");
    CHECK(appliance_time_sync_deadline(&sync, &deadline) &&
              deadline == 1000U + APPLIANCE_TIME_SYNC_DEADLINE_MS,
          "the first attempt arms a bounded deadline");
    CHECK(appliance_time_sync_tick(&sync, 1000U) ==
              APPLIANCE_TIME_SYNC_OK &&
              sync.attempt_count == 1U,
          "tick before the deadline keeps the attempt");
    CHECK(appliance_time_sync_tick(
              &sync, 1000U + APPLIANCE_TIME_SYNC_DEADLINE_MS) ==
              APPLIANCE_TIME_SYNC_OK &&
              sync.attempt_count == 2U,
          "the deadline starts the next attempt");
    CHECK(appliance_time_sync_tick(
              &sync, 1000U + 2U * APPLIANCE_TIME_SYNC_DEADLINE_MS) ==
              APPLIANCE_TIME_SYNC_OK &&
              sync.attempt_count == APPLIANCE_TIME_SYNC_ATTEMPT_MAX,
          "the last attempt is bounded");
    CHECK(appliance_time_sync_tick(
              &sync, 1000U + 3U * APPLIANCE_TIME_SYNC_DEADLINE_MS) ==
              APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED &&
              appliance_time_sync_state(&sync) ==
                  APPLIANCE_TIME_SYNC_STATE_FAILED &&
              !appliance_time_sync_ready(&sync),
          "exhausting the attempts fails closed");
    CHECK(!appliance_time_sync_deadline(&sync, &deadline),
          "a failed client arms no further deadline");
    CHECK(strcmp(appliance_time_sync_state_string(
                     APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED),
                 "SYNCHRONIZED") == 0 &&
              strcmp(appliance_time_sync_status_string(
                         APPLIANCE_TIME_SYNC_ATTEMPTS_EXHAUSTED),
                     "ATTEMPTS_EXHAUSTED") == 0,
          "time sync strings are exact");
}

static void host_time_sync_resync(void)
{
    struct appliance_time_sync sync;
    struct appliance_time_sync_config config = host_config();
    uint8_t reply[APPLIANCE_TIME_SYNC_PACKET_LENGTH];

    memset(&sync, 0, sizeof(sync));
    CHECK(appliance_time_sync_init(&sync, &config) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_start(&sync, 1000U) ==
                  APPLIANCE_TIME_SYNC_OK,
          "the resync fixture starts");
    host_build_reply(&sync, test_epoch, 0U, reply);
    CHECK(appliance_time_sync_input(&sync, reply, sizeof(reply),
                                    1500U) ==
              APPLIANCE_TIME_SYNC_OK,
          "the first sample synchronizes");
    CHECK(appliance_time_sync_start(&sync, 5000U) ==
              APPLIANCE_TIME_SYNC_OK,
          "a synchronized client can resynchronize");
    host_build_reply(&sync, test_epoch + 10U, 0U, reply);
    CHECK(appliance_time_sync_input(&sync, reply, sizeof(reply),
                                    5500U) ==
              APPLIANCE_TIME_SYNC_OK &&
              appliance_time_sync_epoch_seconds(&sync, 5500U) ==
                  test_epoch + 10U,
          "a later sample advances the anchor");
}


static void host_time_wire_round_trip(void)
{
    struct appliance_time_wire_endpoint client;
    struct appliance_time_wire_endpoint server;
    uint8_t request[APPLIANCE_TIME_SYNC_PACKET_LENGTH];
    uint8_t frame[APPLIANCE_TIME_WIRE_FRAME_MAX];
    uint8_t parsed[APPLIANCE_TIME_SYNC_PACKET_LENGTH];
    size_t frame_length = 0U;
    size_t parsed_length = 0U;
    size_t index;

    memset(&client, 0, sizeof(client));
    memset(&server, 0, sizeof(server));
    memset(request, 0, sizeof(request));
    for (index = 0U; index < sizeof(request); ++index) {
        request[index] = (uint8_t)(index + 1U);
    }
    client.local_mac[0] = 0x52U;
    client.local_mac[5] = 0x11U;
    client.local_address[0] = 10U;
    client.local_address[3] = 15U;
    client.next_hop_mac[0] = 0x52U;
    client.next_hop_mac[5] = 0x22U;
    client.next_hop_address[0] = 10U;
    client.next_hop_address[3] = 2U;
    client.source_port = 50000U;
    client.destination_port = APPLIANCE_TIME_SYNC_PORT;
    server = client;
    memcpy(server.local_mac, client.next_hop_mac,
           APPLIANCE_TIME_WIRE_MAC_LENGTH);
    memcpy(server.local_address, client.next_hop_address,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    memcpy(server.next_hop_mac, client.local_mac,
           APPLIANCE_TIME_WIRE_MAC_LENGTH);
    memcpy(server.next_hop_address, client.local_address,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    server.source_port = APPLIANCE_TIME_SYNC_PORT;
    server.destination_port = client.source_port;

    CHECK(appliance_time_wire_build(
              NULL, request, sizeof(request), frame, sizeof(frame),
              &frame_length) == APPLIANCE_TIME_WIRE_NULL,
          "time wire build refuses a null endpoint");
    CHECK(appliance_time_wire_build(
              &client, request, sizeof(request), frame, 16U,
              &frame_length) == APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL,
          "time wire build refuses a short frame buffer");
    CHECK(appliance_time_wire_build(
              &client, request, sizeof(request), frame, sizeof(frame),
              &frame_length) == APPLIANCE_TIME_WIRE_OK &&
              frame_length == APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH +
                                  APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH +
                                  APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH +
                                  sizeof(request),
          "time wire build produces one bounded frame");
    CHECK(frame[12U] == 0x08U && frame[13U] == 0x00U &&
              frame[14U] == 0x45U && frame[23U] == 17U,
          "the frame carries an IPv4/UDP header");
    CHECK(memcmp(frame + 26U, client.local_address, 4U) == 0 &&
              memcmp(frame + 30U, client.next_hop_address, 4U) == 0,
          "the frame carries the client and server addresses");
    CHECK(frame[34U] == 0xC3U && frame[35U] == 0x50U &&
              frame[36U] == 0x00U && frame[37U] == 0x7BU,
          "the frame carries the UDP source and SNTP ports");
    CHECK(memcmp(frame + 14U + 20U + 8U, request,
                 sizeof(request)) == 0,
          "the SNTP payload sits after the IPv4 and UDP headers");

    CHECK(appliance_time_wire_parse(
              &server, frame, frame_length, parsed, sizeof(parsed),
              &parsed_length) == APPLIANCE_TIME_WIRE_OK &&
              parsed_length == sizeof(request) &&
              memcmp(parsed, request, sizeof(request)) == 0,
          "the server endpoint parses the request payload");
    CHECK(appliance_time_wire_parse(
              &client, frame, frame_length, parsed, sizeof(parsed),
              &parsed_length) == APPLIANCE_TIME_WIRE_BAD_FRAME,
          "the client endpoint refuses its own outbound frame");
    {
        uint8_t reply[APPLIANCE_TIME_WIRE_FRAME_MAX];
        size_t reply_length = 0U;

        CHECK(appliance_time_wire_build(
                  &server, request, sizeof(request), reply, sizeof(reply),
                  &reply_length) == APPLIANCE_TIME_WIRE_OK &&
                  appliance_time_wire_parse(
                      &client, reply, reply_length, parsed, sizeof(parsed),
                      &parsed_length) == APPLIANCE_TIME_WIRE_OK,
              "the client accepts a reply sourced from its selected server");
        reply[26U] = (uint8_t)(client.next_hop_address[0] + 1U);
        CHECK(appliance_time_wire_parse(
                  &client, reply, reply_length, parsed, sizeof(parsed),
                  &parsed_length) == APPLIANCE_TIME_WIRE_BAD_FRAME,
              "a reply from a different IPv4 source is refused");
        reply[26U] = client.next_hop_address[0];
    }
    {
        uint8_t broken[APPLIANCE_TIME_WIRE_FRAME_MAX];

        memcpy(broken, frame, frame_length);
        broken[12U] = 0x86U;
        CHECK(appliance_time_wire_parse(
                  &server, broken, frame_length, parsed, sizeof(parsed),
                  &parsed_length) == APPLIANCE_TIME_WIRE_BAD_FRAME,
              "a non-IPv4 frame is refused");
        memcpy(broken, frame, frame_length);
        broken[23U] = 6U;
        CHECK(appliance_time_wire_parse(
                  &server, broken, frame_length, parsed, sizeof(parsed),
                  &parsed_length) == APPLIANCE_TIME_WIRE_BAD_FRAME,
              "a non-UDP frame is refused");
        CHECK(appliance_time_wire_parse(
                  &server, frame, 20U, parsed, sizeof(parsed),
                  &parsed_length) == APPLIANCE_TIME_WIRE_BAD_FRAME,
              "a truncated frame is refused");
        CHECK(appliance_time_wire_parse(
                  &server, frame, frame_length, parsed, 8U,
                  &parsed_length) == APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL,
              "a short payload buffer is refused");
    }
    CHECK(strcmp(appliance_time_wire_status_string(
                     APPLIANCE_TIME_WIRE_BAD_FRAME),
                 "BAD_FRAME") == 0,
          "time wire status strings are exact");
}

int main(void)
{
    host_time_sync_contract();
    host_time_sync_request();
    host_time_sync_valid_reply();
    host_time_sync_bad_replies();
    host_time_sync_attempts();
    host_time_sync_resync();
    host_time_wire_round_trip();

    if (failures != 0) {
        fprintf(stderr, "Appliance time sync host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance time sync host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_time_sync_host_translation_unit_marker;
#endif
