#include "appliance_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_CONSOLE_HOST_TEST

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

static struct appliance_identity make_identity(void)
{
    struct appliance_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = APPLIANCE_IDENTITY_VERSION;
    memcpy(identity.machine_class, APPLIANCE_IDENTITY_MACHINE_CLASS,
           sizeof(APPLIANCE_IDENTITY_MACHINE_CLASS));
    memcpy(identity.product, APPLIANCE_IDENTITY_PRODUCT,
           sizeof(APPLIANCE_IDENTITY_PRODUCT));
    memcpy(identity.architecture, APPLIANCE_IDENTITY_ARCHITECTURE,
           sizeof(APPLIANCE_IDENTITY_ARCHITECTURE));
    memcpy(identity.platform, APPLIANCE_IDENTITY_PLATFORM,
           sizeof(APPLIANCE_IDENTITY_PLATFORM));
    memcpy(identity.hostname, APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_HOSTNAME));
    memcpy(identity.instance, APPLIANCE_IDENTITY_DEFAULT_INSTANCE,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_INSTANCE));
    identity.published = true;
    return identity;
}

static const char *unavailable_output(void)
{
    return "appliance v=1\n"
           "identity product=HFOC-APPLIANCE-V1 arch=x86_64 "
           "platform=q35 hostname=hfoc-appliance\n"
           "config state=UNAVAILABLE source=NONE\n"
           "health state=OK uptime_ticks=42\n"
           "network state=UNAVAILABLE\n"
           "services state=UNAVAILABLE\n";
}

static const char *full_output(void)
{
    return "appliance v=1\n"
           "identity product=HFOC-APPLIANCE-V1 arch=x86_64 "
           "platform=q35 hostname=hfoc-appliance\n"
           "config state=OK source=COMMITTED hostname=node-305\n"
           "health state=DEGRADED uptime_ticks=12345\n"
           "network state=OK generation=7 devices=1\n"
           "services state=DEGRADED services=2 ready=1 failed=1\n";
}

static struct appliance_console_snapshot make_base_snapshot(void)
{
    struct appliance_console_snapshot snapshot;
    struct appliance_identity identity = make_identity();

    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_console_snapshot_init(
            &snapshot, &identity,
            APPLIANCE_CONSOLE_STATE_OK, 42U) != APPLIANCE_CONSOLE_OK) {
        fprintf(stderr, "fixture failed to init snapshot\n");
        __builtin_unreachable();
    }
    return snapshot;
}

static void test_strings(void)
{
    enum appliance_console_state state;
    enum appliance_console_status status;

    for (status = APPLIANCE_CONSOLE_OK;
         status <= APPLIANCE_CONSOLE_OVERFLOW; ++status) {
        const char *string = appliance_console_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_CONSOLE_OK ?
                       strcmp(string, "APPLIANCE_CONSOLE_OK") == 0 :
                   status == APPLIANCE_CONSOLE_NULL ?
                       strcmp(string, "APPLIANCE_CONSOLE_NULL") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_VERSION") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_STATE") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_IDENTITY ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_IDENTITY") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_CONFIG") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_HEALTH ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_HEALTH") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_NETWORK ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_NETWORK") == 0 :
                   status == APPLIANCE_CONSOLE_BAD_SERVICE ?
                       strcmp(string,
                              "APPLIANCE_CONSOLE_BAD_SERVICE") == 0 :
                       strcmp(string,
                              "APPLIANCE_CONSOLE_OVERFLOW") == 0),
              "appliance console status string is exact for every status");
    }
    CHECK(appliance_console_status_string(
              (enum appliance_console_status)10U) == NULL,
          "appliance console rejects undefined status string");
    for (state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
         state <= APPLIANCE_CONSOLE_STATE_FAILED; ++state) {
        const char *string = appliance_console_state_string(state);

        CHECK(string != NULL &&
                  (state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE ?
                       strcmp(string, "UNAVAILABLE") == 0 :
                   state == APPLIANCE_CONSOLE_STATE_OK ?
                       strcmp(string, "OK") == 0 :
                   state == APPLIANCE_CONSOLE_STATE_DEGRADED ?
                       strcmp(string, "DEGRADED") == 0 :
                       strcmp(string, "FAILED") == 0),
              "appliance console state string is exact for every state");
    }
    CHECK(appliance_console_state_string(
              (enum appliance_console_state)4U) == NULL,
          "appliance console rejects undefined state string");
}

static void test_snapshot_init(void)
{
    struct appliance_console_snapshot snapshot;
    struct appliance_identity identity = make_identity();
    struct appliance_identity bad = make_identity();

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_console_snapshot_init(
              NULL, &identity,
              APPLIANCE_CONSOLE_STATE_OK, 0U) ==
              APPLIANCE_CONSOLE_NULL,
          "appliance console snapshot init rejects null snapshot");
    CHECK(appliance_console_snapshot_init(
              &snapshot, NULL,
              APPLIANCE_CONSOLE_STATE_OK, 0U) ==
              APPLIANCE_CONSOLE_NULL,
          "appliance console snapshot init rejects null identity");

    bad.version = 0U;
    CHECK(appliance_console_snapshot_init(
              &snapshot, &bad,
              APPLIANCE_CONSOLE_STATE_OK, 0U) ==
              APPLIANCE_CONSOLE_BAD_IDENTITY,
          "appliance console snapshot init rejects invalid identity");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_console_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_CONSOLE_STATE_OK, 42U) ==
              APPLIANCE_CONSOLE_OK &&
              snapshot.published &&
              snapshot.version == APPLIANCE_CONSOLE_VERSION &&
              strcmp(snapshot.product,
                     APPLIANCE_IDENTITY_PRODUCT) == 0 &&
              strcmp(snapshot.architecture,
                     APPLIANCE_IDENTITY_ARCHITECTURE) == 0 &&
              strcmp(snapshot.platform,
                     APPLIANCE_IDENTITY_PLATFORM) == 0 &&
              strcmp(snapshot.hostname,
                     APPLIANCE_IDENTITY_DEFAULT_HOSTNAME) == 0 &&
              snapshot.config_state ==
                  APPLIANCE_CONSOLE_STATE_UNAVAILABLE &&
              snapshot.config_source ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE &&
              snapshot.health_state == APPLIANCE_CONSOLE_STATE_OK &&
              snapshot.uptime_ticks == 42U &&
              snapshot.network_state ==
                  APPLIANCE_CONSOLE_STATE_UNAVAILABLE &&
              snapshot.services_state ==
                  APPLIANCE_CONSOLE_STATE_UNAVAILABLE,
          "appliance console snapshot init publishes unavailable domains");

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_console_snapshot_init(
              &snapshot, &identity,
              (enum appliance_console_state)9U, 1U) ==
              APPLIANCE_CONSOLE_BAD_HEALTH,
          "appliance console snapshot init rejects unknown health state");
}

static void test_publish_config(void)
{
    struct appliance_console_snapshot snapshot = make_base_snapshot();
    char output[APPLIANCE_CONSOLE_OUTPUT_MAX + 1U];
    size_t length = 0U;

    CHECK(appliance_console_publish_config(
              &snapshot,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
              NULL, 0U) == APPLIANCE_CONSOLE_NULL,
          "appliance console config publish rejects null hostname");
    CHECK(appliance_console_publish_config(
              &snapshot,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              "node-305", 8U) == APPLIANCE_CONSOLE_BAD_CONFIG,
          "appliance console config publish rejects none source");
    CHECK(appliance_console_publish_config(
              &snapshot,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
              "bad_name", 8U) == APPLIANCE_CONSOLE_BAD_CONFIG,
          "appliance console config publish rejects invalid hostname");
    CHECK(snapshot.config_state ==
              APPLIANCE_CONSOLE_STATE_UNAVAILABLE &&
              snapshot.config_source ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
          "failed config publication leaves snapshot unchanged");

    CHECK(appliance_console_publish_config(
              &snapshot,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
              "node-305", 8U) == APPLIANCE_CONSOLE_OK &&
              snapshot.config_state == APPLIANCE_CONSOLE_STATE_OK &&
              snapshot.config_source ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              strcmp(snapshot.config_hostname, "node-305") == 0,
          "appliance console config publish copies valid committed config");
    CHECK(appliance_console_render(
              &snapshot, output, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_OK &&
              strstr(output,
                     "config state=OK source=COMMITTED "
                     "hostname=node-305\n") != NULL,
          "committed config renders source and hostname");
}

static void test_publish_network_and_services(void)
{
    struct appliance_console_snapshot snapshot = make_base_snapshot();
    char output[APPLIANCE_CONSOLE_OUTPUT_MAX + 1U];
    size_t length = 0U;

    CHECK(appliance_console_publish_network(
              &snapshot, APPLIANCE_CONSOLE_STATE_OK, 7U, 0U) ==
              APPLIANCE_CONSOLE_BAD_NETWORK,
          "appliance console network publish rejects zero devices");
    CHECK(appliance_console_publish_network(
              &snapshot,
              (enum appliance_console_state)9U, 7U, 1U) ==
              APPLIANCE_CONSOLE_BAD_NETWORK,
          "appliance console network publish rejects unknown state");
    CHECK(snapshot.network_state ==
              APPLIANCE_CONSOLE_STATE_UNAVAILABLE,
          "failed network publication leaves snapshot unchanged");
    CHECK(appliance_console_publish_network(
              &snapshot, APPLIANCE_CONSOLE_STATE_OK, 7U, 1U) ==
              APPLIANCE_CONSOLE_OK &&
              snapshot.network_generation == 7U &&
              snapshot.network_devices == 1U,
          "appliance console network publish copies exact state");

    CHECK(appliance_console_publish_services(
              &snapshot, APPLIANCE_CONSOLE_STATE_OK, 0U, 0U, 0U) ==
              APPLIANCE_CONSOLE_BAD_SERVICE,
          "appliance console service publish rejects zero services");
    CHECK(appliance_console_publish_services(
              &snapshot, APPLIANCE_CONSOLE_STATE_DEGRADED,
              2U, 1U, 2U) == APPLIANCE_CONSOLE_BAD_SERVICE,
          "appliance console service publish rejects inconsistent counts");
    CHECK(snapshot.services_state ==
              APPLIANCE_CONSOLE_STATE_UNAVAILABLE,
          "failed service publication leaves snapshot unchanged");
    CHECK(appliance_console_publish_services(
              &snapshot, APPLIANCE_CONSOLE_STATE_DEGRADED,
              2U, 1U, 1U) == APPLIANCE_CONSOLE_OK &&
              snapshot.service_count == 2U &&
              snapshot.service_ready == 1U &&
              snapshot.service_failed == 1U,
          "appliance console service publish copies exact state");
    CHECK(appliance_console_render(
              &snapshot, output, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_OK &&
              strstr(output,
                     "network state=OK generation=7 devices=1\n") != NULL &&
              strstr(output,
                     "services state=DEGRADED services=2 ready=1 "
                     "failed=1\n") != NULL,
          "network and service state render exact fields");
}

static void test_render(void)
{
    struct appliance_console_snapshot unavailable = make_base_snapshot();
    struct appliance_console_snapshot full = make_base_snapshot();
    char output[APPLIANCE_CONSOLE_OUTPUT_MAX + 1U];
    char small[4] = {'K', 'E', 'E', 'P'};
    size_t length = 0U;
    size_t length_a = 0U;
    size_t length_b = 0U;

    CHECK(appliance_console_render(
              &unavailable, output, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_OK &&
              length == strlen(unavailable_output()) &&
              strcmp(output, unavailable_output()) == 0 &&
              output[length] == '\0',
          "unavailable snapshot renders exact canonical record");

    (void)appliance_console_publish_config(
        &full,
        APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
        "node-305", 8U);
    (void)appliance_console_publish_network(
        &full, APPLIANCE_CONSOLE_STATE_OK, 7U, 1U);
    (void)appliance_console_publish_services(
        &full, APPLIANCE_CONSOLE_STATE_DEGRADED,
        2U, 1U, 1U);
    full.health_state = APPLIANCE_CONSOLE_STATE_DEGRADED;
    full.uptime_ticks = 12345U;

    CHECK(appliance_console_render(
              &full, output, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_OK &&
              length == strlen(full_output()) &&
              strcmp(output, full_output()) == 0 &&
              output[length] == '\0',
          "full snapshot renders exact canonical record");

    CHECK(appliance_console_render(
              &full, small, sizeof(small), &length) ==
              APPLIANCE_CONSOLE_OVERFLOW &&
              memcmp(small, "KEEP", sizeof(small)) == 0,
          "appliance console render rejects small buffer without mutation");
    CHECK(appliance_console_render(
              &full, output, sizeof(output), &length_a) ==
              APPLIANCE_CONSOLE_OK &&
              appliance_console_render(
                  &full, output, sizeof(output), &length_b) ==
                  APPLIANCE_CONSOLE_OK &&
              length_a == length_b &&
              strcmp(output, full_output()) == 0,
          "appliance console repeated render is byte-identical");
    CHECK(appliance_console_render(
              NULL, output, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_NULL,
          "appliance console render rejects null snapshot");
    CHECK(appliance_console_render(
              &full, NULL, sizeof(output), &length) ==
              APPLIANCE_CONSOLE_NULL,
          "appliance console render rejects null output");
    CHECK(appliance_console_render(
              &full, output, sizeof(output), NULL) ==
              APPLIANCE_CONSOLE_NULL,
          "appliance console render rejects null output length");
}

int main(void)
{
    test_strings();
    test_snapshot_init();
    test_publish_config();
    test_publish_network_and_services();
    test_render();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance console performs no allocation");
    if (failures != 0) {
        fprintf(stderr, "Appliance console host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance console host tests: ALL OK");
    return 0;
}

#endif
