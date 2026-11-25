#include "appliance_update_bundle.h"

#include "appliance_update_integrity.h"
#include "key_use.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-376 signed update bundle.  See the header for the contract.
 */

#define APPLIANCE_UPDATE_BUNDLE_VERSION_OFFSET 8U
#define APPLIANCE_UPDATE_BUNDLE_PACKAGE_OFFSET 12U
#define APPLIANCE_UPDATE_BUNDLE_SOURCE_OFFSET 16U
#define APPLIANCE_UPDATE_BUNDLE_TARGET_OFFSET 20U
#define APPLIANCE_UPDATE_BUNDLE_RELEASE_OFFSET 24U
#define APPLIANCE_UPDATE_BUNDLE_ARCHITECTURE_OFFSET \
    (APPLIANCE_UPDATE_BUNDLE_RELEASE_OFFSET + RELEASE_PROFILE_ID_LENGTH)
#define APPLIANCE_UPDATE_BUNDLE_REVISION_OFFSET \
    (APPLIANCE_UPDATE_BUNDLE_ARCHITECTURE_OFFSET + \
     RELEASE_PROFILE_ARCH_LENGTH)
#define APPLIANCE_UPDATE_BUNDLE_METADATA_OFFSET \
    (APPLIANCE_UPDATE_BUNDLE_REVISION_OFFSET + \
     APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH)
#define APPLIANCE_UPDATE_BUNDLE_PAYLOAD_OFFSET \
    (APPLIANCE_UPDATE_BUNDLE_METADATA_OFFSET + SHA256_DIGEST_SIZE)
#define APPLIANCE_UPDATE_BUNDLE_DIGEST_LENGTH 64U

static const uint8_t appliance_update_bundle_magic[
    APPLIANCE_UPDATE_BUNDLE_MAGIC_SIZE] =
    APPLIANCE_UPDATE_BUNDLE_MAGIC_BYTES;

static void appliance_update_bundle_zero(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_update_bundle_store_le32(uint8_t *destination,
                                               uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static bool appliance_update_bundle_hex_nibble(char character,
                                               uint8_t *value_out)
{
    if (character >= '0' && character <= '9') {
        *value_out = (uint8_t)(character - '0');
        return true;
    }
    if (character >= 'a' && character <= 'f') {
        *value_out = (uint8_t)(character - 'a' + 10);
        return true;
    }
    if (character >= 'A' && character <= 'F') {
        *value_out = (uint8_t)(character - 'A' + 10);
        return true;
    }
    return false;
}

static bool appliance_update_bundle_hex_decode(
    const char *hex,
    uint8_t *output,
    size_t output_length)
{
    size_t index;
    uint8_t high;
    uint8_t low;

    for (index = 0U; index < output_length; ++index) {
        if (!appliance_update_bundle_hex_nibble(
                hex[index * 2U], &high) ||
            !appliance_update_bundle_hex_nibble(
                hex[index * 2U + 1U], &low)) {
            return false;
        }
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return hex[output_length * 2U] == '\0';
}

static bool appliance_update_bundle_copy_field(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index;

    if (source == NULL) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        if (source[index] == '\0') {
            return true;
        }
        destination[index] = source[index];
    }
    return source[capacity] == '\0';
}

enum appliance_update_bundle_status appliance_update_bundle_bind(
    const struct appliance_update_manifest *manifest,
    const struct appliance_update_integrity_gate *gate,
    struct appliance_update_bundle_binding *binding_out)
{
    if (manifest == NULL || gate == NULL || binding_out == NULL) {
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    }
    if (!appliance_update_integrity_is_verified(gate)) {
        return APPLIANCE_UPDATE_BUNDLE_GATE_NOT_VERIFIED;
    }

    appliance_update_bundle_zero(binding_out, sizeof(*binding_out));
    binding_out->version = APPLIANCE_UPDATE_BUNDLE_VERSION;
    binding_out->update_package_version =
        manifest->update_package_version;
    binding_out->source_image_id =
        manifest->compatibility.source_image_id;
    binding_out->target_image_id =
        manifest->compatibility.target_image_id;
    if (!appliance_update_bundle_copy_field(
            binding_out->release_id, RELEASE_PROFILE_ID_LENGTH,
            manifest->release_id) ||
        !appliance_update_bundle_copy_field(
            binding_out->architecture, RELEASE_PROFILE_ARCH_LENGTH,
            manifest->architecture) ||
        !appliance_update_bundle_copy_field(
            binding_out->source_revision,
            APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH,
            manifest->source_revision) ||
        !appliance_update_bundle_hex_decode(
            manifest->metadata_digest, binding_out->metadata_digest,
            SHA256_DIGEST_SIZE) ||
        !appliance_update_bundle_hex_decode(
            manifest->payload_digest, binding_out->payload_digest,
            SHA256_DIGEST_SIZE)) {
        appliance_update_bundle_zero(binding_out,
                                     sizeof(*binding_out));
        return APPLIANCE_UPDATE_BUNDLE_BAD_BINDING;
    }
    /* The binding must commit to exactly the bytes the gate verified. */
    if (memcmp(binding_out->metadata_digest, gate->metadata_digest,
               SHA256_DIGEST_SIZE) != 0 ||
        memcmp(binding_out->payload_digest, gate->payload_digest,
               SHA256_DIGEST_SIZE) != 0) {
        appliance_update_bundle_zero(binding_out,
                                     sizeof(*binding_out));
        return APPLIANCE_UPDATE_BUNDLE_BAD_BINDING;
    }
    return APPLIANCE_UPDATE_BUNDLE_OK;
}

enum appliance_update_bundle_status appliance_update_bundle_encode(
    const struct appliance_update_bundle_binding *binding,
    uint8_t *output,
    size_t capacity,
    size_t *length_out)
{
    if (binding == NULL || output == NULL || length_out == NULL) {
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    }
    if (binding->version != APPLIANCE_UPDATE_BUNDLE_VERSION) {
        return APPLIANCE_UPDATE_BUNDLE_BAD_VERSION;
    }
    if (capacity < APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE) {
        return APPLIANCE_UPDATE_BUNDLE_BUFFER_TOO_SMALL;
    }

    appliance_update_bundle_zero(
        output, APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE);
    memcpy(output, appliance_update_bundle_magic,
           APPLIANCE_UPDATE_BUNDLE_MAGIC_SIZE);
    appliance_update_bundle_store_le32(
        output + APPLIANCE_UPDATE_BUNDLE_VERSION_OFFSET,
        binding->version);
    appliance_update_bundle_store_le32(
        output + APPLIANCE_UPDATE_BUNDLE_PACKAGE_OFFSET,
        binding->update_package_version);
    appliance_update_bundle_store_le32(
        output + APPLIANCE_UPDATE_BUNDLE_SOURCE_OFFSET,
        binding->source_image_id);
    appliance_update_bundle_store_le32(
        output + APPLIANCE_UPDATE_BUNDLE_TARGET_OFFSET,
        binding->target_image_id);
    memcpy(output + APPLIANCE_UPDATE_BUNDLE_RELEASE_OFFSET,
           binding->release_id, RELEASE_PROFILE_ID_LENGTH);
    memcpy(output + APPLIANCE_UPDATE_BUNDLE_ARCHITECTURE_OFFSET,
           binding->architecture, RELEASE_PROFILE_ARCH_LENGTH);
    memcpy(output + APPLIANCE_UPDATE_BUNDLE_REVISION_OFFSET,
           binding->source_revision,
           APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH);
    memcpy(output + APPLIANCE_UPDATE_BUNDLE_METADATA_OFFSET,
           binding->metadata_digest, SHA256_DIGEST_SIZE);
    memcpy(output + APPLIANCE_UPDATE_BUNDLE_PAYLOAD_OFFSET,
           binding->payload_digest, SHA256_DIGEST_SIZE);
    *length_out = APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE;
    return APPLIANCE_UPDATE_BUNDLE_OK;
}

enum appliance_update_bundle_status appliance_update_bundle_digest(
    const struct appliance_update_bundle_binding *binding,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    uint8_t encoding[APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE];
    enum appliance_update_bundle_status status;
    size_t length = 0U;

    if (digest_out == NULL) {
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    }
    status = appliance_update_bundle_encode(
        binding, encoding, sizeof(encoding), &length);
    if (status != APPLIANCE_UPDATE_BUNDLE_OK) {
        return status;
    }
    if (sha256_compute(encoding, length, digest_out) != SHA256_OK) {
        return APPLIANCE_UPDATE_BUNDLE_BAD_BINDING;
    }
    return APPLIANCE_UPDATE_BUNDLE_OK;
}

static enum appliance_update_bundle_status
appliance_update_bundle_map_signature(enum image_signature_status status)
{
    switch (status) {
    case IMAGE_SIGNATURE_OK:
        return APPLIANCE_UPDATE_BUNDLE_OK;
    case IMAGE_SIGNATURE_PURPOSE_DENIED:
        return APPLIANCE_UPDATE_BUNDLE_PURPOSE_DENIED;
    case IMAGE_SIGNATURE_UNKNOWN_SIGNER:
    case IMAGE_SIGNATURE_UNAVAILABLE:
        return APPLIANCE_UPDATE_BUNDLE_SIGNATURE_UNAVAILABLE;
    default:
        return APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID;
    }
}

enum appliance_update_bundle_status appliance_update_bundle_verify(
    struct appliance_update_bundle *bundle,
    const struct appliance_update_manifest *manifest,
    const struct appliance_update_integrity_gate *gate,
    const struct image_signature_store *store,
    const void *signature,
    size_t signature_length)
{
    enum appliance_update_bundle_status status;
    enum image_signature_status signature_status;

    if (bundle == NULL || manifest == NULL || gate == NULL ||
        store == NULL) {
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    }
    if (signature == NULL || signature_length == 0U) {
        return APPLIANCE_UPDATE_BUNDLE_UNSIGNED;
    }
    if (signature_length != APPLIANCE_UPDATE_BUNDLE_SIGNATURE_SIZE) {
        return APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID;
    }

    appliance_update_bundle_zero(bundle, sizeof(*bundle));
    bundle->version = APPLIANCE_UPDATE_BUNDLE_VERSION;
    bundle->algorithm = APPLIANCE_UPDATE_BUNDLE_ALGORITHM;
    bundle->signer_id = gate->signer_id;

    status = appliance_update_bundle_bind(
        manifest, gate, &bundle->binding);
    if (status != APPLIANCE_UPDATE_BUNDLE_OK) {
        return status;
    }
    status = appliance_update_bundle_digest(&bundle->binding,
                                            bundle->digest);
    if (status != APPLIANCE_UPDATE_BUNDLE_OK) {
        return status;
    }

    signature_status = image_signature_verify_purpose(
        (void *)store,
        APPLIANCE_UPDATE_BUNDLE_ALGORITHM,
        gate->signer_id,
        KEY_USE_PURPOSE_UPDATE,
        bundle->digest,
        (const unsigned char *)signature,
        signature_length);
    status = appliance_update_bundle_map_signature(signature_status);
    if (status != APPLIANCE_UPDATE_BUNDLE_OK) {
        return status;
    }
    bundle->verified = true;
    return APPLIANCE_UPDATE_BUNDLE_OK;
}

bool appliance_update_bundle_is_verified(
    const struct appliance_update_bundle *bundle)
{
    return bundle != NULL &&
           bundle->version == APPLIANCE_UPDATE_BUNDLE_VERSION &&
           bundle->verified;
}

enum appliance_update_bundle_status appliance_update_bundle_admit(
    const struct appliance_update_bundle *bundle,
    const struct appliance_update_record *record)
{
    if (bundle == NULL || record == NULL) {
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    }
    if (!appliance_update_bundle_is_verified(bundle)) {
        return APPLIANCE_UPDATE_BUNDLE_NOT_VERIFIED;
    }
    if (bundle->binding.version != APPLIANCE_UPDATE_BUNDLE_VERSION) {
        return APPLIANCE_UPDATE_BUNDLE_BAD_VERSION;
    }
    switch (appliance_update_record_check_upgrade(
        record,
        bundle->binding.target_image_id,
        bundle->binding.source_image_id)) {
    case APPLIANCE_UPDATE_RECORD_OK:
        return APPLIANCE_UPDATE_BUNDLE_OK;
    case APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED:
        return APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED;
    case APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH:
        return APPLIANCE_UPDATE_BUNDLE_SOURCE_MISMATCH;
    case APPLIANCE_UPDATE_RECORD_NULL:
        return APPLIANCE_UPDATE_BUNDLE_NULL;
    default:
        return APPLIANCE_UPDATE_BUNDLE_BAD_STATE;
    }
}

const char *appliance_update_bundle_status_string(
    enum appliance_update_bundle_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_BUNDLE_OK:
        return "OK";
    case APPLIANCE_UPDATE_BUNDLE_NULL:
        return "NULL";
    case APPLIANCE_UPDATE_BUNDLE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_UPDATE_BUNDLE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_UPDATE_BUNDLE_BAD_BINDING:
        return "BAD_BINDING";
    case APPLIANCE_UPDATE_BUNDLE_GATE_NOT_VERIFIED:
        return "GATE_NOT_VERIFIED";
    case APPLIANCE_UPDATE_BUNDLE_UNSIGNED:
        return "UNSIGNED";
    case APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID:
        return "SIGNATURE_INVALID";
    case APPLIANCE_UPDATE_BUNDLE_SIGNATURE_UNAVAILABLE:
        return "SIGNATURE_UNAVAILABLE";
    case APPLIANCE_UPDATE_BUNDLE_PURPOSE_DENIED:
        return "PURPOSE_DENIED";
    case APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED:
        return "DOWNGRADE_REFUSED";
    case APPLIANCE_UPDATE_BUNDLE_SOURCE_MISMATCH:
        return "SOURCE_MISMATCH";
    case APPLIANCE_UPDATE_BUNDLE_NOT_VERIFIED:
        return "NOT_VERIFIED";
    case APPLIANCE_UPDATE_BUNDLE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
