#include "appliance_management_api.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const uint8_t version_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"management\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false}";
static const uint8_t not_found_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"error\":\"not_found\","
    "\"auth\":\"unavailable\",\"mutating\":false}";
static const uint8_t method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"error\":\"method_not_allowed\","
    "\"auth\":\"unavailable\",\"mutating\":false}";

static bool starts_with(
    const uint8_t *left,
    size_t left_length,
    const char *right)
{
    size_t right_length = strlen(right);

    return left_length >= right_length &&
           memcmp(left, right, right_length) == 0;
}

static void check_result_unchanged(
    const struct appliance_management_api_result *result,
    const unsigned char *original)
{
    CHECK(memcmp(result, original, sizeof(*result)) == 0,
          "non-management dispatch leaves result bytes unchanged");
}

static void test_status_and_route_strings(void)
{
    CHECK(strcmp(
              appliance_management_api_status_string(
                  APPLIANCE_MANAGEMENT_API_OK),
              "APPLIANCE_MANAGEMENT_API_OK") == 0,
          "management API OK status string is exact");
    CHECK(strcmp(
              appliance_management_api_status_string(
                  APPLIANCE_MANAGEMENT_API_NULL),
              "APPLIANCE_MANAGEMENT_API_NULL") == 0,
          "management API NULL status string is exact");
    CHECK(strcmp(
              appliance_management_api_status_string(
                  APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT),
              "APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT") == 0,
          "management API not-management status string is exact");
    CHECK(strcmp(
              appliance_management_api_status_string(
                  APPLIANCE_MANAGEMENT_API_NOT_FOUND),
              "APPLIANCE_MANAGEMENT_API_NOT_FOUND") == 0,
          "management API not-found status string is exact");
    CHECK(strcmp(
              appliance_management_api_status_string(
                  APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED),
              "APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED") == 0,
          "management API method-not-allowed status string is exact");
    CHECK(appliance_management_api_status_string(
              (enum appliance_management_api_status)99U) == NULL,
          "undefined management API status string is null");
    CHECK(strcmp(
              appliance_management_api_route_string(
                  APPLIANCE_MANAGEMENT_API_ROUTE_NONE),
              "APPLIANCE_MANAGEMENT_API_ROUTE_NONE") == 0,
          "management API none route string is exact");
    CHECK(strcmp(
              appliance_management_api_route_string(
                  APPLIANCE_MANAGEMENT_API_ROUTE_VERSION),
              "APPLIANCE_MANAGEMENT_API_ROUTE_VERSION") == 0,
          "management API version route string is exact");
    CHECK(appliance_management_api_route_string(
              (enum appliance_management_api_route)99U) == NULL,
          "undefined management API route string is null");
}

static void test_null_and_unpublished(void)
{
    struct appliance_management_api_result result;
    unsigned char original[sizeof(result)];

    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              NULL,
              1U,
              &result) == APPLIANCE_MANAGEMENT_API_NULL,
          "management API dispatch rejects null target");
    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1",
              APPLIANCE_MANAGEMENT_API_PATH_LENGTH,
              NULL) == APPLIANCE_MANAGEMENT_API_NULL,
          "management API dispatch rejects null result");

    memset(&result, 0, sizeof(result));
    memcpy(original, &result, sizeof(original));
    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/",
              1U,
              &result) == APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT,
          "root target is not management API");
    check_result_unchanged(&result, original);

    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api2/v1",
              8U,
              &result) == APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT,
          "api2 prefix is not management API");
    check_result_unchanged(&result, original);

    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/API/v1",
              7U,
              &result) == APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT,
          "uppercase API prefix is not management API");
    check_result_unchanged(&result, original);
}

static void check_version_response(
    enum http_method_kind method,
    const char *message)
{
    struct appliance_management_api_result result;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;

    memset(&result, 0, sizeof(result));
    CHECK(appliance_management_api_dispatch(
              method,
              (const uint8_t *)APPLIANCE_MANAGEMENT_API_PATH,
              APPLIANCE_MANAGEMENT_API_PATH_LENGTH,
              &result) == APPLIANCE_MANAGEMENT_API_OK &&
              result.published &&
              result.version == APPLIANCE_MANAGEMENT_API_VERSION &&
              result.route ==
                  APPLIANCE_MANAGEMENT_API_ROUTE_VERSION &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON &&
              result.response.body != NULL &&
              result.response.body_length ==
                  (uint32_t)(sizeof(version_body) - 1U) &&
              memcmp(
                  result.response.body,
                  version_body,
                  sizeof(version_body) - 1U) == 0,
          message);

    if (method == HTTP_METHOD_GET) {
        CHECK(http_response_encode(
                  &result.response,
                  output,
                  sizeof(output),
                  &written) == HTTP_RESPONSE_OK &&
                  starts_with(output, written,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"),
              "version GET response status and content type are exact");
    } else {
        CHECK(http_response_encode_head(
                  &result.response,
                  output,
                  sizeof(output),
                  &written) == HTTP_RESPONSE_OK &&
                  starts_with(output, written,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"),
              "version HEAD response status and content type are exact");
    }
}

static void check_error_response(
    enum http_method_kind method,
    const char *target,
    enum appliance_management_api_status expected_status,
    enum http_status_code expected_http,
    const uint8_t *expected_body,
    const char *message)
{
    struct appliance_management_api_result result;

    memset(&result, 0, sizeof(result));
    CHECK(appliance_management_api_dispatch(
              method,
              (const uint8_t *)target,
              (uint32_t)strlen(target),
              &result) == expected_status &&
              result.published &&
              result.version == APPLIANCE_MANAGEMENT_API_VERSION &&
              result.response.status == expected_http &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON &&
              result.response.body != NULL &&
              result.response.body_length ==
                  (uint32_t)(strlen((const char *)expected_body)) &&
              memcmp(
                  result.response.body,
                  expected_body,
                  strlen((const char *)expected_body)) == 0,
          message);
}

static void test_dispatch_semantics(void)
{
    check_version_response(
        HTTP_METHOD_GET,
        "GET /api/v1 publishes exact version discovery body");
    check_version_response(
        HTTP_METHOD_HEAD,
        "HEAD /api/v1 publishes exact version discovery body");

    check_error_response(
        HTTP_METHOD_UNSUPPORTED,
        APPLIANCE_MANAGEMENT_API_PATH,
        APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED,
        HTTP_STATUS_405_METHOD_NOT_ALLOWED,
        method_body,
        "unsupported method on exact version route maps to API 405");
    check_error_response(
        HTTP_METHOD_NONE,
        APPLIANCE_MANAGEMENT_API_PATH,
        APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED,
        HTTP_STATUS_405_METHOD_NOT_ALLOWED,
        method_body,
        "none method on exact version route maps to API 405");

    check_error_response(
        HTTP_METHOD_GET,
        "/api",
        APPLIANCE_MANAGEMENT_API_NOT_FOUND,
        HTTP_STATUS_404_NOT_FOUND,
        not_found_body,
        "missing API version is an API 404");
    check_error_response(
        HTTP_METHOD_GET,
        "/api/v1/",
        APPLIANCE_MANAGEMENT_API_NOT_FOUND,
        HTTP_STATUS_404_NOT_FOUND,
        not_found_body,
        "trailing slash is not normalized to API version");
    check_error_response(
        HTTP_METHOD_GET,
        "/api/v1/status",
        APPLIANCE_MANAGEMENT_API_NOT_FOUND,
        HTTP_STATUS_404_NOT_FOUND,
        not_found_body,
        "future operation route remains an API 404");
    check_error_response(
        HTTP_METHOD_GET,
        "/api/v2",
        APPLIANCE_MANAGEMENT_API_NOT_FOUND,
        HTTP_STATUS_404_NOT_FOUND,
        not_found_body,
        "unsupported API version remains an API 404");
    check_error_response(
        HTTP_METHOD_GET,
        "/api/v1?x=1",
        APPLIANCE_MANAGEMENT_API_NOT_FOUND,
        HTTP_STATUS_404_NOT_FOUND,
        not_found_body,
        "query-bearing API target is not normalized to version route");
}

static void test_determinism_and_bounds(void)
{
    struct appliance_management_api_result first;
    struct appliance_management_api_result second;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(appliance_management_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_MANAGEMENT_API_PATH,
              APPLIANCE_MANAGEMENT_API_PATH_LENGTH,
              &first) == APPLIANCE_MANAGEMENT_API_OK &&
              appliance_management_api_dispatch(
                  HTTP_METHOD_GET,
                  (const uint8_t *)APPLIANCE_MANAGEMENT_API_PATH,
                  APPLIANCE_MANAGEMENT_API_PATH_LENGTH,
                  &second) == APPLIANCE_MANAGEMENT_API_OK &&
              memcmp(&first, &second, sizeof(first)) == 0,
          "management API dispatch is deterministic");
    CHECK(first.response.body_length <=
              APPLIANCE_MANAGEMENT_API_BODY_MAX &&
              second.response.body_length <=
                  APPLIANCE_MANAGEMENT_API_BODY_MAX,
          "management API bodies are bounded");
}

int main(void)
{
    test_status_and_route_strings();
    test_null_and_unpublished();
    test_dispatch_semantics();
    test_determinism_and_bounds();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance management API host failures: %d\n",
                failures);
        return 1;
    }
    printf("Appliance management API host test: ALL OK\n");
    return 0;
}
