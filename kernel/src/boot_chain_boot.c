#include "boot_chain_boot.h"

#include "image_signature.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_CHAIN_BOOT_POLICY_GENERATION 1U

enum boot_chain_status boot_chain_boot_run(
    const struct boot_chain_boot_source *source,
    struct boot_measurements *measurements,
    struct boot_attestation *attestation)
{
    struct image_signature_store store;
    struct verification_policy policy;

    if (measurements == NULL || attestation == NULL) {
        return BOOT_CHAIN_NULL;
    }
    if (boot_measurements_reset(measurements) != BOOT_MEASUREMENTS_OK) {
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }
    if (source == NULL || source->manifest == NULL ||
        source->manifest_length == 0U) {
        (void)boot_attestation_publish_unavailable(attestation, true);
        return BOOT_CHAIN_NULL;
    }
    if (source->components == NULL || source->component_count == 0U) {
        (void)boot_attestation_publish_unavailable(attestation, false);
        return BOOT_CHAIN_NULL;
    }

    if (image_signature_store_declared(&store, false) !=
            IMAGE_SIGNATURE_OK ||
        image_signature_policy_init(&store, &policy,
                                    BOOT_CHAIN_BOOT_POLICY_GENERATION) !=
            IMAGE_SIGNATURE_OK) {
        (void)boot_attestation_publish_unavailable(attestation, false);
        return BOOT_CHAIN_VERIFIER_UNAVAILABLE;
    }

    return boot_attestation_evaluate(
        &policy, &store, BOOT_CHAIN_EXPECTED_RELEASE_ID,
        source->manifest, source->manifest_length, source->components,
        source->component_count, measurements, attestation);
}
