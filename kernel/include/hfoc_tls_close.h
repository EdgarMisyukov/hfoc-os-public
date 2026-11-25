#ifndef HFOC_TLS_CLOSE_H
#define HFOC_TLS_CLOSE_H

#include "hfoc_tls_handshake.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_TLS_CLOSE_VERSION 1U
#define HFOC_TLS_CLOSE_SLOT_COUNT 4U

#define HFOC_TLS_CLOSE_GENERATION_MIN 1U
#define HFOC_TLS_CLOSE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define HFOC_TLS_CLOSE_DEADLINE_MAX (UINT32_MAX / 4U)
#define HFOC_TLS_CLOSE_STEP_LIMIT_MAX 1024U

enum hfoc_tls_close_state {
    HFOC_TLS_CLOSE_STATE_FREE = 0,
    HFOC_TLS_CLOSE_STATE_RESERVED = 1,
    HFOC_TLS_CLOSE_STATE_CLOSING = 2,
    HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT = 3,
    HFOC_TLS_CLOSE_STATE_FAILED = 4,
    HFOC_TLS_CLOSE_STATE_ABORTED = 5,
};

enum hfoc_tls_close_outcome {
    HFOC_TLS_CLOSE_OUTCOME_NONE = 0,
    HFOC_TLS_CLOSE_OUTCOME_CLOSE_NOTIFY_SENT = 1,
    HFOC_TLS_CLOSE_OUTCOME_ABORTED = 2,
    HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_CLOSED = 3,
    HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_REJECTED = 4,
    HFOC_TLS_CLOSE_OUTCOME_DEADLINE_EXPIRED = 5,
    HFOC_TLS_CLOSE_OUTCOME_STEP_LIMIT = 6,
    HFOC_TLS_CLOSE_OUTCOME_INVALID_PROGRESS = 7,
};

enum hfoc_tls_close_status {
    HFOC_TLS_CLOSE_OK = 0,
    HFOC_TLS_CLOSE_NULL = 1,
    HFOC_TLS_CLOSE_BAD_STATE = 2,
    HFOC_TLS_CLOSE_BAD_TOKEN = 3,
    HFOC_TLS_CLOSE_STALE_TOKEN = 4,
    HFOC_TLS_CLOSE_NOT_ATTACHED = 5,
    HFOC_TLS_CLOSE_ALREADY_RESERVED = 6,
    HFOC_TLS_CLOSE_INVALID_CONFIG = 7,
    HFOC_TLS_CLOSE_DEADLINE_EXPIRED = 8,
    HFOC_TLS_CLOSE_STEP_LIMIT = 9,
    HFOC_TLS_CLOSE_INVALID_PROGRESS = 10,
    HFOC_TLS_CLOSE_TRANSPORT_CLOSED = 11,
    HFOC_TLS_CLOSE_TRANSPORT_REJECTED = 12,
    HFOC_TLS_CLOSE_CLOSE_FAILED = 13,
    HFOC_TLS_CLOSE_ABORTED = 14,
};

enum hfoc_tls_close_step_result {
    HFOC_TLS_CLOSE_STEP_WANT_READ = 0,
    HFOC_TLS_CLOSE_STEP_WANT_WRITE = 1,
    HFOC_TLS_CLOSE_STEP_PROGRESS = 2,
    HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT = 3,
    HFOC_TLS_CLOSE_STEP_TRANSPORT_CLOSED = 4,
    HFOC_TLS_CLOSE_STEP_TRANSPORT_REJECTED = 5,
    HFOC_TLS_CLOSE_STEP_FAILED = 6,
};

enum hfoc_tls_close_error {
    HFOC_TLS_CLOSE_ERROR_NONE = 0,
    HFOC_TLS_CLOSE_ERROR_SUCCESS = 1,
    HFOC_TLS_CLOSE_ERROR_WANT_READ = 2,
    HFOC_TLS_CLOSE_ERROR_WANT_WRITE = 3,
    HFOC_TLS_CLOSE_ERROR_TRANSPORT_CLOSED = 4,
    HFOC_TLS_CLOSE_ERROR_PEER_CLOSE_NOTIFY = 5,
    HFOC_TLS_CLOSE_ERROR_BAD_INPUT = 6,
    HFOC_TLS_CLOSE_ERROR_PROTOCOL = 7,
    HFOC_TLS_CLOSE_ERROR_TIMEOUT = 8,
    HFOC_TLS_CLOSE_ERROR_INTERNAL = 9,
    HFOC_TLS_CLOSE_ERROR_UNKNOWN = 10,
};

typedef enum hfoc_tls_close_step_result
    (*hfoc_tls_close_step_fn)(void *context);

typedef void (*hfoc_tls_close_zeroize_fn)(void *context);

struct hfoc_tls_close_token {
    uint32_t index;
    uint32_t generation;
};

struct hfoc_tls_close_config {
    uint32_t version;
    uint32_t deadline_ticks;
    uint32_t max_steps;
    const struct hfoc_tls_handshake *handshake;
    const struct hfoc_tls_handshake_token *handshake_token;
    const struct hfoc_mbedtls_bio *bio;
    const struct hfoc_mbedtls_bio_token *bio_token;
    hfoc_tls_close_step_fn step;
    void *step_context;
    hfoc_tls_close_zeroize_fn zeroize;
    void *zeroize_context;
};

struct hfoc_tls_close_entry {
    bool active;
    uint32_t generation;
    uint32_t due_ticks;
    uint32_t max_steps;
    uint32_t step_count;
    enum hfoc_tls_close_state state;
    enum hfoc_tls_close_outcome outcome;
    const struct hfoc_tls_handshake *handshake;
    struct hfoc_tls_handshake_token handshake_token;
    const struct hfoc_mbedtls_bio *bio;
    struct hfoc_mbedtls_bio_token bio_token;
    hfoc_tls_close_step_fn step;
    void *step_context;
    hfoc_tls_close_zeroize_fn zeroize;
    void *zeroize_context;
};

struct hfoc_tls_close {
    bool published;
    uint32_t version;
    uint32_t active_count;
    uint32_t close_notify_count;
    uint32_t abort_count;
    uint32_t failure_count;
    struct hfoc_tls_close_entry
        entries[HFOC_TLS_CLOSE_SLOT_COUNT];
};

enum hfoc_tls_close_status hfoc_tls_close_init(
    struct hfoc_tls_close *close);
enum hfoc_tls_close_status hfoc_tls_close_reserve(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);
enum hfoc_tls_close_status hfoc_tls_close_configure(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token,
    const struct hfoc_tls_close_config *config,
    uint32_t now_ticks);
enum hfoc_tls_close_status hfoc_tls_close_progress(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token,
    uint32_t now_ticks);
enum hfoc_tls_close_status hfoc_tls_close_abort(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);
enum hfoc_tls_close_status hfoc_tls_close_release(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);

bool hfoc_tls_close_published(
    const struct hfoc_tls_close *close);
uint32_t hfoc_tls_close_active_count(
    const struct hfoc_tls_close *close);
uint32_t hfoc_tls_close_close_notify_count(
    const struct hfoc_tls_close *close);
uint32_t hfoc_tls_close_abort_count(
    const struct hfoc_tls_close *close);
uint32_t hfoc_tls_close_failure_count(
    const struct hfoc_tls_close *close);

enum hfoc_tls_close_state hfoc_tls_close_state_at(
    const struct hfoc_tls_close *close,
    uint32_t index);
enum hfoc_tls_close_outcome hfoc_tls_close_outcome_at(
    const struct hfoc_tls_close *close,
    uint32_t index);
uint32_t hfoc_tls_close_generation_at(
    const struct hfoc_tls_close *close,
    uint32_t index);

enum hfoc_tls_close_state hfoc_tls_close_state(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);
enum hfoc_tls_close_outcome hfoc_tls_close_outcome(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);
uint32_t hfoc_tls_close_generation(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token);

enum hfoc_tls_close_error hfoc_tls_close_error_normalize(
    int result);

const char *hfoc_tls_close_state_string(
    enum hfoc_tls_close_state state);
const char *hfoc_tls_close_outcome_string(
    enum hfoc_tls_close_outcome outcome);
const char *hfoc_tls_close_status_string(
    enum hfoc_tls_close_status status);
const char *hfoc_tls_close_step_result_string(
    enum hfoc_tls_close_step_result result);
const char *hfoc_tls_close_error_string(
    enum hfoc_tls_close_error error);

#endif
