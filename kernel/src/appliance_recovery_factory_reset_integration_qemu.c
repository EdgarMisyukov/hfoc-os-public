#ifdef HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_QEMU_TEST

#include "appliance_recovery_factory_reset_integration.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void recovery_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void recovery_qemu_failure(const char *message)
    __attribute__((noreturn));

static void recovery_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[RECOVERY-RESET] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void recovery_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        recovery_qemu_failure(message);
    }
}

static bool recovery_qemu_mem_equal(
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

static bool recovery_qemu_contains_exact(
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
        if (recovery_qemu_mem_equal(
                text + index, needle, needle_length)) {
            return true;
        }
    }
    return false;
}

static struct appliance_recovery_factory_reset_integration_evidence
recovery_qemu_evidence(
    uint32_t operation,
    uint64_t generation,
    bool complete,
    bool management_profile_required)
{
    struct appliance_recovery_factory_reset_integration_evidence evidence;

    recovery_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_EVIDENCE_VERSION;
    evidence.source_generation = generation;
    evidence.published = true;
    evidence.operation_kind = operation;
    evidence.storage_loader_ready = complete;
    evidence.boot_foundation_verified = complete;
    evidence.installer_recovery_ready = complete;
    evidence.efi_environment_ready = complete;
    if (operation ==
        APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY) {
        evidence.recovery_decision_present = complete;
        evidence.recovery_action = complete ? 5U : 0U;
        evidence.recovery_local_only = true;
        evidence.recovery_requires_reboot = false;
    } else if (operation ==
               APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET) {
        evidence.factory_reset_requested = complete;
        evidence.factory_reset_plan_published = complete;
        evidence.authorization_verified = complete;
    }
    evidence.management_profile_required = management_profile_required;
    evidence.management_active = complete && management_profile_required;
    evidence.first_boot_complete = complete && management_profile_required;
    evidence.network_active = complete && management_profile_required;
    evidence.https_management_ready =
        complete && management_profile_required;
    return evidence;
}

void appliance_recovery_factory_reset_integration_qemu_test_init(void)
{
    struct appliance_recovery_factory_reset_integration integration;
    struct appliance_recovery_factory_reset_integration_evidence none =
        recovery_qemu_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE,
            1U, false, false);
    struct appliance_recovery_factory_reset_integration_evidence
        incomplete_recovery =
            recovery_qemu_evidence(
                APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY,
                1U, false, true);
    struct appliance_recovery_factory_reset_integration_evidence recovery =
        recovery_qemu_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY,
            2U, true, true);
    struct appliance_recovery_factory_reset_integration_evidence
        unauthorized_reset =
            recovery_qemu_evidence(
                APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
                1U, false, true);
    struct appliance_recovery_factory_reset_integration_evidence reset =
        recovery_qemu_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            2U, true, true);
    struct appliance_recovery_factory_reset_integration_evidence stale =
        recovery_qemu_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY,
            1U, true, true);
    struct appliance_recovery_factory_reset_integration_evidence final_reset =
        recovery_qemu_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            5U, true, true);
    struct appliance_recovery_factory_reset_integration_snapshot snapshot;
    char output[
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    recovery_qemu_zero_bytes(&integration, sizeof(integration));
    recovery_qemu_zero_bytes(&snapshot, sizeof(snapshot));

    serial_write("[RECOVERY-RESET] proof init\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_init(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_publish(
            &integration, &none) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_RECOVERY,
        "no operation blocks ready");
    serial_write("[RECOVERY-RESET] no operation blocks ready\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_publish(
            &integration, &incomplete_recovery) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_FOUNDATION,
        "missing foundation blocks ready");
    serial_write("[RECOVERY-RESET] missing foundation blocks ready\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_publish(
            &integration, &recovery) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY &&
        appliance_recovery_factory_reset_integration_snapshot(
            &integration, &snapshot) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        recovery_qemu_contains_exact(
            output,
            "recovery_integration v=1 state=READY "
            "operation=RECOVERY recovery=READ_ONLY reset=NONE "
            "auth=UNKNOWN boot=VERIFIED storage=READY installer=READY "
            "efi=READY management=ACTIVE health=PASS"),
        "recovery snapshot canonical");
    serial_write("[RECOVERY-RESET] recovery snapshot\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_reset(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_publish(
            &integration, &unauthorized_reset) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_AUTHORIZATION &&
        appliance_recovery_factory_reset_integration_publish(
            &integration, &reset) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY &&
        appliance_recovery_factory_reset_integration_snapshot(
            &integration, &snapshot) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        recovery_qemu_contains_exact(
            output,
            "recovery_integration v=1 state=READY "
            "operation=FACTORY_RESET recovery=NONE reset=APPROVED "
            "auth=VERIFIED boot=VERIFIED storage=READY installer=READY "
            "efi=READY management=ACTIVE health=PASS"),
        "factory reset snapshot canonical");
    serial_write("[RECOVERY-RESET] factory reset snapshot\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_publish(
            &integration, &stale) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STALE_EVIDENCE &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY,
        "stale evidence rejected");
    serial_write("[RECOVERY-RESET] stale evidence rejected\n");

    recovery_qemu_require(
        appliance_recovery_factory_reset_integration_reset(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_publish(
            &integration, &final_reset) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_complete(
            &integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
        appliance_recovery_factory_reset_integration_state(&integration) ==
            APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE,
        "clean retry and completion");
    serial_write("[RECOVERY-RESET] completion confirmed\n");
    serial_write("[RECOVERY-RESET] clean shutdown\n");
    serial_write("[RECOVERY-RESET] proof OK\n");
}

#else
typedef int appliance_recovery_factory_reset_integration_qemu_translation_unit_guard;
#endif
