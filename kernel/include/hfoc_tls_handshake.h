#ifndef HFOC_TLS_HANDSHAKE_H
#define HFOC_TLS_HANDSHAKE_H

#include "hfoc_mbedtls_bio.h"
#include "hfoc_mbedtls_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_TLS_HANDSHAKE_VERSION 1U
#define HFOC_TLS_HANDSHAKE_SLOT_COUNT 4U

#define HFOC_TLS_HANDSHAKE_GENERATION_MIN 1U
#define HFOC_TLS_HANDSHAKE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define HFOC_TLS_HANDSHAKE_DEADLINE_MAX (UINT32_MAX / 4U)
#define HFOC_TLS_HANDSHAKE_STEP_LIMIT_MAX 1024U

#define HFOC_TLS_HANDSHAKE_DRBG_GENERATION_MIN 1U
#define HFOC_TLS_HANDSHAKE_DRBG_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HFOC_TLS_HANDSHAKE_DRBG_SOURCE_MIN 1U
#define HFOC_TLS_HANDSHAKE_DRBG_SOURCE_MAX UINT32_C(0xFFFFFFFE)

enum hfoc_tls_handshake_state {
    HFOC_TLS_HANDSHAKE_STATE_FREE = 0,
    HFOC_TLS_HANDSHAKE_STATE_RESERVED = 1,
    HFOC_TLS_HANDSHAKE_STATE_CONFIGURED = 2,
    HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING = 3,
    HFOC_TLS_HANDSHAKE_STATE_SUCCESS = 4,
    HFOC_TLS_HANDSHAKE_STATE_FAILED = 5,
};

enum hfoc_tls_handshake_fault {
    HFOC_TLS_HANDSHAKE_FAULT_NONE = 0,
    HFOC_TLS_HANDSHAKE_FAULT_ALLOCATOR_UNAVAILABLE = 1,
    HFOC_TLS_HANDSHAKE_FAULT_BIO_ATTACH_FAILED = 2,
    HFOC_TLS_HANDSHAKE_FAULT_INVALID_CONFIG = 3,
    HFOC_TLS_HANDSHAKE_FAULT_STEP_FAILED = 4,
    HFOC_TLS_HANDSHAKE_FAULT_INVALID_PROGRESS = 5,
    HFOC_TLS_HANDSHAKE_FAULT_DEADLINE_EXPIRED = 6,
    HFOC_TLS_HANDSHAKE_FAULT_STEP_LIMIT = 7,
};

enum hfoc_tls_handshake_status {
    HFOC_TLS_HANDSHAKE_OK = 0,
    HFOC_TLS_HANDSHAKE_NULL = 1,
    HFOC_TLS_HANDSHAKE_BAD_STATE = 2,
    HFOC_TLS_HANDSHAKE_BAD_TOKEN = 3,
    HFOC_TLS_HANDSHAKE_STALE_TOKEN = 4,
    HFOC_TLS_HANDSHAKE_NOT_ATTACHED = 5,
    HFOC_TLS_HANDSHAKE_ALREADY_RESERVED = 6,
    HFOC_TLS_HANDSHAKE_INVALID_CONFIG = 7,
    HFOC_TLS_HANDSHAKE_ALLOCATOR_UNAVAILABLE = 8,
    HFOC_TLS_HANDSHAKE_BIO_ATTACH_FAILED = 9,
    HFOC_TLS_HANDSHAKE_STEP_FAILED = 10,
    HFOC_TLS_HANDSHAKE_INVALID_PROGRESS = 11,
    HFOC_TLS_HANDSHAKE_DEADLINE_EXPIRED = 12,
    HFOC_TLS_HANDSHAKE_STEP_LIMIT = 13,
};

enum hfoc_tls_handshake_step_result {
    HFOC_TLS_HANDSHAKE_STEP_WANT_READ = 0,
    HFOC_TLS_HANDSHAKE_STEP_WANT_WRITE = 1,
    HFOC_TLS_HANDSHAKE_STEP_PROGRESS = 2,
    HFOC_TLS_HANDSHAKE_STEP_COMPLETE = 3,
    HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED = 4,
};

typedef enum hfoc_tls_handshake_step_result
    (*hfoc_tls_handshake_step_fn)(
        void *context,
        const uint8_t *input,
        size_t input_length,
        size_t *bytes_consumed);

struct hfoc_tls_handshake_token {
    uint32_t index;
    uint32_t generation;
};

struct hfoc_tls_handshake_config {
    uint32_t version;
    uint32_t deadline_ticks;
    uint32_t max_steps;
    uint32_t drbg_generation;
    uint32_t drbg_source_id;
    const struct hfoc_mbedtls_manifest *manifest;
    const struct hfoc_mbedtls_bio *bio;
    const struct hfoc_mbedtls_bio_token *bio_token;
    const struct hfoc_mbedtls_bio_policy *bio_policy;
    const struct hfoc_mbedtls_bio_transport *bio_transport;
    hfoc_tls_handshake_step_fn step;
    void *step_context;
};

struct hfoc_tls_handshake_entry {
    bool active;
    bool success_published;
    uint32_t generation;
    uint32_t allocator_id;
    uint32_t bio_index;
    uint32_t due_ticks;
    uint32_t max_steps;
    uint32_t step_count;
    uint32_t drbg_generation;
    uint32_t drbg_source_id;
    enum hfoc_tls_handshake_state state;
    enum hfoc_tls_handshake_fault fault;
    const struct hfoc_mbedtls_manifest *manifest;
    const struct hfoc_mbedtls_bio *bio;
    struct hfoc_mbedtls_bio_token bio_token;
    hfoc_tls_handshake_step_fn step;
    void *step_context;
};

struct hfoc_tls_handshake {
    bool published;
    uint32_t version;
    uint32_t active_count;
    uint32_t success_count;
    uint32_t failure_count;
    struct hfoc_tls_handshake_entry
        entries[HFOC_TLS_HANDSHAKE_SLOT_COUNT];
};

enum hfoc_tls_handshake_status hfoc_tls_handshake_init(
    struct hfoc_tls_handshake *handshake);
enum hfoc_tls_handshake_status hfoc_tls_handshake_reserve(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);
enum hfoc_tls_handshake_status hfoc_tls_handshake_configure(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token,
    const struct hfoc_tls_handshake_config *config,
    uint32_t now_ticks);
enum hfoc_tls_handshake_status hfoc_tls_handshake_progress(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token,
    uint32_t now_ticks,
    const uint8_t *input,
    size_t input_length,
    size_t *bytes_consumed);
enum hfoc_tls_handshake_status hfoc_tls_handshake_release(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);

bool hfoc_tls_handshake_published(
    const struct hfoc_tls_handshake *handshake);
uint32_t hfoc_tls_handshake_active_count(
    const struct hfoc_tls_handshake *handshake);
uint32_t hfoc_tls_handshake_success_count(
    const struct hfoc_tls_handshake *handshake);
uint32_t hfoc_tls_handshake_failure_count(
    const struct hfoc_tls_handshake *handshake);

enum hfoc_tls_handshake_state hfoc_tls_handshake_state_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index);
enum hfoc_tls_handshake_fault hfoc_tls_handshake_fault_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index);
bool hfoc_tls_handshake_success_published_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index);
uint32_t hfoc_tls_handshake_generation_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index);

enum hfoc_tls_handshake_state hfoc_tls_handshake_state(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);
enum hfoc_tls_handshake_fault hfoc_tls_handshake_fault(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);
bool hfoc_tls_handshake_success_published(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);
uint32_t hfoc_tls_handshake_generation(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token);

const char *hfoc_tls_handshake_state_string(
    enum hfoc_tls_handshake_state state);
const char *hfoc_tls_handshake_fault_string(
    enum hfoc_tls_handshake_fault fault);
const char *hfoc_tls_handshake_status_string(
    enum hfoc_tls_handshake_status status);
const char *hfoc_tls_handshake_step_result_string(
    enum hfoc_tls_handshake_step_result result);

#endif
