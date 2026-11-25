#ifdef HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_QEMU_TEST

#include "appliance_first_boot_integration.h"
#include "appliance_production_management.h"
#include "appliance_production_service_https.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void first_boot_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void first_boot_qemu_failure(const char *message)
    __attribute__((noreturn));

static void first_boot_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[FIRST-BOOT] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void first_boot_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        first_boot_qemu_failure(message);
    }
}

static bool first_boot_qemu_mem_equal(
    const char *text,
    const char *needle,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (text[index] != needle[index]) {
            return false;
        }
    }
    return true;
}

static bool first_boot_qemu_contains_exact(
    const char *text,
    const char *needle)
{
    size_t text_length = 0U;
    size_t needle_length = 0U;
    size_t index;

    while (text[text_length] != '\0') {
        ++text_length;
    }
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    if (needle_length > text_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= text_length; ++index) {
        if (first_boot_qemu_mem_equal(
                text + index, needle, needle_length)) {
            return true;
        }
    }
    return false;
}

static struct appliance_production_service_https_evidence
first_boot_qemu_service_evidence(void)
{
    struct appliance_production_service_https_evidence evidence;

    first_boot_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = true;
    evidence.manager_stopped = true;
    evidence.https_enabled = true;
    evidence.https_stopped = true;
    return evidence;
}

static struct appliance_production_service_https_readiness
first_boot_qemu_readiness(void)
{
    struct appliance_production_service_https_readiness readiness;

    first_boot_qemu_zero_bytes(&readiness, sizeof(readiness));
    readiness.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION;
    readiness.source_generation = 1U;
    readiness.network_active = true;
    readiness.entropy_ready = true;
    readiness.certificate_ready = true;
    return readiness;
}

static struct appliance_production_management_evidence
first_boot_qemu_management_evidence(bool complete)
{
    struct appliance_production_management_evidence evidence;

    first_boot_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.system_identity_present = complete;
    evidence.configuration_ready = complete;
    evidence.network_required = true;
    evidence.network_active = complete;
    evidence.network_health_ok = complete;
    evidence.production_services_ready = complete;
    evidence.https_management_ready = complete;
    return evidence;
}

static struct appliance_first_boot_integration_evidence
first_boot_qemu_integration_evidence(
    uint64_t generation,
    bool storage_ready,
    bool wizard_complete,
    bool management_active,
    bool network_active,
    bool https_ready)
{
    struct appliance_first_boot_integration_evidence evidence;

    first_boot_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_FIRST_BOOT_INTEGRATION_EVIDENCE_VERSION;
    evidence.source_generation = generation;
    evidence.published = true;
    evidence.first_boot_pending = true;
    evidence.storage_ready = storage_ready;
    evidence.wizard_complete = wizard_complete;
    evidence.management_active = management_active;
    evidence.network_active = network_active;
    evidence.https_management_ready = https_ready;
    return evidence;
}

void appliance_first_boot_integration_qemu_test_init(void)
{
    struct appliance_production_service_https service;
    struct appliance_production_service_https_evidence service_evidence =
        first_boot_qemu_service_evidence();
    struct appliance_production_service_https_readiness readiness =
        first_boot_qemu_readiness();
    struct appliance_production_service_https_snapshot service_snapshot;
    struct appliance_production_management management;
    struct appliance_production_management_evidence missing_management =
        first_boot_qemu_management_evidence(false);
    struct appliance_production_management_evidence active_management =
        first_boot_qemu_management_evidence(true);
    struct appliance_production_management_snapshot management_snapshot;
    struct appliance_first_boot_integration integration;
    struct appliance_first_boot_integration_evidence awaiting_wizard =
        first_boot_qemu_integration_evidence(
            1U, true, false, false, false, false);
    struct appliance_first_boot_integration_evidence awaiting_management =
        first_boot_qemu_integration_evidence(
            1U, true, true, false, false, false);
    struct appliance_first_boot_integration_evidence ready_evidence =
        first_boot_qemu_integration_evidence(
            1U, true, true, true, true, true);
    struct appliance_first_boot_integration_evidence missing_after_ready =
        first_boot_qemu_integration_evidence(
            2U, true, true, false, true, true);
    struct appliance_first_boot_integration_snapshot snapshot;
    char output[
        APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    first_boot_qemu_zero_bytes(&service, sizeof(service));
    first_boot_qemu_zero_bytes(
        &service_snapshot, sizeof(service_snapshot));
    first_boot_qemu_zero_bytes(&management, sizeof(management));
    first_boot_qemu_zero_bytes(
        &management_snapshot, sizeof(management_snapshot));
    first_boot_qemu_zero_bytes(&integration, sizeof(integration));
    first_boot_qemu_zero_bytes(&snapshot, sizeof(snapshot));

    serial_write("[FIRST-BOOT] proof init\n");

    first_boot_qemu_require(
        appliance_production_service_https_init(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_configure(
            &service, &service_evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_readiness(
            &service, &readiness) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_start(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_running(&service) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_snapshot(
            &service, &service_snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        service_snapshot.state ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING,
        "production service https evidence active");
    serial_write("[FIRST-BOOT] service https evidence active\n");

    first_boot_qemu_require(
        appliance_production_management_init(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_publish(
            &management, &missing_management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE &&
        appliance_production_management_publish(
            &management, &active_management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        appliance_production_management_state(&management) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE &&
        appliance_production_management_snapshot(
            &management, &management_snapshot) ==
            APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
        management_snapshot.state ==
            APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE,
        "production management evidence active");
    serial_write("[FIRST-BOOT] production management evidence active\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_init(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_publish(
            &integration, &awaiting_wizard) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD,
        "missing wizard blocks ready");
    serial_write("[FIRST-BOOT] missing wizard blocks ready\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_publish(
            &integration, &awaiting_management) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT,
        "missing management blocks ready");
    serial_write("[FIRST-BOOT] missing management blocks ready\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_publish(
            &integration, &ready_evidence) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY &&
        appliance_first_boot_integration_snapshot(
            &integration, &snapshot) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        first_boot_qemu_contains_exact(
            output,
            "first_boot_integration v=1 state=READY "
            "first_boot=PENDING storage=READY wizard=COMPLETE "
            "management=ACTIVE network=ACTIVE https=READY "
            "health=PASS"),
        "canonical first boot snapshot");
    serial_write("[FIRST-BOOT] canonical snapshot\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_publish(
            &integration, &missing_after_ready) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED &&
        !appliance_first_boot_integration_published(&integration),
        "missing management after ready fails closed");
    serial_write("[FIRST-BOOT] failure fails closed\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_reset(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_publish(
            &integration, &ready_evidence) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_complete(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE &&
        appliance_first_boot_integration_snapshot(
            &integration, &snapshot) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        first_boot_qemu_contains_exact(
            output, "first_boot_integration v=1 state=COMPLETE"),
        "completion confirmed");
    serial_write("[FIRST-BOOT] completion confirmed\n");

    first_boot_qemu_require(
        appliance_first_boot_integration_reset(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
        appliance_first_boot_integration_state(&integration) ==
            APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED &&
        !appliance_first_boot_integration_published(&integration),
        "clean reset");
    serial_write("[FIRST-BOOT] clean shutdown\n");
    serial_write("[FIRST-BOOT] proof OK\n");
}

#else
typedef int appliance_first_boot_integration_qemu_translation_unit_guard;
#endif
