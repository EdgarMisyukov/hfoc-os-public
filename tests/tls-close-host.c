#include "hfoc_tls_close.h"

#include "hfoc_mbedtls_alloc.h"
#include "hfoc_mbedtls_bio.h"
#include "hfoc_mbedtls_manifest.h"
#include "hfoc_tls_handshake.h"

#include "mbedtls/ssl.h"

#include <stdbool.h>
#include <stddef.h>
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

enum close_mode {
    CLOSE_MODE_NOTIFY = 0,
    CLOSE_MODE_WANT = 1,
    CLOSE_MODE_WANT_WRITE = 2,
    CLOSE_MODE_PROGRESS = 3,
    CLOSE_MODE_TRANSPORT_CLOSED = 4,
    CLOSE_MODE_TRANSPORT_REJECTED = 5,
    CLOSE_MODE_FAIL = 6,
    CLOSE_MODE_INVALID = 7,
};

struct close_context {
    enum close_mode mode;
    uint32_t calls;
    uint32_t required_calls;
};

struct zeroize_state {
    uint32_t calls;
};

enum handshake_mode {
    HANDSHAKE_MODE_COMPLETE = 0,
    HANDSHAKE_MODE_WANT = 1,
};

struct handshake_context {
    enum handshake_mode mode;
    uint32_t calls;
};

static enum hfoc_tls_close_step_result
fake_close_step(void *opaque)
{
    struct close_context *context = opaque;

    if (context == NULL) {
        return HFOC_TLS_CLOSE_STEP_FAILED;
    }
    ++context->calls;

    switch (context->mode) {
    case CLOSE_MODE_NOTIFY:
        return HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT;
    case CLOSE_MODE_WANT:
        return HFOC_TLS_CLOSE_STEP_WANT_READ;
    case CLOSE_MODE_WANT_WRITE:
        return HFOC_TLS_CLOSE_STEP_WANT_WRITE;
    case CLOSE_MODE_TRANSPORT_CLOSED:
        return HFOC_TLS_CLOSE_STEP_TRANSPORT_CLOSED;
    case CLOSE_MODE_TRANSPORT_REJECTED:
        return HFOC_TLS_CLOSE_STEP_TRANSPORT_REJECTED;
    case CLOSE_MODE_FAIL:
        return HFOC_TLS_CLOSE_STEP_FAILED;
    case CLOSE_MODE_INVALID:
        return (enum hfoc_tls_close_step_result)99U;
    case CLOSE_MODE_PROGRESS:
        break;
    default:
        return HFOC_TLS_CLOSE_STEP_FAILED;
    }

    if (context->calls < context->required_calls) {
        return HFOC_TLS_CLOSE_STEP_PROGRESS;
    }
    return HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT;
}

static void fake_zeroize(void *opaque)
{
    struct zeroize_state *state = opaque;

    if (state != NULL) {
        ++state->calls;
    }
}

static enum hfoc_tls_handshake_step_result
fake_handshake_step(
    void *opaque,
    const uint8_t *input,
    size_t input_length,
    size_t *bytes_consumed)
{
    struct handshake_context *context = opaque;

    if (context == NULL || bytes_consumed == NULL) {
        return HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
    }
    *bytes_consumed = 0U;
    ++context->calls;
    if (input == NULL || input_length < 5U ||
        input[0] != 0x16U ||
        input[1] != 0x03U ||
        input[2] != 0x01U) {
        return HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
    }
    *bytes_consumed = input_length;
    if (context->mode == HANDSHAKE_MODE_WANT) {
        return HFOC_TLS_HANDSHAKE_STEP_WANT_READ;
    }
    return HFOC_TLS_HANDSHAKE_STEP_COMPLETE;
}

static enum hfoc_mbedtls_bio_transport_status
retained_send(
    void *opaque,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    (void)opaque;
    (void)bytes;
    (void)length;
    *bytes_accepted = 0U;
    return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
}

static enum hfoc_mbedtls_bio_transport_status
retained_recv(
    void *opaque,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    (void)opaque;
    (void)bytes;
    (void)capacity;
    *bytes_received = 0U;
    return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
}

static struct hfoc_mbedtls_manifest make_manifest(void)
{
    struct hfoc_mbedtls_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.published = true;
    manifest.version = HFOC_MBEDTLS_MANIFEST_VERSION;
    manifest.purpose_id = HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE;
    manifest.algorithm_id =
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256;
    manifest.hostname_length = HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH;
    memcpy(
        manifest.hostname,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH);
    return manifest;
}

static struct hfoc_mbedtls_bio_policy make_bio_policy(void)
{
    struct hfoc_mbedtls_bio_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_MBEDTLS_BIO_VERSION;
    policy.max_send_bytes = HFOC_MBEDTLS_BIO_MAX_LEN_MAX;
    policy.max_recv_bytes = HFOC_MBEDTLS_BIO_MAX_LEN_MAX;
    return policy;
}

static struct hfoc_mbedtls_bio_transport make_bio_transport(void)
{
    struct hfoc_mbedtls_bio_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.send = retained_send;
    transport.recv = retained_recv;
    return transport;
}

static struct hfoc_tls_handshake_token make_handshake_token(
    uint32_t index,
    uint32_t generation)
{
    struct hfoc_tls_handshake_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static struct hfoc_mbedtls_bio_token make_bio_token(
    uint32_t index,
    uint32_t generation)
{
    struct hfoc_mbedtls_bio_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static struct hfoc_tls_close_token make_close_token(
    uint32_t index,
    uint32_t generation)
{
    struct hfoc_tls_close_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static void reset_allocator(void)
{
    hfoc_mbedtls_alloc_shutdown();
    CHECK(hfoc_mbedtls_alloc_init(),
          "fixture initializes mbedTLS allocator");
}

static void init_bio(struct hfoc_mbedtls_bio *bio)
{
    memset(bio, 0, sizeof(*bio));
    CHECK(hfoc_mbedtls_bio_init(bio) == HFOC_MBEDTLS_BIO_OK,
          "fixture initializes BIO gate");
}

static void init_handshake(struct hfoc_tls_handshake *handshake)
{
    memset(handshake, 0, sizeof(*handshake));
    CHECK(hfoc_tls_handshake_init(handshake) ==
              HFOC_TLS_HANDSHAKE_OK,
          "fixture initializes handshake gate");
}

static void init_close(struct hfoc_tls_close *close)
{
    memset(close, 0, sizeof(*close));
    CHECK(hfoc_tls_close_init(close) == HFOC_TLS_CLOSE_OK,
          "fixture initializes close gate");
}

static bool allocator_initial_counts(
    const struct hfoc_mbedtls_alloc_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->total_used_bytes == 0U &&
           snapshot->total_free_bytes ==
               HFOC_MBEDTLS_ALLOC_TOTAL_BYTES &&
           snapshot->total_high_water_bytes == 0U;
}

static bool connection_inactive(
    const struct hfoc_mbedtls_alloc_snapshot *snapshot,
    uint32_t index)
{
    return snapshot != NULL &&
           index < HFOC_MBEDTLS_CONNECTION_COUNT &&
           !snapshot->connections[index].active &&
           snapshot->connections[index].id == 0U;
}

static struct hfoc_tls_handshake_config make_handshake_config(
    uint32_t slot_index,
    uint32_t generation,
    struct hfoc_mbedtls_bio *bio,
    struct handshake_context *step_context)
{
    static struct hfoc_mbedtls_bio_policy bio_policy;
    static struct hfoc_mbedtls_bio_transport bio_transport;
    static struct hfoc_mbedtls_bio_token bio_token;
    static struct hfoc_mbedtls_manifest manifest;
    struct hfoc_tls_handshake_config config;

    bio_policy = make_bio_policy();
    bio_transport = make_bio_transport();
    bio_token = make_bio_token(slot_index, generation);
    manifest = make_manifest();
    memset(step_context, 0, sizeof(*step_context));
    step_context->mode = HANDSHAKE_MODE_COMPLETE;

    memset(&config, 0, sizeof(config));
    config.version = HFOC_TLS_HANDSHAKE_VERSION;
    config.deadline_ticks = 100U;
    config.max_steps = 10U;
    config.drbg_generation = 1U;
    config.drbg_source_id = 7U;
    config.manifest = &manifest;
    config.bio = bio;
    config.bio_token = &bio_token;
    config.bio_policy = &bio_policy;
    config.bio_transport = &bio_transport;
    config.step = fake_handshake_step;
    config.step_context = step_context;
    return config;
}

static struct hfoc_tls_close_config make_close_config(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *handshake_token,
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *bio_token,
    struct close_context *step_context,
    struct zeroize_state *zeroize,
    uint32_t deadline_ticks,
    uint32_t max_steps,
    enum close_mode mode,
    uint32_t required_calls)
{
    struct hfoc_tls_close_config config;

    memset(step_context, 0, sizeof(*step_context));
    step_context->mode = mode;
    step_context->required_calls = required_calls;
    memset(zeroize, 0, sizeof(*zeroize));

    memset(&config, 0, sizeof(config));
    config.version = HFOC_TLS_CLOSE_VERSION;
    config.deadline_ticks = deadline_ticks;
    config.max_steps = max_steps;
    config.handshake = handshake;
    config.handshake_token = handshake_token;
    config.bio = bio;
    config.bio_token = bio_token;
    config.step = fake_close_step;
    config.step_context = step_context;
    config.zeroize = fake_zeroize;
    config.zeroize_context = zeroize;
    return config;
}

static void setup_successful_handshake(
    struct hfoc_mbedtls_bio *bio,
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *handshake_token)
{
    struct handshake_context handshake_context;
    struct hfoc_tls_handshake_config handshake_config;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    handshake_config = make_handshake_config(
        handshake_token->index,
        handshake_token->generation,
        bio,
        &handshake_context);
    CHECK(hfoc_tls_handshake_reserve(
              handshake, handshake_token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_configure(
                  handshake,
                  handshake_token,
                  &handshake_config,
                  1000U) ==
                  HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_progress(
                  handshake,
                  handshake_token,
                  1000U,
                  client_hello,
                  sizeof(client_hello),
                  &consumed) ==
                  HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state(
                  handshake, handshake_token) ==
                  HFOC_TLS_HANDSHAKE_STATE_SUCCESS,
          "fixture publishes successful handshake");
}

static void test_strings_and_unpublished(void)
{
    struct hfoc_tls_close close;
    enum hfoc_tls_close_state state;
    enum hfoc_tls_close_outcome outcome;
    enum hfoc_tls_close_status status;
    enum hfoc_tls_close_step_result step_result;
    enum hfoc_tls_close_error error;

    for (state = HFOC_TLS_CLOSE_STATE_FREE;
         state <= HFOC_TLS_CLOSE_STATE_ABORTED;
         ++state) {
        CHECK(hfoc_tls_close_state_string(state) != NULL,
              "close state string is defined for every state");
    }
    CHECK(hfoc_tls_close_state_string(
              (enum hfoc_tls_close_state)99U) == NULL,
          "close undefined state string is rejected");

    for (outcome = HFOC_TLS_CLOSE_OUTCOME_NONE;
         outcome <= HFOC_TLS_CLOSE_OUTCOME_INVALID_PROGRESS;
         ++outcome) {
        CHECK(hfoc_tls_close_outcome_string(outcome) != NULL,
              "close outcome string is defined for every outcome");
    }
    CHECK(hfoc_tls_close_outcome_string(
              (enum hfoc_tls_close_outcome)99U) == NULL,
          "close undefined outcome string is rejected");

    for (status = HFOC_TLS_CLOSE_OK;
         status <= HFOC_TLS_CLOSE_ABORTED;
         ++status) {
        CHECK(hfoc_tls_close_status_string(status) != NULL,
              "close status string is defined for every status");
    }
    CHECK(hfoc_tls_close_status_string(
              (enum hfoc_tls_close_status)999U) == NULL,
          "close undefined status string is rejected");

    for (step_result = HFOC_TLS_CLOSE_STEP_WANT_READ;
         step_result <= HFOC_TLS_CLOSE_STEP_FAILED;
         ++step_result) {
        CHECK(hfoc_tls_close_step_result_string(step_result) != NULL,
              "close step result string is defined for every result");
    }
    CHECK(hfoc_tls_close_step_result_string(
              (enum hfoc_tls_close_step_result)99U) == NULL,
          "close undefined step result string is rejected");

    for (error = HFOC_TLS_CLOSE_ERROR_NONE;
         error <= HFOC_TLS_CLOSE_ERROR_UNKNOWN;
         ++error) {
        CHECK(hfoc_tls_close_error_string(error) != NULL,
              "close error string is defined for every category");
    }
    CHECK(hfoc_tls_close_error_string(
              (enum hfoc_tls_close_error)999U) == NULL,
          "close undefined error string is rejected");

    memset(&close, 0, sizeof(close));
    CHECK(!hfoc_tls_close_published(&close),
          "unpublished close gate is not published");
    CHECK(hfoc_tls_close_active_count(&close) == 0U &&
              hfoc_tls_close_close_notify_count(&close) == 0U &&
              hfoc_tls_close_abort_count(&close) == 0U &&
              hfoc_tls_close_failure_count(&close) == 0U,
          "unpublished close accessors are conservative");
}

static void test_init_validation(void)
{
    struct hfoc_tls_close close;
    struct hfoc_tls_close_token token =
        make_close_token(0U, 1U);

    CHECK(hfoc_tls_close_init(NULL) == HFOC_TLS_CLOSE_NULL,
          "close init rejects null object");

    memset(&close, 0, sizeof(close));
    CHECK(hfoc_tls_close_init(&close) == HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_published(&close) &&
              close.version == HFOC_TLS_CLOSE_VERSION &&
              close.active_count == 0U,
          "close init publishes fixed empty gate");
    CHECK(hfoc_tls_close_init(&close) == HFOC_TLS_CLOSE_BAD_STATE,
          "close init rejects duplicate publication");

    CHECK(hfoc_tls_close_reserve(NULL, &token) ==
              HFOC_TLS_CLOSE_NULL,
          "close reserve rejects null gate");
    CHECK(hfoc_tls_close_reserve(&close, NULL) ==
              HFOC_TLS_CLOSE_NULL,
          "close reserve rejects null token");
}

static void test_close_notify_release_idempotent(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        10U,
        CLOSE_MODE_NOTIFY,
        0U);

    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_active_count(&close) == 1U &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_RESERVED,
          "close reserve publishes one slot");
    CHECK(hfoc_tls_close_configure(
              &close, &close_token, &config, 1000U) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_RESERVED,
          "close configure binds handshake and close seam");

    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_CLOSE_NOTIFY_SENT &&
              hfoc_tls_close_close_notify_count(&close) == 1U,
          "close-notify step publishes close-notify");

    CHECK(hfoc_tls_close_release(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              zeroize.calls == 1U &&
              hfoc_tls_close_active_count(&close) == 0U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "close release zeroizes and releases handshake exactly once");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(connection_inactive(&snapshot, 0U) &&
              allocator_initial_counts(&snapshot),
          "close release restores allocator counts");

    CHECK(hfoc_tls_close_release(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              zeroize.calls == 1U,
          "second close release is idempotent without second zeroize");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_want_and_abort(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        10U,
        CLOSE_MODE_WANT,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "want fixture reserves and configures close slot");

    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_CLOSING &&
              hfoc_tls_close_close_notify_count(&close) == 0U,
          "WANT_READ keeps close slot active without close-notify");

    CHECK(hfoc_tls_close_abort(&close, &close_token) ==
              HFOC_TLS_CLOSE_ABORTED &&
              zeroize.calls == 1U &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_ABORTED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_ABORTED &&
              hfoc_tls_close_abort_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "abort zeroizes and releases handshake exactly once");
    CHECK(hfoc_tls_close_release(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              zeroize.calls == 1U,
          "release after abort is idempotent");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_terminal_failure_cleanup(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;
    enum close_mode mode;
    enum hfoc_tls_close_status expected_status;
    enum hfoc_tls_close_outcome expected_outcome;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);

    mode = CLOSE_MODE_TRANSPORT_CLOSED;
    expected_status = HFOC_TLS_CLOSE_TRANSPORT_CLOSED;
    expected_outcome = HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_CLOSED;
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        10U,
        mode,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "transport-closed fixture configures close slot");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              expected_status &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_FAILED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  expected_outcome &&
              zeroize.calls == 1U &&
              hfoc_tls_close_failure_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "transport-closed failure cleans the slot");
    CHECK(hfoc_tls_close_release(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              zeroize.calls == 1U,
          "transport-closed failure release is idempotent");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(connection_inactive(&snapshot, 0U) &&
              allocator_initial_counts(&snapshot),
          "transport-closed failure restores allocator counts");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_step_failed_cleanup(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        10U,
        CLOSE_MODE_FAIL,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "close-step-failure fixture configures close slot");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              HFOC_TLS_CLOSE_CLOSE_FAILED &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_FAILED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_ABORTED &&
              zeroize.calls == 1U &&
              hfoc_tls_close_failure_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U,
          "close-step failure aborts and cleans the slot");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_invalid_progress_cleanup(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        10U,
        CLOSE_MODE_INVALID,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "invalid-progress fixture configures close slot");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              HFOC_TLS_CLOSE_INVALID_PROGRESS &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_FAILED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_INVALID_PROGRESS &&
              zeroize.calls == 1U &&
              hfoc_tls_close_failure_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U,
          "invalid close progress fails closed and cleans the slot");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_deadline_and_limit_cleanup(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        2U,
        CLOSE_MODE_WANT,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "deadline/limit fixture configures close slot");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1099U) ==
              HFOC_TLS_CLOSE_OK,
          "close progress before deadline is accepted");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1100U) ==
              HFOC_TLS_CLOSE_DEADLINE_EXPIRED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_DEADLINE_EXPIRED &&
              zeroize.calls == 1U &&
              hfoc_tls_close_failure_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U,
          "deadline expiry aborts and cleans the slot");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_step_limit_cleanup(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_token =
        make_handshake_token(0U, 1U);
    struct hfoc_mbedtls_bio_token bio_token =
        make_bio_token(0U, 1U);
    struct hfoc_tls_close_token close_token =
        make_close_token(0U, 1U);
    struct close_context close_context;
    struct zeroize_state zeroize;
    struct hfoc_tls_close_config config;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    setup_successful_handshake(
        &bio, &handshake, &handshake_token);
    init_close(&close);
    config = make_close_config(
        &handshake,
        &handshake_token,
        &bio,
        &bio_token,
        &close_context,
        &zeroize,
        100U,
        1U,
        CLOSE_MODE_WANT,
        0U);
    CHECK(hfoc_tls_close_reserve(&close, &close_token) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_configure(
                  &close, &close_token, &config, 1000U) ==
                  HFOC_TLS_CLOSE_OK,
          "step-limit fixture configures close slot");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1000U) ==
              HFOC_TLS_CLOSE_OK,
          "first close step consumes the exact budget");
    CHECK(hfoc_tls_close_progress(
              &close, &close_token, 1001U) ==
              HFOC_TLS_CLOSE_STEP_LIMIT &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_STEP_LIMIT &&
              zeroize.calls == 1U &&
              hfoc_tls_close_failure_count(&close) == 1U &&
              hfoc_tls_close_active_count(&close) == 0U,
          "step-limit expiry aborts and cleans the slot");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_slot_isolation(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_close close;
    struct hfoc_tls_handshake_token handshake_tokens[2];
    struct hfoc_mbedtls_bio_token bio_tokens[2];
    struct hfoc_tls_close_token close_tokens[2];
    struct close_context close_contexts[2];
    struct zeroize_state zeroizes[2];
    struct hfoc_tls_close_config configs[2];
    uint32_t index;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    for (index = 0U; index < 2U; ++index) {
        handshake_tokens[index] =
            make_handshake_token(index, index + 1U);
        bio_tokens[index] =
            make_bio_token(index, index + 1U);
        setup_successful_handshake(
            &bio,
            &handshake,
            &handshake_tokens[index]);
    }
    init_close(&close);
    for (index = 0U; index < 2U; ++index) {
        close_tokens[index] =
            make_close_token(index, index + 1U);
        configs[index] = make_close_config(
            &handshake,
            &handshake_tokens[index],
            &bio,
            &bio_tokens[index],
            &close_contexts[index],
            &zeroizes[index],
            100U,
            10U,
            index == 0U
                ? CLOSE_MODE_TRANSPORT_REJECTED
                : CLOSE_MODE_NOTIFY,
            0U);
        CHECK(hfoc_tls_close_reserve(
                  &close, &close_tokens[index]) ==
                  HFOC_TLS_CLOSE_OK &&
                  hfoc_tls_close_configure(
                      &close,
                      &close_tokens[index],
                      &configs[index],
                      1000U) ==
                      HFOC_TLS_CLOSE_OK,
              "isolation fixture configures two close slots");
    }

    CHECK(hfoc_tls_close_progress(
              &close, &close_tokens[0], 1000U) ==
              HFOC_TLS_CLOSE_TRANSPORT_REJECTED &&
              zeroizes[0].calls == 1U &&
              zeroizes[1].calls == 0U &&
              hfoc_tls_close_state_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_STATE_FAILED &&
              hfoc_tls_close_outcome_at(&close, 0U) ==
                  HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_REJECTED &&
              hfoc_tls_close_active_count(&close) == 1U &&
              hfoc_tls_handshake_active_count(&handshake) == 1U &&
              hfoc_mbedtls_bio_active_count(&bio) == 1U,
          "rejected close affects exactly one slot");

    CHECK(hfoc_tls_close_progress(
              &close, &close_tokens[1], 1000U) ==
              HFOC_TLS_CLOSE_OK &&
              hfoc_tls_close_state_at(&close, 1U) ==
                  HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT,
          "unrelated close slot still publishes close-notify");
    CHECK(hfoc_tls_close_release(
              &close, &close_tokens[1]) ==
              HFOC_TLS_CLOSE_OK &&
              zeroizes[1].calls == 1U &&
              hfoc_tls_close_active_count(&close) == 0U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "isolation fixture releases surviving close slot");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_error_normalization(void)
{
    CHECK(hfoc_tls_close_error_normalize(0) ==
              HFOC_TLS_CLOSE_ERROR_SUCCESS,
          "zero maps to success category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_WANT_READ) ==
              HFOC_TLS_CLOSE_ERROR_WANT_READ,
          "WANT_READ maps to stable category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_WANT_WRITE) ==
              HFOC_TLS_CLOSE_ERROR_WANT_WRITE,
          "WANT_WRITE maps to stable category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_CONN_EOF) ==
              HFOC_TLS_CLOSE_ERROR_TRANSPORT_CLOSED,
          "connection EOF maps to transport-closed category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) ==
              HFOC_TLS_CLOSE_ERROR_PEER_CLOSE_NOTIFY,
          "peer close-notify maps to stable category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA) ==
              HFOC_TLS_CLOSE_ERROR_BAD_INPUT,
          "bad input maps to stable category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) ==
              HFOC_TLS_CLOSE_ERROR_PROTOCOL,
          "fatal alert maps to protocol category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_TIMEOUT) ==
              HFOC_TLS_CLOSE_ERROR_TIMEOUT,
          "timeout maps to stable category");
    CHECK(hfoc_tls_close_error_normalize(
              MBEDTLS_ERR_SSL_ALLOC_FAILED) ==
              HFOC_TLS_CLOSE_ERROR_INTERNAL,
          "allocation failure maps to internal category");
    CHECK(hfoc_tls_close_error_normalize(-123456) ==
              HFOC_TLS_CLOSE_ERROR_UNKNOWN,
          "unknown result maps to unknown category");
}

int main(void)
{
    test_strings_and_unpublished();
    test_init_validation();
    test_close_notify_release_idempotent();
    test_want_and_abort();
    test_terminal_failure_cleanup();
    test_step_failed_cleanup();
    test_invalid_progress_cleanup();
    test_deadline_and_limit_cleanup();
    test_step_limit_cleanup();
    test_slot_isolation();
    test_error_normalization();

    if (failures == 0) {
        printf("TLS close and error normalization host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr,
            "TLS close and error normalization host tests: %d FAILURES\n",
            failures);
    return 1;
}
