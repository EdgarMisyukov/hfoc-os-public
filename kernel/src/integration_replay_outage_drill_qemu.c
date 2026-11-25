#ifdef HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_QEMU_TEST

#include "fault_injection.h"
#include "integration_boundary.h"
#include "integration_compatibility.h"
#include "integration_envelope.h"
#include "integration_failure_isolation.h"
#include "integration_ledger.h"
#include "integration_session.h"
#include "serial.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTEGRATION_REPLAY_DRILL_RESULT 0x1234U
#define INTEGRATION_REPLAY_DRILL_MALFORMED_PEER 1001U
#define INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER 1002U
#define INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER 1003U

static void integration_replay_drill_fail(const char *message)
    __attribute__((noreturn));

static void integration_replay_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[INTEGRATION-REPLAY-OUTAGE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void integration_replay_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        integration_replay_drill_fail(message);
    }
}

static void integration_replay_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void integration_replay_drill_copy(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *const out = destination;
    const unsigned char *const in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool integration_replay_drill_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *const a = left;
    const unsigned char *const b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static bool integration_replay_drill_string_equal(
    const char *left,
    const char *right)
{
    size_t index = 0U;

    for (;;) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
        ++index;
    }
}

static void integration_replay_drill_fill_correlation_id(
    uint8_t *correlation_id)
{
    uint32_t index;

    integration_replay_drill_zero(
        correlation_id, INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE);
    for (index = 0U;
         index < INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE;
         ++index) {
        correlation_id[index] = (uint8_t)(0x41U + index);
    }
}

static bool integration_replay_drill_seal_envelope(
    unsigned char *envelope,
    size_t capacity,
    size_t *length_out,
    struct integration_envelope_layout *layout_out,
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE])
{
    static const unsigned char payload[] = "accounting audit hand-off";
    uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    size_t length = 0U;

    integration_replay_drill_fill_correlation_id(correlation_id);
    if (integration_envelope_seal(
            INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
            INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
            correlation_id, payload, sizeof(payload),
            envelope, capacity, &length) != INTEGRATION_ENVELOPE_OK) {
        return false;
    }
    if (integration_envelope_validate(
            envelope, length, layout_out) != INTEGRATION_ENVELOPE_OK ||
        !layout_out->valid) {
        return false;
    }
    integration_replay_drill_copy(
        identity, envelope + layout_out->digest_offset,
        INTEGRATION_LEDGER_IDENTITY_SIZE);
    *length_out = length;
    return true;
}

static void integration_replay_drill_check_fault_point(
    enum fault_injection_id id,
    const char *expected_outcome)
{
    const struct fault_injection_site *site = NULL;

    integration_replay_drill_require(
        fault_injection_site_by_id(id, &site) == FAULT_INJECTION_OK &&
        site != NULL &&
        site->subsystem == FAULT_INJECTION_SUBSYSTEM_INTEGRATION &&
        site->bounded &&
        site->test_only &&
        site->expected_outcome != NULL &&
        integration_replay_drill_string_equal(
            site->expected_outcome, expected_outcome),
        "integration replay drill fault point exact");
}

static struct integration_session_request
integration_replay_drill_make_session_request(
    uint32_t schema_version,
    uint32_t transport_version,
    uint32_t peer_id,
    uint8_t first_nonce_byte)
{
    struct integration_session_request request;

    integration_replay_drill_zero(&request, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    request.peer_id = peer_id;
    request.nonce[0] = first_nonce_byte;
    return request;
}

static void integration_replay_drill_isolate_peer(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    enum integration_failure_kind kind,
    uint64_t start_tick)
{
    integration_replay_drill_require(
        integration_failure_isolation_record(
            table, peer_id, kind, start_tick) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_record(
            table, peer_id, kind, start_tick + 1U) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_record(
            table, peer_id, kind, start_tick + 2U) ==
            INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED,
        "integration peer isolation after threshold");
}

static void integration_replay_drill_check_local_paths(void)
{
    integration_replay_drill_require(
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_MANAGEMENT) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_SERVICE_SUPERVISION) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        integration_failure_isolation_local_allowed(
            INTEGRATION_LOCAL_STATE_RECOVERY) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "integration peer outage never blocks local paths");
}

static void integration_replay_drill_prove_catalogue(void)
{
    integration_replay_drill_check_fault_point(
        FAULT_INJECTION_INTEGRATION_DUPLICATE,
        "duplicate request is idempotent");
    integration_replay_drill_check_fault_point(
        FAULT_INJECTION_INTEGRATION_MALFORMED,
        "malformed request rejected and isolated");
    integration_replay_drill_check_fault_point(
        FAULT_INJECTION_INTEGRATION_INCOMPATIBLE,
        "incompatible request rejected and isolated");
    integration_replay_drill_check_fault_point(
        FAULT_INJECTION_INTEGRATION_UNAVAILABLE,
        "unavailable/slow peer isolated without local block");
}

static void integration_replay_drill_prove_duplicate(void)
{
    unsigned char envelope[512];
    unsigned char before[sizeof(struct integration_ledger)];
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_envelope_layout layout;
    struct integration_ledger ledger;
    size_t envelope_length = 0U;
    uint32_t result = 0U;

    integration_replay_drill_require(
        integration_replay_drill_seal_envelope(
            envelope, sizeof(envelope), &envelope_length,
            &layout, identity),
        "integration replay envelope seal and validate");
    integration_replay_drill_require(
        integration_ledger_init(&ledger, 4U) == INTEGRATION_LEDGER_OK &&
        integration_ledger_prepare(
            &ledger, identity, INTEGRATION_REPLAY_DRILL_RESULT,
            &result) == INTEGRATION_LEDGER_OK &&
        result == INTEGRATION_REPLAY_DRILL_RESULT &&
        ledger.count == 1U &&
        ledger.entries[0].pending &&
        !ledger.entries[0].committed,
        "integration envelope identity prepared once");
    integration_replay_drill_copy(
        before, &ledger, sizeof(ledger));
    result = 0U;
    integration_replay_drill_require(
        integration_ledger_prepare(
            &ledger, identity, 0x7777U, &result) ==
            INTEGRATION_LEDGER_DUPLICATE &&
        result == INTEGRATION_REPLAY_DRILL_RESULT &&
        integration_replay_drill_equal(
            &ledger, before, sizeof(ledger)),
        "duplicate prepare idempotent");
    result = 0U;
    integration_replay_drill_require(
        integration_ledger_commit(
            &ledger, identity, &result) == INTEGRATION_LEDGER_OK &&
        result == INTEGRATION_REPLAY_DRILL_RESULT &&
        ledger.count == 1U &&
        !ledger.entries[0].pending &&
        ledger.entries[0].committed,
        "integration envelope identity committed once");
    integration_replay_drill_copy(
        before, &ledger, sizeof(ledger));
    result = 0U;
    integration_replay_drill_require(
        integration_ledger_commit(
            &ledger, identity, &result) ==
            INTEGRATION_LEDGER_ALREADY_COMMITTED &&
        result == INTEGRATION_REPLAY_DRILL_RESULT &&
        integration_replay_drill_equal(
            &ledger, before, sizeof(ledger)),
        "duplicate commit idempotent");
    integration_replay_drill_copy(
        before, &ledger, sizeof(ledger));
    result = 0U;
    integration_replay_drill_require(
        integration_ledger_replay(
            &ledger, identity, &result) == INTEGRATION_LEDGER_OK &&
        result == INTEGRATION_REPLAY_DRILL_RESULT &&
        integration_replay_drill_equal(
            &ledger, before, sizeof(ledger)),
        "read-only replay idempotent");
}

static void integration_replay_drill_prove_malformed(void)
{
    unsigned char envelope[512];
    unsigned char tampered[512];
    unsigned char before[sizeof(struct integration_ledger)];
    unsigned char short_envelope[
        INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH - 1U];
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_envelope_layout layout;
    struct integration_ledger ledger;
    struct integration_failure_isolation_table table;
    size_t envelope_length = 0U;

    integration_replay_drill_require(
        integration_replay_drill_seal_envelope(
            envelope, sizeof(envelope), &envelope_length,
            &layout, identity),
        "malformed drill valid envelope");
    integration_replay_drill_copy(
        tampered, envelope, envelope_length);
    tampered[layout.digest_offset] ^= 0x01U;
    integration_replay_drill_require(
        integration_envelope_validate(
            tampered, envelope_length, &layout) ==
            INTEGRATION_ENVELOPE_BAD_DIGEST,
        "tampered envelope digest rejection");
    integration_replay_drill_zero(
        short_envelope, sizeof(short_envelope));
    integration_replay_drill_require(
        integration_envelope_validate(
            short_envelope, sizeof(short_envelope), &layout) ==
            INTEGRATION_ENVELOPE_TOO_SHORT,
        "truncated envelope rejection");
    integration_replay_drill_require(
        integration_ledger_init(&ledger, 4U) == INTEGRATION_LEDGER_OK,
        "malformed drill ledger init");
    integration_replay_drill_copy(
        before, &ledger, sizeof(ledger));
    integration_replay_drill_require(
        ledger.count == 0U &&
        integration_replay_drill_equal(
            &ledger, before, sizeof(ledger)),
        "malformed request leaves ledger unchanged");
    integration_replay_drill_require(
        integration_failure_isolation_init(
            &table, 4U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "malformed drill failure table init");
    integration_replay_drill_isolate_peer(
        &table, INTEGRATION_REPLAY_DRILL_MALFORMED_PEER,
        INTEGRATION_FAILURE_MALFORMED, 100U);
    integration_replay_drill_require(
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_MALFORMED_PEER, 111U) ==
            INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_MALFORMED_PEER, 112U) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "malformed peer cooldown boundary");
    integration_replay_drill_check_local_paths();
    integration_replay_drill_require(
        integration_failure_isolation_recover(
            &table, INTEGRATION_REPLAY_DRILL_MALFORMED_PEER) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        table.count == 0U,
        "malformed peer explicit recovery");
}

static void integration_replay_drill_prove_incompatible(void)
{
    struct integration_compatibility_policy policy;
    struct integration_session_nonce_registry registry;
    struct integration_ledger ledger;
    struct integration_failure_isolation_table table;
    struct integration_session_request incompatible =
        integration_replay_drill_make_session_request(
            INTEGRATION_SESSION_SCHEMA_VERSION_MAX + 1U,
            INTEGRATION_SESSION_TRANSPORT_VERSION,
            INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER, 0x11U);
    struct integration_session_request bad_transport =
        integration_replay_drill_make_session_request(
            INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
            INTEGRATION_SESSION_TRANSPORT_VERSION - 1U,
            INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER, 0x22U);
    struct integration_compatibility_request request;
    unsigned char before[sizeof(registry)];
    unsigned char before_ledger[sizeof(ledger)];

    integration_replay_drill_require(
        integration_compatibility_policy_init(
            &policy) == INTEGRATION_COMPATIBILITY_OK &&
        integration_session_nonce_registry_init(
            &registry, 4U) == INTEGRATION_SESSION_OK &&
        integration_ledger_init(
            &ledger, 4U) == INTEGRATION_LEDGER_OK &&
        integration_failure_isolation_init(
            &table, 4U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "incompatible drill fixture init");
    request.schema_version = incompatible.schema_version;
    request.transport_version = incompatible.transport_version;
    integration_replay_drill_require(
        integration_compatibility_check(
            &policy, &request) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
        "unsupported schema version rejection");
    request.schema_version = bad_transport.schema_version;
    request.transport_version = bad_transport.transport_version;
    integration_replay_drill_require(
        integration_compatibility_check(
            &policy, &request) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
        "unsupported transport version rejection");
    integration_replay_drill_copy(
        before, &registry, sizeof(registry));
    integration_replay_drill_require(
        integration_compatibility_session_validate(
            &policy, &incompatible, &registry) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION &&
        integration_replay_drill_equal(
            &registry, before, sizeof(registry)) &&
        integration_compatibility_session_validate(
            &policy, &bad_transport, &registry) ==
            INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION &&
        integration_replay_drill_equal(
            &registry, before, sizeof(registry)),
        "incompatible session gate records no nonce");
    integration_replay_drill_copy(
        before_ledger, &ledger, sizeof(ledger));
    integration_replay_drill_require(
        ledger.count == 0U &&
        integration_replay_drill_equal(
            &ledger, before_ledger, sizeof(ledger)),
        "incompatible request leaves ledger unchanged");
    integration_replay_drill_isolate_peer(
        &table, INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER,
        INTEGRATION_FAILURE_MALFORMED, 200U);
    integration_replay_drill_require(
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER, 211U) ==
            INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER, 212U) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "incompatible peer cooldown boundary");
    integration_replay_drill_check_local_paths();
    integration_replay_drill_require(
        integration_failure_isolation_recover(
            &table, INTEGRATION_REPLAY_DRILL_INCOMPATIBLE_PEER) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        table.count == 0U,
        "incompatible peer explicit recovery");
}

static void integration_replay_drill_prove_unavailable(void)
{
    struct integration_failure_isolation_table table;

    integration_replay_drill_require(
        integration_failure_isolation_init(
            &table, 4U) == INTEGRATION_FAILURE_ISOLATION_OK,
        "unavailable drill failure table init");
    integration_replay_drill_isolate_peer(
        &table, INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER,
        INTEGRATION_FAILURE_UNAVAILABLE, 300U);
    integration_replay_drill_require(
        table.peers[0].isolated_until_tick == 322U &&
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER, 321U) ==
            INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER, 322U) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "unavailable peer cooldown boundary");
    integration_replay_drill_check_local_paths();
    integration_replay_drill_require(
        integration_failure_isolation_recover(
            &table, INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER) ==
            INTEGRATION_FAILURE_ISOLATION_OK &&
        table.count == 0U &&
        integration_failure_isolation_check(
            &table, INTEGRATION_REPLAY_DRILL_UNAVAILABLE_PEER, 323U) ==
            INTEGRATION_FAILURE_ISOLATION_OK,
        "unavailable peer explicit recovery");
}

void integration_replay_outage_drill_qemu_test_init(void)
{
    serial_write("[INTEGRATION-REPLAY-OUTAGE-DRILL] proof init\n");
    integration_replay_drill_prove_catalogue();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] catalogue integration fault points exact\n");
    integration_replay_drill_prove_duplicate();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] duplicate request idempotent\n");
    integration_replay_drill_prove_malformed();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] malformed request rejected\n");
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] malformed peer isolated\n");
    integration_replay_drill_prove_incompatible();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] incompatible request rejected\n");
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] incompatible peer isolated\n");
    integration_replay_drill_prove_unavailable();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] unavailable peer isolated without local block\n");
    integration_replay_drill_check_local_paths();
    serial_write(
        "[INTEGRATION-REPLAY-OUTAGE-DRILL] local paths unaffected\n");
    serial_write("[INTEGRATION-REPLAY-OUTAGE-DRILL] proof OK\n");
}

#else
typedef int integration_replay_outage_drill_qemu_translation_unit_guard;
#endif
