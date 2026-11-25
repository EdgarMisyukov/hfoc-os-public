#include "appliance_network_lifecycle.h"

#include "appliance_network_config.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_NETWORK_LIFECYCLE_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

struct lifecycle_transport_state {
    unsigned int activate_calls;
    unsigned int deactivate_calls;
    unsigned int discover_calls;
    unsigned int request_calls;
    unsigned int renew_calls;
    unsigned int rebind_calls;
    unsigned int dns_query_calls;
};

static bool transport_activate(
    void *context,
    const struct network_ipv4_config *active)
{
    struct lifecycle_transport_state *state = context;

    (void)active;
    ++state->activate_calls;
    return true;
}

static bool transport_deactivate(void *context)
{
    struct lifecycle_transport_state *state = context;

    ++state->deactivate_calls;
    return true;
}

static bool transport_discover(void *context)
{
    struct lifecycle_transport_state *state = context;

    ++state->discover_calls;
    return true;
}

static bool transport_request(void *context)
{
    struct lifecycle_transport_state *state = context;

    ++state->request_calls;
    return true;
}

static bool transport_renew(void *context)
{
    struct lifecycle_transport_state *state = context;

    ++state->renew_calls;
    return true;
}

static bool transport_rebind(void *context)
{
    struct lifecycle_transport_state *state = context;

    ++state->rebind_calls;
    return true;
}

static bool transport_dns_query(
    void *context,
    const char *hostname,
    size_t hostname_length)
{
    struct lifecycle_transport_state *state = context;

    (void)hostname;
    (void)hostname_length;
    ++state->dns_query_calls;
    return true;
}

static struct appliance_network_lifecycle_transport make_transport(
    struct lifecycle_transport_state *state)
{
    struct appliance_network_lifecycle_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    transport.context = state;
    transport.activate_ipv4 = transport_activate;
    transport.deactivate_ipv4 = transport_deactivate;
    transport.dhcp_discover = transport_discover;
    transport.dhcp_request = transport_request;
    transport.dhcp_renew = transport_renew;
    transport.dhcp_rebind = transport_rebind;
    transport.dns_query = transport_dns_query;
    return transport;
}

static struct network_ipv4_config make_ipv4(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static struct appliance_network_config make_static_desired(void)
{
    struct appliance_network_config desired;
    struct network_ipv4_config config = make_ipv4();

    memset(&desired, 0, sizeof(desired));
    CHECK(appliance_network_config_publish_static(
              &desired, "eth0", 4U, &config) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "static desired fixture");
    return desired;
}

static struct appliance_network_config make_dhcp_desired(void)
{
    struct appliance_network_config desired;

    memset(&desired, 0, sizeof(desired));
    CHECK(appliance_network_config_publish_dhcp(
              &desired, "eth0", 4U) ==
              APPLIANCE_NETWORK_CONFIG_OK,
          "DHCP desired fixture");
    return desired;
}

static struct network_device_snapshot make_device(void)
{
    struct network_device_snapshot device;

    memset(&device, 0, sizeof(device));
    device.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device.device_id = 1U;
    device.vendor_id = 0x1234U;
    device.product_id = 0x5678U;
    device.bus_address = 0U;
    device.capability_mask = NETWORK_DEVICE_CAPABILITY_MASK;
    device.mac_address[0] = 0x02U;
    device.mac_address[5] = 0x01U;
    return device;
}

static struct network_link_state_record make_link_generation(
    enum network_link_state state,
    uint32_t generation)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = state;
    link.generation = generation;
    return link;
}

static struct network_link_state_record make_link(
    enum network_link_state state)
{
    return make_link_generation(state, 7U);
}

static bool ipv4_equal(
    const struct network_ipv4_config *left,
    const struct network_ipv4_config *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static void test_static_lifecycle(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_static_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    const struct network_ipv4_config *active;
    char output[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_PREPARED,
          "static lifecycle init");
    CHECK(appliance_network_lifecycle_start(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_STARTED,
          "static lifecycle start");
    CHECK(appliance_network_lifecycle_link(
              &lifecycle, &link) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              transport_state.activate_calls == 1U,
          "static link activates desired config");
    active = appliance_network_lifecycle_active_config(&lifecycle);
    CHECK(ipv4_equal(active, &desired.static_config),
          "static active config matches desired");
    CHECK(appliance_network_lifecycle_reachability(
              &lifecycle,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK,
          "reachability publication");
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              strstr(output,
                     "state=ACTIVE interface=eth0 mode=STATIC "
                     "device=1 link=UP active=yes address=10.0.2.15 "
                     "prefix=24 dhcp=OFF dns=READY health=PASS") != NULL,
          "static lifecycle canonical render");

    link.state = NETWORK_LINK_DOWN;
    CHECK(appliance_network_lifecycle_link(
              &lifecycle, &link) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              !appliance_network_lifecycle_active(&lifecycle) &&
              transport_state.deactivate_calls == 1U &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK,
          "link loss clears active config");
    CHECK(appliance_network_lifecycle_stop(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_STOPPED,
          "lifecycle stop");
}

static void test_dhcp_lifecycle(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_dhcp_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config lease = make_ipv4();
    const struct appliance_dhcp_client *dhcp;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK,
          "DHCP lifecycle init and start");
    CHECK(appliance_network_lifecycle_link(
              &lifecycle, &link) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.discover_calls == 1U &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING,
          "DHCP link starts discovery");
    CHECK(appliance_network_lifecycle_dhcp_offer(
              &lifecycle, &lease) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.request_calls == 1U,
          "DHCP offer requests lease");
    CHECK(appliance_network_lifecycle_dhcp_ack(
              &lifecycle, &lease) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              transport_state.activate_calls == 1U,
          "DHCP acknowledgement activates lease");
    dhcp = appliance_network_lifecycle_dhcp(&lifecycle);
    CHECK(dhcp != NULL &&
              appliance_dhcp_client_lease_published(dhcp),
          "DHCP lease published through lifecycle");
    CHECK(appliance_network_lifecycle_dns_query(
              &lifecycle, "api.hfoc.test", 13U) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.dns_query_calls == 1U,
          "DNS query reaches transport");
    CHECK(appliance_network_lifecycle_dns_response(
              &lifecycle, &lease.dns) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_dns_resolver_answer_published(
                  appliance_network_lifecycle_dns(&lifecycle)),
          "DNS answer publication");
}

static void test_dhcp_renewal_failures(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_dhcp_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config lease = make_ipv4();
    char output[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_offer(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_ack(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "DHCP renewal fixture reaches active");
    CHECK(appliance_network_lifecycle_reachability(
              &lifecycle,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_render(
                  &lifecycle, output, sizeof(output),
                  &output_length) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              strstr(output, " health=PASS") != NULL,
          "DHCP renewal fixture publishes healthy diagnostics");
    CHECK(appliance_network_lifecycle_dhcp_renew(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.renew_calls == 1U &&
              appliance_network_lifecycle_dhcp_nak(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              !appliance_network_lifecycle_active(&lifecycle) &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
              transport_state.deactivate_calls == 1U &&
              transport_state.discover_calls == 2U &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_REFUSED,
          "DHCP NAK clears active lease and restarts discovery");
    output_length = 0U;
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              strstr(output, "state=ADDRESSING") != NULL &&
              strstr(output, "active=no address=0.0.0.0") != NULL &&
              strstr(output, " health=WARN") != NULL &&
              strstr(output, " reason=DHCP_REFUSED hold=0") != NULL,
          "DHCP NAK republishes address warning diagnostics");
    CHECK(appliance_network_lifecycle_dhcp_offer(
              &lifecycle, &lease) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.request_calls == 2U &&
              appliance_network_lifecycle_dhcp_ack(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              transport_state.activate_calls == 2U &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE,
          "DHCP lifecycle recovers after NAK");
    /*
     * ITEM-408: the renewal cycle that runs out of attempts is a transient,
     * not a reason to stop serving.  The address stays applied (the transport
     * is never deactivated), the reason names the timeout that ended the
     * cycle, the survived-failure count is published, and the appliance
     * re-enters addressing with a fresh cycle so the next server that answers
     * puts it back in ACTIVE.
     */
    CHECK(appliance_network_lifecycle_reachability(
              &lifecycle,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
              APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_renew(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.renew_calls == 2U &&
              appliance_network_lifecycle_dhcp_timeout(
                  &lifecycle,
                  APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.rebind_calls == 1U &&
              appliance_network_lifecycle_dhcp_timeout(
                  &lifecycle,
                  APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND) ==
                  APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED &&
              appliance_network_lifecycle_active(&lifecycle) &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
              transport_state.deactivate_calls == 1U &&
              transport_state.discover_calls == 3U &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_REBIND_TIMEOUT &&
              appliance_network_lifecycle_hold_count(&lifecycle) == 1U,
          "DHCP renewal exhaustion holds the live address");
    CHECK(ipv4_equal(appliance_network_lifecycle_active_config(&lifecycle),
                     &lease),
          "DHCP renewal exhaustion keeps the held address applied");
    output_length = 0U;
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              strstr(output,
                     "state=ADDRESSING interface=eth0 mode=DHCP "
                     "device=1 link=UP active=yes address=10.0.2.15 "
                     "prefix=24") != NULL &&
              strstr(output, " reason=REBIND_TIMEOUT hold=1") != NULL,
          "DHCP renewal exhaustion names the timeout in the record");
    CHECK(appliance_network_lifecycle_dhcp_offer(
              &lifecycle, &lease) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_ack(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_lifecycle_hold_count(&lifecycle) == 0U &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE,
          "DHCP lifecycle returns to active after the hold");
}

/*
 * ITEM-408: a renewal that cannot even be transmitted is a transient too, and
 * the record names it as one instead of dropping the address.
 */
static void test_renewal_transport_failure(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_dhcp_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config lease = make_ipv4();

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_offer(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dhcp_ack(
                  &lifecycle, &lease) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK,
          "renewal transport fixture reaches active");
    transport.dhcp_renew = NULL;
    lifecycle.transport = transport;
    CHECK(appliance_network_lifecycle_dhcp_renew(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE &&
              appliance_network_lifecycle_active(&lifecycle) &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT &&
              appliance_network_lifecycle_hold_count(&lifecycle) == 1U,
          "an unsendable renewal holds the live address");
}

static void test_dns_exhaustion_holds_address(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_static_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_ipv4_config lease = make_ipv4();
    char output[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    unsigned int deactivate_baseline;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_query(
                  &lifecycle, "api.hfoc.test", 13U) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_timeout(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_timeout(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED &&
              appliance_network_lifecycle_active(&lifecycle) &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              transport_state.deactivate_calls == 0U &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT &&
              appliance_network_lifecycle_hold_count(&lifecycle) == 1U,
          "DNS timeout exhaustion holds the live address");
    output_length = 0U;
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, output, sizeof(output), &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              strstr(output, "active=yes address=10.0.2.15") != NULL &&
              strstr(output, " dns=FAILED") != NULL &&
              strstr(output, " health=FAIL") != NULL &&
              strstr(output, " reason=DNS_PROBE_TIMEOUT hold=1") != NULL,
          "DNS exhaustion publishes an unreachable probe, not a lost address");
    CHECK(appliance_network_lifecycle_dns_probe_retry(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_query(
                  &lifecycle, "api.hfoc.test", 13U) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              transport_state.dns_query_calls == 2U &&
              appliance_network_lifecycle_dns_response(
                  &lifecycle, &lease.dns) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE &&
              appliance_network_lifecycle_active(&lifecycle),
          "a retried DNS probe resolves without touching the address");
    deactivate_baseline = transport_state.deactivate_calls;
    CHECK(appliance_network_lifecycle_reset(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK,
          "DNS probe fixture resets");
    deactivate_baseline = transport_state.deactivate_calls;
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_query(
                  &lifecycle, "api.hfoc.test", 13U) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_server_failure(
                  &lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_dns_server_failure(
                  &lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_EXHAUSTED &&
              appliance_network_lifecycle_active(&lifecycle) &&
              transport_state.deactivate_calls == deactivate_baseline &&
              appliance_network_lifecycle_reason(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_REFUSED,
          "DNS server failure exhaustion holds the live address");
}

static void test_stale_link_rejected(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_static_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct network_link_state_record stale =
        make_link_generation(NETWORK_LINK_UP, 6U);

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &stale) ==
                  APPLIANCE_NETWORK_LIFECYCLE_BAD_LINK &&
              appliance_network_lifecycle_active(&lifecycle) &&
              appliance_network_lifecycle_state(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              transport_state.activate_calls == 1U,
          "stale link generation is rejected without mutating active state");
}

static void test_render_boundaries(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_static_desired();
    struct network_device_snapshot device = make_device();
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    char full_output[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    char caller[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    char caller_original[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    char exact[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    char exact_original[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    size_t full_length;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_render(
                  &lifecycle, full_output, sizeof(full_output),
                  &output_length) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              output_length > 16U &&
              strstr(full_output,
                     "state=ACTIVE interface=eth0 mode=STATIC") != NULL,
          "render boundary full record");
    full_length = output_length;

    memset(caller, 0x55, sizeof(caller));
    memcpy(caller_original, caller, sizeof(caller_original));
    output_length = 77U;
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, caller, 32U, &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL &&
              output_length == 77U &&
              memcmp(caller, caller_original,
                     sizeof(caller_original)) == 0,
          "lifecycle render overflow preserves caller buffer");

    memset(exact, 0x5a, sizeof(exact));
    memcpy(exact_original, exact, sizeof(exact_original));
    output_length = 99U;
    CHECK(appliance_network_lifecycle_render(
              &lifecycle, exact, full_length, &output_length) ==
              APPLIANCE_NETWORK_LIFECYCLE_BUFFER_TOO_SMALL &&
              output_length == 99U &&
              memcmp(exact, exact_original,
                     sizeof(exact_original)) == 0,
          "lifecycle render requires terminator capacity");
}

static void test_invalid_and_no_transport(void)
{
    struct appliance_network_lifecycle lifecycle;
    struct appliance_network_config desired =
        make_static_desired();
    struct network_device_snapshot device = make_device();
    struct network_link_state_record link = make_link(NETWORK_LINK_UP);
    struct lifecycle_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;

    memset(&transport_state, 0, sizeof(transport_state));
    transport = make_transport(&transport_state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              NULL, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_NULL,
          "null lifecycle rejected");
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, &transport) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_init(
                  &lifecycle, &desired, &device, &transport) ==
                  APPLIANCE_NETWORK_LIFECYCLE_BAD_STATE,
          "duplicate init rejected");
    CHECK(appliance_network_lifecycle_start(&lifecycle) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK,
          "start valid lifecycle");
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(appliance_network_lifecycle_init(
              &lifecycle, &desired, &device, NULL) ==
              APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_start(&lifecycle) ==
                  APPLIANCE_NETWORK_LIFECYCLE_OK &&
              appliance_network_lifecycle_link(
                  &lifecycle, &link) ==
                  APPLIANCE_NETWORK_LIFECYCLE_TRANSPORT_UNAVAILABLE,
          "no transport fails activation");
}

int main(void)
{
    test_static_lifecycle();
    test_dhcp_lifecycle();
    test_dhcp_renewal_failures();
    test_renewal_transport_failure();
    test_dns_exhaustion_holds_address();
    test_stale_link_rejected();
    test_render_boundaries();
    test_invalid_and_no_transport();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance network lifecycle host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Appliance network lifecycle host tests allocated memory\n");
        return 1;
    }
    printf("Appliance network lifecycle host tests: ALL OK\n");
    return 0;
}

#else
int hfoc_appliance_network_lifecycle_host_translation_unit_guard;
#endif
