#ifndef HFOC_APPLIANCE_SSH_AUTHORIZED_KEY_H
#define HFOC_APPLIANCE_SSH_AUTHORIZED_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct appliance_state_loader;

/*
 * ITEM-606: the installer/medium import path for one SSH client public key.
 *
 * This module is deliberately independent of mbedTLS: the installer kernel
 * links the bounded state and SHA-256 owners but not the TLS closure.  It
 * validates the same 88-character base64 P-256 point the console accepts,
 * rebuilds the OpenSSH public blob, and writes the authorized-key record into
 * the CERT tail after the installed state exists.  The record layout mirrors
 * `appliance_ssh_identity.c`; the identity host proof reloads a record written
 * here, so the two cannot drift silently.
 */
#define APPLIANCE_SSH_AUTHORIZED_POINT_TEXT_LENGTH 88U
#define APPLIANCE_SSH_AUTHORIZED_BLOB_MAX 192U
#define APPLIANCE_SSH_AUTHORIZED_RECORD_BYTES 512U
/* CERT = 12 blocks, name table = 32, ACME control = 1, then host key = 4. */
#define APPLIANCE_SSH_AUTHORIZED_CERT_TAIL_BLOCK_OFFSET 45U
#define APPLIANCE_SSH_AUTHORIZED_HOST_KEY_RECORD_BLOCKS 4U
#define APPLIANCE_SSH_AUTHORIZED_RECORD_BLOCKS 4U

enum appliance_ssh_authorized_key_status {
    APPLIANCE_SSH_AUTHORIZED_KEY_OK = 0,
    APPLIANCE_SSH_AUTHORIZED_KEY_NULL = 1,
    APPLIANCE_SSH_AUTHORIZED_KEY_BAD_POINT = 2,
    APPLIANCE_SSH_AUTHORIZED_KEY_STORE_UNAVAILABLE = 3,
    APPLIANCE_SSH_AUTHORIZED_KEY_STORE_SEALED = 4,
    APPLIANCE_SSH_AUTHORIZED_KEY_STORE_TOO_SMALL = 5,
    APPLIANCE_SSH_AUTHORIZED_KEY_STORE_READ_FAILED = 6,
    APPLIANCE_SSH_AUTHORIZED_KEY_STORE_WRITE_FAILED = 7,
    APPLIANCE_SSH_AUTHORIZED_KEY_RECORD_CORRUPT = 8,
};

bool appliance_ssh_authorized_key_from_point(
    const char *text, size_t length,
    uint8_t blob[APPLIANCE_SSH_AUTHORIZED_BLOB_MAX], size_t *blob_length,
    uint8_t fingerprint[32]);
size_t appliance_ssh_authorized_key_fingerprint_text(
    const uint8_t fingerprint[32], char *output, size_t capacity);
enum appliance_ssh_authorized_key_status appliance_ssh_authorized_key_store(
    struct appliance_state_loader *loader,
    const uint8_t *blob, size_t blob_length);
const char *appliance_ssh_authorized_key_status_string(
    enum appliance_ssh_authorized_key_status status);

#endif
