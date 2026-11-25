#ifdef HFOC_ACCOUNTING_BOUNDARY_QEMU_TEST

#include "audit.h"
#include "authorization.h"
#include "config_schema.h"
#include "event_ring.h"
#include "integration_boundary.h"
#include "integration_compatibility.h"
#include "integration_config_intent.h"
#include "integration_envelope.h"
#include "integration_event_cursor.h"
#include "integration_failure_isolation.h"
#include "integration_ledger.h"
#include "integration_principal.h"
#include "integration_session.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ACCOUNTING_BOUNDARY_PEER_ID 7U
#define ACCOUNTING_BOUNDARY_SESSION_ID 3U
#define ACCOUNTING_BOUNDARY_PRINCIPAL_ID 4U
#define ACCOUNTING_BOUNDARY_RESULT 7U
#define ACCOUNTING_BOUNDARY_MAX_PROCESSES 48U

static void accounting_boundary_fail(const char *message)
    __attribute__((noreturn));

static void accounting_boundary_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[ACCOUNTING-BOUNDARY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void accounting_boundary_require(bool condition,
                                        const char *message)
{
    if (!condition) {
        accounting_boundary_fail(message);
    }
}

static void accounting_boundary_zero(void *destination, size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static struct integration_session_request
accounting_boundary_make_session_request(uint32_t schema_version,
                                         uint32_t transport_version,
                                         uint8_t nonce_first)
{
    struct integration_session_request request;

    accounting_boundary_zero(&request, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    request.peer_id = ACCOUNTING_BOUNDARY_PEER_ID;
    request.nonce[0] = nonce_first;
    return request;
}

static struct authorization_matrix accounting_boundary_matrix(void)
{
    struct authorization_entry entries[2];
    struct authorization_matrix matrix;

    accounting_boundary_zero(&entries, sizeof(entries));
    entries[0].version = AUTHORIZATION_VERSION;
    entries[0].command_id = 1U;
    entries[0].role_count = 1U;
    entries[0].roles[0] = AUTHORIZATION_ROLE_ADMIN;
    entries[0].capability_count = 1U;
    entries[0].capabilities[0] =
        AUTHORIZATION_CAPABILITY_STATUS_READ;
    entries[1].version = AUTHORIZATION_VERSION;
    entries[1].command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    entries[1].role_count = 1U;
    entries[1].roles[0] = AUTHORIZATION_ROLE_ADMIN;
    entries[1].capability_count = 1U;
    entries[1].capabilities[0] =
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE;

    accounting_boundary_require(
        authorization_matrix_init(
            &matrix, entries, 2U) == AUTHORIZATION_OK,
        "authorization matrix fixture");
    return matrix;
}

static void accounting_boundary_prove_boundary(void)
{
    static const char expected_schema[] = "HFOC-ACCT-INT-1";

    accounting_boundary_require(
        INTEGRATION_BOUNDARY_VERSION == 1U &&
        INTEGRATION_BOUNDARY_SCHEMA_ID_LENGTH ==
            sizeof(expected_schema) - 1U &&
        memcmp(INTEGRATION_BOUNDARY_SCHEMA_ID,
               expected_schema,
               sizeof(expected_schema) - 1U) == 0 &&
        INTEGRATION_BOUNDARY_OWNER_APPLIANCE == 1U &&
        INTEGRATION_BOUNDARY_OWNER_ACCOUNTING == 2U &&
        INTEGRATION_BOUNDARY_OWNER_SHARED_BOUNDARY == 3U &&
        INTEGRATION_BOUNDARY_AUDIT_HANDOFF_VERSION == 1U &&
        INTEGRATION_BOUNDARY_TRANSPORT_VERSION == 1U,
        "integration boundary exact constants");
    accounting_boundary_require(
        (INTEGRATION_BOUNDARY_UNSUPPORTED_ARBITRARY_SHELL &
             INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_LEDGER) == 0U &&
        (INTEGRATION_BOUNDARY_UNSUPPORTED_ACCOUNTING_BALANCE &
             INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_CONFIG_WRITE) == 0U &&
        (INTEGRATION_BOUNDARY_UNSUPPORTED_DIRECT_AUDIT_MUTATION &
             INTEGRATION_BOUNDARY_UNSUPPORTED_LOCAL_ADMIN_OVERRIDE) == 0U,
        "integration unsupported operations are distinct");
}

static void accounting_boundary_prove_handshake(void)
{
    struct integration_session_nonce_registry registry;
    struct integration_session_request valid =
        accounting_boundary_make_session_request(1U, 1U, 0x11U);
    struct integration_session_request bad_schema =
        accounting_boundary_make_session_request(0U, 1U, 0x12U);
    struct integration_session_request bad_transport =
        accounting_boundary_make_session_request(1U, 0U, 0x13U);

    accounting_boundary_require(
        integration_session_nonce_registry_init(
            &registry, 4U) == INTEGRATION_SESSION_OK,
        "session nonce registry init");
    accounting_boundary_require(
        integration_session_handshake_validate(
            &valid, &registry) == INTEGRATION_SESSION_OK &&
        registry.count == 1U,
        "valid session handshake");
    accounting_boundary_require(
        integration_session_handshake_validate(
            &valid, &registry) == INTEGRATION_SESSION_REPLAY &&
        registry.count == 1U,
        "replayed session nonce rejection");
    accounting_boundary_require(
        integration_session_handshake_validate(
            &bad_schema, &registry) ==
            INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION &&
        registry.count == 1U,
        "unsupported schema version rejection");
    accounting_boundary_require(
        integration_session_handshake_validate(
            &bad_transport, &registry) ==
            INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION &&
        registry.count == 1U,
        "unsupported transport version rejection");
}

static void accounting_boundary_prove_envelope_and_ledger(void)
{
    static const char payload[] = "audit-handoff-v1";
    unsigned char correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    unsigned char envelope[INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH];
    unsigned char tampered[INTEGRATION_ENVELOPE_MAX_ENVELOPE_LENGTH];
    unsigned char identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    unsigned char zero_identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_envelope_layout layout;
    struct integration_ledger ledger;
    size_t envelope_length = 0U;
    uint32_t result = 0U;

    accounting_boundary_zero(
        correlation_id, sizeof(correlation_id));
    correlation_id[0] = 0x42U;
    accounting_boundary_zero(&layout, sizeof(layout));
    accounting_boundary_zero(&zero_identity, sizeof(zero_identity));
    accounting_boundary_require(
        integration_envelope_seal(
            INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN,
            INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
            correlation_id,
            payload,
            sizeof(payload) - 1U,
            envelope,
            sizeof(envelope),
            &envelope_length) == INTEGRATION_ENVELOPE_OK &&
        envelope_length ==
            INTEGRATION_ENVELOPE_HEADER_SIZE +
            sizeof(payload) - 1U +
            INTEGRATION_ENVELOPE_DIGEST_SIZE,
        "audit hand-off envelope seal");
    accounting_boundary_require(
        integration_envelope_validate(
            envelope, envelope_length, &layout) ==
            INTEGRATION_ENVELOPE_OK &&
        layout.valid &&
        layout.envelope_version == INTEGRATION_ENVELOPE_VERSION &&
        layout.schema_version ==
            INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN &&
        layout.purpose == INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF &&
        layout.payload_length == sizeof(payload) - 1U &&
        layout.envelope_length == envelope_length,
        "audit hand-off envelope layout");

    memcpy(tampered, envelope, envelope_length);
    tampered[layout.digest_offset] ^= 0x01U;
    accounting_boundary_require(
        integration_envelope_validate(
            tampered, envelope_length, &layout) ==
            INTEGRATION_ENVELOPE_BAD_DIGEST,
        "tampered envelope digest rejection");

    memcpy(identity, envelope + layout.digest_offset,
           INTEGRATION_LEDGER_IDENTITY_SIZE);
    accounting_boundary_require(
        memcmp(identity, zero_identity,
               INTEGRATION_LEDGER_IDENTITY_SIZE) != 0,
        "envelope digest identity nonzero");
    accounting_boundary_require(
        integration_ledger_init(
            &ledger, 4U) == INTEGRATION_LEDGER_OK,
        "idempotency ledger init");
    accounting_boundary_require(
        integration_ledger_prepare(
            &ledger, identity, ACCOUNTING_BOUNDARY_RESULT,
            &result) == INTEGRATION_LEDGER_OK &&
        result == ACCOUNTING_BOUNDARY_RESULT,
        "envelope identity prepare");
    accounting_boundary_require(
        integration_ledger_prepare(
            &ledger, identity, ACCOUNTING_BOUNDARY_RESULT,
            &result) == INTEGRATION_LEDGER_DUPLICATE &&
        result == ACCOUNTING_BOUNDARY_RESULT &&
        ledger.count == 1U,
        "envelope identity duplicate prepare");
    accounting_boundary_require(
        integration_ledger_commit(
            &ledger, identity, &result) ==
            INTEGRATION_LEDGER_OK &&
        result == ACCOUNTING_BOUNDARY_RESULT,
        "envelope identity atomic commit");
    accounting_boundary_require(
        integration_ledger_commit(
            &ledger, identity, &result) ==
            INTEGRATION_LEDGER_ALREADY_COMMITTED &&
        result == ACCOUNTING_BOUNDARY_RESULT &&
        ledger.count == 1U,
        "envelope identity duplicate commit");
    accounting_boundary_require(
        integration_ledger_replay(
            &ledger, identity, &result) ==
            INTEGRATION_LEDGER_OK &&
        result == ACCOUNTING_BOUNDARY_RESULT &&
        ledger.count == 1U,
        "envelope identity replay");
}

static void accounting_boundary_prove_overreach_and_cursor(void)
{
    static const char initial_text[] =
        "schema_version=1\n"
        "recovery_mode=1\n"
        "max_processes=32\n"
        "max_user_memory_pages=512\n"
        "audit_ring_capacity=64\n"
        "boot_timeout_ms=5000\n"
        "restart_limit=3\n"
        "service_health_timeout_ms=10000\n";
    struct appliance_config initial;
    struct appliance_config active_before;
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct authorization_matrix matrix = accounting_boundary_matrix();
    struct integration_config_intent intent;
    struct audit_correlation correlation;
    struct integration_principal_scope scope;
    struct integration_event_cursor cursor;
    struct integration_event_cursor_record
        cursor_records[4];
    struct audit_record denied_record;
    struct kernel_event cursor_event;
    uint8_t integration_role[] = { AUTHORIZATION_ROLE_INTEGRATION };
    uint8_t integration_capability[] = {
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION
    };
    uint64_t audit_sequence = UINT64_MAX;
    uint32_t cursor_count = 0U;
    uint64_t cursor_generation = 0U;

    accounting_boundary_zero(&snapshot, sizeof(snapshot));
    accounting_boundary_zero(&ring, sizeof(ring));
    accounting_boundary_require(
        config_schema_parse(
            initial_text,
            sizeof(initial_text) - 1U,
            &initial) == CONFIG_OK &&
        config_snapshot_init(
            &snapshot, &initial) == CONFIG_SNAPSHOT_OK &&
        event_ring_init(&ring) == EVENT_RING_OK,
        "denial fixture initialization");
    active_before = *config_snapshot_active(&snapshot);

    accounting_boundary_zero(&intent, sizeof(intent));
    intent.version = INTEGRATION_CONFIG_INTENT_VERSION;
    intent.intent_id = INTEGRATION_CONFIG_INTENT_MAX_PROCESSES;
    intent.peer_id = ACCOUNTING_BOUNDARY_PEER_ID;
    intent.value = ACCOUNTING_BOUNDARY_MAX_PROCESSES;

    accounting_boundary_zero(&correlation, sizeof(correlation));
    correlation.version = AUDIT_CORRELATION_VERSION;
    correlation.session_id = ACCOUNTING_BOUNDARY_SESSION_ID;
    correlation.principal_id = ACCOUNTING_BOUNDARY_PRINCIPAL_ID;
    correlation.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    correlation.result = AUDIT_CORRELATION_RESULT_OK;

    accounting_boundary_zero(&scope, sizeof(scope));
    scope.version = INTEGRATION_PRINCIPAL_VERSION;
    scope.role = AUTHORIZATION_ROLE_INTEGRATION;
    scope.capability =
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION;
    accounting_boundary_require(
        integration_principal_scope_check(
            &scope, &matrix,
            CONTROLLED_CONFIGURATION_COMMAND_ID) ==
            INTEGRATION_PRINCIPAL_MISSING_ROLE,
        "integration principal local-admin denial");

    accounting_boundary_require(
        integration_config_intent_apply(
            &snapshot,
            &intent,
            &matrix,
            integration_role,
            1U,
            integration_capability,
            1U,
            &correlation,
            NULL,
            NULL,
            &ring,
            &audit_sequence) ==
            INTEGRATION_CONFIG_INTENT_DENIED &&
        audit_sequence == 0U,
        "integration config intent denial");
    accounting_boundary_require(
        memcmp(config_snapshot_active(&snapshot),
               &active_before,
               sizeof(active_before)) == 0 &&
        !config_snapshot_has_pending(&snapshot),
        "denied overreach leaves config unchanged");
    accounting_boundary_require(
        event_ring_count(&ring) == 1U &&
        audit_record_pop(&ring, &denied_record) == AUDIT_OK &&
        denied_record.type == AUDIT_RECORD_MANAGEMENT &&
        denied_record.operation ==
            INTEGRATION_CONFIG_INTENT_AUDIT_OPERATION &&
        denied_record.result ==
            INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED &&
        denied_record.session_id ==
            ACCOUNTING_BOUNDARY_SESSION_ID &&
        denied_record.principal_id ==
            ACCOUNTING_BOUNDARY_PRINCIPAL_ID &&
        denied_record.command_id ==
            CONTROLLED_CONFIGURATION_COMMAND_ID,
        "denied overreach audit correlation");

    accounting_boundary_require(
        audit_record_to_event(
            &denied_record, &cursor_event) == AUDIT_OK,
        "denied audit record event encoding");
    cursor_event.sequence = denied_record.sequence;
    accounting_boundary_require(
        integration_event_cursor_init(
            &cursor, 4U) == INTEGRATION_EVENT_CURSOR_OK &&
        integration_event_cursor_export(
            &cursor, &cursor_event) ==
            INTEGRATION_EVENT_CURSOR_OK &&
        integration_event_cursor_pending_count(&cursor) == 1U &&
        integration_event_cursor_next_export_sequence(&cursor) == 1U &&
        integration_event_cursor_snapshot(
            &cursor,
            cursor_records,
            4U,
            &cursor_count,
            &cursor_generation) ==
            INTEGRATION_EVENT_CURSOR_OK &&
        cursor_count == 1U &&
        cursor_generation == 1U &&
        cursor_records[0].export_sequence == 0U &&
        cursor_records[0].event.type == AUDIT_RECORD_MANAGEMENT,
        "outbound cursor coherent snapshot");
    accounting_boundary_require(
        integration_event_cursor_acknowledge(
            &cursor, 0U) == INTEGRATION_EVENT_CURSOR_OK &&
        integration_event_cursor_pending_count(&cursor) == 0U &&
        integration_event_cursor_last_ack_sequence(&cursor) == 0U,
        "outbound cursor oldest-only acknowledgement");
}

static void accounting_boundary_prove_failure_isolation(void)
{
    struct integration_failure_isolation_table table;

    accounting_boundary_require(
        integration_failure_isolation_init(
            &table, 4U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "failure isolation table init");
    accounting_boundary_require(
        integration_failure_isolation_record(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            INTEGRATION_FAILURE_MALFORMED,
            100U) == INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_record(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            INTEGRATION_FAILURE_MALFORMED,
            101U) == INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_record(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            INTEGRATION_FAILURE_MALFORMED,
            102U) ==
            INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED,
        "integration peer isolation after threshold");
    accounting_boundary_require(
        integration_failure_isolation_check(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            111U) == INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
        integration_failure_isolation_check(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            112U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "integration peer cooldown boundary");
    accounting_boundary_require(
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_MANAGEMENT) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_SERVICE_SUPERVISION) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_STATE_RECOVERY) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "integration peer failure never blocks local paths");
    accounting_boundary_require(
        integration_failure_isolation_recover(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        table.count == 0U &&
        integration_failure_isolation_check(
            &table,
            ACCOUNTING_BOUNDARY_PEER_ID,
            113U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "integration peer explicit recovery");
}

static void accounting_boundary_prove_compatibility(void)
{
    struct integration_compatibility_policy policy;
    struct integration_compatibility_request request;
    struct integration_session_nonce_registry registry;
    struct integration_session_request session_request =
        accounting_boundary_make_session_request(1U, 1U, 0x21U);

    accounting_boundary_require(
        integration_compatibility_policy_init(
            &policy) == INTEGRATION_COMPATIBILITY_OK,
        "compatibility policy init");
    request.schema_version = 1U;
    request.transport_version = 1U;
    accounting_boundary_require(
        integration_compatibility_check(
            &policy, &request) == INTEGRATION_COMPATIBILITY_OK,
        "compatibility policy accepts exact version");
    request.schema_version = 0U;
    accounting_boundary_require(
        integration_compatibility_check(
            &policy, &request) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
        "compatibility policy rejects unsupported schema");
    request.schema_version = 1U;
    request.transport_version = 0U;
    accounting_boundary_require(
        integration_compatibility_check(
            &policy, &request) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
        "compatibility policy rejects unsupported transport");
    accounting_boundary_require(
        integration_compatibility_policy_deprecate(
            &policy) == INTEGRATION_COMPATIBILITY_OK &&
        integration_compatibility_check(
            &policy, &request) ==
            INTEGRATION_COMPATIBILITY_DEPRECATED,
        "compatibility policy deprecation gate");
    accounting_boundary_require(
        integration_session_nonce_registry_init(
            &registry, 4U) == INTEGRATION_SESSION_OK &&
        integration_compatibility_session_validate(
            &policy,
            &session_request,
            &registry) ==
            INTEGRATION_COMPATIBILITY_DEPRECATED &&
        registry.count == 0U,
        "deprecated policy cannot obtain session nonce");
}

void accounting_boundary_qemu_test_init(void)
{
    accounting_boundary_prove_boundary();
    serial_write("[ACCOUNTING-BOUNDARY] proof init\n");

    accounting_boundary_prove_handshake();
    serial_write("[ACCOUNTING-BOUNDARY] handshake accepted\n");
    serial_write("[ACCOUNTING-BOUNDARY] version rejection exact\n");

    accounting_boundary_prove_envelope_and_ledger();
    serial_write(
        "[ACCOUNTING-BOUNDARY] envelope sealed and validated\n");
    serial_write("[ACCOUNTING-BOUNDARY] idempotent envelope replay\n");

    accounting_boundary_prove_overreach_and_cursor();
    serial_write("[ACCOUNTING-BOUNDARY] overreach denied\n");
    serial_write("[ACCOUNTING-BOUNDARY] audit correlation exact\n");
    serial_write("[ACCOUNTING-BOUNDARY] cursor coherent\n");

    accounting_boundary_prove_failure_isolation();
    serial_write("[ACCOUNTING-BOUNDARY] peer failure isolated\n");
    serial_write("[ACCOUNTING-BOUNDARY] local paths unaffected\n");

    accounting_boundary_prove_compatibility();
    serial_write("[ACCOUNTING-BOUNDARY] compatibility gate enforced\n");

    serial_write("[ACCOUNTING-BOUNDARY] clean shutdown\n");
    serial_write("[ACCOUNTING-BOUNDARY] proof OK\n");
}

#else
typedef int accounting_boundary_qemu_translation_unit_guard;
#endif
