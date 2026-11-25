#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST

#include "appliance_admin_auth.h"
#include "appliance_management_authorization.h"
#include "http_parser.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void management_plane_qemu_fill(
    void *destination,
    uint8_t value,
    size_t length)
{
    uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static void management_plane_qemu_zero(
    void *destination,
    size_t length)
{
    uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void management_plane_qemu_fail(const char *message)
    __attribute__((noreturn));

static void management_plane_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[MGMT-PLANE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void management_plane_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        management_plane_qemu_fail(message);
    }
}

static void management_plane_qemu_mark(const char *marker)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(marker);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static struct appliance_admin_auth_credential
management_plane_qemu_make_credential(uint8_t value)
{
    struct appliance_admin_auth_credential credential;

    management_plane_qemu_zero(
        &credential, sizeof(credential));
    credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    management_plane_qemu_fill(
        credential.verifier, value,
        sizeof(credential.verifier));
    return credential;
}

static struct appliance_admin_auth_proof
management_plane_qemu_make_proof(uint8_t value)
{
    struct appliance_admin_auth_proof proof;

    management_plane_qemu_zero(&proof, sizeof(proof));
    proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    management_plane_qemu_fill(
        proof.proof, value, sizeof(proof.proof));
    return proof;
}

static void management_plane_qemu_make_token(
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t value)
{
    management_plane_qemu_fill(
        token, value, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static void management_plane_qemu_make_bearer_header(
    uint8_t *header,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    memcpy(header,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static void management_plane_qemu_auth_fixture(void)
{
    struct appliance_admin_auth auth;
    struct appliance_admin_auth credential_auth;
    struct appliance_admin_auth_credential credential =
        management_plane_qemu_make_credential(0x21U);
    struct appliance_admin_auth_credential replacement =
        management_plane_qemu_make_credential(0x22U);
    struct appliance_admin_auth_credential throttle_credential =
        management_plane_qemu_make_credential(0x23U);
    struct appliance_admin_auth_proof proof =
        management_plane_qemu_make_proof(0x21U);
    struct appliance_admin_auth_proof wrong_proof =
        management_plane_qemu_make_proof(0x31U);
    struct appliance_admin_auth_proof throttle_proof =
        management_plane_qemu_make_proof(0x23U);
    struct appliance_admin_auth_proof throttle_bad_proof =
        management_plane_qemu_make_proof(0x33U);
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t replacement_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t wrong_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t throttle_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t wrong_header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                         APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t malformed[sizeof(header)];
    struct appliance_admin_auth_decision decision;
    uint32_t session_id = 0U;
    uint32_t attempt;

    management_plane_qemu_make_token(token, 0x51U);
    management_plane_qemu_make_token(replacement_token, 0x52U);
    management_plane_qemu_make_token(wrong_token, 0x53U);
    management_plane_qemu_make_token(throttle_token, 0x54U);
    management_plane_qemu_make_bearer_header(header, token);
    management_plane_qemu_make_bearer_header(
        wrong_header, wrong_token);
    memcpy(malformed, header, sizeof(malformed));
    malformed[0U] = (uint8_t)'x';

    management_plane_qemu_zero(&auth, sizeof(auth));
    management_plane_qemu_require(
        appliance_admin_auth_init(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        auth.state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT &&
        !auth.credential_published &&
        auth.session_count == 0U,
        "admin auth absent init");
    management_plane_qemu_require(
        appliance_admin_auth_authenticate_request(
            &auth,
            header,
            sizeof(header),
            0U,
            &decision) == APPLIANCE_ADMIN_AUTH_BAD_STATE &&
        !decision.authenticated,
        "absent admin auth request fails closed");
    management_plane_qemu_mark("[MGMT-PLANE] auth absent ok");

    management_plane_qemu_require(
        appliance_admin_auth_credential_publish(
            &auth, &credential) == APPLIANCE_ADMIN_AUTH_OK &&
        auth.state == APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED &&
        appliance_admin_auth_enable(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        auth.state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE &&
        auth.credential_generation == 1U,
        "admin credential activation");
    management_plane_qemu_mark("[MGMT-PLANE] credential active ok");

    management_plane_qemu_require(
        appliance_admin_auth_authenticate_local(
            &auth, &wrong_proof, token, 10U, 100U, &session_id) ==
            APPLIANCE_ADMIN_AUTH_AUTH_FAILED &&
        auth.auth_failures == 1U,
        "wrong admin proof denial");
    management_plane_qemu_mark("[MGMT-PLANE] wrong proof denied ok");

    session_id = 0U;
    management_plane_qemu_require(
        appliance_admin_auth_authenticate_local(
            &auth, &proof, token, 10U, 100U, &session_id) ==
            APPLIANCE_ADMIN_AUTH_OK &&
        session_id == 1U &&
        auth.session_count == 1U &&
        auth.sessions[0].id == 1U &&
        auth.sessions[0].credential_generation == 1U &&
        auth.sessions[0].opened_tick == 10U &&
        auth.sessions[0].expires_tick == 110U,
        "admin valid session publication");
    management_plane_qemu_mark("[MGMT-PLANE] valid session ok");

    management_plane_qemu_zero(&decision, sizeof(decision));
    management_plane_qemu_require(
        appliance_admin_auth_authenticate_request(
            &auth,
            header,
            sizeof(header),
            50U,
            &decision) == APPLIANCE_ADMIN_AUTH_OK &&
        decision.authenticated &&
        decision.principal_id == APPLIANCE_ADMIN_PRINCIPAL_ID &&
        decision.session_id == 1U &&
        decision.credential_generation == 1U,
        "exact bearer request authentication");
    management_plane_qemu_mark("[MGMT-PLANE] valid bearer ok");

    management_plane_qemu_require(
        appliance_admin_auth_authenticate_request(
            &auth,
            NULL,
            0U,
            50U,
            &decision) == APPLIANCE_ADMIN_AUTH_HEADER_MISSING &&
        !decision.authenticated,
        "missing bearer header denial");
    management_plane_qemu_mark("[MGMT-PLANE] missing bearer denied ok");

    management_plane_qemu_require(
        appliance_admin_auth_authenticate_request(
            &auth,
            malformed,
            sizeof(malformed),
            50U,
            &decision) == APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED &&
        !decision.authenticated,
        "malformed bearer header denial");
    management_plane_qemu_mark("[MGMT-PLANE] malformed bearer denied ok");

    management_plane_qemu_require(
        appliance_admin_auth_authenticate_request(
            &auth,
            wrong_header,
            sizeof(wrong_header),
            50U,
            &decision) == APPLIANCE_ADMIN_AUTH_TOKEN_INVALID &&
        !decision.authenticated,
        "unknown bearer token denial");
    management_plane_qemu_mark("[MGMT-PLANE] unknown bearer denied ok");

    management_plane_qemu_require(
        appliance_admin_auth_disable(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        auth.state == APPLIANCE_ADMIN_AUTH_STATE_DISABLED &&
        auth.session_count == 0U &&
        appliance_admin_auth_authenticate_request(
            &auth,
            header,
            sizeof(header),
            60U,
            &decision) == APPLIANCE_ADMIN_AUTH_BAD_STATE &&
        !decision.authenticated,
        "disable revocation");
    management_plane_qemu_mark("[MGMT-PLANE] disable revocation ok");

    management_plane_qemu_require(
        appliance_admin_auth_reset(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        auth.state == APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING &&
        appliance_admin_auth_credential_publish(
            &auth, &replacement) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_enable(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_authenticate_request(
            &auth,
            header,
            sizeof(header),
            70U,
            &decision) == APPLIANCE_ADMIN_AUTH_TOKEN_INVALID &&
        !decision.authenticated,
        "reset recovery rejects old bearer");
    management_plane_qemu_mark("[MGMT-PLANE] reset recovery ok");

    management_plane_qemu_zero(&credential_auth, sizeof(credential_auth));
    management_plane_qemu_require(
        appliance_admin_auth_init(&credential_auth) ==
            APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_credential_publish(
            &credential_auth,
            &throttle_credential) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_enable(&credential_auth) ==
            APPLIANCE_ADMIN_AUTH_OK,
        "throttle fixture activation");
    for (attempt = 0U;
         attempt < (APPLIANCE_ADMIN_AUTH_FAILURE_LIMIT - 1U);
         ++attempt) {
        management_plane_qemu_require(
            appliance_admin_auth_authenticate_local(
                &credential_auth,
                &throttle_bad_proof,
                throttle_token,
                0U,
                100U,
                &session_id) == APPLIANCE_ADMIN_AUTH_AUTH_FAILED,
            "throttle bounded failure");
    }
    management_plane_qemu_require(
        appliance_admin_auth_authenticate_local(
            &credential_auth,
            &throttle_bad_proof,
            throttle_token,
            0U,
            100U,
            &session_id) == APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED &&
        credential_auth.locked_until_tick == 100U &&
        appliance_admin_auth_authenticate_local(
            &credential_auth,
            &throttle_proof,
            throttle_token,
            50U,
            100U,
            &session_id) == APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED,
        "local proof throttle lock");
    management_plane_qemu_mark("[MGMT-PLANE] throttle denial ok");
}

static void management_plane_qemu_check_route(
    const struct appliance_management_authorization_policy *policy,
    const struct appliance_admin_auth_decision *decision,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_authorization_status expected_status,
    enum appliance_management_authorization_route expected_route,
    const char *message)
{
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    management_plane_qemu_require(
        appliance_management_authorization_check_request(
            policy,
            decision,
            method,
            target,
            target_length,
            &route) == expected_status &&
        route == expected_route,
        message);
}

static void management_plane_qemu_authorization_fixture(void)
{
    struct appliance_admin_auth auth;
    struct appliance_admin_auth_credential credential =
        management_plane_qemu_make_credential(0x41U);
    struct appliance_admin_auth_proof proof =
        management_plane_qemu_make_proof(0x41U);
    struct appliance_management_authorization_policy policy;
    struct appliance_admin_auth_decision unauth;
    struct appliance_admin_auth_decision auth_decision;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;

    management_plane_qemu_make_token(token, 0x61U);
    management_plane_qemu_make_bearer_header(header, token);
    management_plane_qemu_zero(&auth, sizeof(auth));
    management_plane_qemu_require(
        appliance_admin_auth_init(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_credential_publish(
            &auth, &credential) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_enable(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
        appliance_admin_auth_authenticate_local(
            &auth, &proof, token, 0U, 1000U, &session_id) ==
            APPLIANCE_ADMIN_AUTH_OK &&
        session_id == 1U &&
        appliance_admin_auth_authenticate_request(
            &auth,
            header,
            sizeof(header),
            10U,
            &auth_decision) == APPLIANCE_ADMIN_AUTH_OK &&
        auth_decision.authenticated,
        "authorization fixture session");

    management_plane_qemu_zero(&policy, sizeof(policy));
    management_plane_qemu_require(
        appliance_management_authorization_policy_init(&policy) ==
            APPLIANCE_MANAGEMENT_AUTHORIZATION_OK &&
        policy.version == APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION &&
        policy.initialized &&
        policy.matrix.initialized &&
        /*
         * ITEM-503 grew the matrix from ten entries to eleven with the
         * certificate import command, ITEM-504 grew it to twelve with the
         * routing configuration command, and ITEM-505 increment 3 grew it to
         * thirteen with the system-hostname command; ITEM-506 increment 6 grew
         * it to fourteen with the credential-change command, ITEM-507
         * increment 4 grew it to fifteen with the certificate-name command, and
         * ITEM-508 increment 34 grew it to sixteen with the ACME control
         * command.  The fixture pins the exact set, so the number moves with
         * the item.
         */
        policy.matrix.count == 17U,
        "management authorization policy");
    management_plane_qemu_mark("[MGMT-PLANE] authorization policy ok");

    management_plane_qemu_zero(&unauth, sizeof(unauth));
    management_plane_qemu_check_route(
        &policy,
        &unauth,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY,
        "public discovery route");
    management_plane_qemu_mark("[MGMT-PLANE] discovery public ok");

    /*
     * ITEM-384: the rendered panel is a management target too. It resolves to
     * its own read-only route, it is denied without a session, and an
     * unpublished panel target stays unknown instead of being admitted.
     */
    management_plane_qemu_check_route(
        &policy,
        &unauth,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH_LENGTH,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
        "unauthenticated panel route");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)"/panel/system",
        13U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
        "authenticated panel route");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)"/panel/attack",
        13U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
        "unknown panel route");

    management_plane_qemu_check_route(
        &policy,
        &unauth,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        "unauthenticated protected route denial");
    management_plane_qemu_mark("[MGMT-PLANE] protected deny closed ok");

    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        "authorized system route");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG,
        "authorized network config route");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_HEAD,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS,
        "authorized network status route");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_OK,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS,
        "authorized service route");
    management_plane_qemu_mark("[MGMT-PLANE] protected read authorized ok");

    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_UNSUPPORTED,
        (const uint8_t *)
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH,
        sizeof(APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH) - 1U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        "management method denial");
    management_plane_qemu_mark("[MGMT-PLANE] method denied ok");

    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)"/api/v1/unknown",
        15U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
        "unknown management route denial");
    management_plane_qemu_check_route(
        &policy,
        &auth_decision,
        HTTP_METHOD_GET,
        (const uint8_t *)"/api2/v1",
        8U,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE,
        "non-management prefix denial");
    management_plane_qemu_mark("[MGMT-PLANE] unknown denied ok");
    management_plane_qemu_mark("[MGMT-PLANE] production route boundary ok");
}

void appliance_management_plane_qemu_test_init(void)
{
    management_plane_qemu_mark("[MGMT-PLANE] proof init");
    management_plane_qemu_auth_fixture();
    management_plane_qemu_authorization_fixture();
    management_plane_qemu_mark("[MGMT-PLANE] proof OK");
}

#else
typedef int appliance_management_plane_qemu_translation_unit_guard;
#endif
