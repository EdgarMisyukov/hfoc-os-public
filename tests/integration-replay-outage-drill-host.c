#include "fault_injection.h"
#include "integration_boundary.h"
#include "integration_compatibility.h"
#include "integration_envelope.h"
#include "integration_failure_isolation.h"
#include "integration_ledger.h"
#include "integration_session.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_HOST_TEST

#define DRILL_RESULT 0x1234U
#define DRILL_MALFORMED_PEER 1001U
#define DRILL_INCOMPATIBLE_PEER 1002U
#define DRILL_UNAVAILABLE_PEER 1003U

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

static void fill_correlation_id(uint8_t *correlation_id)
{
    uint32_t index;

    memset(correlation_id, 0, INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE);
    for (index = 0U;
         index < INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE;
         ++index) {
        correlation_id[index] = (uint8_t)(0x41U + index);
    }
}

static bool seal_envelope(
    unsigned char *envelope,
    size_t capacity,
    size_t *length_out,
    struct integration_envelope_layout *layout_out,
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE])
{
    static const unsigned char payload[] = "accounting audit hand-off";
    uint8_t correlation_id[INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE];
    size_t length = 0U;

    fill_correlation_id(correlation_id);
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
    memcpy(identity, envelope + layout_out->digest_offset,
           INTEGRATION_LEDGER_IDENTITY_SIZE);
    *length_out = length;
    return true;
}

static struct integration_ledger make_ledger(uint32_t capacity)
{
    struct integration_ledger ledger;

    memset(&ledger, 0xA5, sizeof(ledger));
    CHECK(integration_ledger_init(&ledger, capacity) ==
              INTEGRATION_LEDGER_OK,
          "integration replay drill initializes ledger");
    return ledger;
}

static struct integration_failure_isolation_table make_table(
    uint32_t capacity)
{
    struct integration_failure_isolation_table table;

    memset(&table, 0xA5, sizeof(table));
    CHECK(integration_failure_isolation_init(&table, capacity) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "integration replay drill initializes failure table");
    return table;
}

static struct integration_compatibility_policy make_policy(void)
{
    struct integration_compatibility_policy policy;

    memset(&policy, 0xA5, sizeof(policy));
    CHECK(integration_compatibility_policy_init(&policy) ==
              INTEGRATION_COMPATIBILITY_OK,
          "integration replay drill initializes compatibility policy");
    return policy;
}

static struct integration_session_nonce_registry make_registry(
    uint32_t capacity)
{
    struct integration_session_nonce_registry registry;

    memset(&registry, 0xA5, sizeof(registry));
    CHECK(integration_session_nonce_registry_init(&registry, capacity) ==
              INTEGRATION_SESSION_OK,
          "integration replay drill initializes session registry");
    return registry;
}

static struct integration_session_request make_session_request(
    uint32_t schema_version,
    uint32_t transport_version,
    uint32_t peer_id,
    uint8_t first_nonce_byte)
{
    struct integration_session_request request;

    memset(&request, 0, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    request.peer_id = peer_id;
    memset(request.nonce, 0, sizeof(request.nonce));
    request.nonce[0] = first_nonce_byte;
    return request;
}

static void capture_ledger(
    const struct integration_ledger *ledger,
    unsigned char *original)
{
    memcpy(original, ledger, sizeof(*ledger));
}

static void check_ledger_unchanged(
    const struct integration_ledger *ledger,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(ledger, original, sizeof(*ledger)) == 0, message);
}

static void capture_registry(
    const struct integration_session_nonce_registry *registry,
    unsigned char *original)
{
    memcpy(original, registry, sizeof(*registry));
}

static void check_registry_unchanged(
    const struct integration_session_nonce_registry *registry,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(registry, original, sizeof(*registry)) == 0, message);
}

static void check_integration_fault_point(
    enum fault_injection_id id,
    const char *expected_outcome)
{
    const struct fault_injection_site *site = NULL;

    CHECK(fault_injection_site_by_id(id, &site) == FAULT_INJECTION_OK &&
              site != NULL &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_INTEGRATION &&
              site->bounded &&
              site->test_only &&
              site->expected_outcome != NULL &&
              strcmp(site->expected_outcome, expected_outcome) == 0,
          "integration replay drill fault point is exact");
}

static void isolate_peer(
    struct integration_failure_isolation_table *table,
    uint32_t peer_id,
    enum integration_failure_kind kind,
    uint64_t start_tick)
{
    CHECK(integration_failure_isolation_record(
              table, peer_id, kind, start_tick) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          integration_failure_isolation_record(
              table, peer_id, kind, start_tick + 1U) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          integration_failure_isolation_record(
              table, peer_id, kind, start_tick + 2U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED,
          "integration replay drill isolates peer after threshold");
}

static void check_local_operations_allowed(void)
{
    CHECK(integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_MANAGEMENT) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_SERVICE_SUPERVISION) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          integration_failure_isolation_local_allowed(
              INTEGRATION_LOCAL_STATE_RECOVERY) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "integration replay drill keeps local paths allowed");
}

static void test_fault_catalogue(void)
{
    check_integration_fault_point(
        FAULT_INJECTION_INTEGRATION_DUPLICATE,
        "duplicate request is idempotent");
    check_integration_fault_point(
        FAULT_INJECTION_INTEGRATION_MALFORMED,
        "malformed request rejected and isolated");
    check_integration_fault_point(
        FAULT_INJECTION_INTEGRATION_INCOMPATIBLE,
        "incompatible request rejected and isolated");
    check_integration_fault_point(
        FAULT_INJECTION_INTEGRATION_UNAVAILABLE,
        "unavailable/slow peer isolated without local block");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration fault catalogue lookup performs no allocation");
}

static void test_duplicate_replay(void)
{
    unsigned char envelope[512];
    unsigned char before[sizeof(struct integration_ledger)];
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_envelope_layout layout;
    struct integration_ledger ledger = make_ledger(4U);
    size_t envelope_length = 0U;
    uint32_t result = 0U;

    CHECK(seal_envelope(
              envelope, sizeof(envelope), &envelope_length,
              &layout, identity),
          "integration replay drill seals and validates envelope");
    CHECK(integration_ledger_prepare(
              &ledger, identity, DRILL_RESULT, &result) ==
              INTEGRATION_LEDGER_OK &&
          result == DRILL_RESULT &&
          ledger.count == 1U &&
          ledger.entries[0].pending &&
          !ledger.entries[0].committed,
          "integration replay drill prepares envelope identity once");

    capture_ledger(&ledger, before);
    result = 0U;
    CHECK(integration_ledger_prepare(
              &ledger, identity, 0x7777U, &result) ==
              INTEGRATION_LEDGER_DUPLICATE &&
          result == DRILL_RESULT,
          "integration replay drill rejects duplicate prepare idempotently");
    check_ledger_unchanged(
        &ledger, before,
        "duplicate prepare leaves ledger unchanged");

    result = 0U;
    CHECK(integration_ledger_commit(
              &ledger, identity, &result) == INTEGRATION_LEDGER_OK &&
          result == DRILL_RESULT &&
          ledger.count == 1U &&
          !ledger.entries[0].pending &&
          ledger.entries[0].committed,
          "integration replay drill commits envelope identity once");

    capture_ledger(&ledger, before);
    result = 0U;
    CHECK(integration_ledger_commit(
              &ledger, identity, &result) ==
              INTEGRATION_LEDGER_ALREADY_COMMITTED &&
          result == DRILL_RESULT,
          "integration replay drill rejects duplicate commit idempotently");
    check_ledger_unchanged(
        &ledger, before,
        "duplicate commit leaves ledger unchanged");

    capture_ledger(&ledger, before);
    result = 0U;
    CHECK(integration_ledger_replay(
              &ledger, identity, &result) == INTEGRATION_LEDGER_OK &&
          result == DRILL_RESULT,
          "integration replay drill replays exact committed result");
    check_ledger_unchanged(
        &ledger, before,
        "read-only replay leaves ledger unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate replay performs no allocation");
}

static void test_malformed_isolation(void)
{
    unsigned char envelope[512];
    unsigned char tampered[512];
    unsigned char before_ledger[sizeof(struct integration_ledger)];
    unsigned char before_layout[sizeof(struct integration_envelope_layout)];
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_envelope_layout layout;
    struct integration_ledger ledger = make_ledger(4U);
    struct integration_failure_isolation_table table = make_table(4U);
    size_t envelope_length = 0U;
    unsigned char short_envelope[
        INTEGRATION_ENVELOPE_MIN_ENVELOPE_LENGTH - 1U];

    memset(short_envelope, 0xA5, sizeof(short_envelope));
    CHECK(seal_envelope(
              envelope, sizeof(envelope), &envelope_length,
              &layout, identity),
          "malformed drill seals valid envelope");

    memcpy(tampered, envelope, envelope_length);
    tampered[layout.digest_offset] ^= 0x01U;
    memset(&layout, 0x5A, sizeof(layout));
    memcpy(before_layout, &layout, sizeof(layout));
    CHECK(integration_envelope_validate(
              tampered, envelope_length, &layout) ==
              INTEGRATION_ENVELOPE_BAD_DIGEST,
          "malformed drill rejects tampered envelope digest");
    CHECK(memcmp(&layout, before_layout, sizeof(layout)) == 0,
          "tampered envelope rejection leaves layout unchanged");
    CHECK(integration_envelope_validate(
              short_envelope, sizeof(short_envelope), &layout) ==
              INTEGRATION_ENVELOPE_TOO_SHORT,
          "malformed drill rejects truncated envelope");

    capture_ledger(&ledger, before_ledger);
    CHECK(ledger.count == 0U,
          "malformed requests do not publish ledger state");
    check_ledger_unchanged(
        &ledger, before_ledger,
        "malformed requests leave ledger unchanged");

    isolate_peer(
        &table, DRILL_MALFORMED_PEER,
        INTEGRATION_FAILURE_MALFORMED, 100U);
    CHECK(integration_failure_isolation_check(
              &table, DRILL_MALFORMED_PEER, 111U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
          integration_failure_isolation_check(
              &table, DRILL_MALFORMED_PEER, 112U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "malformed peer cooldown boundary is exact");
    check_local_operations_allowed();
    CHECK(integration_failure_isolation_recover(
              &table, DRILL_MALFORMED_PEER) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          table.count == 0U,
          "malformed peer explicit recovery reuses slot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed isolation performs no allocation");
}

static void test_incompatible_isolation(void)
{
    struct integration_compatibility_policy policy = make_policy();
    struct integration_session_nonce_registry registry =
        make_registry(4U);
    struct integration_ledger ledger = make_ledger(4U);
    struct integration_failure_isolation_table table = make_table(4U);
    struct integration_session_request incompatible =
        make_session_request(
            INTEGRATION_SESSION_SCHEMA_VERSION_MAX + 1U,
            INTEGRATION_SESSION_TRANSPORT_VERSION,
            DRILL_INCOMPATIBLE_PEER, 0x11U);
    struct integration_session_request bad_transport =
        make_session_request(
            INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
            INTEGRATION_SESSION_TRANSPORT_VERSION - 1U,
            DRILL_INCOMPATIBLE_PEER, 0x22U);
    struct integration_compatibility_request request;
    unsigned char before_registry[sizeof(registry)];
    unsigned char before_ledger[sizeof(ledger)];

    request.schema_version = incompatible.schema_version;
    request.transport_version = incompatible.transport_version;
    CHECK(integration_compatibility_check(
              &policy, &request) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
          "incompatible drill rejects unsupported schema version");
    request.schema_version = bad_transport.schema_version;
    request.transport_version = bad_transport.transport_version;
    CHECK(integration_compatibility_check(
              &policy, &request) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
          "incompatible drill rejects unsupported transport version");

    capture_registry(&registry, before_registry);
    CHECK(integration_compatibility_session_validate(
              &policy, &incompatible, &registry) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_SCHEMA_VERSION,
          "incompatible session gate rejects unsupported schema");
    check_registry_unchanged(
        &registry, before_registry,
        "unsupported schema rejection records no live nonce");
    CHECK(integration_compatibility_session_validate(
              &policy, &bad_transport, &registry) ==
              INTEGRATION_COMPATIBILITY_UNSUPPORTED_TRANSPORT_VERSION,
          "incompatible session gate rejects unsupported transport");
    check_registry_unchanged(
        &registry, before_registry,
        "unsupported transport rejection records no live nonce");
    capture_ledger(&ledger, before_ledger);
    check_ledger_unchanged(
        &ledger, before_ledger,
        "incompatible requests leave ledger unchanged");

    isolate_peer(
        &table, DRILL_INCOMPATIBLE_PEER,
        INTEGRATION_FAILURE_MALFORMED, 200U);
    CHECK(integration_failure_isolation_check(
              &table, DRILL_INCOMPATIBLE_PEER, 211U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
          integration_failure_isolation_check(
              &table, DRILL_INCOMPATIBLE_PEER, 212U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "incompatible peer cooldown boundary is exact");
    check_local_operations_allowed();
    CHECK(integration_failure_isolation_recover(
              &table, DRILL_INCOMPATIBLE_PEER) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          table.count == 0U,
          "incompatible peer explicit recovery reuses slot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "incompatible isolation performs no allocation");
}

static void test_unavailable_isolation(void)
{
    struct integration_failure_isolation_table table = make_table(4U);

    isolate_peer(
        &table, DRILL_UNAVAILABLE_PEER,
        INTEGRATION_FAILURE_UNAVAILABLE, 300U);
    CHECK(table.peers[0].isolated_until_tick == 322U,
          "unavailable peer uses exact twenty-tick cooldown");
    CHECK(integration_failure_isolation_check(
              &table, DRILL_UNAVAILABLE_PEER, 321U) ==
              INTEGRATION_FAILURE_ISOLATION_PEER_ISOLATED &&
          integration_failure_isolation_check(
              &table, DRILL_UNAVAILABLE_PEER, 322U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "unavailable peer cooldown boundary is exact");
    check_local_operations_allowed();
    CHECK(integration_failure_isolation_recover(
              &table, DRILL_UNAVAILABLE_PEER) ==
              INTEGRATION_FAILURE_ISOLATION_OK &&
          table.count == 0U &&
          integration_failure_isolation_check(
              &table, DRILL_UNAVAILABLE_PEER, 323U) ==
              INTEGRATION_FAILURE_ISOLATION_OK,
          "unavailable peer explicit recovery reuses slot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "unavailable isolation performs no allocation");
}

int main(void)
{
    test_fault_catalogue();
    test_duplicate_replay();
    test_malformed_isolation();
    test_incompatible_isolation();
    test_unavailable_isolation();

    if (failures != 0) {
        fprintf(stderr,
                "Integration replay/outage drill host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration replay/outage drill host tests: ALL OK\n");
    return 0;
}

#endif
