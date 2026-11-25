#include "appliance_service_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SERVICE_API_HOST_TEST

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
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"service\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";
static const uint8_t running_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"service\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"state\":\"running\",\"policy\":\"rollback\",\"generation\":1,"
    "\"configured_count\":1,\"active_count\":1,\"ready_count\":1,"
    "\"pending_count\":0,\"failed_count\":0,\"exhausted_count\":0,"
    "\"health\":\"pass\",\"fault\":\"none\"}";

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

static struct appliance_service_manager_snapshot make_manager_snapshot(void)
{
    struct appliance_service_manager_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = APPLIANCE_SERVICE_MANAGER_VERSION;
    snapshot.published = true;
    snapshot.state = APPLIANCE_SERVICE_MANAGER_STATE_RUNNING;
    snapshot.startup_policy = APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK;
    snapshot.generation = 1U;
    snapshot.configured_count = 1U;
    snapshot.active_count = 1U;
    snapshot.ready_count = 1U;
    snapshot.health_result = HEALTH_CHECK_RESULT_PASS;
    snapshot.health_fault = SERVICE_HEALTH_FAULT_NONE;
    return snapshot;
}

static struct appliance_service_api_snapshot make_snapshot(void)
{
    struct appliance_service_manager_snapshot manager =
        make_manager_snapshot();
    struct appliance_service_api_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_service_api_snapshot_init(
            &snapshot, &manager) != APPLIANCE_SERVICE_API_OK) {
        fprintf(stderr, "fixture failed service snapshot init\n");
        __builtin_unreachable();
    }
    return snapshot;
}

static bool body_is(
    const struct appliance_service_api_result *result,
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
    enum appliance_service_api_status status;
    enum appliance_service_api_route route;

    for (status = APPLIANCE_SERVICE_API_OK;
         status <= APPLIANCE_SERVICE_API_OVERFLOW;
         ++status) {
        CHECK(appliance_service_api_status_string(status) != NULL,
              "service API status string is present for every status");
    }
    CHECK(appliance_service_api_status_string(
              (enum appliance_service_api_status)8U) == NULL,
          "service API rejects undefined status string");

    for (route = APPLIANCE_SERVICE_API_ROUTE_NONE;
         route <= APPLIANCE_SERVICE_API_ROUTE_STATUS;
         ++route) {
        CHECK(appliance_service_api_route_string(route) != NULL,
              "service API route string is present for every route");
    }
    CHECK(appliance_service_api_route_string(
              (enum appliance_service_api_route)2U) == NULL,
          "service API rejects undefined route string");
}

static void test_snapshot_init_validation(void)
{
    struct appliance_service_manager_snapshot manager =
        make_manager_snapshot();
    struct appliance_service_api_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_service_api_snapshot_init(NULL, &manager) ==
              APPLIANCE_SERVICE_API_NULL,
          "service snapshot init rejects null snapshot");
    CHECK(appliance_service_api_snapshot_init(&snapshot, NULL) ==
              APPLIANCE_SERVICE_API_NULL,
          "service snapshot init rejects null manager");

    manager.published = false;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects unpublished manager");
    manager = make_manager_snapshot();
    manager.version = 99U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects manager version");
    manager = make_manager_snapshot();
    manager.state = (enum appliance_service_manager_state)9U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects manager state");
    manager = make_manager_snapshot();
    manager.startup_policy =
        (enum appliance_service_manager_startup_policy)9U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects startup policy");
    manager = make_manager_snapshot();
    manager.health_result = (enum health_check_result_state)9U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects health result");
    manager = make_manager_snapshot();
    manager.health_fault = (enum service_health_fault)9U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects health fault");
    manager = make_manager_snapshot();
    manager.configured_count = APPLIANCE_SERVICE_MANAGER_CAPACITY + 1U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects configured overflow");
    manager = make_manager_snapshot();
    manager.active_count = SERVICE_SUPERVISOR_CAPACITY + 1U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects active overflow");
    manager = make_manager_snapshot();
    manager.pending_count = 1U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot init rejects incoherent counts");
    manager = make_manager_snapshot();
    manager.failed_count = 1U;
    manager.ready_count = 0U;
    manager.health_result = HEALTH_CHECK_RESULT_FAIL;
    manager.health_fault = SERVICE_HEALTH_FAULT_READINESS;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_OK &&
              snapshot.manager.failed_count == 1U &&
              snapshot.manager.health_result == HEALTH_CHECK_RESULT_FAIL &&
              snapshot.manager.health_fault ==
                  SERVICE_HEALTH_FAULT_READINESS,
          "service snapshot copies a valid manager failure count");
    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.failed_count = 1U;
    manager.ready_count = 0U;
    manager.exhausted_count = 1U;
    manager.health_result = HEALTH_CHECK_RESULT_FAIL;
    manager.health_fault = SERVICE_HEALTH_FAULT_RESTART;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_OK &&
              snapshot.manager.exhausted_count == 1U &&
              snapshot.manager.health_fault ==
                  SERVICE_HEALTH_FAULT_RESTART,
          "service snapshot copies valid failure health");

    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.health_fault = SERVICE_HEALTH_FAULT_READINESS;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot rejects incoherent pass/readiness health");
    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.health_result = HEALTH_CHECK_RESULT_WARN;
    manager.health_fault = SERVICE_HEALTH_FAULT_NONE;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot rejects incoherent warn health");
    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.health_result = HEALTH_CHECK_RESULT_FAIL;
    manager.health_fault = SERVICE_HEALTH_FAULT_NONE;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot rejects incoherent fail health");
    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.failed_count = 1U;
    manager.ready_count = 0U;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_MANAGER,
          "service snapshot rejects failed count without fail health");
    memset(&snapshot, 0, sizeof(snapshot));
    manager = make_manager_snapshot();
    manager.active_count = 1U;
    manager.ready_count = 0U;
    manager.pending_count = 1U;
    manager.health_result = HEALTH_CHECK_RESULT_UNKNOWN;
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_OK &&
              snapshot.manager.pending_count == 1U,
          "service snapshot accepts coherent unknown health");

    manager = make_manager_snapshot();
    CHECK(appliance_service_api_snapshot_init(&snapshot, &manager) ==
              APPLIANCE_SERVICE_API_BAD_STATE,
          "service snapshot double init returns BAD_STATE");
}

static void test_dispatch_semantics(void)
{
    struct appliance_service_api_snapshot snapshot = make_snapshot();
    struct appliance_service_api_result result;
    struct appliance_service_api_result original;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;

    memset(&result, 0, sizeof(result));
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET, NULL, 1U, &snapshot, &result) ==
              APPLIANCE_SERVICE_API_NULL,
          "service API dispatch rejects null target");
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              NULL, &result) == APPLIANCE_SERVICE_API_NULL,
          "service API dispatch rejects null snapshot");
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              &snapshot, NULL) == APPLIANCE_SERVICE_API_NULL,
          "service API dispatch rejects null result");

    memset(&result, 0, sizeof(result));
    original = result;
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1",
              7U, &snapshot, &result) ==
              APPLIANCE_SERVICE_API_NOT_SERVICE,
          "version route is not service API");
    CHECK(memcmp(&result, &original, sizeof(result)) == 0,
          "non-service dispatch leaves result unchanged");
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1/service/child",
              22U, &snapshot, &result) ==
              APPLIANCE_SERVICE_API_NOT_SERVICE,
          "unknown service child route is not owned in this boundary");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              &snapshot, &result) == APPLIANCE_SERVICE_API_OK &&
              result.route == APPLIANCE_SERVICE_API_ROUTE_STATUS &&
              body_is(&result, running_body,
                      sizeof(running_body) - 1U) &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.content_type == HTTP_CONTENT_TYPE_JSON,
          "GET service publishes exact running body");
    CHECK(http_response_encode(
              &result.response, output, sizeof(output), &written) ==
              HTTP_RESPONSE_OK &&
              written > 0U &&
              memcmp(output, "HTTP/1.1 200 OK\r\n", 17U) == 0,
          "service GET HTTP status is exact");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              &snapshot, &result) == APPLIANCE_SERVICE_API_OK &&
              result.response.status == HTTP_STATUS_200_OK &&
              result.response.body_length ==
                  (uint32_t)(sizeof(running_body) - 1U),
          "HEAD service publishes same bounded body");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_UNSUPPORTED,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              &snapshot, &result) ==
              APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED &&
              result.route == APPLIANCE_SERVICE_API_ROUTE_STATUS &&
              body_is(&result, method_body,
                      sizeof(method_body) - 1U) &&
              result.response.status ==
                  HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "unsupported method on service route returns API 405");
}

static void test_determinism_and_bounds(void)
{
    struct appliance_service_api_snapshot snapshot = make_snapshot();
    struct appliance_service_api_result first;
    struct appliance_service_api_result second;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(appliance_service_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
              APPLIANCE_SERVICE_API_PATH_LENGTH,
              &snapshot, &first) == APPLIANCE_SERVICE_API_OK &&
              appliance_service_api_dispatch(
                  HTTP_METHOD_GET,
                  (const uint8_t *)APPLIANCE_SERVICE_API_PATH,
                  APPLIANCE_SERVICE_API_PATH_LENGTH,
                  &snapshot, &second) == APPLIANCE_SERVICE_API_OK &&
              first.response.body_length ==
                  second.response.body_length &&
              memcmp(first.body, second.body,
                     sizeof(running_body) - 1U) == 0,
          "service API repeat dispatch is byte-identical");
    CHECK(first.response.body_length <= APPLIANCE_SERVICE_API_BODY_MAX &&
              second.response.body_length <= APPLIANCE_SERVICE_API_BODY_MAX,
          "service API bodies are bounded");
}

int main(void)
{
    test_strings();
    test_snapshot_init_validation();
    test_dispatch_semantics();
    test_determinism_and_bounds();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service API performs no allocation");
    if (failures != 0) {
        fprintf(stderr, "Appliance service API host failures: %d\n",
                failures);
        return 1;
    }
    puts("Appliance service API host test: ALL OK");
    return 0;
}

#endif
