#ifdef HFOC_LOCAL_CONTROL_QEMU_TEST

#include "appliance_status.h"
#include "audit.h"
#include "audit_correlation.h"
#include "auth_throttle.h"
#include "authorization.h"
#include "config_schema.h"
#include "controlled_configuration.h"
#include "credential_verifier.h"
#include "event_ring.h"
#include "local_principal.h"
#include "local_session.h"
#include "management_command.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCAL_CONTROL_PRINCIPAL_ADMIN 7U
#define LOCAL_CONTROL_PRINCIPAL_OPERATOR 8U
#define LOCAL_CONTROL_SESSION_GENERATION 1U
#define LOCAL_CONTROL_SESSION_OPEN_TICK 1200U
#define LOCAL_CONTROL_SESSION_LIFETIME_TICKS 100U
#define LOCAL_CONTROL_AUTH_TICK_BASE 1000U
#define LOCAL_CONTROL_AUTH_TICK_LOCK 1010U
#define LOCAL_CONTROL_AUTH_LOCKED_UNTIL 1110U
#define LOCAL_CONTROL_AUTH_TICK_RECOVER 1120U
#define LOCAL_CONTROL_MANAGEMENT_OPERATION 1U

static void local_control_qemu_fail(const char *message)
    __attribute__((noreturn));

static void local_control_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[LOCAL-CONTROL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void local_control_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        local_control_qemu_fail(message);
    }
}

static void local_control_qemu_zero(void *destination, size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void local_control_qemu_fill_secret(uint8_t *secret, uint8_t value)
{
    uint32_t index;

    for (index = 0U; index < CREDENTIAL_VERIFIER_SECRET_LENGTH; ++index) {
        secret[index] = value;
    }
}

static void local_control_qemu_principal_init(
    struct local_principal *principal,
    uint32_t id,
    const char *name,
    uint8_t role)
{
    uint32_t index;

    local_control_qemu_zero(principal, sizeof(*principal));
    principal->id = id;
    principal->version = LOCAL_PRINCIPAL_DESCRIPTOR_VERSION;
    principal->state = LOCAL_PRINCIPAL_STATE_ENABLED;
    principal->role_count = 1U;
    principal->roles[0] = role;
    for (index = 0U; index < LOCAL_PRINCIPAL_NAME_MAX; ++index) {
        if (name[index] == '\0') {
            break;
        }
        principal->name[index] = name[index];
    }
}

static bool local_control_qemu_config_equal(
    const struct appliance_config *left,
    const struct appliance_config *right)
{
    return left != NULL && right != NULL &&
           left->schema_version == right->schema_version &&
           left->recovery_mode == right->recovery_mode &&
           left->max_processes == right->max_processes &&
           left->max_user_memory_pages == right->max_user_memory_pages &&
           left->audit_ring_capacity == right->audit_ring_capacity &&
           left->boot_timeout_ms == right->boot_timeout_ms &&
           left->restart_limit == right->restart_limit &&
           left->service_health_timeout_ms ==
               right->service_health_timeout_ms;
}

static bool local_control_qemu_output_equals(
    const char *output,
    size_t output_length,
    const char *expected,
    size_t expected_length)
{
    size_t index;

    if (output_length != expected_length) {
        return false;
    }
    for (index = 0U; index < output_length; ++index) {
        if (output[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

static bool local_control_qemu_audit_matches(
    const struct audit_record *record,
    uint64_t expected_sequence,
    uint32_t expected_operation,
    uint32_t expected_result)
{
    return record != NULL &&
           record->sequence == expected_sequence &&
           record->type == AUDIT_RECORD_MANAGEMENT &&
           record->operation == expected_operation &&
           record->result == expected_result &&
           record->pid == 0U &&
           record->domain == 0U &&
           record->generation == 0U &&
           record->session_id == 1U &&
           record->principal_id == LOCAL_CONTROL_PRINCIPAL_ADMIN &&
           record->command_id == CONTROLLED_CONFIGURATION_COMMAND_ID &&
           record->detail == CONTROLLED_CONFIGURATION_COMMAND_ID;
}

static bool local_control_qemu_commit_accept(
    const struct appliance_config *candidate,
    void *context)
{
    (void)candidate;
    (void)context;
    return true;
}

void local_control_qemu_test_init(void)
{
    struct local_principal principals[2];
    struct local_principal published;
    struct credential_verifier verifier;
    struct credential_input correct_input;
    struct credential_input wrong_input;
    struct auth_throttle_table throttle;
    uint64_t lock_tick = 0U;
    struct local_session_table sessions;
    uint32_t session_id = 0U;
    struct management_command_descriptor descriptors[2];
    struct management_command_registry registry;
    struct management_command_header header;
    struct authorization_entry authorization_entries[2];
    struct authorization_matrix matrix;
    static const uint8_t admin_roles[1] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t admin_capabilities[2] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ,
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    static const uint8_t operator_roles[1] = {
        AUTHORIZATION_ROLE_OPERATOR
    };
    static const uint8_t config_capabilities[1] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    static const char initial_config_text[] =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "restart_limit=3\n"
        "service_health_timeout_ms=10000\n";
    static const char replacement_config_text[] =
        "schema_version=1\n"
        "recovery_mode=0\n"
        "max_processes=48\n"
        "max_user_memory_pages=1024\n"
        "audit_ring_capacity=32\n"
        "boot_timeout_ms=7000\n"
        "restart_limit=5\n"
        "service_health_timeout_ms=12000\n";
    static const char expected_status_output[] =
        "status v1=1 g=7 u=12345 r=0 c=2 a=99 p=2\n"
        "storage j=4 b=3 s=1 st=0\n"
        "health st=1 r=1 p=1 f=0\n"
        "services=2\n"
        "1:2/1/1\n"
        "2:1/0/0\n";
    static const char request_byte = 'x';
    struct appliance_config initial_config;
    struct appliance_config expected_config;
    struct config_snapshot snapshot;
    struct appliance_status_snapshot status_source;
    struct appliance_status_snapshot status_snapshot;
    char status_output[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    size_t status_length = 0U;
    struct audit_correlation denied;
    struct audit_correlation success;
    struct audit_record record;
    struct event_ring *const ring = kernel_event_ring_get();
    uint64_t sequence = UINT64_MAX;

    local_control_qemu_principal_init(
        &principals[0], LOCAL_CONTROL_PRINCIPAL_ADMIN,
        "admin", AUTHORIZATION_ROLE_ADMIN);
    local_control_qemu_principal_init(
        &principals[1], LOCAL_CONTROL_PRINCIPAL_OPERATOR,
        "operator", AUTHORIZATION_ROLE_OPERATOR);
    local_control_qemu_require(
        local_principal_set_validate(principals, 2U) ==
            LOCAL_PRINCIPAL_SET_OK &&
        local_principal_init(&published, &principals[0]) ==
            LOCAL_PRINCIPAL_OK &&
        published.roles[0] == AUTHORIZATION_ROLE_ADMIN,
        "principal fixture validation");
    serial_write("[LOCAL-CONTROL] proof init\n");

    local_control_qemu_zero(&verifier, sizeof(verifier));
    verifier.version = CREDENTIAL_VERIFIER_VERSION;
    verifier.principal_id = LOCAL_CONTROL_PRINCIPAL_ADMIN;
    verifier.state = CREDENTIAL_VERIFIER_STATE_ENABLED;
    local_control_qemu_fill_secret(verifier.expected_secret, 0x5AU);
    local_control_qemu_zero(&correct_input, sizeof(correct_input));
    correct_input.version = CREDENTIAL_VERIFIER_VERSION;
    correct_input.principal_id = LOCAL_CONTROL_PRINCIPAL_ADMIN;
    local_control_qemu_fill_secret(correct_input.secret, 0x5AU);
    local_control_qemu_zero(&wrong_input, sizeof(wrong_input));
    wrong_input.version = CREDENTIAL_VERIFIER_VERSION;
    wrong_input.principal_id = LOCAL_CONTROL_PRINCIPAL_ADMIN;
    local_control_qemu_fill_secret(wrong_input.secret, 0x00U);
    local_control_qemu_require(
        credential_verifier_verify(&verifier, &correct_input) ==
            CREDENTIAL_VERIFIER_OK &&
        credential_verifier_verify(&verifier, &wrong_input) ==
            CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED,
        "credential authentication proof");
    serial_write("[LOCAL-CONTROL] authentication success\n");

    local_control_qemu_require(
        auth_throttle_table_init(
            &throttle, 4U, 2U, 100U) == AUTH_THROTTLE_OK &&
        auth_throttle_record_failure(
            &throttle, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_AUTH_TICK_BASE, &lock_tick) ==
            AUTH_THROTTLE_OK &&
        auth_throttle_record_failure(
            &throttle, LOCAL_CONTROL_PRINCIPAL_OPERATOR,
            LOCAL_CONTROL_AUTH_TICK_BASE, &lock_tick) ==
            AUTH_THROTTLE_OK,
        "auth failure recording");
    serial_write("[LOCAL-CONTROL] auth failure counted\n");
    local_control_qemu_require(
        auth_throttle_record_failure(
            &throttle, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_AUTH_TICK_LOCK, &lock_tick) ==
            AUTH_THROTTLE_OK &&
        lock_tick == LOCAL_CONTROL_AUTH_LOCKED_UNTIL &&
        auth_throttle_check(
            &throttle, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_AUTH_LOCKED_UNTIL - 1U) ==
            AUTH_THROTTLE_LOCKED &&
        auth_throttle_check(
            &throttle, LOCAL_CONTROL_PRINCIPAL_OPERATOR,
            LOCAL_CONTROL_AUTH_LOCKED_UNTIL - 1U) ==
            AUTH_THROTTLE_OK,
        "throttle isolation");
    serial_write(
        "[LOCAL-CONTROL] throttle locked principal=7 unrelated=8\n");
    local_control_qemu_require(
        auth_throttle_check(
            &throttle, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_AUTH_TICK_RECOVER) == AUTH_THROTTLE_OK &&
        auth_throttle_reset(
            &throttle, LOCAL_CONTROL_PRINCIPAL_ADMIN) ==
            AUTH_THROTTLE_OK,
        "throttle recovery after cooldown");

    local_control_qemu_require(
        local_session_table_init(&sessions, 2U) == LOCAL_SESSION_OK &&
        local_session_open(
            &sessions, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_SESSION_GENERATION,
            LOCAL_CONTROL_SESSION_OPEN_TICK,
            LOCAL_CONTROL_SESSION_LIFETIME_TICKS,
            &session_id) == LOCAL_SESSION_OK &&
        session_id == 1U &&
        local_session_validate(
            &sessions, session_id, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_SESSION_GENERATION,
            LOCAL_CONTROL_SESSION_OPEN_TICK) == LOCAL_SESSION_OK,
        "local session open");
    serial_write(
        "[LOCAL-CONTROL] session open principal=7 id=1 generation=1\n");

    local_control_qemu_zero(&descriptors, sizeof(descriptors));
    local_control_qemu_require(
        appliance_status_command_descriptor(&descriptors[0]) ==
            APPLIANCE_STATUS_OK &&
        controlled_configuration_command_descriptor(&descriptors[1]) ==
            CONTROLLED_CONFIGURATION_OK &&
        management_command_registry_init(
            &registry, descriptors, 2U) == MANAGEMENT_COMMAND_OK,
        "management command registry");

    local_control_qemu_zero(&header, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = APPLIANCE_STATUS_COMMAND_ID;
    local_control_qemu_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, NULL, 0U,
            MANAGEMENT_COMMAND_HEADER_LENGTH) ==
            MANAGEMENT_COMMAND_OK,
        "status frame validation");
    header.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    local_control_qemu_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, &request_byte, 1U,
            MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
            MANAGEMENT_COMMAND_OK,
        "configuration frame validation");
    header.command_id = 99U;
    local_control_qemu_require(
        management_command_validate_frame(
            &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
            &header, &request_byte, 1U,
            MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
            MANAGEMENT_COMMAND_UNKNOWN_ID,
        "unknown command frame rejection");
    serial_write("[LOCAL-CONTROL] command frames validated\n");

    local_control_qemu_zero(
        &authorization_entries, sizeof(authorization_entries));
    local_control_qemu_require(
        appliance_status_authorization_entry(
            &authorization_entries[0]) == APPLIANCE_STATUS_OK &&
        controlled_configuration_authorization_entry(
            &authorization_entries[1]) ==
            CONTROLLED_CONFIGURATION_OK &&
        authorization_matrix_init(
            &matrix, authorization_entries, 2U) ==
            AUTHORIZATION_OK &&
        authorization_check(
            &matrix, APPLIANCE_STATUS_COMMAND_ID,
            admin_roles, 1U, admin_capabilities, 2U) ==
            AUTHORIZATION_OK &&
        authorization_check(
            &matrix, CONTROLLED_CONFIGURATION_COMMAND_ID,
            operator_roles, 1U, config_capabilities, 1U) ==
            AUTHORIZATION_MISSING_ROLE,
        "authorization matrix proof");

    local_control_qemu_zero(&snapshot, sizeof(snapshot));
    local_control_qemu_require(
        config_schema_parse(
            initial_config_text,
            sizeof(initial_config_text) - 1U,
            &initial_config) == CONFIG_OK &&
        config_snapshot_init(&snapshot, &initial_config) ==
            CONFIG_SNAPSHOT_OK,
        "config snapshot init");
    local_control_qemu_zero(&denied, sizeof(denied));
    denied.version = AUDIT_CORRELATION_VERSION;
    denied.session_id = session_id;
    denied.principal_id = LOCAL_CONTROL_PRINCIPAL_ADMIN;
    denied.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    denied.result = AUDIT_CORRELATION_RESULT_DENIED;
    local_control_qemu_require(
        ring != NULL &&
        event_ring_is_published(ring) &&
        event_ring_count(ring) == 0U &&
        audit_correlation_append(
            ring, LOCAL_CONTROL_MANAGEMENT_OPERATION,
            &denied, CONTROLLED_CONFIGURATION_COMMAND_ID,
            &sequence) == AUDIT_CORRELATION_OK &&
        sequence == 0U &&
        config_snapshot_active(&snapshot) != NULL &&
        local_control_qemu_config_equal(
            config_snapshot_active(&snapshot), &initial_config) &&
        !config_snapshot_has_pending(&snapshot),
        "denied command side-effect isolation");
    serial_write("[LOCAL-CONTROL] denied command no side effects\n");

    local_control_qemu_zero(&status_source, sizeof(status_source));
    status_source.version = APPLIANCE_STATUS_VERSION;
    status_source.state.version = APPLIANCE_STATUS_VERSION;
    status_source.state.generation = 7U;
    status_source.state.uptime_ticks = 12345U;
    status_source.state.recovery_mode = 0U;
    status_source.state.config_revision = 2U;
    status_source.state.audit_sequence = 99U;
    status_source.state.active_process_count = 2U;
    status_source.storage.version = APPLIANCE_STATUS_VERSION;
    status_source.storage.journal_generation = 4U;
    status_source.storage.backup_generation = 3U;
    status_source.storage.schema_version = 1U;
    status_source.storage.state = APPLIANCE_STORAGE_OK;
    status_source.health.version = APPLIANCE_STATUS_VERSION;
    status_source.health.state = APPLIANCE_HEALTH_DEGRADED;
    status_source.health.ready_count = 1U;
    status_source.health.pending_count = 1U;
    status_source.health.failed_count = 0U;
    status_source.service_count = 2U;
    status_source.services[0].id = 1U;
    status_source.services[0].state = APPLIANCE_SERVICE_READY;
    status_source.services[0].health = APPLIANCE_SERVICE_HEALTH_READY;
    status_source.services[0].restart_count = 1U;
    status_source.services[1].id = 2U;
    status_source.services[1].state = APPLIANCE_SERVICE_RUNNING;
    status_source.services[1].health = APPLIANCE_SERVICE_HEALTH_PENDING;
    status_source.services[1].restart_count = 0U;
    /* appliance_status_snapshot_init() refuses a destination that already
     * carries a published snapshot, and status_snapshot is a stack local: the
     * caller must hand it over initialised.  Without this the proof read
     * whatever the interrupt and boot path had left on the stack, so the
     * "read-only appliance status proof" step failed or passed with the stack
     * contents instead of with the code under test (measured 2026-09-14:
     * four consecutive local runs and one CI run stopped at exactly this step
     * with "[LOCAL-CONTROL] failed: read-only appliance status proof"). */
    local_control_qemu_zero(&status_snapshot, sizeof(status_snapshot));
    local_control_qemu_require(
        appliance_status_snapshot_init(
            &status_snapshot, &status_source) ==
            APPLIANCE_STATUS_OK &&
        appliance_status_render(
            &status_snapshot, status_output,
            sizeof(status_output), &status_length) ==
            APPLIANCE_STATUS_OK &&
        local_control_qemu_output_equals(
            status_output, status_length,
            expected_status_output,
            sizeof(expected_status_output) - 1U),
        "read-only appliance status proof");
    serial_write(
        "[LOCAL-CONTROL] status authorized and rendered\n");

    local_control_qemu_require(
        config_schema_parse(
            replacement_config_text,
            sizeof(replacement_config_text) - 1U,
            &expected_config) == CONFIG_OK,
        "replacement config parse");
    local_control_qemu_zero(&success, sizeof(success));
    success.version = AUDIT_CORRELATION_VERSION;
    success.session_id = session_id;
    success.principal_id = LOCAL_CONTROL_PRINCIPAL_ADMIN;
    success.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    success.result = AUDIT_CORRELATION_RESULT_OK;
    sequence = UINT64_MAX;
    local_control_qemu_require(
        controlled_configuration_apply_correlated(
            &snapshot, replacement_config_text,
            sizeof(replacement_config_text) - 1U,
            local_control_qemu_commit_accept, NULL,
            &success, ring, &sequence) ==
            CONTROLLED_CONFIGURATION_OK &&
        sequence == 1U &&
        config_snapshot_active(&snapshot) != NULL &&
        local_control_qemu_config_equal(
            config_snapshot_active(&snapshot), &expected_config) &&
        !config_snapshot_has_pending(&snapshot),
        "controlled configuration activation");
    serial_write("[LOCAL-CONTROL] configuration activated\n");

    local_control_qemu_require(
        audit_record_pop(ring, &record) == AUDIT_OK &&
        local_control_qemu_audit_matches(
            &record, 0U, LOCAL_CONTROL_MANAGEMENT_OPERATION,
            AUDIT_CORRELATION_RESULT_DENIED) &&
        audit_record_pop(ring, &record) == AUDIT_OK &&
        local_control_qemu_audit_matches(
            &record, 1U,
            CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE,
            AUDIT_CORRELATION_RESULT_OK) &&
        event_ring_count(ring) == 0U,
        "management audit evidence");
    serial_write("[LOCAL-CONTROL] audit evidence verified\n");

    local_control_qemu_require(
        local_session_close(&sessions, session_id) ==
            LOCAL_SESSION_OK &&
        local_session_validate(
            &sessions, session_id, LOCAL_CONTROL_PRINCIPAL_ADMIN,
            LOCAL_CONTROL_SESSION_GENERATION,
            LOCAL_CONTROL_SESSION_OPEN_TICK + 50U) ==
            LOCAL_SESSION_CLOSED,
        "clean session close");
    serial_write("[LOCAL-CONTROL] session closed\n");
    serial_write("[LOCAL-CONTROL] proof OK\n");
}

#else
typedef int local_control_qemu_translation_unit_guard;
#endif
