#include "first_boot_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void first_boot_health_zero_bytes(char *destination,
                                         size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool first_boot_health_subsystem_valid(
    enum first_boot_health_subsystem subsystem)
{
    return subsystem >= FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT &&
           subsystem <= FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT;
}

static bool first_boot_health_milestone_id_valid(uint32_t milestone_id)
{
    return milestone_id >= FIRST_BOOT_HEALTH_MILESTONE_ID_MIN &&
           milestone_id <= FIRST_BOOT_HEALTH_MILESTONE_ID_MAX;
}

static bool first_boot_health_milestone_equal(
    const struct first_boot_health_milestone *left,
    const struct first_boot_health_milestone *right)
{
    return left->subsystem == right->subsystem &&
           left->milestone_id == right->milestone_id;
}

static enum first_boot_health_status
first_boot_health_requirements_validate(
    const struct first_boot_health_requirements *requirements)
{
    bool subsystem_seen[FIRST_BOOT_HEALTH_SUBSYSTEM_COUNT];
    size_t index;
    size_t previous;

    if (requirements->version != FIRST_BOOT_HEALTH_VERSION) {
        return FIRST_BOOT_HEALTH_BAD_REQUEST;
    }
    if (requirements->milestone_count == 0U ||
        requirements->milestone_count > FIRST_BOOT_HEALTH_MAX_MILESTONES) {
        return FIRST_BOOT_HEALTH_BAD_REQUEST;
    }

    for (index = 0U;
         index < FIRST_BOOT_HEALTH_SUBSYSTEM_COUNT;
         ++index) {
        subsystem_seen[index] = false;
    }
    for (index = 0U;
         index < requirements->milestone_count;
         ++index) {
        const struct first_boot_health_milestone *milestone =
            &requirements->milestones[index];

        if (!first_boot_health_subsystem_valid(milestone->subsystem)) {
            return FIRST_BOOT_HEALTH_BAD_SUBSYSTEM;
        }
        if (!first_boot_health_milestone_id_valid(
                milestone->milestone_id)) {
            return FIRST_BOOT_HEALTH_BAD_MILESTONE;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (first_boot_health_milestone_equal(
                    milestone,
                    &requirements->milestones[previous])) {
                return FIRST_BOOT_HEALTH_DUPLICATE;
            }
        }
        subsystem_seen[
            (unsigned int)milestone->subsystem - 1U] = true;
    }
    for (index = 0U;
         index < FIRST_BOOT_HEALTH_SUBSYSTEM_COUNT;
         ++index) {
        if (!subsystem_seen[index]) {
            return FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM;
        }
    }
    return FIRST_BOOT_HEALTH_OK;
}

static size_t first_boot_health_find_milestone(
    const struct first_boot_health_store *store,
    const struct first_boot_health_milestone *milestone)
{
    size_t index;

    for (index = 0U;
         index < store->requirements.milestone_count;
         ++index) {
        if (first_boot_health_milestone_equal(
                milestone,
                &store->requirements.milestones[index])) {
            return index;
        }
    }
    return (size_t)-1;
}

enum first_boot_health_status first_boot_health_init(
    struct first_boot_health_store *store)
{
    if (store == NULL) {
        return FIRST_BOOT_HEALTH_NULL;
    }
    if (store->initialized) {
        return FIRST_BOOT_HEALTH_BAD_STATE;
    }

    first_boot_health_zero_bytes((char *)store, sizeof(*store));
    store->version = FIRST_BOOT_HEALTH_VERSION;
    store->initialized = true;
    return FIRST_BOOT_HEALTH_OK;
}

enum first_boot_health_status first_boot_health_define(
    struct first_boot_health_store *store,
    const struct first_boot_health_requirements *requirements)
{
    enum first_boot_health_status status;

    if (store == NULL || requirements == NULL) {
        return FIRST_BOOT_HEALTH_NULL;
    }
    if (!store->initialized) {
        return FIRST_BOOT_HEALTH_BAD_STATE;
    }
    if (store->requirements.milestone_count != 0U) {
        return FIRST_BOOT_HEALTH_ALREADY_DEFINED;
    }
    status = first_boot_health_requirements_validate(requirements);
    if (status != FIRST_BOOT_HEALTH_OK) {
        return status;
    }

    store->requirements = *requirements;
    return FIRST_BOOT_HEALTH_OK;
}

enum first_boot_health_status first_boot_health_report(
    struct first_boot_health_store *store,
    const struct first_boot_health_report *report)
{
    size_t index;

    if (store == NULL || report == NULL) {
        return FIRST_BOOT_HEALTH_NULL;
    }
    if (!store->initialized ||
        store->requirements.milestone_count == 0U) {
        return FIRST_BOOT_HEALTH_BAD_STATE;
    }
    if (store->failed) {
        return FIRST_BOOT_HEALTH_FAILED;
    }
    if (report->version != FIRST_BOOT_HEALTH_VERSION) {
        return FIRST_BOOT_HEALTH_BAD_REQUEST;
    }
    if (!first_boot_health_subsystem_valid(
            report->milestone.subsystem)) {
        return FIRST_BOOT_HEALTH_BAD_SUBSYSTEM;
    }
    if (!first_boot_health_milestone_id_valid(
            report->milestone.milestone_id)) {
        return FIRST_BOOT_HEALTH_BAD_MILESTONE;
    }

    index = first_boot_health_find_milestone(
        store, &report->milestone);
    if (index == (size_t)-1) {
        return FIRST_BOOT_HEALTH_UNKNOWN;
    }
    if (store->observed[index]) {
        return FIRST_BOOT_HEALTH_DUPLICATE;
    }

    store->observed[index] = true;
    store->healthy[index] = report->healthy;
    ++store->observed_count;
    if (!report->healthy) {
        store->failed = true;
    }
    return FIRST_BOOT_HEALTH_OK;
}

enum first_boot_health_status first_boot_health_evaluate(
    const struct first_boot_health_store *store,
    struct first_boot_health_verdict *verdict_out)
{
    size_t index;

    if (store == NULL || verdict_out == NULL) {
        return FIRST_BOOT_HEALTH_NULL;
    }
    if (!store->initialized ||
        store->requirements.milestone_count == 0U) {
        return FIRST_BOOT_HEALTH_BAD_STATE;
    }

    first_boot_health_zero_bytes(
        (char *)verdict_out, sizeof(*verdict_out));
    verdict_out->required_count =
        store->requirements.milestone_count;
    verdict_out->observed_count = store->observed_count;
    if (store->failed) {
        verdict_out->failed = true;
        return FIRST_BOOT_HEALTH_FAILED;
    }

    for (index = 0U;
         index < store->requirements.milestone_count;
         ++index) {
        if (store->observed[index] && !store->healthy[index]) {
            verdict_out->failed = true;
            return FIRST_BOOT_HEALTH_FAILED;
        }
        if (!store->observed[index]) {
            ++verdict_out->missing_count;
            if (verdict_out->missing_count == 1U) {
                verdict_out->first_missing =
                    store->requirements.milestones[index];
            }
        }
    }
    if (verdict_out->missing_count != 0U) {
        return FIRST_BOOT_HEALTH_NOT_READY;
    }

    verdict_out->ready = true;
    return FIRST_BOOT_HEALTH_OK;
}

const char *first_boot_health_status_string(
    enum first_boot_health_status status)
{
    switch (status) {
    case FIRST_BOOT_HEALTH_OK:
        return "FIRST_BOOT_HEALTH_OK";
    case FIRST_BOOT_HEALTH_NULL:
        return "FIRST_BOOT_HEALTH_NULL";
    case FIRST_BOOT_HEALTH_BAD_STATE:
        return "FIRST_BOOT_HEALTH_BAD_STATE";
    case FIRST_BOOT_HEALTH_BAD_REQUEST:
        return "FIRST_BOOT_HEALTH_BAD_REQUEST";
    case FIRST_BOOT_HEALTH_BAD_SUBSYSTEM:
        return "FIRST_BOOT_HEALTH_BAD_SUBSYSTEM";
    case FIRST_BOOT_HEALTH_BAD_MILESTONE:
        return "FIRST_BOOT_HEALTH_BAD_MILESTONE";
    case FIRST_BOOT_HEALTH_DUPLICATE:
        return "FIRST_BOOT_HEALTH_DUPLICATE";
    case FIRST_BOOT_HEALTH_UNKNOWN:
        return "FIRST_BOOT_HEALTH_UNKNOWN";
    case FIRST_BOOT_HEALTH_NOT_READY:
        return "FIRST_BOOT_HEALTH_NOT_READY";
    case FIRST_BOOT_HEALTH_FAILED:
        return "FIRST_BOOT_HEALTH_FAILED";
    case FIRST_BOOT_HEALTH_ALREADY_DEFINED:
        return "FIRST_BOOT_HEALTH_ALREADY_DEFINED";
    case FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM:
        return "FIRST_BOOT_HEALTH_MISSING_SUBSYSTEM";
    default:
        return NULL;
    }
}
