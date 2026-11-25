#ifndef HFOC_APPLIANCE_PRODUCTION_SERVING_H
#define HFOC_APPLIANCE_PRODUCTION_SERVING_H

#include "appliance_management_reachability.h"
#include "appliance_management_tls.h"
#include "appliance_production_auth.h"
#include "appliance_production_certificate.h"
#include "appliance_production_management_plane.h"
#include "appliance_production_network.h"
#include "appliance_state_loader.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-386 production management serving path.
 *
 * This is the ordinary-boot owner of the management listener.  Before
 * ITEM-386 every piece of the serving path -- the lwIP TCP listener, the HTTP
 * parser/router/encoder, the HTTPS handler and the mbedTLS TLS 1.3 handshake --
 * was preprocessor-gated to QEMU and host-test builds, the production kernel
 * had no entropy owner and no certificate, and `scripts/check.sh` actively
 * forbade those symbols in the production ELF.  On an ordinary `make iso` boot
 * the appliance therefore served nothing at all.
 *
 * The composition is allocation-free and owns one file-scope state object:
 *
 *   - one lwIP listener on the production netif, bound to the appliance's own
 *     management address and the ITEM-362 port policy;
 *   - a fixed per-connection table sized to the mbedTLS connection budget and
 *     the lwIP backlog;
 *   - a non-blocking TLS 1.3 handshake driver with a bounded step count and a
 *     bounded deadline, plus a bounded per-connection RX buffer and a bounded
 *     response buffer;
 *   - the request path: framing and policy gates, the fixed route table,
 *     the ITEM-385 reachability gate and the ITEM-366/ITEM-365 production
 *     admission gates, and the existing static dispatchers.
 *
 * Admission order is deliberate and is the same order the QEMU fixture proves:
 * accept, complete the TLS handshake, then decide reachability for the real
 * peer address of the accepted connection, then the framing and policy gates,
 * then the request line and the route, then authorization, then the handler.
 * A peer the reachability policy refuses is answered exactly like an unknown
 * route, so it cannot learn that a management plane exists.
 *
 * The listener is raised only when the machine has an account, the network, the
 * entropy owner and a certificate.  Otherwise the module stays DEGRADED or
 * AWAITING_* and says why in its rendered record; the console keeps working.
 * The account term (ITEM-506 increment 5) applies only while the borrowed
 * state loader is ready: a boot with no initialized state volume has no durable
 * place an account could live in and keeps serving with a per-boot certificate
 * as before.
 */

#define APPLIANCE_PRODUCTION_SERVING_VERSION 1U

#define APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT \
    HFOC_MBEDTLS_CONNECTION_COUNT
#define APPLIANCE_PRODUCTION_SERVING_RX_MAX 4096U
#define APPLIANCE_PRODUCTION_SERVING_PLAINTEXT_CHUNK 256U
#define APPLIANCE_PRODUCTION_SERVING_HANDSHAKE_STEP_LIMIT 64U
#define APPLIANCE_PRODUCTION_SERVING_HANDSHAKE_DEADLINE_MS 20000U
#define APPLIANCE_PRODUCTION_SERVING_IDLE_TIMEOUT_MS 20000U
#define APPLIANCE_PRODUCTION_SERVING_WRITE_CHUNK 512U
/* Bound of the single-line `[PRODUCTION-SERVING]` record, NUL included. */
#define APPLIANCE_PRODUCTION_SERVING_RENDER_MAX 704U
#define APPLIANCE_PRODUCTION_SERVING_GENERATION 1U
/*
 * The V1 management port.  It is the port the ITEM-362 production network
 * profile publishes as its fallback port policy, and it is the only port the
 * listener will bind: hfoc_lwip_tcp_bind refuses every other requested
 * policy, so the serving path cannot open a second, unpoliced port.
 */
#define APPLIANCE_PRODUCTION_SERVING_PORT 443U

enum appliance_production_serving_state {
    APPLIANCE_PRODUCTION_SERVING_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_SERVING_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK = 2,
    APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_ENTROPY = 3,
    APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CERTIFICATE = 4,
    APPLIANCE_PRODUCTION_SERVING_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING = 6,
    APPLIANCE_PRODUCTION_SERVING_STATE_FAILED = 7,
    /*
     * ITEM-506 increment 5: the account the machine is gated on has not been
     * set yet.  The listener is withheld while the state volume is ready and
     * the one account (login + PBKDF2 verifier) does not exist; the console
     * conversation sets it and the next poll raises the listener.  Appended so
     * no existing state value moves.
     */
    APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CREDENTIAL = 8,
};

struct appliance_production_serving;

/*
 * Publish the serving composition.  `network`, `auth`, `plane`, `loader` and
 * `certificate` are borrowed and the serving module never owns them.
 * `tls_lifecycle` is the ITEM-375 lifecycle the boot certificate is imported
 * into: the serving module pushes the certificate metadata when the clock is
 * validated, which mutates that borrowed lifecycle exactly as its contract
 * requires.  It may be NULL, in which case the certificate is served but the
 * lifecycle record is left to its owner.
 */
enum appliance_production_serving_state appliance_production_serving_init(
    const struct appliance_production_network *network,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *plane,
    struct appliance_state_loader *loader,
    const struct appliance_management_tls *tls_lifecycle,
    struct appliance_production_certificate *certificate,
    uint32_t now_epoch_seconds);

/*
 * Advance the serving path.  Called from the ordinary-boot event loop with the
 * monotonic tick and the live wall-clock epoch (zero while unvalidated).  A
 * validated reading is consumed once and never regresses; the lifecycle import
 * the certificate needs is retried on every poll until it succeeds, because
 * the ITEM-374 SNTP sample can land after the listener has already bound.
 */
void appliance_production_serving_poll(
    uint32_t now_ms,
    uint32_t now_epoch_seconds);

/*
 * ITEM-655: true while a browser sign-in derivation is still owned by the
 * serving path.  The ordinary-boot loop uses this to keep advancing the
 * cooperative job instead of parking in `cpu_hlt()` until the next PIT edge;
 * every pass still returns to the normal loop body, so network, console and
 * watchdog work keep running between the measured slices.  It is false once
 * the job finished, was refused or was cancelled; a graceful peer FIN does
 * not cancel the bounded derivation early.
 */
bool appliance_production_serving_panel_auth_pending(void);

/*
 * ITEM-400: bind the ordinary-boot read-only management snapshot owner.  The
 * owner is borrowed and must stay alive for the lifetime of the serving path;
 * its four targets (`/api/v1/system`, `/api/v1/network`,
 * `/api/v1/network/status`, `/api/v1/service`, `/api/v1/health`) are then
 * resolved under the same ITEM-365 request gate as every other management
 * target.  Passing NULL or calling it before the serving path is published
 * leaves the serving path unchanged and reports FAILED.
 */
struct appliance_production_management_api;
enum appliance_production_serving_state
appliance_production_serving_publish_management_api(
    struct appliance_production_management_api *api);

/*
 * ITEM-445 step 2: bind the ITEM-401 administrator enrollment to the panel's
 * browser sign-in form.  The enrollment is borrowed and must stay alive for
 * the lifetime of the serving path; it is the one owner of the verifier, the
 * persisted-fingerprint check, the throttle, the token and the session, so
 * the form adds no second credential path.  Passing NULL or calling it before
 * the panel is published reports FAILED and leaves the form UNAVAILABLE.
 */
struct appliance_production_admin_enrollment;
enum appliance_production_serving_state
appliance_production_serving_bind_admin_enrollment(
    struct appliance_production_admin_enrollment *enrollment);

bool appliance_production_serving_listening(void);
enum appliance_production_serving_state
appliance_production_serving_state(void);
const char *appliance_production_serving_state_string(
    enum appliance_production_serving_state state);
const char *appliance_production_serving_reason(void);

/* Counters rendered by the production record. */
uint32_t appliance_production_serving_accept_count(void);
uint32_t appliance_production_serving_request_count(void);
uint32_t appliance_production_serving_denied_count(void);
uint32_t appliance_production_serving_rejected_count(void);
uint32_t appliance_production_serving_handshake_failure_count(void);
uint32_t appliance_production_serving_rx_callbacks(void);
uint32_t appliance_production_serving_rx_bytes(void);
uint32_t appliance_production_serving_tls_attempts(void);
/*
 * ITEM-503: how many operator certificates were imported and persisted into
 * the CERTIFICATE store by this boot, and whether one of them waits for the
 * reboot that applies it.
 */
uint32_t appliance_production_serving_certificate_imports(void);
bool appliance_production_serving_certificate_applies_on_reboot(void);

/*
 * ITEM-504 increment 5: hand over the `[PRODUCTION-ROUTE]` record body the
 * routing configuration left, exactly once.  False means "no record" (nothing
 * terminal happened yet, or the routing executor was never published) or "the
 * buffer cannot hold the whole body"; a buffer that is too small drops
 * nothing, so a later, larger call still reads the record.
 */
bool appliance_production_serving_take_route_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-504 increment 6: hand over the `[PRODUCTION-ROUTES]` body the boot's
 * route store produced, exactly once.  The body names the source the store
 * reported (`COMMITTED`, `DEFAULT_MISSING`, `DEFAULT_INVALID`,
 * `DEFAULT_UNAVAILABLE`) and the applied table as the model renders it.  False
 * means "nothing to publish" (the store was never read, or the record was
 * already handed over) or "the buffer cannot hold the whole body"; a buffer
 * that is too small publishes nothing and loses nothing.
 */
bool appliance_production_serving_take_route_restore_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-505: hand over the `[PRODUCTION-NET-SET]` record body the network
 * mutation left, exactly once, after a commit wrote the durable record or a
 * stage/cancel/refusal named its outcome.  The body names the source
 * (`PANEL`), the mode and, for a static configuration, the address, the
 * gateway and the DNS the operator confirmed; a commit that could not prove
 * its write says `persisted=FAILED` instead of claiming the record.  False
 * means "no record" or "the buffer cannot hold the whole body"; a buffer that
 * is too small drops nothing.
 */
#define APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX 288U
bool appliance_production_serving_take_network_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-505 increment 3: hand over the `[PRODUCTION-SYSTEM-SET]` record body the
 * hostname mutation left, exactly once, after a commit wrote the durable
 * record or a stage/cancel/refusal named its outcome.  The body names the
 * source (`PANEL`), the hostname the operator confirmed, whether the durable
 * write proved itself, that the change waits for the reboot, and the identity
 * the running system still serves.  False means "no record" or "the buffer
 * cannot hold the whole body"; a buffer that is too small drops nothing.
 */
#define APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX 224U
bool appliance_production_serving_take_system_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-506 increment 6: hand over the `[PRODUCTION-CREDENTIAL-SET]` record
 * body the credential change left, exactly once, after the commit wrote the
 * durable record or a failed or conflicted commit named its outcome.  A
 * refused stage and a cancel publish no record - there is no commit to report.
 * The body names
 * the source (`PANEL`), the login the operator confirmed, whether the login
 * moved, whether the durable write proved itself and whether the owner closed
 * the issued sessions.  It never carries the password or a hash of it.  False
 * means "no record" or "the buffer cannot hold the whole body"; a buffer that
 * is too small drops nothing.
 */
#define APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX 192U
bool appliance_production_serving_take_account_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-507 increment 3: one record per TLS handshake that SNI decided.
 *
 * The callback runs during the handshake, where the operator cannot see it, so
 * it leaves a bounded line for the ordinary-boot loop to publish:
 * `[PRODUCTION-TLS-SNI] name=<name|-> result=<EXACT|DEFAULT|REFUSED>
 * cert=<index|default|none> reason=<...>`.  `EXACT` names the index of the
 * declared entry whose certificate was selected, `DEFAULT` says the
 * appliance's own identity was served (no name was presented, or the record
 * declares the default certificate), and `REFUSED` names why the handshake was
 * aborted.  The record never carries certificate or key bytes.  False means
 * "no record" or "the buffer cannot hold the whole body"; a buffer that is too
 * small drops nothing.
 */
#define APPLIANCE_PRODUCTION_SERVING_SNI_RECORD_MAX 384U
bool appliance_production_serving_take_sni_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-507 increment 4: one record per name -> certificate/key mutation.
 *
 * `[PRODUCTION-CERTIFICATE-NAME] source=PANEL action=<IMPORT|DELETE|DEFAULT>
 * name=<name|-> names=<n> default=<REFUSE|CERTIFICATE> persisted=<OK|FAILED>
 * applies=reboot` names what the operator confirmed and whether the durable
 * write proved itself.  It never carries certificate or key bytes, and the
 * change waits for the reboot the running listener promises.  False means "no
 * record" or "the buffer cannot hold the whole body"; a buffer that is too
 * small drops nothing.
 */
#define APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX 256U
bool appliance_production_serving_take_certificate_name_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * ITEM-508 increment 34: the durable ACME control.
 *
 * `[PRODUCTION-ACME-CONTROL] source=PANEL action=<ENABLE|DISABLE|REQUEST>
 * enabled=<0|1> requested=<0|1> at=<epoch|-> persisted=<OK|FAILED>
 * applies=<service|reboot>` names what the operator confirmed and whether the
 * durable write proved itself.  It never carries a key, a nonce or a token,
 * and the record buffer refuses a body it cannot hold whole instead of
 * truncating it.
 */
#define APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX 224U
bool appliance_production_serving_take_acme_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * The committed control value the running service reads: whether ACME is
 * enabled and whether one issuance request is pending.  The pointer stays
 * owned by the serving composition and outlives the call; NULL means the
 * record was never loaded, which the caller reads as disabled and no request.
 */
const struct appliance_acme_control *appliance_production_serving_acme_control(void);

/*
 * ITEM-508 increment 35: consume the pending issuance request durably.
 *
 * The order path calls this the moment it really started an order, so the
 * request is cleared in the record and on the volume before the CA is spoken
 * to.  A failed durable write leaves the request pending and returns false;
 * the caller must not treat an order as requested-and-started in that case.
 */
bool appliance_production_serving_acme_take_request(void);

/*
 * The exact names the operator declared for this appliance, from the saved
 * ITEM-507 table - the identifiers an order is placed for.  The accessor
 * hands out borrowed pointers into the table; a count of zero means the
 * operator declared nothing and there is nothing to order for.
 */
uint32_t appliance_production_serving_acme_identifier_count(void);
bool appliance_production_serving_acme_identifier(
    uint32_t index,
    const char **name_out,
    size_t *name_length_out);

const struct appliance_management_reachability_selection *
appliance_production_serving_reachability(void);

/*
 * ITEM-505 increment 2: publish the live panel view the generic sections show.
 *
 * The ITEM-384 panel renders one borrowed view (hostname, management MAC,
 * service name, detail and recent log line) on every generic section.  The
 * caller - the ordinary-boot composition, which owns the configuration, the
 * network and the serving state - composes the bounded view and publishes it
 * here; the serving path stores the struct (the caller keeps owning the bytes
 * the pointers name) and hands it to the panel.  A publication before the
 * panel exists is kept and applied the moment the panel is initialized, so a
 * machine whose listener comes up on a later poll still serves live values.
 *
 * Passing NULL clears the view and returns the panel to its honest
 * `unavailable` rendering.  The returned state is the serving state, or
 * UNINITIALIZED when the serving path was never published.
 */
struct appliance_management_web_ui_view;
enum appliance_production_serving_state
appliance_production_serving_publish_panel_view(
    const struct appliance_management_web_ui_view *view);

/*
 * ITEM-508 increment 32: publish the read-only ACME state the Certificates
 * page shows.
 *
 * ITEM-508 point 10 gives the panel the ACME state as a reading: the service
 * state, the account's bound/refused verdict, the renewal decision with the
 * expiry and the attempt count.  The caller - the ordinary-boot composition,
 * which owns the ACME service and the serving path - composes the bounded view
 * from the service's own getters and publishes it here; the serving path
 * stores the struct (the caller keeps owning the bytes the pointers name),
 * copies it into the Certificates page view and republishes that page, so the
 * operator reads the state just published and never a stale one.
 *
 * Passing NULL clears the block and returns the page to its honest named
 * `NO_ACME_STATE` line.  The returned state is the serving state, or
 * UNINITIALIZED when the serving path was never published.
 */
struct appliance_management_web_ui_acme;
enum appliance_production_serving_state
appliance_production_serving_publish_acme(
    const struct appliance_management_web_ui_acme *acme);

/*
 * ITEM-505 increment 2: the bounded `[PRODUCTION-SERVING]` panel line.  It
 * carries the state, the listener reading and the panel publication state, and
 * is composed to fit the panel renderer's per-value bound so the page can
 * never cut it; the panel's detail field carries the state's reason and the
 * durable record keeps the whole reading.  The caller owns the buffer.
 */
#define APPLIANCE_PRODUCTION_SERVING_PANEL_LINE_MAX 96U
void appliance_production_serving_render_panel_line(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

/*
 * Render the single-line `[PRODUCTION-SERVING]` record.  The caller wraps the
 * write in serial_sequence_begin/end, so the record is one uninterruptible
 * line.
 */
void appliance_production_serving_render(
    char *output,
    uint32_t capacity,
    uint32_t *output_length);

#endif
