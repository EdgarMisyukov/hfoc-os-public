#ifndef HFOC_BOOT_MEASUREMENTS_H
#define HFOC_BOOT_MEASUREMENTS_H

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-372 measured-boot register set.
 *
 * A bounded, readable set of SHA-256 measurement registers with extend
 * semantics: extending a register binds it to everything measured into it
 * before, so a value reported later cannot be replayed against a different
 * boot chain.  Every measured component is also appended to a bounded log,
 * so a reader can see which component produced which digest instead of a
 * single opaque aggregate.
 *
 * The register set is caller-owned; the boot chain publishes it only after
 * the signed chain verified, and a failed measurement marks the whole set
 * untrustworthy instead of leaving stale values readable.
 */

#define BOOT_MEASUREMENTS_VERSION 1U
#define BOOT_MEASUREMENTS_REGISTER_COUNT 6U
#define BOOT_MEASUREMENTS_LOG_CAPACITY 8U

enum boot_measurements_status {
    BOOT_MEASUREMENTS_OK = 0,
    BOOT_MEASUREMENTS_NULL = 1,
    BOOT_MEASUREMENTS_BAD_STATE = 2,
    BOOT_MEASUREMENTS_BAD_INDEX = 3,
    BOOT_MEASUREMENTS_CAPACITY = 4,
    BOOT_MEASUREMENTS_DIGEST_FAILED = 5,
};

struct boot_measurements_log_entry {
    uint32_t component_kind;
    uint32_t reserved;
    uint64_t component_length;
    unsigned char digest[SHA256_DIGEST_SIZE];
};

struct boot_measurements {
    uint32_t version;
    bool published;
    bool failed;
    uint32_t register_count;
    unsigned char registers[BOOT_MEASUREMENTS_REGISTER_COUNT]
                           [SHA256_DIGEST_SIZE];
    uint32_t log_count;
    struct boot_measurements_log_entry log[BOOT_MEASUREMENTS_LOG_CAPACITY];
    unsigned char aggregate[SHA256_DIGEST_SIZE];
    bool aggregate_published;
};

/* Reset clears every register and the log.  A measurement set must be reset
 * before a new chain is measured; measuring into a published set is
 * refused. */
enum boot_measurements_status boot_measurements_reset(
    struct boot_measurements *measurements);
enum boot_measurements_status boot_measurements_publish(
    struct boot_measurements *measurements);
enum boot_measurements_status boot_measurements_fail(
    struct boot_measurements *measurements);

/* Measure one component: digest the bytes, extend the selected register
 * with that digest, and append the component to the log. */
enum boot_measurements_status boot_measurements_measure(
    struct boot_measurements *measurements,
    uint32_t register_index,
    uint32_t component_kind,
    const void *data,
    size_t data_length);
/* Record an already-measured component: extend the selected register with
 * the supplied digest and append the component to the log. */
enum boot_measurements_status boot_measurements_record(
    struct boot_measurements *measurements,
    uint32_t register_index,
    uint32_t component_kind,
    uint64_t component_length,
    const unsigned char digest[SHA256_DIGEST_SIZE]);
/* Extend a register with a caller-supplied digest (already measured bytes,
 * for example a signed manifest digest).  No log entry is appended. */
enum boot_measurements_status boot_measurements_extend(
    struct boot_measurements *measurements,
    uint32_t register_index,
    const unsigned char digest[SHA256_DIGEST_SIZE]);

bool boot_measurements_is_published(
    const struct boot_measurements *measurements);
bool boot_measurements_failed(
    const struct boot_measurements *measurements);
uint32_t boot_measurements_register_count(
    const struct boot_measurements *measurements);
enum boot_measurements_status boot_measurements_read(
    const struct boot_measurements *measurements,
    uint32_t register_index,
    unsigned char digest_out[SHA256_DIGEST_SIZE]);
uint32_t boot_measurements_log_count(
    const struct boot_measurements *measurements);
const struct boot_measurements_log_entry *boot_measurements_log_entry(
    const struct boot_measurements *measurements,
    uint32_t index);
enum boot_measurements_status boot_measurements_aggregate(
    const struct boot_measurements *measurements,
    unsigned char digest_out[SHA256_DIGEST_SIZE]);
const char *boot_measurements_status_string(
    enum boot_measurements_status status);

#endif
