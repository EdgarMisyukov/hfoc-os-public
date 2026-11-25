#include "https_service_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HTTPS_SERVICE_LIFECYCLE_HOST_TEST

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

enum test_callback_kind {
    TEST_CALLBACK_STOP = 0,
    TEST_CALLBACK_DRAIN = 1,
    TEST_CALLBACK_ABORT = 2,
    TEST_CALLBACK_ZEROIZE = 3,
    TEST_CALLBACK_HTTP = 4,
    TEST_CALLBACK_PACKET = 5,
    TEST_CALLBACK_RESTART = 6,
};

struct test_sequence_entry {
    enum test_callback_kind kind;
    uint32_t value;
};

struct test_callback_state {
    bool stop_result;
    bool drain_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool abort_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool zeroize_result;
    bool http_result;
    bool packet_result;
    bool restart_result;
    unsigned int stop_calls;
    unsigned int drain_calls;
    unsigned int abort_calls;
    unsigned int zeroize_calls;
    unsigned int http_calls;
    unsigned int packet_calls;
    unsigned int restart_calls;
    unsigned int sequence_count;
    struct test_sequence_entry sequence[64];
};

static bool test_stop_admission(void *context, uint32_t generation)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;

    ++state->stop_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_STOP;
    state->sequence[state->sequence_count].value = generation;
    ++state->sequence_count;
    return state->stop_result;
}

static bool test_drain_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;
    bool result;

    ++state->drain_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_DRAIN;
    state->sequence[state->sequence_count].value =
        generation * 10U + slot;
    ++state->sequence_count;
    result = state->drain_results[slot];
    state->drain_results[slot] = false;
    return result;
}

static bool test_abort_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;
    bool result;

    ++state->abort_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_ABORT;
    state->sequence[state->sequence_count].value =
        generation * 10U + slot;
    ++state->sequence_count;
    result = state->abort_results[slot];
    state->abort_results[slot] = false;
    return result;
}

static bool test_zeroize_tls(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;

    ++state->zeroize_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_ZEROIZE;
    state->sequence[state->sequence_count].value = generation * 100U + count;
    ++state->sequence_count;
    return state->zeroize_result;
}

static bool test_release_http(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;

    ++state->http_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_HTTP;
    state->sequence[state->sequence_count].value = generation * 100U + count;
    ++state->sequence_count;
    return state->http_result;
}

static bool test_release_packet(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;

    ++state->packet_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_PACKET;
    state->sequence[state->sequence_count].value = generation * 100U + count;
    ++state->sequence_count;
    return state->packet_result;
}

static bool test_restart_admission(void *context, uint32_t generation)
{
    struct test_callback_state *state =
        (struct test_callback_state *)context;

    ++state->restart_calls;
    state->sequence[state->sequence_count].kind = TEST_CALLBACK_RESTART;
    state->sequence[state->sequence_count].value = generation;
    ++state->sequence_count;
    return state->restart_result;
}

static struct https_service_lifecycle_config make_config(void)
{
    struct https_service_lifecycle_config config;

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    config.drain_timeout_ms = 100U;
    config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
    config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY;
    config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY;
    config.packet_pool_capacity = 16U;
    return config;
}

static void init_callback_state(struct test_callback_state *state)
{
    uint32_t index;

    memset(state, 0, sizeof(*state));
    state->stop_result = true;
    state->zeroize_result = true;
    state->http_result = true;
    state->packet_result = true;
    state->restart_result = true;
    for (index = 0U;
         index < HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
         ++index) {
        state->drain_results[index] = true;
        state->abort_results[index] = true;
    }
}

static struct https_service_lifecycle_ops make_ops(
    struct test_callback_state *state)
{
    struct https_service_lifecycle_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.stop_admission = test_stop_admission;
    ops.drain_connection = test_drain_connection;
    ops.abort_connection = test_abort_connection;
    ops.zeroize_tls = test_zeroize_tls;
    ops.release_http = test_release_http;
    ops.release_packet = test_release_packet;
    ops.restart_admission = test_restart_admission;
    ops.context = state;
    return ops;
}

static void check_init_failure(
    struct https_service_lifecycle *lifecycle,
    const struct https_service_lifecycle_config *config,
    const struct https_service_lifecycle_ops *ops,
    enum https_service_lifecycle_status expected,
    const char *message)
{
    unsigned char original[sizeof(*lifecycle)];

    if (lifecycle == NULL) {
        CHECK(https_service_lifecycle_init(
                  lifecycle, config, ops) == expected,
              message);
        return;
    }
    memset(lifecycle, 0, sizeof(*lifecycle));
    memcpy(original, lifecycle, sizeof(*lifecycle));
    CHECK(https_service_lifecycle_init(
              lifecycle, config, ops) == expected &&
              memcmp(lifecycle, original, sizeof(*lifecycle)) == 0,
          message);
}

static void test_strings(void)
{
    static const char *phases[] = {
        "HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE",
        "HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING",
        "HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING",
        "HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED",
        "HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED",
        "HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED",
    };
    static const char *statuses[] = {
        "HTTPS_SERVICE_LIFECYCLE_OK",
        "HTTPS_SERVICE_LIFECYCLE_NULL",
        "HTTPS_SERVICE_LIFECYCLE_BAD_STATE",
        "HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION",
        "HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG",
        "HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED",
        "HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED",
        "HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE",
        "HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION",
        "HTTPS_SERVICE_LIFECYCLE_INVALID_OPS",
        "HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_DRAIN_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED",
        "HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED",
        "HTTPS_SERVICE_LIFECYCLE_GENERATION_EXHAUSTED",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(phases) / sizeof(phases[0]);
         ++index) {
        const char *string = https_service_lifecycle_phase_string(
            (enum https_service_lifecycle_phase)index);

        CHECK(string != NULL &&
                  strcmp(string, phases[index]) == 0,
              "HTTPS lifecycle phase string is exact");
    }
    CHECK(https_service_lifecycle_phase_string(
              (enum https_service_lifecycle_phase)99U) == NULL,
          "HTTPS lifecycle unknown phase string is rejected");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = https_service_lifecycle_status_string(
            (enum https_service_lifecycle_status)index);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index]) == 0,
              "HTTPS lifecycle status string is exact");
    }
    CHECK(https_service_lifecycle_status_string(
              (enum https_service_lifecycle_status)99U) == NULL,
          "HTTPS lifecycle unknown status string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle strings perform no allocation");
}

static void test_init(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    unsigned char original[sizeof(lifecycle)];

    memset(&lifecycle, 0xA5, sizeof(lifecycle));
    memcpy(original, &lifecycle, sizeof(lifecycle));
    check_init_failure(
        &lifecycle, NULL, &ops, HTTPS_SERVICE_LIFECYCLE_NULL,
        "HTTPS lifecycle init rejects null config");
    check_init_failure(
        &lifecycle, &config, NULL, HTTPS_SERVICE_LIFECYCLE_NULL,
        "HTTPS lifecycle init rejects null ops");
    check_init_failure(
        NULL, &config, &ops, HTTPS_SERVICE_LIFECYCLE_NULL,
        "HTTPS lifecycle init rejects null lifecycle");

    config.version = HTTPS_SERVICE_LIFECYCLE_VERSION + 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION,
        "HTTPS lifecycle init rejects unknown version");
    config = make_config();
    config.drain_timeout_ms =
        HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MIN - 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects zero drain timeout");
    config = make_config();
    config.drain_timeout_ms =
        HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MAX + 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects over-limit drain timeout");
    config = make_config();
    config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY + 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects mutable connection capacity");
    config = make_config();
    config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY - 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects mutable TLS capacity");
    config = make_config();
    config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY - 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects mutable HTTP capacity");
    config = make_config();
    config.packet_pool_capacity =
        HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MIN - 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects zero packet capacity");
    config = make_config();
    config.packet_pool_capacity =
        HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MAX + 1U;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG,
        "HTTPS lifecycle init rejects over-limit packet capacity");

    config = make_config();
    ops.context = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects null callback context");
    ops = make_ops(&state);
    ops.stop_admission = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing stop callback");
    ops = make_ops(&state);
    ops.drain_connection = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing drain callback");
    ops = make_ops(&state);
    ops.abort_connection = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing abort callback");
    ops = make_ops(&state);
    ops.zeroize_tls = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing zeroize callback");
    ops = make_ops(&state);
    ops.release_http = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing HTTP callback");
    ops = make_ops(&state);
    ops.release_packet = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing packet callback");
    ops = make_ops(&state);
    ops.restart_admission = NULL;
    check_init_failure(
        &lifecycle, &config, &ops, HTTPS_SERVICE_LIFECYCLE_INVALID_OPS,
        "HTTPS lifecycle init rejects missing restart callback");

    ops = make_ops(&state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_published(&lifecycle) &&
              lifecycle.version == HTTPS_SERVICE_LIFECYCLE_VERSION &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE &&
              lifecycle.generation == 0U &&
              lifecycle.drain_timeout_ms == 100U &&
              lifecycle.connection_capacity ==
                  HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY &&
              lifecycle.tls_context_capacity ==
                  HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY &&
              lifecycle.http_parser_capacity ==
                  HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY &&
              lifecycle.packet_pool_capacity == 16U &&
              lifecycle.drain_deadline_ms == 0U &&
              lifecycle.active_slots == 0U &&
              lifecycle.stopped_count == 0U &&
              lifecycle.drained_count == 0U &&
              lifecycle.aborted_count == 0U &&
              lifecycle.zeroized_count == 0U &&
              lifecycle.http_released_count == 0U &&
              lifecycle.packet_released_count == 0U &&
              lifecycle.restarted_count == 0U,
          "HTTPS lifecycle init publishes exact IDLE state");
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) ==
              HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED,
          "HTTPS lifecycle init rejects double init");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle init performs no allocation");
}

static void test_start(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    unsigned char original[sizeof(lifecycle)];

    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle start fixture initializes");
    memcpy(original, &lifecycle, sizeof(lifecycle));
    CHECK(https_service_lifecycle_start(
              &lifecycle, 0U) == HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle start rejects zero generation");
    CHECK(https_service_lifecycle_start(
              &lifecycle,
              HTTPS_SERVICE_LIFECYCLE_GENERATION_MAX + 1U) ==
              HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION,
          "HTTPS lifecycle start rejects over-limit generation");
    CHECK(https_service_lifecycle_start(
              &lifecycle, 1U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING &&
              lifecycle.generation == 1U &&
              lifecycle.active_slots == 0x0FU,
          "HTTPS lifecycle start publishes LISTENING with four active slots");
    CHECK(https_service_lifecycle_start(
              &lifecycle, 2U) == HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE,
          "HTTPS lifecycle start rejects non-IDLE phase");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle start performs no allocation");
}

static void test_stop_admission_flow(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    unsigned char original[sizeof(lifecycle)];

    init_callback_state(&state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 7U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle stop fixture starts");
    CHECK(https_service_lifecycle_stop_admission(
              &lifecycle, 1000U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING &&
              lifecycle.drain_deadline_ms == 1100U &&
              lifecycle.stopped_count == 1U &&
              state.stop_calls == 1U,
          "HTTPS lifecycle stop admission arms exact deadline");

    CHECK(https_service_lifecycle_stop_admission(
              &lifecycle, 2000U) == HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE,
          "HTTPS lifecycle repeated stop from DRAINING is rejected");

    memset(&lifecycle, 0, sizeof(lifecycle));
    init_callback_state(&state);
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 8U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle stop-failure fixture starts");
    state.stop_result = false;
    memcpy(original, &lifecycle, sizeof(lifecycle));
    CHECK(https_service_lifecycle_stop_admission(
              &lifecycle, 1000U) ==
              HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle stop callback failure leaves state unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle stop performs no allocation");
}

static void test_drain_and_abort(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    struct https_service_lifecycle_snapshot snapshot;

    init_callback_state(&state);
    state.drain_results[0] = true;
    state.drain_results[1] = true;
    state.drain_results[2] = false;
    state.drain_results[3] = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 9U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_stop_admission(
                  &lifecycle, 1000U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle drain fixture starts draining");
    CHECK(https_service_lifecycle_step(
              &lifecycle, 1050U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.drained_count == 2U &&
              lifecycle.aborted_count == 0U &&
              lifecycle.active_slots == 0xCU &&
              state.drain_calls == 4U,
          "HTTPS lifecycle step before deadline drains released slots");
    CHECK(https_service_lifecycle_step(
              &lifecycle, 1500U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED &&
              lifecycle.aborted_count == 2U &&
              lifecycle.active_slots == 0U &&
              state.abort_calls == 2U,
          "HTTPS lifecycle deadline aborts remaining slots exactly");
    CHECK(https_service_lifecycle_snapshot(
              &lifecycle, &snapshot) == HTTPS_SERVICE_LIFECYCLE_OK &&
              snapshot.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED &&
              snapshot.generation == 9U &&
              snapshot.active_connection_count == 0U &&
              snapshot.drained_count == 2U &&
              snapshot.aborted_count == 2U,
          "HTTPS lifecycle snapshot publishes exact drain state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle drain step performs no allocation");
}

static void test_finish_failures_and_cleanup(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    unsigned char original[sizeof(lifecycle)];
    uint32_t index;

    init_callback_state(&state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 5U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_stop_admission(
                  &lifecycle, 100U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle cleanup fixture starts draining");
    for (index = 0U; index < 4U; ++index) {
        state.drain_results[index] = true;
    }
    CHECK(https_service_lifecycle_step(
              &lifecycle, 150U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED,
          "HTTPS lifecycle cleanup fixture drains all slots");
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED &&
              lifecycle.zeroized_count ==
                  HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY &&
              lifecycle.http_released_count ==
                  HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY &&
              lifecycle.packet_released_count == 16U &&
              state.zeroize_calls == 1U &&
              state.http_calls == 1U &&
              state.packet_calls == 1U,
          "HTTPS lifecycle finish zeroizes TLS and releases HTTP/packet");
    CHECK(state.sequence_count == 8U &&
              state.sequence[0].kind == TEST_CALLBACK_STOP &&
              state.sequence[0].value == 5U &&
              state.sequence[1].kind == TEST_CALLBACK_DRAIN &&
              state.sequence[4].kind == TEST_CALLBACK_DRAIN &&
              state.sequence[5].kind == TEST_CALLBACK_ZEROIZE &&
              state.sequence[5].value == 500U + 4U &&
              state.sequence[6].kind == TEST_CALLBACK_HTTP &&
              state.sequence[6].value == 500U + 4U &&
              state.sequence[7].kind == TEST_CALLBACK_PACKET &&
              state.sequence[7].value == 500U + 16U,
          "HTTPS lifecycle cleanup callback order is exact");
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE,
          "HTTPS lifecycle finish after STOPPED is rejected");

    memset(&lifecycle, 0, sizeof(lifecycle));
    init_callback_state(&state);
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 6U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_stop_admission(
                  &lifecycle, 100U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle cleanup-failure fixture starts draining");
    for (index = 0U; index < 4U; ++index) {
        state.drain_results[index] = true;
    }
    CHECK(https_service_lifecycle_step(
              &lifecycle, 150U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle cleanup-failure fixture drains all slots");
    memcpy(original, &lifecycle, sizeof(lifecycle));
    state.zeroize_result = false;
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle zeroize failure leaves state unchanged");
    state.zeroize_result = true;
    state.http_result = false;
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle HTTP release failure leaves state unchanged");
    state.http_result = true;
    state.packet_result = false;
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle packet release failure leaves state unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle finish performs no allocation");
}

static void test_finish_before_empty(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    unsigned char original[sizeof(lifecycle)];

    init_callback_state(&state);
    state.drain_results[0] = true;
    state.drain_results[1] = false;
    state.drain_results[2] = false;
    state.drain_results[3] = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 4U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_stop_admission(
                  &lifecycle, 100U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_step(
                  &lifecycle, 150U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle partial-drain fixture drains one slot");
    memcpy(original, &lifecycle, sizeof(lifecycle));
    CHECK(https_service_lifecycle_finish_drain(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0 &&
              state.zeroize_calls == 0U &&
              state.http_calls == 0U &&
              state.packet_calls == 0U,
          "HTTPS lifecycle finish before empty is rejected without cleanup");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle partial finish performs no allocation");
}

static void test_restart(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    struct https_service_lifecycle_snapshot snapshot;
    unsigned char original[sizeof(lifecycle)];
    uint32_t index;

    init_callback_state(&state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 5U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle restart fixture starts");
    CHECK(https_service_lifecycle_restart(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE,
          "HTTPS lifecycle restart from LISTENING is rejected");
    CHECK(https_service_lifecycle_stop_admission(
              &lifecycle, 100U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle restart fixture stops admission");
    for (index = 0U; index < 4U; ++index) {
        state.drain_results[index] = true;
    }
    CHECK(https_service_lifecycle_step(
              &lifecycle, 150U) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_finish_drain(
                  &lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle restart fixture stops cleanly");
    state.restart_result = false;
    memcpy(original, &lifecycle, sizeof(lifecycle));
    CHECK(https_service_lifecycle_restart(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED &&
              memcmp(&lifecycle, original, sizeof(lifecycle)) == 0,
          "HTTPS lifecycle restart callback failure leaves state unchanged");
    state.restart_result = true;
    state.restart_calls = 0U;
    CHECK(https_service_lifecycle_restart(
              &lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK &&
              lifecycle.phase == HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED &&
              lifecycle.generation == 6U &&
              lifecycle.active_slots == 0x0FU &&
              lifecycle.drain_deadline_ms == 0U &&
              lifecycle.drained_count == 0U &&
              lifecycle.aborted_count == 0U &&
              lifecycle.zeroized_count == 0U &&
              lifecycle.http_released_count == 0U &&
              lifecycle.packet_released_count == 0U &&
              lifecycle.stopped_count == 1U &&
              lifecycle.restarted_count == 1U &&
              state.restart_calls == 1U,
          "HTTPS lifecycle restart publishes exact initial counters");
    CHECK(https_service_lifecycle_snapshot(
              &lifecycle, &snapshot) == HTTPS_SERVICE_LIFECYCLE_OK &&
              snapshot.generation == 6U &&
              snapshot.active_connection_count == 4U &&
              snapshot.drained_count == 0U &&
              snapshot.aborted_count == 0U &&
              snapshot.zeroized_count == 0U &&
              snapshot.http_released_count == 0U &&
              snapshot.packet_released_count == 0U &&
              snapshot.stopped_count == 1U &&
              snapshot.restarted_count == 1U,
          "HTTPS lifecycle snapshot proves exact initial counters");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle restart performs no allocation");
}

static void test_repeated_cycles(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_config config = make_config();
    struct test_callback_state state;
    struct https_service_lifecycle_ops ops = make_ops(&state);
    uint32_t cycle;

    init_callback_state(&state);
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(https_service_lifecycle_init(
              &lifecycle, &config, &ops) == HTTPS_SERVICE_LIFECYCLE_OK &&
              https_service_lifecycle_start(
                  &lifecycle, 10U) == HTTPS_SERVICE_LIFECYCLE_OK,
          "HTTPS lifecycle repeated-cycle fixture initializes");
    for (cycle = 0U; cycle < 3U; ++cycle) {
        uint32_t slot;

        CHECK(https_service_lifecycle_stop_admission(
                  &lifecycle, 1000U + cycle) ==
                  HTTPS_SERVICE_LIFECYCLE_OK,
              "HTTPS lifecycle repeated stop succeeds");
        for (slot = 0U; slot < 4U; ++slot) {
            state.drain_results[slot] = true;
            state.abort_results[slot] = true;
        }
        CHECK(https_service_lifecycle_step(
                  &lifecycle, 1100U + cycle) ==
                  HTTPS_SERVICE_LIFECYCLE_OK &&
                  https_service_lifecycle_finish_drain(
                      &lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK,
              "HTTPS lifecycle repeated drain and cleanup succeeds");
        CHECK(https_service_lifecycle_restart(
                  &lifecycle) == HTTPS_SERVICE_LIFECYCLE_OK &&
                  lifecycle.generation == 11U + cycle &&
                  lifecycle.restarted_count == cycle + 1U &&
                  lifecycle.active_slots == 0x0FU,
              "HTTPS lifecycle repeated restart advances exactly");
    }
    CHECK(lifecycle.stopped_count == 3U &&
              lifecycle.restarted_count == 3U &&
              lifecycle.drained_count == 0U &&
              lifecycle.aborted_count == 0U &&
              lifecycle.zeroized_count == 0U,
          "HTTPS lifecycle repeated cycles restore exact initial counters");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS lifecycle repeated cycles perform no allocation");
}

static void test_accessors_fail_closed(void)
{
    struct https_service_lifecycle lifecycle;
    struct https_service_lifecycle_snapshot snapshot;

    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(!https_service_lifecycle_published(NULL),
          "HTTPS lifecycle null published accessor fails closed");
    CHECK(!https_service_lifecycle_published(&lifecycle),
          "HTTPS lifecycle unpublished accessor fails closed");
    CHECK(https_service_lifecycle_phase(NULL) ==
              HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE,
          "HTTPS lifecycle null phase accessor returns IDLE");
    CHECK(https_service_lifecycle_generation(NULL) == 0U,
          "HTTPS lifecycle null generation accessor returns zero");
    CHECK(https_service_lifecycle_active_connection_count(NULL) == 0U,
          "HTTPS lifecycle null active-count accessor returns zero");
    CHECK(https_service_lifecycle_snapshot(
              NULL, &snapshot) == HTTPS_SERVICE_LIFECYCLE_NULL,
          "HTTPS lifecycle snapshot rejects null lifecycle");
    CHECK(https_service_lifecycle_snapshot(
              &lifecycle, NULL) == HTTPS_SERVICE_LIFECYCLE_NULL,
          "HTTPS lifecycle snapshot rejects null output");
    CHECK(https_service_lifecycle_snapshot(
              &lifecycle, &snapshot) == HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED,
          "HTTPS lifecycle unpublished snapshot fails closed");
}

int main(void)
{
    test_strings();
    test_init();
    test_start();
    test_stop_admission_flow();
    test_drain_and_abort();
    test_finish_failures_and_cleanup();
    test_finish_before_empty();
    test_restart();
    test_repeated_cycles();
    test_accessors_fail_closed();

    if (failures != 0) {
        fprintf(stderr,
                "HTTPS graceful stop and restart host tests: %d FAIL\n",
                failures);
        return 1;
    }
    printf("HTTPS graceful stop and restart host test: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
