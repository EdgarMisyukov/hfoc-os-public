#ifndef HFOC_FAULT_INJECTION_H
#define HFOC_FAULT_INJECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAULT_INJECTION_VERSION 1U
#define FAULT_INJECTION_CATALOGUE_CAPACITY 24U
#define FAULT_INJECTION_ACTIVATION_VERSION 1U
#define FAULT_INJECTION_SEQUENCE_MAX 1000000U

enum fault_injection_subsystem {
    FAULT_INJECTION_SUBSYSTEM_UNKNOWN = 0,
    FAULT_INJECTION_SUBSYSTEM_STORAGE = 1,
    FAULT_INJECTION_SUBSYSTEM_SERVICE = 2,
    FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL = 3,
    FAULT_INJECTION_SUBSYSTEM_NETWORK = 4,
    FAULT_INJECTION_SUBSYSTEM_TRUST = 5,
    FAULT_INJECTION_SUBSYSTEM_UPDATE = 6,
    FAULT_INJECTION_SUBSYSTEM_INTEGRATION = 7,
    FAULT_INJECTION_SUBSYSTEM_SCHEDULER = 8,
};

enum fault_injection_id {
    FAULT_INJECTION_BLOCK_DEVICE_READ = 1,
    FAULT_INJECTION_BLOCK_DEVICE_WRITE = 2,
    FAULT_INJECTION_CONFIG_JOURNAL_WRITE = 3,
    FAULT_INJECTION_CONFIG_JOURNAL_READBACK = 4,
    FAULT_INJECTION_RESTORE_WRITE = 5,
    FAULT_INJECTION_SERVICE_DEPENDENCY = 6,
    FAULT_INJECTION_SERVICE_RESTART_EXHAUSTION = 7,
    FAULT_INJECTION_SERVICE_CONTROLLED_STOP = 8,
    FAULT_INJECTION_LOCAL_SESSION_EXPIRY = 9,
    FAULT_INJECTION_AUTH_THROTTLE = 10,
    FAULT_INJECTION_LOCAL_MALFORMED_COMMAND = 11,
    FAULT_INJECTION_NETWORK_LINK_LOSS = 12,
    FAULT_INJECTION_NETWORK_PEER_SILENCE = 13,
    FAULT_INJECTION_TRUST_REVOKED_SIGNER = 14,
    FAULT_INJECTION_TRUST_TAMPERED_METADATA = 15,
    FAULT_INJECTION_TRUST_RECOVERY_PURPOSE = 16,
    FAULT_INJECTION_UPDATE_STAGED_WRITE = 17,
    FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH = 18,
    FAULT_INJECTION_INTEGRATION_DUPLICATE = 19,
    FAULT_INJECTION_INTEGRATION_MALFORMED = 20,
    FAULT_INJECTION_INTEGRATION_INCOMPATIBLE = 21,
    FAULT_INJECTION_INTEGRATION_UNAVAILABLE = 22,
    FAULT_INJECTION_SCHEDULER_SPAWN_NO_MEMORY = 23,
    FAULT_INJECTION_SCHEDULER_BAD_CONTEXT = 24,
};

enum fault_injection_status {
    FAULT_INJECTION_OK = 0,
    FAULT_INJECTION_NULL = 1,
    FAULT_INJECTION_BAD_VERSION = 2,
    FAULT_INJECTION_BAD_ID = 3,
    FAULT_INJECTION_BAD_SEQUENCE = 4,
    FAULT_INJECTION_NOT_PERMITTED = 5,
    FAULT_INJECTION_OUT_OF_RANGE = 6,
};

struct fault_injection_site {
    enum fault_injection_id id;
    const char *name;
    enum fault_injection_subsystem subsystem;
    const char *expected_outcome;
    bool bounded;
    bool test_only;
};

struct fault_injection_activation {
    uint32_t version;
    enum fault_injection_id id;
    uint32_t sequence;
};

size_t fault_injection_catalogue_count(void);
enum fault_injection_status fault_injection_catalogue_at(
    size_t index,
    const struct fault_injection_site **site_out);
enum fault_injection_status fault_injection_site_by_id(
    enum fault_injection_id id,
    const struct fault_injection_site **site_out);
const char *fault_injection_subsystem_string(
    enum fault_injection_subsystem subsystem);
const char *fault_injection_status_string(
    enum fault_injection_status status);
const char *fault_injection_id_string(enum fault_injection_id id);

#ifdef HFOC_FAULT_INJECTION_HOST_TEST
enum fault_injection_status fault_injection_activation_validate(
    const struct fault_injection_activation *activation);
#endif

#endif
