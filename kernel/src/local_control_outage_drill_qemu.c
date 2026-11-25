#ifdef HFOC_LOCAL_CONTROL_OUTAGE_DRILL_QEMU_TEST

#include "auth_throttle.h"
#include "fault_injection.h"
#include "local_session.h"
#include "management_command.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void local_control_outage_drill_fail(const char *message)
    __attribute__((noreturn));

static void local_control_outage_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[LOCAL-CONTROL-OUTAGE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void local_control_outage_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        local_control_outage_drill_fail(message);
    }
}

static void local_control_outage_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void local_control_outage_drill_check_faults(void)
{
    const struct fault_injection_site *site = NULL;

    local_control_outage_drill_require(
        fault_injection_site_by_id(
            FAULT_INJECTION_LOCAL_SESSION_EXPIRY, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(
            FAULT_INJECTION_AUTH_THROTTLE, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(
            FAULT_INJECTION_LOCAL_MALFORMED_COMMAND, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL &&
            site->bounded && site->test_only,
        "catalogue local-control fault points");
}

void local_control_outage_drill_qemu_test_init(void)
{
    struct auth_throttle_table throttle;
    struct local_session_table sessions;
    struct management_command_registry registry;
    struct management_command_descriptor descriptor;
    struct management_command_header header;
    const unsigned char payload = 0xA5U;
    uint32_t expired_session_id = 0U;
    uint32_t recovery_session_id = 0U;
    uint64_t locked_until = 0U;

    serial_write("[LOCAL-CONTROL-OUTAGE-DRILL] proof init\n");
    local_control_outage_drill_check_faults();
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] catalogue local-control fault points exact\n");

    local_control_outage_drill_require(
        auth_throttle_table_init(
            &throttle, 4U, 3U, 100U) == AUTH_THROTTLE_OK &&
        local_session_table_init(&sessions, 2U) == LOCAL_SESSION_OK,
        "bounded local-control tables");
    local_control_outage_drill_zero(
        &descriptor, sizeof(descriptor));
    descriptor.version = MANAGEMENT_COMMAND_VERSION;
    descriptor.command_id = 1U;
    descriptor.request_min_length = 0U;
    descriptor.request_max_length = 0U;
    descriptor.response_min_length = 1U;
    descriptor.response_max_length = 64U;
    local_control_outage_drill_require(
        management_command_registry_init(
            &registry, &descriptor, 1U) == MANAGEMENT_COMMAND_OK &&
        registry.count == 1U,
        "command registry");

    local_control_outage_drill_require(
        local_session_open(
            &sessions, 7U, 1U, 100U, 50U,
            &expired_session_id) == LOCAL_SESSION_OK &&
        expired_session_id == 1U &&
        local_session_validate(
            &sessions, expired_session_id, 7U, 1U, 149U) ==
            LOCAL_SESSION_OK &&
        local_session_validate(
            &sessions, expired_session_id, 7U, 1U, 150U) ==
            LOCAL_SESSION_EXPIRED,
        "session expiry rejection");
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] session expiry rejected\n");

    local_control_outage_drill_require(
        auth_throttle_record_failure(
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
        "throttle lock isolation");
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] throttle lock isolated\n");

    local_control_outage_drill_require(
        auth_throttle_check(
            &throttle, 7U, 1120U) == AUTH_THROTTLE_OK &&
        auth_throttle_reset(
            &throttle, 7U) == AUTH_THROTTLE_OK &&
        auth_throttle_check(
            &throttle, 7U, 1120U) == AUTH_THROTTLE_OK,
        "throttle recovery");
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] throttle recovery after cooldown\n");

    local_control_outage_drill_zero(
        &header, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = 99U;
    local_control_outage_drill_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, NULL, 0U,
            MANAGEMENT_COMMAND_HEADER_LENGTH) ==
            MANAGEMENT_COMMAND_UNKNOWN_ID,
        "unknown command ID");
    header.command_id = 1U;
    local_control_outage_drill_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, &payload, 1U,
            MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
            MANAGEMENT_COMMAND_BAD_LENGTH,
        "bad command payload length");
    local_control_outage_drill_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, NULL, 0U,
            MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
            MANAGEMENT_COMMAND_TRAILING_FIELDS,
        "trailing command fields");
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] malformed commands rejected\n");

    local_control_outage_drill_require(
        local_session_open(
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
        "future authorized recovery");
    serial_write(
        "[LOCAL-CONTROL-OUTAGE-DRILL] future authorized local recovery\n");
    serial_write("[LOCAL-CONTROL-OUTAGE-DRILL] proof OK\n");
}

#else
typedef int local_control_outage_drill_qemu_translation_unit_guard;
#endif
