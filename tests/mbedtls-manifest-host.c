#include "hfoc_mbedtls_manifest.h"

#include "hfoc_mbedtls_alloc.h"

#include "mbedtls/sha256.h"

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

static const uint8_t hfoc_test_cert_der[] = {
    0x30, 0x82, 0x01, 0x93, 0x30, 0x82, 0x01, 0x39, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x14, 0x77, 0xee, 0xcf, 0x8c, 0x2f, 0x6b, 0x00, 0x53, 0xc2,
    0x8c, 0xb4, 0xa0, 0xdf, 0x35, 0x22, 0xea, 0x12, 0xb1, 0x03, 0x53, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x14, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x09,
    0x68, 0x66, 0x6f, 0x63, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1e, 0x17,
    0x0d, 0x32, 0x36, 0x30, 0x38, 0x32, 0x39, 0x30, 0x37, 0x34, 0x32, 0x32,
    0x30, 0x5a, 0x17, 0x0d, 0x32, 0x36, 0x30, 0x39, 0x32, 0x38, 0x30, 0x37,
    0x34, 0x32, 0x32, 0x30, 0x5a, 0x30, 0x14, 0x31, 0x12, 0x30, 0x10, 0x06,
    0x03, 0x55, 0x04, 0x03, 0x0c, 0x09, 0x68, 0x66, 0x6f, 0x63, 0x2e, 0x74,
    0x65, 0x73, 0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03,
    0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x1c, 0x53, 0x22, 0x3f, 0x97, 0xae,
    0x47, 0xfc, 0x5d, 0x9e, 0x93, 0xed, 0xed, 0x8b, 0x1b, 0xe3, 0xc0, 0x72,
    0x23, 0x41, 0x94, 0x09, 0xda, 0x51, 0x05, 0x39, 0xcf, 0xac, 0xee, 0x53,
    0xc5, 0x2a, 0x4d, 0x0a, 0xbd, 0x8d, 0x72, 0x03, 0x6c, 0xdd, 0x40, 0xdf,
    0x24, 0x99, 0xfa, 0x4a, 0x25, 0xfd, 0x0a, 0x1f, 0x10, 0x54, 0x4a, 0x05,
    0xfe, 0x7b, 0x90, 0x74, 0xf9, 0xc0, 0xf8, 0xd7, 0xb2, 0xf5, 0xa3, 0x69,
    0x30, 0x67, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04,
    0x14, 0xf0, 0x8d, 0x26, 0x20, 0x84, 0x23, 0xd4, 0x48, 0x27, 0x97, 0xae,
    0x15, 0x56, 0x76, 0x3a, 0xcf, 0xd1, 0xcd, 0x3b, 0xfe, 0x30, 0x1f, 0x06,
    0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xf0, 0x8d,
    0x26, 0x20, 0x84, 0x23, 0xd4, 0x48, 0x27, 0x97, 0xae, 0x15, 0x56, 0x76,
    0x3a, 0xcf, 0xd1, 0xcd, 0x3b, 0xfe, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d,
    0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30,
    0x14, 0x06, 0x03, 0x55, 0x1d, 0x11, 0x04, 0x0d, 0x30, 0x0b, 0x82, 0x09,
    0x68, 0x66, 0x6f, 0x63, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x0a, 0x06,
    0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00,
    0x30, 0x45, 0x02, 0x20, 0x37, 0xe6, 0x60, 0xfe, 0x60, 0xf7, 0x25, 0xbe,
    0x84, 0x33, 0xa7, 0xae, 0x98, 0x35, 0x04, 0xb5, 0x1b, 0x71, 0x2f, 0x3a,
    0xbd, 0xf7, 0x1d, 0x5b, 0xa4, 0xba, 0xa3, 0xde, 0x20, 0x0e, 0xbd, 0x5e,
    0x02, 0x21, 0x00, 0xc5, 0x64, 0x24, 0x44, 0x19, 0xe1, 0x81, 0x17, 0x84,
    0xdf, 0x0e, 0xcc, 0x56, 0xc9, 0xcb, 0x06, 0xc4, 0xcc, 0x04, 0x2e, 0x74,
    0x66, 0x1c, 0xd8, 0xf4, 0x30, 0x79, 0xa7, 0xbd, 0x3c, 0x9a, 0x2b,
};

static const uint8_t hfoc_test_key_der[] = {
    0x30, 0x77, 0x02, 0x01, 0x01, 0x04, 0x20, 0xc2, 0x8b, 0x86, 0xa0, 0x0e,
    0xeb, 0x72, 0xa7, 0xff, 0xf1, 0xb7, 0x48, 0x5c, 0xff, 0xad, 0xa0, 0x75,
    0x6f, 0x2e, 0xf2, 0x3c, 0xec, 0x22, 0x25, 0xbb, 0xc6, 0xdf, 0x81, 0x6c,
    0xf0, 0xc5, 0x1b, 0xa0, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
    0x03, 0x01, 0x07, 0xa1, 0x44, 0x03, 0x42, 0x00, 0x04, 0x1c, 0x53, 0x22,
    0x3f, 0x97, 0xae, 0x47, 0xfc, 0x5d, 0x9e, 0x93, 0xed, 0xed, 0x8b, 0x1b,
    0xe3, 0xc0, 0x72, 0x23, 0x41, 0x94, 0x09, 0xda, 0x51, 0x05, 0x39, 0xcf,
    0xac, 0xee, 0x53, 0xc5, 0x2a, 0x4d, 0x0a, 0xbd, 0x8d, 0x72, 0x03, 0x6c,
    0xdd, 0x40, 0xdf, 0x24, 0x99, 0xfa, 0x4a, 0x25, 0xfd, 0x0a, 0x1f, 0x10,
    0x54, 0x4a, 0x05, 0xfe, 0x7b, 0x90, 0x74, 0xf9, 0xc0, 0xf8, 0xd7, 0xb2,
    0xf5,
};

static const uint8_t hfoc_other_cert_der[] = {
    0x30, 0x82, 0x01, 0xa4, 0x30, 0x82, 0x01, 0x49, 0xa0, 0x03, 0x02, 0x01,
    0x02, 0x02, 0x14, 0x5e, 0x18, 0xc3, 0x8c, 0x69, 0xc0, 0x1d, 0x37, 0xc8,
    0xfe, 0xb0, 0xfb, 0x55, 0x55, 0xf5, 0xfd, 0xcf, 0xd1, 0x2d, 0x9d, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
    0x15, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0a,
    0x6f, 0x74, 0x68, 0x65, 0x72, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x1e,
    0x17, 0x0d, 0x32, 0x36, 0x30, 0x38, 0x32, 0x39, 0x30, 0x37, 0x34, 0x37,
    0x30, 0x31, 0x5a, 0x17, 0x0d, 0x32, 0x36, 0x30, 0x39, 0x32, 0x38, 0x30,
    0x37, 0x34, 0x37, 0x30, 0x31, 0x5a, 0x30, 0x15, 0x31, 0x13, 0x30, 0x11,
    0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0a, 0x6f, 0x74, 0x68, 0x65, 0x72,
    0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce,
    0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x0c, 0x0a, 0x24, 0x1f,
    0x9e, 0xa2, 0x16, 0x90, 0xb8, 0xf0, 0xa0, 0xbc, 0x6f, 0xd9, 0xe2, 0xf7,
    0x68, 0xf0, 0x9a, 0xfd, 0x92, 0x73, 0xaa, 0xa6, 0xca, 0x0e, 0x86, 0x1b,
    0x11, 0xec, 0xc7, 0xfc, 0xb7, 0x42, 0x32, 0xb2, 0xea, 0x10, 0xad, 0x81,
    0x90, 0x13, 0xac, 0xbf, 0xdb, 0x48, 0x9f, 0xe9, 0xed, 0x9e, 0x4c, 0x4e,
    0x49, 0xbe, 0xa0, 0xb9, 0x95, 0xe8, 0x76, 0x8f, 0xd9, 0xac, 0x2b, 0x8a,
    0xa3, 0x77, 0x30, 0x75, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04,
    0x16, 0x04, 0x14, 0x6c, 0xe9, 0xc5, 0x23, 0x29, 0x10, 0x74, 0x38, 0x24,
    0x68, 0x8d, 0xc5, 0x95, 0x16, 0xaa, 0x49, 0xae, 0x49, 0x0f, 0x5e, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
    0x6c, 0xe9, 0xc5, 0x23, 0x29, 0x10, 0x74, 0x38, 0x24, 0x68, 0x8d, 0xc5,
    0x95, 0x16, 0xaa, 0x49, 0xae, 0x49, 0x0f, 0x5e, 0x30, 0x15, 0x06, 0x03,
    0x55, 0x1d, 0x11, 0x04, 0x0e, 0x30, 0x0c, 0x82, 0x0a, 0x6f, 0x74, 0x68,
    0x65, 0x72, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x30, 0x0c, 0x06, 0x03, 0x55,
    0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x02, 0x30, 0x00, 0x30, 0x0e, 0x06,
    0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x07,
    0x80, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03,
    0x02, 0x03, 0x49, 0x00, 0x30, 0x46, 0x02, 0x21, 0x00, 0xa5, 0xf2, 0x05,
    0x32, 0xc7, 0xad, 0xa6, 0xa6, 0x73, 0x54, 0x41, 0x70, 0x2e, 0xd0, 0x9f,
    0x07, 0x86, 0x80, 0xd4, 0xe8, 0x31, 0xbd, 0x00, 0xf6, 0x58, 0x91, 0x20,
    0xd5, 0x92, 0xdf, 0x1c, 0x1d, 0x02, 0x21, 0x00, 0xea, 0x27, 0x32, 0x7f,
    0xc2, 0xc2, 0xeb, 0x91, 0x99, 0x47, 0x84, 0x0a, 0x46, 0xb0, 0xc1, 0x2a,
    0x35, 0xa9, 0x57, 0x15, 0x35, 0xb8, 0xe7, 0xf5, 0x24, 0xb5, 0xa7, 0x03,
    0x7a, 0x76, 0x5a, 0xd3,
};

static const uint8_t hfoc_other_key_der[] = {
    0x30, 0x77, 0x02, 0x01, 0x01, 0x04, 0x20, 0xe3, 0x59, 0xd4, 0xe2, 0x54,
    0x61, 0xc0, 0xa9, 0x69, 0xa7, 0x8a, 0xa2, 0x65, 0x9b, 0x11, 0x00, 0x8b,
    0x9e, 0xb6, 0x18, 0x24, 0xe9, 0x37, 0x1a, 0x78, 0x14, 0x7f, 0xfc, 0xc9,
    0x01, 0x2a, 0xdc, 0xa0, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
    0x03, 0x01, 0x07, 0xa1, 0x44, 0x03, 0x42, 0x00, 0x04, 0x0c, 0x0a, 0x24,
    0x1f, 0x9e, 0xa2, 0x16, 0x90, 0xb8, 0xf0, 0xa0, 0xbc, 0x6f, 0xd9, 0xe2,
    0xf7, 0x68, 0xf0, 0x9a, 0xfd, 0x92, 0x73, 0xaa, 0xa6, 0xca, 0x0e, 0x86,
    0x1b, 0x11, 0xec, 0xc7, 0xfc, 0xb7, 0x42, 0x32, 0xb2, 0xea, 0x10, 0xad,
    0x81, 0x90, 0x13, 0xac, 0xbf, 0xdb, 0x48, 0x9f, 0xe9, 0xed, 0x9e, 0x4c,
    0x4e, 0x49, 0xbe, 0xa0, 0xb9, 0x95, 0xe8, 0x76, 0x8f, 0xd9, 0xac, 0x2b,
    0x8a,
};

static void write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8U) & 0xFFU);
    out[2] = (uint8_t)((value >> 16U) & 0xFFU);
    out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t make_manifest(
    uint8_t *out,
    size_t capacity,
    const uint8_t *certificate,
    uint32_t certificate_length,
    const uint8_t *key,
    uint32_t key_length,
    const char *hostname,
    const uint8_t *digest,
    uint32_t purpose_id,
    uint32_t algorithm_id,
    uint32_t entry_count,
    uint32_t version,
    uint32_t magic)
{
    size_t hostname_length;
    uint32_t offset;

    memset(out, 0, capacity);
    write_u32(&out[0], magic);
    write_u32(&out[4], version);
    write_u32(&out[8], entry_count);
    write_u32(&out[12], purpose_id);
    write_u32(&out[16], algorithm_id);
    hostname_length = strlen(hostname);
    out[20] = (uint8_t)(hostname_length & 0xFFU);
    out[21] = (uint8_t)((hostname_length >> 8U) & 0xFFU);
    memcpy(&out[22], hostname, hostname_length);
    write_u32(&out[86], certificate_length);
    write_u32(&out[90], key_length);
    if (digest != NULL) {
        memcpy(&out[94], digest, HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH);
    }

    offset = HFOC_MBEDTLS_MANIFEST_HEADER_BYTES;
    memcpy(&out[offset], certificate, certificate_length);
    offset += certificate_length;
    memcpy(&out[offset], key, key_length);
    return offset + key_length;
}

static uint32_t valid_manifest(uint8_t *out)
{
    uint8_t digest[HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH];

    mbedtls_sha256(
        hfoc_test_cert_der,
        sizeof(hfoc_test_cert_der),
        digest,
        0);
    return make_manifest(
        out,
        HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES,
        hfoc_test_cert_der,
        (uint32_t)sizeof(hfoc_test_cert_der),
        hfoc_test_key_der,
        (uint32_t)sizeof(hfoc_test_key_der),
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
}

static bool snapshot_equal(
    const struct hfoc_mbedtls_alloc_snapshot *left,
    const struct hfoc_mbedtls_alloc_snapshot *right)
{
    return left != NULL &&
           right != NULL &&
           left->total_used_bytes == right->total_used_bytes &&
           left->total_free_bytes == right->total_free_bytes &&
           left->total_high_water_bytes == right->total_high_water_bytes;
}

static void test_strings_and_null(void)
{
    struct hfoc_mbedtls_manifest manifest;
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    uint32_t length;
    enum hfoc_mbedtls_manifest_status status;

    for (status = HFOC_MBEDTLS_MANIFEST_OK;
         status <= HFOC_MBEDTLS_MANIFEST_LAST_STATUS;
         ++status) {
        CHECK(hfoc_mbedtls_manifest_status_string(status) != NULL,
              "manifest status string is defined for every status");
    }
    CHECK(hfoc_mbedtls_manifest_status_string(
              (enum hfoc_mbedtls_manifest_status)999U) == NULL,
          "manifest undefined status string is rejected");

    length = valid_manifest(bytes);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(NULL, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_NULL,
          "manifest parse rejects null output");
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, NULL, length) ==
              HFOC_MBEDTLS_MANIFEST_NULL,
          "manifest parse rejects null bytes");
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, 0U) ==
              HFOC_MBEDTLS_MANIFEST_BAD_TOTAL_LENGTH,
          "manifest parse rejects zero-length blob");
}

static void test_header_rejections(void)
{
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    struct hfoc_mbedtls_manifest manifest;
    uint32_t length;

    length = valid_manifest(bytes);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(
              &manifest, bytes,
              HFOC_MBEDTLS_MANIFEST_HEADER_BYTES - 1U) ==
              HFOC_MBEDTLS_MANIFEST_BAD_TOTAL_LENGTH,
          "manifest parse rejects short blob");
    CHECK(hfoc_mbedtls_manifest_parse(
              &manifest, bytes,
              HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES + 1U) ==
              HFOC_MBEDTLS_MANIFEST_BAD_TOTAL_LENGTH,
          "manifest parse rejects oversized blob");

    bytes[0] ^= 0x01U;
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_MAGIC,
          "manifest parse rejects bad magic");

    length = valid_manifest(bytes);
    write_u32(&bytes[4], 0U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_VERSION,
          "manifest parse rejects bad version");

    length = valid_manifest(bytes);
    write_u32(&bytes[8], 0U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_ENTRY_COUNT,
          "manifest parse rejects zero entry count");

    length = valid_manifest(bytes);
    write_u32(&bytes[8], 2U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_ENTRY_COUNT,
          "manifest parse rejects multiple entries");

    length = valid_manifest(bytes);
    write_u32(&bytes[12], 0U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_PURPOSE,
          "manifest parse rejects zero purpose");

    length = valid_manifest(bytes);
    write_u32(&bytes[12], UINT32_C(0xFFFFFFFF));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_PURPOSE,
          "manifest parse rejects over-limit purpose");

    length = valid_manifest(bytes);
    write_u32(&bytes[16], 0U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_ALGORITHM,
          "manifest parse rejects zero algorithm");

    length = valid_manifest(bytes);
    write_u32(&bytes[16], UINT32_C(0xFFFFFFFF));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_ALGORITHM,
          "manifest parse rejects over-limit algorithm");

    length = valid_manifest(bytes);
    bytes[20] = 8U;
    bytes[21] = 0U;
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_HOSTNAME_LENGTH,
          "manifest parse rejects short hostname length");

    length = valid_manifest(bytes);
    bytes[20] = 10U;
    bytes[21] = 0U;
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_HOSTNAME_LENGTH,
          "manifest parse rejects long hostname length");

    length = valid_manifest(bytes);
    memcpy(&bytes[22], "wrong.com", 9U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_HOSTNAME,
          "manifest parse rejects wrong hostname bytes");
}

static void test_lengths_digest_and_trailing(void)
{
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    uint8_t digest[HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH];
    struct hfoc_mbedtls_manifest manifest;
    uint32_t length;

    memset(&digest, 0x11, sizeof(digest));
    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_test_cert_der,
        0U,
        hfoc_test_key_der,
        (uint32_t)sizeof(hfoc_test_key_der),
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_CERTIFICATE_LENGTH,
          "manifest parse rejects zero certificate length");

    length = valid_manifest(bytes);
    write_u32(&bytes[86], HFOC_MBEDTLS_MANIFEST_CERTIFICATE_MAX + 1U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_CERTIFICATE_LENGTH,
          "manifest parse rejects oversized certificate length");

    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_test_cert_der,
        (uint32_t)sizeof(hfoc_test_cert_der),
        hfoc_test_key_der,
        0U,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_KEY_LENGTH,
          "manifest parse rejects zero key length");

    length = valid_manifest(bytes);
    write_u32(&bytes[90], HFOC_MBEDTLS_MANIFEST_KEY_MAX + 1U);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_KEY_LENGTH,
          "manifest parse rejects oversized key length");

    length = valid_manifest(bytes);
    bytes[length] = 0xEE;
    CHECK(hfoc_mbedtls_manifest_parse(
              &manifest, bytes, length + 1U) ==
              HFOC_MBEDTLS_MANIFEST_TRAILING_BYTES,
          "manifest parse rejects trailing bytes");

    length = valid_manifest(bytes);
    memset(&bytes[94], 0xCC, HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_DIGEST,
          "manifest parse rejects digest mismatch");
}

static void test_der_validation(void)
{
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    uint8_t digest[HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH];
    struct hfoc_mbedtls_manifest manifest;
    uint32_t length;

    mbedtls_sha256(
        hfoc_test_cert_der,
        sizeof(hfoc_test_cert_der) - 1U,
        digest,
        0);
    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_test_cert_der,
        (uint32_t)sizeof(hfoc_test_cert_der) - 1U,
        hfoc_test_key_der,
        (uint32_t)sizeof(hfoc_test_key_der),
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_CERTIFICATE_PARSE_FAILED,
          "manifest parse rejects truncated certificate DER");

    mbedtls_sha256(
        hfoc_test_cert_der,
        sizeof(hfoc_test_cert_der),
        digest,
        0);
    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_test_cert_der,
        (uint32_t)sizeof(hfoc_test_cert_der),
        hfoc_test_key_der,
        (uint32_t)sizeof(hfoc_test_key_der) - 1U,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_KEY_PARSE_FAILED,
          "manifest parse rejects truncated key DER");

    mbedtls_sha256(
        hfoc_other_cert_der,
        sizeof(hfoc_other_cert_der),
        digest,
        0);
    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_other_cert_der,
        (uint32_t)sizeof(hfoc_other_cert_der),
        hfoc_other_key_der,
        (uint32_t)sizeof(hfoc_other_key_der),
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_CERTIFICATE_HOSTNAME_MISMATCH,
          "manifest parse rejects certificate without hfoc.test SAN");

    mbedtls_sha256(
        hfoc_test_cert_der,
        sizeof(hfoc_test_cert_der),
        digest,
        0);
    length = make_manifest(
        bytes,
        sizeof(bytes),
        hfoc_test_cert_der,
        (uint32_t)sizeof(hfoc_test_cert_der),
        hfoc_other_key_der,
        (uint32_t)sizeof(hfoc_other_key_der),
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        digest,
        HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE,
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256,
        1U,
        HFOC_MBEDTLS_MANIFEST_VERSION,
        HFOC_MBEDTLS_MANIFEST_MAGIC);
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_KEY_MISMATCH,
          "manifest parse rejects mismatched private key");
}

static void test_valid_and_fail_closed(void)
{
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    struct hfoc_mbedtls_manifest manifest;
    uint32_t length;
    uint32_t certificate_length = (uint32_t)sizeof(hfoc_test_cert_der);
    uint32_t key_length = (uint32_t)sizeof(hfoc_test_key_der);

    length = valid_manifest(bytes);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_OK,
          "valid manifest parses");
    CHECK(manifest.published,
          "valid manifest publishes");
    CHECK(manifest.version == HFOC_MBEDTLS_MANIFEST_VERSION &&
              manifest.purpose_id ==
                  HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE &&
              manifest.algorithm_id ==
                  HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256 &&
              manifest.hostname_length ==
                  HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH,
          "valid manifest publishes exact metadata");
    CHECK(memcmp(manifest.hostname,
                 HFOC_MBEDTLS_MANIFEST_HOSTNAME,
                 HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH) == 0,
          "valid manifest publishes hfoc.test hostname");
    CHECK(manifest.certificate == bytes +
              HFOC_MBEDTLS_MANIFEST_HEADER_BYTES &&
              manifest.certificate_length == certificate_length &&
              manifest.key == bytes +
                  HFOC_MBEDTLS_MANIFEST_HEADER_BYTES +
                  certificate_length &&
              manifest.key_length == key_length,
          "valid manifest publishes bounded DER pointers");

    length = valid_manifest(bytes);
    manifest.published = true;
    bytes[0] ^= 0x01U;
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_MAGIC &&
              manifest.published,
          "failed manifest parse leaves output unchanged");
}

static void test_allocator_balance(void)
{
    uint8_t bytes[HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES];
    struct hfoc_mbedtls_alloc_snapshot initial;
    struct hfoc_mbedtls_alloc_snapshot after;
    struct hfoc_mbedtls_manifest manifest;
    uint32_t length;

    initial = hfoc_mbedtls_alloc_snapshot();
    length = valid_manifest(bytes);
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_OK,
          "allocator fixture parses valid manifest");
    after = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot_equal(&initial, &after),
          "valid manifest parse restores allocator counts");

    bytes[0] ^= 0x01U;
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hfoc_mbedtls_manifest_parse(&manifest, bytes, length) ==
              HFOC_MBEDTLS_MANIFEST_BAD_MAGIC,
          "allocator fixture rejects bad manifest");
    after = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot_equal(&initial, &after),
          "failed manifest parse restores allocator counts");
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: mbedTLS allocator init\n");
        return 1;
    }

    test_strings_and_null();
    test_header_rejections();
    test_lengths_digest_and_trailing();
    test_der_validation();
    test_valid_and_fail_closed();
    test_allocator_balance();

    hfoc_mbedtls_alloc_shutdown();

    if (failures != 0) {
        fprintf(stderr,
                "mbedTLS manifest parser host tests: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("[CERTIFICATE-FAIL-CLOSED] matrix ok\n");
    printf("mbedTLS manifest parser host tests: ALL OK\n");
    return 0;
}
