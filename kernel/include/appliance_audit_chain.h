#ifndef HFOC_APPLIANCE_AUDIT_CHAIN_H
#define HFOC_APPLIANCE_AUDIT_CHAIN_H

#include "audit.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-377 hash-chained audit records.
 *
 * `audit.c` gives every record a sequence number, but nothing binds a record
 * to the record before it: a rewritten or reordered record is
 * indistinguishable from an original one.  This module owns the tamper-evident
 * chain.
 *
 * Every entry stores one `struct audit_record` together with its chain digest
 *
 *     digest(0) = SHA-256("HFOC-AUDIT-CHAIN-V1" || generation_le64)
 *     digest(n) = SHA-256("HFOC-AUDIT-LINK-V1" || sequence_le64 ||
 *                         digest(n-1) || canonical(record))
 *
 * so verification recomputes the whole retained window and reports the exact
 * sequence of the first entry that no longer matches.  The canonical record
 * encoding is fixed-width little-endian and covers every field of the record
 * that is stored and exported, including the record's own sequence copy, so a
 * digest cannot depend on struct padding or on the host's byte order and a
 * rewritten stored record is detected.
 *
 * The window is bounded and rotates: rotation retires the oldest entries and
 * moves the running anchor digest forward, so the retained window still
 * verifies, the retired prefix can never be silently replayed, and the cost of
 * the chain never depends on uptime.  The anchor of an empty rotated chain is
 * the head digest, so appends after a full rotation continue the same chain.
 *
 * Honest limits of this module, all published rather than hidden:
 *
 *   - the chain lives in caller-owned memory; the frozen baseline has no
 *     persistent audit store, so a reboot starts a new chain and physical
 *     tamper of stored bytes is out of scope;
 *   - a rotated prefix cannot be recomputed from the window alone: the anchor
 *     digest is the evidence for everything that was retired, and the verdict
 *     reports that the window is anchored instead of claiming a genesis
 *     verification.
 *
 * The module is allocation-free, never touches a device, and is fully
 * host-testable.
 */

#define APPLIANCE_AUDIT_CHAIN_VERSION 1U
#define APPLIANCE_AUDIT_CHAIN_CAPACITY 32U
#define APPLIANCE_AUDIT_CHAIN_RENDER_MAX 256U
#define APPLIANCE_AUDIT_CHAIN_GENESIS_TAG "HFOC-AUDIT-CHAIN-V1"
#define APPLIANCE_AUDIT_CHAIN_LINK_TAG "HFOC-AUDIT-LINK-V1"
#define APPLIANCE_AUDIT_CHAIN_CANONICAL_SIZE 52U

enum appliance_audit_chain_status {
    APPLIANCE_AUDIT_CHAIN_OK = 0,
    APPLIANCE_AUDIT_CHAIN_NULL = 1,
    APPLIANCE_AUDIT_CHAIN_BAD_VERSION = 2,
    APPLIANCE_AUDIT_CHAIN_BAD_STATE = 3,
    APPLIANCE_AUDIT_CHAIN_INVALID_RECORD = 4,
    APPLIANCE_AUDIT_CHAIN_FULL = 5,
    APPLIANCE_AUDIT_CHAIN_EMPTY = 6,
    APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT = 7,
    APPLIANCE_AUDIT_CHAIN_SHA_FAILED = 8,
    APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL = 9,
};

enum appliance_audit_chain_state {
    APPLIANCE_AUDIT_CHAIN_STATE_NONE = 0,
    APPLIANCE_AUDIT_CHAIN_STATE_EMPTY = 1,
    APPLIANCE_AUDIT_CHAIN_STATE_OK = 2,
    APPLIANCE_AUDIT_CHAIN_STATE_TAMPERED = 3,
};

struct appliance_audit_chain_entry {
    uint64_t sequence;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
};

struct appliance_audit_chain {
    bool initialized;
    uint32_t version;
    uint64_t generation;
    uint64_t next_sequence;
    uint64_t first_sequence;
    uint64_t appended_count;
    uint64_t rotated_count;
    uint32_t count;
    bool anchor_is_genesis;
    uint8_t anchor_digest[SHA256_DIGEST_SIZE];
    struct appliance_audit_chain_entry
        entries[APPLIANCE_AUDIT_CHAIN_CAPACITY];
};

struct appliance_audit_chain_verdict {
    uint32_t version;
    bool chain_ok;
    bool tamper_detected;
    bool anchored;
    uint64_t generation;
    uint64_t first_sequence;
    uint64_t head_sequence;
    uint64_t checked_count;
    uint64_t tamper_sequence;
    uint8_t head_digest[SHA256_DIGEST_SIZE];
};

struct appliance_audit_chain_snapshot {
    uint32_t version;
    bool published;
    enum appliance_audit_chain_state state;
    uint64_t generation;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint64_t appended_count;
    uint64_t rotated_count;
    uint32_t count;
    uint32_t capacity;
    bool anchor_is_genesis;
    uint8_t head_digest[SHA256_DIGEST_SIZE];
};

enum appliance_audit_chain_status appliance_audit_chain_init(
    struct appliance_audit_chain *chain,
    uint64_t generation);
enum appliance_audit_chain_status appliance_audit_chain_append(
    struct appliance_audit_chain *chain,
    const struct audit_record *record,
    uint8_t digest_out[SHA256_DIGEST_SIZE]);
enum appliance_audit_chain_status appliance_audit_chain_verify(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_chain_verdict *verdict_out);
enum appliance_audit_chain_status appliance_audit_chain_rotate(
    struct appliance_audit_chain *chain,
    uint32_t retire_count,
    struct appliance_audit_chain_verdict *verdict_out);
enum appliance_audit_chain_status appliance_audit_chain_snapshot(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_chain_snapshot *snapshot_out);
/* Returns false when the window does not verify: a tampered chain has no head
 * digest to publish. */
bool appliance_audit_chain_head_digest(
    const struct appliance_audit_chain *chain,
    uint8_t digest_out[SHA256_DIGEST_SIZE]);
uint32_t appliance_audit_chain_count(
    const struct appliance_audit_chain *chain);
uint64_t appliance_audit_chain_first_sequence(
    const struct appliance_audit_chain *chain);
uint64_t appliance_audit_chain_next_sequence(
    const struct appliance_audit_chain *chain);
uint64_t appliance_audit_chain_generation(
    const struct appliance_audit_chain *chain);
bool appliance_audit_chain_find(
    const struct appliance_audit_chain *chain,
    uint64_t sequence,
    struct audit_record *record_out,
    uint8_t digest_out[SHA256_DIGEST_SIZE]);
enum appliance_audit_chain_status appliance_audit_chain_render_snapshot(
    const struct appliance_audit_chain_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_audit_chain_status appliance_audit_chain_render_verdict(
    const struct appliance_audit_chain_verdict *verdict,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_audit_chain_status_string(
    enum appliance_audit_chain_status status);
const char *appliance_audit_chain_state_string(
    enum appliance_audit_chain_state state);

#ifdef HFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST
/* Corrupt one retained record in place, or the anchor, to prove detection. */
bool appliance_audit_chain_host_test_tamper(
    struct appliance_audit_chain *chain,
    uint32_t index);
bool appliance_audit_chain_host_test_tamper_anchor(
    struct appliance_audit_chain *chain);
#endif

#endif
