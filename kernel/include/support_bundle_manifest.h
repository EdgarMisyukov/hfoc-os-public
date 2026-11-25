#ifndef HFOC_SUPPORT_BUNDLE_MANIFEST_H
#define HFOC_SUPPORT_BUNDLE_MANIFEST_H

#include "appliance_status.h"
#include "audit.h"
#include "config_kv.h"
#include "health_check.h"
#include "metrics.h"

#include <stddef.h>
#include <stdint.h>

#define SUPPORT_BUNDLE_MANIFEST_VERSION 1U
#define SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID "HFOC-SB-MANIFEST-1"
#define SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_LENGTH 18U
#define SUPPORT_BUNDLE_MANIFEST_MAGIC_SIZE 8U
#define SUPPORT_BUNDLE_MANIFEST_SIZE 512U
#define SUPPORT_BUNDLE_MANIFEST_DIGEST_SIZE 32U
#define SUPPORT_BUNDLE_MANIFEST_ENTRY_MAX 6U
/* Published layout offsets so a seal can re-derive every field from the
 * exported bytes instead of trusting a parallel copy. */
#define SUPPORT_BUNDLE_MANIFEST_HEADER_SIZE 128U
#define SUPPORT_BUNDLE_MANIFEST_ENTRY_RECORD_SIZE 64U
#define SUPPORT_BUNDLE_MANIFEST_VERSION_OFFSET 8U
#define SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID_OFFSET 16U
#define SUPPORT_BUNDLE_MANIFEST_AUDIT_COUNT_OFFSET 76U
#define SUPPORT_BUNDLE_MANIFEST_CONFIG_INCLUDED_OFFSET 80U
#define SUPPORT_BUNDLE_MANIFEST_CONFIG_EXCLUDED_OFFSET 84U
#define SUPPORT_BUNDLE_MANIFEST_POLICY_ID_OFFSET 112U
#define SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID "HFOCSBPRIV1"
#define SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH 11U
#define SUPPORT_BUNDLE_MANIFEST_SECRET_KEY_COUNT 6U
#define SUPPORT_BUNDLE_AUDIT_SLICE_MAX KERNEL_EVENT_RING_CAPACITY
#define SUPPORT_BUNDLE_CONFIG_METADATA_MAX CONFIG_KV_CAPACITY_MAX

enum support_bundle_manifest_status {
    SUPPORT_BUNDLE_MANIFEST_OK = 0,
    SUPPORT_BUNDLE_MANIFEST_NULL = 1,
    SUPPORT_BUNDLE_MANIFEST_BAD_VERSION = 2,
    SUPPORT_BUNDLE_MANIFEST_BAD_STATE = 3,
    SUPPORT_BUNDLE_MANIFEST_TOO_SMALL = 4,
    SUPPORT_BUNDLE_MANIFEST_BAD_SNAPSHOT = 5,
    SUPPORT_BUNDLE_MANIFEST_BAD_AUDIT = 6,
    SUPPORT_BUNDLE_MANIFEST_BAD_CONFIG = 7,
};

enum support_bundle_entry_type {
    SUPPORT_BUNDLE_ENTRY_NONE = 0,
    SUPPORT_BUNDLE_ENTRY_METRICS = 1,
    SUPPORT_BUNDLE_ENTRY_APPLIANCE_STATUS = 2,
    SUPPORT_BUNDLE_ENTRY_HEALTH = 3,
    SUPPORT_BUNDLE_ENTRY_AUDIT_SLICE = 4,
    SUPPORT_BUNDLE_ENTRY_CONFIG_METADATA = 5,
    SUPPORT_BUNDLE_ENTRY_VERSIONS = 6,
};

struct support_bundle_manifest_source {
    uint32_t version;
    const struct metrics_snapshot *metrics;
    const struct appliance_status_snapshot *status;
    const struct health_check_snapshot *health;
    const struct audit_record *audit_records;
    uint32_t audit_count;
    const struct config_kv *config;
};

enum support_bundle_manifest_status support_bundle_manifest_export(
    const struct support_bundle_manifest_source *source,
    void *buffer,
    size_t capacity,
    size_t *length_out);
const char *support_bundle_manifest_status_string(
    enum support_bundle_manifest_status status);
const char *support_bundle_manifest_entry_type_string(
    enum support_bundle_entry_type type);
const char *support_bundle_manifest_schema_id(void);

#endif
