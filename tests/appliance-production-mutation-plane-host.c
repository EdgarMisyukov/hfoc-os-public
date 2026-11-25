#include "appliance_production_mutation_plane.h"
#include "appliance_route_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_MUTATION_PLANE_HOST_TEST

static int failures;

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
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    return false;
}

static void make_credential(
    struct appliance_admin_auth_credential *credential,
    uint8_t seed)
{
    size_t index;

    memset(credential, 0, sizeof(*credential));
    credential->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(credential->verifier); ++index) {
        credential->verifier[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_token(
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t seed)
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_proof(
    struct appliance_admin_auth_proof *proof,
    uint8_t seed)
{
    size_t index;

    memset(proof, 0, sizeof(*proof));
    proof->version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    for (index = 0U; index < sizeof(proof->proof); ++index) {
        proof->proof[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static void make_bearer_header(
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    memcpy(header, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static void make_evidence(
    struct appliance_production_management_evidence *evidence,
    uint64_t generation,
    bool degraded)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence->published = true;
    evidence->source_generation = generation;
    evidence->system_identity_present = true;
    evidence->configuration_ready = true;
    evidence->network_required = true;
    evidence->network_active = true;
    evidence->network_health_ok = true;
    evidence->production_services_ready = true;
    evidence->https_management_ready = true;
    evidence->authorization_available = true;
    evidence->degraded_requested = degraded;
}

static void make_initial_config(struct network_ipv4_config *config)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config->address.octets[0] = 10U;
    config->address.octets[1] = 0U;
    config->address.octets[2] = 2U;
    config->address.octets[3] = 15U;
    config->prefix_length = 24U;
    config->gateway.octets[0] = 10U;
    config->gateway.octets[1] = 0U;
    config->gateway.octets[2] = 2U;
    config->gateway.octets[3] = 2U;
    config->dns.octets[0] = 10U;
    config->dns.octets[1] = 0U;
    config->dns.octets[2] = 2U;
    config->dns.octets[3] = 3U;
}

static bool config_is_initial(const struct network_ipv4_config *config)
{
    return config != NULL &&
           config->address.octets[3] == 15U &&
           config->dns.octets[3] == 3U;
}

static bool config_is_staged(const struct network_ipv4_config *config)
{
    return config != NULL &&
           config->address.octets[3] == 77U &&
           config->dns.octets[3] == 53U;
}

static const char staged_body[] =
    "schema_version=1\n"
    "ipv4_address=10.0.2.77\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=10.0.2.2\n"
    "ipv4_dns=10.0.2.53\n";

static void test_init_and_fail_closed(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct network_config_transaction network;
    struct network_ipv4_config initial;
    struct appliance_production_mutation_plane mutation;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t network_target[] = "/api/v1/network";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&network, 0, sizeof(network));
    memset(&mutation, 0, sizeof(mutation));
    make_initial_config(&initial);
    if (network_config_transaction_init(&network, &initial) !=
        NETWORK_CONFIG_TRANSACTION_OK) {
        fprintf(stderr, "FAIL: transaction bootstrap\n");
        ++failures;
        return;
    }
    CHECK(appliance_production_mutation_plane_init(NULL, &auth, &plane,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL,
          "mutation plane init rejects null plane");
    CHECK(appliance_production_mutation_plane_init(&mutation, NULL, &plane,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL,
          "mutation plane init rejects null auth");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "mutation plane accepts an unbound read-only management plane");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, &plane,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "mutation plane accepts an unbound network transaction");
    CHECK(appliance_production_mutation_plane_init(&mutation, NULL, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL,
          "mutation plane init rejects an unbound administrator gate");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, &plane,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "mutation plane binds existing compositions");
    CHECK(appliance_production_mutation_plane_fail_closed(NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL,
          "fail-closed rejects null plane");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, NULL, 0U,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 1U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE,
          "unpublished credential fails the mutation closed");
    CHECK(network_config_transaction_has_pending(&network) == false,
          "unavailable mutation never stages a pending change");
    CHECK(appliance_production_mutation_plane_ready(&mutation) == false,
          "mutation plane is not ready before auth and evidence");
}

static void test_confirmation_cancel_and_apply(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct network_config_transaction network;
    struct network_ipv4_config initial;
    struct appliance_production_mutation_plane mutation;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    struct appliance_production_mutation_plane_audit_entry entry;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    const struct network_ipv4_config *active;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t network_target[] = "/api/v1/network";
    static const uint8_t service_target[] = "/api/v1/service";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&network, 0, sizeof(network));
    memset(&mutation, 0, sizeof(mutation));
    make_initial_config(&initial);
    CHECK(network_config_transaction_init(&network, &initial) ==
              NETWORK_CONFIG_TRANSACTION_OK,
          "network transaction bootstraps with the active config");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "production auth composition initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "read-only management plane binds the auth gate");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, &plane,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "mutation plane binds auth, management evidence and transaction");

    make_credential(&credential, 0x40U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "administrator credential publishes");
    make_proof(&proof, 0x40U);
    make_token(token, 0x90U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "ready auth and evidence refresh the mutation plane");
    CHECK(appliance_production_mutation_plane_state(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_READY,
          "mutation plane publishes READY honestly");
    CHECK(appliance_production_mutation_plane_ready(&mutation) == true,
          "ready mutation plane admits authorized mutations");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "authorized mutation stages a pending change");
    CHECK(network_config_transaction_has_pending(&network) == true,
          "staged change is pending and not applied");
    active = network_config_transaction_active(&network);
    CHECK(config_is_initial(active),
          "staging never mutates the active configuration");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "staged mutation issues a non-zero confirmation token");
    CHECK(appliance_production_mutation_plane_state(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION,
          "pending mutation publishes PENDING_CONFIRMATION");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              confirmation_token + 1U, false, 11U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "wrong confirmation token is rejected");
    active = network_config_transaction_active(&network);
    CHECK(config_is_initial(active),
          "rejected confirmation applies nothing");
    CHECK(network_config_transaction_has_pending(&network) == true,
          "rejected confirmation keeps the pending change");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              confirmation_token, false, 12U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "matching confirmation token applies the mutation");
    active = network_config_transaction_active(&network);
    CHECK(config_is_staged(active),
          "confirmed mutation swaps the active configuration");
    CHECK(network_config_transaction_has_pending(&network) == false,
          "applied mutation clears the pending change");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 1U,
          "applied mutation is counted");
    CHECK(appliance_production_mutation_plane_audit_count(&mutation) > 0U,
          "applied mutation is audited");
    CHECK(appliance_production_mutation_plane_audit_entry(
              &mutation,
              appliance_production_mutation_plane_audit_count(&mutation) - 1U,
              &entry) == true &&
              entry.outcome ==
                  APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED &&
              entry.mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG &&
              entry.session_id == session_id,
          "audit entry records the authorized mutation outcome");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 20U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "second mutation stages again");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              appliance_production_mutation_plane_pending_token(&mutation),
              true, 21U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "explicit cancel discards the pending mutation");
    CHECK(network_config_transaction_has_pending(&network) == false,
          "cancelled mutation clears the pending change");
    active = network_config_transaction_active(&network);
    CHECK(config_is_staged(active),
          "cancelled mutation leaves the active configuration untouched");
    CHECK(appliance_production_mutation_plane_cancelled_count(&mutation) == 1U,
          "cancelled mutation is counted");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, true, 22U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_NO_PENDING,
          "cancel without a pending mutation is rejected");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              service_target, sizeof(service_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 23U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "service mutation without an executor is refused, not faked");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, NULL, 0U,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 24U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
          "anonymous mutation is denied");
    CHECK(appliance_production_mutation_plane_denied_count(&mutation) == 1U,
          "denied mutation is counted");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              "schema_version=1\nipv4_address=999.0.0.1\n"
              "ipv4_prefix=24\nipv4_gateway=10.0.2.2\n"
              "ipv4_dns=10.0.2.3\n",
              sizeof("schema_version=1\nipv4_address=999.0.0.1\n"
                     "ipv4_prefix=24\nipv4_gateway=10.0.2.2\n"
                     "ipv4_dns=10.0.2.3\n") - 1U,
              0U, false, 25U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "invalid mutation body fails safely");
    CHECK(network_config_transaction_has_pending(&network) == false,
          "failed mutation leaves no pending state");
    active = network_config_transaction_active(&network);
    CHECK(config_is_staged(active),
          "failed mutation never partially applies");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 30U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "mutation stages before the confirmation window closes");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              appliance_production_mutation_plane_pending_token(&mutation),
              false,
              30U + APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS +
                  1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED,
          "confirmation after the window expires safely");
    CHECK(network_config_transaction_has_pending(&network) == false,
          "expired confirmation rolls the pending change back");
    active = network_config_transaction_active(&network);
    CHECK(config_is_staged(active),
          "expired confirmation leaves the active configuration untouched");
    CHECK(appliance_production_mutation_plane_expired_count(&mutation) == 1U,
          "expired confirmation is counted");
    /*
     * ITEM-504: `now_tick` is milliseconds, and the token is confirmed by a
     * person on the panel page that printed it.  A window shorter than a
     * minute would expire between the response and the click; the constant
     * names the unit so the next reader does not take "ticks" for seconds
     * again.
     */
    CHECK(APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS >=
              60000U,
          "the confirmation window leaves a person time to confirm");

    CHECK(appliance_production_mutation_plane_stop(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "mutation plane stops");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 200U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_STATE,
          "stopped mutation plane rejects further mutations");
}


/*
 * ITEM-503: the certificate-import executor binding.  The plane owns the
 * authorization, the single-use token, the window and the audit; the bound
 * executor is called exactly once per accepted step and never on a refusal.
 */
struct certificate_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    bool refuse_stage;
    bool fail_commit;
};

static enum appliance_production_mutation_plane_certificate_status
fixture_certificate_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct certificate_executor_fixture *fixture = context;

    ++fixture->stage_calls;
    if (fixture->refuse_stage || body == NULL || body_length == 0U) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_REFUSED;
    }
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK;
}

static enum appliance_production_mutation_plane_certificate_status
fixture_certificate_commit(void *context)
{
    struct certificate_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->fail_commit
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_APPLY_FAILED
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK;
}

static void fixture_certificate_rollback(void *context)
{
    struct certificate_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

static void test_certificate_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct network_config_transaction network;
    struct network_ipv4_config initial;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_certificate_executor executor;
    struct certificate_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    struct appliance_production_mutation_plane_audit_entry entry;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t certificate_target[] = "/panel/security";
    static const uint8_t network_target[] = "/api/v1/network";
    static const char staged_body[] =
        "schema_version=1\nipv4_address=10.0.2.44\nipv4_prefix=24\n"
        "ipv4_gateway=10.0.2.2\nipv4_dns=10.0.2.3\n";
    static const uint8_t certificate_body[] =
        "certificate=-----BEGIN%20CERTIFICATE-----%0AMIIB&key=-----BEGIN"
        "%20PRIVATE%20KEY-----%0AMIIB";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&network, 0, sizeof(network));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    make_initial_config(&initial);
    CHECK(network_config_transaction_init(&network, &initial) ==
              NETWORK_CONFIG_TRANSACTION_OK,
          "certificate executor: network transaction bootstraps");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "certificate executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "certificate executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "certificate executor: read-only plane binds");
    /*
     * The production composition binds no read-only management plane and no
     * network transaction; the executor is what makes the certificate
     * mutation available, and everything else stays refused by name.
     */
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate executor: the plane binds without a management plane");
    make_credential(&credential, 0x40U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "certificate executor: administrator credential publishes");
    make_proof(&proof, 0x40U);
    make_token(token, 0x90U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "certificate executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "certificate executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate executor: the plane refreshes with auth alone");

    /* An unbound executor refuses the mutation instead of faking it. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "certificate executor: an unbound executor is refused");
    CHECK(fixture.stage_calls == 0U,
          "certificate executor: the refusal calls no executor");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_certificate_stage;
    executor.commit = fixture_certificate_commit;
    executor.rollback = fixture_certificate_rollback;
    CHECK(appliance_production_mutation_plane_bind_certificate(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate executor: an authorized import stages");
    CHECK(fixture.stage_calls == 1U && fixture.commit_calls == 0U,
          "certificate executor: staging calls the stage once and never the "
          "commit");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "certificate executor: the staged import issues a token");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              confirmation_token + 1U, false, 12U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "certificate executor: a wrong token applies nothing");
    CHECK(fixture.commit_calls == 0U,
          "certificate executor: a wrong token calls no commit");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              confirmation_token, false, 13U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "certificate executor: the confirmed import applies");
    CHECK(fixture.commit_calls == 1U,
          "certificate executor: the confirmation calls the commit once");
    CHECK(appliance_production_mutation_plane_audit_entry(
              &mutation,
              appliance_production_mutation_plane_audit_count(&mutation) - 1U,
              &entry) == true &&
              entry.outcome ==
                  APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED &&
              entry.mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT,
          "certificate executor: the applied import is audited as the "
          "certificate mutation");

    /* A refused stage and a failed commit both roll back and apply nothing. */
    fixture.refuse_stage = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 14U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "certificate executor: a refused candidate fails the stage");
    CHECK(fixture.commit_calls == 1U,
          "certificate executor: a refused candidate is never committed");
    fixture.refuse_stage = false;

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 15U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate executor: a second import stages");
    fixture.fail_commit = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              appliance_production_mutation_plane_pending_token(&mutation),
              false, 16U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "certificate executor: a failed commit fails the import");
    fixture.fail_commit = false;

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 17U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate executor: a cancelled import stages");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              appliance_production_mutation_plane_pending_token(&mutation),
              true, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "certificate executor: the explicit cancel discards the import");
    CHECK(fixture.rollback_calls > 0U,
          "certificate executor: the cancel rolls the candidate back");

    /* The network mutation stays refused: no transaction is bound. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 19U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "certificate executor: an unbound network mutation is refused");

    CHECK(appliance_production_mutation_plane_bind_certificate(
              &mutation, NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate executor: the executor unbinds");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              certificate_target, sizeof(certificate_target) - 1U,
              certificate_body, sizeof(certificate_body) - 1U,
              0U, false, 20U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "certificate executor: the unbound import is refused again");
}

/*
 * ITEM-504: the routing-configuration executor binding.  The real candidate
 * pair is behind the adapter, so this proof covers the composition the serving
 * path will bind: a stage holds a candidate, the applied table only changes on
 * the exact token, and a refusal or a cancel leaves it exactly as it was.
 */
struct routing_executor_fixture {
    struct appliance_route_config config;
    uint32_t rollback_calls;
};

static enum appliance_production_mutation_plane_routing_status
fixture_routing_stage(void *context, const uint8_t *body, size_t body_length)
{
    struct routing_executor_fixture *fixture = context;
    static const uint8_t form_type[] = "application/x-www-form-urlencoded";
    enum appliance_route_config_status status;

    if (body == NULL || body_length == 0U || body_length > 0xFFFFFFFFU) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_REFUSED;
    }
    status = appliance_route_config_stage(
        &fixture->config, form_type, (uint32_t)(sizeof(form_type) - 1U),
        body, (uint32_t)body_length);
    return status == APPLIANCE_ROUTE_CONFIG_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_REFUSED;
}

static enum appliance_production_mutation_plane_routing_status
fixture_routing_commit(void *context)
{
    struct routing_executor_fixture *fixture = context;

    return appliance_route_config_commit(&fixture->config) ==
                   APPLIANCE_ROUTE_CONFIG_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_APPLY_FAILED;
}

static void fixture_routing_rollback(void *context)
{
    struct routing_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
    (void)appliance_route_config_rollback(&fixture->config);
}

static void test_routing_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct network_config_transaction network;
    struct network_ipv4_config initial;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_routing_executor executor;
    struct routing_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    struct appliance_production_mutation_plane_audit_entry entry;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t routes_target[] = "/api/v1/system/routes";
    static const uint8_t panel_target[] = "/panel/routing";
    static const uint8_t network_target[] = "/api/v1/network";
    static const char add_body[] =
        "name=example.com&backend=10.0.0.5&port=8443&tls=on";
    static const char remove_body[] = "action=remove&name=example.com";
    static const char refused_body[] =
        "name=EXAMPLE.com&backend=10.0.0.9&port=443";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&network, 0, sizeof(network));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    appliance_route_config_init(&fixture.config);
    make_initial_config(&initial);
    CHECK(network_config_transaction_init(&network, &initial) ==
              NETWORK_CONFIG_TRANSACTION_OK,
          "routing executor: network transaction bootstraps");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "routing executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "routing executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "routing executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "routing executor: the plane binds without a management plane");
    make_credential(&credential, 0x50U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "routing executor: administrator credential publishes");
    make_proof(&proof, 0x50U);
    make_token(token, 0xa0U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "routing executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "routing executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "routing executor: the plane refreshes with auth alone");

    /* An unbound executor refuses the mutation instead of faking it. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              add_body, sizeof(add_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "routing executor: an unbound executor is refused");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 0U,
          "routing executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_routing_stage;
    executor.commit = fixture_routing_commit;
    executor.rollback = fixture_routing_rollback;
    CHECK(appliance_production_mutation_plane_bind_routing(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "routing executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              add_body, sizeof(add_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "routing executor: an authorized route stages");
    CHECK(appliance_route_config_staged(&fixture.config),
          "routing executor: the staged candidate is held");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 0U,
          "routing executor: staging changes nothing applied");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "routing executor: the staged route issues a token");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              add_body, sizeof(add_body) - 1U,
              confirmation_token + 1U, false, 12U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "routing executor: a wrong token applies nothing");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 0U,
          "routing executor: a wrong token keeps the applied table");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              panel_target, sizeof(panel_target) - 1U,
              add_body, sizeof(add_body) - 1U,
              confirmation_token, false, 13U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "routing executor: the panel target confirms the same mutation");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 1U,
          "routing executor: the confirmed route is applied");
    CHECK(!appliance_route_config_staged(&fixture.config),
          "routing executor: the candidate is gone after the commit");
    CHECK(appliance_production_mutation_plane_audit_entry(
              &mutation,
              appliance_production_mutation_plane_audit_count(&mutation) - 1U,
              &entry) == true &&
              entry.outcome ==
                  APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED &&
              entry.mutation ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG,
          "routing executor: the applied route is audited as ROUTING_CONFIG");

    /* A refused stage fails, rolls back and keeps the applied table. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              refused_body, sizeof(refused_body) - 1U,
              0U, false, 14U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "routing executor: a duplicate route fails the stage");
    CHECK(appliance_route_config_last_route_status(&fixture.config) ==
              APPLIANCE_ROUTE_DUPLICATE_NAME,
          "routing executor: the failure names DUPLICATE_NAME");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 1U,
          "routing executor: a refused route keeps the applied table");

    /* A staged remove is cancelled and the applied table stays. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              remove_body, sizeof(remove_body) - 1U,
              0U, false, 15U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "routing executor: a remove stages");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              remove_body, sizeof(remove_body) - 1U,
              appliance_production_mutation_plane_pending_token(&mutation),
              true, 16U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "routing executor: the explicit cancel discards the remove");
    CHECK(fixture.rollback_calls > 0U,
          "routing executor: the cancel rolls the candidate back");
    CHECK(appliance_route_config_applied(&fixture.config)->count == 1U,
          "routing executor: a cancelled remove keeps the applied table");

    /* The routing mutation is refused while another mutation is bound. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              remove_body, sizeof(remove_body) - 1U,
              0U, false, 17U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "routing executor: the unbound network mutation stays refused");

    CHECK(appliance_production_mutation_plane_bind_routing(&mutation, NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "routing executor: the executor unbinds");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              routes_target, sizeof(routes_target) - 1U,
              add_body, sizeof(add_body) - 1U,
              0U, false, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "routing executor: the unbound routing mutation is refused again");
}

struct network_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    size_t last_stage_length;
    bool commit_ok;
};

static enum appliance_production_mutation_plane_network_status
fixture_network_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct network_executor_fixture *fixture = context;

    ++fixture->stage_calls;
    fixture->last_stage_length = body_length;
    return body != NULL && body_length != 0U
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_REFUSED;
}

static enum appliance_production_mutation_plane_network_status
fixture_network_commit(void *context)
{
    struct network_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->commit_ok
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_APPLY_FAILED;
}

static void fixture_network_rollback(void *context)
{
    struct network_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

/*
 * ITEM-505: the network mutation runs through the bound executor when one
 * exists, with the durable record on the other side of the interface.  The
 * test proves that the executor - not the borrowed RAM-only transaction -
 * receives the stage, the commit and the rollback, and that a failed commit is
 * answered as APPLY_FAILED while the previously stored value stays in force.
 */
static void test_network_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct network_config_transaction network;
    struct network_ipv4_config initial;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_network_executor executor;
    struct network_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t network_target[] = "/api/v1/network";
    static const uint8_t panel_target[] = "/panel/network";
    static const char stage_body[] = "mode=dhcp";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&network, 0, sizeof(network));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    fixture.commit_ok = true;
    make_initial_config(&initial);
    CHECK(network_config_transaction_init(&network, &initial) ==
              NETWORK_CONFIG_TRANSACTION_OK,
          "network executor: the RAM-only transaction bootstraps");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "network executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "network executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "network executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "network executor: the plane binds with no transaction at all");
    make_credential(&credential, 0x60U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "network executor: administrator credential publishes");
    make_proof(&proof, 0x60U);
    make_token(token, 0xb0U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "network executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "network executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "network executor: the plane refreshes with auth alone");

    /* Neither a transaction nor an executor: the mutation is refused. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "network executor: an unbound network mutation is refused");
    CHECK(fixture.stage_calls == 0U,
          "network executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_network_stage;
    executor.commit = fixture_network_commit;
    executor.rollback = fixture_network_rollback;
    CHECK(appliance_production_mutation_plane_bind_network_executor(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "network executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "network executor: an authorized candidate stages");
    CHECK(fixture.stage_calls == 1U &&
              fixture.last_stage_length == sizeof(stage_body) - 1U,
          "network executor: the executor received the exact body");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "network executor: the staged candidate issues a token");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token + 1U, false, 12U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "network executor: the wrong token is rejected");
    CHECK(fixture.commit_calls == 0U,
          "network executor: the wrong token commits nothing");

    /* A commit the durable record refuses is APPLY_FAILED, not APPLIED. */
    fixture.commit_ok = false;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 13U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "network executor: a failed durable write is APPLY_FAILED");
    CHECK(fixture.commit_calls == 1U && fixture.rollback_calls == 1U,
          "network executor: the failed commit rolls the candidate back");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 0U,
          "network executor: a failed commit counts no applied change");

    /* The same stage with a writable record applies. */
    fixture.commit_ok = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 14U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "network executor: the candidate stages again");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 15U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "network executor: the confirmed candidate applies");
    CHECK(fixture.commit_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "network executor: the executor committed once and the plane "
          "counted it");

    /* The explicit cancel rolls the second candidate back, unapplied. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 16U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "network executor: a third candidate stages");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              network_target, sizeof(network_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, true, 17U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "network executor: the explicit cancel is reported");
    CHECK(fixture.rollback_calls == 2U,
          "network executor: the cancel rolls the candidate back");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 1U,
          "network executor: a cancelled candidate applies nothing");

    /*
     * The panel's own form target is the same mutation.  It is POST-only:
     * a PUT of the page target is denied by the authorization matrix instead
     * of reaching the executor.
     */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "network executor: the panel form target stages the same mutation");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 19U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
          "network executor: the panel form target refuses a non-POST method");

    /*
     * The borrowed RAM-only transaction stays the fallback when no executor
     * is bound, so the existing compositions keep their behavior.
     */
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   &network) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "network executor: the plane binds the RAM-only transaction");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "network executor: the fallback plane refreshes");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              network_target, sizeof(network_target) - 1U,
              staged_body, sizeof(staged_body) - 1U,
              0U, false, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "network executor: the unbound executor falls back to the "
          "transaction");
    CHECK(network_config_transaction_has_pending(&network) == true,
          "network executor: the fallback transaction received the stage");
}

struct system_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    size_t last_stage_length;
    bool commit_ok;
};

static enum appliance_production_mutation_plane_system_status
fixture_system_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct system_executor_fixture *fixture = context;

    ++fixture->stage_calls;
    fixture->last_stage_length = body_length;
    return body != NULL && body_length != 0U
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
}

static enum appliance_production_mutation_plane_system_status
fixture_system_commit(void *context)
{
    struct system_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->commit_ok
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED;
}

static void fixture_system_rollback(void *context)
{
    struct system_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

/*
 * ITEM-505 increment 3: the hostname mutation runs through the bound executor
 * when one exists.  The test proves that the executor - not the borrowed
 * RAM-only transaction - receives the stage, the commit and the rollback, that
 * an unbound executor refuses the mutation instead of faking a write, and that
 * a failed commit is answered as APPLY_FAILED while the previously stored name
 * stays in force.
 */
static void test_system_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_system_executor executor;
    struct system_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t system_target[] = "/api/v1/system";
    static const uint8_t panel_target[] = "/panel/system";
    static const char stage_body[] = "hostname=node-505";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    fixture.commit_ok = true;
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "system executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "system executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "system executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "system executor: the plane binds with no transaction");
    make_credential(&credential, 0x61U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "system executor: administrator credential publishes");
    make_proof(&proof, 0x61U);
    make_token(token, 0xb1U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "system executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "system executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "system executor: the plane refreshes with auth alone");

    /* No executor: the mutation is refused and nothing is staged. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "system executor: an unbound hostname mutation is refused");
    CHECK(fixture.stage_calls == 0U,
          "system executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_system_stage;
    executor.commit = fixture_system_commit;
    executor.rollback = fixture_system_rollback;
    CHECK(appliance_production_mutation_plane_bind_system(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "system executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "system executor: an authorized hostname stages");
    CHECK(fixture.stage_calls == 1U &&
              fixture.last_stage_length == sizeof(stage_body) - 1U,
          "system executor: the executor received the exact body");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "system executor: the staged hostname issues a token");
    CHECK(appliance_production_mutation_plane_pending_mutation(&mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME,
          "system executor: the plane names the hostname mutation");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token + 1U, false, 12U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "system executor: the wrong token is rejected");
    CHECK(fixture.commit_calls == 0U,
          "system executor: the wrong token commits nothing");

    /* A commit the durable record refuses is APPLY_FAILED, not APPLIED. */
    fixture.commit_ok = false;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 13U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "system executor: a failed durable write is APPLY_FAILED");
    CHECK(fixture.commit_calls == 1U && fixture.rollback_calls == 1U,
          "system executor: the failed commit rolls the candidate back");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 0U,
          "system executor: a failed commit counts no applied change");

    /* The same stage with a writable record applies. */
    fixture.commit_ok = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 14U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "system executor: the hostname stages again");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 15U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "system executor: the confirmed hostname applies");
    CHECK(fixture.commit_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "system executor: the executor committed once and the plane "
          "counted it");

    /* The explicit cancel rolls the candidate back, unapplied. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 16U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "system executor: a third hostname stages");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, true, 17U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "system executor: the explicit cancel cancels the candidate");
    CHECK(fixture.rollback_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "system executor: the cancel rolled back and applied nothing");

    /* The panel form target is the same mutation; there is no PUT form. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
          "system executor: the panel form target refuses a non-POST method");

    /* Clearing the binding refuses the mutation again. */
    CHECK(appliance_production_mutation_plane_bind_system(&mutation, NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "system executor: the executor binding clears");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              system_target, sizeof(system_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 19U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "system executor: the unbound hostname mutation is refused again");
}

/*
 * ITEM-506 increment 6: the credential-change mutation runs through the bound
 * executor when one exists.  The test proves that the executor receives the
 * stage, the commit and the rollback, that an unbound executor refuses the
 * mutation instead of faking a change, that a wrong token confirms nothing and
 * that a failed commit is answered APPLY_FAILED while the account keeps its
 * previous credential.
 */
struct account_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    size_t last_stage_length;
    uint8_t last_stage_body[128];
    bool commit_ok;
};

static enum appliance_production_mutation_plane_account_status
fixture_account_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct account_executor_fixture *fixture = context;

    ++fixture->stage_calls;
    fixture->last_stage_length = body_length;
    memset(fixture->last_stage_body, 0, sizeof(fixture->last_stage_body));
    if (body != NULL && body_length != 0U &&
        body_length <= sizeof(fixture->last_stage_body)) {
        memcpy(fixture->last_stage_body, body, body_length);
    }
    return body != NULL && body_length != 0U
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
}

static enum appliance_production_mutation_plane_account_status
fixture_account_commit(void *context)
{
    struct account_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->commit_ok
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED;
}

static void fixture_account_rollback(void *context)
{
    struct account_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

static void test_account_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_account_executor executor;
    struct account_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t account_target[] = "/api/v1/system/account";
    static const uint8_t panel_target[] = "/panel/account";
    static const char stage_body[] =
        "current=old-secret&login=operator&password=new-secret"
        "&repeat=new-secret";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    fixture.commit_ok = true;
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "account executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "account executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "account executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "account executor: the plane binds with no transaction");
    make_credential(&credential, 0x71U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "account executor: administrator credential publishes");
    make_proof(&proof, 0x71U);
    make_token(token, 0xc1U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "account executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "account executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "account executor: the plane refreshes with auth alone");

    /* No executor: the mutation is refused and nothing is staged. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "account executor: an unbound credential change is refused");
    CHECK(fixture.stage_calls == 0U,
          "account executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_account_stage;
    executor.commit = fixture_account_commit;
    executor.rollback = fixture_account_rollback;
    CHECK(appliance_production_mutation_plane_bind_account(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "account executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "account executor: an authorized change stages");
    CHECK(fixture.stage_calls == 1U &&
              fixture.last_stage_length == sizeof(stage_body) - 1U &&
              memcmp(fixture.last_stage_body, stage_body,
                     sizeof(stage_body) - 1U) == 0,
          "account executor: the executor received the exact four-field body");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U &&
              appliance_production_mutation_plane_pending_mutation(
                  &mutation) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL,
          "account executor: the plane names the credential change");

    /* The wrong token confirms nothing. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token + 1U, false, 12U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "account executor: the wrong token is rejected");
    CHECK(fixture.commit_calls == 0U,
          "account executor: the wrong token commits nothing");

    /*
     * ITEM-506 increment 6: the token confirms the candidate the plane holds
     * and nothing else.  Posting it to another mutation target must refuse
     * without committing either mutation, and must keep the candidate alive
     * for its own valid target.
     */
    {
        static const uint8_t other_target[] = "/api/v1/system/routes";

        output_length = 0U;
        CHECK(appliance_production_mutation_plane_request(
                  &mutation, header, sizeof(header),
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
                  other_target, sizeof(other_target) - 1U,
                  stage_body, sizeof(stage_body) - 1U,
                  confirmation_token, false, 12U, output, sizeof(output),
                  &output_length) ==
                  APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
              "account executor: the token confirms nothing on another target");
        CHECK(fixture.commit_calls == 0U &&
                  fixture.rollback_calls == 0U &&
                  appliance_production_mutation_plane_pending_mutation(
                      &mutation) ==
                      APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL,
              "account executor: the misdirected confirmation keeps the "
              "candidate");
    }

    /* A commit the owner refuses is APPLY_FAILED, not APPLIED. */
    fixture.commit_ok = false;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 13U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "account executor: a refused change is APPLY_FAILED");
    CHECK(fixture.commit_calls == 1U && fixture.rollback_calls == 1U,
          "account executor: the failed commit rolls the candidate back");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 0U,
          "account executor: a failed change counts no applied mutation");

    /* The same stage with a writable owner applies. */
    fixture.commit_ok = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 14U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "account executor: the change stages again");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 15U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "account executor: the confirmed change applies");
    CHECK(fixture.commit_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "account executor: the executor committed once and the plane "
          "counted it");

    /* The explicit cancel rolls the candidate back, unapplied. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 16U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "account executor: a third change stages");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, true, 17U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "account executor: the explicit cancel cancels the candidate");
    CHECK(fixture.rollback_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "account executor: the cancel rolled back and applied nothing");

    /*
     * ITEM-506 increment 6: the plane holds one candidate, so a new stage
     * discards the one it replaces through that executor's own rollback.  The
     * replacement here is a system-hostname stage whose executor is unbound,
     * yet the staged credential candidate is still zeroed: a plaintext
     * candidate may not survive with no token that could ever reach it.
     */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 18U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "account executor: a fourth change stages");
    {
        static const uint8_t system_target[] = "/api/v1/system";
        static const char system_body[] = "hostname=node-506";

        output_length = 0U;
        CHECK(appliance_production_mutation_plane_request(
                  &mutation, header, sizeof(header),
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
                  system_target, sizeof(system_target) - 1U,
                  system_body, sizeof(system_body) - 1U,
                  0U, false, 19U, output, sizeof(output), &output_length) ==
                  APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
              "account executor: a new stage is refused without an executor");
    }
    CHECK(fixture.rollback_calls == 3U &&
              appliance_production_mutation_plane_pending_mutation(
                  &mutation) ==
                  APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE,
          "account executor: a new stage discards the candidate it replaces");

    /* The panel form target is the same mutation; there is no PUT form. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 20U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
          "account executor: the panel form target refuses a non-POST method");

    /* Clearing the binding refuses the mutation again. */
    CHECK(appliance_production_mutation_plane_bind_account(&mutation, NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "account executor: the executor binding clears");
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              account_target, sizeof(account_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 21U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "account executor: the unbound credential change is refused again");
}

/*
 * ITEM-507 increment 4: the name -> certificate/key mutation runs through the
 * bound executor when one exists.  The test proves that the executor receives
 * the stage, the commit and the rollback, that an unbound executor refuses the
 * mutation, that the wrong token confirms nothing and that a failed commit is
 * answered APPLY_FAILED with the candidate rolled back.
 */
struct certificate_name_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    size_t last_stage_length;
    uint8_t last_stage_body[256];
    bool commit_ok;
    enum appliance_production_mutation_plane_certificate_name_status
        stage_status;
};

static enum appliance_production_mutation_plane_certificate_name_status
fixture_certificate_name_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct certificate_name_executor_fixture *fixture = context;

    ++fixture->stage_calls;
    fixture->last_stage_length = body_length;
    memset(fixture->last_stage_body, 0, sizeof(fixture->last_stage_body));
    if (body != NULL && body_length != 0U &&
        body_length <= sizeof(fixture->last_stage_body)) {
        memcpy(fixture->last_stage_body, body, body_length);
    }
    if (fixture->stage_status !=
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK) {
        return fixture->stage_status;
    }
    return body != NULL && body_length != 0U
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED;
}

static enum appliance_production_mutation_plane_certificate_name_status
fixture_certificate_name_commit(void *context)
{
    struct certificate_name_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->commit_ok
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_APPLY_FAILED;
}

static void fixture_certificate_name_rollback(void *context)
{
    struct certificate_name_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

static void test_certificate_name_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_certificate_name_executor
        executor;
    struct certificate_name_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    uint32_t commits_before;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t name_target[] =
        "/api/v1/system/certificate/name";
    static const char stage_body[] = "action=delete&name=b.example";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    fixture.commit_ok = true;
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "certificate-name executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "certificate-name executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "certificate-name executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate-name executor: the plane binds with no transaction");
    make_credential(&credential, 0x51U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "certificate-name executor: administrator credential publishes");
    make_proof(&proof, 0x51U);
    make_token(token, 0xc1U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "certificate-name executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "certificate-name executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate-name executor: the plane refreshes with auth alone");

    /* No executor: the mutation is refused and nothing is staged. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 30U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "certificate-name executor: an unbound mutation is refused");
    CHECK(fixture.stage_calls == 0U,
          "certificate-name executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_certificate_name_stage;
    executor.commit = fixture_certificate_name_commit;
    executor.rollback = fixture_certificate_name_rollback;
    CHECK(appliance_production_mutation_plane_bind_certificate_name(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "certificate-name executor: the executor binds");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 31U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate-name executor: an authorized mutation stages");
    CHECK(fixture.stage_calls == 1U &&
              fixture.last_stage_length == sizeof(stage_body) - 1U,
          "certificate-name executor: the executor received the exact body");
    CHECK(appliance_production_mutation_plane_pending_mutation(&mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME,
          "certificate-name executor: the plane names the mutation");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "certificate-name executor: the staged mutation issues a token");

    /* The read method is not a mutation. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 32U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED,
          "certificate-name executor: the target refuses a non-POST method");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token + 1U, false, 33U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "certificate-name executor: the wrong token is rejected");
    CHECK(fixture.commit_calls == 0U,
          "certificate-name executor: the wrong token commits nothing");

    /* A commit the durable record refuses is APPLY_FAILED, not APPLIED. */
    fixture.commit_ok = false;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 34U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "certificate-name executor: a failed durable write is APPLY_FAILED");
    CHECK(fixture.commit_calls == 1U && fixture.rollback_calls == 1U,
          "certificate-name executor: the failed commit rolls the candidate "
          "back");
    CHECK(appliance_production_mutation_plane_applied_count(&mutation) == 0U,
          "certificate-name executor: a failed commit counts no change");

    /* The same stage with a writable record applies. */
    fixture.commit_ok = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 35U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate-name executor: the mutation stages again");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 36U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "certificate-name executor: the confirmed mutation applies");
    CHECK(fixture.commit_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "certificate-name executor: the executor committed and the plane "
          "counted it");

    /* The explicit cancel rolls the candidate back, unapplied. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 37U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "certificate-name executor: a third mutation stages");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, true, 38U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "certificate-name executor: the explicit cancel cancels");
    CHECK(fixture.rollback_calls == 2U &&
              appliance_production_mutation_plane_applied_count(&mutation) ==
                  1U,
          "certificate-name executor: the cancel applied nothing");
    /*
     * ITEM-649: an import the gate refused because the wall clock is not yet
     * validated names that reason instead of collapsing it into the generic
     * refusal the operator cannot act on.  The serving path reads the same
     * name back from the plane.
     */
    commits_before = fixture.commit_calls;
    fixture.stage_status =
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_BAD_CLOCK;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 39U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "certificate-name executor: an unverified clock is APPLY_FAILED");
    CHECK(output_length != 0U && strstr(output, "reason=BAD_CLOCK") != NULL,
          "certificate-name executor: the unverified clock is named in the "
          "refusal record");
    CHECK(appliance_production_mutation_plane_certificate_name_refusal(
              &mutation) != NULL &&
              strcmp(
                  appliance_production_mutation_plane_certificate_name_refusal(
                      &mutation),
                  "BAD_CLOCK") == 0,
          "certificate-name executor: the serving path can read the named "
          "refusal");
    CHECK(fixture.commit_calls == commits_before,
          "certificate-name executor: the unverified clock commits nothing");

    fixture.stage_status =
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              name_target, sizeof(name_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 40U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "certificate-name executor: a generic stage refusal is APPLY_FAILED");
    CHECK(strstr(output, "reason=") == NULL &&
              appliance_production_mutation_plane_certificate_name_refusal(
                  &mutation) == NULL,
          "certificate-name executor: a generic refusal keeps its old record");
    fixture.stage_status =
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK;
}

/*
 * ITEM-516: the management-gate executor.
 *
 * The panel may only ever close the gate, so the fixture refuses exactly the
 * body that tries to open it and holds the candidate otherwise.  The test
 * proves that the plane - not the panel - owns the token and the window, that
 * an unbound executor refuses the mutation instead of faking a write, that an
 * `open` attempt is refused before any token is issued, and that a failed
 * commit is answered as APPLY_FAILED while a confirmed close is APPLIED.
 */
struct panel_gate_executor_fixture {
    uint32_t stage_calls;
    uint32_t commit_calls;
    uint32_t rollback_calls;
    size_t last_stage_length;
    bool commit_ok;
};

static enum appliance_production_mutation_plane_panel_gate_status
fixture_panel_gate_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct panel_gate_executor_fixture *fixture = context;
    static const char open_body[] = "action=open";

    ++fixture->stage_calls;
    fixture->last_stage_length = body_length;
    if (body == NULL || body_length == 0U) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
    }
    if (body_length == sizeof(open_body) - 1U &&
        memcmp(body, open_body, sizeof(open_body) - 1U) == 0) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
    }
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK;
}

static enum appliance_production_mutation_plane_panel_gate_status
fixture_panel_gate_commit(void *context)
{
    struct panel_gate_executor_fixture *fixture = context;

    ++fixture->commit_calls;
    return fixture->commit_ok
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_APPLY_FAILED;
}

static void fixture_panel_gate_rollback(void *context)
{
    struct panel_gate_executor_fixture *fixture = context;

    ++fixture->rollback_calls;
}

static void test_panel_gate_executor(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_mutation_plane mutation;
    struct appliance_production_mutation_plane_panel_gate_executor executor;
    struct panel_gate_executor_fixture fixture;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t gate_target[] = "/api/v1/system/gate";
    static const uint8_t panel_target[] = "/panel/gate";
    static const char stage_body[] = "action=close";
    static const char open_body[] = "action=open";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&mutation, 0, sizeof(mutation));
    memset(&fixture, 0, sizeof(fixture));
    fixture.commit_ok = true;
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "gate executor: management composition initializes");
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "gate executor: production auth initializes");
    CHECK(appliance_production_management_plane_init(&plane, &management,
                                                     &auth) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_PLANE_OK,
          "gate executor: read-only plane binds");
    CHECK(appliance_production_mutation_plane_init(&mutation, &auth, NULL,
                                                   NULL) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "gate executor: the plane binds with no transaction");
    make_credential(&credential, 0x61U);
    CHECK(appliance_production_auth_publish_credential(&auth, &credential) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "gate executor: administrator credential publishes");
    make_proof(&proof, 0x61U);
    make_token(token, 0xb1U);
    CHECK(appliance_admin_auth_authenticate_local(
              &auth.admin, &proof, token, 10U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK && session_id == 1U,
          "gate executor: administrator session is issued");
    make_bearer_header(header, token);
    make_evidence(&evidence, 1U, false);
    CHECK(appliance_production_management_publish(&management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "gate executor: management evidence publishes");
    CHECK(appliance_production_mutation_plane_refresh(&mutation) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "gate executor: the plane refreshes with auth alone");

    /* No executor: the mutation is refused and nothing is staged. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 10U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET,
          "gate executor: an unbound gate mutation is refused");
    CHECK(fixture.stage_calls == 0U,
          "gate executor: the refusal stages nothing");

    memset(&executor, 0, sizeof(executor));
    executor.context = &fixture;
    executor.stage = fixture_panel_gate_stage;
    executor.commit = fixture_panel_gate_commit;
    executor.rollback = fixture_panel_gate_rollback;
    CHECK(appliance_production_mutation_plane_bind_panel_gate(
              &mutation, &executor) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_OK,
          "gate executor: the executor binds");

    /* An attempt to open is refused before any token exists. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              open_body, sizeof(open_body) - 1U,
              0U, false, 11U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "gate executor: opening from the panel is refused");
    CHECK(fixture.stage_calls == 1U && fixture.commit_calls == 0U &&
              !appliance_production_mutation_plane_has_pending(&mutation),
          "gate executor: the refused open issues no token and writes "
          "nothing");

    /* The authorized close stages, then commits with the exact token. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 12U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "gate executor: the authorized close stages");
    CHECK(fixture.last_stage_length == sizeof(stage_body) - 1U,
          "gate executor: the executor received the exact body");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    CHECK(confirmation_token != 0U,
          "gate executor: the staged close issues a token");
    CHECK(appliance_production_mutation_plane_pending_mutation(&mutation) ==
              APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE,
          "gate executor: the plane names the gate mutation");

    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token + 1U, false, 13U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION,
          "gate executor: the wrong token is rejected");
    CHECK(fixture.commit_calls == 0U,
          "gate executor: the wrong token commits nothing");

    /* A commit the durable record refuses is APPLY_FAILED, not APPLIED. */
    fixture.commit_ok = false;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              panel_target, sizeof(panel_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 14U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED,
          "gate executor: a failed durable write is APPLY_FAILED");
    CHECK(fixture.commit_calls == 1U && fixture.rollback_calls == 1U,
          "gate executor: the failed commit rolls the candidate back");

    /* The same stage with a good commit is APPLIED. */
    fixture.commit_ok = true;
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 15U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "gate executor: the close stages again");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, false, 16U, output, sizeof(output),
              &output_length) == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED,
          "gate executor: the confirmed close is applied");

    /* A pending candidate cancelled by the operator applies nothing. */
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              0U, false, 17U, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION,
          "gate executor: the cancel fixture stages");
    confirmation_token =
        appliance_production_mutation_plane_pending_token(&mutation);
    output_length = 0U;
    CHECK(appliance_production_mutation_plane_request(
              &mutation, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              gate_target, sizeof(gate_target) - 1U,
              stage_body, sizeof(stage_body) - 1U,
              confirmation_token, true, 18U, output, sizeof(output),
              &output_length) ==
              APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED,
          "gate executor: the cancel is answered as cancelled");
    CHECK(!appliance_production_mutation_plane_has_pending(&mutation),
          "gate executor: the cancel applied nothing");
}

int main(void)
{
    test_init_and_fail_closed();
    test_confirmation_cancel_and_apply();
    test_certificate_executor();
    test_routing_executor();
    test_network_executor();
    test_system_executor();
    test_account_executor();
    test_certificate_name_executor();
    test_panel_gate_executor();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production mutation plane host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production mutation plane host tests: ALL OK\n");
    return 0;
}

#endif
