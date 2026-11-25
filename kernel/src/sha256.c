#include "sha256.h"

#include <stddef.h>
#include <stdint.h>

static const uint32_t sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t sha256_rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

static uint32_t sha256_load_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void sha256_store_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void sha256_store_be64(uint8_t *bytes, uint64_t value) {
    uint32_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] =
            (uint8_t)(value >> (56U - 8U * index));
    }
}

static void sha256_copy_bytes(uint8_t *destination,
                              const uint8_t *source,
                              size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static void sha256_zero_bytes(uint8_t *destination, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
}

#if defined(HFOC_SHA256_HOST_TEST)
/* The credential KDF's cost is counted in compressions; see sha256.h. */
static unsigned long long sha256_host_test_compression_count;

void sha256_host_test_reset_compressions(void)
{
    sha256_host_test_compression_count = 0ULL;
}

unsigned long long sha256_host_test_compressions(void)
{
    return sha256_host_test_compression_count;
}
#endif

static void sha256_compress(struct sha256 *context,
                            const uint8_t block[SHA256_BLOCK_SIZE])
{
#if defined(HFOC_SHA256_HOST_TEST)
    ++sha256_host_test_compression_count;
#endif
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        w[index] = sha256_load_be32(block + 4U * index);
    }
    for (index = 16U; index < 64U; ++index) {
        const uint32_t s0 =
            sha256_rotr(w[index - 15U], 7U) ^
            sha256_rotr(w[index - 15U], 18U) ^
            (w[index - 15U] >> 3U);
        const uint32_t s1 =
            sha256_rotr(w[index - 2U], 17U) ^
            sha256_rotr(w[index - 2U], 19U) ^
            (w[index - 2U] >> 10U);

        w[index] = w[index - 16U] + s0 + w[index - 7U] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (index = 0U; index < 64U; ++index) {
        const uint32_t big_s0 =
            sha256_rotr(a, 2U) ^ sha256_rotr(a, 13U) ^
            sha256_rotr(a, 22U);
        const uint32_t big_s1 =
            sha256_rotr(e, 6U) ^ sha256_rotr(e, 11U) ^
            sha256_rotr(e, 25U);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp1 = h + big_s1 + ch + sha256_k[index] + w[index];
        const uint32_t temp2 = big_s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

enum sha256_status sha256_init(struct sha256 *context) {
    static const uint32_t initial_state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    uint32_t index;

    if (context == NULL) {
        return SHA256_NULL;
    }
    context->initialized = false;
    context->total_bytes = 0U;
    context->buffer_used = 0U;
    sha256_zero_bytes(context->buffer, sizeof(context->buffer));
    for (index = 0U; index < 8U; ++index) {
        context->state[index] = initial_state[index];
    }
    context->initialized = true;
    return SHA256_OK;
}

enum sha256_status sha256_update(struct sha256 *context,
                                 const void *data,
                                 size_t data_length)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (context == NULL || data == NULL) {
        return SHA256_NULL;
    }
    if (!context->initialized) {
        return SHA256_BAD_STATE;
    }
    if (context->total_bytes > SHA256_MAX_INPUT_BYTES ||
        data_length > SHA256_MAX_INPUT_BYTES - context->total_bytes) {
        return SHA256_OVERFLOW;
    }

    while (data_length != 0U) {
        const size_t available =
            SHA256_BLOCK_SIZE - context->buffer_used;
        const size_t take =
            data_length < available ? data_length : available;

        if (context->buffer_used == SHA256_BLOCK_SIZE) {
            sha256_compress(context, context->buffer);
            context->total_bytes += SHA256_BLOCK_SIZE;
            context->buffer_used = 0U;
        } else {
            sha256_copy_bytes(context->buffer + context->buffer_used,
                              bytes, take);
            context->buffer_used += (uint32_t)take;
            bytes += take;
            data_length -= take;
        }
    }
    return SHA256_OK;
}

enum sha256_status sha256_absorb_block(struct sha256 *context)
{
    if (context == NULL) {
        return SHA256_NULL;
    }
    if (!context->initialized) {
        return SHA256_BAD_STATE;
    }
    if (context->buffer_used != SHA256_BLOCK_SIZE) {
        return SHA256_BAD_STATE;
    }
    if (context->total_bytes > SHA256_MAX_INPUT_BYTES - SHA256_BLOCK_SIZE) {
        context->initialized = false;
        return SHA256_OVERFLOW;
    }
    sha256_compress(context, context->buffer);
    context->total_bytes += SHA256_BLOCK_SIZE;
    context->buffer_used = 0U;
    return SHA256_OK;
}

enum sha256_status sha256_final(
    struct sha256 *context,
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bit_length;
    uint32_t index;

    if (context == NULL || digest == NULL) {
        return SHA256_NULL;
    }
    if (!context->initialized) {
        return SHA256_BAD_STATE;
    }
    if (context->total_bytes > SHA256_MAX_INPUT_BYTES ||
        context->buffer_used > SHA256_BLOCK_SIZE) {
        context->initialized = false;
        return SHA256_OVERFLOW;
    }

    bit_length = (context->total_bytes + context->buffer_used) * 8U;
    if (context->buffer_used == SHA256_BLOCK_SIZE) {
        sha256_compress(context, context->buffer);
        context->total_bytes += SHA256_BLOCK_SIZE;
        context->buffer_used = 0U;
    }

    context->buffer[context->buffer_used] = 0x80U;
    ++context->buffer_used;
    if (context->buffer_used > 56U) {
        sha256_zero_bytes(context->buffer + context->buffer_used,
                          SHA256_BLOCK_SIZE - context->buffer_used);
        sha256_compress(context, context->buffer);
        context->total_bytes += SHA256_BLOCK_SIZE;
        context->buffer_used = 0U;
    }
    while (context->buffer_used < 56U) {
        context->buffer[context->buffer_used] = 0U;
        ++context->buffer_used;
    }
    sha256_store_be64(context->buffer + 56U, bit_length);
    sha256_compress(context, context->buffer);
    context->total_bytes += SHA256_BLOCK_SIZE;

    for (index = 0U; index < 8U; ++index) {
        sha256_store_be32(digest + 4U * index, context->state[index]);
    }
    context->initialized = false;
    return SHA256_OK;
}

enum sha256_status sha256_compute(const void *data,
                                  size_t data_length,
                                  uint8_t digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    enum sha256_status status;

    if (data == NULL || digest == NULL) {
        return SHA256_NULL;
    }
    status = sha256_init(&context);
    if (status != SHA256_OK) {
        return status;
    }
    status = sha256_update(&context, data, data_length);
    if (status != SHA256_OK) {
        return status;
    }
    return sha256_final(&context, digest);
}

const char *sha256_status_string(enum sha256_status status) {
    switch (status) {
    case SHA256_OK:
        return "SHA256_OK";
    case SHA256_NULL:
        return "SHA256_NULL";
    case SHA256_BAD_STATE:
        return "SHA256_BAD_STATE";
    case SHA256_OVERFLOW:
        return "SHA256_OVERFLOW";
    default:
        return "SHA256_UNKNOWN";
    }
}
