#include "appliance_production_update_plane.h"

#include "key_use.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "trust_anchor.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_UPDATE_PLANE_HOST_TEST

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

static const unsigned char metadata_data[] = "update-metadata-v1";
static const unsigned char payload_data[] = "update-payload-v1";

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

static struct trust_anchor make_anchor(
    uint32_t id,
    enum trust_anchor_algorithm algorithm_id,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = algorithm_id;
    anchor.state = TRUST_ANCHOR_STATE_ACTIVE;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static void make_policy(struct verification_policy *policy)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot snapshot;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ED25519,
        KEY_USE_ACCEPTED_PURPOSES_ALL);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 7U;
    snapshot.anchor_count = 2U;
    memcpy(snapshot.anchors, anchors, sizeof(anchors));
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &snapshot) ==
              VERIFICATION_POLICY_OK,
          "update plane fixture publishes an active verification policy");
}

static void bytes_to_hex(
    const unsigned char digest[SHA256_DIGEST_SIZE],
    char output[2U * SHA256_DIGEST_SIZE + 1U])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        output[index * 2U] = digits[digest[index] >> 4U];
        output[index * 2U + 1U] = digits[digest[index] & 0x0FU];
    }
    output[2U * SHA256_DIGEST_SIZE] = '\0';
}

static void make_binding(
    const unsigned char metadata_digest[SHA256_DIGEST_SIZE],
    const unsigned char payload_digest[SHA256_DIGEST_SIZE],
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE])
{
    memcpy(binding, metadata_digest, SHA256_DIGEST_SIZE);
    memcpy(binding + SHA256_DIGEST_SIZE,
           payload_digest, SHA256_DIGEST_SIZE);
}

static void make_artifact(
    const struct verification_policy *policy,
    struct appliance_update_manifest *manifest,
    char *manifest_text,
    size_t manifest_text_capacity,
    unsigned char envelope[256],
    size_t *envelope_length)
{
    char metadata_hex[2U * SHA256_DIGEST_SIZE + 1U];
    char payload_hex[2U * SHA256_DIGEST_SIZE + 1U];
    unsigned char metadata_digest[SHA256_DIGEST_SIZE];
    unsigned char payload_digest[SHA256_DIGEST_SIZE];
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    int written;

    CHECK(sha256_compute(
              metadata_data, sizeof(metadata_data) - 1U,
              metadata_digest) == SHA256_OK,
          "update plane fixture hashes metadata");
    CHECK(sha256_compute(
              payload_data, sizeof(payload_data) - 1U,
              payload_digest) == SHA256_OK,
          "update plane fixture hashes payload");
    bytes_to_hex(metadata_digest, metadata_hex);
    bytes_to_hex(payload_digest, payload_hex);
    make_binding(metadata_digest, payload_digest, binding);

    written = snprintf(
        manifest_text, manifest_text_capacity,
        "update_package_version=1\n"
        "release_id=HFOC-APPLIANCE-V1\n"
        "architecture=x86_64\n"
        "source_revision=0123456789abcdef0123456789abcdef01234567\n"
        "metadata_digest=%s\n"
        "payload_digest=%s\n"
        "compatibility_version=1\n"
        "source_image_id=7\n"
        "target_image_id=8\n"
        "source_schema_min=1\n"
        "source_schema_max=1\n"
        "target_schema_min=1\n"
        "target_schema_max=2\n"
        "source_kernel_abi_min=1\n"
        "source_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\n"
        "target_kernel_abi_max=3\n"
        "source_service_abi_min=1\n"
        "source_service_abi_max=1\n"
        "target_service_abi_min=1\n"
        "target_service_abi_max=1\n"
        "source_policy_generation_min=1\n"
        "source_policy_generation_max=1\n"
        "target_policy_generation_min=2\n"
        "target_policy_generation_max=2\n",
        metadata_hex, payload_hex);
    CHECK(written > 0 && (size_t)written < manifest_text_capacity,
          "update plane fixture renders the manifest");
    CHECK(appliance_update_manifest_parse(
              manifest_text, (size_t)written, manifest) ==
              APPLIANCE_UPDATE_MANIFEST_OK,
          "update plane fixture parses the manifest");
    CHECK(signed_metadata_seal(
              policy,
              KEY_USE_PURPOSE_UPDATE,
              1U,
              binding,
              APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE,
              envelope,
              256U,
              envelope_length) == SIGNED_METADATA_OK,
          "update plane fixture seals the update envelope");
}

static void fill_artifact(
    struct appliance_production_update_artifact *artifact,
    const char *manifest_text,
    size_t manifest_length,
    const unsigned char *envelope,
    size_t envelope_length)
{
    memset(artifact, 0, sizeof(*artifact));
    artifact->manifest_text = manifest_text;
    artifact->manifest_length = manifest_length;
    artifact->metadata = metadata_data;
    artifact->metadata_length = sizeof(metadata_data) - 1U;
    artifact->payload = payload_data;
    artifact->payload_length = sizeof(payload_data) - 1U;
    artifact->envelope = envelope;
    artifact->envelope_length = envelope_length;
}

static bool accept_reboot(void *context)
{
    bool *called = context;

    *called = true;
    return true;
}

static void test_init_and_fail_closed(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_update_plane update;
    struct verification_policy policy;
    struct appliance_update_manifest manifest;
    struct appliance_production_update_artifact artifact;
    char manifest_text[4096];
    unsigned char envelope[256];
    size_t envelope_length = 0U;
    char output[APPLIANCE_PRODUCTION_UPDATE_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t update_target[] = "/api/v1/system/update";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&update, 0, sizeof(update));
    make_policy(&policy);
    make_artifact(&policy, &manifest, manifest_text, sizeof(manifest_text),
                  envelope, &envelope_length);

    CHECK(appliance_production_update_plane_init(NULL, &auth, &plane,
                                                 &policy) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL,
          "update plane init rejects null plane");
    CHECK(appliance_production_update_plane_init(&update, NULL, &plane,
                                                 &policy) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL,
          "update plane init rejects null auth");
    CHECK(appliance_production_update_plane_init(&update, &auth, NULL,
                                                 &policy) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL,
          "update plane init rejects null management plane");
    CHECK(appliance_production_update_plane_init(&update, &auth, &plane,
                                                 NULL) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL,
          "update plane init rejects null verification policy");
    CHECK(appliance_production_update_plane_init(&update, &auth, &plane,
                                                 &policy) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_OK,
          "update plane binds the production compositions");
    CHECK(appliance_production_update_plane_fail_closed(NULL) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_NULL,
          "update plane fail-closed rejects null plane");

    fill_artifact(&artifact, manifest_text, strlen(manifest_text),
                  envelope, envelope_length);
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, NULL, 0U,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_UNAVAILABLE,
          "unpublished credential fails the update closed");
    CHECK(appliance_production_update_plane_has_pending(&update) == false,
          "unavailable update never stages a candidate");
    CHECK(appliance_production_update_plane_reboot_required(&update) == false,
          "unavailable update never claims a reboot");
    CHECK(appliance_production_update_plane_ready(&update) == false,
          "update plane is not ready before auth and evidence");
    CHECK(appliance_production_update_plane_state(&update) !=
              APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY,
          "update plane never publishes READY without evidence");
}

static void test_signed_accept_reject_cancel_and_reboot(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_management management;
    struct appliance_production_management_plane plane;
    struct appliance_production_update_plane update;
    struct verification_policy policy;
    struct appliance_update_manifest manifest;
    struct appliance_production_update_artifact artifact;
    struct appliance_production_update_reboot_io reboot;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    struct appliance_production_management_evidence evidence;
    struct appliance_production_update_plane_audit_entry entry;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint64_t confirmation_token;
    bool reboot_called = false;
    char manifest_text[4096];
    unsigned char envelope[256];
    size_t envelope_length = 0U;
    char output[APPLIANCE_PRODUCTION_UPDATE_PLANE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    static const uint8_t update_target[] = "/api/v1/system/update";

    memset(&auth, 0, sizeof(auth));
    memset(&management, 0, sizeof(management));
    memset(&plane, 0, sizeof(plane));
    memset(&update, 0, sizeof(update));
    make_policy(&policy);
    make_artifact(&policy, &manifest, manifest_text, sizeof(manifest_text),
                  envelope, &envelope_length);
    fill_artifact(&artifact, manifest_text, strlen(manifest_text),
                  envelope, envelope_length);

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
    CHECK(appliance_production_update_plane_init(&update, &auth, &plane,
                                                 &policy) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_OK,
          "update plane binds auth, management evidence and policy");

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
    CHECK(appliance_production_update_plane_refresh(&update) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_OK,
          "ready auth and evidence refresh the update plane");
    CHECK(appliance_production_update_plane_state(&update) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_READY,
          "update plane publishes READY honestly");
    CHECK(appliance_production_update_plane_ready(&update) == true,
          "ready update plane admits authorized requests");

    envelope[8] ^= 0xFFU;
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 11U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_VERIFY_FAILED,
          "tampered signed envelope is rejected");
    CHECK(appliance_production_update_plane_has_pending(&update) == false,
          "rejected update stages nothing");
    CHECK(appliance_production_update_plane_rejected_count(&update) == 1U,
          "rejected update is counted");
    envelope[8] ^= 0xFFU;

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, NULL, 0U,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 12U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_DENIED,
          "anonymous update is denied");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 13U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION,
          "verified signed update stages a pending candidate");
    CHECK(appliance_production_update_plane_has_pending(&update) == true,
          "staged update is pending");
    confirmation_token =
        appliance_production_update_plane_pending_token(&update);
    CHECK(confirmation_token != 0U,
          "staged update issues a non-zero confirmation token");
    CHECK(appliance_production_update_plane_state(&update) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_PENDING_CONFIRMATION,
          "pending update publishes PENDING_CONFIRMATION");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, confirmation_token + 1U, false, 14U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_CONFIRMATION,
          "wrong confirmation token is rejected");
    CHECK(appliance_production_update_plane_has_pending(&update) == true,
          "rejected confirmation keeps the pending candidate");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, confirmation_token, true, 15U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_CANCELLED,
          "explicit cancel discards the pending candidate");
    CHECK(appliance_production_update_plane_has_pending(&update) == false,
          "cancelled update clears the pending candidate");
    CHECK(appliance_production_update_plane_cancelled_count(&update) == 1U,
          "cancelled update is counted");
    CHECK(appliance_production_update_plane_reboot_required(&update) == false,
          "cancelled update never requires a reboot");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 16U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION,
          "update stages before the confirmation window closes");
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact,
              appliance_production_update_plane_pending_token(&update),
              false,
              16U + APPLIANCE_PRODUCTION_UPDATE_PLANE_CONFIRMATION_TICKS + 1U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_EXPIRED,
          "confirmation after the window expires safely");
    CHECK(appliance_production_update_plane_has_pending(&update) == false,
          "expired confirmation discards the candidate");
    CHECK(appliance_production_update_plane_expired_count(&update) == 1U,
          "expired confirmation is counted");
    CHECK(appliance_production_update_plane_reboot_required(&update) == false,
          "expired confirmation never requires a reboot");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 20U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION,
          "update stages again for an unbound reboot executor");
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact,
              appliance_production_update_plane_pending_token(&update),
              false, 21U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_UNSUPPORTED_TARGET,
          "update without a reboot executor is refused, not faked");
    CHECK(appliance_production_update_plane_has_pending(&update) == false,
          "refused update leaves no pending candidate");
    CHECK(appliance_production_update_plane_accepted_count(&update) == 0U,
          "refused update is never counted as accepted");
    CHECK(appliance_production_update_plane_reboot_required(&update) == false,
          "refused update never claims a reboot");

    memset(&reboot, 0, sizeof(reboot));
    reboot.context = &reboot_called;
    reboot.request = accept_reboot;
    CHECK(appliance_production_update_plane_bind_reboot(&update, &reboot) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_OK,
          "reboot executor binds");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 30U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_PENDING_CONFIRMATION,
          "update stages with a bound reboot executor");
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact,
              appliance_production_update_plane_pending_token(&update),
              false, 31U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_ACCEPTED,
          "matching confirmation token accepts the signed update");
    CHECK(reboot_called == true,
          "accepted update requests the bound reboot executor");
    CHECK(appliance_production_update_plane_accepted_count(&update) == 1U,
          "accepted update is counted");
    CHECK(appliance_production_update_plane_reboot_required(&update) == true,
          "accepted update publishes reboot required");
    CHECK(appliance_production_update_plane_state(&update) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_STATE_REBOOT_REQUIRED,
          "accepted update publishes REBOOT_REQUIRED");
    CHECK(appliance_production_update_plane_audit_count(&update) > 0U,
          "accepted update is audited");
    CHECK(appliance_production_update_plane_audit_entry(
              &update,
              appliance_production_update_plane_audit_count(&update) - 1U,
              &entry) == true &&
              entry.outcome ==
                  APPLIANCE_PRODUCTION_UPDATE_PLANE_AUDIT_ALLOWED &&
              entry.session_id == session_id,
          "audit entry records the authorized update outcome");

    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 40U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE,
          "update staged after acceptance is refused");

    CHECK(appliance_production_update_plane_stop(&update) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_OK,
          "update plane stops");
    output_length = 0U;
    CHECK(appliance_production_update_plane_request(
              &update, header, sizeof(header),
              APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
              update_target, sizeof(update_target) - 1U,
              &artifact, 0U, false, 200U,
              output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_UPDATE_PLANE_BAD_STATE,
          "stopped update plane rejects further updates");
}

int main(void)
{
    test_init_and_fail_closed();
    test_signed_accept_reject_cancel_and_reboot();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production update plane host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production update plane host tests: ALL OK\n");
    return 0;
}

#endif
