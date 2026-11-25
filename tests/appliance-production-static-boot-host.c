/*
 * ITEM-600: the STATIC configuration after reboot is activated only when the
 * interface it names is really live.
 *
 * The owner set STATIC 192.0.2.13 from the menu, rebooted, and the machine
 * held no address: `[NET-ADDR] state=FAILED lease=NONE reason=
 * ADDRESS_ACTIVATION`.  The production wire published `NETWORK_LINK_UP` at
 * boot unconditionally, so the lifecycle tried the saved STATIC address while
 * the Realtek bind had only *staged* the netif (the PHY still negotiating);
 * `hfoc_lwip_netif_set_config` answered NOT_LIVE, and the lifecycle turned
 * that boot ordering into a terminal FAILED state.  The later, successful
 * bind made the netif live but nothing re-tried the address.
 *
 * This proof drives the real production wire translation unit and the real
 * hfoc_lwip_netif set_config: the first boot link is reported unready, then
 * the composition becomes ready and the paced link sample must apply the
 * saved STATIC configuration.  It must fail before the fix because the boot
 * link record says UP and the address is never acquired.
 */
#include "appliance_network_config.h"
#include "appliance_network_lifecycle.h"
#include "appliance_network_wire.h"
#include "appliance_production_network.h"
#include "appliance_production_network_wire.h"
#include "appliance_time_sync.h"
#include "appliance_time_wire.h"
#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_proto.h"
#include "hfoc_lwip_time.h"
#include "ipv4_config.h"
#include "network_device.h"

#include "lwip/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_NETWORK_WIRE_X86_HOST_TEST

static int failures;
static struct appliance_production_network fake_network;
static bool fake_device_present;
static bool fake_ready;
static bool fake_link_up;
static uint32_t fake_now_ms;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* --- production network composition stubs ---------------------------- */

struct appliance_production_network *
appliance_production_network_boot_network(void)
{
    return &fake_network;
}

bool appliance_production_network_boot_device_present(void)
{
    return fake_device_present;
}

bool appliance_production_network_bound(
    const struct appliance_production_network *network)
{
    return network != NULL && network->initialized && network->bound;
}

const struct network_device_snapshot *
appliance_production_network_device(
    const struct appliance_production_network *network)
{
    return network != NULL && network->bound
        ? &network->device_snapshot
        : NULL;
}

bool appliance_production_network_ready(
    const struct appliance_production_network *network)
{
    (void)network;
    return fake_ready;
}

bool appliance_production_network_boot_link_up(void)
{
    return fake_link_up;
}

/* --- lwIP / time stubs the wire translation unit references ---------- */

uint32_t hfoc_lwip_time_now(void)
{
    return fake_now_ms;
}

enum hfoc_lwip_proto_status hfoc_lwip_proto_lookup(
    const struct hfoc_lwip_proto *proto,
    const ip4_addr_t *address,
    struct eth_addr *mac)
{
    (void)proto;
    (void)address;
    (void)mac;
    return HFOC_LWIP_PROTO_UNPUBLISHED;
}

err_t etharp_request(struct netif *netif, const ip4_addr_t *ipaddr)
{
    (void)netif;
    (void)ipaddr;
    return ERR_OK;
}

enum appliance_time_wire_status appliance_time_wire_build(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length_out)
{
    (void)endpoint;
    (void)payload;
    (void)payload_length;
    (void)frame;
    (void)frame_capacity;
    (void)frame_length_out;
    return APPLIANCE_TIME_WIRE_NULL;
}

enum appliance_time_wire_status appliance_time_wire_parse(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *frame,
    size_t frame_length,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_length_out)
{
    (void)endpoint;
    (void)frame;
    (void)frame_length;
    (void)payload;
    (void)payload_capacity;
    (void)payload_length_out;
    return APPLIANCE_TIME_WIRE_NULL;
}

/* --- fixtures -------------------------------------------------------- */

static void reset_fake_network(void)
{
    memset(&fake_network, 0, sizeof(fake_network));
    fake_network.initialized = true;
    fake_network.bound = true;
    fake_network.readiness = APPLIANCE_PRODUCTION_NETWORK_DEGRADED;
    fake_network.device_snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    fake_network.device_snapshot.device_id = 1U;
    fake_network.device_snapshot.vendor_id = 0x10ECU;
    fake_network.device_snapshot.product_id = 0x8168U;
    fake_network.device_snapshot.bus_address = 0x0100U;
    fake_network.device_snapshot.mac_address[0] = 0x52U;
    fake_network.device_snapshot.mac_address[1] = 0x54U;
    fake_network.device_snapshot.mac_address[2] = 0x00U;
    fake_network.device_snapshot.mac_address[3] = 0x12U;
    fake_network.device_snapshot.mac_address[4] = 0x34U;
    fake_network.device_snapshot.mac_address[5] = 0x56U;
    fake_network.device_snapshot.capability_mask =
        NETWORK_DEVICE_CAPABILITY_RX |
        NETWORK_DEVICE_CAPABILITY_TX |
        NETWORK_DEVICE_CAPABILITY_LINK_STATE;
    fake_network.netif.published = true;
    fake_network.netif.version = HFOC_LWIP_NETIF_VERSION;
    fake_network.netif.generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    fake_network.netif.live = false;
    fake_network.proto.netif.published = true;
    fake_network.proto.netif.version = HFOC_LWIP_NETIF_VERSION;
    fake_network.proto.netif.generation =
        APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    fake_network.proto.netif.live = false;
    fake_device_present = true;
    fake_ready = false;
    fake_link_up = true;
    fake_now_ms = 0U;
}

static void make_network_ready(void)
{
    fake_ready = true;
    fake_link_up = true;
    fake_network.readiness = APPLIANCE_PRODUCTION_NETWORK_READY;
    fake_network.netif.live = true;
    fake_network.proto.netif.live = true;
}

static struct appliance_network_config make_static_desired(void)
{
    struct appliance_network_config desired;
    struct network_ipv4_config config;

    memset(&desired, 0, sizeof(desired));
    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 192U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 13U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 192U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 1U;
    config.dns.octets[0] = 8U;
    config.dns.octets[1] = 8U;
    config.dns.octets[2] = 8U;
    config.dns.octets[3] = 8U;
    (void)appliance_network_config_publish_static(
        &desired, "eth0", 4U, &config);
    return desired;
}

static bool address_is(
    const struct network_ipv4_address *address,
    uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return address->octets[0] == a && address->octets[1] == b &&
           address->octets[2] == c && address->octets[3] == d;
}

/* --- the proof ------------------------------------------------------- */

static void test_static_boot_waits_for_live_netif(void)
{
    struct appliance_network_config desired = make_static_desired();
    const struct appliance_network_wire *wire;
    const struct network_ipv4_config *active;

    reset_fake_network();
    CHECK(appliance_production_network_wire_boot_init(&desired, NULL) ==
              APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED,
          "a boot whose composition is not ready publishes DEGRADED");
    wire = appliance_production_network_wire_boot_wire();
    CHECK(wire != NULL &&
              appliance_network_wire_state(wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK,
          "the boot link is DOWN until the interface carrying STATIC is live");
    CHECK(wire != NULL &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE,
          "waiting for a live interface is not an address failure");

    make_network_ready();
    fake_now_ms = 1000U;
    appliance_production_network_wire_boot_poll(fake_now_ms);
    wire = appliance_production_network_wire_boot_wire();
    active = wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    CHECK(wire != NULL &&
              appliance_network_wire_state(wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "the saved STATIC configuration is applied once the netif is live");
    CHECK(active != NULL &&
              address_is(&active->address, 192U, 0U, 2U, 13U) &&
              active->prefix_length == 24U &&
              address_is(&active->gateway, 192U, 0U, 2U, 1U) &&
              address_is(&active->dns, 8U, 8U, 8U, 8U),
          "the active configuration is the operator's literal STATIC answer");
    CHECK(fake_network.netif.live &&
              address_is(&fake_network.netif.config.address,
                         192U, 0U, 2U, 13U),
          "the production netif holds the activated address");
}

/*
 * A link that comes up before the transport is usable must not turn into a
 * terminal failure: the first sample is not enough, the ready sample is what
 * applies the address.  This is the same ordering the Toshiba hit, with the
 * readiness bit made explicit instead of inferred from a register.
 */
static void test_static_retries_after_transport_becomes_live(void)
{
    struct appliance_network_config desired = make_static_desired();
    const struct appliance_network_wire *wire;

    reset_fake_network();
    CHECK(appliance_production_network_wire_boot_init(&desired, NULL) ==
              APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED,
          "unready boot starts in the waiting state");
    wire = appliance_production_network_wire_boot_wire();
    CHECK(wire != NULL &&
              appliance_network_wire_state(wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK,
          "no address activation is attempted against a staged netif");

    /* The device reports link up, but the composition is still not ready:
       the wire must keep waiting, not claim a link it cannot use. */
    fake_link_up = true;
    fake_now_ms = 1000U;
    appliance_production_network_wire_boot_poll(fake_now_ms);
    CHECK(wire != NULL &&
              appliance_network_wire_state(wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK,
          "a raw link bit alone does not activate the address");

    make_network_ready();
    fake_now_ms = 2000U;
    appliance_production_network_wire_boot_poll(fake_now_ms);
    CHECK(wire != NULL &&
              appliance_network_wire_state(wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_wire_active_config(wire) != NULL,
          "the ready link applies the saved address without a reboot");
}

static void test_explicit_sntp_source(void)
{
    struct appliance_network_config desired = make_static_desired();
    struct appliance_production_network_time_config time;
    struct network_ipv4_address server;
    enum appliance_production_network_time_source source =
        APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE;

    memset(&time, 0, sizeof(time));
    time.declared = true;
    time.valid = true;
    time.server.octets[0] = 192U;
    time.server.octets[1] = 0U;
    time.server.octets[2] = 2U;
    time.server.octets[3] = 7U;
    reset_fake_network();
    make_network_ready();
    fake_now_ms = 1000U;
    (void)appliance_production_network_wire_boot_init(&desired, &time);
    appliance_production_network_wire_boot_poll(fake_now_ms);
    appliance_production_network_wire_boot_poll(fake_now_ms + 100U);
    CHECK(appliance_production_network_time_server(&server, &source) &&
              source ==
                  APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_EXPLICIT &&
              address_is(&server, 192U, 0U, 2U, 7U),
          "the explicit SNTP source is selected and published");

    memset(&time, 0, sizeof(time));
    time.declared = true;
    time.valid = false;
    reset_fake_network();
    make_network_ready();
    fake_now_ms = 2000U;
    (void)appliance_production_network_wire_boot_init(&desired, &time);
    appliance_production_network_wire_boot_poll(fake_now_ms);
    appliance_production_network_wire_boot_poll(fake_now_ms + 100U);
    CHECK(!appliance_production_network_time_server(&server, &source) &&
              source ==
                  APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID &&
              appliance_production_network_time_state() ==
                  APPLIANCE_TIME_SYNC_STATE_UNSYNCHRONIZED &&
              appliance_production_network_time_status() ==
                  APPLIANCE_TIME_SYNC_BAD_CONFIG,
          "an invalid SNTP source is visible and never falls back");
}

int main(void)
{
    test_static_boot_waits_for_live_netif();
    test_static_retries_after_transport_becomes_live();
    test_explicit_sntp_source();
    if (failures != 0) {
        fprintf(stderr,
                "Production STATIC boot host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("Production STATIC boot host tests: ALL OK\n");
    return 0;
}

#else
typedef int hfoc_production_static_boot_host_translation_unit_guard;
#endif
