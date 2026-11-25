#ifndef HFOC_APPLIANCE_SSH_IDENTITY_H
#define HFOC_APPLIANCE_SSH_IDENTITY_H

#include "appliance_acme_control.h"
#include "appliance_certificate_name_store.h"
#include "appliance_production_certificate.h"
#include "appliance_state_loader.h"
#include "appliance_state_layout.h"
#include "appliance_state_block_device.h"
#include "hfoc_ssh_crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-601: durable SSH host identity and the one authorized diagnostic key.
 *
 * The records live in the genuinely free tail of the CERTIFICATE store, after
 * the TLS certificate (12 blocks), the certificate-name table (32) and the
 * ACME control record (1).  The offset is compiled from the three owners'
 * counts so a future growth moves it instead of overlapping it, and the
 * runtime bind checks the volume's own block_count.  A sealed volume refuses
 * the identity exactly like the other CERT owners: the private scalar is never
 * written in the clear.
 *
 * The host key is generated once from the production entropy owner, written,
 * flushed and read back.  A present-but-corrupt record is a named refusal, not
 * a reason to regenerate silently.  The authorized key is a single ECDSA
 * P-256 public blob enrolled locally; an absent record means no key may
 * authenticate.
 */
#define APPLIANCE_SSH_IDENTITY_VERSION 1U
#define APPLIANCE_SSH_RECORD_BYTES 512U

#define APPLIANCE_SSH_CERT_TAIL_BLOCK_OFFSET \
    (APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS + \
     APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS + \
     APPLIANCE_ACME_CONTROL_RECORD_BLOCKS)
#define APPLIANCE_SSH_HOST_KEY_RECORD_BLOCKS 4U
#define APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_BLOCKS 4U

enum appliance_ssh_identity_status {
    APPLIANCE_SSH_IDENTITY_OK = 0,
    APPLIANCE_SSH_IDENTITY_NULL = 1,
    APPLIANCE_SSH_IDENTITY_BAD_STATE = 2,
    APPLIANCE_SSH_IDENTITY_STORE_UNAVAILABLE = 3,
    APPLIANCE_SSH_IDENTITY_STORE_SEALED = 4,
    APPLIANCE_SSH_IDENTITY_STORE_TOO_SMALL = 5,
    APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED = 6,
    APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED = 7,
    APPLIANCE_SSH_IDENTITY_RECORD_CORRUPT = 8,
    APPLIANCE_SSH_IDENTITY_KEY_GENERATE_FAILED = 9,
    APPLIANCE_SSH_IDENTITY_KEY_INVALID = 10,
    APPLIANCE_SSH_IDENTITY_AUTHORIZED_MISSING = 11,
    APPLIANCE_SSH_IDENTITY_AUTHORIZED_INVALID = 12,
    APPLIANCE_SSH_IDENTITY_BUFFER_TOO_SMALL = 13,
};

struct appliance_ssh_identity {
    bool initialized;
    uint32_t version;
    bool store_bound;
    struct appliance_state_block_device host_store;
    struct appliance_state_block_device authorized_store;
    uint8_t host_scalar[32];
    uint8_t host_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    size_t host_blob_length;
    uint8_t host_fingerprint[32];
    uint8_t authorized_blob[192];
    size_t authorized_length;
    bool host_ready;
    bool authorized_ready;
    enum appliance_ssh_identity_status last_status;
};

void appliance_ssh_identity_init(struct appliance_ssh_identity *identity);
/*
 * Bind the CERT tail, load the host key (generating it once when the record
 * is genuinely absent), and load the authorized key if one was enrolled.
 * A host-key failure is a refusal; an absent/corrupt authorized record leaves
 * the host key usable but no client may authenticate until it is enrolled.
 */
enum appliance_ssh_identity_status appliance_ssh_identity_prepare(
    struct appliance_ssh_identity *identity,
    struct appliance_state_loader *loader,
    hfoc_ssh_rng_fn rng,
    void *rng_context);
enum appliance_ssh_identity_status appliance_ssh_identity_authorized_key_add(
    struct appliance_ssh_identity *identity,
    const uint8_t *blob,
    size_t blob_length);
enum appliance_ssh_identity_status appliance_ssh_identity_authorized_key_remove(
    struct appliance_ssh_identity *identity);
bool appliance_ssh_identity_host_ready(
    const struct appliance_ssh_identity *identity);
bool appliance_ssh_identity_authorized_ready(
    const struct appliance_ssh_identity *identity);
const uint8_t *appliance_ssh_identity_host_scalar(
    const struct appliance_ssh_identity *identity);
const uint8_t *appliance_ssh_identity_host_blob(
    const struct appliance_ssh_identity *identity,
    size_t *length);
const uint8_t *appliance_ssh_identity_host_fingerprint(
    const struct appliance_ssh_identity *identity);
const uint8_t *appliance_ssh_identity_authorized_blob(
    const struct appliance_ssh_identity *identity,
    size_t *length);
/*
 * ITEM-606: write one validated authorized-key blob into the installed state
 * and prove it by reading it back through the same record reader the installed
 * system uses.  This is the medium/installer import path: it binds the CERT
 * tail from an existing loader, never generates or touches the host key, and
 * clears its temporary identity before returning.  A caller that only has a
 * public key keeps the private key outside the appliance.
 */
enum appliance_ssh_identity_status appliance_ssh_identity_authorized_key_store(
    struct appliance_state_loader *loader,
    const uint8_t *blob, size_t blob_length);
/*
 * Console enrollment helpers.  `..._from_point_base64` accepts the 88-character
 * base64 of the 65-byte uncompressed P-256 point (the console's bounded input
 * cannot carry the 140-character OpenSSH blob), reconstructs and validates the
 * full SSH public-key blob and returns its SHA-256 fingerprint.  The text form
 * is the `SHA256:<base64>` the operator compares with `ssh-keygen -lf`.
 */
bool appliance_ssh_identity_blob_from_point_base64(
    const char *text, size_t length,
    uint8_t blob[192], size_t *blob_length, uint8_t fingerprint[32]);
size_t appliance_ssh_identity_fingerprint_text(
    const uint8_t fingerprint[32], char *output, size_t capacity);
const char *appliance_ssh_identity_status_string(
    enum appliance_ssh_identity_status status);

#endif
