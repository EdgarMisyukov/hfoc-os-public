#include "appliance_secret_protection.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t appliance_secret_protection_domain[] =
    "HFOCSECPRV1";

static void appliance_secret_protection_zero_bytes(
    void *destination,
    size_t length)
{
    volatile uint8_t *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_secret_protection_bytes_all_zero(
    const uint8_t *bytes,
    size_t length)
{
    size_t index;
    uint8_t nonzero = 0U;

    for (index = 0U; index < length; ++index) {
        nonzero |= bytes[index];
    }
    return nonzero == 0U;
}

static bool appliance_secret_protection_bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length)
{
    volatile uint8_t difference = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

static bool appliance_secret_protection_kind_valid(
    enum appliance_secret_protection_kind kind)
{
    return kind == APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL ||
           kind == APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN ||
           kind == APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY ||
           kind == APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET;
}

static bool appliance_secret_protection_state_valid(
    enum appliance_secret_protection_state state)
{
    return state == APPLIANCE_SECRET_PROTECTION_STATE_EMPTY ||
           state == APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED;
}

static enum appliance_secret_protection_status
appliance_secret_protection_validate(
    const struct appliance_secret_protection *protection)
{
    if (protection == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    if (protection->version != APPLIANCE_SECRET_PROTECTION_VERSION) {
        return APPLIANCE_SECRET_PROTECTION_BAD_VERSION;
    }
    if (!appliance_secret_protection_state_valid(
            protection->state)) {
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    if (protection->state == APPLIANCE_SECRET_PROTECTION_STATE_EMPTY) {
        if (protection->kind != APPLIANCE_SECRET_PROTECTION_KIND_NONE ||
            protection->at_rest_supported ||
            protection->material_retained ||
            protection->retained_length != 0U ||
            !appliance_secret_protection_bytes_all_zero(
                protection->fingerprint,
                sizeof(protection->fingerprint)) ||
            !appliance_secret_protection_bytes_all_zero(
                protection->retained_material,
                sizeof(protection->retained_material))) {
            return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
        }
        return APPLIANCE_SECRET_PROTECTION_OK;
    }

    if (!appliance_secret_protection_kind_valid(
            protection->kind) ||
        protection->generation == 0U) {
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    if (protection->kind ==
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL) {
        if (!protection->at_rest_supported ||
            protection->material_retained ||
            protection->retained_length != 0U ||
            !appliance_secret_protection_bytes_all_zero(
                protection->retained_material,
                sizeof(protection->retained_material))) {
            return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
        }
    } else {
        if (protection->at_rest_supported ||
            !protection->material_retained ||
            protection->retained_length == 0U ||
            protection->retained_length >
                APPLIANCE_SECRET_PROTECTION_MAX_SECRET_LENGTH) {
            return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
        }
    }
    if (appliance_secret_protection_bytes_all_zero(
            protection->fingerprint,
            sizeof(protection->fingerprint))) {
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    return APPLIANCE_SECRET_PROTECTION_OK;
}

static enum appliance_secret_protection_status
appliance_secret_protection_validate_material(
    const struct appliance_secret_material *material)
{
    if (material == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    if (material->version != APPLIANCE_SECRET_PROTECTION_VERSION) {
        return APPLIANCE_SECRET_PROTECTION_BAD_VERSION;
    }
    if (!appliance_secret_protection_kind_valid(material->kind)) {
        return APPLIANCE_SECRET_PROTECTION_BAD_KIND;
    }
    if (material->length == 0U) {
        return APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL;
    }
    if (material->bytes == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    if (material->kind ==
            APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL &&
        material->length !=
            APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH) {
        return APPLIANCE_SECRET_PROTECTION_BAD_LENGTH;
    }
    if (material->kind ==
            APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN &&
        material->length != APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH) {
        return APPLIANCE_SECRET_PROTECTION_BAD_LENGTH;
    }
    if (material->length > APPLIANCE_SECRET_PROTECTION_MAX_SECRET_LENGTH) {
        return APPLIANCE_SECRET_PROTECTION_BAD_LENGTH;
    }
    if (appliance_secret_protection_bytes_all_zero(
            material->bytes,
            material->length)) {
        return APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL;
    }
    return APPLIANCE_SECRET_PROTECTION_OK;
}

static void appliance_secret_protection_hash_u32(
    struct sha256 *context,
    uint32_t value)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
    (void)sha256_update(context, bytes, sizeof(bytes));
}

/*
 * ITEM-401: the fingerprint computation is the record contract the first-boot
 * flow commits and the enrollment owner verifies against, so it is exposed
 * instead of being copied into a second derivation.
 */
enum appliance_secret_protection_status
appliance_secret_protection_fingerprint(
    enum appliance_secret_protection_kind kind,
    uint32_t generation,
    const uint8_t *bytes,
    uint32_t length,
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    struct sha256 context;
    enum sha256_status status;

    if (digest == NULL || (bytes == NULL && length != 0U)) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = sha256_init(&context);
    if (status != SHA256_OK) {
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    status = sha256_update(
        &context,
        appliance_secret_protection_domain,
        sizeof(appliance_secret_protection_domain) - 1U);
    if (status != SHA256_OK) {
        appliance_secret_protection_zero_bytes(
            &context,
            sizeof(context));
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    appliance_secret_protection_hash_u32(
        &context,
        APPLIANCE_SECRET_PROTECTION_VERSION);
    appliance_secret_protection_hash_u32(&context, (uint32_t)kind);
    appliance_secret_protection_hash_u32(&context, generation);
    appliance_secret_protection_hash_u32(&context, length);
    status = sha256_update(&context, bytes, length);
    if (status != SHA256_OK) {
        appliance_secret_protection_zero_bytes(
            &context,
            sizeof(context));
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    status = sha256_final(&context, digest);
    appliance_secret_protection_zero_bytes(
        &context,
        sizeof(context));
    if (status != SHA256_OK) {
        appliance_secret_protection_zero_bytes(
            digest,
            SHA256_DIGEST_SIZE);
        return APPLIANCE_SECRET_PROTECTION_BAD_STATE;
    }
    return APPLIANCE_SECRET_PROTECTION_OK;
}

static void appliance_secret_protection_clear(
    struct appliance_secret_protection *protection)
{
    appliance_secret_protection_zero_bytes(
        protection->fingerprint,
        sizeof(protection->fingerprint));
    if (protection->material_retained &&
        protection->retained_length != 0U) {
        appliance_secret_protection_zero_bytes(
            protection->retained_material,
            sizeof(protection->retained_material));
    }
    protection->at_rest_supported = false;
    protection->material_retained = false;
    protection->retained_length = 0U;
    protection->kind = APPLIANCE_SECRET_PROTECTION_KIND_NONE;
    protection->state = APPLIANCE_SECRET_PROTECTION_STATE_EMPTY;
}

static enum appliance_secret_protection_status
appliance_secret_protection_publish(
    struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material,
    uint32_t generation)
{
    enum appliance_secret_protection_status status;
    uint8_t digest[SHA256_DIGEST_SIZE];
    bool retain = material->kind !=
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL;

    status = appliance_secret_protection_fingerprint(
        material->kind,
        generation,
        material->bytes,
        material->length,
        digest);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }

    appliance_secret_protection_clear(protection);
    memcpy(protection->fingerprint,
           digest,
           sizeof(protection->fingerprint));
    protection->generation = generation;
    protection->kind = material->kind;
    protection->at_rest_supported =
        material->kind ==
            APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL;
    protection->material_retained = retain;
    if (retain) {
        memcpy(protection->retained_material,
               material->bytes,
               material->length);
        protection->retained_length = material->length;
    } else {
        protection->retained_length = 0U;
    }
    protection->state = APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED;
    appliance_secret_protection_zero_bytes(
        digest,
        sizeof(digest));
    return APPLIANCE_SECRET_PROTECTION_OK;
}

enum appliance_secret_protection_status
appliance_secret_protection_init(
    struct appliance_secret_protection *protection)
{
    if (protection == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    appliance_secret_protection_zero_bytes(
        protection,
        sizeof(*protection));
    protection->version = APPLIANCE_SECRET_PROTECTION_VERSION;
    protection->state = APPLIANCE_SECRET_PROTECTION_STATE_EMPTY;
    protection->kind = APPLIANCE_SECRET_PROTECTION_KIND_NONE;
    return APPLIANCE_SECRET_PROTECTION_OK;
}

enum appliance_secret_protection_status
appliance_secret_protection_protect(
    struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material)
{
    enum appliance_secret_protection_status status;
    uint32_t generation;

    if (protection == NULL || material == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = appliance_secret_protection_validate(protection);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    status = appliance_secret_protection_validate_material(material);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    if (protection->state != APPLIANCE_SECRET_PROTECTION_STATE_EMPTY) {
        return APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED;
    }
    if (protection->generation == UINT32_MAX) {
        return APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED;
    }
    generation = protection->generation + 1U;
    return appliance_secret_protection_publish(
        protection,
        material,
        generation);
}

enum appliance_secret_protection_status
appliance_secret_protection_rotate(
    struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material)
{
    enum appliance_secret_protection_status status;
    uint32_t generation;

    if (protection == NULL || material == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = appliance_secret_protection_validate(protection);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    status = appliance_secret_protection_validate_material(material);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    if (protection->state != APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED) {
        return APPLIANCE_SECRET_PROTECTION_EMPTY;
    }
    if (protection->kind != material->kind) {
        return APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH;
    }
    if (protection->generation == UINT32_MAX) {
        return APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED;
    }
    generation = protection->generation + 1U;
    return appliance_secret_protection_publish(
        protection,
        material,
        generation);
}

enum appliance_secret_protection_status
appliance_secret_protection_reset(
    struct appliance_secret_protection *protection)
{
    enum appliance_secret_protection_status status;

    if (protection == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = appliance_secret_protection_validate(protection);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    if (protection->state != APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED) {
        return APPLIANCE_SECRET_PROTECTION_EMPTY;
    }
    appliance_secret_protection_clear(protection);
    return APPLIANCE_SECRET_PROTECTION_OK;
}

enum appliance_secret_protection_status
appliance_secret_protection_verify(
    const struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material)
{
    enum appliance_secret_protection_status status;
    uint8_t digest[SHA256_DIGEST_SIZE];
    bool matches;

    if (protection == NULL || material == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = appliance_secret_protection_validate(protection);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    status = appliance_secret_protection_validate_material(material);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    if (protection->state != APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED) {
        return APPLIANCE_SECRET_PROTECTION_EMPTY;
    }
    if (protection->kind != material->kind) {
        return APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH;
    }
    status = appliance_secret_protection_fingerprint(
        material->kind,
        protection->generation,
        material->bytes,
        material->length,
        digest);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    matches = appliance_secret_protection_bytes_equal(
        digest,
        protection->fingerprint,
        SHA256_DIGEST_SIZE);
    appliance_secret_protection_zero_bytes(
        digest,
        sizeof(digest));
    if (!matches) {
        return APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED;
    }
    return APPLIANCE_SECRET_PROTECTION_OK;
}

enum appliance_secret_protection_status
appliance_secret_protection_export_at_rest(
    const struct appliance_secret_protection *protection,
    struct appliance_secret_protection_at_rest_record *record)
{
    enum appliance_secret_protection_status status;

    if (protection == NULL || record == NULL) {
        return APPLIANCE_SECRET_PROTECTION_NULL;
    }
    status = appliance_secret_protection_validate(protection);
    if (status != APPLIANCE_SECRET_PROTECTION_OK) {
        return status;
    }
    if (protection->state != APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED) {
        return APPLIANCE_SECRET_PROTECTION_EMPTY;
    }
    if (protection->kind !=
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL) {
        return APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED;
    }
    memset(record, 0, sizeof(*record));
    record->version = APPLIANCE_SECRET_PROTECTION_VERSION;
    record->kind = (uint32_t)protection->kind;
    record->generation = protection->generation;
    memcpy(record->fingerprint,
           protection->fingerprint,
           sizeof(record->fingerprint));
    return APPLIANCE_SECRET_PROTECTION_OK;
}

const char *appliance_secret_protection_status_string(
    enum appliance_secret_protection_status status)
{
    switch (status) {
    case APPLIANCE_SECRET_PROTECTION_OK:
        return "APPLIANCE_SECRET_PROTECTION_OK";
    case APPLIANCE_SECRET_PROTECTION_NULL:
        return "APPLIANCE_SECRET_PROTECTION_NULL";
    case APPLIANCE_SECRET_PROTECTION_BAD_VERSION:
        return "APPLIANCE_SECRET_PROTECTION_BAD_VERSION";
    case APPLIANCE_SECRET_PROTECTION_BAD_STATE:
        return "APPLIANCE_SECRET_PROTECTION_BAD_STATE";
    case APPLIANCE_SECRET_PROTECTION_BAD_KIND:
        return "APPLIANCE_SECRET_PROTECTION_BAD_KIND";
    case APPLIANCE_SECRET_PROTECTION_BAD_LENGTH:
        return "APPLIANCE_SECRET_PROTECTION_BAD_LENGTH";
    case APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL:
        return "APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL";
    case APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL:
        return "APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL";
    case APPLIANCE_SECRET_PROTECTION_EMPTY:
        return "APPLIANCE_SECRET_PROTECTION_EMPTY";
    case APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED:
        return "APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED";
    case APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH:
        return "APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH";
    case APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED:
        return "APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED";
    case APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED:
        return "APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED";
    case APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED:
        return "APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_secret_protection_kind_string(
    enum appliance_secret_protection_kind kind)
{
    switch (kind) {
    case APPLIANCE_SECRET_PROTECTION_KIND_NONE:
        return "APPLIANCE_SECRET_PROTECTION_KIND_NONE";
    case APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL:
        return "APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL";
    case APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN:
        return "APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN";
    case APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY:
        return "APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY";
    case APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET:
        return "APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET";
    default:
        return NULL;
    }
}

const char *appliance_secret_protection_state_string(
    enum appliance_secret_protection_state state)
{
    switch (state) {
    case APPLIANCE_SECRET_PROTECTION_STATE_EMPTY:
        return "APPLIANCE_SECRET_PROTECTION_STATE_EMPTY";
    case APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED:
        return "APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED";
    default:
        return NULL;
    }
}
