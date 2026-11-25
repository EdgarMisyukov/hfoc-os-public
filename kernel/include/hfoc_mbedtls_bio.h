#ifndef HFOC_MBEDTLS_BIO_H
#define HFOC_MBEDTLS_BIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_MBEDTLS_BIO_VERSION 1U
#define HFOC_MBEDTLS_BIO_SLOT_COUNT 4U
#define HFOC_MBEDTLS_BIO_GENERATION_MIN 1U
#define HFOC_MBEDTLS_BIO_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define HFOC_MBEDTLS_BIO_MAX_LEN_MIN 1U
#define HFOC_MBEDTLS_BIO_MAX_LEN_MAX 4096U
#define HFOC_MBEDTLS_BIO_MAX_SEND_BYTES HFOC_MBEDTLS_BIO_MAX_LEN_MAX
#define HFOC_MBEDTLS_BIO_MAX_RECV_BYTES HFOC_MBEDTLS_BIO_MAX_LEN_MAX

enum hfoc_mbedtls_bio_transport_status {
    HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED = 0,
    HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED = 1,
    HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED = 2,
    HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED = 3,
};

enum hfoc_mbedtls_bio_state {
    HFOC_MBEDTLS_BIO_STATE_FREE = 0,
    HFOC_MBEDTLS_BIO_STATE_ATTACHED = 1,
    HFOC_MBEDTLS_BIO_STATE_WANT_READ = 2,
    HFOC_MBEDTLS_BIO_STATE_WANT_WRITE = 3,
    HFOC_MBEDTLS_BIO_STATE_CLOSED = 4,
    HFOC_MBEDTLS_BIO_STATE_FAILED = 5,
};

enum hfoc_mbedtls_bio_status {
    HFOC_MBEDTLS_BIO_OK = 0,
    HFOC_MBEDTLS_BIO_NULL = 1,
    HFOC_MBEDTLS_BIO_BAD_STATE = 2,
    HFOC_MBEDTLS_BIO_BAD_TOKEN = 3,
    HFOC_MBEDTLS_BIO_STALE_TOKEN = 4,
    HFOC_MBEDTLS_BIO_ALREADY_ATTACHED = 5,
    HFOC_MBEDTLS_BIO_INVALID_POLICY = 6,
    HFOC_MBEDTLS_BIO_INVALID_TRANSPORT = 7,
    HFOC_MBEDTLS_BIO_NOT_ATTACHED = 8,
};

typedef enum hfoc_mbedtls_bio_transport_status
    (*hfoc_mbedtls_bio_transport_send_fn)(
        void *ctx,
        const uint8_t *bytes,
        size_t length,
        size_t *bytes_accepted);
typedef enum hfoc_mbedtls_bio_transport_status
    (*hfoc_mbedtls_bio_transport_recv_fn)(
        void *ctx,
        uint8_t *bytes,
        size_t capacity,
        size_t *bytes_received);

struct hfoc_mbedtls_bio_transport {
    hfoc_mbedtls_bio_transport_send_fn send;
    hfoc_mbedtls_bio_transport_recv_fn recv;
    void *ctx;
};

struct hfoc_mbedtls_bio_policy {
    uint32_t version;
    uint32_t max_send_bytes;
    uint32_t max_recv_bytes;
};

struct hfoc_mbedtls_bio_token {
    uint32_t index;
    uint32_t generation;
};

struct hfoc_mbedtls_bio;

struct hfoc_mbedtls_bio_entry {
    struct hfoc_mbedtls_bio *owner;
    struct hfoc_mbedtls_bio_token token;
    bool active;
    uint32_t generation;
    enum hfoc_mbedtls_bio_state state;
    struct hfoc_mbedtls_bio_policy policy;
    struct hfoc_mbedtls_bio_transport transport;
    uint32_t send_calls;
    uint32_t recv_calls;
    uint32_t want_read_count;
    uint32_t want_write_count;
    uint32_t short_send_count;
    uint32_t short_recv_count;
    uint32_t send_bytes;
    uint32_t recv_bytes;
    uint32_t rejected_count;
    uint32_t closed_count;
};

struct hfoc_mbedtls_bio {
    bool published;
    uint32_t version;
    uint32_t active_count;
    struct hfoc_mbedtls_bio_entry
        entries[HFOC_MBEDTLS_BIO_SLOT_COUNT];
};

enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_init(
    struct hfoc_mbedtls_bio *bio);
enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_attach(
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token,
    const struct hfoc_mbedtls_bio_policy *policy,
    const struct hfoc_mbedtls_bio_transport *transport);
enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_detach(
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token);

int hfoc_mbedtls_bio_send_cb(
    void *context,
    const unsigned char *buf,
    size_t len);
int hfoc_mbedtls_bio_recv_cb(
    void *context,
    unsigned char *buf,
    size_t len);

bool hfoc_mbedtls_bio_published(
    const struct hfoc_mbedtls_bio *bio);
uint32_t hfoc_mbedtls_bio_active_count(
    const struct hfoc_mbedtls_bio *bio);
enum hfoc_mbedtls_bio_state hfoc_mbedtls_bio_state(
    const struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token);
uint32_t hfoc_mbedtls_bio_generation(
    const struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token);

const char *hfoc_mbedtls_bio_state_string(
    enum hfoc_mbedtls_bio_state state);
const char *hfoc_mbedtls_bio_status_string(
    enum hfoc_mbedtls_bio_status status);
const char *hfoc_mbedtls_bio_transport_status_string(
    enum hfoc_mbedtls_bio_transport_status status);

#endif
