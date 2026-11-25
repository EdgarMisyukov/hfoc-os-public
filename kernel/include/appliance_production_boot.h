#ifndef HFOC_APPLIANCE_PRODUCTION_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_BOOT_H

#include "appliance_state_loader.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY 16U

enum appliance_production_boot_backend_kind {
    APPLIANCE_PRODUCTION_BOOT_BACKEND_NONE = 0,
    APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI = 1,
    APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME = 2,
};

enum appliance_production_boot_probe_status {
    APPLIANCE_PRODUCTION_BOOT_PROBE_OK = 0,
    APPLIANCE_PRODUCTION_BOOT_PROBE_UNAVAILABLE = 1,
    APPLIANCE_PRODUCTION_BOOT_PROBE_IO_ERROR = 2,
    APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY = 3,
};

enum appliance_production_boot_status {
    APPLIANCE_PRODUCTION_BOOT_OK = 0,
    APPLIANCE_PRODUCTION_BOOT_NULL = 1,
    APPLIANCE_PRODUCTION_BOOT_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_BOOT_PROBE_FAILED = 3,
    APPLIANCE_PRODUCTION_BOOT_NO_BACKEND = 4,
    APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND = 5,
    APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED = 6,
    APPLIANCE_PRODUCTION_BOOT_UNATTESTED = 7,
    APPLIANCE_PRODUCTION_BOOT_STATE_LOCKED = 8,
    APPLIANCE_PRODUCTION_BOOT_STATE_KEY_REJECTED = 9,
};

/* ITEM-372 boot-attestation gate.  A provisioned signed boot chain that
 * did not verify refuses normal production boot; media without a signed
 * chain report NONE and never claim a verified chain. */
enum appliance_production_boot_attestation {
    APPLIANCE_PRODUCTION_BOOT_ATTESTATION_NONE = 0,
    APPLIANCE_PRODUCTION_BOOT_ATTESTATION_TRUSTED = 1,
    APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED = 2,
};

enum appliance_production_boot_readiness {
    APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE = 0,
    APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED = 1,
    APPLIANCE_PRODUCTION_BOOT_READINESS_READY = 2,
};

enum appliance_production_boot_mode {
    APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED = 0,
    APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED = 1,
    APPLIANCE_PRODUCTION_BOOT_MODE_NORMAL = 2,
};

struct appliance_production_boot_backend {
    enum appliance_production_boot_backend_kind kind;
    uint64_t identity;
    struct block_device device;
};

typedef enum appliance_production_boot_probe_status
(*appliance_production_boot_probe_fn)(
    void *context,
    struct appliance_production_boot_backend *backends,
    size_t capacity,
    size_t *count);

struct appliance_production_boot {
    bool initialized;
    enum appliance_production_boot_status status;
    enum appliance_production_boot_readiness readiness;
    enum appliance_production_boot_mode mode;
    size_t discovered_backend_count;
    size_t valid_backend_count;
    enum appliance_production_boot_backend_kind selected_kind;
    uint64_t selected_identity;
    enum appliance_production_boot_attestation attestation;
    const char *attestation_detail;
    struct appliance_state_loader state;
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    /* ITEM-373: the caller-bound key provider.  Ordinary boot leaves it
     * null, so a sealed state volume publishes LOCKED instead of ready. */
    const struct appliance_key_custody_material *state_key;
#endif
};

enum appliance_production_boot_status appliance_production_boot_init(
    struct appliance_production_boot *boot);
enum appliance_production_boot_status appliance_production_boot_run(
    struct appliance_production_boot *boot,
    appliance_production_boot_probe_fn probe,
    void *probe_context);
enum appliance_production_boot_status
appliance_production_boot_set_state_key(
    struct appliance_production_boot *boot,
    const struct appliance_key_custody_material *material);
enum appliance_production_boot_status
appliance_production_boot_set_attestation(
    struct appliance_production_boot *boot,
    enum appliance_production_boot_attestation attestation,
    const char *detail);
enum appliance_production_boot_attestation
appliance_production_boot_attestation(
    const struct appliance_production_boot *boot);
const char *appliance_production_boot_attestation_detail(
    const struct appliance_production_boot *boot);
enum appliance_production_boot_readiness appliance_production_boot_readiness(
    const struct appliance_production_boot *boot);
enum appliance_production_boot_mode appliance_production_boot_mode(
    const struct appliance_production_boot *boot);
const struct appliance_state_loader *
appliance_production_boot_state_loader(
    const struct appliance_production_boot *boot);
uint32_t appliance_production_boot_configured_backend_count(
    const struct appliance_production_boot *boot);
const char *appliance_production_boot_backend_kind_string(
    enum appliance_production_boot_backend_kind kind);
const char *appliance_production_boot_readiness_string(
    enum appliance_production_boot_readiness readiness);
const char *appliance_production_boot_mode_string(
    enum appliance_production_boot_mode mode);
const char *appliance_production_boot_status_string(
    enum appliance_production_boot_status status);

#endif
