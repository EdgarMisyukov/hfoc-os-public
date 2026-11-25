#ifdef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_QEMU_TEST

#include "appliance_production_management.h"
#include "appliance_production_auth.h"
#include "appliance_audit_chain.h"
#include "audit.h"
#include "appliance_production_service_https.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void prod_mgmt_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void prod_mgmt_qemu_failure(const char *message)
    __attribute__((noreturn));

static void prod_mgmt_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PROD-MGMT] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void prod_mgmt_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        prod_mgmt_qemu_failure(message);
    }
}

static bool prod_mgmt_qemu_mem_equal(
    const char *text,
    const char *needle,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (text[index] != needle[index]) {
            return false;
        }
    }
    return true;
}

static bool prod_mgmt_qemu_contains_exact(
    const char *text,
    const char *needle)
{
    size_t text_length = 0U;
    size_t needle_length = 0U;
    size_t index;

    while (text[text_length] != '\0') {
        ++text_length;
    }
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    if (needle_length > text_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= text_length; ++index) {
        if (prod_mgmt_qemu_mem_equal(
                text + index, needle, needle_length)) {
            return true;
        }
    }
    return false;
}

static struct appliance_production_service_https_evidence
prod_mgmt_qemu_service_evidence(void)
{
    struct appliance_production_service_https_evidence evidence;

    prod_mgmt_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = true;
    evidence.manager_stopped = true;
    evidence.https_enabled = true;
    evidence.https_stopped = true;
    return evidence;
}

static struct appliance_production_service_https_readiness
prod_mgmt_qemu_readiness(void)
{
    struct appliance_production_service_https_readiness readiness;

    prod_mgmt_qemu_zero_bytes(&readiness, sizeof(readiness));
    readiness.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION;
    readiness.source_generation = 1U;
    readiness.network_active = true;
    readiness.entropy_ready = true;
    readiness.certificate_ready = true;
    return readiness;
}

static struct appliance_production_management_evidence
prod_mgmt_qemu_management_evidence(
    bool complete,
    bool network_health_ok,
    bool authorization_available)
{
    struct appliance_production_management_evidence evidence;

    prod_mgmt_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.system_identity_present = complete;
    evidence.configuration_ready = complete;
    evidence.network_required = true;
    evidence.network_active = complete;
    evidence.network_health_ok = network_health_ok;
    evidence.production_services_ready = complete;
    evidence.https_management_ready = complete;
    evidence.authorization_available = authorization_available;
    return evidence;
}

/* ITEM-377: build a real hash-chained audit window and prove that the panel
 * view publishes its verified head and, after a rewrite, the TAMPERED state. */
static uint64_t prod_mgmt_qemu_audit_head(
    const uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t value = 0U;
    uint32_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | (uint64_t)digest[index];
    }
    return value;
}

static void prod_mgmt_qemu_audit_hex64(
    uint64_t value,
    char output[17])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        output[index] =
            hex[(value >> ((15U - index) * 4U)) & UINT64_C(0xF)];
    }
    output[16] = '\0';
}

static bool prod_mgmt_qemu_audit_build(
    struct appliance_production_management_evidence *evidence,
    uint64_t *head_out)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain_verdict verdict;
    struct audit_record record;
    uint32_t operation;

    if (appliance_audit_chain_init(&chain, 1U) !=
        APPLIANCE_AUDIT_CHAIN_OK) {
        return false;
    }
    for (operation = 1U; operation <= 3U; ++operation) {
        if (audit_record_init_management(
                &record, operation, 0U, 1U, 1U, operation,
                0x100U + operation) != AUDIT_OK ||
            appliance_audit_chain_append(&chain, &record, NULL) !=
                APPLIANCE_AUDIT_CHAIN_OK) {
            return false;
        }
    }
    if (appliance_audit_chain_verify(&chain, &verdict) !=
            APPLIANCE_AUDIT_CHAIN_OK ||
        !verdict.chain_ok || verdict.checked_count != 3U ||
        verdict.head_sequence != 3U) {
        return false;
    }
    evidence->audit_chain_present = true;
    evidence->audit_chain = APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK;
    evidence->audit_records = 3U;
    evidence->audit_head = prod_mgmt_qemu_audit_head(verdict.head_digest);
    *head_out = evidence->audit_head;

    /* A rewritten record must be detected at its exact sequence. */
    chain.entries[1].record.detail ^= UINT64_C(0x5A5A5A5A);
    if (appliance_audit_chain_verify(&chain, &verdict) !=
            APPLIANCE_AUDIT_CHAIN_OK ||
        verdict.chain_ok || !verdict.tamper_detected ||
        verdict.tamper_sequence != 2U) {
        return false;
    }
    return true;
}

void appliance_production_management_qemu_test_init(void)
{
    static struct appliance_production_service_https service;
    struct appliance_production_service_https_evidence service_evidence =
        prod_mgmt_qemu_service_evidence();
    struct appliance_production_service_https_readiness readiness =
        prod_mgmt_qemu_readiness();
    static struct appliance_production_service_https_snapshot service_snapshot;
    static struct appliance_production_management management;
    struct appliance_production_management_evidence missing =
        prod_mgmt_qemu_management_evidence(false, false, false);
    struct appliance_production_management_evidence complete =
        prod_mgmt_qemu_management_evidence(true, true, false);
    struct appliance_production_management_evidence degraded =
        prod_mgmt_qemu_management_evidence(true, false, false);
    static struct appliance_production_management_snapshot snapshot;
    static struct appliance_production_management_evidence tampered;
    static struct appliance_production_auth auth;
    static struct appliance_admin_auth_credential credential;
    static struct appliance_admin_auth_proof proof;
    static struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route auth_route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    static uint8_t auth_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    static uint8_t auth_header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                       APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    static uint32_t auth_session_id = 0U;
    static uint64_t audit_head = 0U;
    static char audit_head_hex[17];
    static bool auth_available;
    static size_t index;
    static char output[APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX + 1U];
    static size_t output_length = 0U;

    prod_mgmt_qemu_zero_bytes(&service, sizeof(service));
    prod_mgmt_qemu_zero_bytes(&service_snapshot, sizeof(service_snapshot));
    prod_mgmt_qemu_zero_bytes(&management, sizeof(management));
    prod_mgmt_qemu_zero_bytes(&snapshot, sizeof(snapshot));
    prod_mgmt_qemu_zero_bytes(&tampered, sizeof(tampered));
    prod_mgmt_qemu_zero_bytes(audit_head_hex, sizeof(audit_head_hex));
    prod_mgmt_qemu_zero_bytes(&auth, sizeof(auth));
    prod_mgmt_qemu_zero_bytes(&credential, sizeof(credential));
    prod_mgmt_qemu_zero_bytes(&proof, sizeof(proof));
    prod_mgmt_qemu_zero_bytes(&decision, sizeof(decision));
    credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH;
         ++index) {
        credential.verifier[index] = (uint8_t)(0x71U + (uint8_t)index);
        proof.proof[index] = (uint8_t)(0x71U + (uint8_t)index);
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        auth_token[index] = (uint8_t)(0x81U + (uint8_t)index);
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH;
         ++index) {
        auth_header[index] =
            (uint8_t)APPLIANCE_ADMIN_AUTH_BEARER_PREFIX[index];
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        auth_header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH + index] =
            auth_token[index];
    }

    serial_write("[PROD-MGMT] proof init\n");

    prod_mgmt_qemu_require(
        appliance_production_auth_init(&auth) ==
            APPLIANCE_PRODUCTION_AUTH_OK &&
        appliance_production_auth_publish_credential(
            &auth, &credential) == APPLIANCE_PRODUCTION_AUTH_OK &&
        appliance_admin_auth_authenticate_local(
            &auth.admin, &proof, auth_token, 0U, 100U,
            &auth_session_id) == APPLIANCE_ADMIN_AUTH_OK &&
        auth_session_id == 1U &&
        (auth_available = appliance_production_auth_available(&auth)) &&
        appliance_production_auth_authorize_request(
            &auth,
            auth_header,
            sizeof(auth_header),
            HTTP_METHOD_GET,
            (const uint8_t *)
                APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
            sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH) - 1U,
            10U,
            &decision,
            &auth_route) == APPLIANCE_PRODUCTION_AUTH_ALLOWED &&
        decision.authenticated &&
        auth_route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        "production auth request integration active");
    complete.authorization_available = auth_available;
    degraded.authorization_available = auth_available;
    serial_write("[PROD-MGMT] production auth active\n");

    prod_mgmt_qemu_require(
        appliance_production_service_https_init(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_configure(
            &service, &service_evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_readiness(
            &service, &readiness) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_start(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_running(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_snapshot(
            &service, &service_snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        service_snapshot.state ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
        service_snapshot.evidence_published,
        "production service https evidence active");
    serial_write("[PROD-MGMT] service https evidence active\n");

    prod_mgmt_qemu_require(
        appliance_production_management_init(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_publish(
            &management, &missing) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE,
        "missing evidence blocks management publication");
    serial_write("[PROD-MGMT] missing evidence blocks publication\n");

    prod_mgmt_qemu_require(
        appliance_production_management_publish(
            &management, &complete) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE &&
        appliance_production_management_snapshot(
            &management, &snapshot) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        prod_mgmt_qemu_contains_exact(
            output,
            "production_management v=1 state=ACTIVE "
            "identity=PRESENT config=COMMITTED network=ACTIVE "
            "services=ACTIVE https=READY authorization=AVAILABLE "
            "attestation=UNAVAILABLE health=PASS"),
        "canonical production management snapshot");
    serial_write("[PROD-MGMT] canonical snapshot\n");

    prod_mgmt_qemu_require(
        prod_mgmt_qemu_audit_build(&complete, &audit_head),
        "audit chain and tamper detection");
    prod_mgmt_qemu_audit_hex64(audit_head, audit_head_hex);
    prod_mgmt_qemu_require(
        appliance_production_management_publish(
            &management, &complete) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_snapshot(
            &management, &snapshot) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        snapshot.audit_chain ==
            APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK &&
        snapshot.audit_records == 3U &&
        snapshot.audit_head == audit_head &&
        appliance_production_management_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        prod_mgmt_qemu_contains_exact(
            output, "audit_chain=OK audit_records=3 audit_head=") &&
        prod_mgmt_qemu_contains_exact(output, audit_head_hex),
        "audit integrity view published in the panel");
    serial_write("[PROD-MGMT] audit integrity view\n");

    tampered = complete;
    tampered.audit_chain = APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED;
    prod_mgmt_qemu_require(
        appliance_production_management_publish(
            &management, &tampered) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_snapshot(
            &management, &snapshot) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        snapshot.audit_chain ==
            APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED &&
        appliance_production_management_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        prod_mgmt_qemu_contains_exact(
            output, "audit_chain=TAMPERED audit_records=3"),
        "tampered audit view published in the panel");
    serial_write("[PROD-MGMT] audit tamper view\n");

    prod_mgmt_qemu_require(
        appliance_production_management_publish(
            &management, &degraded) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED &&
        appliance_production_management_snapshot(
            &management, &snapshot) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        snapshot.health ==
            APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_WARN,
        "network warning publishes degraded management");
    serial_write("[PROD-MGMT] degraded health published\n");

    prod_mgmt_qemu_require(
        appliance_production_management_fail_closed(
            &management,
            APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NETWORK) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED &&
        !appliance_production_management_published(&management),
        "network failure fails closed");
    serial_write("[PROD-MGMT] failure fails closed\n");

    prod_mgmt_qemu_require(
        appliance_production_management_reset(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_stop(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED,
        "reset and clean stop");
    serial_write("[PROD-MGMT] clean shutdown\n");
    serial_write("[PROD-MGMT] proof OK\n");
}

#else
typedef int appliance_production_management_qemu_translation_unit_guard;
#endif
