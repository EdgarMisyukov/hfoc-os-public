#include "appliance_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_STATUS_HOST_TEST

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

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct appliance_status_snapshot make_snapshot(void)
{
    struct appliance_status_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = APPLIANCE_STATUS_VERSION;
    snapshot.state.version = APPLIANCE_STATUS_VERSION;
    snapshot.state.generation = 7U;
    snapshot.state.uptime_ticks = 12345U;
    snapshot.state.recovery_mode = 0U;
    snapshot.state.config_revision = 2U;
    snapshot.state.audit_sequence = 99U;
    snapshot.state.active_process_count = 2U;
    snapshot.storage.version = APPLIANCE_STATUS_VERSION;
    snapshot.storage.journal_generation = 4U;
    snapshot.storage.backup_generation = 3U;
    snapshot.storage.schema_version = 1U;
    snapshot.storage.state = APPLIANCE_STORAGE_OK;
    snapshot.health.version = APPLIANCE_STATUS_VERSION;
    snapshot.health.state = APPLIANCE_HEALTH_DEGRADED;
    snapshot.health.ready_count = 1U;
    snapshot.health.pending_count = 1U;
    snapshot.health.failed_count = 0U;
    snapshot.service_count = 2U;
    snapshot.services[0].id = 1U;
    snapshot.services[0].state = APPLIANCE_SERVICE_READY;
    snapshot.services[0].health = APPLIANCE_SERVICE_HEALTH_READY;
    snapshot.services[0].restart_count = 1U;
    snapshot.services[1].id = 2U;
    snapshot.services[1].state = APPLIANCE_SERVICE_RUNNING;
    snapshot.services[1].health = APPLIANCE_SERVICE_HEALTH_PENDING;
    snapshot.services[1].restart_count = 0U;
    return snapshot;
}

static const char *expected_output(void)
{
    return "status v1=1 g=7 u=12345 r=0 c=2 a=99 p=2\n"
           "storage j=4 b=3 s=1 st=0\n"
           "health st=1 r=1 p=1 f=0\n"
           "services=2\n"
           "1:2/1/1\n"
           "2:1/0/0\n";
}

static void test_status_strings(void)
{
    enum appliance_status_status status;

    for (status = APPLIANCE_STATUS_OK;
         status <= APPLIANCE_STATUS_OVERFLOW; ++status) {
        const char *string = appliance_status_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_STATUS_OK ?
                       strcmp(string, "APPLIANCE_STATUS_OK") == 0 :
                   status == APPLIANCE_STATUS_NULL ?
                       strcmp(string, "APPLIANCE_STATUS_NULL") == 0 :
                   status == APPLIANCE_STATUS_BAD_VERSION ?
                       strcmp(string, "APPLIANCE_STATUS_BAD_VERSION") == 0 :
                   status == APPLIANCE_STATUS_BAD_STATE ?
                       strcmp(string, "APPLIANCE_STATUS_BAD_STATE") == 0 :
                   status == APPLIANCE_STATUS_INVALID_SNAPSHOT ?
                       strcmp(string,
                              "APPLIANCE_STATUS_INVALID_SNAPSHOT") == 0 :
                   status == APPLIANCE_STATUS_INVALID_SERVICE ?
                       strcmp(string,
                              "APPLIANCE_STATUS_INVALID_SERVICE") == 0 :
                   status == APPLIANCE_STATUS_INVALID_STORAGE ?
                       strcmp(string,
                              "APPLIANCE_STATUS_INVALID_STORAGE") == 0 :
                   status == APPLIANCE_STATUS_INVALID_HEALTH ?
                       strcmp(string,
                              "APPLIANCE_STATUS_INVALID_HEALTH") == 0 :
                       strcmp(string,
                              "APPLIANCE_STATUS_OVERFLOW") == 0),
              "appliance status status string is exact for every status");
    }
    CHECK(appliance_status_status_string(
              (enum appliance_status_status)9U) == NULL,
          "appliance status undefined status is rejected");
    CHECK(strcmp(appliance_status_health_string(APPLIANCE_HEALTH_OK),
                 "APPLIANCE_HEALTH_OK") == 0 &&
              strcmp(appliance_status_health_string(
                         APPLIANCE_HEALTH_DEGRADED),
                     "APPLIANCE_HEALTH_DEGRADED") == 0 &&
              strcmp(appliance_status_health_string(
                         APPLIANCE_HEALTH_FAILED),
                     "APPLIANCE_HEALTH_FAILED") == 0 &&
              appliance_status_health_string(
                  (enum appliance_status_health_state)3U) == NULL,
          "appliance health strings are exact and bounded");
    CHECK(strcmp(appliance_status_storage_string(APPLIANCE_STORAGE_OK),
                 "APPLIANCE_STORAGE_OK") == 0 &&
              strcmp(appliance_status_storage_string(
                         APPLIANCE_STORAGE_DEGRADED),
                     "APPLIANCE_STORAGE_DEGRADED") == 0 &&
              strcmp(appliance_status_storage_string(
                         APPLIANCE_STORAGE_FAILED),
                     "APPLIANCE_STORAGE_FAILED") == 0 &&
              appliance_status_storage_string(
                  (enum appliance_status_storage_state)3U) == NULL,
          "appliance storage strings are exact and bounded");
    CHECK(strcmp(appliance_status_service_state_string(
                     APPLIANCE_SERVICE_STOPPED),
                 "APPLIANCE_SERVICE_STOPPED") == 0 &&
              strcmp(appliance_status_service_state_string(
                         APPLIANCE_SERVICE_RUNNING),
                     "APPLIANCE_SERVICE_RUNNING") == 0 &&
              strcmp(appliance_status_service_state_string(
                         APPLIANCE_SERVICE_READY),
                     "APPLIANCE_SERVICE_READY") == 0 &&
              strcmp(appliance_status_service_state_string(
                         APPLIANCE_SERVICE_EXITED),
                     "APPLIANCE_SERVICE_EXITED") == 0 &&
              strcmp(appliance_status_service_state_string(
                         APPLIANCE_SERVICE_EXHAUSTED),
                     "APPLIANCE_SERVICE_EXHAUSTED") == 0 &&
              appliance_status_service_state_string(
                  (enum appliance_status_service_state)5U) == NULL,
          "appliance service state strings are exact and bounded");
    CHECK(strcmp(appliance_status_service_health_string(
                     APPLIANCE_SERVICE_HEALTH_PENDING),
                 "APPLIANCE_SERVICE_HEALTH_PENDING") == 0 &&
              strcmp(appliance_status_service_health_string(
                         APPLIANCE_SERVICE_HEALTH_READY),
                     "APPLIANCE_SERVICE_HEALTH_READY") == 0 &&
              strcmp(appliance_status_service_health_string(
                         APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED),
                     "APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED") == 0 &&
              strcmp(appliance_status_service_health_string(
                         APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT),
                     "APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT") == 0 &&
              appliance_status_service_health_string(
                  (enum appliance_status_service_health)4U) == NULL,
          "appliance service health strings are exact and bounded");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance status status handling performs no allocation");
}

static void test_snapshot_init(void)
{
    struct appliance_status_snapshot source = make_snapshot();
    struct appliance_status_snapshot invalid = make_snapshot();
    struct appliance_status_snapshot snapshot;
    struct appliance_status_snapshot saved;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(NULL, &source) ==
              APPLIANCE_STATUS_NULL,
          "appliance status snapshot init rejects null snapshot");
    CHECK(appliance_status_snapshot_init(&snapshot, NULL) ==
              APPLIANCE_STATUS_NULL,
          "appliance status snapshot init rejects null source");
    CHECK(appliance_status_snapshot_init(&snapshot, &source) ==
              APPLIANCE_STATUS_OK &&
              snapshot.published &&
              snapshot.version == APPLIANCE_STATUS_VERSION &&
              snapshot.state.generation == 7U &&
              snapshot.storage.journal_generation == 4U &&
              snapshot.health.state == APPLIANCE_HEALTH_DEGRADED &&
              snapshot.service_count == 2U &&
              snapshot.services[0].id == 1U &&
              snapshot.services[1].id == 2U,
          "appliance status snapshot init publishes exact snapshot");
    CHECK(appliance_status_snapshot_init(&snapshot, &source) ==
              APPLIANCE_STATUS_BAD_STATE,
          "appliance status snapshot init rejects reinitialization");

    invalid.version = APPLIANCE_STATUS_VERSION + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_BAD_VERSION,
          "appliance status snapshot init rejects bad top-level version");
    invalid = make_snapshot();
    invalid.state.version = APPLIANCE_STATUS_VERSION + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_BAD_VERSION,
          "appliance status snapshot init rejects bad state version");
    invalid = make_snapshot();
    invalid.storage.version = APPLIANCE_STATUS_VERSION + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_BAD_VERSION,
          "appliance status snapshot init rejects bad storage version");
    invalid = make_snapshot();
    invalid.health.version = APPLIANCE_STATUS_VERSION + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_BAD_VERSION,
          "appliance status snapshot init rejects bad health version");

    invalid = make_snapshot();
    invalid.state.recovery_mode = APPLIANCE_STATUS_RECOVERY_MODE_MAX + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SNAPSHOT,
          "appliance status snapshot init rejects over-limit recovery mode");
    invalid = make_snapshot();
    invalid.state.active_process_count =
        APPLIANCE_STATUS_COUNT_MAX + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SNAPSHOT,
          "appliance status snapshot init rejects over-limit process count");
    invalid = make_snapshot();
    invalid.service_count = APPLIANCE_STATUS_SERVICE_MAX + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SERVICE,
          "appliance status snapshot init rejects over-limit service count");

    invalid = make_snapshot();
    invalid.storage.state =
        (enum appliance_status_storage_state)3U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_STORAGE,
          "appliance status snapshot init rejects unknown storage state");
    invalid = make_snapshot();
    invalid.health.state =
        (enum appliance_status_health_state)3U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_HEALTH,
          "appliance status snapshot init rejects unknown health state");
    invalid = make_snapshot();
    invalid.health.ready_count = APPLIANCE_STATUS_COUNT_MAX + 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_HEALTH,
          "appliance status snapshot init rejects over-limit health count");

    invalid = make_snapshot();
    invalid.services[0].id = 2U;
    invalid.services[1].id = 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SERVICE,
          "appliance status snapshot init rejects unsorted service entries");
    invalid = make_snapshot();
    invalid.services[1].id = 1U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SERVICE,
          "appliance status snapshot init rejects duplicate service ids");
    invalid = make_snapshot();
    invalid.services[0].state =
        (enum appliance_status_service_state)5U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SERVICE,
          "appliance status snapshot init rejects unknown service state");
    invalid = make_snapshot();
    invalid.services[0].health =
        (enum appliance_status_service_health)4U;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_SERVICE,
          "appliance status snapshot init rejects unknown service health");

    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    saved = snapshot;
    invalid = make_snapshot();
    invalid.storage.state =
        (enum appliance_status_storage_state)3U;
    CHECK(appliance_status_snapshot_init(&snapshot, &invalid) ==
              APPLIANCE_STATUS_INVALID_STORAGE &&
              memcmp(&snapshot, &saved, sizeof(snapshot)) == 0,
          "appliance status failed snapshot init leaves destination unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance status snapshot init performs no allocation");
}

static void test_render(void)
{
    struct appliance_status_snapshot source = make_snapshot();
    struct appliance_status_snapshot snapshot;
    struct appliance_status_snapshot invalid;
    char output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    char small[4] = {'K', 'E', 'E', 'P'};
    size_t output_length = 0U;
    size_t length_a = 0U;
    size_t length_b = 0U;
    const char *expected = expected_output();

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &source) ==
              APPLIANCE_STATUS_OK,
          "appliance status render fixture initializes snapshot");

    CHECK(appliance_status_render(NULL, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_NULL,
          "appliance status render rejects null snapshot");
    CHECK(appliance_status_render(&snapshot, NULL, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_NULL,
          "appliance status render rejects null output");
    CHECK(appliance_status_render(&snapshot, output, sizeof(output),
                                  NULL) ==
              APPLIANCE_STATUS_NULL,
          "appliance status render rejects null output length");

    memset(&invalid, 0, sizeof(invalid));
    invalid.version = APPLIANCE_STATUS_VERSION;
    invalid.state.version = APPLIANCE_STATUS_VERSION;
    invalid.storage.version = APPLIANCE_STATUS_VERSION;
    invalid.health.version = APPLIANCE_STATUS_VERSION;
    CHECK(appliance_status_render(&invalid, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_BAD_STATE,
          "appliance status render rejects unpublished snapshot");
    invalid.published = true;
    invalid.version = APPLIANCE_STATUS_VERSION + 1U;
    CHECK(appliance_status_render(&invalid, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_BAD_VERSION,
          "appliance status render rejects bad version");

    CHECK(appliance_status_render(&snapshot, small, sizeof(small),
                                  &output_length) ==
              APPLIANCE_STATUS_OVERFLOW &&
              memcmp(small, "KEEP", sizeof(small)) == 0,
          "appliance status render rejects small buffer without mutation");
    CHECK(appliance_status_render(&snapshot, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_OK &&
              output_length == strlen(expected) &&
              strcmp(output, expected) == 0 &&
              output[output_length] == '\0',
          "appliance status render emits exact bounded output");
    CHECK(appliance_status_render(&snapshot, output, sizeof(output),
                                  &length_a) ==
              APPLIANCE_STATUS_OK &&
              appliance_status_render(&snapshot, output, sizeof(output),
                                      &length_b) ==
              APPLIANCE_STATUS_OK &&
              length_a == length_b &&
              strcmp(output, expected) == 0,
          "appliance status repeated render is byte-identical");
    CHECK(output_length <= APPLIANCE_STATUS_OUTPUT_MAX,
          "appliance status output remains within management payload bound");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance status render performs no allocation");
}

static void test_render_maximum_services(void)
{
    struct appliance_status_snapshot source = make_snapshot();
    struct appliance_status_snapshot snapshot;
    char output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    uint32_t index;

    for (index = 0U; index < APPLIANCE_STATUS_SERVICE_MAX; ++index) {
        source.services[index].id = 248U + index;
        source.services[index].state = APPLIANCE_SERVICE_EXHAUSTED;
        source.services[index].health =
            APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT;
        source.services[index].restart_count =
            APPLIANCE_STATUS_COUNT_MAX;
    }
    source.service_count = APPLIANCE_STATUS_SERVICE_MAX;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &source) ==
              APPLIANCE_STATUS_OK,
          "maximum service fixture initializes");
    CHECK(appliance_status_render(&snapshot, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_OK &&
              output_length <= APPLIANCE_STATUS_OUTPUT_MAX &&
              output[output_length] == '\0',
          "maximum service output remains bounded");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "maximum service render performs no allocation");
}

static void test_command_integration(void)
{
    struct appliance_status_snapshot source = make_snapshot();
    struct appliance_status_snapshot snapshot;
    struct management_command_descriptor descriptor;
    struct management_command_registry registry;
    struct management_command_header header;
    struct authorization_entry authorization;
    struct authorization_matrix matrix;
    static const uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t status_capability[] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    char output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_status_snapshot_init(&snapshot, &source) ==
              APPLIANCE_STATUS_OK,
          "appliance status integration fixture initializes snapshot");
    CHECK(appliance_status_command_descriptor(NULL) ==
              APPLIANCE_STATUS_NULL,
          "appliance status descriptor init rejects null descriptor");
    CHECK(appliance_status_command_descriptor(&descriptor) ==
              APPLIANCE_STATUS_OK &&
              descriptor.version == MANAGEMENT_COMMAND_VERSION &&
              descriptor.command_id == APPLIANCE_STATUS_COMMAND_ID &&
              descriptor.request_min_length ==
                  APPLIANCE_STATUS_REQUEST_LENGTH &&
              descriptor.request_max_length ==
                  APPLIANCE_STATUS_REQUEST_LENGTH &&
              descriptor.response_min_length ==
                  APPLIANCE_STATUS_RESPONSE_MIN &&
              descriptor.response_max_length ==
                  APPLIANCE_STATUS_RESPONSE_MAX,
          "appliance status descriptor init publishes bounded command");
    CHECK(management_command_registry_init(&registry, &descriptor, 1U) ==
              MANAGEMENT_COMMAND_OK,
          "appliance status descriptor is registry-valid");

    memset(&header, 0, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = APPLIANCE_STATUS_COMMAND_ID;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, APPLIANCE_STATUS_REQUEST_LENGTH,
              MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_OK,
          "appliance status empty request frame validates");
    CHECK(appliance_status_render(&snapshot, output, sizeof(output),
                                  &output_length) ==
              APPLIANCE_STATUS_OK,
          "appliance status integration render succeeds");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, output, output_length,
              MANAGEMENT_COMMAND_HEADER_LENGTH + output_length) ==
              MANAGEMENT_COMMAND_OK,
          "appliance status bounded response frame validates");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "appliance status empty response frame is rejected");

    CHECK(appliance_status_authorization_entry(NULL) ==
              APPLIANCE_STATUS_NULL,
          "appliance status authorization init rejects null entry");
    CHECK(appliance_status_authorization_entry(&authorization) ==
              APPLIANCE_STATUS_OK &&
              authorization.version == AUTHORIZATION_VERSION &&
              authorization.command_id == APPLIANCE_STATUS_COMMAND_ID &&
              authorization.role_count == 1U &&
              authorization.roles[0] == AUTHORIZATION_ROLE_ADMIN &&
              authorization.capability_count == 1U &&
              authorization.capabilities[0] ==
                  AUTHORIZATION_CAPABILITY_STATUS_READ,
          "appliance status authorization entry publishes exact requirement");
    CHECK(authorization_matrix_init(&matrix, &authorization, 1U) ==
              AUTHORIZATION_OK,
          "appliance status authorization entry is matrix-valid");
    CHECK(authorization_check(&matrix, APPLIANCE_STATUS_COMMAND_ID,
                              admin_role, 1U, status_capability, 1U) ==
              AUTHORIZATION_OK,
          "appliance status read is granted to admin with status capability");
    CHECK(authorization_check(&matrix, APPLIANCE_STATUS_COMMAND_ID,
                              admin_role, 1U, NULL, 0U) ==
              AUTHORIZATION_MISSING_CAPABILITY,
          "appliance status read denies missing status capability");
    CHECK(authorization_check(&matrix, APPLIANCE_STATUS_COMMAND_ID,
                              NULL, 0U, status_capability, 1U) ==
              AUTHORIZATION_MISSING_ROLE,
          "appliance status read denies missing admin role");
    CHECK(authorization_check(&matrix, 99U,
                              admin_role, 1U, status_capability, 1U) ==
              AUTHORIZATION_UNKNOWN_COMMAND,
          "appliance status read denies unknown command");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance status command integration performs no allocation");
}

int main(void) {
    test_status_strings();
    test_snapshot_init();
    test_render();
    test_render_maximum_services();
    test_command_integration();
    if (failures != 0) {
        fprintf(stderr, "Appliance status host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance status host tests: ALL OK");
    return 0;
}

#endif
