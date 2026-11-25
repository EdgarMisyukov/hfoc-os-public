#include "appliance_license_inventory.h"
#include "appliance_management_panel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(APPLIANCE_LICENSE_INVENTORY_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/*
 * The declared inventory is read from `release/hfoc-license-inventory.txt` and
 * compared field by field with the compiled-in mirror. A component, version,
 * license identifier, election or notice path that differs is a drift between
 * what the panel publishes and what the payload contract enforces, so it fails
 * here instead of reaching the ISO.
 */
static bool read_file(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    size_t length;

    if (file == NULL) {
        return false;
    }
    length = fread(buffer, 1U, capacity - 1U, file);
    fclose(file);
    buffer[length] = '\0';
    return true;
}

static bool declared_value(
    const char *document,
    const char *key,
    char *value,
    size_t capacity)
{
    size_t key_length = strlen(key);
    const char *cursor = document;

    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length =
            line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_length > key_length + 1U &&
            cursor[key_length] == '=' &&
            strncmp(cursor, key, key_length) == 0) {
            size_t value_length = line_length - key_length - 1U;

            if (value_length + 1U > capacity) {
                return false;
            }
            memcpy(value, cursor + key_length + 1U, value_length);
            value[value_length] = '\0';
            return true;
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }
    return false;
}

static void expect_declared(
    const char *document,
    const char *key,
    const char *expected,
    const char *message)
{
    char value[128];

    value[0] = '\0';
    CHECK(declared_value(document, key, value, sizeof(value)) &&
              strcmp(value, expected) == 0,
          message);
}

static int parse_decimal(const char *text)
{
    int result = 0;
    size_t index = 0U;

    while (text[index] >= '0' && text[index] <= '9') {
        result = result * 10 + (int)(text[index] - '0');
        ++index;
    }
    return result;
}

static void test_declared_inventory_matches(const char *inventory_path)
{
    static char document[16384];
    char key[64];
    char value[128];
    uint32_t count;
    uint32_t index;

    CHECK(inventory_path != NULL,
          "license inventory proof needs the declared inventory path");
    if (inventory_path == NULL) {
        return;
    }
    CHECK(read_file(inventory_path, document, sizeof(document)),
          "declared license inventory is readable");
    if (failures != 0) {
        return;
    }

    expect_declared(document, "appliance_component",
                    appliance_license_inventory_component(),
                    "declared appliance component matches the kernel mirror");
    expect_declared(document, "appliance_license",
                    appliance_license_inventory_license(),
                    "declared appliance license matches the kernel mirror");
    expect_declared(document, "appliance_license_notice",
                    appliance_license_inventory_license_notice(),
                    "declared appliance license notice matches the mirror");
    expect_declared(document, "appliance_licensing_terms",
                    appliance_license_inventory_licensing_terms(),
                    "declared licensing terms match the mirror");
    expect_declared(document, "appliance_commercial_terms",
                    appliance_license_inventory_commercial_terms(),
                    "declared commercial terms match the mirror");
    expect_declared(document, "payload_manifest",
                    appliance_license_inventory_manifest(),
                    "declared payload manifest matches the mirror");
    expect_declared(document, "payload_root",
                    appliance_license_inventory_payload_root(),
                    "declared payload root matches the mirror");

    value[0] = '\0';
    CHECK(declared_value(document, "dependency_count", value, sizeof(value)) &&
              parse_decimal(value) ==
                  (int)APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT,
          "declared dependency count matches the kernel mirror");
    count = appliance_license_inventory_dependency_count();
    CHECK(count == APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT,
          "kernel dependency count is exact");

    for (index = 0U; index < count; ++index) {
        const struct appliance_license_inventory_dependency *dependency =
            appliance_license_inventory_dependency(index);
        const char *required[5] = {
            "id", "component", "version", "license", "notice",
        };
        const char *values[5];
        size_t field;

        CHECK(dependency != NULL,
              "kernel inventory publishes every declared dependency");
        if (dependency == NULL) {
            return;
        }
        values[0] = dependency->id;
        values[1] = dependency->component;
        values[2] = dependency->version;
        values[3] = dependency->license;
        values[4] = dependency->notice;

        for (field = 0U; field < 5U; ++field) {
            snprintf(key,
                     sizeof(key),
                     "dependency_%u_%s",
                     (unsigned)(index + 1U),
                     required[field]);
            expect_declared(document, key, values[field],
                            "declared dependency field matches the mirror");
        }
        /* The election fields are optional: the declared file and the mirror
         * must agree on presence as well as on value. */
        {
            const char *optional_keys[3] = {
                "license_offered", "election", "election_notice",
            };
            const char *optional_values[3] = {
                dependency->license_offered,
                dependency->election,
                dependency->election_notice,
            };
            size_t optional;

            for (optional = 0U; optional < 3U; ++optional) {
                snprintf(key,
                         sizeof(key),
                         "dependency_%u_%s",
                         (unsigned)(index + 1U),
                         optional_keys[optional]);
                value[0] = '\0';
                if (declared_value(document, key, value, sizeof(value))) {
                    CHECK(optional_values[optional] != NULL &&
                              strcmp(value, optional_values[optional]) == 0,
                          "declared optional dependency field matches the "
                          "mirror");
                } else {
                    CHECK(optional_values[optional] == NULL,
                          "mirror invents no optional dependency field");
                }
            }
        }
    }
}

static void test_mbedtls_election(void)
{
    const struct appliance_license_inventory_dependency *dependency =
        appliance_license_inventory_find("mbedtls");

    CHECK(appliance_license_inventory_mbedtls_election() != NULL &&
              strcmp(appliance_license_inventory_mbedtls_election(),
                     "Apache-2.0") == 0,
          "mbedTLS election is stated as Apache-2.0");
    CHECK(dependency != NULL &&
              strcmp(dependency->license, "Apache-2.0") == 0,
          "mbedTLS ships under the elected Apache-2.0");
    CHECK(dependency != NULL && dependency->license_offered != NULL &&
              strcmp(dependency->license_offered,
                     "Apache-2.0 OR GPL-2.0-or-later") == 0,
          "mbedTLS records the offered disjunction separately");
    CHECK(dependency != NULL && dependency->election_notice != NULL &&
              strcmp(dependency->election_notice,
                     "notices/mbedtls-Apache-2.0-election.txt") == 0,
          "mbedTLS election notice is shipped in the payload");
    CHECK(appliance_license_inventory_find("ghost") == NULL &&
              appliance_license_inventory_dependency(
                  APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT) == NULL,
          "kernel inventory publishes no undeclared dependency");
    CHECK(appliance_license_inventory_notice(dependency, 0U) != NULL &&
              appliance_license_inventory_notice(dependency, 1U) != NULL &&
              appliance_license_inventory_notice(dependency, 2U) == NULL &&
              appliance_license_inventory_notice(NULL, 0U) == NULL,
          "notice slots are bounded");
}

struct license_panel_fixture {
    struct appliance_production_auth auth;
    struct appliance_management_panel panel;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t bearer[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t bearer_length;
};

static void fill_bytes(void *destination, uint8_t value, size_t length)
{
    uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static bool license_panel_fixture_init(struct license_panel_fixture *fixture)
{
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_proof proof;
    uint32_t session_id = 0U;

    memset(fixture, 0, sizeof(*fixture));
    memset(&credential, 0, sizeof(credential));
    memset(&proof, 0, sizeof(proof));
    credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    proof.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    fill_bytes(credential.verifier, 0x42U, sizeof(credential.verifier));
    fill_bytes(proof.proof, 0x42U, sizeof(proof.proof));
    fill_bytes(fixture->token, 0x66U, sizeof(fixture->token));
    memcpy(fixture->bearer,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(fixture->bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           fixture->token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    fixture->bearer_length = APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                             APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH;

    if (appliance_production_auth_init(&fixture->auth) !=
            APPLIANCE_PRODUCTION_AUTH_OK ||
        appliance_production_auth_publish_credential(
            &fixture->auth, &credential) != APPLIANCE_PRODUCTION_AUTH_OK ||
        appliance_management_panel_init(
            &fixture->panel, &fixture->auth) !=
            APPLIANCE_MANAGEMENT_PANEL_OK) {
        return false;
    }
    return appliance_management_panel_login(
               &fixture->panel,
               &proof,
               fixture->token,
               0U,
               APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
               &session_id) == APPLIANCE_MANAGEMENT_PANEL_OK;
}

static bool contains(const uint8_t *body, uint32_t length, const char *text)
{
    size_t text_length = strlen(text);
    uint32_t index;

    if (length < (uint32_t)text_length) {
        return false;
    }
    for (index = 0U; index + (uint32_t)text_length <= length; ++index) {
        if (memcmp(body + index, text, text_length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_panel_licenses_page(void)
{
    struct license_panel_fixture fixture;
    struct appliance_management_panel_response response;
    struct appliance_management_web_ui_licenses licenses;
    static const uint8_t licenses_target[] = "/panel/about/licenses";

    CHECK(license_panel_fixture_init(&fixture),
          "license panel fixture signs in through the administrator boundary");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              NULL,
              0U,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED &&
              response.status == HTTP_STATUS_401_UNAUTHORIZED,
          "licenses page requires an administrator session");

    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body,
                       response.body_length,
                       "<h2>About and licenses</h2>") &&
              contains(response.body, response.body_length, "UNAVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "LICENSE_INVENTORY_UNPUBLISHED"),
          "licenses page publishes UNAVAILABLE with a reason before an owner "
          "publishes the payload inventory");

    memset(&licenses, 0, sizeof(licenses));
    CHECK(appliance_management_panel_set_licenses(&fixture.panel, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "licenses page accepts a published payload view");
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "UNAVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "LICENSE_INVENTORY_UNPUBLISHED") &&
              !contains(response.body, response.body_length, "<table>"),
          "licenses page publishes an empty owner view as UNAVAILABLE instead "
          "of an invented list");

    memset(&licenses, 0, sizeof(licenses));
    licenses.declared = true;
    licenses.payload_root = appliance_license_inventory_payload_root();
    licenses.declared_count = appliance_license_inventory_dependency_count();
    licenses.shipped_count = 1U;
    CHECK(appliance_management_panel_set_licenses(&fixture.panel, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "DEGRADED") &&
              contains(response.body,
                       response.body_length,
                       "LICENSE_PAYLOAD_INCOMPLETE") &&
              contains(response.body, response.body_length, "<table>"),
          "licenses page publishes an incomplete payload as DEGRADED with a "
          "reason and the declared inventory");

    memset(&licenses, 0, sizeof(licenses));
    licenses.declared = true;
    licenses.payload_complete = true;
    licenses.manifest = appliance_license_inventory_manifest();
    licenses.payload_root = appliance_license_inventory_payload_root();
    licenses.declared_count = appliance_license_inventory_dependency_count();
    licenses.shipped_count = appliance_license_inventory_dependency_count();
    CHECK(appliance_management_panel_set_licenses(&fixture.panel, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_OK,
          "licenses page accepts the declared payload inventory");
    CHECK(appliance_management_panel_dispatch(
              &fixture.panel,
              HTTP_METHOD_GET,
              licenses_target,
              sizeof(licenses_target) - 1U,
              fixture.bearer,
              fixture.bearer_length,
              1U,
              NULL,
              NULL,
              &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              response.status == HTTP_STATUS_200_OK &&
              contains(response.body, response.body_length, "AVAILABLE") &&
              contains(response.body,
                       response.body_length,
                       "AGPL-3.0-only OR LicenseRef-HFOC-Commercial") &&
              contains(response.body, response.body_length, "mbedTLS") &&
              contains(response.body, response.body_length, "Apache-2.0") &&
              contains(response.body, response.body_length, "BSD-3-Clause") &&
              contains(response.body, response.body_length, "BSD-2-Clause") &&
              contains(response.body,
                       response.body_length,
                       "notices/mbedtls-Apache-2.0-election.txt"),
          "licenses page lists the shipped inventory with the mbedTLS election");

    /* A hostile owner value must be escaped, never injected. */
    memset(&licenses, 0, sizeof(licenses));
    licenses.declared = true;
    licenses.payload_complete = false;
    licenses.reason = "<script>alert(1)</script>";
    licenses.manifest = "manifest\" onload=\"x";
    licenses.payload_root = "licenses";
    licenses.declared_count = 3U;
    licenses.shipped_count = 1U;
    CHECK(appliance_management_panel_set_licenses(&fixture.panel, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              !contains(response.body, response.body_length, "<script>") &&
              !contains(response.body,
                        response.body_length,
                        "onload=\"x") &&
              contains(response.body,
                       response.body_length,
                       "&lt;script&gt;") &&
              contains(response.body, response.body_length, "DEGRADED"),
          "licenses page escapes every owner-supplied value");

    CHECK(appliance_management_panel_clear_licenses(&fixture.panel) ==
              APPLIANCE_MANAGEMENT_PANEL_OK &&
              appliance_management_panel_dispatch(
                  &fixture.panel,
                  HTTP_METHOD_GET,
                  licenses_target,
                  sizeof(licenses_target) - 1U,
                  fixture.bearer,
                  fixture.bearer_length,
                  1U,
                  NULL,
              NULL,
                  &response) == APPLIANCE_MANAGEMENT_PANEL_ALLOWED &&
              contains(response.body, response.body_length, "UNAVAILABLE"),
          "licenses page returns to UNAVAILABLE when the owner is cleared");

    CHECK(appliance_management_panel_set_licenses(NULL, &licenses) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL &&
              appliance_management_panel_clear_licenses(NULL) ==
              APPLIANCE_MANAGEMENT_PANEL_NULL,
          "licenses view handling rejects a null panel");
}

/*
 * The page the panel renders and the page the authorization matrix resolves
 * must be the same exact target: if they drift, the panel would hand the
 * authorizer a route it cannot resolve and an administrator would get a 403 for
 * a page that exists.
 */
static void test_licenses_target_alignment(void)
{
    CHECK(strcmp(APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH,
                 APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LICENSES_PATH) == 0,
          "panel and authorization publish the same about/licenses target");
    CHECK(strcmp(APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH,
                 "/panel/about/licenses") == 0 &&
              APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH_LENGTH ==
                  strlen(APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH),
          "about/licenses target is the declared exact path");
}

int main(int argc, char **argv)
{
    const char *inventory_path = NULL;

    if (argc > 1) {
        inventory_path = argv[1];
    }
    test_declared_inventory_matches(inventory_path);
    test_mbedtls_election();
    test_licenses_target_alignment();
    test_panel_licenses_page();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance license inventory host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Appliance license inventory host test: ALL OK\n");
    return 0;
}

#else

typedef int appliance_license_inventory_host_unused;

#endif
