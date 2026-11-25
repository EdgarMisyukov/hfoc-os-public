#include "appliance_dhcp_client.h"

#include "appliance_network_config.h"
#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_DHCP_CLIENT_HOST_TEST

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

static struct network_ipv4_config make_lease(void)
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

static struct appliance_network_config make_dhcp_desired(void)
{
    struct appliance_network_config config;

    memset(&config, 0, sizeof(config));
    if (appliance_network_config_publish_dhcp(
            &config, "eth0", 4U) != APPLIANCE_NETWORK_CONFIG_OK) {
        fprintf(stderr, "fixture failed to publish DHCP desired\n");
        __builtin_unreachable();
    }
    return config;
}

static struct appliance_network_config make_static_desired(void)
{
    struct appliance_network_config config;
    struct network_ipv4_config lease = make_lease();

    memset(&config, 0, sizeof(config));
    if (appliance_network_config_publish_static(
            &config, "eth0", 4U, &lease) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        fprintf(stderr, "fixture failed to publish static desired\n");
        __builtin_unreachable();
    }
    return config;
}

static void test_status_strings(void)
{
    enum appliance_dhcp_client_status status;
    enum appliance_dhcp_client_state state;

    for (status = APPLIANCE_DHCP_CLIENT_OK;
         status <= APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL;
         ++status) {
        const char *string =
            appliance_dhcp_client_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_DHCP_CLIENT_OK ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_OK") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_NULL ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_NULL") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_BAD_STATE") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_BAD_VERSION") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_BAD_CONFIG") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_BAD_LEASE ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_BAD_LEASE") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_STOPPED ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_STOPPED") == 0 :
                   status == APPLIANCE_DHCP_CLIENT_EXHAUSTED ?
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_EXHAUSTED") == 0 :
                       strcmp(string,
                              "APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL") == 0),
              "DHCP client status string is exact for every status");
    }
    CHECK(appliance_dhcp_client_status_string(
              (enum appliance_dhcp_client_status)9U) == NULL,
          "DHCP client rejects undefined status string");

    for (state = APPLIANCE_DHCP_CLIENT_STATE_NONE;
         state <= APPLIANCE_DHCP_CLIENT_STATE_FAILED;
         ++state) {
        const char *string =
            appliance_dhcp_client_state_string(state);

        CHECK(string != NULL &&
                  (state == APPLIANCE_DHCP_CLIENT_STATE_NONE ?
                       strcmp(string, "NONE") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_IDLE ?
                       strcmp(string, "IDLE") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING ?
                       strcmp(string, "DISCOVERING") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_REQUESTING ?
                       strcmp(string, "REQUESTING") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_BOUND ?
                       strcmp(string, "BOUND") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_RENEWING ?
                       strcmp(string, "RENEWING") == 0 :
                   state == APPLIANCE_DHCP_CLIENT_STATE_REBINDING ?
                       strcmp(string, "REBINDING") == 0 :
                       strcmp(string, "FAILED") == 0),
              "DHCP client state string is exact for every state");
    }
    CHECK(appliance_dhcp_client_state_string(
              (enum appliance_dhcp_client_state)8U) == NULL,
          "DHCP client rejects undefined state string");
}

static void test_null_and_invalid(void)
{
    struct appliance_dhcp_client client;
    struct appliance_network_config dhcp = make_dhcp_desired();
    struct appliance_network_config static_desired = make_static_desired();
    struct network_ipv4_config lease = make_lease();
    struct network_ipv4_config invalid;
    char output[APPLIANCE_DHCP_CLIENT_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&client, 0, sizeof(client));
    memset(&invalid, 0, sizeof(invalid));

    CHECK(appliance_dhcp_client_init(NULL, &dhcp) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client init rejects null client");
    CHECK(appliance_dhcp_client_init(&client, NULL) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client init rejects null desired config");
    CHECK(appliance_dhcp_client_init(
              &client, &static_desired) ==
              APPLIANCE_DHCP_CLIENT_BAD_CONFIG,
          "DHCP client rejects static desired mode");
    CHECK(appliance_dhcp_client_start(NULL) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client start rejects null client");
    CHECK(appliance_dhcp_client_start(&client) ==
              APPLIANCE_DHCP_CLIENT_BAD_STATE,
          "DHCP client start rejects uninitialized client");
    CHECK(appliance_dhcp_client_offer(
              NULL, &lease) == APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client offer rejects null client");
    CHECK(appliance_dhcp_client_ack(
              NULL, &lease) == APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client ack rejects null client");
    CHECK(appliance_dhcp_client_nak(NULL) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client nak rejects null client");
    CHECK(appliance_dhcp_client_link_down(NULL) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client link-down rejects null client");
    CHECK(appliance_dhcp_client_render(
              NULL, output, sizeof(output), &output_length) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client render rejects null client");
    CHECK(appliance_dhcp_client_render(
              &client, NULL, sizeof(output), &output_length) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client render rejects null output");
    CHECK(appliance_dhcp_client_render(
              &client, output, sizeof(output), NULL) ==
              APPLIANCE_DHCP_CLIENT_NULL,
          "DHCP client render rejects null output length");
    CHECK(appliance_dhcp_client_state(&client) ==
              APPLIANCE_DHCP_CLIENT_STATE_NONE,
          "uninitialized DHCP client state is none");
    CHECK(appliance_dhcp_client_lease(&client) == NULL,
          "uninitialized DHCP client lease accessor is null");
    CHECK(!appliance_dhcp_client_lease_published(&client),
          "uninitialized DHCP client lease is unpublished");
}

static void test_lifecycle_and_render(void)
{
    struct appliance_dhcp_client client;
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_ipv4_config lease = make_lease();
    struct network_ipv4_config invalid;
    char output[APPLIANCE_DHCP_CLIENT_OUTPUT_MAX + 1U];
    char tiny[16U];
    char tiny_original[16U];
    char exact_no_nul[117U];
    char exact_original[117U];
    size_t output_length = 0U;

    memset(&client, 0, sizeof(client));
    memset(&invalid, 0, sizeof(invalid));
    memset(tiny, 0x55, sizeof(tiny));
    memcpy(tiny_original, tiny, sizeof(tiny_original));
    memset(exact_no_nul, 0x5a, sizeof(exact_no_nul));
    memcpy(exact_original, exact_no_nul, sizeof(exact_original));
    CHECK(appliance_dhcp_client_init(&client, &desired) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.initialized &&
              client.version == APPLIANCE_DHCP_CLIENT_VERSION &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_IDLE &&
              client.max_attempts == APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS,
          "DHCP client init publishes DHCP desired");
    CHECK(appliance_dhcp_client_start(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING &&
              client.attempt_count == 1U,
          "DHCP client start enters discovery");
    CHECK(appliance_dhcp_client_offer(&client, &invalid) ==
              APPLIANCE_DHCP_CLIENT_BAD_LEASE &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING,
          "DHCP client rejects invalid offer without mutation");
    CHECK(appliance_dhcp_client_offer(&client, &lease) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_REQUESTING,
          "DHCP client offer enters request");
    CHECK(appliance_dhcp_client_ack(&client, &invalid) ==
              APPLIANCE_DHCP_CLIENT_BAD_LEASE &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_REQUESTING &&
              !client.lease_published,
          "DHCP client rejects invalid ack without lease publication");
    CHECK(appliance_dhcp_client_ack(&client, &lease) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_BOUND &&
              client.lease_published &&
              appliance_dhcp_client_lease(&client) != NULL &&
              memcmp(&client.lease, &lease,
                     sizeof(client.lease)) == 0,
          "DHCP client ack publishes validated lease");

    output_length = 0U;
    CHECK(appliance_dhcp_client_render(
              &client, output, sizeof(output), &output_length) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              output_length == 117U &&
              output[output_length] == '\0' &&
              memcmp(output,
                     "dhcp_client v=1 interface=eth0 state=BOUND "
                     "lease=published address=10.0.2.15 prefix=24 "
                     "gateway=10.0.2.2 dns=10.0.2.3\n",
                    117U) == 0,
          "DHCP client canonical bound render is exact");

    output_length = 77U;
    CHECK(appliance_dhcp_client_render(
              &client, tiny, sizeof(tiny), &output_length) ==
              APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL &&
              output_length == 77U &&
              memcmp(tiny, tiny_original, sizeof(tiny)) == 0,
          "DHCP client render overflow preserves caller buffer");
    output_length = 77U;
    CHECK(appliance_dhcp_client_render(
              &client, exact_no_nul, sizeof(exact_no_nul),
              &output_length) ==
              APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL &&
              output_length == 77U &&
              memcmp(exact_no_nul, exact_original,
                     sizeof(exact_no_nul)) == 0,
          "DHCP client render requires terminator capacity");

    CHECK(appliance_dhcp_client_renew(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_RENEWING,
          "DHCP client bound lease starts renewal");
    CHECK(appliance_dhcp_client_renew_timeout(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_REBINDING,
          "DHCP client renew timeout enters rebinding");
    CHECK(appliance_dhcp_client_ack(&client, &lease) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_BOUND &&
              client.lease_published,
          "DHCP client rebinding ack restores bound lease");
    CHECK(appliance_dhcp_client_link_down(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_IDLE &&
              !client.lease_published,
          "DHCP client link down clears lease");

    CHECK(appliance_dhcp_client_reset(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              !client.initialized,
          "DHCP client reset returns to unpublished state");
}

static void test_retry_failure(void)
{
    struct appliance_dhcp_client client;
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_ipv4_config lease = make_lease();

    /*
     * ITEM-416: the acquisition keeps asking inside its budget, and the budget
     * is what bounds it.
     *
     * This used to be two timeouts and then `EXHAUSTED` - two seconds of
     * patience, which the acceptance host spent behind a switch port that was
     * forwarding nothing yet and then reported a failed acquisition.  The
     * budget is forty attempts now, unaddressed state is a legitimate state to
     * wait in, and the failure belongs at the end of the budget rather than a
     * second and a half into the boot.
     */
    memset(&client, 0, sizeof(client));
    CHECK(appliance_dhcp_client_init(&client, &desired) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              appliance_dhcp_client_start(&client) ==
                  APPLIANCE_DHCP_CLIENT_OK &&
              appliance_dhcp_client_attempt_budget(&client) ==
                  APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS &&
              appliance_dhcp_client_attempt_count(&client) == 1U,
          "a started client states the attempt it is on and its budget");
    CHECK(appliance_dhcp_client_discover_timeout(&client) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING &&
              appliance_dhcp_client_attempt_count(&client) == 2U,
          "a discovery timeout inside the budget keeps asking");
    {
        uint32_t attempt;

        for (attempt = appliance_dhcp_client_attempt_count(&client);
             attempt + 1U < APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS;
             ++attempt) {
            if (appliance_dhcp_client_discover_timeout(&client) !=
                APPLIANCE_DHCP_CLIENT_OK) {
                CHECK(false, "the budget ended the acquisition early");
                break;
            }
        }
    }
    CHECK(appliance_dhcp_client_attempt_count(&client) ==
              APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS - 1U &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING,
          "the client keeps asking until the end of its budget");
    CHECK(appliance_dhcp_client_discover_timeout(&client) ==
              APPLIANCE_DHCP_CLIENT_EXHAUSTED &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_FAILED,
          "the budget is what ends the acquisition");

    memset(&client, 0, sizeof(client));
    CHECK(appliance_dhcp_client_init(&client, &desired) ==
              APPLIANCE_DHCP_CLIENT_OK &&
              appliance_dhcp_client_start(&client) ==
                  APPLIANCE_DHCP_CLIENT_OK &&
              appliance_dhcp_client_offer(
                  &client, &lease) ==
                  APPLIANCE_DHCP_CLIENT_OK &&
              appliance_dhcp_client_nak(&client) ==
                  APPLIANCE_DHCP_CLIENT_OK &&
              client.state == APPLIANCE_DHCP_CLIENT_STATE_IDLE &&
              client.attempt_count == 2U,
          "DHCP client negative acknowledgement returns to idle bounded");
}

int main(void)
{
    test_status_strings();
    test_null_and_invalid();
    test_lifecycle_and_render();
    test_retry_failure();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr, "FAIL: DHCP client performed heap allocation\n");
        ++failures;
    }
    if (failures != 0) {
        fprintf(stderr, "DHCP client host test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("DHCP client host test: OK\n");
    return 0;
}

#endif
