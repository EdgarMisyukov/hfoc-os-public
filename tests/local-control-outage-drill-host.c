#include "auth_throttle.h"
#include "fault_injection.h"
#include "local_session.h"
#include "management_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LOCAL_CONTROL_OUTAGE_DRILL_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_local_control_fault_catalogue(void)
{
    const struct fault_injection_site *site = NULL;

    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_LOCAL_SESSION_EXPIRY, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "local-control session-expiry fault point is exact");
    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_AUTH_THROTTLE, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "local-control auth-throttle fault point is exact");
    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_LOCAL_MALFORMED_COMMAND, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "local-control malformed-command fault point is exact");
}

static void test_local_control_outage_drill(void)
{
    struct auth_throttle_table throttle;
    struct local_session_table sessions;
    struct management_command_registry registry;
    struct management_command_descriptor descriptor;
    struct management_command_header header;
    unsigned char registry_before[sizeof(registry)];
    const unsigned char payload = 0xA5U;
    uint32_t expired_session_id = 0U;
    uint32_t recovery_session_id = 0U;
    uint64_t locked_until = UINT64_MAX;

    CHECK(auth_throttle_table_init(
              &throttle, 4U, 3U, 100U) == AUTH_THROTTLE_OK &&
              local_session_table_init(&sessions, 2U) == LOCAL_SESSION_OK,
          "local-control outage drill initializes bounded tables");

    descriptor.version = MANAGEMENT_COMMAND_VERSION;
    descriptor.command_id = 1U;
    descriptor.request_min_length = 0U;
    descriptor.request_max_length = 0U;
    descriptor.response_min_length = 1U;
    descriptor.response_max_length = 64U;
    CHECK(management_command_registry_init(
              &registry, &descriptor, 1U) == MANAGEMENT_COMMAND_OK &&
              registry.count == 1U,
          "local-control outage drill initializes command registry");
    memcpy(registry_before, &registry, sizeof(registry));

    CHECK(local_session_open(
              &sessions, 7U, 1U, 100U, 50U,
              &expired_session_id) == LOCAL_SESSION_OK &&
              expired_session_id == 1U &&
              local_session_validate(
                  &sessions, expired_session_id, 7U, 1U, 149U) ==
                  LOCAL_SESSION_OK &&
              local_session_validate(
                  &sessions, expired_session_id, 7U, 1U, 150U) ==
                  LOCAL_SESSION_EXPIRED,
          "expired local session is rejected");

    CHECK(auth_throttle_record_failure(
              &throttle, 7U, 1000U, &locked_until) == AUTH_THROTTLE_OK &&
              auth_throttle_record_failure(
                  &throttle, 8U, 1000U, &locked_until) == AUTH_THROTTLE_OK &&
              auth_throttle_record_failure(
                  &throttle, 7U, 1010U, &locked_until) == AUTH_THROTTLE_OK &&
              auth_throttle_record_failure(
                  &throttle, 7U, 1020U, &locked_until) == AUTH_THROTTLE_OK &&
              locked_until == 1120U &&
              auth_throttle_check(
                  &throttle, 7U, 1119U) == AUTH_THROTTLE_LOCKED &&
              auth_throttle_check(
                  &throttle, 8U, 1119U) == AUTH_THROTTLE_OK,
          "auth throttle locks only the failed principal during cooldown");
    CHECK(auth_throttle_check(
              &throttle, 7U, 1120U) == AUTH_THROTTLE_OK &&
              auth_throttle_reset(
                  &throttle, 7U) == AUTH_THROTTLE_OK &&
              auth_throttle_check(
                  &throttle, 7U, 1120U) == AUTH_THROTTLE_OK,
          "auth throttle permits authorized recovery after cooldown/reset");

    memset(&header, 0, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = 99U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U,
              MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_UNKNOWN_ID,
          "unknown command ID is rejected");
    header.command_id = 1U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, &payload, 1U,
              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "bad command payload length is rejected");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U,
              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_TRAILING_FIELDS,
          "trailing command fields are rejected");
    CHECK(memcmp(&registry, registry_before, sizeof(registry)) == 0,
          "malformed command attempts leave registry unchanged");

    CHECK(local_session_open(
              &sessions, 7U, 1U, 1200U, 100U,
              &recovery_session_id) == LOCAL_SESSION_OK &&
              recovery_session_id == 2U &&
              local_session_validate(
                  &sessions, recovery_session_id, 7U, 1U, 1200U) ==
                  LOCAL_SESSION_OK &&
              auth_throttle_check(
                  &throttle, 7U, 1200U) == AUTH_THROTTLE_OK &&
              management_command_validate_frame(
                  &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
                  &header, NULL, 0U,
                  MANAGEMENT_COMMAND_HEADER_LENGTH) ==
                  MANAGEMENT_COMMAND_OK &&
              local_session_close(
                  &sessions, recovery_session_id) == LOCAL_SESSION_OK,
          "future authorized local recovery succeeds and closes cleanly");
}

int main(void)
{
    test_local_control_fault_catalogue();
    test_local_control_outage_drill();

    if (failures != 0) {
        fprintf(stderr,
                "Local-control outage drill host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Local-control outage drill host tests: ALL OK\n");
    return 0;
}

#else
#error "local-control-outage-drill-host.c must be compiled with HFOC_LOCAL_CONTROL_OUTAGE_DRILL_HOST_TEST"
#endif
