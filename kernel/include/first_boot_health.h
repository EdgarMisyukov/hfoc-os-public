#ifndef HFOC_FIRST_BOOT_HEALTH_H
#define HFOC_FIRST_BOOT_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIRST_BOOT_HEALTH_VERSION 1U
#define FIRST_BOOT_HEALTH_MAX_MILESTONES 16U
#define FIRST_BOOT_HEALTH_SUBSYSTEM_COUNT 4U
#define FIRST_BOOT_HEALTH_MILESTONE_ID_MIN 1U
#define FIRST_BOOT_HEALTH_MILESTONE_ID_MAX 16U

enum first_boot_health_subsystem {
    FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT = 1,
    FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE = 2,
    FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE = 3,
    FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT = 4,
};

enum first_boot_health_status {
    FIRST_BOOT_HEALTH_OK = 0,
    FIRST_BOOT_HEALTH_NULL = 1,
    FIRST_BOOT_HEALTH_BAD_STATE = 2,
    FIRST_BOOT_HEALTH_BAD_REQUEST = 3,
    FIRST_BOOT_HEALTH_BAD_SUBSYSTEM = 4,
    FIRST_BOOT_HEALTH_BAD_MILESTONE = 5,
    FIRST_BOOT_HEALTH_DUPLICATE = 6,
    FIRST_BOOT_HEALTH_UNKNOWN = 7,
    FIRST_BOOT_HEALTH_NOT_READY = 8,
    FIRST_BOOT_HEALTH_FAILED = 9,
    FIRST_BOOT_HEALTH_ALREADY_DEFINED = 10,
    FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM = 11,
};

struct first_boot_health_milestone {
    enum first_boot_health_subsystem subsystem;
    uint32_t milestone_id;
};

struct first_boot_health_requirements {
    uint32_t version;
    size_t milestone_count;
    struct first_boot_health_milestone
        milestones[FIRST_BOOT_HEALTH_MAX_MILESTONES];
};

struct first_boot_health_report {
    uint32_t version;
    struct first_boot_health_milestone milestone;
    bool healthy;
};

struct first_boot_health_verdict {
    bool ready;
    bool failed;
    size_t required_count;
    size_t observed_count;
    size_t missing_count;
    struct first_boot_health_milestone first_missing;
};

struct first_boot_health_store {
    uint32_t version;
    struct first_boot_health_requirements requirements;
    bool observed[FIRST_BOOT_HEALTH_MAX_MILESTONES];
    bool healthy[FIRST_BOOT_HEALTH_MAX_MILESTONES];
    size_t observed_count;
    bool failed;
    bool initialized;
};

enum first_boot_health_status first_boot_health_init(
    struct first_boot_health_store *store);
enum first_boot_health_status first_boot_health_define(
    struct first_boot_health_store *store,
    const struct first_boot_health_requirements *requirements);
enum first_boot_health_status first_boot_health_report(
    struct first_boot_health_store *store,
    const struct first_boot_health_report *report);
enum first_boot_health_status first_boot_health_evaluate(
    const struct first_boot_health_store *store,
    struct first_boot_health_verdict *verdict_out);
const char *first_boot_health_status_string(
    enum first_boot_health_status status);

#endif
