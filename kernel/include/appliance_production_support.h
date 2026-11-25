#ifndef HFOC_APPLIANCE_PRODUCTION_SUPPORT_H
#define HFOC_APPLIANCE_PRODUCTION_SUPPORT_H

#include "appliance_management_bind.h"
#include "appliance_support_break_glass.h"
#include "appliance_support_bundle.h"
#include "appliance_support_session.h"
#include "support_bundle_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-378 production support plane.
 *
 * This caller-owned, allocation-free composition binds the three ITEM-378
 * boundaries into one ordinary-composition state the management plane can
 * publish:
 *
 *   - the signed diagnostic bundle (appliance_support_bundle): the exported
 *     bundle, its redaction attestation and its keyed seal;
 *   - the consent-based remote support session (appliance_support_session);
 *   - the documented break-glass recovery (appliance_support_break_glass)
 *     together with its effects: restoring the ITEM-374 management ingress
 *     policy to LOCAL_ONLY and revoking any remote support session.
 *
 * The plane borrows the management bind owner; it owns the session, the
 * break-glass gate and the bounded export buffer.  Readiness is honest:
 * READY only when a bundle was exported and sealed, DEGRADED when the bundle
 * is unsigned or a break-glass effect was partial, UNAVAILABLE when a
 * dependency is missing or the redaction verdict failed.
 *
 * The export buffer is sized to the worst case the ITEM-167/168 formats can
 * produce from the frozen bounds:
 *   header 64 + manifest 512 + digest 32
 *   + metrics 245 + status (101 + 16 * APPLIANCE_STATUS_SERVICE_MAX = 229)
 *   + health 336 + audit (KERNEL_EVENT_RING_CAPACITY * 52 = 3328)
 *   + config (CONFIG_KV_CAPACITY_MAX * (12 + CONFIG_KV_KEY_MAX +
 *     CONFIG_KV_VALUE_MAX) = 9088) + versions 35
 *   = 13869 bytes, rounded up to 14080.
 */

#define APPLIANCE_PRODUCTION_SUPPORT_VERSION 1U
#define APPLIANCE_PRODUCTION_SUPPORT_SCHEMA_ID "HFOC-PRODUCTION-SUPPORT-1"
#define APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_MAX 14080U
#define APPLIANCE_PRODUCTION_SUPPORT_RENDER_MAX 520U
#define APPLIANCE_PRODUCTION_SUPPORT_SEAL_KEY_MAX 64U

enum appliance_production_support_state {
    APPLIANCE_PRODUCTION_SUPPORT_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE = 2,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED = 3,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_READY = 4,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED = 5,
    APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED = 6,
};

enum appliance_production_support_status {
    APPLIANCE_PRODUCTION_SUPPORT_OK = 0,
    APPLIANCE_PRODUCTION_SUPPORT_NULL = 1,
    APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_SUPPORT_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_SUPPORT_DEGRADED = 5,
    APPLIANCE_PRODUCTION_SUPPORT_DENIED = 6,
    APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_TOO_LARGE = 7,
    APPLIANCE_PRODUCTION_SUPPORT_BUFFER_TOO_SMALL = 8,
};

struct appliance_production_support {
    bool initialized;
    uint32_t version;
    enum appliance_production_support_state state;
    enum appliance_production_support_status last_status;
    struct appliance_support_bundle bundle;
    struct appliance_support_session session;
    struct appliance_support_break_glass break_glass;
    struct appliance_management_bind *management_bind;
    bool bundle_published;
    uint64_t bundle_bytes;
    uint32_t bundle_sections;
    uint32_t bundle_withheld;
    uint32_t bundle_count;
    uint32_t break_glass_effect_count;
    uint32_t break_glass_partial_count;
    unsigned char bundle_buffer[APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_MAX];
};

enum appliance_production_support_status appliance_production_support_init(
    struct appliance_production_support *support,
    struct appliance_management_bind *management_bind);
/*
 * Export, redaction-check and seal the diagnostic bundle from a live source.
 * The bundle is never published unsealed and un-attested: a redaction
 * failure denies, a missing seal key degrades to UNSIGNED, a missing MAC
 * provider reports UNAVAILABLE.
 */
enum appliance_production_support_status
appliance_production_support_publish_bundle(
    struct appliance_production_support *support,
    const struct support_bundle_manifest_source *source,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *seal_key,
    size_t seal_key_length);
enum appliance_production_support_status
appliance_production_support_session_request(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t requester_id,
    uint32_t scopes,
    uint64_t now_tick);
enum appliance_production_support_status
appliance_production_support_session_consent(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t ttl_ticks,
    uint64_t now_tick);
enum appliance_production_support_status
appliance_production_support_session_revoke(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t now_tick);
enum appliance_production_support_status
appliance_production_support_break_glass_arm(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint32_t principal_id,
    uint32_t actions,
    uint64_t now_tick,
    uint64_t *token_out);
/* Apply the armed break-glass actions and perform their effects. */
enum appliance_production_support_status
appliance_production_support_break_glass_apply(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint64_t token,
    uint64_t now_tick,
    uint32_t *actions_out);
enum appliance_production_support_status
appliance_production_support_break_glass_cancel(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint64_t now_tick);
enum appliance_production_support_status appliance_production_support_refresh(
    struct appliance_production_support *support,
    uint64_t now_tick);
enum appliance_production_support_status appliance_production_support_stop(
    struct appliance_production_support *support,
    uint64_t now_tick);
const struct appliance_support_bundle *
appliance_production_support_bundle(
    const struct appliance_production_support *support);
const struct appliance_support_session *
appliance_production_support_session(
    const struct appliance_production_support *support);
const struct appliance_support_break_glass *
appliance_production_support_break_glass(
    const struct appliance_production_support *support);
bool appliance_production_support_bundle_published(
    const struct appliance_production_support *support);
uint64_t appliance_production_support_bundle_bytes(
    const struct appliance_production_support *support);
uint32_t appliance_production_support_sections(
    const struct appliance_production_support *support);
uint32_t appliance_production_support_withheld(
    const struct appliance_production_support *support);
uint32_t appliance_production_support_break_glass_effects(
    const struct appliance_production_support *support);
enum appliance_production_support_state appliance_production_support_state(
    const struct appliance_production_support *support);
enum appliance_production_support_status
appliance_production_support_last_status(
    const struct appliance_production_support *support);
enum appliance_production_support_status appliance_production_support_render(
    const struct appliance_production_support *support,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_production_support_state_string(
    enum appliance_production_support_state state);
const char *appliance_production_support_status_string(
    enum appliance_production_support_status status);
const char *appliance_production_support_schema_id(void);

#endif
