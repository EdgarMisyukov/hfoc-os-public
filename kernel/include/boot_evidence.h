#ifndef HFOC_BOOT_EVIDENCE_H
#define HFOC_BOOT_EVIDENCE_H

#include "config_kv.h"
#include "image_policy.h"
#include "sha256.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_EVIDENCE_VERSION 1U
#define BOOT_EVIDENCE_KEY "boot.evidence"
#define BOOT_EVIDENCE_KEY_LEN 13U
#define BOOT_EVIDENCE_RECORD_SIZE 96U
#define BOOT_EVIDENCE_DIGEST_INPUT_SIZE 64U
#define BOOT_EVIDENCE_DIGEST_OFFSET 64U

enum boot_evidence_status {
    BOOT_EVIDENCE_OK = 0,
    BOOT_EVIDENCE_NULL = 1,
    BOOT_EVIDENCE_BAD_STATE = 2,
    BOOT_EVIDENCE_NO_POLICY = 3,
    BOOT_EVIDENCE_NO_IMAGE = 4,
    BOOT_EVIDENCE_POLICY_MISMATCH = 5,
    BOOT_EVIDENCE_INVALID_RECORD = 6,
    BOOT_EVIDENCE_DIGEST_MISMATCH = 7,
    BOOT_EVIDENCE_STORAGE_FAILED = 8,
    BOOT_EVIDENCE_NOT_FOUND = 9,
};

struct boot_evidence {
    uint32_t version;
    uint32_t policy_generation;
    uint32_t image_id;
    uint8_t measurement[SHA256_DIGEST_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    bool digest_match;
    bool published;
};

enum boot_evidence_status boot_evidence_init(
    struct boot_evidence *evidence,
    const struct verification_policy *policy,
    const struct image_policy_binding *binding,
    const uint8_t measurement[SHA256_DIGEST_SIZE]);
enum boot_evidence_status boot_evidence_save(
    const struct boot_evidence *evidence,
    struct config_kv *audit_state);
enum boot_evidence_status boot_evidence_load(
    struct boot_evidence *evidence,
    const struct config_kv *audit_state);
enum boot_evidence_status boot_evidence_check_policy(
    const struct boot_evidence *evidence,
    const struct verification_policy *policy);
bool boot_evidence_is_published(
    const struct boot_evidence *evidence);
const char *boot_evidence_status_string(
    enum boot_evidence_status status);

#endif
