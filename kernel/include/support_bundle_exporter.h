#ifndef HFOC_SUPPORT_BUNDLE_EXPORTER_H
#define HFOC_SUPPORT_BUNDLE_EXPORTER_H

#include "sha256.h"
#include "support_bundle_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUPPORT_BUNDLE_EXPORTER_VERSION 1U
#define SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID "HFOC-SB-EXPORT-1"
#define SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_LENGTH 16U
#define SUPPORT_BUNDLE_EXPORTER_MAGIC_SIZE 8U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE 64U
#define SUPPORT_BUNDLE_EXPORTER_MANIFEST_SIZE \
    SUPPORT_BUNDLE_MANIFEST_SIZE
#define SUPPORT_BUNDLE_EXPORTER_DIGEST_SIZE SHA256_DIGEST_SIZE
/* Published stream layout so a seal can parse the exported bytes. */
#define SUPPORT_BUNDLE_EXPORTER_VERSION_OFFSET 8U
#define SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID_OFFSET 16U
#define SUPPORT_BUNDLE_EXPORTER_MANIFEST_OFFSET \
    SUPPORT_BUNDLE_EXPORTER_HEADER_SIZE
#define SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_SIZE_OFFSET 32U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_ENTRY_COUNT_OFFSET 36U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_EXPORTER_VERSION_OFFSET 40U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_MANIFEST_VERSION_OFFSET 44U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_PAYLOAD_BYTES_OFFSET 48U
#define SUPPORT_BUNDLE_EXPORTER_HEADER_TOTAL_BYTES_OFFSET 56U

enum support_bundle_exporter_status {
    SUPPORT_BUNDLE_EXPORTER_OK = 0,
    SUPPORT_BUNDLE_EXPORTER_NULL = 1,
    SUPPORT_BUNDLE_EXPORTER_BAD_STATE = 2,
    SUPPORT_BUNDLE_EXPORTER_BAD_MANIFEST = 3,
    SUPPORT_BUNDLE_EXPORTER_TOO_SMALL = 4,
    SUPPORT_BUNDLE_EXPORTER_DONE = 5,
};

struct support_bundle_exporter {
    const struct support_bundle_manifest_source *source;
    unsigned char manifest[SUPPORT_BUNDLE_MANIFEST_SIZE];
    struct sha256 digest;
    uint32_t phase;
    uint64_t payload_bytes;
    uint64_t total_bytes;
    bool active;
    bool done;
    bool closed;
};

enum support_bundle_exporter_status support_bundle_exporter_init(
    struct support_bundle_exporter *exporter,
    const struct support_bundle_manifest_source *source);
enum support_bundle_exporter_status support_bundle_exporter_next(
    struct support_bundle_exporter *exporter,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum support_bundle_exporter_status support_bundle_exporter_close(
    struct support_bundle_exporter *exporter);
const char *support_bundle_exporter_status_string(
    enum support_bundle_exporter_status status);

#endif
