#ifndef HFOC_TESTS_BOOT_CHAIN_VECTOR_H
#define HFOC_TESTS_BOOT_CHAIN_VECTOR_H

/*
 * ITEM-372 shared boot-chain vector.
 *
 * One fixed component set signed by two declared identities:
 *   - boot_chain_vector_dev_signature is produced by
 *     boot/keys/hfoc-development-signing-key.pem (signer id 1), the same
 *     identity the ISO build uses, so the host and QEMU proofs verify the
 *     production trust set and the production manifest format;
 *   - boot_chain_vector_fixture_signature is produced by a proof-only key
 *     (signer id 7) that exists only in the harness.
 * The QEMU proof and the host proofs consume this single definition, so a
 * format drift between producer, kernel and proof fails in the host suite.
 */

#include "boot_chain.h"

#include <stdint.h>

#define BOOT_CHAIN_VECTOR_RELEASE_ID 372U
#define BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID 7U
#define BOOT_CHAIN_VECTOR_BOOT_PAYLOAD_LENGTH 64U
#define BOOT_CHAIN_VECTOR_LOADER_CONFIG_LENGTH 85U
#define BOOT_CHAIN_VECTOR_KERNEL_LENGTH 256U

extern const unsigned char
    boot_chain_vector_boot_payload[BOOT_CHAIN_VECTOR_BOOT_PAYLOAD_LENGTH];
extern const unsigned char
    boot_chain_vector_loader_config[BOOT_CHAIN_VECTOR_LOADER_CONFIG_LENGTH];
extern const unsigned char
    boot_chain_vector_kernel[BOOT_CHAIN_VECTOR_KERNEL_LENGTH];
extern const unsigned char
    boot_chain_vector_dev_signature[BOOT_CHAIN_SIGNATURE_SIZE];
extern const unsigned char
    boot_chain_vector_fixture_signature[BOOT_CHAIN_SIGNATURE_SIZE];
extern const unsigned char
    boot_chain_vector_fixture_public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE];
extern const unsigned char
    boot_chain_vector_chain_digest[SHA256_DIGEST_SIZE];

void boot_chain_vector_components(
    struct boot_chain_component components[3]);

#endif
