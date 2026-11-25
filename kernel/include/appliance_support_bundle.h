#ifndef HFOC_APPLIANCE_SUPPORT_BUNDLE_H
#define HFOC_APPLIANCE_SUPPORT_BUNDLE_H

#include "diagnostic_privacy.h"
#include "sha256.h"
#include "support_bundle_exporter.h"
#include "support_bundle_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-378 signed diagnostic bundle.
 *
 * The ITEM-167/168 support bundle carries a manifest and a terminal SHA-256
 * digest: it proves that the bytes were not damaged in transit and that the
 * declared sections are what they claim, but anyone who can produce the bytes
 * can produce the digest, and the manifest never states whether secret
 * configuration was withheld.  A support engineer therefore cannot tell a
 * genuine, redaction-checked bundle from a hand-assembled one.
 *
 * This module closes both gaps on top of the existing exporter, without
 * changing its format:
 *
 *   - redaction attestation: the ITEM-169 diagnostic-privacy policy is
 *     re-applied to the live configuration source, and the report (declared
 *     secret keys, included entries, withheld entries, leaked entries) is
 *     bound into the seal.  A single secret-classified entry that reached the
 *     payload is REFUSED (REDACTION_FAILED): no digest, no seal, no claim.
 *   - canonical binding: the exported bundle, its embedded manifest and the
 *     redaction report are re-derived from the actual bytes and encoded into
 *     one fixed-width canonical string.  A seal always covers exactly the
 *     bytes it was produced for.
 *   - authenticity: the binding is sealed with a keyed HMAC-SHA-256 through a
 *     caller-bound MAC boundary, so the appliance proves the bundle came from
 *     a holder of the seal key.  When no key is bound the bundle is exported
 *     as UNSIGNED with the exact reason instead of a fabricated signature.
 *
 * Honest limits, published rather than hidden:
 *
 *   - the frozen baseline has no on-device asymmetric signing identity: an
 *     ECDSA private key is not in the repository or in the kernel image, so
 *     this item seals with a keyed MAC and reports UNSIGNED when no seal key
 *     is bound.  Asymmetric signing of exported artifacts belongs to the
 *     release signing ceremony (ITEM-380);
 *   - a verifier that does not hold the seal key can check the binding
 *     self-consistency but not the authenticity: the module reports
 *     UNSIGNED/UNAVAILABLE for that case instead of a pass;
 *   - the MAC boundary is caller-bound so the composition, not this module,
 *     chooses the crypto provider; the production composition binds
 *     appliance_state_crypto_hmac_sha256.
 *
 * Allocation-free: the module only reads caller-owned buffers.
 */

#define APPLIANCE_SUPPORT_BUNDLE_VERSION 1U
#define APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID "HFOC-SB-BIND-1"
#define APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID_LENGTH 14U
#define APPLIANCE_SUPPORT_BUNDLE_MAGIC_SIZE 8U
#define APPLIANCE_SUPPORT_BUNDLE_PURPOSE 1U
#define APPLIANCE_SUPPORT_BUNDLE_DIGEST_SIZE SHA256_DIGEST_SIZE
#define APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE SHA256_DIGEST_SIZE
/* Canonical binding encoding: magic 8 + schema 14 + version 4 + exporter
 * version 4 + manifest version 4 + entry count 4 + bundle bytes 8 + payload
 * bytes 8 + bundle digest 32 + manifest digest 32 + policy id 11 + declared
 * keys 4 + config entries 4 + included 4 + withheld 4 + leaked 4 + verdict 4
 * + reserved 7. */
#define APPLIANCE_SUPPORT_BUNDLE_ENCODING_SIZE 160U
#define APPLIANCE_SUPPORT_BUNDLE_RENDER_MAX 320U

enum appliance_support_bundle_state {
    APPLIANCE_SUPPORT_BUNDLE_STATE_NONE = 0,
    APPLIANCE_SUPPORT_BUNDLE_STATE_UNAVAILABLE = 1,
    APPLIANCE_SUPPORT_BUNDLE_STATE_UNSIGNED = 2,
    APPLIANCE_SUPPORT_BUNDLE_STATE_SEALED = 3,
    APPLIANCE_SUPPORT_BUNDLE_STATE_REDACTION_FAILED = 4,
};

enum appliance_support_bundle_status {
    APPLIANCE_SUPPORT_BUNDLE_OK = 0,
    APPLIANCE_SUPPORT_BUNDLE_NULL = 1,
    APPLIANCE_SUPPORT_BUNDLE_BAD_STATE = 2,
    APPLIANCE_SUPPORT_BUNDLE_BAD_VERSION = 3,
    APPLIANCE_SUPPORT_BUNDLE_BAD_BINDING = 4,
    APPLIANCE_SUPPORT_BUNDLE_TOO_SMALL = 5,
    APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED = 6,
    APPLIANCE_SUPPORT_BUNDLE_UNSIGNED = 7,
    APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY = 8,
    APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE = 9,
    APPLIANCE_SUPPORT_BUNDLE_MISMATCH = 10,
    APPLIANCE_SUPPORT_BUNDLE_BUFFER_TOO_SMALL = 11,
};

enum appliance_support_bundle_scheme {
    APPLIANCE_SUPPORT_BUNDLE_SCHEME_NONE = 0,
    APPLIANCE_SUPPORT_BUNDLE_SCHEME_HMAC_SHA256 = 1,
};

/*
 * The redaction report.  `declared_keys` is the fixed deny-list size of the
 * ITEM-169 policy, `included` and `withheld` are the configuration entries
 * that did and did not reach the payload, and `leaked` counts entries that
 * are secret-classified and were nevertheless included (a policy failure).
 */
struct appliance_support_bundle_redaction {
    uint32_t version;
    char policy_id[SUPPORT_BUNDLE_MANIFEST_SECRET_POLICY_ID_LENGTH + 1U];
    uint32_t declared_keys;
    uint32_t config_entries;
    uint32_t included;
    uint32_t withheld;
    uint32_t leaked;
    bool policy_applied;
};

/*
 * The canonical binding.  Every field is re-derived from the exported bytes
 * or from the live source; nothing is copied on trust from the artifact.
 */
struct appliance_support_bundle_binding {
    uint32_t version;
    uint32_t exporter_version;
    uint32_t manifest_version;
    uint32_t entry_count;
    uint64_t bundle_bytes;
    uint64_t payload_bytes;
    uint8_t bundle_digest[SHA256_DIGEST_SIZE];
    uint8_t manifest_digest[SHA256_DIGEST_SIZE];
    struct appliance_support_bundle_redaction redaction;
};

/* The caller-bound MAC boundary.  The composition chooses the provider; the
 * module never links a crypto implementation of its own. */
typedef enum appliance_support_bundle_status
(*appliance_support_bundle_mac_fn)(
    void *context,
    const uint8_t *key,
    size_t key_length,
    const uint8_t *data,
    size_t data_length,
    uint8_t mac[APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE]);

struct appliance_support_bundle {
    uint32_t version;
    enum appliance_support_bundle_state state;
    enum appliance_support_bundle_scheme scheme;
    enum appliance_support_bundle_status last_status;
    struct appliance_support_bundle_binding binding;
    uint8_t digest[APPLIANCE_SUPPORT_BUNDLE_DIGEST_SIZE];
    uint8_t mac[APPLIANCE_SUPPORT_BUNDLE_MAC_SIZE];
    bool verified;
    uint32_t sealed_count;
    uint32_t unsigned_count;
    uint32_t refused_count;
};

/* Re-apply the diagnostic-privacy deny list to the live configuration. */
enum appliance_support_bundle_status appliance_support_bundle_redaction_eval(
    const struct support_bundle_manifest_source *source,
    struct appliance_support_bundle_redaction *report_out);
/* Re-derive the binding from the exported bundle and the redaction report. */
enum appliance_support_bundle_status appliance_support_bundle_bind(
    const void *exported,
    size_t exported_length,
    const struct appliance_support_bundle_redaction *report,
    struct appliance_support_bundle_binding *binding_out);
enum appliance_support_bundle_status appliance_support_bundle_encode(
    const struct appliance_support_bundle_binding *binding,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum appliance_support_bundle_status appliance_support_bundle_digest(
    const struct appliance_support_bundle_binding *binding,
    uint8_t digest_out[APPLIANCE_SUPPORT_BUNDLE_DIGEST_SIZE]);
/*
 * Seal the exported bundle.  A bundle whose redaction verdict is not clean is
 * refused before any MAC is computed.  Without a bound key the bundle is
 * published as UNSIGNED with the exact reason and no MAC.
 */
enum appliance_support_bundle_status appliance_support_bundle_seal(
    struct appliance_support_bundle *bundle,
    const void *exported,
    size_t exported_length,
    const struct appliance_support_bundle_redaction *report,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *key,
    size_t key_length);
/* Re-derive the binding, recompute the MAC and refuse any mismatch. */
enum appliance_support_bundle_status appliance_support_bundle_verify(
    struct appliance_support_bundle *bundle,
    const void *exported,
    size_t exported_length,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *key,
    size_t key_length);
bool appliance_support_bundle_is_sealed(
    const struct appliance_support_bundle *bundle);
uint32_t appliance_support_bundle_withheld(
    const struct appliance_support_bundle *bundle);
enum appliance_support_bundle_state appliance_support_bundle_state(
    const struct appliance_support_bundle *bundle);
enum appliance_support_bundle_status appliance_support_bundle_last_status(
    const struct appliance_support_bundle *bundle);
enum appliance_support_bundle_status appliance_support_bundle_render(
    const struct appliance_support_bundle *bundle,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_support_bundle_state_string(
    enum appliance_support_bundle_state state);
const char *appliance_support_bundle_status_string(
    enum appliance_support_bundle_status status);
const char *appliance_support_bundle_scheme_string(
    enum appliance_support_bundle_scheme scheme);
const char *appliance_support_bundle_schema_id(void);

#endif
