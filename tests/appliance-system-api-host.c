#include "appliance_system_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SYSTEM_API_HOST_TEST

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

static const uint8_t method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"system\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";
static const uint8_t unavailable_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"system\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"machine_class\":\"hfoc-appliance\",\"product\":"
    "\"HFOC-APPLIANCE-V1\",\"architecture\":\"x86_64\",\"platform\":"
    "\"q35\",\"hostname\":\"hfoc-appliance\",\"instance\":\"default\","
    "\"uptime_ticks\":42,\"health\":\"ok\","
    "\"config_state\":\"unavailable\",\"config_source\":\"none\","
    "\"recovery_mode\":0,\"active_process_count\":0}";
static const uint8_t committed_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"system\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"machine_class\":\"hfoc-appliance\",\"product\":"
    "\"HFOC-APPLIANCE-V1\",\"architecture\":\"x86_64\",\"platform\":"
    "\"q35\",\"hostname\":\"hfoc-appliance\",\"instance\":\"default\","
    "\"uptime_ticks\":316,\"health\":\"degraded\","
    "\"config_state\":\"ok\",\"config_source\":\"committed\","
    "\"config_hostname\":\"node-316\",\"recovery_mode\":0,"
    "\"active_process_count\":2}";

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

static struct appliance_identity make_identity(void)
{
    struct appliance_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = APPLIANCE_IDENTITY_VERSION;
    memcpy(identity.machine_class, APPLIANCE_IDENTITY_MACHINE_CLASS,
           sizeof(APPLIANCE_IDENTITY_MACHINE_CLASS));
    memcpy(identity.product, APPLIANCE_IDENTITY_PRODUCT,
           sizeof(APPLIANCE_IDENTITY_PRODUCT));
    memcpy(identity.architecture, APPLIANCE_IDENTITY_ARCHITECTURE,
           sizeof(APPLIANCE_IDENTITY_ARCHITECTURE));
    memcpy(identity.platform, APPLIANCE_IDENTITY_PLATFORM,
           sizeof(APPLIANCE_IDENTITY_PLATFORM));
    memcpy(identity.hostname, APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_HOSTNAME));
    memcpy(identity.instance, APPLIANCE_IDENTITY_DEFAULT_INSTANCE,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_INSTANCE));
    identity.published = true;
    return identity;
}

static struct appliance_system_api_snapshot make_snapshot(
    enum appliance_system_api_state config_state)
{
    struct appliance_system_api_snapshot snapshot;
    struct appliance_identity identity = make_identity();

    memset(&snapshot, 0, sizeof(snapshot));
    if (config_state == APPLIANCE_SYSTEM_STATE_UNAVAILABLE) {
        CHECK(appliance_system_api_snapshot_init(
                  &snapshot, &identity,
                  APPLIANCE_SYSTEM_STATE_OK, 42U,
                  APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
                  NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_OK,
              "system snapshot init publishes unavailable config");
    } else {
        CHECK(appliance_system_api_snapshot_init(
                  &snapshot, &identity,
                  APPLIANCE_SYSTEM_STATE_DEGRADED, 316U,
                  APPLIANCE_SYSTEM_STATE_OK,
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
                  "node-316", 8U, 0U, 2U) == APPLIANCE_SYSTEM_API_OK,
              "system snapshot init publishes committed config");
    }
    return snapshot;
}

static bool body_is(
    const struct appliance_system_api_result *result,
    const uint8_t *expected,
    size_t expected_length)
{
    return result->published &&
           result->response.body != NULL &&
           result->response.body_length == expected_length &&
           memcmp(result->response.body, expected, expected_length) == 0;
}

static void test_strings(void)
{
    enum appliance_system_api_status status;
    enum appliance_system_api_state state;

    for (status = APPLIANCE_SYSTEM_API_OK;
         status <= APPLIANCE_SYSTEM_API_OVERFLOW; ++status) {
        const char *string = appliance_system_api_status_string(status);

        CHECK(string != NULL &&
                  (status == APPLIANCE_SYSTEM_API_OK ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_OK") == 0 :
                   status == APPLIANCE_SYSTEM_API_NULL ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_NULL") == 0 :
                   status == APPLIANCE_SYSTEM_API_NOT_SYSTEM ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_NOT_SYSTEM") == 0 :
                   status == APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED") == 0 :
                   status == APPLIANCE_SYSTEM_API_BAD_STATE ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_BAD_STATE") == 0 :
                   status == APPLIANCE_SYSTEM_API_BAD_VERSION ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_BAD_VERSION") == 0 :
                   status == APPLIANCE_SYSTEM_API_BAD_IDENTITY ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_BAD_IDENTITY") == 0 :
                   status == APPLIANCE_SYSTEM_API_BAD_CONFIG ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_BAD_CONFIG") == 0 :
                   status == APPLIANCE_SYSTEM_API_BAD_HEALTH ?
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_BAD_HEALTH") == 0 :
                       strcmp(string,
                              "APPLIANCE_SYSTEM_API_OVERFLOW") == 0),
              "system API status string is exact for every status");
    }
    CHECK(appliance_system_api_status_string(
              (enum appliance_system_api_status)10U) == NULL,
          "system API rejects undefined status string");
    for (state = APPLIANCE_SYSTEM_STATE_UNAVAILABLE;
         state <= APPLIANCE_SYSTEM_STATE_FAILED; ++state) {
        const char *string = appliance_system_api_state_string(state);

        CHECK(string != NULL &&
                  (state == APPLIANCE_SYSTEM_STATE_UNAVAILABLE ?
                       strcmp(string, "unavailable") == 0 :
                   state == APPLIANCE_SYSTEM_STATE_OK ?
                       strcmp(string, "ok") == 0 :
                   state == APPLIANCE_SYSTEM_STATE_DEGRADED ?
                       strcmp(string, "degraded") == 0 :
                       strcmp(string, "failed") == 0),
              "system API state string is exact for every state");
    }
    CHECK(appliance_system_api_state_string(
              (enum appliance_system_api_state)4U) == NULL,
          "system API rejects undefined state string");
}

static void test_snapshot_init_validation(void)
{
    struct appliance_identity identity = make_identity();
    struct appliance_system_api_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_system_api_snapshot_init(
              NULL, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_NULL,
          "system snapshot init rejects null snapshot");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, NULL,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_NULL,
          "system snapshot init rejects null identity");

    identity.version = 0U;
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_IDENTITY,
          "system snapshot init rejects invalid identity");
    identity = make_identity();

    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              (enum appliance_system_api_state)9U, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_HEALTH,
          "system snapshot init rejects unknown health state");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              (enum appliance_system_api_state)9U,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_CONFIG,
          "system snapshot init rejects unknown config state");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
              "node-316", 8U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_CONFIG,
          "system snapshot init rejects unavailable config with source");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_OK,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              "node-316", 8U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_CONFIG,
          "system snapshot init rejects available config without source");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_OK,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
              "bad_name", 8U, 0U, 0U) == APPLIANCE_SYSTEM_API_BAD_CONFIG,
          "system snapshot init rejects invalid config hostname");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U,
              APPLIANCE_SYSTEM_API_RECOVERY_MODE_MAX + 1U,
              0U) == APPLIANCE_SYSTEM_API_BAD_STATE,
          "system snapshot init rejects over-limit recovery mode");
    CHECK(appliance_system_api_snapshot_init(
              &snapshot, &identity,
              APPLIANCE_SYSTEM_STATE_OK, 1U,
              APPLIANCE_SYSTEM_STATE_UNAVAILABLE,
              APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE,
              NULL, 0U, 0U,
              APPLIANCE_SYSTEM_API_COUNT_MAX + 1U) ==
              APPLIANCE_SYSTEM_API_BAD_STATE,
          "system snapshot init rejects over-limit process count");
}

static void test_dispatch_semantics(void)
{
    struct appliance_system_api_snapshot snapshot =
        make_snapshot(APPLIANCE_SYSTEM_STATE_UNAVAILABLE);
    struct appliance_system_api_result result;
    struct appliance_system_api_result original;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;

    memset(&result, 0, sizeof(result));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET, NULL, 1U, &snapshot, &result) ==
              APPLIANCE_SYSTEM_API_NULL,
          "system API dispatch rejects null target");
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, NULL) == APPLIANCE_SYSTEM_API_NULL,
          "system API dispatch rejects null result");
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              NULL, &result) == APPLIANCE_SYSTEM_API_NULL,
          "system API dispatch rejects null snapshot");

    memset(&result, 0, sizeof(result));
    original = result;
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1",
              7U, &snapshot, &result) ==
              APPLIANCE_SYSTEM_API_NOT_SYSTEM,
          "version route is not system API");
    CHECK(memcmp(&result, &original, sizeof(result)) == 0,
          "non-system dispatch leaves result unchanged");
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1/system/status",
              21U, &snapshot, &result) ==
              APPLIANCE_SYSTEM_API_NOT_SYSTEM,
          "system child route is not system API in this boundary");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, &result) == APPLIANCE_SYSTEM_API_OK &&
              body_is(&result, unavailable_body,
                      sizeof(unavailable_body) - 1U) &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON,
          "GET system publishes exact unavailable-config body");
    CHECK(http_response_encode(
              &result.response, output, sizeof(output), &written) ==
              HTTP_RESPONSE_OK &&
              written > 0U &&
              memcmp(output, "HTTP/1.1 200 OK\r\n", 17U) == 0,
          "system GET HTTP status is exact");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, &result) == APPLIANCE_SYSTEM_API_OK &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON &&
              result.response.body_length ==
                  (uint32_t)(sizeof(unavailable_body) - 1U),
          "HEAD system publishes same bounded body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_UNSUPPORTED,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, &result) ==
              APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED &&
              body_is(&result, method_body,
                      sizeof(method_body) - 1U) &&
              result.response.status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "unsupported method on system route returns API 405");

    snapshot = make_snapshot(APPLIANCE_SYSTEM_STATE_OK);
    memset(&result, 0, sizeof(result));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, &result) == APPLIANCE_SYSTEM_API_OK &&
              body_is(&result, committed_body,
                      sizeof(committed_body) - 1U),
          "GET system publishes exact committed-config body");
}

static void test_determinism_and_bounds(void)
{
    struct appliance_system_api_snapshot snapshot =
        make_snapshot(APPLIANCE_SYSTEM_STATE_OK);
    struct appliance_system_api_result first;
    struct appliance_system_api_result second;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(appliance_system_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
              APPLIANCE_SYSTEM_API_PATH_LENGTH,
              &snapshot, &first) == APPLIANCE_SYSTEM_API_OK &&
              appliance_system_api_dispatch(
                  HTTP_METHOD_GET,
                  (const uint8_t *)APPLIANCE_SYSTEM_API_PATH,
                  APPLIANCE_SYSTEM_API_PATH_LENGTH,
                  &snapshot, &second) == APPLIANCE_SYSTEM_API_OK &&
              first.response.body_length ==
                  second.response.body_length &&
              memcmp(first.body, second.body,
                     sizeof(committed_body) - 1U) == 0,
          "system API repeat dispatch is byte-identical");
    CHECK(first.response.body_length <= APPLIANCE_SYSTEM_API_BODY_MAX &&
              second.response.body_length <= APPLIANCE_SYSTEM_API_BODY_MAX,
          "system API bodies are bounded");
}

int main(void)
{
    test_strings();
    test_snapshot_init_validation();
    test_dispatch_semantics();
    test_determinism_and_bounds();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "system API performs no allocation");
    if (failures != 0) {
        fprintf(stderr, "Appliance system API host failures: %d\n",
                failures);
        return 1;
    }
    puts("Appliance system API host test: ALL OK");
    return 0;
}

#endif
