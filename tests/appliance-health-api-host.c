#include "appliance_health.h"
#include "appliance_health_api.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static struct appliance_health_input make_ok_input(void)
{
    static const enum appliance_health_domain domains[
        APPLIANCE_HEALTH_DOMAIN_COUNT] = {
        APPLIANCE_HEALTH_DOMAIN_BOOT,
        APPLIANCE_HEALTH_DOMAIN_STORAGE,
        APPLIANCE_HEALTH_DOMAIN_CONFIGURATION,
        APPLIANCE_HEALTH_DOMAIN_NETWORK,
        APPLIANCE_HEALTH_DOMAIN_SERVICE,
        APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT,
    };
    struct appliance_health_input input;
    uint32_t index;

    memset(&input, 0, sizeof(input));
    input.version = APPLIANCE_HEALTH_VERSION;
    input.generation = 328U;
    for (index = 0U;
         index < APPLIANCE_HEALTH_DOMAIN_COUNT;
         ++index) {
        input.domains[index].version = APPLIANCE_HEALTH_VERSION;
        input.domains[index].domain = domains[index];
        input.domains[index].state = APPLIANCE_HEALTH_STATE_PASS;
        input.domains[index].fault = APPLIANCE_HEALTH_FAULT_NONE;
        input.domains[index].last_checked_ticks =
            (uint64_t)index + 1U;
    }
    return input;
}

static const uint8_t health_ok_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"health\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false,"
    "\"state\":\"OK\",\"healthy\":true,\"critical\":false,"
    "\"first_fault_domain\":\"NONE\",\"first_fault\":\"NONE\"}";
static const uint8_t health_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"health\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";

static struct appliance_health_api_snapshot make_snapshot(void)
{
    struct appliance_health_input input = make_ok_input();
    struct appliance_health_api_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (appliance_health_api_snapshot_init(
            &snapshot, &input) != APPLIANCE_HEALTH_API_OK) {
        fprintf(stderr, "failed fixture snapshot\n");
        __builtin_unreachable();
    }
    return snapshot;
}

static bool body_is(
    const struct appliance_health_api_result *result,
    const uint8_t *expected,
    size_t expected_length)
{
    return result->response.body_length == expected_length &&
           memcmp(result->response.body, expected, expected_length) == 0;
}

static void test_strings_empty(void)
{
    enum appliance_health_api_status status;

    for (status = APPLIANCE_HEALTH_API_OK;
         status <= APPLIANCE_HEALTH_API_OVERFLOW;
         ++status) {
        CHECK(appliance_health_api_status_string(status) != NULL,
              "appliance health api status string non-null");
    }
    CHECK(appliance_health_api_status_string(
              (enum appliance_health_api_status)99U) == NULL,
          "appliance health api rejects undefined status string");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance health api strings perform no allocation");
}

static void test_snapshot_init(void)
{
    struct appliance_health_input input = make_ok_input();
    struct appliance_health_api_snapshot snapshot;
    struct appliance_health_input bad = make_ok_input();

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_health_api_snapshot_init(NULL, &input) ==
              APPLIANCE_HEALTH_API_NULL,
          "appliance health api snapshot rejects null snapshot");
    CHECK(appliance_health_api_snapshot_init(&snapshot, NULL) ==
              APPLIANCE_HEALTH_API_NULL,
          "appliance health api snapshot rejects null input");
    CHECK(appliance_health_api_snapshot_init(&snapshot, &input) ==
              APPLIANCE_HEALTH_API_OK &&
              snapshot.published &&
              snapshot.health.version == APPLIANCE_HEALTH_VERSION &&
              snapshot.health.state == APPLIANCE_HEALTH_STATE_OK &&
              snapshot.health.healthy,
          "appliance health api snapshot publishes ok health");
    CHECK(appliance_health_api_snapshot_init(&snapshot, &input) ==
              APPLIANCE_HEALTH_API_BAD_STATE,
          "appliance health api snapshot rejects reinitialization");

    bad.domains[4].state = APPLIANCE_HEALTH_STATE_FAIL;
    bad.domains[4].fault = APPLIANCE_HEALTH_SERVICE_FAULT_FAILED;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_health_api_snapshot_init(&snapshot, &bad) ==
              APPLIANCE_HEALTH_API_OK &&
              snapshot.health.state == APPLIANCE_HEALTH_STATE_FAILED &&
              snapshot.health.healthy == false,
          "appliance health api snapshot accepts failed health");

    bad = make_ok_input();
    bad.domains[4].state = APPLIANCE_HEALTH_STATE_FAIL;
    bad.domains[4].fault = APPLIANCE_HEALTH_FAULT_NONE;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(appliance_health_api_snapshot_init(&snapshot, &bad) ==
              APPLIANCE_HEALTH_API_BAD_HEALTH,
          "appliance health api snapshot rejects invalid health");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance health api snapshot performs no allocation");
}

static void test_dispatch_ok(void)
{
    struct appliance_health_api_snapshot snapshot = make_snapshot();
    struct appliance_health_api_result result;
    struct appliance_health_api_result second;

    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_HEALTH_API_PATH,
              APPLIANCE_HEALTH_API_PATH_LENGTH,
              &snapshot,
              &result) == APPLIANCE_HEALTH_API_OK &&
              result.published &&
              result.response.status == HTTP_STATUS_200_OK &&
              body_is(&result, health_ok_body,
                      sizeof(health_ok_body) - 1U),
          "appliance health api GET returns exact status body");

    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_HEAD,
              (const uint8_t *)APPLIANCE_HEALTH_API_PATH,
              APPLIANCE_HEALTH_API_PATH_LENGTH,
              &snapshot,
              &result) == APPLIANCE_HEALTH_API_OK &&
              result.published &&
              result.response.status == HTTP_STATUS_200_OK &&
              body_is(&result, health_ok_body,
                      sizeof(health_ok_body) - 1U),
          "appliance health api HEAD returns exact status body");

    memset(&second, 0, sizeof(second));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_HEALTH_API_PATH,
              APPLIANCE_HEALTH_API_PATH_LENGTH,
              &snapshot,
              &second) == APPLIANCE_HEALTH_API_OK &&
              body_is(&second, health_ok_body,
                      sizeof(health_ok_body) - 1U),
          "appliance health api repeated GET is deterministic");
    CHECK(result.response.body_length <= APPLIANCE_HEALTH_API_BODY_MAX &&
              second.response.body_length <= APPLIANCE_HEALTH_API_BODY_MAX,
          "appliance health api output is bounded");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance health api GET performs no allocation");
}

static void test_dispatch_denials_and_bad_state(void)
{
    struct appliance_health_api_snapshot snapshot = make_snapshot();
    struct appliance_health_api_snapshot bad = snapshot;
    struct appliance_health_api_result result;

    bad.health.published = false;
    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)"/api/v1/health/child",
              20U,
              &snapshot,
              &result) == APPLIANCE_HEALTH_API_NOT_HEALTH,
          "appliance health api rejects child path");
    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_UNSUPPORTED,
              (const uint8_t *)APPLIANCE_HEALTH_API_PATH,
              APPLIANCE_HEALTH_API_PATH_LENGTH,
              &snapshot,
              &result) == APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED &&
              body_is(&result, health_method_body,
                      sizeof(health_method_body) - 1U),
          "appliance health api rejects unsupported method");
    memset(&result, 0xA5, sizeof(result));
    CHECK(appliance_health_api_dispatch(
              HTTP_METHOD_GET,
              (const uint8_t *)APPLIANCE_HEALTH_API_PATH,
              APPLIANCE_HEALTH_API_PATH_LENGTH,
              &bad,
              &result) == APPLIANCE_HEALTH_API_BAD_STATE,
          "appliance health api rejects unpublished health state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance health api denial performs no allocation");
}

int main(void)
{
    test_strings_empty();
    test_snapshot_init();
    test_dispatch_ok();
    test_dispatch_denials_and_bad_state();
    if (failures != 0) {
        fprintf(stderr, "Appliance health api host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance health api host tests: ALL OK");
    return 0;
}
