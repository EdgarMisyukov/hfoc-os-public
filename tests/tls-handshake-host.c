#include "hfoc_tls_handshake.h"

#include "hfoc_mbedtls_alloc.h"
#include "hfoc_mbedtls_bio.h"
#include "hfoc_mbedtls_manifest.h"

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

enum step_mode {
    STEP_MODE_COMPLETE = 0,
    STEP_MODE_WANT = 1,
    STEP_MODE_WANT_WRITE = 2,
    STEP_MODE_PROGRESS_THEN_COMPLETE = 3,
    STEP_MODE_FAIL = 4,
    STEP_MODE_INVALID_CONSUME = 5,
};

struct step_context {
    enum step_mode mode;
    uint32_t calls;
    uint32_t required_calls;
};

static enum hfoc_tls_handshake_step_result
fake_step(
    void *opaque,
    const uint8_t *input,
    size_t input_length,
    size_t *bytes_consumed)
{
    struct step_context *context = opaque;

    if (context == NULL || bytes_consumed == NULL) {
        return HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
    }
    *bytes_consumed = 0U;
    ++context->calls;

    if (context->mode == STEP_MODE_FAIL) {
        return HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
    }
    if (context->mode == STEP_MODE_INVALID_CONSUME) {
        *bytes_consumed = input_length + 1U;
        return HFOC_TLS_HANDSHAKE_STEP_PROGRESS;
    }
    if (input == NULL || input_length == 0U) {
        return HFOC_TLS_HANDSHAKE_STEP_WANT_READ;
    }

    /*
     * Host proof uses the TLS record header as a bounded ClientHello
     * fingerprint. The real mbedTLS step adapter is owned by item 270.
     */
    if (input_length < 5U ||
        input[0] != 0x16U ||
        input[1] != 0x03U ||
        input[2] != 0x01U) {
        return HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
    }
    *bytes_consumed = input_length;

    if (context->mode == STEP_MODE_WANT_WRITE) {
        return HFOC_TLS_HANDSHAKE_STEP_WANT_WRITE;
    }
    if (context->mode == STEP_MODE_COMPLETE) {
        return HFOC_TLS_HANDSHAKE_STEP_COMPLETE;
    }
    if (context->mode == STEP_MODE_WANT) {
        return HFOC_TLS_HANDSHAKE_STEP_WANT_READ;
    }
    if (context->calls < context->required_calls) {
        return HFOC_TLS_HANDSHAKE_STEP_PROGRESS;
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

static struct hfoc_tls_handshake_token make_token(
    uint32_t index,
    uint32_t generation)
{
    struct hfoc_tls_handshake_token token;

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

static struct hfoc_mbedtls_bio_token make_bio_token_for_handshake(
    uint32_t index,
    uint32_t generation);

static struct hfoc_tls_handshake_config make_config(
    uint32_t slot_index,
    uint32_t generation,
    struct hfoc_mbedtls_bio *bio,
    struct step_context *step_context,
    uint32_t deadline_ticks,
    uint32_t max_steps,
    enum step_mode mode,
    uint32_t required_calls)
{
    static struct hfoc_mbedtls_bio_policy bio_policy;
    static struct hfoc_mbedtls_bio_transport bio_transport;
    static struct hfoc_mbedtls_bio_token bio_token;
    static struct hfoc_mbedtls_manifest manifest;
    struct hfoc_tls_handshake_config config;

    bio_policy = make_bio_policy();
    bio_transport = make_bio_transport();
    bio_token = make_bio_token_for_handshake(slot_index, generation);
    manifest = make_manifest();
    memset(step_context, 0, sizeof(*step_context));
    step_context->mode = mode;
    step_context->required_calls = required_calls;

    memset(&config, 0, sizeof(config));
    config.version = HFOC_TLS_HANDSHAKE_VERSION;
    config.deadline_ticks = deadline_ticks;
    config.max_steps = max_steps;
    config.drbg_generation = 1U;
    config.drbg_source_id = 7U;
    config.manifest = &manifest;
    config.bio = bio;
    config.bio_token = &bio_token;
    config.bio_policy = &bio_policy;
    config.bio_transport = &bio_transport;
    config.step = fake_step;
    config.step_context = step_context;
    return config;
}

static struct hfoc_mbedtls_bio_token make_bio_token_for_handshake(
    uint32_t index,
    uint32_t generation)
{
    struct hfoc_mbedtls_bio_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static void test_strings_and_unpublished(void)
{
    struct hfoc_tls_handshake handshake;
    enum hfoc_tls_handshake_state state;
    enum hfoc_tls_handshake_fault fault;
    enum hfoc_tls_handshake_status status;
    enum hfoc_tls_handshake_step_result step_result;

    for (state = HFOC_TLS_HANDSHAKE_STATE_FREE;
         state <= HFOC_TLS_HANDSHAKE_STATE_FAILED;
         ++state) {
        CHECK(hfoc_tls_handshake_state_string(state) != NULL,
              "handshake state string is defined for every state");
    }
    CHECK(hfoc_tls_handshake_state_string(
              (enum hfoc_tls_handshake_state)99U) == NULL,
          "handshake undefined state string is rejected");

    for (fault = HFOC_TLS_HANDSHAKE_FAULT_NONE;
         fault <= HFOC_TLS_HANDSHAKE_FAULT_STEP_LIMIT;
         ++fault) {
        CHECK(hfoc_tls_handshake_fault_string(fault) != NULL,
              "handshake fault string is defined for every fault");
    }
    CHECK(hfoc_tls_handshake_fault_string(
              (enum hfoc_tls_handshake_fault)99U) == NULL,
          "handshake undefined fault string is rejected");

    for (status = HFOC_TLS_HANDSHAKE_OK;
         status <= HFOC_TLS_HANDSHAKE_STEP_LIMIT;
         ++status) {
        CHECK(hfoc_tls_handshake_status_string(status) != NULL,
              "handshake status string is defined for every status");
    }
    CHECK(hfoc_tls_handshake_status_string(
              (enum hfoc_tls_handshake_status)999U) == NULL,
          "handshake undefined status string is rejected");

    for (step_result = HFOC_TLS_HANDSHAKE_STEP_WANT_READ;
         step_result <= HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED;
         ++step_result) {
        CHECK(hfoc_tls_handshake_step_result_string(
                  step_result) != NULL,
              "handshake step result string is defined for every result");
    }
    CHECK(hfoc_tls_handshake_step_result_string(
              (enum hfoc_tls_handshake_step_result)99U) == NULL,
          "handshake undefined step result string is rejected");

    memset(&handshake, 0, sizeof(handshake));
    CHECK(!hfoc_tls_handshake_published(&handshake),
          "unpublished handshake gate is not published");
    CHECK(hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_tls_handshake_success_count(&handshake) == 0U &&
              hfoc_tls_handshake_failure_count(&handshake) == 0U,
          "unpublished handshake accessors are conservative");
}

static void test_init_validation(void)
{
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);

    CHECK(hfoc_tls_handshake_init(NULL) ==
              HFOC_TLS_HANDSHAKE_NULL,
          "handshake init rejects null object");

    memset(&handshake, 0, sizeof(handshake));
    CHECK(hfoc_tls_handshake_init(&handshake) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_published(&handshake) &&
              handshake.version == HFOC_TLS_HANDSHAKE_VERSION &&
              handshake.active_count == 0U,
          "handshake init publishes fixed empty gate");
    CHECK(hfoc_tls_handshake_init(&handshake) ==
              HFOC_TLS_HANDSHAKE_BAD_STATE,
          "handshake init rejects duplicate publication");

    CHECK(hfoc_tls_handshake_reserve(NULL, &token) ==
              HFOC_TLS_HANDSHAKE_NULL,
          "handshake reserve rejects null gate");
    CHECK(hfoc_tls_handshake_reserve(&handshake, NULL) ==
              HFOC_TLS_HANDSHAKE_NULL,
          "handshake reserve rejects null token");
}

static void test_reserve_configure_success_release(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);
    struct step_context step_context;
    struct hfoc_tls_handshake_config config;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_COMPLETE, 0U);

    CHECK(hfoc_tls_handshake_reserve(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_active_count(&handshake) == 1U &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_RESERVED,
          "reserve publishes one generation-tagged slot");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot.connections[0].active &&
              snapshot.connections[0].id == 1U,
          "reserve opens allocator connection scope");

    CHECK(hfoc_tls_handshake_configure(
              &handshake, &token, &config, 1000U) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_CONFIGURED &&
              hfoc_mbedtls_bio_active_count(&bio) == 1U,
          "configure attaches BIO and publishes configured state");

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              consumed == sizeof(client_hello) &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_SUCCESS &&
              hfoc_tls_handshake_success_published(&handshake, &token) &&
              hfoc_tls_handshake_success_count(&handshake) == 1U,
          "complete step publishes handshake success");

    CHECK(hfoc_tls_handshake_release(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "successful release restores BIO and gate counts");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(connection_inactive(&snapshot, 0U) &&
              allocator_initial_counts(&snapshot),
          "successful release restores allocator counts");
    CHECK(hfoc_tls_handshake_release(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK,
          "second release is idempotent");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_progress_wait_and_limit(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);
    struct step_context step_context;
    struct hfoc_tls_handshake_config config;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 2U,
        STEP_MODE_WANT, 0U);
    CHECK(hfoc_tls_handshake_reserve(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_configure(
                  &handshake, &token, &config, 1000U) ==
                  HFOC_TLS_HANDSHAKE_OK,
          "wait fixture reserves and configures one slot");

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING,
          "WANT_READ keeps handshake active");
    consumed = 0U;
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1001U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING,
          "second WANT_READ advances step count without success");
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1002U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_STEP_LIMIT &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_FAILED &&
              hfoc_tls_handshake_failure_count(&handshake) == 1U,
          "step limit fails closed after exact budget");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_progress_want_write_and_progress(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token want_write_token =
        make_token(0U, 1U);
    struct hfoc_tls_handshake_token progress_token =
        make_token(1U, 2U);
    struct step_context want_write_context;
    struct step_context progress_context;
    struct hfoc_tls_handshake_config want_write_config;
    struct hfoc_tls_handshake_config progress_config;
    enum hfoc_tls_handshake_status want_write_reserve;
    enum hfoc_tls_handshake_status want_write_configure;
    enum hfoc_tls_handshake_status progress_reserve;
    enum hfoc_tls_handshake_status progress_configure;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    want_write_config = make_config(
        0U, 1U, &bio, &want_write_context, 100U, 10U,
        STEP_MODE_WANT_WRITE, 0U);

    want_write_reserve = hfoc_tls_handshake_reserve(
        &handshake, &want_write_token);
    want_write_configure = hfoc_tls_handshake_configure(
        &handshake, &want_write_token, &want_write_config, 1000U);
    progress_config = make_config(
        1U, 2U, &bio, &progress_context, 100U, 10U,
        STEP_MODE_PROGRESS_THEN_COMPLETE, 2U);
    progress_reserve = hfoc_tls_handshake_reserve(
        &handshake, &progress_token);
    progress_configure = hfoc_tls_handshake_configure(
        &handshake, &progress_token, &progress_config, 1000U);
    if (want_write_reserve != HFOC_TLS_HANDSHAKE_OK ||
        want_write_configure != HFOC_TLS_HANDSHAKE_OK ||
        progress_reserve != HFOC_TLS_HANDSHAKE_OK ||
        progress_configure != HFOC_TLS_HANDSHAKE_OK) {
        fprintf(stderr,
                "want-write/progress statuses: %d %d %d %d\n",
                want_write_reserve,
                want_write_configure,
                progress_reserve,
                progress_configure);
    }
    CHECK(want_write_reserve == HFOC_TLS_HANDSHAKE_OK &&
              want_write_configure == HFOC_TLS_HANDSHAKE_OK &&
              progress_reserve == HFOC_TLS_HANDSHAKE_OK &&
              progress_configure == HFOC_TLS_HANDSHAKE_OK,
          "want-write/progress fixture reserves and configures two slots");

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &want_write_token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              consumed == sizeof(client_hello) &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING &&
              hfoc_tls_handshake_success_count(&handshake) == 0U,
          "WANT_WRITE keeps the handshake active without success");

    consumed = 0U;
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &progress_token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              consumed == sizeof(client_hello) &&
              hfoc_tls_handshake_state_at(&handshake, 1U) ==
                  HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING &&
              hfoc_tls_handshake_success_count(&handshake) == 0U,
          "PROGRESS advances without publishing success");

    consumed = 0U;
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &progress_token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              consumed == sizeof(client_hello) &&
              hfoc_tls_handshake_state_at(&handshake, 1U) ==
                  HFOC_TLS_HANDSHAKE_STATE_SUCCESS &&
              hfoc_tls_handshake_success_count(&handshake) == 1U,
          "second PROGRESS step publishes handshake success");

    CHECK(hfoc_tls_handshake_release(
              &handshake, &want_write_token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_release(
                  &handshake, &progress_token) ==
                  HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "want-write/progress fixture releases both slots cleanly");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(allocator_initial_counts(&snapshot) &&
              connection_inactive(&snapshot, 0U) &&
              connection_inactive(&snapshot, 1U),
          "want-write/progress fixture restores allocator counts");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_invalid_progress_cleanup(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);
    struct step_context step_context;
    struct hfoc_tls_handshake_config config;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_INVALID_CONSUME, 0U);
    CHECK(hfoc_tls_handshake_reserve(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_configure(
                  &handshake, &token, &config, 1000U) ==
                  HFOC_TLS_HANDSHAKE_OK,
          "invalid-progress fixture reserves and configures one slot");

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1000U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_INVALID_PROGRESS &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_FAILED &&
              hfoc_tls_handshake_failure_count(&handshake) == 1U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "invalid consumed length fails and cleans the slot");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(allocator_initial_counts(&snapshot) &&
              connection_inactive(&snapshot, 0U),
          "invalid consumed length restores allocator counts");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_deadline_expiry_cleanup(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);
    struct step_context step_context;
    struct hfoc_tls_handshake_config config;
    const uint8_t client_hello[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_WANT, 0U);
    CHECK(hfoc_tls_handshake_reserve(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_configure(
                  &handshake, &token, &config, 1000U) ==
                  HFOC_TLS_HANDSHAKE_OK,
          "deadline fixture reserves and configures one slot");
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1099U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK,
          "progress before deadline is accepted");

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1100U,
              client_hello, sizeof(client_hello), &consumed) ==
              HFOC_TLS_HANDSHAKE_DEADLINE_EXPIRED &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_FAILED &&
              hfoc_tls_handshake_failure_count(&handshake) == 1U &&
              hfoc_tls_handshake_active_count(&handshake) == 0U &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U,
          "deadline expiry fails and cleans the slot");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(allocator_initial_counts(&snapshot) &&
              connection_inactive(&snapshot, 0U),
          "deadline expiry restores allocator counts");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_malformed_clienthello_isolation(void)
{
    struct hfoc_mbedtls_alloc_snapshot snapshot;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token tokens[3];
    struct step_context step_contexts[3];
    struct hfoc_tls_handshake_config configs[3];
    const uint8_t malformed[] = {
        0x00, 0x03, 0x01, 0x00, 0x05,
    };
    const uint8_t valid[] = {
        0x16, 0x03, 0x01, 0x00, 0x05,
    };
    size_t consumed = 0U;
    uint32_t index;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);

    for (index = 0U; index < 3U; ++index) {
        tokens[index] = make_token(index, index + 1U);
        configs[index] = make_config(
            index,
            index + 1U,
            &bio,
            &step_contexts[index],
            100U,
            10U,
            index == 0U ? STEP_MODE_FAIL : STEP_MODE_COMPLETE,
            0U);
        CHECK(hfoc_tls_handshake_reserve(
                  &handshake, &tokens[index]) ==
                  HFOC_TLS_HANDSHAKE_OK &&
                  hfoc_tls_handshake_configure(
                      &handshake, &tokens[index],
                      &configs[index], 1000U) ==
                      HFOC_TLS_HANDSHAKE_OK,
              "isolation fixture reserves three slots");
    }

    CHECK(hfoc_tls_handshake_progress(
              &handshake, &tokens[0], 1000U,
              malformed, sizeof(malformed), &consumed) ==
              HFOC_TLS_HANDSHAKE_STEP_FAILED &&
              hfoc_tls_handshake_state_at(&handshake, 0U) ==
                  HFOC_TLS_HANDSHAKE_STATE_FAILED &&
              hfoc_tls_handshake_failure_count(&handshake) == 1U &&
              hfoc_tls_handshake_active_count(&handshake) == 2U &&
              hfoc_mbedtls_bio_active_count(&bio) == 2U,
          "malformed ClientHello fails exactly one slot");
    snapshot = hfoc_mbedtls_alloc_snapshot();
    CHECK(connection_inactive(&snapshot, 0U) &&
              snapshot.connections[1].active &&
              snapshot.connections[2].active &&
              snapshot.total_used_bytes == 0U,
          "failed slot releases allocator without touching others");

    consumed = 0U;
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &tokens[1], 1000U,
              valid, sizeof(valid), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state_at(&handshake, 1U) ==
                  HFOC_TLS_HANDSHAKE_STATE_SUCCESS &&
              hfoc_tls_handshake_success_count(&handshake) == 1U,
          "unrelated slot still publishes success");
    consumed = 0U;
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &tokens[2], 1000U,
              valid, sizeof(valid), &consumed) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_state_at(&handshake, 2U) ==
                  HFOC_TLS_HANDSHAKE_STATE_SUCCESS,
          "second unrelated slot is unaffected by failure");

    CHECK(hfoc_tls_handshake_release(
              &handshake, &tokens[1]) == HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_release(
                  &handshake, &tokens[2]) == HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_active_count(&handshake) == 0U,
          "isolation fixture releases surviving slots");

    hfoc_mbedtls_alloc_shutdown();
}

static void test_stale_generation_and_validation(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_tls_handshake handshake;
    struct hfoc_tls_handshake_token token =
        make_token(0U, 1U);
    struct hfoc_tls_handshake_token stale =
        make_token(0U, 99U);
    struct hfoc_tls_handshake_token invalid =
        make_token(HFOC_TLS_HANDSHAKE_SLOT_COUNT, 1U);
    struct step_context step_context;
    struct hfoc_tls_handshake_config config;
    size_t consumed = 0U;

    reset_allocator();
    init_bio(&bio);
    init_handshake(&handshake);
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_COMPLETE, 0U);

    CHECK(hfoc_tls_handshake_reserve(&handshake, &invalid) ==
              HFOC_TLS_HANDSHAKE_BAD_TOKEN,
          "reserve rejects invalid slot index");
    CHECK(hfoc_tls_handshake_reserve(&handshake, &token) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_reserve(
                  &handshake, &token) ==
                  HFOC_TLS_HANDSHAKE_ALREADY_RESERVED,
          "reserve rejects duplicate reservation");

    config.version = 0U;
    CHECK(hfoc_tls_handshake_configure(
              &handshake, &token, &config, 1000U) ==
              HFOC_TLS_HANDSHAKE_INVALID_CONFIG,
          "configure rejects invalid version");
    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_COMPLETE, 0U);
    config.bio = NULL;
    CHECK(hfoc_tls_handshake_configure(
              &handshake, &token, &config, 1000U) ==
              HFOC_TLS_HANDSHAKE_INVALID_CONFIG,
          "configure rejects missing BIO gate");

    config = make_config(
        0U, 1U, &bio, &step_context, 100U, 10U,
        STEP_MODE_COMPLETE, 0U);
    CHECK(hfoc_tls_handshake_configure(
              &handshake, &token, &config, 1000U) ==
              HFOC_TLS_HANDSHAKE_OK &&
              hfoc_tls_handshake_progress(
                  &handshake, &stale, 1000U,
                  NULL, 0U, &consumed) ==
                  HFOC_TLS_HANDSHAKE_STALE_TOKEN,
          "stale token cannot progress a configured slot");
    CHECK(hfoc_tls_handshake_progress(
              &handshake, &token, 1000U,
              NULL, 0U, &consumed) == HFOC_TLS_HANDSHAKE_OK,
          "zero-length input is a valid WANT_READ step");

    hfoc_mbedtls_alloc_shutdown();
}

int main(void)
{
    test_strings_and_unpublished();
    test_init_validation();
    test_reserve_configure_success_release();
    test_progress_wait_and_limit();
    test_progress_want_write_and_progress();
    test_invalid_progress_cleanup();
    test_deadline_expiry_cleanup();
    test_malformed_clienthello_isolation();
    test_stale_generation_and_validation();

    if (failures == 0) {
        printf("TLS handshake state machine host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "TLS handshake state machine host tests: %d FAILURES\n",
            failures);
    return 1;
}
