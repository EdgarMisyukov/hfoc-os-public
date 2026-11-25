#include "appliance_production_serving.h"

#ifdef HFOC_PRODUCTION_TLS

#include "appliance_acme_control.h"
#include "appliance_certificate_import.h"
#include "appliance_certificate_name_store.h"
#include "appliance_certificate_upload.h"
#include "appliance_health.h"
#include "appliance_host_seed.h"
#include "appliance_identity.h"
#include "appliance_language.h"
#include "appliance_management_api.h"
#include "appliance_management_panel.h"
#include "appliance_management_authorization.h"
#include "appliance_management_web_ui.h"
#include "appliance_network_config.h"
#include "appliance_panel_auth_trace.h"
#include "appliance_panel_form.h"
#include "appliance_panel_gate.h"
#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_production_admin_enrollment.h"
#include "appliance_production_entropy.h"
#include "appliance_production_management_api.h"
#include "appliance_production_management_plane.h"
#include "appliance_production_mutation_plane.h"
#include "appliance_production_network_wire.h"
#include "appliance_route_config.h"
#include "appliance_route_store.h"
#include "appliance_state_loader.h"
#include "arch/x86_64/timer.h"
#include "ipv4_config.h"
#include "hfoc_lwip_time.h"
#include "hfoc_mbedtls_alloc.h"
#include "hfoc_mbedtls_bio.h"
#include "http_error_matrix.h"
#include "hfoc_lwip_tcp.h"
#include "http_parser.h"
#include "http_response.h"
#include "https_handler.h"
#include "pem_decode.h"

#include "hfoc_lwip.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-652: the completed panel-auth record and its KDF/wait counters must use
 * one clock.  The lwIP millisecond counter is tick-driven and can lag the
 * calibrated cycle clock under TCG, which made the old duration_ms smaller
 * than the work it described.  These records do not drive timeouts; they only
 * publish measurements, so the monotonic cycle clock is the honest source.
 */
static uint32_t appliance_production_serving_panel_auth_now_ms(void)
{
    const uint64_t millis = timer_real_milliseconds();

    return millis > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)millis;
}

#define APPLIANCE_PRODUCTION_SERVING_TLS_VERSION_MAX 16U
#define APPLIANCE_PRODUCTION_SERVING_TLS_CIPHER_MAX 48U
#define APPLIANCE_PRODUCTION_SERVING_REASON_MAX 48U
#define APPLIANCE_PRODUCTION_SERVING_REACHABILITY_INTERFACE \
    APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_MANAGEMENT
/*
 * ITEM-504 increment 8: the facade relay.  `REQUEST_RAW_MAX` bounds the raw
 * request head the relay forwards to the backend (the parser's own limits,
 * with room for the final CRLF); a body is forwarded from the already bounded
 * `connection->body`, so no second body buffer exists.
 */
#define APPLIANCE_PRODUCTION_SERVING_REQUEST_RAW_MAX \
    (HTTP_PARSER_REQUEST_LINE_MAX + HTTP_PARSER_HEADERS_MAX + \
     APPLIANCE_PRODUCTION_SERVING_PLAINTEXT_CHUNK + 8U)
#define APPLIANCE_PRODUCTION_SERVING_RELAY_BUFFER_MAX 1024U
/*
 * The answer the backend sends is staged whole, because lwIP hands the relay
 * one delivered pbuf at a time and a pbuf is not bounded by our own TCP_MSS:
 * the QEMU user-network link delivers 1440-byte segments into a stack that
 * advertises 536, and a staging buffer smaller than the pbuf would refuse the
 * first segment forever.  A full receive window plus one segment is the
 * honest bound: lwIP never lets a peer keep more than TCP_WND unacknowledged,
 * and no single segment can exceed one MSS of that window.
 */
#define APPLIANCE_PRODUCTION_SERVING_RELAY_CLIENT_BUFFER_MAX (TCP_WND + TCP_MSS)

struct appliance_production_serving_connection {
    bool active;
    bool releasing;
    bool ssl_setup;
    bool handshake_done;
    /*
     * ITEM-507 increment 3: true once this connection's own handshake invoked
     * the SNI callback.  The callback finds the connection by its ssl context,
     * so an interleaved handshake on another connection cannot claim this
     * one's decision.
     */
    bool sni_present;
    bool parser_started;
    bool request_line_done;
    bool request_parsed;
    bool malformed;
    bool peer_closed;
    bool closing;
    bool response_ready;
    bool response_sent;
    bool close_notify_sent;
    /* ITEM-619: the panel credential job owns this request until it finishes. */
    bool panel_auth_pending;
    bool panel_auth_resuming;
    bool response_head;
    /*
     * ITEM-445 step 2: the bounded sign-in body. The receive loop stops
     * completing the request at the end of the headers when framing published
     * a body, collects exactly `body_expected` bytes, and only then calls the
     * completion path.
     */
    bool body_pending;
    uint32_t body_length;
    uint32_t body_expected;
    uint32_t index;
    uint32_t allocator_id;
    uint32_t last_activity_ms;
    uint32_t response_length;
    uint32_t response_offset;
    uint32_t response_body_length;
    struct tcp_pcb *pcb;
    struct http_parser_token parser_token;
    struct hfoc_mbedtls_bio_token bio_token;
    uint8_t rx[APPLIANCE_PRODUCTION_SERVING_RX_MAX];
    uint8_t body[HTTP_FRAMING_BODY_MAX];
    size_t rx_length;
    size_t rx_offset;
    uint8_t response[HTTP_RESPONSE_BUFFER_MAX];
    mbedtls_ssl_context ssl;
    /*
     * ITEM-504 increment 7: the route the request named.  The facade resolves
     * the `Host` header once, before any dispatch, and keeps the answer - and
     * the reason when there is none - on the connection: the relay that leads
     * the request to its backend must not guess a name, and a request that
     * names no declared route is refused with the reason that names it.
     */
    struct appliance_route route;
    enum appliance_route_status route_status;
    bool route_resolved;
    /*
     * ITEM-504 increment 8: the relay of a request the facade was addressed
     * by.  The raw head is kept while the request is parsed, because the
     * parser publishes fields, not bytes, and the backend must receive the
     * request the client wrote; the body lives in `body` and is forwarded
     * with the head.  Every buffer is bounded, and a refusal never leaves a
     * target behind.
     */
    bool facade;
    bool relay_started;
    bool target_connected;
    bool target_eof;
    bool client_eof;
    bool target_shutdown;
    bool client_shutdown;
    bool request_staged;
    struct tcp_pcb *target_pcb;
    size_t request_raw_length;
    size_t request_head_length;
    size_t forward_head_offset;
    size_t forward_body_offset;
    size_t to_target_length;
    size_t to_target_offset;
    size_t to_client_length;
    size_t to_client_offset;
    uint32_t relay_last_progress_ms;
    uint8_t request_raw[APPLIANCE_PRODUCTION_SERVING_REQUEST_RAW_MAX];
    uint8_t to_target[APPLIANCE_PRODUCTION_SERVING_RELAY_BUFFER_MAX];
    uint8_t to_client[APPLIANCE_PRODUCTION_SERVING_RELAY_CLIENT_BUFFER_MAX];
};

struct appliance_production_serving_connection;

struct appliance_production_serving {
    bool initialized;
    uint32_t version;
    enum appliance_production_serving_state state;
    const char *reason;
    const struct appliance_production_network *network;
    struct appliance_production_auth *auth;
    struct appliance_production_management_plane *plane;
    /* ITEM-400: the ordinary-boot read-only management snapshot owner. */
    struct appliance_production_management_api *management_api;
    /*
     * ITEM-445 step 2: the borrowed ITEM-401 enrollment the browser sign-in
     * form consults. It is stored before the panel exists and applied as soon
     * as the panel is published, because the serving path is published on the
     * first poll that has a live address, not at boot.
     */
    struct appliance_production_admin_enrollment *admin_enrollment;
    /*
     * ITEM-619: one cooperative browser sign-in.  The job holds only
     * secret-derived KDF state; the connection keeps the request body and is
     * resumed by the poll once the job finishes.  A second sign-in while one
     * is running is refused instead of sharing the state.
     */
    struct {
        bool active;
        bool result_ready;
        /* The connection whose request owns the job; NULL when idle. */
        struct appliance_production_serving_connection *owner;
        /*
         * ITEM-650: the next slice starts at the proven minimum and is
         * re-chosen from each slice's measured cost, so the job spends a
         * bounded share of a turn instead of one fixed 64 rounds per wake.
         */
        uint32_t slice_rounds;
        /*
         * ITEM-652: the end of the previous measured slice, so the next call
         * can report how long the job waited for the serving loop instead of
         * folding that wait into the derivation.  `slice_seen` keeps the
         * first slice's wait at zero.
         */
        uint64_t last_slice_end_cycles;
        bool slice_seen;
        struct appliance_admin_credential_check check;
    } panel_auth;
    struct appliance_production_certificate *certificate;
    /*
     * ITEM-503: the two-phase certificate import.  The mutation plane owns the
     * authorization, the single-use token, the window and the audit; the
     * upload composition owns the decoded candidate and the store write.
     */
    struct appliance_production_mutation_plane mutations;
    bool mutations_published;
    struct appliance_certificate_upload certificate_upload;
    bool certificate_upload_published;
    uint32_t certificate_import_count;
    bool certificate_pending_reboot;
    /*
     * ITEM-504: the routing-configuration mutation.  The same plane owns the
     * two-phase state; the route configuration owns the candidate pair behind
     * the applied table.
     */
    struct appliance_route_config route_config;
    bool routing_published;
    /*
     * ITEM-504 increment 6: the durable table.  The store is bound and read
     * once at init, and the commit writes the candidate through it before the
     * applied table moves, so the panel never shows a route the next boot
     * would not carry.
     */
    struct appliance_route_store route_store;
    bool route_store_loaded;
    bool route_restore_taken;
    uint32_t route_mutation_count;
    uint32_t route_mutation_pending_count;
    uint32_t route_mutation_refused_count;
    /*
     * ITEM-505: the network-configuration mutation.  The plane keeps the
     * token, the window and the audit; this composition keeps the staged
     * candidate, writes it through the durable network record before it
     * answers OK, and hands the `[PRODUCTION-NET-SET]` record to the loop.
     * The commit does not rebind the live interface: the machine applies the
     * saved configuration at the next boot, and the page says so.
     */
    struct {
        bool staged;
        enum appliance_network_config_mode mode;
        struct network_ipv4_config static_config;
    } network_candidate;
    bool network_published;
    bool network_record_pending;
    /*
     * ITEM-515: the named refusal of the last network stage attempt.  The form
     * grammar names the field that failed; the HTTP layer quotes that name in
     * the refusal document instead of one generic word, and the pointer is
     * cleared before every plane request so a commit's own failure never wears
     * a stage refusal's name.
     */
    const char *network_refusal;
    uint32_t network_mutation_count;
    uint32_t network_mutation_pending_count;
    uint32_t network_mutation_refused_count;
    char network_record[APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX];
    char network_active_text[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX];
    char network_saved_text[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX];
    /*
     * ITEM-505 increment 3: the system-hostname mutation.  The plane keeps the
     * token, the window and the audit; this composition keeps the staged name,
     * writes it through the durable system record before it answers OK, and
     * hands the `[PRODUCTION-SYSTEM-SET]` record to the loop.  The commit does
     * not rebind the running identity: the next boot serves the saved name,
     * and the page says so.
     */
    struct {
        /*
         * ITEM-509 increment 6: the same two-phase system mutation carries one
         * field, either the hostname or the interface language.  Both write the
         * same durable system record, so they share the token, the window and
         * the audit; the stage decides which one was typed and the commit
         * writes exactly that one.
         */
        bool staged;
        bool language_staged;
        enum appliance_language language;
        char hostname[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    } system_candidate;
    bool system_published;
    bool system_record_pending;
    uint32_t system_mutation_count;
    uint32_t system_mutation_pending_count;
    uint32_t system_mutation_refused_count;
    char system_record[APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX];
    /* The identity this boot resolved, and the record's current reading. */
    char system_active_text[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    char system_saved_text[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    /*
     * ITEM-509 increment 5: the language pair the System page shows.  Both
     * renderings are bounded (the longest language code is two bytes) and are
     * composed at render time, so the page cannot show a value the owner did
     * not just read.
     */
    char system_language_active_text[16];
    char system_language_saved_text[16];
    /*
     * ITEM-506 increment 6: the credential change.  The plane keeps the token,
     * the window and the audit; this composition keeps the one staged
     * candidate - the new login and the new password, proven against the
     * current password - until the exact token comes back, and hands the
     * `[PRODUCTION-CREDENTIAL-SET]` record to the loop.  The candidate's
     * password lives only for the confirmation window and is zeroed by the
     * commit, by a rollback and by a new stage.
     */
    struct {
        bool staged;
        bool login_changed;
        char login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
        char current_password
            [APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
        char password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
        /*
         * The stored record the stage proved, captured so the commit can
         * refuse a candidate whose account another writer changed in the
         * confirmation window instead of silently reverting that change.
         */
        bool snapshot_valid;
        char snapshot_login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
        uint8_t snapshot_salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
        uint32_t snapshot_iterations;
        uint8_t snapshot_verifier[APPLIANCE_ADMIN_CREDENTIAL_VERIFIER_LENGTH];
    } account_candidate;
    bool account_published;
    bool account_record_pending;
    uint32_t account_mutation_count;
    uint32_t account_mutation_pending_count;
    uint32_t account_mutation_refused_count;
    /* ITEM-507 increment 4: the certificate-name mutation's own counters. */
    uint32_t certificate_name_mutation_count;
    uint32_t certificate_name_mutation_pending_count;
    uint32_t certificate_name_mutation_refused_count;
    char account_record[APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX];
    /* The stored login the Account page shows; never the password. */
    char account_saved_text[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    /*
     * ITEM-507 increment 5: the two bounded table renderings the Certificates
     * page shows.  The renderings name each declared entry and its
     * certificate's byte count; no key byte and no certificate body is ever
     * part of them.
     */
    char certificates_active_text[
        APPLIANCE_MANAGEMENT_WEB_UI_CERTIFICATES_TEXT_MAX];
    char certificates_saved_text[
        APPLIANCE_MANAGEMENT_WEB_UI_CERTIFICATES_TEXT_MAX];
    const struct appliance_management_tls *tls_lifecycle;
    struct appliance_state_loader *loader;
    bool tls_ready;
    bool listener_published;
    struct appliance_management_reachability_selection reachability;
    bool reachability_published;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_tcp_callback listener_callback;
    struct appliance_production_serving_connection
        connections[APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT];
    uint32_t active_count;
    uint32_t accept_count;
    uint32_t request_count;
    uint32_t denied_count;
    uint32_t rejected_count;
    uint32_t handshake_failure_count;
    uint32_t reachability_denied_count;
    uint32_t route_root_count;
    uint32_t route_health_count;
    uint32_t route_api_count;
    /* ITEM-503: how many times the public certificate document was served. */
    uint32_t route_certificate_count;
    uint32_t not_found_count;
    uint32_t method_denied_count;
    uint32_t unauthorized_count;
    uint32_t close_count;
    uint32_t rx_callbacks;
    uint32_t rx_bytes;
    uint32_t tls_attempts;
    /* ITEM-619: the last bounded TLS failure, for the serial record only. */
    uint32_t tls_error_stage;
    int32_t tls_error;
    struct http_parser parser;
    struct appliance_management_panel panel;
    bool panel_published;
    /*
     * ITEM-505 increment 2: the live view the generic sections show.  The
     * ordinary-boot composition owns the bytes every pointer names and
     * republishes the struct as its owners move; this module only keeps the
     * borrowed struct and hands it to the panel.
     */
    struct appliance_management_web_ui_view panel_view;
    bool panel_view_published;
    /*
     * ITEM-508 increment 32: the read-only ACME state block of the
     * Certificates page.  The ordinary-boot composition owns the bytes every
     * pointer names; this module keeps the borrowed struct and copies it into
     * the page view whenever the certificates view is (re)published.
     */
    struct appliance_management_web_ui_acme acme;
    bool acme_published;
    uint32_t panel_denied_count;
    uint32_t panel_served_count;
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy bio_policy;
    mbedtls_ssl_config ssl_config;
    mbedtls_x509_crt certificate_chain;
    mbedtls_pk_context certificate_key;
    bool crypto_published;
    bool ssl_config_published;
    bool certificate_published;
    bool key_published;
    char tls_version[APPLIANCE_PRODUCTION_SERVING_TLS_VERSION_MAX];
    char tls_cipher[APPLIANCE_PRODUCTION_SERVING_TLS_CIPHER_MAX];
    bool tls_measured;
    uint32_t handshake_count;
    uint32_t tls_lifecycle_status;
    bool tls_lifecycle_promoted;
    uint32_t now_epoch_seconds;
    /*
     * ITEM-507 increment 3: the names the operator declared, one parsed
     * certificate and key per name, and the record the handshake leaves for
     * the ordinary-boot loop.  The table and the parsed pairs are static
     * members of the one serving owner: the SNI callback runs inside the
     * handshake and must not allocate.
     */
    struct appliance_certificate_name_store name_store;
    struct appliance_certificate_name_table name_table;
    mbedtls_x509_crt name_chain[APPLIANCE_CERTIFICATE_NAME_MAX_COUNT];
    mbedtls_pk_context name_key[APPLIANCE_CERTIFICATE_NAME_MAX_COUNT];
    bool name_usable[APPLIANCE_CERTIFICATE_NAME_MAX_COUNT];
    bool name_store_ready;
    char sni_record[APPLIANCE_PRODUCTION_SERVING_SNI_RECORD_MAX];
    bool sni_record_pending;
    /*
     * ITEM-507 increment 4: the durable table as last read or written, so a
     * second mutation builds on the first one's result and the panel can show
     * the saved value while the running listener keeps the boot-time table.
     */
    struct appliance_certificate_name_table name_saved;
    struct appliance_production_serving_certificate_name_candidate {
        bool staged;
        uint32_t action;
        char name[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
        uint32_t name_length;
        uint8_t certificate_der[APPLIANCE_CERTIFICATE_NAME_DER_MAX];
        uint32_t certificate_length;
        uint8_t key_der[APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX];
        uint32_t key_length;
        bool make_default;
    } certificate_name_candidate;
    char certificate_name_record[
        APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX];
    bool certificate_name_record_pending;
    bool certificate_name_published;
    /*
     * ITEM-508 increment 34: the durable ACME control record and the one
     * two-phase candidate.  The store is the same CERTIFICATE-namespace store
     * the certificate record and the name table live in; this composition
     * binds only its own block range.  `acme_control` is the value as last
     * read or written, so a second mutation builds on the first one's result
     * and the running service reads the committed setting.
     */
    struct appliance_acme_control_store acme_control_store;
    struct appliance_acme_control acme_control;
    bool acme_control_loaded;
    struct appliance_production_serving_acme_candidate {
        bool staged;
        uint32_t action;
    } acme_candidate;
    char acme_requested_at_text[16];
    char acme_record[APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX];
    bool acme_record_pending;
    bool acme_executor_bound;
    uint32_t acme_mutation_count;
    uint32_t acme_mutation_pending_count;
    uint32_t acme_mutation_refused_count;
    /*
     * ITEM-516: the management gate.  The plane keeps the token, the window
     * and the audit; this composition keeps the one staged candidate - the
     * panel can only ever stage the safe direction - the refusal word the
     * stage produced, and the view the gate page renders.  Nothing here can
     * open the gate: opening is the local console's.
     */
    struct {
        bool staged;
        bool open;
    } gate_candidate;
    const char *gate_refusal;
    bool gate_published;
    struct appliance_management_web_ui_gate gate;
    bool gate_executor_bound;
    uint32_t gate_mutation_count;
    uint32_t gate_mutation_pending_count;
    uint32_t gate_mutation_refused_count;
};

/* ITEM-655: one event-loop pass spends at most this much cooperative KDF work
 * before returning to the network/console/watchdog body.  The slice cap also
 * bounds the burst if the calibrated cycle clock is unavailable. */
#define APPLIANCE_PRODUCTION_SERVING_PANEL_AUTH_BURST_US 100000U
#define APPLIANCE_PRODUCTION_SERVING_PANEL_AUTH_BURST_SLICES 4U

static struct appliance_production_serving appliance_production_serving_owner;

/* ------------------------------------------------------------------ helpers */

static void appliance_production_serving_zero(
    void *buffer,
    size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)buffer;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_production_serving_copy_string(
    char *destination,
    uint32_t capacity,
    const char *source)
{
    uint32_t index = 0U;

    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source != NULL) {
        while (source[index] != '\0' && index + 1U < capacity) {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

static bool appliance_production_serving_target_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *expected,
    uint32_t expected_length)
{
    uint32_t index;

    if (target == NULL || target_length != expected_length) {
        return false;
    }
    for (index = 0U; index < target_length; ++index) {
        if (target[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-503: the active certificate as a PEM document.
 *
 * A browser or an operating system imports a certificate from a file, and PEM
 * is the form every trust-store dialog accepts; the DER stays the wire form.
 * The encoder is local because the frozen mbedTLS profile deliberately has no
 * base64, and the document is bounded by the DER bound: 1024 bytes become at
 * most 1368 base64 characters plus the armour and line breaks.
 */
#define APPLIANCE_PRODUCTION_SERVING_PEM_MAX 1600U

static void appliance_production_serving_release(
    struct appliance_production_serving_connection *connection,
    bool graceful);
static void appliance_production_serving_relay_forget_target(
    struct appliance_production_serving_connection *connection);
static void appliance_production_serving_encode(
    struct appliance_production_serving_connection *connection,
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length,
    bool head);

static uint32_t appliance_production_serving_pem_encode(
    const uint8_t *der,
    uint32_t der_length,
    char *output,
    uint32_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char header[] = "-----BEGIN CERTIFICATE-----\n";
    static const char footer[] = "-----END CERTIFICATE-----\n";
    uint32_t written = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < sizeof(header) - 1U; ++index) {
        if (written + 1U >= capacity) {
            return 0U;
        }
        output[written++] = header[index];
    }
    for (index = 0U; index < der_length; index += 3U) {
        const uint32_t remaining = der_length - index;
        const uint32_t triple =
            ((uint32_t)der[index] << 16U) |
            (remaining > 1U ? ((uint32_t)der[index + 1U] << 8U) : 0U) |
            (remaining > 2U ? (uint32_t)der[index + 2U] : 0U);
        char block[4U];

        block[0U] = alphabet[(triple >> 18U) & 0x3FU];
        block[1U] = alphabet[(triple >> 12U) & 0x3FU];
        block[2U] = remaining > 1U ? alphabet[(triple >> 6U) & 0x3FU] : '=';
        block[3U] = remaining > 2U ? alphabet[triple & 0x3FU] : '=';
        for (uint32_t part = 0U; part < 4U; ++part) {
            if (written + 1U >= capacity) {
                return 0U;
            }
            output[written++] = block[part];
        }
        if (((index / 3U) + 1U) % 16U == 0U) {
            if (written + 1U >= capacity) {
                return 0U;
            }
            output[written++] = '\n';
        }
    }
    if (der_length != 0U && (der_length % 48U) != 0U) {
        if (written + 1U >= capacity) {
            return 0U;
        }
        output[written++] = '\n';
    }
    for (index = 0U; index < sizeof(footer) - 1U; ++index) {
        if (written + 1U >= capacity) {
            return 0U;
        }
        output[written++] = footer[index];
    }
    output[written] = '\0';
    return written;
}

/*
 * ITEM-503: the total length of one DER certificate at the head of `der`,
 * from the outer SEQUENCE tag and length.  It is the same walk the import
 * gate uses to split a chain bundle; a length that is malformed, indefinite
 * or longer than the buffer returns zero, so the caller refuses the bundle
 * instead of handing mbedTLS a truncated certificate.
 */
static size_t appliance_production_serving_der_sequence_length(
    const uint8_t *der,
    size_t available)
{
    size_t length = 0U;
    size_t index;
    uint8_t count;

    if (der == NULL || available < 2U || der[0] != 0x30U) {
        return 0U;
    }
    if (der[1] < 0x80U) {
        return 2U + (size_t)der[1];
    }
    count = (uint8_t)(der[1] & 0x7FU);
    if (count == 0U || count > 4U || available < 2U + (size_t)count) {
        return 0U;
    }
    for (index = 0U; index < count; ++index) {
        length = (length << 8U) | (size_t)der[2U + index];
    }
    if (length > available - 2U - (size_t)count) {
        return 0U;
    }
    return 2U + (size_t)count + length;
}

static void appliance_production_serving_certificate_response(
    struct appliance_production_serving_connection *connection,
    bool head)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    static char pem[APPLIANCE_PRODUCTION_SERVING_PEM_MAX];
    uint32_t length;

    if (serving->certificate == NULL ||
        !serving->certificate->available ||
        serving->certificate->certificate_length == 0U) {
        /* No certificate is ready: the honest answer is "not ready", never an
         * empty or invented document. */
        appliance_production_serving_release(connection, false);
        return;
    }
    length = appliance_production_serving_pem_encode(
        serving->certificate->certificate_der,
        serving->certificate->certificate_length, pem, sizeof(pem));
    if (length == 0U) {
        appliance_production_serving_release(connection, false);
        return;
    }
    ++serving->route_certificate_count;
    appliance_production_serving_encode(
        connection,
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_PEM,
        (const uint8_t *)pem,
        length,
        head);
}

static bool appliance_production_serving_target_public(
    const uint8_t *target,
    uint32_t target_length)
{
    /*
     * The three targets the frozen management contracts publish without a
     * session: the rendered panel sign-in document, the liveness target and
     * the API discovery target.  Everything else is decided by the ITEM-365
     * request gate.
     */
    return appliance_production_serving_target_equal(
               target, target_length,
               APPLIANCE_MANAGEMENT_API_PATH,
               APPLIANCE_MANAGEMENT_API_PATH_LENGTH) ||
           /*
            * ITEM-503: the appliance certificate is public by design - it is
            * sent in every handshake - and it must be fetchable before any
            * session so an operator can import it into a trust store.
            */
           (target_length == HTTP_ROUTE_CERTIFICATE_LENGTH &&
            target != NULL &&
            memcmp(target, "/hfoc-ca.crt",
                   HTTP_ROUTE_CERTIFICATE_LENGTH) == 0) ||
           (target_length == HTTP_ROUTE_HEALTH_LENGTH &&
            target != NULL &&
            target[0] == (uint8_t)'/' &&
            memcmp(target, "/health", HTTP_ROUTE_HEALTH_LENGTH) == 0) ||
           (target_length == HTTP_ROUTE_ROOT_LENGTH &&
            target != NULL &&
            target[0] == (uint8_t)'/');
}

/* --------------------------------------------------------------- reachability */

static void appliance_production_serving_publish_reachability(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_management_reachability_policy policy;

    if (serving->reachability_published) {
        return;
    }
    /*
     * Ordinary-boot default: the appliance's own management interface is the
     * perimeter, and the declared management address is zero, which the
     * ITEM-385 evaluator reads as "the appliance's own interface address".
     * This is the documented V1 behaviour of the policy: the management plane
     * is reachable on the management interface and nowhere else, the console
     * stays the break-glass path, and anything stricter stays available to an
     * operator through the same policy object.
     */
    memset(&policy, 0, sizeof(policy));
    policy.version = APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
    policy.mode =
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE;
    policy.management_interface =
        APPLIANCE_PRODUCTION_SERVING_REACHABILITY_INTERFACE;
    if (appliance_management_reachability_selection_init(
            &serving->reachability, &policy) !=
        APPLIANCE_MANAGEMENT_REACHABILITY_OK) {
        return;
    }
    serving->reachability_published = true;
}

static bool appliance_production_serving_reachability_admit(
    struct appliance_production_serving_connection *connection)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict =
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED;

    memset(&peer, 0, sizeof(peer));
    peer.interface_index = APPLIANCE_PRODUCTION_SERVING_REACHABILITY_INTERFACE;
    if (connection->pcb != NULL) {
        const ip4_addr_t *remote =
            ip_2_ip4(&connection->pcb->remote_ip);

        if (remote != NULL) {
            memcpy(
                peer.source_address,
                &remote->addr,
                APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
        }
    }
    if (appliance_management_reachability_evaluate(
            &serving->reachability, &peer, &verdict) !=
        APPLIANCE_MANAGEMENT_REACHABILITY_OK) {
        return false;
    }
    if (appliance_management_reachability_verdict_allowed(verdict)) {
        return true;
    }
    ++serving->reachability_denied_count;
    return false;
}

/* ------------------------------------------------------------------- socket */

static enum hfoc_mbedtls_bio_transport_status
appliance_production_serving_ssl_send(
    void *transport_context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    struct appliance_production_serving_connection *connection =
        transport_context;
    err_t error;

    *bytes_accepted = 0U;
    if (connection == NULL || connection->pcb == NULL ||
        bytes == NULL || length == 0U ||
        length > (size_t)UINT16_MAX) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    /*
     * Never wait for the whole record to fit: lwIP accepts a write only up to
     * its current send-buffer space, and the bounded BIO contract reports a
     * short send so mbedTLS continues with the remainder on the next step.
     * Waiting for the whole record deadlocks a large flight, which is exactly
     * the failure the ITEM-384 send buffer comment describes.
     */
    {
        u16_t room = tcp_sndbuf(connection->pcb);

        if (room == 0U) {
            return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
        }
        if ((size_t)room < length) {
            length = (size_t)room;
        }
    }
    error = tcp_write(
        connection->pcb, bytes, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_MEM) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    if (error != ERR_OK) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    error = tcp_output(connection->pcb);
    if (error == ERR_MEM) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    if (error != ERR_OK) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    *bytes_accepted = length;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

static enum hfoc_mbedtls_bio_transport_status
appliance_production_serving_ssl_recv(
    void *transport_context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    struct appliance_production_serving_connection *connection =
        transport_context;
    size_t available;
    size_t received;

    *bytes_received = 0U;
    if (connection == NULL || bytes == NULL || capacity == 0U) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    available = connection->rx_length - connection->rx_offset;
    if (available == 0U) {
        if (connection->peer_closed) {
            return HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED;
        }
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    received = available < capacity ? available : capacity;
    memcpy(bytes, &connection->rx[connection->rx_offset], received);
    connection->rx_offset += received;
    if (connection->rx_offset == connection->rx_length) {
        connection->rx_offset = 0U;
        connection->rx_length = 0U;
    }
    *bytes_received = received;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

static void appliance_production_serving_release(
    struct appliance_production_serving_connection *connection,
    bool graceful)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (!connection->active || connection->releasing) {
        return;
    }
    connection->releasing = true;
    if (connection->panel_auth_pending ||
        serving->panel_auth.owner == connection) {
        appliance_admin_credential_check_cancel(&serving->panel_auth.check);
        serving->panel_auth.active = false;
        serving->panel_auth.result_ready = false;
        serving->panel_auth.owner = NULL;
        connection->panel_auth_pending = false;
        /*
         * A disconnect or timeout while the job is pending is a result, not a
         * forever-IN_PROGRESS record: commit the honest CANCELLED word.  When
         * the job already completed this call is a no-op.
         */
        appliance_panel_auth_trace_commit(
            APPLIANCE_PANEL_AUTH_RESULT_CANCELLED,
            appliance_production_serving_panel_auth_now_ms());
    }
    /*
     * ITEM-504 increment 8: a released connection owns no target.  The call
     * clears the target's callbacks before aborting it, so this cannot recurse
     * through the target's error handler.
     */
    appliance_production_serving_relay_forget_target(connection);
    connection->facade = false;
    connection->relay_started = false;
    connection->target_eof = false;
    connection->client_eof = false;
    connection->target_shutdown = false;
    connection->client_shutdown = false;
    connection->request_staged = false;
    connection->request_raw_length = 0U;
    connection->forward_head_offset = 0U;
    connection->forward_body_offset = 0U;
    connection->to_target_length = 0U;
    connection->to_target_offset = 0U;
    connection->to_client_length = 0U;
    connection->to_client_offset = 0U;
    appliance_production_serving_zero(
        connection->request_raw, sizeof(connection->request_raw));
    appliance_production_serving_zero(
        connection->to_target, sizeof(connection->to_target));
    appliance_production_serving_zero(
        connection->to_client, sizeof(connection->to_client));
    if (connection->pcb != NULL) {
        if (graceful && !connection->peer_closed) {
            if (tcp_close(connection->pcb) != ERR_OK) {
                tcp_abort(connection->pcb);
            }
        } else {
            tcp_abort(connection->pcb);
        }
        connection->pcb = NULL;
    }
    if (connection->ssl_setup) {
        mbedtls_ssl_free(&connection->ssl);
        connection->ssl_setup = false;
    }
    if (connection->allocator_id != 0U) {
        (void)hfoc_mbedtls_alloc_connection_exit();
        (void)hfoc_mbedtls_alloc_connection_close(
            connection->allocator_id);
        connection->allocator_id = 0U;
    }
    if (connection->parser_started) {
        (void)http_parser_release(
            &serving->parser, &connection->parser_token);
        connection->parser_started = false;
    }
    (void)hfoc_mbedtls_bio_detach(&serving->bio, &connection->bio_token);
    appliance_production_serving_zero(
        connection->rx, sizeof(connection->rx));
    /*
     * ITEM-619: the sign-in body holds the presented password for the length
     * of the cooperative job; release must not leave it in the connection.
     */
    appliance_production_serving_zero(
        connection->body, sizeof(connection->body));
    connection->body_length = 0U;
    connection->body_expected = 0U;
    connection->rx_length = 0U;
    connection->rx_offset = 0U;
    connection->response_length = 0U;
    connection->response_offset = 0U;
    connection->response_ready = false;
    connection->response_sent = false;
    connection->close_notify_sent = false;
    connection->handshake_done = false;
    connection->active = false;
    connection->releasing = false;
    if (serving->active_count != 0U) {
        --serving->active_count;
    }
    ++serving->close_count;
}

/* -------------------------------------------------------------- TCP callbacks */

static err_t appliance_production_serving_tcp_recv(
    void *context,
    struct tcp_pcb *pcb,
    struct pbuf *packet,
    err_t error)
{
    struct appliance_production_serving_connection *connection = context;
    size_t length;

    (void)pcb;
    (void)error;
    ++appliance_production_serving_owner.rx_callbacks;
    if (connection == NULL || !connection->active) {
        if (packet != NULL) {
            pbuf_free(packet);
        }
        return ERR_OK;
    }
    if (packet == NULL) {
        connection->peer_closed = true;
        return ERR_OK;
    }
    length = packet->tot_len;
    if (length > sizeof(connection->rx) - connection->rx_length) {
        pbuf_free(packet);
        ++appliance_production_serving_owner.rejected_count;
        appliance_production_serving_release(connection, false);
        return ERR_ABRT;
    }
    if (pbuf_copy_partial(
            packet,
            &connection->rx[connection->rx_length],
            (u16_t)length,
            0U) != (u16_t)length) {
        pbuf_free(packet);
        ++appliance_production_serving_owner.rejected_count;
        appliance_production_serving_release(connection, false);
        return ERR_ABRT;
    }
    connection->rx_length += length;
    appliance_production_serving_owner.rx_bytes += (uint32_t)length;
    pbuf_free(packet);
    tcp_recved(pcb, (u16_t)length);
    connection->last_activity_ms = hfoc_lwip_time_now();
    return ERR_OK;
}

static err_t appliance_production_serving_tcp_sent(
    void *context,
    struct tcp_pcb *pcb,
    u16_t length)
{
    (void)context;
    (void)pcb;
    (void)length;
    return ERR_OK;
}

static err_t appliance_production_serving_tcp_poll(
    void *context,
    struct tcp_pcb *pcb)
{
    struct appliance_production_serving_connection *connection = context;

    (void)pcb;
    if (connection == NULL || !connection->active) {
        return ERR_OK;
    }
    if ((uint32_t)(hfoc_lwip_time_now() - connection->last_activity_ms) >=
        APPLIANCE_PRODUCTION_SERVING_IDLE_TIMEOUT_MS) {
        ++appliance_production_serving_owner.rejected_count;
        appliance_production_serving_release(connection, false);
    }
    return ERR_OK;
}

static void appliance_production_serving_tcp_error(
    void *context,
    err_t error)
{
    struct appliance_production_serving_connection *connection = context;

    (void)error;
    if (connection != NULL) {
        appliance_production_serving_release(connection, false);
    }
}

static err_t appliance_production_serving_accept(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    struct appliance_production_serving *serving = context;
    struct appliance_production_serving_connection *connection = NULL;
    uint32_t index;

    (void)generation;
    if (serving == NULL || pcb == NULL) {
        if (pcb != NULL) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    /*
     * ITEM-385: the peer address is read from the accepted connection and is
     * re-evaluated for every request, so a peer the policy refuses never
     * reaches TLS setup, parsing, routing or authorization.
     */
    if (serving->active_count >=
        APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT;
         ++index) {
        if (!serving->connections[index].active) {
            connection = &serving->connections[index];
            break;
        }
    }
    if (connection == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    memset(connection, 0, sizeof(*connection));
    connection->active = true;
    connection->index = index;
    connection->pcb = pcb;
    connection->last_activity_ms = hfoc_lwip_time_now();
    connection->parser_token.index = index;
    connection->parser_token.generation = 1U;
    connection->bio_token.index = index;
    connection->bio_token.generation = 1U;
    tcp_arg(pcb, connection);
    tcp_recv(pcb, appliance_production_serving_tcp_recv);
    tcp_sent(pcb, appliance_production_serving_tcp_sent);
    tcp_poll(pcb, appliance_production_serving_tcp_poll, 1U);
    tcp_err(pcb, appliance_production_serving_tcp_error);
    ++serving->active_count;
    ++serving->accept_count;
    return ERR_OK;
}

/* ---------------------------------------------------------------------- TLS */

/*
 * ITEM-507 increment 3: SNI chooses the certificate presentation for one
 * handshake.  The callback runs inside mbedTLS and must not allocate, so the
 * table it consults and the certificates it points at were parsed once when
 * the listener was configured, and everything it writes is a bounded member of
 * the one serving owner.  SNI is a presentation choice: it is never trust,
 * never authentication and never an authorization decision.
 */

static uint32_t appliance_production_serving_sni_append_text(
    char *buffer,
    uint32_t offset,
    const char *text)
{
    while (*text != '\0') {
        buffer[offset] = *text;
        ++offset;
        ++text;
    }
    buffer[offset] = '\0';
    return offset;
}

static uint32_t appliance_production_serving_sni_append_u32(
    char *buffer,
    uint32_t offset,
    uint32_t value)
{
    char digits[10];
    uint32_t count = 0U;
    uint32_t index;

    if (value == 0U) {
        buffer[offset] = '0';
        buffer[offset + 1U] = '\0';
        return offset + 1U;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    for (index = 0U; index < count; ++index) {
        buffer[offset + index] = digits[count - 1U - index];
    }
    offset += count;
    buffer[offset] = '\0';
    return offset;
}

/*
 * The client chooses the bytes of the SNI name, so the record may not copy
 * them blindly: a control byte would forge journal lines.  A valid name is
 * printable by definition; anything else is recorded as `?`, and the
 * selection has already refused it.
 */
static size_t appliance_production_serving_sni_sanitize(
    char *output,
    const unsigned char *name,
    size_t name_length)
{
    size_t index;

    if (name_length > APPLIANCE_CERTIFICATE_NAME_MAX) {
        name_length = APPLIANCE_CERTIFICATE_NAME_MAX;
    }
    for (index = 0U; index < name_length; ++index) {
        unsigned char character = name[index];

        if (character >= (unsigned char)'A' &&
            character <= (unsigned char)'Z') {
            character = (unsigned char)(character - (unsigned char)'A' +
                                        (unsigned char)'a');
        }
        if ((character >= (unsigned char)'a' &&
             character <= (unsigned char)'z') ||
            (character >= (unsigned char)'0' &&
             character <= (unsigned char)'9') ||
            character == (unsigned char)'.' ||
            character == (unsigned char)'-') {
            output[index] = (char)character;
        } else {
            output[index] = '?';
        }
    }
    output[name_length] = '\0';
    return name_length;
}

/*
 * `certificate` is the index of the selected entry, or -1 for the declared
 * default, -2 for no certificate at all and -3 for an entry that could not be
 * parsed.  The record never carries certificate or key bytes.
 */
static void appliance_production_serving_sni_record(
    struct appliance_production_serving *serving,
    const unsigned char *name,
    size_t name_length,
    const char *result,
    int32_t certificate,
    const char *reason)
{
    char sanitized[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    uint32_t offset = 0U;

    memset(serving->sni_record, 0, sizeof(serving->sni_record));
    offset = appliance_production_serving_sni_append_text(
        serving->sni_record, offset, "[PRODUCTION-TLS-SNI] name=");
    if (name != NULL && name_length != 0U) {
        const size_t sanitized_length =
            appliance_production_serving_sni_sanitize(
                sanitized, name, name_length);

        memcpy(serving->sni_record + offset, sanitized, sanitized_length);
        offset += (uint32_t)sanitized_length;
        serving->sni_record[offset] = '\0';
    } else {
        offset = appliance_production_serving_sni_append_text(
            serving->sni_record, offset, "-");
    }
    offset = appliance_production_serving_sni_append_text(
        serving->sni_record, offset, " result=");
    offset = appliance_production_serving_sni_append_text(
        serving->sni_record, offset, result);
    offset = appliance_production_serving_sni_append_text(
        serving->sni_record, offset, " cert=");
    if (certificate >= 0) {
        offset = appliance_production_serving_sni_append_u32(
            serving->sni_record, offset, (uint32_t)certificate);
    } else if (certificate == -1) {
        offset = appliance_production_serving_sni_append_text(
            serving->sni_record, offset, "default");
    } else if (certificate == -2) {
        offset = appliance_production_serving_sni_append_text(
            serving->sni_record, offset, "none");
    } else {
        offset = appliance_production_serving_sni_append_text(
            serving->sni_record, offset, "invalid");
    }
    if (reason != NULL) {
        offset = appliance_production_serving_sni_append_text(
            serving->sni_record, offset, " reason=");
        (void)appliance_production_serving_sni_append_text(
            serving->sni_record, offset, reason);
    }
    serving->sni_record_pending = true;
}

static struct appliance_production_serving_connection *
appliance_production_serving_sni_connection(
    struct appliance_production_serving *serving,
    mbedtls_ssl_context *ssl)
{
    uint32_t index;

    for (index = 0U;
         index < APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT;
         ++index) {
        struct appliance_production_serving_connection *connection =
            &serving->connections[index];

        if (connection->active && &connection->ssl == ssl) {
            return connection;
        }
    }
    return NULL;
}

static int appliance_production_serving_sni_callback(
    void *context,
    mbedtls_ssl_context *ssl,
    const unsigned char *name,
    size_t name_length)
{
    struct appliance_production_serving *serving = context;
    struct appliance_production_serving_connection *connection;
    const struct appliance_route_table *routes = NULL;
    struct appliance_route route;
    bool name_is_routed = false;
    enum appliance_certificate_name_selection selection =
        APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED;
    const struct appliance_certificate_name *entry = NULL;
    enum appliance_certificate_name_status status;

    if (serving == NULL || ssl == NULL) {
        return -1;
    }
    connection = appliance_production_serving_sni_connection(serving, ssl);
    if (connection != NULL) {
        connection->sni_present = true;
    }
    /*
     * ITEM-507 increment 4: a name the applied route table carries must have
     * its own certificate; the selection refuses it otherwise, even when a
     * default certificate is declared.
     */
    routes = appliance_route_config_applied(&serving->route_config);
    memset(&route, 0, sizeof(route));
    if (routes != NULL && name != NULL && name_length != 0U) {
        name_is_routed =
            appliance_route_table_lookup(
                routes, (const char *)name, name_length, &route) ==
            APPLIANCE_ROUTE_OK;
    }
    status = appliance_certificate_name_select(
        &serving->name_table, (const char *)name, name_length,
        name_is_routed, &selection, &entry);
    if (selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT &&
        entry != NULL) {
        const uint32_t index =
            (uint32_t)(entry - serving->name_table.entries);

        if (index < APPLIANCE_CERTIFICATE_NAME_MAX_COUNT &&
            serving->name_usable[index] &&
            mbedtls_ssl_set_hs_own_cert(
                ssl, &serving->name_chain[index],
                &serving->name_key[index]) == 0) {
            appliance_production_serving_sni_record(
                serving, name, name_length, "EXACT", (int32_t)index, NULL);
            return 0;
        }
        appliance_production_serving_sni_record(
            serving, name, name_length, "REFUSED", -3, "ENTRY_INVALID");
        return -1;
    }
    if (selection == APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT) {
        appliance_production_serving_sni_record(
            serving, name, name_length, "DEFAULT", -1, NULL);
        return 0;
    }
    appliance_production_serving_sni_record(
        serving, name, name_length, "REFUSED", -2,
        appliance_certificate_name_status_string(status));
    return -1;
}

/*
 * Load the declared names once, parse each pair, and register the callback.
 * An entry that does not parse is named and refused instead of taking the
 * whole listener down: the other names and the appliance's own identity still
 * work.  A store that is absent entirely is the pre-507 appliance and keeps
 * serving its one certificate; a record that exists but cannot be trusted
 * declares nothing, so unknown names are refused.
 */
static void appliance_production_serving_publish_name_table(
    struct appliance_production_serving *serving)
{
    enum appliance_certificate_name_store_source source;
    uint32_t index;

    appliance_certificate_name_store_init(&serving->name_store);
    (void)appliance_certificate_name_store_load(
        &serving->name_store, serving->loader, &serving->name_table);
    source = appliance_certificate_name_store_source(&serving->name_store);
    if (source ==
        APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID) {
        serving->name_table.default_mode =
            APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE;
    } else if (source !=
               APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED) {
        serving->name_table.default_mode =
            APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE;
    }
    /*
     * The saved table is what the durable record carries; the listener keeps
     * this boot-time copy in force, and a mutation updates only the saved one.
     */
    serving->name_saved = serving->name_table;
    for (index = 0U; index < serving->name_table.count; ++index) {
        const struct appliance_certificate_name *entry =
            &serving->name_table.entries[index];
        int result;

        mbedtls_x509_crt_init(&serving->name_chain[index]);
        mbedtls_pk_init(&serving->name_key[index]);
        result = mbedtls_x509_crt_parse_der(
            &serving->name_chain[index], entry->certificate_der,
            entry->certificate_length);
        if (result == 0) {
            result = mbedtls_pk_parse_key(
                &serving->name_key[index], entry->key_der,
                entry->key_length, NULL, 0U,
                appliance_production_entropy_random, NULL);
        }
        serving->name_usable[index] = result == 0;
    }
    mbedtls_ssl_conf_sni(
        &serving->ssl_config,
        appliance_production_serving_sni_callback, serving);
    serving->name_store_ready = true;
}

/*
 * ITEM-619: one serving poll interleaves several live TLS connections.  The
 * allocator scope therefore follows the operation, not the connection's whole
 * lifetime: a held-open sign-in no longer blocks a second handshake.
 */
static bool appliance_production_serving_ssl_scope_enter(
    struct appliance_production_serving_connection *connection)
{
    return connection != NULL &&
           connection->allocator_id != 0U &&
           hfoc_mbedtls_alloc_connection_enter(
               connection->allocator_id);
}

static void appliance_production_serving_ssl_scope_exit(void)
{
    (void)hfoc_mbedtls_alloc_connection_exit();
}

static bool appliance_production_serving_setup_tls(
    struct appliance_production_serving_connection *connection)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct hfoc_mbedtls_bio_transport transport;
    uint32_t allocator_id = 0U;
    bool ssl_initialized = false;
    bool bio_attached = false;
    int result;

    if (!hfoc_mbedtls_alloc_connection_open(&allocator_id)) {
        return false;
    }
    connection->allocator_id = allocator_id;
    if (!hfoc_mbedtls_alloc_connection_enter(allocator_id)) {
        serving->tls_error_stage = 1U;
        serving->tls_error = 0;
        (void)hfoc_mbedtls_alloc_connection_close(allocator_id);
        connection->allocator_id = 0U;
        return false;
    }
    mbedtls_ssl_init(&connection->ssl);
    ssl_initialized = true;
    result = mbedtls_ssl_setup(&connection->ssl, &serving->ssl_config);
    if (result != 0) {
        serving->tls_error_stage = 2U;
        serving->tls_error = result;
        goto fail;
    }
    memset(&transport, 0, sizeof(transport));
    transport.send = appliance_production_serving_ssl_send;
    transport.recv = appliance_production_serving_ssl_recv;
    transport.ctx = connection;
    if (hfoc_mbedtls_bio_attach(
            &serving->bio,
            &connection->bio_token,
            &serving->bio_policy,
            &transport) != HFOC_MBEDTLS_BIO_OK) {
        serving->tls_error_stage = 3U;
        serving->tls_error = -1;
        goto fail;
    }
    bio_attached = true;
    mbedtls_ssl_set_bio(
        &connection->ssl,
        &serving->bio.entries[connection->index],
        hfoc_mbedtls_bio_send_cb,
        hfoc_mbedtls_bio_recv_cb,
        NULL);
    if (http_parser_start(
            &serving->parser, &connection->parser_token) !=
        HTTP_PARSER_OK) {
        serving->tls_error_stage = 4U;
        serving->tls_error = -2;
        goto fail;
    }
    connection->parser_started = true;
    connection->ssl_setup = true;
    appliance_production_serving_ssl_scope_exit();
    return true;

fail:
    appliance_production_serving_ssl_scope_exit();
    if (bio_attached) {
        (void)hfoc_mbedtls_bio_detach(
            &serving->bio, &connection->bio_token);
    }
    if (ssl_initialized) {
        mbedtls_ssl_free(&connection->ssl);
    }
    (void)hfoc_mbedtls_alloc_connection_close(allocator_id);
    connection->allocator_id = 0U;
    return false;
}

static void appliance_production_serving_measure_tls(
    const mbedtls_ssl_context *ssl)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    const char *version;
    const char *cipher;

    if (serving->tls_measured) {
        return;
    }
    version = mbedtls_ssl_get_version(ssl);
    cipher = mbedtls_ssl_get_ciphersuite(ssl);
    appliance_production_serving_copy_string(
        serving->tls_version, sizeof(serving->tls_version), version);
    appliance_production_serving_copy_string(
        serving->tls_cipher, sizeof(serving->tls_cipher), cipher);
    serving->tls_measured = true;
}

/* ------------------------------------------------------------ HTTP handling */

/*
 * ITEM-445 step 2: the same encoder with the one `Set-Cookie` value a browser
 * sign-in or sign-out answers with. Every other response goes through the
 * plain wrapper below, which passes no cookie and therefore produces the
 * pre-item head byte for byte.
 */
static void appliance_production_serving_encode_with_cookie(
    struct appliance_production_serving_connection *connection,
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length,
    bool head,
    const uint8_t *set_cookie,
    uint32_t set_cookie_length,
    const uint8_t *location,
    uint32_t location_length)
{
    struct http_response response;
    enum http_response_status response_status;
    uint32_t written = 0U;

    memset(&response, 0, sizeof(response));
    response.status = status;
    response.content_type = content_type;
    response.body = body;
    response.body_length = body_length;
    response.set_cookie = set_cookie;
    response.set_cookie_length = set_cookie_length;
    response.location = location;
    response.location_length = location_length;
    if (head) {
        response_status = http_response_encode_head(
            &response,
            connection->response,
            sizeof(connection->response),
            &written);
    } else {
        response_status = http_response_encode(
            &response,
            connection->response,
            sizeof(connection->response),
            &written);
    }
    if (response_status != HTTP_RESPONSE_OK || written == 0U ||
        written > sizeof(connection->response)) {
        ++appliance_production_serving_owner.rejected_count;
        appliance_production_serving_release(connection, false);
        return;
    }
    connection->response_head = head;
    connection->response_body_length = response.body_length;
    connection->response_length = written;
    connection->response_offset = 0U;
    connection->response_ready = true;
}

static void appliance_production_serving_encode(
    struct appliance_production_serving_connection *connection,
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length,
    bool head)
{
    appliance_production_serving_encode_with_cookie(
        connection,
        status,
        content_type,
        body,
        body_length,
        head,
        NULL,
        0U,
        NULL,
        0U);
}

/*
 * ITEM-498: the refusal body is built for the layer the peer speaks.  A
 * browser target gets the readable HTML document that names the status and
 * links to the panel sign-in page; every other target gets valid JSON naming
 * the status and the reason.  Only the body and its content type change here:
 * the status is still decided by the call site, and an authorization decision
 * is not consulted or altered.  A target of NULL/0 means "not parsed yet" --
 * the refusal is then the API shape, because nothing is known about the peer.
 */
static void appliance_production_serving_error_response(
    struct appliance_production_serving_connection *connection,
    enum http_status_code status,
    const uint8_t *target,
    size_t target_length)
{
    uint8_t body[HTTP_ERROR_BODY_MAX];
    enum http_content_type content_type = HTTP_CONTENT_TYPE_JSON;
    size_t body_length =
        http_error_matrix_json_body(status, body, sizeof(body));

    if (http_error_matrix_body_family(target, target_length) ==
        HTTP_ERROR_BODY_HTML) {
        size_t html_length =
            http_error_matrix_html_body(status, body, sizeof(body));

        if (html_length != 0U) {
            body_length = html_length;
            content_type = HTTP_CONTENT_TYPE_HTML_UTF8;
        }
    }
    if (body_length == 0U) {
        /* A body that cannot be built is refused, never truncated. */
        appliance_production_serving_release(connection, false);
        return;
    }
    appliance_production_serving_encode(
        connection,
        status,
        content_type,
        body,
        (uint32_t)body_length,
        false);
}

static bool appliance_production_serving_authorization_header(
    struct appliance_production_serving_connection *connection,
    uint8_t *line,
    uint32_t capacity,
    uint32_t *line_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    static const char name[] = "Authorization";
    static const char colon[] = "Authorization:";
    struct http_header_field field;
    uint32_t count = 0U;
    uint32_t index;

    *line_length = 0U;
    if (http_header_count(
            &serving->parser,
            &connection->parser_token,
            &count) != HTTP_PARSER_OK) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        memset(&field, 0, sizeof(field));
        if (http_header_field(
                &serving->parser,
                &connection->parser_token,
                index,
                &field) != HTTP_PARSER_OK) {
            continue;
        }
        if (field.name_length == sizeof(name) - 1U &&
            memcmp(field.name, name, sizeof(name) - 1U) == 0 &&
            field.value_length + (uint32_t)(sizeof(colon) - 1U) <=
                capacity) {
            memcpy(line, colon, sizeof(colon) - 1U);
            memcpy(
                line + sizeof(colon) - 1U,
                field.value,
                field.value_length);
            *line_length =
                (uint32_t)(sizeof(colon) - 1U) + field.value_length;
            return true;
        }
    }
    return false;
}

/*
 * ITEM-445 step 2: the header value the browser sign-in needs, with optional
 * whitespace trimmed. A second header of the same name makes the value
 * untrustworthy (two `Cookie` headers with different sessions, two
 * `Content-Type` declarations), so the function publishes nothing and the
 * caller treats the request as unsigned rather than picking one.
 */
static bool appliance_production_serving_header_value(
    struct appliance_production_serving_connection *connection,
    const char *name,
    uint32_t name_length,
    const uint8_t **value,
    uint32_t *value_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct http_header_field field;
    bool found = false;
    uint32_t count = 0U;
    uint32_t index;

    *value = NULL;
    *value_length = 0U;
    if (http_header_count(
            &serving->parser,
            &connection->parser_token,
            &count) != HTTP_PARSER_OK) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        const uint8_t *start;
        uint32_t length;

        memset(&field, 0, sizeof(field));
        if (http_header_field(
                &serving->parser,
                &connection->parser_token,
                index,
                &field) != HTTP_PARSER_OK) {
            continue;
        }
        if (field.name_length != name_length ||
            memcmp(field.name, name, name_length) != 0) {
            continue;
        }
        if (found) {
            *value = NULL;
            *value_length = 0U;
            return false;
        }
        start = field.value;
        length = field.value_length;
        while (length != 0U &&
               (*start == (uint8_t)' ' || *start == (uint8_t)'\t')) {
            ++start;
            --length;
        }
        while (length != 0U &&
               (start[length - 1U] == (uint8_t)' ' ||
                start[length - 1U] == (uint8_t)'\t')) {
            --length;
        }
        *value = start;
        *value_length = length;
        found = true;
    }
    return found;
}

static void appliance_production_serving_handle_api(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    /*
     * ITEM-400: the four read-only management targets are owned by the
     * production snapshot module; the discovery target stays with the
     * management API module.  The owner is consulted first so a known
     * protected target is not answered with the frozen unknown-route body.
     */
    static struct appliance_production_management_api_result managed;
    struct appliance_management_api_result result;
    enum appliance_management_api_status status;

    if (serving->management_api != NULL &&
        appliance_production_management_api_owns(
            serving->management_api, line->target, line->target_length)) {
        enum appliance_production_management_api_status managed_status;

        memset(&managed, 0, sizeof(managed));
        managed_status = appliance_production_management_api_dispatch(
            serving->management_api,
            policy->method,
            line->target,
            line->target_length,
            &managed);
        if (managed_status == APPLIANCE_PRODUCTION_MANAGEMENT_API_OK) {
            ++serving->route_api_count;
        } else if (managed_status ==
                   APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED) {
            ++serving->method_denied_count;
        } else if (managed_status ==
                   APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE) {
            ++serving->rejected_count;
            appliance_production_serving_error_response(
                connection,
                HTTP_STATUS_503_SERVICE_UNAVAILABLE,
                line->target,
                line->target_length);
            return;
        } else {
            ++serving->rejected_count;
            appliance_production_serving_release(connection, false);
            return;
        }
        appliance_production_serving_encode(
            connection,
            managed.response.status,
            managed.response.content_type,
            managed.response.body,
            managed.response.body_length,
            policy->method == HTTP_METHOD_HEAD);
        return;
    }

    memset(&result, 0, sizeof(result));
    status = appliance_management_api_dispatch(
        policy->method, line->target, line->target_length, &result);
    if (status == APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT) {
        return;
    }
    if (status == APPLIANCE_MANAGEMENT_API_NOT_FOUND) {
        /*
         * An unknown route is answered as an unknown route, before the
         * authentication gate: a peer must not be able to tell a protected
         * target from a target that does not exist.
         */
        ++serving->not_found_count;
    } else if (status == APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED) {
        ++serving->method_denied_count;
    } else if (status == APPLIANCE_MANAGEMENT_API_OK) {
        ++serving->route_api_count;
    } else {
        ++serving->rejected_count;
        appliance_production_serving_release(connection, false);
        return;
    }
    appliance_production_serving_encode(
        connection,
        result.response.status,
        result.response.content_type,
        result.response.body,
        result.response.body_length,
        policy->method == HTTP_METHOD_HEAD);
}

/*
 * ITEM-503: the certificate-import executor the mutation plane calls.  The
 * adapter maps the upload composition's refusals onto the plane's three
 * outcomes; the plane keeps the token, the window and the audit, and the
 * upload keeps the candidate.
 */
static enum appliance_production_mutation_plane_certificate_status
appliance_production_serving_certificate_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_certificate_upload *upload = context;
    enum appliance_certificate_upload_status status;

    status = appliance_certificate_upload_stage(
        upload,
        (const uint8_t *)APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
        APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH,
        body,
        (uint32_t)body_length);
    return status == APPLIANCE_CERTIFICATE_UPLOAD_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_REFUSED;
}

static enum appliance_production_mutation_plane_certificate_status
appliance_production_serving_certificate_commit(void *context)
{
    return appliance_certificate_upload_commit(context) ==
                   APPLIANCE_CERTIFICATE_UPLOAD_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_APPLY_FAILED;
}

static void appliance_production_serving_certificate_rollback(void *context)
{
    (void)appliance_certificate_upload_rollback(context);
}

/*
 * ITEM-504: the routing-configuration executor the mutation plane calls.  The
 * adapter maps the route configuration's refusals onto the plane's three
 * outcomes; the plane keeps the token, the window and the audit, and the
 * route configuration keeps the candidate pair.  The content type is not on
 * the executor interface: the serving handler checks the wire header before
 * the plane is consulted, exactly as it does for the certificate import, and
 * the stage passes the frozen constant the configuration itself pins.
 */
static enum appliance_production_mutation_plane_routing_status
appliance_production_serving_routing_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    enum appliance_route_config_status status;

    status = appliance_route_config_stage(
        &serving->route_config,
        (const uint8_t *)APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE,
        APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH,
        body,
        (uint32_t)body_length);
    return status == APPLIANCE_ROUTE_CONFIG_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_REFUSED;
}

static enum appliance_production_mutation_plane_routing_status
appliance_production_serving_routing_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    const struct appliance_route_table *candidate =
        appliance_route_config_candidate(&serving->route_config);

    if (candidate == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_APPLY_FAILED;
    }
    /*
     * ITEM-504 increment 6: the candidate becomes durable before it becomes
     * the applied table.  A store that cannot prove the record leaves the
     * applied table exactly as it was and answers the operator with a failed
     * commit, instead of a route that would disappear at the next boot.
     */
    if (appliance_route_store_save(&serving->route_store, candidate) !=
        APPLIANCE_ROUTE_STORE_OK) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_APPLY_FAILED;
    }
    return appliance_route_config_commit(&serving->route_config) ==
               APPLIANCE_ROUTE_CONFIG_OK
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_APPLY_FAILED;
}

static void appliance_production_serving_routing_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    (void)appliance_route_config_rollback(&serving->route_config);
}

/*
 * ITEM-504: compose the bounded copy of the applied route table the Routing
 * page lists.  Each line is the record body `appliance_route_render` writes
 * for the route, so the page shows what the table holds instead of a
 * paraphrase; a table that does not fit the page's bounded copy is counted and
 * flagged, never silently cut, and an owner that published no table leaves
 * `published` false so the page says so.
 */
static void appliance_production_serving_publish_routes(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_routes routes;
    const struct appliance_route_table *applied =
        appliance_route_config_applied(&serving->route_config);
    uint32_t index;

    memset(&routes, 0, sizeof(routes));
    routes.published = serving->routing_published;
    if (routes.published && applied != NULL) {
        routes.count = applied->count;
        for (index = 0U; index < applied->count; ++index) {
            char line[APPLIANCE_ROUTE_RECORD_MAX];
            size_t line_length = 0U;
            size_t used = strlen(routes.text);

            if (appliance_route_render(&applied->routes[index], line,
                                       sizeof(line), &line_length) !=
                    APPLIANCE_ROUTE_OK ||
                used + line_length + 2U > sizeof(routes.text)) {
                routes.truncated = true;
                break;
            }
            memcpy(routes.text + used, line, line_length);
            routes.text[used + line_length] = '\n';
            routes.shown = index + 1U;
        }
        if (routes.shown < routes.count) {
            routes.truncated = true;
        }
    }
    (void)appliance_management_panel_set_routes(&serving->panel, &routes);
}

/*
 * ITEM-504 increment 5: the routing configuration's own record, drained by the
 * ordinary boot's loop and published as `[PRODUCTION-ROUTE]`.  The serving path
 * owns the composition and this hand-over; it does not render the record and it
 * does not decide what happened, it only moves the bounded body the terminal
 * state left.
 */
bool appliance_production_serving_take_route_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->routing_published) {
        return false;
    }
    if (!appliance_route_config_take_record(&serving->route_config, output,
                                            (size_t)capacity, &length)) {
        return false;
    }
    *output_length = (uint32_t)length;
    return true;
}

static bool appliance_production_serving_text_append(
    char *buffer,
    uint32_t capacity,
    uint32_t *length,
    const char *text)
{
    uint32_t index = 0U;

    if (buffer == NULL || length == NULL || text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        if (*length + 1U >= capacity) {
            return false;
        }
        buffer[(*length)++] = text[index++];
    }
    buffer[*length] = '\0';
    return true;
}

/*
 * ITEM-504 increment 6: one boot record for the table the store handed back.
 * It is published once, and only when the boot actually read the store: the
 * body names where the table came from and the table rendered by the model's
 * own `appliance_route_table_render`, so the serial log proves the routes
 * survived the reboot and not merely that a count did.  A store that could not
 * be read publishes its own source and the empty table; it never publishes a
 * table it did not read.
 */
bool appliance_production_serving_take_route_restore_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    const struct appliance_route_table *applied;
    size_t rendered = 0U;
    uint32_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->route_store_loaded || serving->route_restore_taken) {
        return false;
    }
    applied = appliance_route_config_applied(&serving->route_config);
    if (applied == NULL) {
        return false;
    }
    /*
     * The table is rendered straight into the caller's buffer, after the
     * source prefix: the boot path runs on the bootloader's stack and this
     * record is not worth a second multi-kilobyte frame beside the caller's.
     */
    if (!appliance_production_serving_text_append(output, capacity, &length,
                                                  "source=") ||
        !appliance_production_serving_text_append(
            output, capacity, &length,
            appliance_route_store_source_string(
                appliance_route_store_source(&serving->route_store))) ||
        !appliance_production_serving_text_append(output, capacity, &length,
                                                  " ") ||
        appliance_route_table_render(applied, output + length,
                                     (size_t)capacity - (size_t)length,
                                     &rendered) != APPLIANCE_ROUTE_OK) {
        return false;
    }
    length += (uint32_t)rendered;
    serving->route_restore_taken = true;
    *output_length = length;
    return true;
}

static bool appliance_production_serving_text_append_u64(
    char *buffer,
    uint32_t capacity,
    uint32_t *length,
    uint64_t value)
{
    char digits[21U];
    uint32_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        if (*length + 1U >= capacity) {
            return false;
        }
        buffer[(*length)++] = digits[--count];
    }
    buffer[*length] = '\0';
    return true;
}

/*
 * ITEM-515: the one form-field reader lives in `appliance_panel_form`, so the
 * serving path and the host proof that derives bodies from the drawn pages
 * read a body the same way.  The certificate payload fields are decoded by the
 * upload composition; this reads the small control fields (`token`, `cancel`)
 * the two-phase request needs, and the first occurrence still wins.
 */
static bool appliance_production_serving_form_field(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    const uint8_t **value_out,
    uint32_t *value_length_out)
{
    return appliance_panel_form_field(body, body_length, name, value_out,
                                      value_length_out);
}

static bool appliance_production_serving_form_u64(
    const uint8_t *value,
    uint32_t value_length,
    uint64_t *value_out)
{
    uint64_t parsed = 0U;
    uint32_t index;

    if (value == NULL || value_out == NULL || value_length == 0U) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        if (value[index] < (uint8_t)'0' ||
            value[index] > (uint8_t)'9') {
            return false;
        }
        if (parsed > (UINT64_MAX - (uint64_t)(value[index] - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (uint64_t)(value[index] - (uint8_t)'0');
    }
    *value_out = parsed;
    return true;
}

static bool appliance_production_serving_form_flag(
    const uint8_t *body,
    uint32_t body_length,
    const char *name)
{
    return appliance_panel_form_flag(body, body_length, name);
}

/*
 * ITEM-505: the network-configuration mutation.
 *
 * The panel's Network page is the second configuring section and the first
 * one whose change the machine does not apply to the live interface.  The
 * operator stages one candidate - DHCP or a static address/prefix, gateway and
 * DNS - confirms it with the plane's token, and the commit writes the durable
 * record the next boot applies.  The live interface keeps running what it
 * booted with, so the record and the page both say `applies=reboot` and name
 * the active configuration next to the saved one.
 *
 * The body grammar is the panel form: `mode=dhcp` with no other field, or
 * `mode=static` with `address`, `prefix`, `gateway` and `dns`.  A missing,
 * unknown or stray field is refused; nothing is written before the exact token
 * comes back, and a failed write leaves the previously stored record in force.
 */
#define APPLIANCE_PRODUCTION_SERVING_NETWORK_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_NETWORK_CONTENT_TYPE_LENGTH 33U

static struct appliance_persistent_network_config *
appliance_production_serving_network_store(
    struct appliance_production_serving *serving)
{
    if (serving == NULL || serving->loader == NULL) {
        return NULL;
    }
    return &serving->loader->network_config;
}

static bool appliance_production_serving_network_append(
    char *output,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    if (output == NULL || length == NULL || text == NULL) {
        return false;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (*length + 1U >= capacity) {
            return false;
        }
        output[(*length)++] = text[index];
    }
    output[*length] = '\0';
    return true;
}

static bool appliance_production_serving_network_append_u8(
    char *output,
    size_t capacity,
    size_t *length,
    uint8_t value)
{
    char digits[3U];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value = (uint8_t)(value / 10U);
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        char one[2U];

        one[0U] = digits[--count];
        one[1U] = '\0';
        if (!appliance_production_serving_network_append(
                output, capacity, length, one)) {
            return false;
        }
    }
    return true;
}

static bool appliance_production_serving_network_append_address(
    char *output,
    size_t capacity,
    size_t *length,
    const struct network_ipv4_address *address)
{
    uint32_t index;

    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U &&
            !appliance_production_serving_network_append(
                output, capacity, length, ".")) {
            return false;
        }
        if (!appliance_production_serving_network_append_u8(
                output, capacity, length, address->octets[index])) {
            return false;
        }
    }
    return true;
}

/*
 * The compact rendering the page and the record carry.  The model's own
 * `appliance_network_config_render` names the interface and the schema on
 * every line and is longer than the page's per-value budget; this rendering
 * keeps the four facts a reader needs and is still the validated model behind
 * it, never a paraphrase of raw request bytes.
 */
static void appliance_production_serving_network_render(
    const struct appliance_network_config *config,
    char *output,
    size_t capacity)
{
    size_t length = 0U;
    bool rendered = true;

    if (output == NULL || capacity == 0U) {
        return;
    }
    output[0] = '\0';
    if (config == NULL ||
        appliance_network_config_validate(config) !=
            APPLIANCE_NETWORK_CONFIG_OK) {
        (void)appliance_production_serving_network_append(
            output, capacity, &length, "UNAVAILABLE");
        return;
    }
    rendered =
        appliance_production_serving_network_append(
            output, capacity, &length, "mode=") &&
        appliance_production_serving_network_append(
            output, capacity, &length,
            appliance_network_config_mode_string(config->mode));
    if (rendered && config->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        rendered =
            appliance_production_serving_network_append(
                output, capacity, &length, " address=") &&
            appliance_production_serving_network_append_address(
                output, capacity, &length,
                &config->static_config.address) &&
            appliance_production_serving_network_append(
                output, capacity, &length, "/") &&
            appliance_production_serving_network_append_u8(
                output, capacity, &length,
                config->static_config.prefix_length) &&
            appliance_production_serving_network_append(
                output, capacity, &length, " gateway=") &&
            appliance_production_serving_network_append_address(
                output, capacity, &length,
                &config->static_config.gateway) &&
            appliance_production_serving_network_append(
                output, capacity, &length, " dns=") &&
            appliance_production_serving_network_append_address(
                output, capacity, &length,
                &config->static_config.dns);
    }
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_network_append(
            output, capacity, &length, "UNAVAILABLE");
    }
}

static void appliance_production_serving_network_render_active(
    struct appliance_production_serving *serving)
{
    const enum appliance_network_mode_action mode =
        appliance_production_network_wire_boot_mode();
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    const char *interface = APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE;
    const size_t interface_length = (size_t)strlen(interface);
    struct appliance_network_config config;
    bool composed = true;

    memset(&config, 0, sizeof(config));
    if (mode == APPLIANCE_NETWORK_MODE_ACTION_DHCP) {
        composed = appliance_network_config_publish_dhcp(
                       &config, interface, interface_length) ==
                   APPLIANCE_NETWORK_CONFIG_OK;
    } else if (mode == APPLIANCE_NETWORK_MODE_ACTION_STATIC) {
        composed = active != NULL &&
                   appliance_network_config_publish_static(
                       &config, interface, interface_length, active) ==
                       APPLIANCE_NETWORK_CONFIG_OK;
    } else if (mode == APPLIANCE_NETWORK_MODE_ACTION_OFFLINE) {
        composed = appliance_network_config_publish_offline(
                       &config, interface, interface_length) ==
                   APPLIANCE_NETWORK_CONFIG_OK;
    } else {
        composed = false;
    }
    appliance_production_serving_network_render(
        composed ? &config : NULL,
        serving->network_active_text,
        sizeof(serving->network_active_text));
}

static void appliance_production_serving_network_render_saved(
    struct appliance_production_serving *serving)
{
    struct appliance_persistent_network_config *store =
        appliance_production_serving_network_store(serving);
    const struct appliance_network_config *saved =
        store != NULL
            ? appliance_persistent_network_config_current(store)
            : NULL;

    appliance_production_serving_network_render(
        saved,
        serving->network_saved_text,
        sizeof(serving->network_saved_text));
}

/*
 * The console menu's own reading of what the interface runs, reused so the
 * record says exactly what the operator would read at the machine: the mode
 * word, and the address with its prefix only when the mode is static.  The
 * page's rendering is richer; this one is the vocabulary ITEM-496 established
 * for `current=`.
 */
static void appliance_production_serving_network_current_short(
    char *output,
    size_t capacity)
{
    const enum appliance_network_mode_action mode =
        appliance_production_network_wire_boot_mode();
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    size_t length = 0U;
    bool rendered = true;

    if (output == NULL || capacity == 0U) {
        return;
    }
    output[0] = '\0';
    if (mode == APPLIANCE_NETWORK_MODE_ACTION_DHCP) {
        rendered = appliance_production_serving_network_append(
            output, capacity, &length, "DHCP");
    } else if (mode == APPLIANCE_NETWORK_MODE_ACTION_STATIC &&
               active != NULL) {
        rendered =
            appliance_production_serving_network_append(
                output, capacity, &length, "STATIC address=") &&
            appliance_production_serving_network_append_address(
                output, capacity, &length, &active->address) &&
            appliance_production_serving_network_append(
                output, capacity, &length, "/") &&
            appliance_production_serving_network_append_u8(
                output, capacity, &length, active->prefix_length);
    } else if (mode == APPLIANCE_NETWORK_MODE_ACTION_OFFLINE) {
        rendered = appliance_production_serving_network_append(
            output, capacity, &length, "OFFLINE");
    } else {
        rendered = false;
    }
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_network_append(
            output, capacity, &length, "UNAVAILABLE");
    }
}

/*
 * The short source word the page shows.  The store's own enum string is
 * `APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED`, which is exact but
 * not what a reader needs next to a value; the page names the same state with
 * the vocabulary the state documents use, and an unknown source stays
 * UNAVAILABLE instead of being guessed.
 */
static const char *appliance_production_serving_network_source_word(
    enum appliance_persistent_network_config_source source)
{
    switch (source) {
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    default:
        return "UNAVAILABLE";
    }
}

/*
 * Publish the pair the Network page shows.  It is composed at render time,
 * exactly as the routing page's table is, so the page cannot show a value the
 * owner did not just read; an owner that is not published leaves the page
 * saying so.
 */
static void appliance_production_serving_publish_network(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_network network;
    struct appliance_persistent_network_config *store;

    memset(&network, 0, sizeof(network));
    network.published = serving->network_published;
    if (!network.published) {
        network.reason = "NO_NETWORK_OWNER";
        (void)appliance_management_panel_set_network(
            &serving->panel, &network);
        return;
    }
    appliance_production_serving_network_render_active(serving);
    appliance_production_serving_network_render_saved(serving);
    store = appliance_production_serving_network_store(serving);
    network.active = serving->network_active_text;
    network.saved = serving->network_saved_text;
    network.source =
        store != NULL
            ? appliance_production_serving_network_source_word(
                  appliance_persistent_network_config_source(store))
            : "UNAVAILABLE";
    network.applies = "reboot";
    (void)appliance_management_panel_set_network(
        &serving->panel, &network);
}

/*
 * The `[PRODUCTION-NET-SET]` record.  It is one bounded line, handed to the
 * loop like the routing record, and it names the source, the candidate the
 * operator confirmed, whether the durable write proved itself, that the change
 * waits for the reboot, and what the interface still runs.  The overlay is
 * explicit: a commit that could not write says `persisted=FAILED` and claims
 * nothing.
 */
static void appliance_production_serving_network_record(
    struct appliance_production_serving *serving,
    bool persisted)
{
    size_t length = 0U;
    bool rendered = true;
    char *record = serving->network_record;

    memset(record, 0, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX);
    rendered =
        appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length, "[PRODUCTION-NET-SET] source=PANEL mode=") &&
        appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length,
            appliance_network_config_mode_string(
                serving->network_candidate.mode));
    if (rendered &&
        serving->network_candidate.mode ==
            APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        rendered =
            appliance_production_serving_network_append(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length, " address=") &&
            appliance_production_serving_network_append_address(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length,
                &serving->network_candidate.static_config.address) &&
            appliance_production_serving_network_append(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length, "/") &&
            appliance_production_serving_network_append_u8(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length,
                serving->network_candidate.static_config.prefix_length) &&
            appliance_production_serving_network_append(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length, " gateway=") &&
            appliance_production_serving_network_append_address(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length,
                &serving->network_candidate.static_config.gateway) &&
            appliance_production_serving_network_append(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length, " dns=") &&
            appliance_production_serving_network_append_address(
                record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
                &length,
                &serving->network_candidate.static_config.dns);
    }
    rendered =
        rendered &&
        appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length, persisted ? " persisted=OK" : " persisted=FAILED") &&
        appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length, " applies=reboot current=");
    if (rendered) {
        char current[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX];

        appliance_production_serving_network_current_short(
            current, sizeof(current));
        rendered = appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length, current);
    }
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_network_append(
            record, APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX,
            &length,
            persisted
                ? "[PRODUCTION-NET-SET] source=PANEL persisted=OK "
                  "applies=reboot current=UNAVAILABLE"
                : "[PRODUCTION-NET-SET] source=PANEL persisted=FAILED "
                  "applies=reboot current=UNAVAILABLE");
    }
    serving->network_record_pending = true;
}

/*
 * ITEM-515: the network stage decodes the panel form through
 * `appliance_panel_form_network_parse`, the same grammar the host proof drives
 * with the body the drawn page submits.  A refusal names the field that failed
 * and that name reaches the HTTP refusal document; the DHCP branch accepts the
 * four empty optional inputs a browser always sends.
 */
static enum appliance_production_mutation_plane_network_status
appliance_production_serving_network_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    struct appliance_panel_form_network parsed;
    enum appliance_panel_form_status form_status;

    if (serving == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_REFUSED;
    }
    serving->network_refusal = NULL;
    memset(&serving->network_candidate, 0,
           sizeof(serving->network_candidate));
    if (body == NULL || body_length == 0U || body_length > UINT32_MAX) {
        serving->network_refusal = appliance_panel_form_status_string(
            APPLIANCE_PANEL_FORM_EMPTY_BODY);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_REFUSED;
    }
    form_status = appliance_panel_form_network_parse(
        body, (uint32_t)body_length, &parsed);
    if (form_status != APPLIANCE_PANEL_FORM_OK) {
        serving->network_refusal =
            appliance_panel_form_status_string(form_status);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_REFUSED;
    }
    serving->network_candidate.mode = parsed.mode;
    if (parsed.mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        serving->network_candidate.static_config = parsed.static_config;
    }
    serving->network_candidate.staged = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK;
}

static enum appliance_production_mutation_plane_network_status
appliance_production_serving_network_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    struct appliance_persistent_network_config *store;
    const char *interface = APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE;
    const size_t interface_length = (size_t)strlen(interface);
    enum appliance_persistent_network_config_status status;
    bool persisted;

    if (serving == NULL || !serving->network_candidate.staged) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_APPLY_FAILED;
    }
    store = appliance_production_serving_network_store(serving);
    if (store == NULL) {
        appliance_production_serving_network_render_active(serving);
        appliance_production_serving_network_record(serving, false);
        memset(&serving->network_candidate, 0,
               sizeof(serving->network_candidate));
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_APPLY_FAILED;
    }
    if (serving->network_candidate.mode ==
        APPLIANCE_NETWORK_CONFIG_MODE_DHCP) {
        status = appliance_persistent_network_config_set_dhcp(
            store, interface, interface_length);
    } else if (serving->network_candidate.mode ==
               APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        status = appliance_persistent_network_config_set_static(
            store, interface, interface_length,
            &serving->network_candidate.static_config);
    } else {
        status = APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG;
    }
    persisted = status == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    /*
     * The reading the record quotes is the interface's own, taken after the
     * write: the commit never rebinds the wire, so `current=` must be what is
     * running, not what was just stored.
     */
    appliance_production_serving_network_render_active(serving);
    appliance_production_serving_network_record(serving, persisted);
    memset(&serving->network_candidate, 0,
           sizeof(serving->network_candidate));
    return persisted
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_APPLY_FAILED;
}

static void appliance_production_serving_network_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        memset(&serving->network_candidate, 0,
               sizeof(serving->network_candidate));
        serving->network_refusal = NULL;
    }
}

bool appliance_production_serving_take_network_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->network_published || !serving->network_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->network_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] = serving->network_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->network_record_pending = false;
    return true;
}

/*
 * ITEM-505 increment 3: the system-hostname mutation.
 *
 * The System page is the third configuring section and the second whose change
 * the machine does not apply live.  The operator stages one hostname, confirms
 * it with the plane's token, and the commit writes the durable system record
 * the next boot resolves as the machine's identity and as the name in its
 * certificate.  The running listener keeps the identity it booted with, so the
 * record and the page both say `applies=reboot` and name the active identity
 * next to the saved hostname.
 *
 * The body grammar is the panel form: exactly one field, `hostname=<name>`,
 * validated by the configuration model's own rule (1..63 characters from
 * `A-Za-z0-9-`, no leading or trailing hyphen).  A missing, stray or invalid
 * field is refused by name and nothing is written; a failed write leaves the
 * previously stored record in force.
 */
#define APPLIANCE_PRODUCTION_SERVING_SYSTEM_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_SYSTEM_CONTENT_TYPE_LENGTH 33U

static struct appliance_persistent_config *
appliance_production_serving_system_store(
    struct appliance_production_serving *serving)
{
    if (serving == NULL || serving->loader == NULL) {
        return NULL;
    }
    return &serving->loader->system_config;
}

/*
 * The short source word the page shows, with the same vocabulary the network
 * page uses; an unknown source stays UNAVAILABLE instead of being guessed.
 */
static const char *appliance_production_serving_system_source_word(
    enum appliance_config_validation_source source)
{
    switch (source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    default:
        return "UNAVAILABLE";
    }
}

/*
 * The identity this boot resolved, captured once when the serving path is
 * published.  The certificate declaration resolves the same record at boot, so
 * `current=` names exactly what the running system serves; an unreadable
 * record falls back to the documented default identity, exactly as the
 * certificate declaration does.
 */
static void appliance_production_serving_system_capture_active(
    struct appliance_production_serving *serving)
{
    struct appliance_persistent_config *store =
        appliance_production_serving_system_store(serving);
    size_t hostname_length = 0U;

    if (serving == NULL) {
        return;
    }
    serving->system_active_text[0] = '\0';
    if (store != NULL &&
        appliance_persistent_config_hostname(
            store,
            serving->system_active_text,
            sizeof(serving->system_active_text),
            &hostname_length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
        hostname_length != 0U) {
        return;
    }
    memcpy(serving->system_active_text,
           APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
           APPLIANCE_IDENTITY_DEFAULT_HOSTNAME_LENGTH + 1U);
}

static void appliance_production_serving_system_render_saved(
    struct appliance_production_serving *serving)
{
    struct appliance_persistent_config *store =
        appliance_production_serving_system_store(serving);
    size_t hostname_length = 0U;

    if (store == NULL ||
        appliance_persistent_config_hostname(
            store,
            serving->system_saved_text,
            sizeof(serving->system_saved_text),
            &hostname_length) != APPLIANCE_PERSISTENT_CONFIG_OK ||
        hostname_length == 0U) {
        memset(serving->system_saved_text, 0,
               sizeof(serving->system_saved_text));
        memcpy(serving->system_saved_text, "UNAVAILABLE",
               sizeof("UNAVAILABLE"));
    }
}

/*
 * ITEM-509 increment 5: the language this render serves in and the language
 * the record holds.  Both are read from the system record now, so a committed
 * language shows up on the next request; a record that cannot be read says
 * UNAVAILABLE on both lines instead of a value.  The pair can differ once the
 * switch exists (a staged language is not the active one yet) - that switch is
 * the next increment.
 */
static void appliance_production_serving_system_render_language(
    struct appliance_production_serving *serving)
{
    struct appliance_persistent_config *store =
        appliance_production_serving_system_store(serving);
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;

    memset(serving->system_language_active_text, 0,
           sizeof(serving->system_language_active_text));
    memset(serving->system_language_saved_text, 0,
           sizeof(serving->system_language_saved_text));
    if (store == NULL ||
        appliance_persistent_config_language(store, &language) !=
            APPLIANCE_PERSISTENT_CONFIG_OK ||
        appliance_language_code(language) == NULL) {
        memcpy(serving->system_language_active_text, "UNAVAILABLE",
               sizeof("UNAVAILABLE"));
        memcpy(serving->system_language_saved_text, "UNAVAILABLE",
               sizeof("UNAVAILABLE"));
        return;
    }
    memcpy(serving->system_language_active_text,
           appliance_language_code(language),
           strlen(appliance_language_code(language)) + 1U);
    memcpy(serving->system_language_saved_text,
           appliance_language_code(language),
           strlen(appliance_language_code(language)) + 1U);
}

/*
 * Publish the pair the System page shows.  It is composed at render time, as
 * the network page's readings are, so the page cannot show a value the owner
 * did not just read; an owner that is not published leaves the page saying so.
 */
static void appliance_production_serving_publish_system(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_system system;
    struct appliance_persistent_config *store;

    memset(&system, 0, sizeof(system));
    system.published = serving->system_published;
    if (!system.published) {
        system.reason = "NO_SYSTEM_OWNER";
        (void)appliance_management_panel_set_system(
            &serving->panel, &system);
        return;
    }
    appliance_production_serving_system_render_saved(serving);
    appliance_production_serving_system_render_language(serving);
    store = appliance_production_serving_system_store(serving);
    system.active = serving->system_active_text;
    system.saved = serving->system_saved_text;
    system.language_active = serving->system_language_active_text;
    system.language_saved = serving->system_language_saved_text;
    system.source =
        store != NULL
            ? appliance_production_serving_system_source_word(
                  appliance_persistent_config_source(store))
            : "UNAVAILABLE";
    system.applies = "reboot";
    (void)appliance_management_panel_set_system(
        &serving->panel, &system);
}

/*
 * ITEM-509 increment 6: the `[PRODUCTION-SYSTEM-LANGUAGE]` record.  One
 * bounded line naming the language the operator confirmed, whether the durable
 * write proved itself, and the language the panel now serves - the same shape
 * as the hostname record, so a reader can tell the two acts apart.
 */
static void appliance_production_serving_system_language_record(
    struct appliance_production_serving *serving,
    bool persisted)
{
    uint32_t length = 0U;
    const char *code = appliance_language_code(
        serving->system_candidate.language);

    memset(serving->system_record, 0,
           APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX);
    if (!appliance_production_serving_text_append(
            serving->system_record,
            APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX, &length,
            "[PRODUCTION-SYSTEM-LANGUAGE] source=PANEL value=") ||
        code == NULL ||
        !appliance_production_serving_text_append(
            serving->system_record,
            APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX, &length, code) ||
        !appliance_production_serving_text_append(
            serving->system_record,
            APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX, &length,
            persisted ? " persisted=OK" : " persisted=FAILED") ||
        !appliance_production_serving_text_append(
            serving->system_record,
            APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX, &length,
            " applies=next-request")) {
        length = 0U;
        (void)appliance_production_serving_text_append(
            serving->system_record,
            APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX, &length,
            persisted
                ? "[PRODUCTION-SYSTEM-LANGUAGE] source=PANEL persisted=OK"
                : "[PRODUCTION-SYSTEM-LANGUAGE] source=PANEL "
                  "persisted=FAILED");
    }
    serving->system_record_pending = true;
}

/*
 * The `[PRODUCTION-SYSTEM-SET]` record.  One bounded line, handed to the loop
 * like the network record, naming the source, the hostname the operator
 * confirmed, whether the durable write proved itself, that the change waits
 * for the reboot, and the identity the running system still serves.
 */
static void appliance_production_serving_system_record(
    struct appliance_production_serving *serving,
    bool persisted)
{
    uint32_t length = 0U;
    bool rendered = true;
    char *record = serving->system_record;

    memset(record, 0, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX);
    rendered =
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length,
            "[PRODUCTION-SYSTEM-SET] source=PANEL hostname=") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length, serving->system_candidate.hostname) &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length, persisted ? " persisted=OK" : " persisted=FAILED") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length, " applies=reboot current=") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length, serving->system_active_text);
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX,
            &length,
            persisted
                ? "[PRODUCTION-SYSTEM-SET] source=PANEL persisted=OK "
                  "applies=reboot current=UNAVAILABLE"
                : "[PRODUCTION-SYSTEM-SET] source=PANEL persisted=FAILED "
                  "applies=reboot current=UNAVAILABLE");
    }
    serving->system_record_pending = true;
}

/*
 * ITEM-515: the one-field shape the System stage has always enforced lives in
 * `appliance_panel_form_system_parse`, so the host proof that renders the page
 * and builds the drawn body drives the same reader the stage drives.
 */

static enum appliance_production_mutation_plane_system_status
appliance_production_serving_system_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    enum appliance_panel_form_system_field field =
        APPLIANCE_PANEL_FORM_SYSTEM_NONE;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;

    if (serving == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
    }
    memset(&serving->system_candidate, 0,
           sizeof(serving->system_candidate));
    if (body == NULL || body_length == 0U || body_length > UINT32_MAX ||
        appliance_panel_form_system_parse(
            body, (uint32_t)body_length, &field, &value, &value_length) !=
            APPLIANCE_PANEL_FORM_OK) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
    }
    if (field == APPLIANCE_PANEL_FORM_SYSTEM_HOSTNAME) {
        if (appliance_config_validation_validate_hostname(
                (const char *)value, value_length) !=
            APPLIANCE_CONFIG_VALIDATION_OK) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
        }
        memcpy(serving->system_candidate.hostname, value, value_length);
        serving->system_candidate.hostname[value_length] = '\0';
        serving->system_candidate.staged = true;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK;
    }
    /*
     * ITEM-509 increment 6: the same stage accepts the interface language.
     * The value is the catalog's own parser's business - exactly `en` or `ru`
     * - so a typo is refused here, before any token is issued, and nothing is
     * written until the exact token comes back.
     */
    if (field == APPLIANCE_PANEL_FORM_SYSTEM_LANGUAGE) {
        enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;

        if (appliance_language_parse(
                (const char *)value, value_length, &language) !=
            APPLIANCE_LANGUAGE_OK) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
        }
        serving->system_candidate.language = language;
        serving->system_candidate.language_staged = true;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK;
    }
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED;
}

static enum appliance_production_mutation_plane_system_status
appliance_production_serving_system_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    struct appliance_persistent_config *store;
    enum appliance_persistent_config_status status;
    bool persisted;

    if (serving == NULL || !serving->system_candidate.staged) {
        if (serving == NULL || !serving->system_candidate.language_staged) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED;
        }
    }
    store = appliance_production_serving_system_store(serving);
    if (store == NULL) {
        if (serving->system_candidate.language_staged) {
            appliance_production_serving_system_language_record(serving,
                                                                false);
        } else {
            appliance_production_serving_system_render_saved(serving);
            appliance_production_serving_system_record(serving, false);
        }
        memset(&serving->system_candidate, 0,
               sizeof(serving->system_candidate));
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED;
    }
    if (serving->system_candidate.language_staged) {
        /*
         * ITEM-509 increment 6: the language is written through the same
         * record the console command uses, and the panel shows the saved value
         * on the next request because it re-reads the record at render time.
         * No reboot is involved, so the record says so.
         */
        status = appliance_persistent_config_set_language(
            store, serving->system_candidate.language);
        persisted = status == APPLIANCE_PERSISTENT_CONFIG_OK;
        appliance_production_serving_system_render_language(serving);
        appliance_production_serving_system_language_record(serving,
                                                            persisted);
        memset(&serving->system_candidate, 0,
               sizeof(serving->system_candidate));
        return persisted
            ? APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK
            : APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED;
    }
    status = appliance_persistent_config_set_hostname(
        store,
        serving->system_candidate.hostname,
        strlen(serving->system_candidate.hostname));
    persisted = status == APPLIANCE_PERSISTENT_CONFIG_OK;
    /*
     * The reading the record quotes is the running identity, captured at boot:
     * the commit never rebinds it, so `current=` must be what is being served,
     * not what was just stored.
     */
    appliance_production_serving_system_render_saved(serving);
    appliance_production_serving_system_record(serving, persisted);
    memset(&serving->system_candidate, 0,
           sizeof(serving->system_candidate));
    return persisted
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED;
}

static void appliance_production_serving_system_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        memset(&serving->system_candidate, 0,
               sizeof(serving->system_candidate));
    }
}

bool appliance_production_serving_take_system_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->system_published || !serving->system_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->system_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] = serving->system_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->system_record_pending = false;
    return true;
}

/*
 * ITEM-506 increment 6: the one account's credential change.
 *
 * The Account page is the fourth configuring section and the only mutation
 * whose commit closes the session that confirmed it.  The operator stages the
 * current password, the new login and the new password (twice); the stage
 * proves the current password through the enrollment owner's verify-only
 * entry, validates the new pair with the credential model's own policy and
 * holds the candidate.  The commit calls the owner's single setter
 * `provision_credential()`, which draws a fresh salt, derives the PBKDF2
 * verifier, writes the durable record, republishes the verifier through the
 * request gate and closes every issued session - there is no second writer and
 * no way to write the record without the invalidation.
 *
 * The plaintext candidate lives in this composition for the confirmation
 * window (300 s, `APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS`)
 * and is zeroed by the commit, by a rollback (cancel, expiry, stop,
 * fail-closed) and by a new stage.  It is never logged, never rendered and
 * never copied into the audit ring; the record names the login and whether the
 * durable write proved itself, never the password.
 */
#define APPLIANCE_PRODUCTION_SERVING_ACCOUNT_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_ACCOUNT_CONTENT_TYPE_LENGTH 33U

/*
 * ITEM-507 increment 4: the certificate-name scriptable target accepts the
 * same body type as every other mutation; the fields carry the PEM pair in
 * percent-encoding.
 */
#define APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_CONTENT_TYPE_LENGTH 33U

/*
 * ITEM-508 increment 34: the ACME control page and its scriptable target take
 * the same one-field form body as every other mutation.
 */
#define APPLIANCE_PRODUCTION_SERVING_ACME_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_ACME_CONTENT_TYPE_LENGTH 33U

/*
 * ITEM-516: the management gate's page and scriptable target take the same
 * one-field form body as every other mutation.
 */
#define APPLIANCE_PRODUCTION_SERVING_GATE_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_PRODUCTION_SERVING_GATE_CONTENT_TYPE_LENGTH 33U
/* The stable name an attempt to open the gate from the panel answers with. */
#define APPLIANCE_PRODUCTION_SERVING_GATE_LOCAL_SECRET_REQUIRED \
    APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED
/* The name every other refused gate action answers with. */
#define APPLIANCE_PRODUCTION_SERVING_GATE_ACTION_UNKNOWN \
    "GATE_ACTION_UNKNOWN"

static struct appliance_persistent_secret_config *
appliance_production_serving_account_store(
    struct appliance_production_serving *serving)
{
    if (serving == NULL || serving->loader == NULL) {
        return NULL;
    }
    return &serving->loader->secret_config;
}

static bool appliance_production_serving_account_rekey_host_seed(
    struct appliance_production_serving *serving,
    const char *current_password,
    size_t current_password_length,
    const char *new_password,
    size_t new_password_length)
{
    const struct appliance_state_layout *layout;
    struct block_device *partition_device;
    bool present = false;

    if (serving == NULL || serving->loader == NULL ||
        current_password == NULL || new_password == NULL) {
        return false;
    }
    layout = appliance_state_loader_layout(serving->loader);
    partition_device = (struct block_device *)
        appliance_state_loader_partition_device(serving->loader);
    if (layout == NULL || partition_device == NULL ||
        appliance_host_seed_store_present(
            partition_device, layout->block_capacity, &present) !=
            APPLIANCE_HOST_SEED_OK) {
        return false;
    }
    if (!present) {
        return true;
    }
    return appliance_host_seed_store_rekey(
               partition_device, layout->block_capacity,
               current_password, current_password_length,
               new_password, new_password_length) ==
           APPLIANCE_HOST_SEED_OK;
}

/*
 * The enrollment owner is bound after this module initialized (the console
 * binding publishes it), so the executor resolves it at stage and commit time
 * instead of capturing it; an unbound owner refuses the change instead of
 * pretending it happened.
 */
static struct appliance_production_admin_enrollment *
appliance_production_serving_account_enrollment(
    struct appliance_production_serving *serving)
{
    return serving != NULL ? serving->admin_enrollment : NULL;
}

/*
 * The stored login, read at render time; the password is never read and never
 * rendered.  An absent, legacy or unreadable record says UNAVAILABLE instead
 * of inventing a login.
 */
static void appliance_production_serving_account_render_saved(
    struct appliance_production_serving *serving)
{
    struct appliance_persistent_secret_config *store =
        appliance_production_serving_account_store(serving);
    struct appliance_admin_credential credential;
    size_t login_length = 0U;

    memset(&credential, 0, sizeof(credential));
    if (store != NULL &&
        appliance_persistent_secret_config_admin_credential(
            store, &credential) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        login_length = strlen(credential.login);
    }
    if (login_length != 0U &&
        login_length <= APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX) {
        memcpy(serving->account_saved_text, credential.login,
               login_length + 1U);
    } else {
        memset(serving->account_saved_text, 0,
               sizeof(serving->account_saved_text));
        memcpy(serving->account_saved_text, "UNAVAILABLE",
               sizeof("UNAVAILABLE"));
    }
    appliance_production_serving_zero(&credential, sizeof(credential));
}

static void appliance_production_serving_publish_account(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_account account;

    memset(&account, 0, sizeof(account));
    account.published = serving->account_published;
    if (!account.published) {
        account.reason = "NO_ACCOUNT_OWNER";
        (void)appliance_management_panel_set_account(
            &serving->panel, &account);
        return;
    }
    appliance_production_serving_account_render_saved(serving);
    account.login = serving->account_saved_text;
    (void)appliance_management_panel_set_account(&serving->panel, &account);
}

/*
 * ITEM-507 increment 5: the two bounded name-table renderings the Certificates
 * page shows.  Each declared name is followed by its certificate's byte count;
 * the key is never part of the rendering, and a table that does not fit is cut
 * at a complete entry and flagged, never silently truncated in the middle of
 * one.
 */
static void appliance_production_serving_certificates_render(
    const struct appliance_certificate_name_table *table,
    char *buffer,
    uint32_t capacity,
    uint32_t *length_out,
    uint32_t *shown_out,
    bool *truncated_out)
{
    uint32_t length = 0U;
    uint32_t shown = 0U;
    bool truncated = false;
    uint32_t index;

    *length_out = 0U;
    *shown_out = 0U;
    *truncated_out = false;
    if (table == NULL || table->count == 0U) {
        (void)appliance_production_serving_text_append(
            buffer, capacity, &length, "none");
        *length_out = length;
        return;
    }
    for (index = 0U; index < table->count; ++index) {
        const struct appliance_certificate_name *entry =
            &table->entries[index];
        const uint32_t entry_start = length;
        bool rendered = true;

        if (index != 0U) {
            rendered = appliance_production_serving_text_append(
                buffer, capacity, &length, ", ");
        }
        rendered = rendered &&
            appliance_production_serving_text_append(
                buffer, capacity, &length, entry->name) &&
            appliance_production_serving_text_append(
                buffer, capacity, &length, " (") &&
            appliance_production_serving_text_append_u64(
                buffer, capacity, &length, entry->certificate_length) &&
            appliance_production_serving_text_append(
                buffer, capacity, &length, ")");
        if (!rendered) {
            /* Cut at a complete entry and say so. */
            length = entry_start;
            buffer[length] = '\0';
            truncated = true;
            break;
        }
        ++shown;
    }
    *length_out = length;
    *shown_out = shown;
    *truncated_out = truncated;
}

static void appliance_production_serving_publish_certificates(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_certificates view;
    uint32_t active_length = 0U;
    uint32_t saved_length = 0U;
    uint32_t shown = 0U;
    bool truncated = false;

    memset(&view, 0, sizeof(view));
    /*
     * ITEM-508 increment 32: the ACME block is published beside the names, and
     * an unpublished block names its reason instead of disappearing.  It is
     * filled here once, so both the "no name owner" page and the table page
     * carry the same state.
     */
    if (serving->acme_published) {
        view.acme = serving->acme;
    } else {
        view.acme.published = false;
        view.acme.reason = "NO_ACME_STATE";
    }
    if (!serving->certificate_name_published) {
        view.published = false;
        view.reason = "NO_CERTIFICATE_NAME_OWNER";
        (void)appliance_management_panel_set_certificates(
            &serving->panel, &view);
        return;
    }
    appliance_production_serving_certificates_render(
        &serving->name_table, serving->certificates_active_text,
        sizeof(serving->certificates_active_text), &active_length, &shown,
        &truncated);
    view.count = serving->name_table.count;
    view.shown = shown;
    view.truncated = truncated;
    appliance_production_serving_certificates_render(
        &serving->name_saved, serving->certificates_saved_text,
        sizeof(serving->certificates_saved_text), &saved_length, &shown,
        &truncated);
    if (serving->name_saved.count > shown) {
        view.truncated = true;
        if (shown > view.shown) {
            view.shown = shown;
        }
        if (serving->name_saved.count > view.count) {
            view.count = serving->name_saved.count;
        }
    }
    view.published = true;
    view.active = serving->certificates_active_text;
    view.active_default =
        serving->name_table.default_mode ==
                APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE
            ? "CERTIFICATE"
            : "REFUSE";
    view.active_source =
        serving->name_store_ready
            ? appliance_certificate_name_store_source_string(
                  appliance_certificate_name_store_source(
                      &serving->name_store))
            : "UNAVAILABLE";
    view.saved = serving->certificates_saved_text;
    view.saved_default =
        serving->name_saved.default_mode ==
                APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE
            ? "CERTIFICATE"
            : "REFUSE";
    view.saved_source = view.active_source;
    view.applies = "reboot";
    (void)appliance_management_panel_set_certificates(
        &serving->panel, &view);
}

/*
 * The `[PRODUCTION-CREDENTIAL-SET]` record.  One bounded line, handed to the
 * loop like the network and system records: the source, the login the operator
 * confirmed, whether the login moved, whether the durable write proved itself
 * and whether the owner closed the issued sessions.  It never carries the
 * password or a hash of it.
 */
static void appliance_production_serving_account_record(
    struct appliance_production_serving *serving,
    bool persisted,
    bool sessions_revoked,
    bool conflict)
{
    uint32_t length = 0U;
    bool rendered = true;
    char *record = serving->account_record;

    memset(record, 0, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX);
    rendered =
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length,
            "[PRODUCTION-CREDENTIAL-SET] source=PANEL login=") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length, serving->account_candidate.login) &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length,
            serving->account_candidate.login_changed
                ? " changed=LOGIN,PASSWORD"
                : " changed=PASSWORD") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length,
            sessions_revoked ? " sessions=REVOKED" : " sessions=KEPT") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length, persisted ? " persisted=OK" : " persisted=FAILED") &&
        (!conflict ||
         appliance_production_serving_text_append(
             record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
             &length, " conflict=CHANGED"));
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX,
            &length,
            persisted
                ? "[PRODUCTION-CREDENTIAL-SET] source=PANEL persisted=OK "
                  "sessions=REVOKED"
                : (conflict
                       ? "[PRODUCTION-CREDENTIAL-SET] source=PANEL "
                         "persisted=FAILED sessions=KEPT conflict=CHANGED"
                       : "[PRODUCTION-CREDENTIAL-SET] source=PANEL "
                         "persisted=FAILED sessions=KEPT"));
    }
    serving->account_record_pending = true;
}

/*
 * True when the durable record still is the one the stage proved.  The
 * comparison covers the login, the salt, the iteration count and the verifier,
 * so a console `credentials` change or a second panel commit inside the
 * confirmation window is refused instead of being silently reverted.
 */
static bool appliance_production_serving_account_unchanged(
    const struct appliance_production_serving *serving,
    const struct appliance_admin_credential *stored)
{
    return serving->account_candidate.snapshot_valid &&
           stored != NULL &&
           strcmp(stored->login,
                  serving->account_candidate.snapshot_login) == 0 &&
           stored->iterations ==
               serving->account_candidate.snapshot_iterations &&
           memcmp(stored->salt,
                  serving->account_candidate.snapshot_salt,
                  sizeof(stored->salt)) == 0 &&
           memcmp(stored->verifier,
                  serving->account_candidate.snapshot_verifier,
                  sizeof(stored->verifier)) == 0;
}

static enum appliance_production_mutation_plane_account_status
appliance_production_serving_account_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    struct appliance_production_admin_enrollment *enrollment;
    struct appliance_persistent_secret_config *store;
    struct appliance_panel_account_form form;
    struct appliance_admin_credential stored;
    enum appliance_production_admin_enrollment_status verify_status;
    bool login_changed = false;

    if (serving == NULL || body == NULL || body_length == 0U) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    memset(&form, 0, sizeof(form));
    if (appliance_panel_account_form_parse(
            body, (uint32_t)body_length, &form) !=
        APPLIANCE_PANEL_LOGIN_OK) {
        appliance_panel_account_form_zero(&form);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    /*
     * A second stage replaces the previous candidate whole: its password is
     * zeroed before the new one is kept, so at most one candidate exists.
     */
    appliance_production_serving_zero(&serving->account_candidate,
                                      sizeof(serving->account_candidate));
    enrollment = appliance_production_serving_account_enrollment(serving);
    if (enrollment == NULL) {
        appliance_panel_account_form_zero(&form);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    /*
     * Prove the current password with the owner's verify-only entry: it opens
     * no session and counts one failure against the same lockout sign-in uses.
     * Only OK is accepted.  UNPROVISIONED is not a change proof: the panel
     * cannot be reached before an account exists (the serving gate withholds
     * the listener), so accepting it would only widen the door for a store
     * that became unreadable while the gate still held a published verifier.
     * LEGACY_RECORD is refused by name: the panel cannot migrate the old
     * fingerprint without proof, and the console's `credentials reset` is the
     * documented gesture.
     */
    verify_status = appliance_production_admin_enrollment_verify_credential(
        enrollment,
        (const char *)form.current,
        form.current_length,
        (uint64_t)hfoc_lwip_time_now());
    if (verify_status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        appliance_panel_account_form_zero(&form);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    if (appliance_admin_credential_validate_login(
            (const char *)form.login, form.login_length) !=
            APPLIANCE_ADMIN_CREDENTIAL_OK ||
        appliance_admin_credential_validate_password(
            (const char *)form.password,
            form.password_length,
            (const char *)form.login,
            form.login_length) != APPLIANCE_ADMIN_CREDENTIAL_OK ||
        form.password_length != form.repeat_length ||
        memcmp(form.password, form.repeat, form.password_length) != 0) {
        appliance_panel_account_form_zero(&form);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    /*
     * `changed=` names whether the login moved, read from the durable record
     * rather than trusted from the form; the same read is the commit's
     * conflict snapshot, so a candidate staged against one record cannot be
     * committed over another writer's record.
     */
    store = appliance_production_serving_account_store(serving);
    memset(&stored, 0, sizeof(stored));
    if (store == NULL ||
        appliance_persistent_secret_config_admin_credential(
            store, &stored) != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        appliance_production_serving_zero(&stored, sizeof(stored));
        appliance_panel_account_form_zero(&form);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED;
    }
    login_changed =
        strlen(stored.login) != (size_t)form.login_length ||
        memcmp(stored.login, form.login, form.login_length) != 0;
    memcpy(serving->account_candidate.snapshot_login, stored.login,
           strlen(stored.login) + 1U);
    memcpy(serving->account_candidate.snapshot_salt, stored.salt,
           sizeof(stored.salt));
    serving->account_candidate.snapshot_iterations = stored.iterations;
    memcpy(serving->account_candidate.snapshot_verifier, stored.verifier,
           sizeof(stored.verifier));
    serving->account_candidate.snapshot_valid = true;
    appliance_production_serving_zero(&stored, sizeof(stored));
    memcpy(serving->account_candidate.login, form.login, form.login_length);
    serving->account_candidate.login[form.login_length] = '\0';
    memcpy(serving->account_candidate.current_password, form.current,
           form.current_length);
    serving->account_candidate.current_password[form.current_length] = '\0';
    memcpy(serving->account_candidate.password, form.password,
           form.password_length);
    serving->account_candidate.password[form.password_length] = '\0';
    serving->account_candidate.login_changed = login_changed;
    serving->account_candidate.staged = true;
    appliance_panel_account_form_zero(&form);
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK;
}

static enum appliance_production_mutation_plane_account_status
appliance_production_serving_account_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    struct appliance_production_admin_enrollment *enrollment;
    struct appliance_persistent_secret_config *store;
    struct appliance_admin_credential stored;
    enum appliance_production_admin_enrollment_status status;
    bool persisted = false;
    bool sessions_revoked = false;
    bool read_back = false;

    if (serving == NULL || !serving->account_candidate.staged) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED;
    }
    enrollment = appliance_production_serving_account_enrollment(serving);
    store = appliance_production_serving_account_store(serving);
    memset(&stored, 0, sizeof(stored));
    if (enrollment == NULL || store == NULL ||
        appliance_persistent_secret_config_admin_credential(
            store, &stored) != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK ||
        !appliance_production_serving_account_unchanged(serving, &stored)) {
        /*
         * The record moved after the stage proved it (a console `credentials`
         * change, the reset gesture or a second panel commit).  The candidate
         * was proven against a record that no longer exists, so it is not
         * written: the newer record wins and the refusal is named.
         */
        appliance_production_serving_account_render_saved(serving);
        appliance_production_serving_account_record(
            serving, false, false, true);
        appliance_production_serving_zero(&stored, sizeof(stored));
        appliance_production_serving_zero(&serving->account_candidate,
                                          sizeof(serving->account_candidate));
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED;
    }
    appliance_production_serving_zero(&stored, sizeof(stored));
    if (!appliance_production_serving_account_rekey_host_seed(
            serving,
            serving->account_candidate.current_password,
            strlen(serving->account_candidate.current_password),
            serving->account_candidate.password,
            strlen(serving->account_candidate.password))) {
        appliance_production_serving_account_render_saved(serving);
        appliance_production_serving_account_record(
            serving, false, false, false);
        appliance_production_serving_zero(&serving->account_candidate,
                                          sizeof(serving->account_candidate));
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED;
    }
    status = appliance_production_admin_enrollment_provision_credential(
        enrollment,
        serving->account_candidate.login,
        strlen(serving->account_candidate.login),
        serving->account_candidate.password,
        strlen(serving->account_candidate.password),
        (uint64_t)hfoc_lwip_time_now());
    /*
     * `provision_credential()` writes the durable record and then republishes
     * the verifier; a publish failure leaves a written record the running gate
     * has not taken.  The record is read back to say what actually persisted,
     * so `persisted=` names the store and `sessions=` names the gate.
     */
    memset(&stored, 0, sizeof(stored));
    read_back =
        appliance_persistent_secret_config_admin_credential(
            store, &stored) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
        strcmp(stored.login, serving->account_candidate.login) == 0;
    appliance_production_serving_zero(&stored, sizeof(stored));
    persisted = read_back;
    sessions_revoked = status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    /*
     * The owner's setter republished the verifier and closed every session
     * when it answered OK.  The record and the page say so, and the candidate
     * is zeroed whichever way the call went.
     */
    appliance_production_serving_account_render_saved(serving);
    appliance_production_serving_account_record(
        serving, persisted, sessions_revoked, false);
    appliance_production_serving_zero(&serving->account_candidate,
                                      sizeof(serving->account_candidate));
    return status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK
               ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK
               : APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED;
}

static void appliance_production_serving_account_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        appliance_production_serving_zero(&serving->account_candidate,
                                          sizeof(serving->account_candidate));
    }
}

bool appliance_production_serving_take_account_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->account_published || !serving->account_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->account_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] = serving->account_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->account_record_pending = false;
    return true;
}

/*
 * ITEM-507 increment 3: hand the last handshake's SNI decision to the
 * ordinary-boot loop, exactly once.  One slot is enough: the loop drains it on
 * the iteration after the handshake, and a later handshake overwrites a record
 * the loop has already published.
 */
bool appliance_production_serving_take_sni_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->name_store_ready || !serving->sni_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->sni_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] = serving->sni_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->sni_record_pending = false;
    return true;
}

/*
 * ITEM-507 increment 4: the name -> certificate/key mutation.
 *
 * One two-phase mutation imports a certificate and key for an exact name,
 * deletes a declared name, or declares what an unknown name is served.  The
 * stage validates the pair through the ITEM-503 import gate against the
 * declared name and holds the candidate - including the private key, which is
 * zeroed on every path.  The commit rewrites the whole durable record and
 * reads it back; the running listener keeps the table and the parsed pairs it
 * was configured with, and the record says `applies=reboot`, so nothing the
 * panel shows disagrees with what is served.
 */

#define APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_LABEL "CERTIFICATE"
#define APPLIANCE_PRODUCTION_SERVING_PRIVATE_KEY_LABEL "PRIVATE KEY"
#define APPLIANCE_PRODUCTION_SERVING_EC_KEY_LABEL "EC PRIVATE KEY"

enum appliance_production_serving_certificate_name_action {
    APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_NONE = 0,
    APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_IMPORT = 1,
    APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DELETE = 2,
    APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT = 3,
};

static const char *appliance_production_serving_certificate_name_action_word(
    uint32_t action)
{
    switch (action) {
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_IMPORT:
        return "IMPORT";
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DELETE:
        return "DELETE";
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT:
        return "DEFAULT";
    default:
        return "NONE";
    }
}

/*
 * ITEM-515: the one decoder lives in `appliance_panel_form`; the serving
 * path's names delegate to it, so the certificate and key fields still travel
 * through the same `%XX` rules the host proof drives.
 */
static bool appliance_production_serving_form_decode(
    const uint8_t *value,
    uint32_t value_length,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length)
{
    return appliance_panel_form_decode(value, value_length, output,
                                       output_capacity, output_length);
}

static bool appliance_production_serving_form_text(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length)
{
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;

    if (!appliance_production_serving_form_field(
            body, body_length, name, &value, &value_length)) {
        return false;
    }
    return appliance_production_serving_form_decode(
        value, value_length, output, output_capacity, output_length);
}

static void appliance_production_serving_certificate_name_reset(
    struct appliance_production_serving *serving)
{
    if (serving != NULL) {
        memset(&serving->certificate_name_candidate, 0,
               sizeof(serving->certificate_name_candidate));
    }
}

static void appliance_production_serving_certificate_name_record(
    struct appliance_production_serving *serving,
    bool persisted);

static enum appliance_production_mutation_plane_certificate_name_status
appliance_production_serving_certificate_name_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    /*
     * The PEM scratch buffers and the import result hold private-key bytes on
     * the way in; they are static because the kernel stack cannot carry them,
     * and they are zeroed on every exit path.
     */
    static char action_text[16];
    static char name_text[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    static char certificate_pem[
        APPLIANCE_CERTIFICATE_UPLOAD_CERTIFICATE_PEM_MAX];
    static char key_pem[APPLIANCE_CERTIFICATE_UPLOAD_KEY_PEM_MAX];
    static char mode_text[8];
    struct appliance_certificate_import_result import_result;
    enum appliance_production_mutation_plane_certificate_name_status status =
        APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED;
    struct appliance_production_serving_certificate_name_candidate *candidate;
    uint32_t action_length = 0U;
    uint32_t name_length = 0U;
    uint32_t certificate_length = 0U;
    uint32_t key_length = 0U;
    uint32_t mode_length = 0U;
    size_t normalized_length = 0U;
    size_t decoded = 0U;
    enum pem_decode_status pem_status;
    enum appliance_certificate_import_status import_status;

    memset(action_text, 0, sizeof(action_text));
    memset(name_text, 0, sizeof(name_text));
    memset(certificate_pem, 0, sizeof(certificate_pem));
    memset(key_pem, 0, sizeof(key_pem));
    memset(mode_text, 0, sizeof(mode_text));
    memset(&import_result, 0, sizeof(import_result));
    if (serving == NULL || body == NULL || body_length == 0U ||
        body_length > UINT32_MAX) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED;
    }
    appliance_production_serving_certificate_name_reset(serving);
    candidate = &serving->certificate_name_candidate;
    /*
     * ITEM-515: the action is read by the same one-field grammar the host
     * proof drives with the drawn body; an absent, empty or undecodable action
     * is a refusal here, before any candidate exists.
     */
    if (appliance_panel_form_action_parse(
            body, (uint32_t)body_length, (uint8_t *)action_text,
            (uint32_t)sizeof(action_text), &action_length) !=
        APPLIANCE_PANEL_FORM_OK) {
        goto refuse;
    }
    if (action_length == 6U &&
        memcmp(action_text, "import", 6U) == 0) {
        candidate->action =
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_IMPORT;
    } else if (action_length == 6U &&
               memcmp(action_text, "delete", 6U) == 0) {
        candidate->action =
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DELETE;
    } else if (action_length == 7U &&
               memcmp(action_text, "default", 7U) == 0) {
        candidate->action =
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT;
    } else {
        goto refuse;
    }
    if (candidate->action ==
        APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT) {
        if (!appliance_production_serving_form_text(
                body, (uint32_t)body_length, "mode", (uint8_t *)mode_text,
                sizeof(mode_text), &mode_length)) {
            goto refuse;
        }
        if (mode_length == 2U && memcmp(mode_text, "on", 2U) == 0) {
            candidate->make_default = true;
        } else if (mode_length == 3U &&
                   memcmp(mode_text, "off", 3U) == 0) {
            candidate->make_default = false;
        } else {
            goto refuse;
        }
        candidate->staged = true;
        status = APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK;
        goto done;
    }
    if (!appliance_production_serving_form_text(
            body, (uint32_t)body_length, "name", (uint8_t *)name_text,
            sizeof(name_text), &name_length) ||
        name_length == 0U ||
        appliance_route_name_normalize(
            name_text, name_length, candidate->name,
            sizeof(candidate->name), &normalized_length) !=
            APPLIANCE_ROUTE_OK) {
        goto refuse;
    }
    candidate->name_length = (uint32_t)normalized_length;
    if (candidate->action ==
        APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DELETE) {
        candidate->staged = true;
        status = APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK;
        goto done;
    }
    if (!appliance_production_serving_form_text(
            body, (uint32_t)body_length, "certificate",
            (uint8_t *)certificate_pem, sizeof(certificate_pem),
            &certificate_length) ||
        certificate_length == 0U ||
        !appliance_production_serving_form_text(
            body, (uint32_t)body_length, "key", (uint8_t *)key_pem,
            sizeof(key_pem), &key_length) ||
        key_length == 0U) {
        goto refuse;
    }
    pem_status = pem_decode_block(
        certificate_pem, certificate_length,
        APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_LABEL,
        candidate->certificate_der, sizeof(candidate->certificate_der),
        &decoded);
    if (pem_status != PEM_DECODE_OK) {
        goto refuse;
    }
    candidate->certificate_length = (uint32_t)decoded;
    pem_status = pem_decode_block(
        key_pem, key_length, APPLIANCE_PRODUCTION_SERVING_PRIVATE_KEY_LABEL,
        candidate->key_der, sizeof(candidate->key_der), &decoded);
    if (pem_status == PEM_DECODE_NO_BLOCK) {
        pem_status = pem_decode_block(
            key_pem, key_length, APPLIANCE_PRODUCTION_SERVING_EC_KEY_LABEL,
            candidate->key_der, sizeof(candidate->key_der), &decoded);
    }
    if (pem_status != PEM_DECODE_OK) {
        goto refuse;
    }
    candidate->key_length = (uint32_t)decoded;
    candidate->make_default = appliance_production_serving_form_flag(
        body, (uint32_t)body_length, "default");
    /*
     * The ITEM-503 import gate is the only validator: it proves the pair, the
     * validity window, the SAN and that the certificate really names the
     * declared name, so a name can never be pointed at somebody else's
     * certificate.
     */
    import_status = appliance_certificate_import_validate(
        candidate->certificate_der, candidate->certificate_length,
        candidate->key_der, candidate->key_length, NULL, 0U,
        serving->now_epoch_seconds, candidate->name,
        candidate->name_length, appliance_production_entropy_random, NULL,
        &import_result);
    if (import_status != APPLIANCE_CERTIFICATE_IMPORT_OK) {
        /*
         * ITEM-649: the panel must not collapse an unverified clock into the
         * generic REFUSED.  The named status reaches the operator as
         * `{"refused":"BAD_CLOCK"}`; the candidate, including the private key,
         * is zeroed before the answer.
         */
        if (import_status == APPLIANCE_CERTIFICATE_IMPORT_BAD_CLOCK) {
            appliance_production_serving_certificate_name_reset(serving);
            status =
                APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_BAD_CLOCK;
            goto done;
        }
        goto refuse;
    }
    candidate->staged = true;
    status = APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK;
    goto done;

refuse:
    appliance_production_serving_certificate_name_reset(serving);
done:
    memset(certificate_pem, 0, sizeof(certificate_pem));
    memset(key_pem, 0, sizeof(key_pem));
    memset(name_text, 0, sizeof(name_text));
    memset(mode_text, 0, sizeof(mode_text));
    memset(&import_result, 0, sizeof(import_result));
    return status;
}

static enum appliance_production_mutation_plane_certificate_name_status
appliance_production_serving_certificate_name_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    /* 14 KiB of table does not belong on the kernel stack. */
    static struct appliance_certificate_name_table table;
    struct appliance_production_serving_certificate_name_candidate *candidate;
    bool changed = false;
    bool persisted = false;

    if (serving == NULL || !serving->certificate_name_candidate.staged) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_APPLY_FAILED;
    }
    candidate = &serving->certificate_name_candidate;
    /*
     * The mutation builds on the durable table as last read or written, not
     * on the boot-time table the listener is serving: a second change must
     * keep the first one's result.
     */
    table = serving->name_saved;
    switch (candidate->action) {
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_IMPORT:
        /*
         * An import replaces a declaration of the same name: the operator is
         * correcting the certificate, not declaring the name twice.
         */
        (void)appliance_certificate_name_table_remove(
            &table, candidate->name, candidate->name_length);
        changed = appliance_certificate_name_table_add(
                      &table, candidate->name, candidate->name_length,
                      candidate->certificate_der,
                      candidate->certificate_length, candidate->key_der,
                      candidate->key_length) ==
                  APPLIANCE_CERTIFICATE_NAME_OK;
        if (changed && candidate->make_default) {
            changed = appliance_certificate_name_table_set_default(
                          &table,
                          APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE) ==
                      APPLIANCE_CERTIFICATE_NAME_OK;
        }
        break;
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DELETE:
        changed = appliance_certificate_name_table_remove(
                      &table, candidate->name, candidate->name_length) ==
                  APPLIANCE_CERTIFICATE_NAME_OK;
        break;
    case APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT:
        changed = appliance_certificate_name_table_set_default(
                      &table,
                      candidate->make_default
                          ? APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE
                          : APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE) ==
                  APPLIANCE_CERTIFICATE_NAME_OK;
        break;
    default:
        changed = false;
        break;
    }
    if (changed) {
        persisted =
            appliance_certificate_name_store_save(
                &serving->name_store, &table) ==
            APPLIANCE_CERTIFICATE_NAME_STORE_OK;
        if (persisted) {
            serving->name_saved = table;
        }
    }
    appliance_production_serving_certificate_name_record(serving, persisted);
    appliance_production_serving_certificate_name_reset(serving);
    return persisted
        ? APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK
        : APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_APPLY_FAILED;
}

static void appliance_production_serving_certificate_name_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        appliance_production_serving_certificate_name_reset(serving);
    }
}

static void appliance_production_serving_certificate_name_record(
    struct appliance_production_serving *serving,
    bool persisted)
{
    uint32_t length = 0U;
    bool rendered;
    char *record = serving->certificate_name_record;
    const struct appliance_production_serving_certificate_name_candidate
        *candidate = &serving->certificate_name_candidate;
    const char *name =
        candidate->action ==
                APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_ACTION_DEFAULT
            ? "-"
            : candidate->name;
    const char *default_word =
        serving->name_saved.default_mode ==
                APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE
            ? " default=CERTIFICATE"
            : " default=REFUSE";

    memset(record, 0,
           APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX);
    rendered =
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, "[PRODUCTION-CERTIFICATE-NAME] source=PANEL action=") &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length,
            appliance_production_serving_certificate_name_action_word(
                candidate->action)) &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, " name=") &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, name) &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, " names=") &&
        appliance_production_serving_text_append_u64(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, serving->name_saved.count) &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, default_word) &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, persisted ? " persisted=OK" : " persisted=FAILED") &&
        appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length, " applies=reboot");
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_text_append(
            record,
            APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX,
            &length,
            persisted
                ? "[PRODUCTION-CERTIFICATE-NAME] source=PANEL "
                  "persisted=OK applies=reboot"
                : "[PRODUCTION-CERTIFICATE-NAME] source=PANEL "
                  "persisted=FAILED applies=reboot");
    }
    serving->certificate_name_record_pending = true;
}

bool appliance_production_serving_take_certificate_name_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->certificate_name_published ||
        !serving->certificate_name_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->certificate_name_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] =
            serving->certificate_name_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->certificate_name_record_pending = false;
    return true;
}

/*
 * ITEM-508 increment 34: the durable ACME control.
 *
 * One two-phase mutation enables ACME, disables it - clearing any pending
 * issuance request with it - or requests one issuance while ACME is enabled.
 * The commit writes the control record next to the certificate record and the
 * name table, proves the write by reading it back, and only then reports
 * applied; the running service reads the committed value on its next poll.
 * The candidate is one action word and holds no material, so stage and
 * rollback have nothing to zero.
 */
enum appliance_production_serving_acme_action {
    APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_NONE = 0,
    APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_ENABLE = 1,
    APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_DISABLE = 2,
    APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_REQUEST = 3,
};

static const char *appliance_production_serving_acme_action_word(
    uint32_t action)
{
    switch (action) {
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_ENABLE:
        return "ENABLE";
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_DISABLE:
        return "DISABLE";
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_REQUEST:
        return "REQUEST";
    default:
        return "NONE";
    }
}

static void appliance_production_serving_acme_reset(
    struct appliance_production_serving *serving)
{
    if (serving != NULL) {
        memset(&serving->acme_candidate, 0, sizeof(serving->acme_candidate));
    }
}

static void appliance_production_serving_publish_acme_control(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_acme_control view;

    if (serving == NULL) {
        return;
    }
    memset(&view, 0, sizeof(view));
    if (!serving->acme_control_loaded) {
        view.published = false;
        view.reason = "NO_ACME_CONTROL_RECORD";
        (void)appliance_management_panel_set_acme_control(
            &serving->panel, &view);
        return;
    }
    view.enabled = appliance_acme_control_enabled(&serving->acme_control)
                       ? "ENABLED"
                       : "DISABLED";
    view.requested =
        appliance_acme_control_order_requested(&serving->acme_control)
            ? "REQUESTED"
            : "NONE";
    if (appliance_acme_control_order_requested(&serving->acme_control)) {
        uint32_t length = 0U;

        memset(serving->acme_requested_at_text, 0,
               sizeof(serving->acme_requested_at_text));
        (void)appliance_production_serving_text_append_u64(
            serving->acme_requested_at_text,
            (uint32_t)sizeof(serving->acme_requested_at_text), &length,
            (uint64_t)appliance_acme_control_order_requested_at(
                &serving->acme_control));
        view.requested_at = serving->acme_requested_at_text;
    } else {
        view.requested_at = "-";
    }
    view.source = appliance_acme_control_source_string(
        appliance_acme_control_store_source(&serving->acme_control_store));
    /*
     * The running service reads the committed value on its next poll, so the
     * page says `service`, not a reboot the operator would wait for.
     */
    view.applies = "service";
    view.published = true;
    (void)appliance_management_panel_set_acme_control(
        &serving->panel, &view);
}

/*
 * ITEM-516: the management gate's view and its close-only executor.
 *
 * The page shows the policy the system record holds and the full warning; the
 * executor can only ever close it.  Opening is the local console's, behind a
 * proven local secret, so there is no panel or API path that writes `open` -
 * an attempt is refused by name before anything is staged or written.
 */
static void appliance_production_serving_gate_reset(
    struct appliance_production_serving *serving)
{
    if (serving != NULL) {
        memset(&serving->gate_candidate, 0, sizeof(serving->gate_candidate));
    }
}

static void appliance_production_serving_publish_gate(
    struct appliance_production_serving *serving)
{
    struct appliance_management_web_ui_gate view;
    bool open = false;

    if (serving == NULL) {
        return;
    }
    memset(&view, 0, sizeof(view));
    if (serving->loader == NULL ||
        appliance_persistent_config_panel_after_boot(
            &serving->loader->system_config, &open) !=
            APPLIANCE_PERSISTENT_CONFIG_OK) {
        view.published = false;
        view.reason = "NO_SYSTEM_RECORD";
        serving->gate_published = false;
        (void)appliance_management_panel_set_gate(&serving->panel, &view);
        return;
    }
    view.setting = open ? "OPEN_AFTER_BOOT" : "CLOSED_AFTER_BOOT";
    view.source = appliance_persistent_config_source(
                      &serving->loader->system_config) ==
                          APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED
                      ? "COMMITTED"
                      : "DEFAULT";
    /* The policy is the boot's decision, so the page says `reboot`. */
    view.applies = "reboot";
    view.refusal = serving->gate_refusal;
    view.published = true;
    serving->gate = view;
    serving->gate_published = true;
    (void)appliance_management_panel_set_gate(&serving->panel, &view);
}

static enum appliance_production_mutation_plane_panel_gate_status
appliance_production_serving_gate_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    static char action_text[16];
    uint32_t action_length = 0U;

    if (serving == NULL || serving->loader == NULL) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
    }
    appliance_production_serving_gate_reset(serving);
    serving->gate_refusal = NULL;
    memset(action_text, 0, sizeof(action_text));
    /*
     * ITEM-515: the one-field action grammar is shared with the host proof
     * that renders the gate page and submits the drawn body.
     */
    if (appliance_panel_form_action_parse(
            body, (uint32_t)body_length, (uint8_t *)action_text,
            (uint32_t)sizeof(action_text), &action_length) !=
        APPLIANCE_PANEL_FORM_OK) {
        serving->gate_refusal = APPLIANCE_PRODUCTION_SERVING_GATE_ACTION_UNKNOWN;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
    }
    if (strcmp(action_text, "close") == 0) {
        serving->gate_candidate.staged = true;
        serving->gate_candidate.open = false;
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK;
    }
    /*
     * `open` - and every other word - is refused by name.  Opening is the
     * local console's and needs a proven local secret; the panel has no path
     * to it, and nothing durable is written for the attempt.
     */
    serving->gate_refusal =
        strcmp(action_text, "open") == 0
            ? APPLIANCE_PRODUCTION_SERVING_GATE_LOCAL_SECRET_REQUIRED
            : APPLIANCE_PRODUCTION_SERVING_GATE_ACTION_UNKNOWN;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
}

static enum appliance_production_mutation_plane_panel_gate_status
appliance_production_serving_gate_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    bool open = false;

    if (serving == NULL || serving->loader == NULL ||
        !serving->gate_candidate.staged || serving->gate_candidate.open) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED;
    }
    if (appliance_persistent_config_set_panel_after_boot(
            &serving->loader->system_config, false) !=
        APPLIANCE_PERSISTENT_CONFIG_OK) {
        appliance_production_serving_gate_reset(serving);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_APPLY_FAILED;
    }
    /*
     * The write is proved by reading the record back through its own getter:
     * a setter that returned OK without moving the value would otherwise be
     * reported as applied.
     */
    if (appliance_persistent_config_panel_after_boot(
            &serving->loader->system_config, &open) !=
            APPLIANCE_PERSISTENT_CONFIG_OK ||
        open) {
        appliance_production_serving_gate_reset(serving);
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_APPLY_FAILED;
    }
    serving->gate_refusal = NULL;
    appliance_production_serving_gate_reset(serving);
    appliance_production_serving_publish_gate(serving);
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK;
}

static void appliance_production_serving_gate_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        appliance_production_serving_gate_reset(serving);
    }
}

static void appliance_production_serving_acme_record(
    struct appliance_production_serving *serving,
    bool persisted)
{
    uint32_t length = 0U;
    bool rendered;
    char *record = serving->acme_record;
    const struct appliance_acme_control *control = &serving->acme_control;

    memset(record, 0, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX);
    rendered =
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            "[PRODUCTION-ACME-CONTROL] source=PANEL action=") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            appliance_production_serving_acme_action_word(
                serving->acme_candidate.action)) &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            appliance_acme_control_enabled(control) ? " enabled=1"
                                                    : " enabled=0") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            appliance_acme_control_order_requested(control)
                ? " requested=1"
                : " requested=0") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            " at=") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            appliance_acme_control_order_requested(control)
                ? serving->acme_requested_at_text
                : "-") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            persisted ? " persisted=OK" : " persisted=FAILED") &&
        appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            " applies=service");
    if (!rendered) {
        length = 0U;
        (void)appliance_production_serving_text_append(
            record, APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX, &length,
            persisted
                ? "[PRODUCTION-ACME-CONTROL] source=PANEL persisted=OK "
                  "applies=service"
                : "[PRODUCTION-ACME-CONTROL] source=PANEL persisted=FAILED "
                  "applies=service");
    }
    serving->acme_record_pending = true;
}

static enum appliance_production_mutation_plane_acme_status
appliance_production_serving_acme_stage(
    void *context,
    const uint8_t *body,
    size_t body_length)
{
    struct appliance_production_serving *serving = context;
    static char action_text[16];
    uint32_t action_length = 0U;

    if (serving == NULL || !serving->acme_control_loaded) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
    }
    appliance_production_serving_acme_reset(serving);
    memset(action_text, 0, sizeof(action_text));
    /*
     * ITEM-515: the one-field action grammar is shared with the host proof
     * that renders the ACME page and submits the drawn body.
     */
    if (appliance_panel_form_action_parse(
            body, (uint32_t)body_length, (uint8_t *)action_text,
            (uint32_t)sizeof(action_text), &action_length) !=
        APPLIANCE_PANEL_FORM_OK) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
    }
    if (strcmp(action_text, "enable") == 0) {
        serving->acme_candidate.action =
            APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_ENABLE;
    } else if (strcmp(action_text, "disable") == 0) {
        serving->acme_candidate.action =
            APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_DISABLE;
    } else if (strcmp(action_text, "request") == 0) {
        /*
         * A request that cannot run is refused at stage with a named line;
         * the plane then reports APPLY_FAILED, and nothing durable is written.
         */
        if (!appliance_acme_control_enabled(&serving->acme_control) ||
            appliance_acme_control_order_requested(&serving->acme_control)) {
            serving->acme_candidate.action =
                APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_REQUEST;
            appliance_production_serving_acme_record(serving, false);
            appliance_production_serving_acme_reset(serving);
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
        }
        serving->acme_candidate.action =
            APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_REQUEST;
    } else {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
    }
    serving->acme_candidate.staged = true;
    return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_OK;
}

static enum appliance_production_mutation_plane_acme_status
appliance_production_serving_acme_commit(void *context)
{
    struct appliance_production_serving *serving = context;
    struct appliance_acme_control candidate;
    bool persisted = false;

    if (serving == NULL || !serving->acme_control_loaded ||
        !serving->acme_candidate.staged) {
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
    }
    candidate = serving->acme_control;
    switch (serving->acme_candidate.action) {
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_ENABLE:
        if (appliance_acme_control_set_enabled(&candidate, true) !=
            APPLIANCE_ACME_CONTROL_OK) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_APPLY_FAILED;
        }
        break;
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_DISABLE:
        if (appliance_acme_control_set_enabled(&candidate, false) !=
            APPLIANCE_ACME_CONTROL_OK) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_APPLY_FAILED;
        }
        break;
    case APPLIANCE_PRODUCTION_SERVING_ACME_ACTION_REQUEST:
        if (appliance_acme_control_request_order(
                &candidate, serving->now_epoch_seconds) !=
            APPLIANCE_ACME_CONTROL_OK) {
            return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_APPLY_FAILED;
        }
        break;
    default:
        return APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED;
    }
    if (appliance_acme_control_store_save(
            &serving->acme_control_store, &candidate) ==
        APPLIANCE_ACME_CONTROL_OK) {
        serving->acme_control = candidate;
        persisted = true;
        appliance_production_serving_publish_acme_control(serving);
    }
    appliance_production_serving_acme_record(serving, persisted);
    appliance_production_serving_acme_reset(serving);
    return persisted ? APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_OK
                     : APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_APPLY_FAILED;
}

static void appliance_production_serving_acme_rollback(void *context)
{
    struct appliance_production_serving *serving = context;

    if (serving != NULL) {
        appliance_production_serving_acme_reset(serving);
    }
}

bool appliance_production_serving_take_acme_record(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    if (!serving->acme_record_pending) {
        return false;
    }
    *output_length = 0U;
    while (serving->acme_record[*output_length] != '\0') {
        if (*output_length + 1U >= capacity) {
            /* A buffer that is too small drops nothing: the record stays. */
            *output_length = 0U;
            return false;
        }
        output[*output_length] = serving->acme_record[*output_length];
        ++(*output_length);
    }
    output[*output_length] = '\0';
    serving->acme_record_pending = false;
    return true;
}

const struct appliance_acme_control *appliance_production_serving_acme_control(
    void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    return serving->acme_control_loaded ? &serving->acme_control : NULL;
}

bool appliance_production_serving_acme_take_request(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_acme_control candidate;

    if (!serving->acme_control_loaded) {
        return false;
    }
    candidate = serving->acme_control;
    if (appliance_acme_control_take_request(&candidate) !=
        APPLIANCE_ACME_CONTROL_OK) {
        return false;
    }
    /*
     * The record is cleared before the order speaks to the CA: a failed write
     * leaves the request pending in memory and on the volume, and the caller
     * must not report a request consumed that the next boot would resurrect.
     */
    if (appliance_acme_control_store_save(&serving->acme_control_store,
                                          &candidate) !=
        APPLIANCE_ACME_CONTROL_OK) {
        return false;
    }
    serving->acme_control = candidate;
    appliance_production_serving_publish_acme_control(serving);
    return true;
}

uint32_t appliance_production_serving_acme_identifier_count(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    return serving->name_saved.count;
}

bool appliance_production_serving_acme_identifier(
    uint32_t index,
    const char **name_out,
    size_t *name_length_out)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    const struct appliance_certificate_name *entry;

    if (name_out == NULL || name_length_out == NULL ||
        index >= serving->name_saved.count) {
        return false;
    }
    entry = &serving->name_saved.entries[index];
    *name_out = entry->name;
    *name_length_out = (size_t)entry->name_length;
    return true;
}

static void appliance_production_serving_load_acme_control(
    struct appliance_production_serving *serving)
{
    appliance_acme_control_store_init(&serving->acme_control_store);
    (void)appliance_acme_control_store_load(
        &serving->acme_control_store, serving->loader,
        &serving->acme_control);
    /*
     * The store publishes a value even when it could not be read (the off
     * state with a named source); the flag says the page may show it.  A value
     * that could not be read is published as it is - off - and the source
     * names why, instead of pretending a record was committed.
     */
    serving->acme_control_loaded = true;
    appliance_production_serving_publish_acme_control(serving);
}

static bool appliance_production_serving_acme_target(
    const struct http_request_line *line)
{
    if (line == NULL || line->target == NULL) {
        return false;
    }
    return (line->target_length == HTTP_POLICY_ACME_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_ACME_TARGET,
                   HTTP_POLICY_ACME_TARGET_LENGTH) == 0) ||
           (line->target_length == HTTP_POLICY_PANEL_ACME_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_PANEL_ACME_TARGET,
                   HTTP_POLICY_PANEL_ACME_TARGET_LENGTH) == 0);
}

/* ITEM-516: the management gate's panel page and scriptable mutation target. */
static bool appliance_production_serving_gate_target(
    const struct http_request_line *line)
{
    if (line == NULL || line->target == NULL) {
        return false;
    }
    return (line->target_length == HTTP_POLICY_GATE_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_GATE_TARGET,
                   HTTP_POLICY_GATE_TARGET_LENGTH) == 0) ||
           (line->target_length == HTTP_POLICY_PANEL_GATE_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_PANEL_GATE_TARGET,
                   HTTP_POLICY_PANEL_GATE_TARGET_LENGTH) == 0);
}

static bool appliance_production_serving_certificate_target(
    const struct http_request_line *line)
{
    if (line == NULL || line->target == NULL) {
        return false;
    }
    return (line->target_length == HTTP_POLICY_SECURITY_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_SECURITY_TARGET,
                   HTTP_POLICY_SECURITY_TARGET_LENGTH) == 0) ||
           (line->target_length ==
                HTTP_POLICY_CERTIFICATE_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_CERTIFICATE_TARGET,
                   HTTP_POLICY_CERTIFICATE_TARGET_LENGTH) == 0);
}

/*
 * ITEM-503: the BYO certificate request path.
 *
 * Both the panel form (`POST /panel/security`) and the scriptable target
 * (`POST /api/v1/system/certificate`) resolve to the same mutation.  The
 * request gate is the mutation plane's own authorization, presented with the
 * same bearer token the panel accepts, so a cookie session and an
 * `Authorization: Bearer` header are the same principal.  The panel form is
 * answered with a 303 back to its page (which renders the pending token); the
 * API target is answered with the one-line state record, because a script
 * needs the token in the reply.
 */
static bool appliance_production_serving_handle_certificate(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (!appliance_production_serving_certificate_target(line) ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    if (!serving->mutations_published) {
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    panel_target =
        line->target_length == HTTP_POLICY_SECURITY_TARGET_LENGTH;
    /*
     * The body is a form and nothing else.  The upload composition carries the
     * frozen content type in its stage call, so the wire header is checked
     * here; a request that claims another type is refused before the body is
     * decoded.
     */
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE,
                   APPLIANCE_CERTIFICATE_UPLOAD_CONTENT_TYPE_LENGTH) != 0) {
            ++serving->rejected_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        /*
         * The import is durable; the listener keeps serving the certificate
         * it loaded at boot, so the record says honestly that a reboot
         * applies it.
         */
        ++serving->certificate_import_count;
        serving->certificate_pending_reboot = true;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION &&
               status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SECURITY_PATH,
                HTTP_POLICY_SECURITY_TARGET_LENGTH);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = appliance_certificate_upload_status_string(
            serving->certificate_upload.last_status);
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

static bool appliance_production_serving_routing_target(
    const struct http_request_line *line)
{
    if (line == NULL || line->target == NULL) {
        return false;
    }
    return (line->target_length == HTTP_POLICY_ROUTING_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_ROUTING_TARGET,
                   HTTP_POLICY_ROUTING_TARGET_LENGTH) == 0) ||
           (line->target_length == HTTP_POLICY_ROUTES_TARGET_LENGTH &&
            memcmp(line->target, HTTP_POLICY_ROUTES_TARGET,
                   HTTP_POLICY_ROUTES_TARGET_LENGTH) == 0);
}

/*
 * ITEM-504: the route-configuration request path.
 *
 * Both the panel form (`POST /panel/routing`) and the scriptable target
 * (`POST /api/v1/system/routes`) resolve to the same mutation, exactly as the
 * certificate import does.  The request gate is the mutation plane's own
 * authorization, presented with the same bearer token the panel accepts, so a
 * cookie session and an `Authorization: Bearer` header are the same
 * principal.  The panel form is answered with a 303 back to its page; the API
 * target is answered with the one-line state record, because a script needs
 * the token in the reply.
 *
 * This increment makes the mutation reachable and auditable over HTTP; the
 * panel page that renders the form and the applied table is the next
 * increment, and until it exists a GET of `/panel/routing` keeps answering
 * the panel's own unknown-route response.
 */
static bool appliance_production_serving_handle_routing(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (!appliance_production_serving_routing_target(line) ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    panel_target =
        line->target_length == HTTP_POLICY_ROUTING_TARGET_LENGTH;
    if (!serving->routing_published) {
        if (panel_target) {
            /* No routing owner is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    /*
     * The body is a form and nothing else.  The executor's stage call carries
     * the frozen content type, so the wire header is checked here; a request
     * that claims another type is refused before the body is decoded.
     */
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length != APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE,
                   APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH) != 0) {
            ++serving->rejected_count;
            ++serving->route_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        /*
         * The applied table is replaced whole and the record says so.  The
         * table is held in volatile memory in this increment: a reboot
         * restores the boot-time table, and persisting it is the next
         * increment.  The count names exactly what happened here.
         */
        ++serving->route_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->route_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->route_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->route_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->route_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = appliance_route_config_status_string(
            serving->route_config.last_status);
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * ITEM-505: the network-configuration request path.
 *
 * Both the panel form (`POST /panel/network`) and the scriptable target
 * (`POST /api/v1/network`) resolve to the same `NETWORK_CONFIG` mutation,
 * exactly as the certificate import and the routes do.  The request gate is
 * the mutation plane's own authorization, presented with the same bearer
 * token the panel accepts, so a cookie session and an `Authorization: Bearer`
 * header are the same principal.  The panel form is answered with a 303 back
 * to its page; the API target is answered with the one-line state record,
 * because a script needs the token in the reply.
 */
static bool appliance_production_serving_handle_network(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    panel_target =
        line->target_length == HTTP_POLICY_PANEL_NETWORK_TARGET_LENGTH &&
        memcmp(line->target, HTTP_POLICY_PANEL_NETWORK_TARGET,
               HTTP_POLICY_PANEL_NETWORK_TARGET_LENGTH) == 0;
    if (!panel_target &&
        !(line->target_length == HTTP_POLICY_NETWORK_TARGET_LENGTH &&
          memcmp(line->target, HTTP_POLICY_NETWORK_TARGET,
                 HTTP_POLICY_NETWORK_TARGET_LENGTH) == 0)) {
        return false;
    }
    if (!serving->network_published) {
        if (panel_target) {
            /* No network owner is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_NETWORK_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_NETWORK_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_NETWORK_CONTENT_TYPE_LENGTH)
                != 0) {
            ++serving->rejected_count;
            ++serving->network_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    /*
     * ITEM-515: the stage's named refusal belongs to this request only.  A
     * confirmation commit that fails must not be answered with a stale stage
     * refusal, so the name is dropped before the plane is asked.
     */
    serving->network_refusal = NULL;
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        ++serving->network_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->network_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->network_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->network_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->network_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        /*
         * ITEM-515: a stage refusal names the field that failed; a commit's
         * own failure keeps the generic word.
         */
        reason = serving->network_refusal != NULL
            ? serving->network_refusal
            : "REFUSED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * ITEM-505 increment 3: the system-hostname request path.
 *
 * Both the panel form (`POST /panel/system`) and the scriptable target
 * (`POST /api/v1/system`) resolve to the same `SYSTEM_HOSTNAME` mutation.  The
 * GET of `/api/v1/system` never reaches this handler: it keeps its read-only
 * meaning and is answered by the management API.  The request gate is the
 * mutation plane's own authorization, presented with the same bearer token the
 * panel accepts, so a cookie session and an `Authorization: Bearer` header are
 * the same principal.  The panel form is answered with a 303 back to its page
 * (which renders the pending token); the API target is answered with the
 * one-line state record, because a script needs the token in the reply.
 */
static bool appliance_production_serving_handle_system(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    panel_target =
        line->target_length == HTTP_POLICY_PANEL_SYSTEM_TARGET_LENGTH &&
        memcmp(line->target, HTTP_POLICY_PANEL_SYSTEM_TARGET,
               HTTP_POLICY_PANEL_SYSTEM_TARGET_LENGTH) == 0;
    if (!panel_target &&
        !(line->target_length == HTTP_POLICY_SYSTEM_TARGET_LENGTH &&
          memcmp(line->target, HTTP_POLICY_SYSTEM_TARGET,
                 HTTP_POLICY_SYSTEM_TARGET_LENGTH) == 0)) {
        return false;
    }
    if (!serving->system_published) {
        if (panel_target) {
            /* No system owner is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_SYSTEM_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_SYSTEM_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_SYSTEM_CONTENT_TYPE_LENGTH)
                != 0) {
            ++serving->rejected_count;
            ++serving->system_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        ++serving->system_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->system_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->system_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->system_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->system_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = "REFUSED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * ITEM-506 increment 6: the credential-change request path.
 *
 * Both the panel form (`POST /panel/account`) and the scriptable target
 * (`POST /api/v1/system/account`) resolve to the same `ACCOUNT_CREDENTIAL`
 * mutation.  The request gate is the mutation plane's own authorization,
 * presented with the same bearer token the panel accepts, so a cookie session
 * and an `Authorization: Bearer` header are the same principal.  The panel
 * form is answered with a 303 back to its page; the API target is answered
 * with the one-line state record, because a script needs the token in the
 * reply.  The commit closes every session through the owner, including the one
 * that confirmed it, so the 303 is the last answer that session receives and
 * the next request with the same cookie is the sign-in page.
 */
static bool appliance_production_serving_handle_account(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    panel_target =
        line->target_length == HTTP_POLICY_PANEL_ACCOUNT_TARGET_LENGTH &&
        memcmp(line->target, HTTP_POLICY_PANEL_ACCOUNT_TARGET,
               HTTP_POLICY_PANEL_ACCOUNT_TARGET_LENGTH) == 0;
    if (!panel_target &&
        !(line->target_length == HTTP_POLICY_ACCOUNT_TARGET_LENGTH &&
          memcmp(line->target, HTTP_POLICY_ACCOUNT_TARGET,
                 HTTP_POLICY_ACCOUNT_TARGET_LENGTH) == 0)) {
        return false;
    }
    if (!serving->account_published) {
        if (panel_target) {
            /* No account owner is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_ACCOUNT_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_ACCOUNT_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_ACCOUNT_CONTENT_TYPE_LENGTH)
                != 0) {
            ++serving->rejected_count;
            ++serving->account_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        ++serving->account_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->account_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->account_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->account_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->account_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = "REFUSED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

static bool appliance_production_serving_handle_certificate_name(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    /*
     * ITEM-507 increments 4-5: the panel form and the scriptable API target
     * are the same mutation.  Only POST is accepted; the GET of
     * `/panel/certificates` keeps resolving to the rendered page, and the API
     * target has no read route.
     */
    panel_target =
        line->target_length == HTTP_POLICY_PANEL_CERTIFICATES_TARGET_LENGTH &&
        memcmp(line->target, HTTP_POLICY_PANEL_CERTIFICATES_TARGET,
               HTTP_POLICY_PANEL_CERTIFICATES_TARGET_LENGTH) == 0;
    if (!panel_target &&
        !(line->target_length == HTTP_POLICY_CERTIFICATE_NAME_TARGET_LENGTH &&
          memcmp(line->target, HTTP_POLICY_CERTIFICATE_NAME_TARGET,
                 HTTP_POLICY_CERTIFICATE_NAME_TARGET_LENGTH) == 0)) {
        return false;
    }
    if (!serving->certificate_name_published) {
        if (panel_target) {
            /* No certificate-name owner is published: the panel answers its
             * own 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_CONTENT_TYPE_LENGTH)
                != 0) {
            ++serving->rejected_count;
            ++serving->certificate_name_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        ++serving->certificate_name_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->certificate_name_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->certificate_name_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->certificate_name_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->certificate_name_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = appliance_production_mutation_plane_certificate_name_refusal(
            &serving->mutations);
        if (reason == NULL) {
            reason = "REFUSED";
        }
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * ITEM-508 increment 34: the durable ACME control request path.
 *
 * Both the panel control page (`POST /panel/acme`) and the scriptable target
 * (`POST /api/v1/system/acme`) resolve to the same mutation.  The request gate
 * is the mutation plane's own authorization, presented with the same bearer
 * token the panel accepts, so a cookie session and an `Authorization: Bearer`
 * header are the same principal.  The panel form is answered with a 303 back to
 * its page (which renders the pending token); the API target is answered with
 * the one-line state record, because a script needs the token in the reply.
 */
static bool appliance_production_serving_handle_acme(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST ||
        !appliance_production_serving_acme_target(line)) {
        return false;
    }
    panel_target = line->target_length == HTTP_POLICY_PANEL_ACME_TARGET_LENGTH;
    if (!serving->acme_executor_bound) {
        if (panel_target) {
            /* No ACME control owner is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_ACME_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_ACME_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_ACME_CONTENT_TYPE_LENGTH) !=
                0) {
            ++serving->rejected_count;
            ++serving->acme_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        /*
         * The control record is durable and the running service reads it on
         * its next poll; the page the operator returns to shows the committed
         * value, not a promise about a later boot.
         */
        ++serving->acme_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->acme_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->acme_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->acme_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->acme_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        reason = "REFUSED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * ITEM-516: the management-gate request path.
 *
 * Both the panel page (`POST /panel/gate`) and the scriptable target
 * (`POST /api/v1/system/gate`) resolve to the same `PANEL_GATE` mutation.  The
 * GET of `/panel/gate` never reaches this handler: it falls through to the
 * panel, which renders the page.  The mutation can only ever close the gate -
 * an attempt to open is refused by name before anything is staged - and the
 * panel form is answered with a 303 back to its page while the API target is
 * answered with the one-line state record.
 */
static bool appliance_production_serving_handle_gate(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t bearer[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    const uint8_t *header = authorization;
    uint32_t header_length = authorization_length;
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;
    uint64_t confirmation_token = 0U;
    bool cancel = false;
    bool panel_target;
    char output[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 1U];
    char response[APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX + 64U];
    size_t output_length = 0U;
    uint32_t response_length = 0U;
    enum appliance_production_mutation_plane_status status;
    const char *reason = NULL;

    if (line == NULL || line->target == NULL ||
        policy->method != HTTP_METHOD_POST ||
        !appliance_production_serving_gate_target(line)) {
        return false;
    }
    panel_target = line->target_length == HTTP_POLICY_PANEL_GATE_TARGET_LENGTH;
    if (!serving->gate_executor_bound) {
        if (panel_target) {
            /* No system record is published: the panel answers its 404. */
            return false;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_503_SERVICE_UNAVAILABLE,
            line->target,
            line->target_length);
        return true;
    }
    {
        const uint8_t *content_type = NULL;
        uint32_t content_type_length = 0U;

        if (!appliance_production_serving_header_value(
                connection, "Content-Type", 12U,
                &content_type, &content_type_length) ||
            content_type_length !=
                APPLIANCE_PRODUCTION_SERVING_GATE_CONTENT_TYPE_LENGTH ||
            memcmp(content_type,
                   APPLIANCE_PRODUCTION_SERVING_GATE_CONTENT_TYPE,
                   APPLIANCE_PRODUCTION_SERVING_GATE_CONTENT_TYPE_LENGTH) !=
                0) {
            ++serving->rejected_count;
            ++serving->gate_mutation_refused_count;
            appliance_production_serving_encode(
                connection,
                HTTP_STATUS_400_BAD_REQUEST,
                HTTP_CONTENT_TYPE_JSON,
                (const uint8_t *)"{\"refused\":\"CONTENT_TYPE\"}\n",
                sizeof("{\"refused\":\"CONTENT_TYPE\"}\n") - 1U,
                false);
            return true;
        }
    }
    memset(bearer, 0, sizeof(bearer));
    if (header_length == 0U) {
        const uint8_t *cookie = NULL;
        uint32_t cookie_length = 0U;
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        bool present = false;

        memset(token, 0, sizeof(token));
        if (appliance_production_serving_header_value(
                connection, "Cookie", 6U, &cookie, &cookie_length) &&
            appliance_panel_login_cookie_token(
                cookie, cookie_length, token, &present) ==
                APPLIANCE_PANEL_LOGIN_OK &&
            present) {
            memcpy(bearer, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
                   APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
            memcpy(bearer + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                   token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
            header = bearer;
            header_length = (uint32_t)sizeof(bearer);
        }
        memset(token, 0, sizeof(token));
    }
    if (appliance_production_serving_form_field(
            body, body_length, "token", &value, &value_length) &&
        !appliance_production_serving_form_u64(
            value, value_length, &confirmation_token)) {
        confirmation_token = 0U;
    }
    cancel = appliance_production_serving_form_flag(
        body, body_length, "cancel");
    /*
     * The refusal word belongs to the request being made: a stale stage
     * refusal must not be quoted by a later commit failure.
     */
    serving->gate_refusal = NULL;
    memset(output, 0, sizeof(output));
    memset(response, 0, sizeof(response));
    status = appliance_production_mutation_plane_request(
        &serving->mutations,
        header_length != 0U ? header : NULL,
        header_length,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST,
        line->target,
        line->target_length,
        (const char *)body,
        body_length,
        confirmation_token,
        cancel,
        (uint64_t)hfoc_lwip_time_now(),
        output,
        sizeof(output),
        &output_length);
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED) {
        ++serving->gate_mutation_count;
        ++serving->panel_served_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED) {
        ++serving->denied_count;
        ++serving->unauthorized_count;
        ++serving->gate_mutation_refused_count;
    } else if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE) {
        ++serving->rejected_count;
        ++serving->gate_mutation_refused_count;
    } else if (status ==
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
        ++serving->gate_mutation_pending_count;
    } else if (status !=
                   APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        ++serving->rejected_count;
        ++serving->gate_mutation_refused_count;
    }
    if (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED ||
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED) {
        if (panel_target) {
            appliance_production_serving_encode_with_cookie(
                connection,
                HTTP_STATUS_303_SEE_OTHER,
                HTTP_CONTENT_TYPE_HTML_UTF8,
                NULL,
                0U,
                false,
                NULL,
                0U,
                (const uint8_t *)
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH,
                (uint32_t)sizeof(
                    APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH) -
                    1U);
            return true;
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"state\":\"");
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, output);
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"");
        if (status ==
            APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION) {
            (void)appliance_production_serving_text_append(
                response, sizeof(response), &response_length,
                ",\"token\":");
            (void)appliance_production_serving_text_append_u64(
                response, sizeof(response), &response_length,
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations));
        }
        (void)appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "}\n");
        appliance_production_serving_encode(
            connection,
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_JSON,
            (const uint8_t *)response,
            response_length,
            false);
        return true;
    }
    switch (status) {
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED:
        reason = authorization_length == 0U && header_length == 0U
            ? "AUTH_REQUIRED"
            : "AUTH_DENIED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE:
        reason = "UNAVAILABLE";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION:
        reason = "BAD_CONFIRMATION";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED:
        reason = "EXPIRED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED:
        /*
         * ITEM-515's rule: a stage refusal is a name, and the reply quotes it
         * instead of one generic word.  The gate's own name is
         * `GATE_LOCAL_SECRET_REQUIRED` when the panel tried to open it.
         */
        reason = serving->gate_refusal != NULL ? serving->gate_refusal
                                               : "REFUSED";
        break;
    case APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET:
        reason = "UNSUPPORTED_TARGET";
        break;
    default:
        reason = "BAD_STATE";
        break;
    }
    if (!appliance_production_serving_text_append(
            response, sizeof(response), &response_length,
            "{\"refused\":\"") ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, reason) ||
        !appliance_production_serving_text_append(
            response, sizeof(response), &response_length, "\"}\n")) {
        response_length = 0U;
    }
    appliance_production_serving_encode(
        connection,
        status == APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE
            ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
            : (status == APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED
                   ? (header_length == 0U ? HTTP_STATUS_401_UNAUTHORIZED
                                          : HTTP_STATUS_403_FORBIDDEN)
                   : HTTP_STATUS_400_BAD_REQUEST),
        HTTP_CONTENT_TYPE_JSON,
        (const uint8_t *)response,
        response_length,
        false);
    return true;
}

/*
 * The panel family is the authenticated management surface: `/panel` and the
 * twelve published section targets.  It is dispatched only after the ITEM-365
 * production request gate admits the request, and the panel layer then applies
 * its own session decision through the same composition, exactly as the
 * ITEM-384 contract requires.
 */
static bool appliance_production_serving_handle_panel(
    struct appliance_production_serving_connection *connection,
    const struct http_policy_result *policy,
    const struct http_request_line *line,
    const uint8_t *authorization,
    uint32_t authorization_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_management_panel_request panel_request;
    struct appliance_management_panel_response panel_response;
    struct appliance_management_web_ui_confirmation panel_confirmation;
    enum appliance_management_panel_target_kind target_kind;
    enum appliance_management_web_ui_section section =
        APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD;
    enum appliance_management_panel_status panel_status;
    const uint8_t *cookie = NULL;
    const uint8_t *content_type = NULL;
    uint32_t cookie_length = 0U;
    uint32_t content_type_length = 0U;

    target_kind = appliance_management_panel_target_kind(
        line->target, line->target_length, &section);
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_NONE) {
        return false;
    }
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
        policy->method == HTTP_METHOD_POST &&
        serving->panel_auth.active && !serving->panel_auth.result_ready &&
        !connection->panel_auth_pending) {
        /*
         * ITEM-619: one sign-in job at a time.  A second browser does not
         * share the KDF state of the first; it gets an honest retryable 503
         * until the running check has issued or refused its own session.
         */
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection, HTTP_STATUS_503_SERVICE_UNAVAILABLE, NULL, 0U);
        return true;
    }
    /*
     * ITEM-503: the BYO certificate form posts to the Security section.  The
     * mutation plane owns the two-phase state; the panel keeps rendering the
     * page, this path only stages, confirms or cancels.
     */
    if (appliance_production_serving_handle_certificate(
            connection, policy, line, authorization,
            authorization_length, body, body_length)) {
        return true;
    }
    /*
     * ITEM-445 step 2: a GET of `/` is not a panel request at all. The frozen
     * public sign-in document keeps being served by the fixed route table,
     * under its own route counters, exactly as before this item.
     */
    if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN &&
        policy->method != HTTP_METHOD_POST) {
        return false;
    }
    if (!serving->panel_published) {
        if (target_kind == APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN) {
            /* No panel owner is published: the public root document stays. */
            return false;
        }
        /* No panel owner is published: the target is an unknown route. */
        ++serving->not_found_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_404_NOT_FOUND,
            line->target,
            line->target_length);
        return true;
    }
    /*
     * ITEM-386: the panel is published, so the request is decided by the
     * panel's own authorization boundary.  Readiness is deliberately not
     * consulted here: appliance_management_panel_ready() also requires a
     * published administrator credential, so a readiness check turned every
     * panel target into 404 while no credential exists instead of the
     * ITEM-365 denial the panel itself renders (AUTH_UNAVAILABLE -> 503,
     * anonymous -> 401).  A published panel owns the panel targets and
     * reports the same denial the production request gate would.
     */
    /*
     * ITEM-445 step 2: the browser sign-in carries its material in the body
     * and the `Cookie` header; a section request carries its session in the
     * `Authorization` header or the cookie. Only what the request can use is
     * extracted, and a duplicated header is never resolved by guessing.
     */
    memset(&panel_request, 0, sizeof(panel_request));
    if (policy->method == HTTP_METHOD_POST) {
        (void)appliance_production_serving_header_value(
            connection, "Content-Type", 12U,
            &content_type, &content_type_length);
        panel_request.content_type = content_type;
        panel_request.content_type_length = content_type_length;
        panel_request.body = body;
        panel_request.body_length = body_length;
    }
    /*
     * ITEM-513: the `Cookie` header is read for every panel request, not only
     * for a GET without a bearer.  The language switch is a POST whose answer
     * sets the browser's `hfoc_lang` cookie, and the browser that clicks the
     * nav button carries only its session cookie - no `Authorization` header -
     * so the POST must be able to authenticate with it exactly like a GET.
     * The panel's own authorize still prefers the bearer header when both are
     * present, and a duplicated `Cookie` header is still refused by the shared
     * header reader rather than guessed at.
     */
    (void)appliance_production_serving_header_value(
        connection, "Cookie", 6U, &cookie, &cookie_length);
    panel_request.cookie_header = cookie;
    panel_request.cookie_header_length = cookie_length;
    memset(&panel_confirmation, 0, sizeof(panel_confirmation));
    if (serving->mutations_published) {
        /*
         * ITEM-505: the pending candidate the page shows belongs to whichever
         * mutation holds it.  The certificate import, the routing
         * configuration and the network configuration all hand their token to
         * a panel page, and each page names its own mutation; a pending
         * mutation with no page of its own is not shown here.
         */
        const enum appliance_management_authorization_mutation pending =
            appliance_production_mutation_plane_pending_mutation(
                &serving->mutations);

        if (appliance_management_authorization_mutation_has_panel_confirmation(
                pending)) {
            const uint64_t now_tick = (uint64_t)hfoc_lwip_time_now();
            const uint64_t pending_tick =
                serving->mutations.pending_tick;
            const uint64_t window = serving->mutations.confirmation_window;

            panel_confirmation.pending = true;
            panel_confirmation.token =
                appliance_production_mutation_plane_pending_token(
                    &serving->mutations);
            panel_confirmation.mutation =
                appliance_management_authorization_mutation_string(pending);
            panel_confirmation.remaining_ticks =
                now_tick > pending_tick && now_tick - pending_tick < window
                    ? window - (now_tick - pending_tick)
                    : 0U;
            panel_confirmation.single_use = true;
        }
    }
    /*
     * ITEM-504: the Routing page lists the applied table as it is when the
     * page is rendered, so the owner publishes the bounded copy first.
     * ITEM-505 publishes the network page's active and saved readings on the
     * same terms: what the owner just read, never a stored copy.
     */
    appliance_production_serving_publish_routes(serving);
    appliance_production_serving_publish_network(serving);
    appliance_production_serving_publish_system(serving);
    appliance_production_serving_publish_account(serving);
    appliance_production_serving_publish_certificates(serving);
    /*
     * ITEM-508 increment 34: the ACME control page follows the committed
     * record; the serving composition owns the rendering and republishes it on
     * every panel request and on every commit.
     */
    appliance_production_serving_publish_acme_control(serving);
    /* ITEM-516: the gate page follows the durable after-boot policy. */
    appliance_production_serving_publish_gate(serving);
    memset(&panel_response, 0, sizeof(panel_response));
    panel_status = appliance_management_panel_dispatch(
        &serving->panel,
        policy->method,
        line->target,
        line->target_length,
        authorization_length != 0U ? authorization : NULL,
        authorization_length,
        (uint64_t)hfoc_lwip_time_now(),
        panel_confirmation.pending ? &panel_confirmation : NULL,
        &panel_request,
        &panel_response);
    if (panel_status == APPLIANCE_MANAGEMENT_PANEL_PENDING) {
        /*
         * ITEM-619: the credential job is still running.  No response is
         * encoded; the poll resumes this connection when the job finishes.
         */
        connection->panel_auth_pending = true;
        serving->panel_auth.owner = connection;
        return true;
    }
    if (panel_status == APPLIANCE_MANAGEMENT_PANEL_NOT_PANEL) {
        return false;
    }
    if (panel_status == APPLIANCE_MANAGEMENT_PANEL_UNAUTHENTICATED ||
        panel_status == APPLIANCE_MANAGEMENT_PANEL_DENIED ||
        panel_status == APPLIANCE_MANAGEMENT_PANEL_THROTTLED ||
        panel_status == APPLIANCE_MANAGEMENT_PANEL_UNAVAILABLE) {
        ++serving->panel_denied_count;
    } else if (panel_status == APPLIANCE_MANAGEMENT_PANEL_ALLOWED) {
        ++serving->panel_served_count;
    }
    appliance_production_serving_encode_with_cookie(
        connection,
        panel_response.status,
        panel_response.content_type,
        panel_response.body,
        panel_response.body_length,
        policy->method == HTTP_METHOD_HEAD,
        panel_response.set_cookie,
        panel_response.set_cookie_length,
        panel_response.location,
        panel_response.location_length);
    return true;
}

/*
 * ITEM-504 increment 7: the name the request was addressed by, decided once.
 * `http_policy_check` has already refused a request without exactly one valid
 * `Host` header; this reads that value, hands it to the one owner of the
 * `Host` -> route decision and keeps the outcome.  A missing or duplicated
 * `Host` is refused here as well, so the relay never sees a name the parser
 * would not publish.
 */
static enum appliance_route_status
appliance_production_serving_resolve_route(
    struct appliance_production_serving_connection *connection)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    const struct appliance_route_table *table;
    const uint8_t *host = NULL;
    uint32_t host_length = 0U;

    if (connection->route_resolved) {
        return connection->route_status;
    }
    connection->route_resolved = true;
    connection->route_status = APPLIANCE_ROUTE_NULL;
    if (!appliance_production_serving_header_value(
            connection, "Host", 4U, &host, &host_length) ||
        host == NULL || host_length == 0U) {
        connection->route_status = APPLIANCE_ROUTE_BAD_NAME;
        return connection->route_status;
    }
    table = appliance_route_config_applied(&serving->route_config);
    connection->route_status = appliance_route_table_resolve_host(
        table, (const char *)host, host_length, &connection->route);
    return connection->route_status;
}

/* -------------------------------------------------------- ITEM-504 relay */

/*
 * ITEM-504 increment 8: the facade leads a request it was addressed by to the
 * backend of that name.  The relay is a byte pipe: the request the client
 * already wrote (raw head plus the bounded body) goes to the target, and the
 * target's answer goes back through the terminated TLS session.  Nothing here
 * decides a name - `appliance_production_serving_resolve_route()` decided it
 * once, before this code was reached - and nothing here invents a backend:
 * a refusal returns before the target is created.
 *
 * `tls_to_backend` is refused by the caller, not silently ignored: the frozen
 * profile has no TLS client, and a route that says `tls=on` must fail with a
 * named reason instead of pretending the hop is protected.
 */

static void appliance_production_serving_relay_forget_target(
    struct appliance_production_serving_connection *connection);

static void appliance_production_serving_relay_step(
    struct appliance_production_serving_connection *connection);

static void appliance_production_serving_relay_forget_target(
    struct appliance_production_serving_connection *connection)
{
    struct tcp_pcb *target = connection->target_pcb;

    connection->target_pcb = NULL;
    connection->target_connected = false;
    if (target != NULL) {
        tcp_arg(target, NULL);
        tcp_recv(target, NULL);
        tcp_err(target, NULL);
        tcp_poll(target, NULL, 0U);
        tcp_abort(target);
    }
}

/* True when every byte of the request reached the target's send queue. */
static bool appliance_production_serving_relay_request_forwarded(
    const struct appliance_production_serving_connection *connection)
{
    return connection->forward_head_offset >= connection->request_head_length &&
           connection->forward_body_offset >=
               (size_t)connection->body_length &&
           connection->to_target_offset >= connection->to_target_length;
}

static void appliance_production_serving_relay_drain_to_target(
    struct appliance_production_serving_connection *connection)
{
    struct tcp_pcb *target = connection->target_pcb;

    if (target == NULL || !connection->target_connected) {
        return;
    }
    while (connection->to_target_offset < connection->to_target_length) {
        u16_t room = tcp_sndbuf(target);
        u16_t chunk = (u16_t)(connection->to_target_length -
                              connection->to_target_offset);

        if (room == 0U) {
            break;
        }
        if (chunk > room) {
            chunk = room;
        }
        if (tcp_write(
                target,
                &connection->to_target[connection->to_target_offset],
                chunk,
                TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        connection->to_target_offset += chunk;
    }
    if (connection->to_target_offset >= connection->to_target_length) {
        connection->to_target_length = 0U;
        connection->to_target_offset = 0U;
    }
    (void)tcp_output(target);
}

/*
 * Stage the request the client wrote: the raw head first, then the bounded
 * body.  Staging stops at the buffer's edge and continues on the next call, so
 * a head larger than one buffer is never truncated and never duplicated.
 */
static void appliance_production_serving_relay_stage_request(
    struct appliance_production_serving_connection *connection)
{
    while (connection->to_target_length < sizeof(connection->to_target)) {
        const uint8_t *source;
        size_t remaining;
        size_t chunk;

        if (connection->forward_head_offset < connection->request_head_length) {
            source = &connection->request_raw[connection->forward_head_offset];
            remaining = connection->request_head_length -
                        connection->forward_head_offset;
        } else if (connection->forward_body_offset <
                   (size_t)connection->body_length) {
            source = &connection->body[connection->forward_body_offset];
            remaining = (size_t)connection->body_length -
                        connection->forward_body_offset;
        } else {
            connection->request_staged = true;
            return;
        }
        chunk = sizeof(connection->to_target) -
                connection->to_target_length;
        if (chunk > remaining) {
            chunk = remaining;
        }
        memcpy(&connection->to_target[connection->to_target_length], source,
               chunk);
        connection->to_target_length += chunk;
        if (connection->forward_head_offset < connection->request_head_length) {
            connection->forward_head_offset += chunk;
        } else {
            connection->forward_body_offset += chunk;
        }
    }
}

static void appliance_production_serving_relay_flush_to_client(
    struct appliance_production_serving_connection *connection)
{
    while (connection->to_client_offset < connection->to_client_length) {
        int result;

        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            appliance_production_serving_release(connection, false);
            return;
        }
        result = mbedtls_ssl_write(
            &connection->ssl,
            &connection->to_client[connection->to_client_offset],
            connection->to_client_length - connection->to_client_offset);
        appliance_production_serving_ssl_scope_exit();
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (result <= 0) {
            appliance_production_serving_release(connection, false);
            return;
        }
        connection->to_client_offset += (size_t)result;
    }
    connection->to_client_length = 0U;
    connection->to_client_offset = 0U;
}

static void appliance_production_serving_relay_settle(
    struct appliance_production_serving_connection *connection)
{
    struct tcp_pcb *target = connection->target_pcb;

    if (target == NULL) {
        return;
    }
    if (connection->client_eof && connection->target_connected &&
        !connection->target_shutdown &&
        appliance_production_serving_relay_request_forwarded(connection)) {
        connection->target_shutdown = true;
        (void)tcp_shutdown(target, 0, 1);
    }
    if (connection->target_eof &&
        connection->to_client_offset >= connection->to_client_length) {
        if (!connection->client_shutdown) {
            connection->client_shutdown = true;
            if (appliance_production_serving_ssl_scope_enter(connection)) {
                (void)mbedtls_ssl_close_notify(&connection->ssl);
                appliance_production_serving_ssl_scope_exit();
            }
        }
        appliance_production_serving_release(connection, true);
    }
}

static void appliance_production_serving_relay_step(
    struct appliance_production_serving_connection *connection)
{
    uint8_t plaintext[APPLIANCE_PRODUCTION_SERVING_PLAINTEXT_CHUNK];
    int result;

    if (!connection->active || connection->target_pcb == NULL) {
        return;
    }
    if (connection->target_connected && !connection->request_staged &&
        connection->to_target_length == 0U) {
        appliance_production_serving_relay_stage_request(connection);
    }
    appliance_production_serving_relay_drain_to_target(connection);
    appliance_production_serving_relay_flush_to_client(connection);
    if (!connection->active) {
        return;
    }
    /*
     * Read the next request bytes only when the request the client already
     * wrote is on its way: reading ahead would put bytes of a second request
     * behind the first one, and the pipe would answer in the wrong order.
     */
    if (connection->target_connected && connection->request_staged &&
        connection->to_target_offset >= connection->to_target_length &&
        !connection->client_eof) {
        memset(plaintext, 0, sizeof(plaintext));
        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            appliance_production_serving_release(connection, false);
            return;
        }
        result = mbedtls_ssl_read(
            &connection->ssl, plaintext, sizeof(plaintext));
        appliance_production_serving_ssl_scope_exit();
        if (result == 0 || result == MBEDTLS_ERR_SSL_CONN_EOF ||
            result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            connection->client_eof = true;
        } else if (result == MBEDTLS_ERR_SSL_WANT_READ ||
                   result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* The session has nothing to read yet. */
        } else if (result < 0) {
            appliance_production_serving_zero(plaintext, sizeof(plaintext));
            appliance_production_serving_release(connection, false);
            return;
        } else {
            memcpy(connection->to_target, plaintext, (size_t)result);
            connection->to_target_length = (size_t)result;
            connection->to_target_offset = 0U;
            connection->relay_last_progress_ms = hfoc_lwip_time_now();
            appliance_production_serving_relay_drain_to_target(connection);
        }
        appliance_production_serving_zero(plaintext, sizeof(plaintext));
    }
    appliance_production_serving_relay_settle(connection);
    if (connection->active &&
        (uint32_t)(hfoc_lwip_time_now() - connection->relay_last_progress_ms) >=
            APPLIANCE_PRODUCTION_SERVING_IDLE_TIMEOUT_MS) {
        appliance_production_serving_release(connection, false);
    }
}

static err_t appliance_production_serving_relay_connected(
    void *context,
    struct tcp_pcb *pcb,
    err_t error)
{
    struct appliance_production_serving_connection *connection = context;

    if (connection == NULL || !connection->active ||
        connection->target_pcb != pcb) {
        if (pcb != NULL && error == ERR_OK) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    if (error != ERR_OK) {
        /*
         * lwIP frees a failed connection before publishing the failure, so
         * the pcb is only an identity here and must not be aborted again.
         */
        connection->target_pcb = NULL;
        connection->target_connected = false;
        appliance_production_serving_release(connection, false);
        return ERR_ABRT;
    }
    connection->target_connected = true;
    connection->relay_last_progress_ms = hfoc_lwip_time_now();
    appliance_production_serving_relay_step(connection);
    return ERR_OK;
}

static err_t appliance_production_serving_relay_recv(
    void *context,
    struct tcp_pcb *pcb,
    struct pbuf *packet,
    err_t error)
{
    struct appliance_production_serving_connection *connection = context;
    size_t used;
    u16_t total;

    (void)error;
    if (connection == NULL || !connection->active ||
        connection->target_pcb != pcb) {
        if (packet != NULL) {
            pbuf_free(packet);
        }
        return ERR_OK;
    }
    if (packet == NULL) {
        connection->target_eof = true;
        appliance_production_serving_relay_settle(connection);
        return ERR_OK;
    }
    if (connection->to_client_offset >= connection->to_client_length) {
        connection->to_client_offset = 0U;
        connection->to_client_length = 0U;
    } else {
        /*
         * ITEM-504 increment 8: hand the staged answer to the TLS session
         * before deciding there is no room.  Bytes that are merely waiting
         * for their next flush must not refuse the segment behind them, or
         * the answer would advance one segment per lwIP retransmit timer
         * instead of streaming.  A refused write releases the connection,
         * and that released the target this callback belongs to.
         */
        appliance_production_serving_relay_flush_to_client(connection);
        if (!connection->active) {
            pbuf_free(packet);
            return ERR_ABRT;
        }
        if (connection->to_client_offset >= connection->to_client_length) {
            connection->to_client_offset = 0U;
            connection->to_client_length = 0U;
        }
    }
    used = connection->to_client_length;
    total = packet->tot_len;
    if ((size_t)total > sizeof(connection->to_client) - used) {
        /* No room: leave the pbuf unacknowledged, lwIP retries later. */
        return ERR_MEM;
    }
    if (pbuf_copy_partial(packet, &connection->to_client[used], total, 0U) !=
        total) {
        pbuf_free(packet);
        tcp_recved(pcb, total);
        return ERR_OK;
    }
    connection->to_client_length = used + (size_t)total;
    pbuf_free(packet);
    tcp_recved(pcb, total);
    connection->relay_last_progress_ms = hfoc_lwip_time_now();
    /*
     * The segment just staged is the newest bytes of the answer: give them to
     * the TLS session now, not at the next poll tick.
     */
    appliance_production_serving_relay_flush_to_client(connection);
    if (!connection->active) {
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void appliance_production_serving_relay_error(
    void *context,
    err_t error)
{
    struct appliance_production_serving_connection *connection = context;

    (void)error;
    if (connection == NULL) {
        return;
    }
    connection->target_pcb = NULL;
    connection->target_connected = false;
    appliance_production_serving_release(connection, false);
}

static err_t appliance_production_serving_relay_poll(
    void *context,
    struct tcp_pcb *pcb)
{
    struct appliance_production_serving_connection *connection = context;

    (void)pcb;
    if (connection != NULL && connection->active) {
        appliance_production_serving_relay_step(connection);
    }
    return ERR_OK;
}

/*
 * The end of the request head inside the captured bytes.  The parser refuses a
 * request without the final CRLF, so this finds it; an unfound terminator
 * falls back to the whole capture, which is then the whole request.
 */
static size_t appliance_production_serving_request_head_length(
    const struct appliance_production_serving_connection *connection)
{
    size_t index;

    for (index = 0U; index + 3U < connection->request_raw_length; ++index) {
        if (connection->request_raw[index] == (uint8_t)'\r' &&
            connection->request_raw[index + 1U] == (uint8_t)'\n' &&
            connection->request_raw[index + 2U] == (uint8_t)'\r' &&
            connection->request_raw[index + 3U] == (uint8_t)'\n') {
            return index + 4U;
        }
    }
    return connection->request_raw_length;
}

static bool appliance_production_serving_relay_start(
    struct appliance_production_serving_connection *connection)
{
    struct tcp_pcb *target;
    ip_addr_t address;

    if (connection->relay_started) {
        return true;
    }
    target = tcp_new();
    if (target == NULL) {
        return false;
    }
    connection->target_pcb = target;
    connection->target_connected = false;
    connection->target_eof = false;
    connection->client_eof = false;
    connection->target_shutdown = false;
    connection->client_shutdown = false;
    connection->request_staged = false;
    connection->to_target_length = 0U;
    connection->to_target_offset = 0U;
    connection->to_client_length = 0U;
    connection->to_client_offset = 0U;
    connection->forward_head_offset = 0U;
    connection->forward_body_offset = 0U;
    connection->request_head_length =
        appliance_production_serving_request_head_length(connection);
    connection->relay_last_progress_ms = hfoc_lwip_time_now();
    tcp_arg(target, connection);
    tcp_recv(target, appliance_production_serving_relay_recv);
    tcp_err(target, appliance_production_serving_relay_error);
    tcp_poll(target, appliance_production_serving_relay_poll, 1U);
    tcp_nagle_disable(target);
    IP4_ADDR(
        &address,
        connection->route.backend[0],
        connection->route.backend[1],
        connection->route.backend[2],
        connection->route.backend[3]);
    if (tcp_connect(
            target,
            &address,
            connection->route.backend_port,
            appliance_production_serving_relay_connected) != ERR_OK) {
        appliance_production_serving_relay_forget_target(connection);
        return false;
    }
    connection->relay_started = true;
    connection->facade = true;
    return true;
}

/*
 * The facade owns everything that is not a management target.  A path of the
 * panel or of the scriptable API is management even when it does not resolve
 * to a known route, so an unknown `/panel/...` keeps the management 404 and is
 * never led to a backend.
 */
static bool appliance_production_serving_target_management(
    const uint8_t *target,
    uint32_t length)
{
    return appliance_production_serving_target_equal(
               target, length, "/panel", 6U) ||
           (length > 6U && memcmp(target, "/panel/", 7U) == 0) ||
           (length >= 5U && memcmp(target, "/api/", 5U) == 0);
}

static void appliance_production_serving_handle_complete(
    struct appliance_production_serving_connection *connection,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct http_framing_result framing;
    struct http_policy_result policy;
    struct http_request_line line;
    struct https_handler_result handler;
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
    enum appliance_production_auth_status auth_status;
    enum http_parser_status parser_status;
    enum http_route_status route_status;
    enum http_route_id route_id = HTTP_ROUTE_NONE;
    enum http_status_code status = HTTP_STATUS_400_BAD_REQUEST;
    uint8_t authorization[
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
        APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t authorization_length = 0U;

    if (!connection->panel_auth_resuming) {
        ++serving->request_count;
    }
    connection->panel_auth_resuming = false;
    memset(&framing, 0, sizeof(framing));
    memset(&policy, 0, sizeof(policy));
    memset(&line, 0, sizeof(line));
    memset(&handler, 0, sizeof(handler));
    memset(&decision, 0, sizeof(decision));
    memset(authorization, 0, sizeof(authorization));

    /*
     * ITEM-385 reachability is decided before any parsing, routing or
     * authorization work, for the peer that completed the handshake.  A
     * refused peer is answered exactly like an unknown route, so it learns
     * nothing about the management plane.
     */
    if (!appliance_production_serving_reachability_admit(connection)) {
        (void)http_error_matrix_map_route(HTTP_ROUTE_NONE, &status);
        ++serving->not_found_count;
        appliance_production_serving_error_response(
            connection, status, NULL, 0U);
        return;
    }

    parser_status = http_framing_check(
        &serving->parser, &connection->parser_token, &framing);
    if (parser_status != HTTP_PARSER_OK) {
        appliance_production_serving_error_response(
            connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
        return;
    }
    if (framing.body_denied || framing.upgrade_denied) {
        if (http_error_matrix_map_framing(&framing, &status) !=
            HTTP_ERROR_MATRIX_OK) {
            status = HTTP_STATUS_400_BAD_REQUEST;
        }
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection, status, NULL, 0U);
        return;
    }

    parser_status = http_policy_check(
        &serving->parser, &connection->parser_token, &policy);
    if (parser_status != HTTP_PARSER_OK) {
        appliance_production_serving_error_response(
            connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
        return;
    }
    parser_status = http_request_line_get(
        &serving->parser, &connection->parser_token, &line);
    if (parser_status != HTTP_PARSER_OK || line.target == NULL) {
        appliance_production_serving_error_response(
            connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
        return;
    }
    if (!policy.allowed) {
        if (http_error_matrix_map_policy(policy.reason, &status) !=
            HTTP_ERROR_MATRIX_OK) {
            status = HTTP_STATUS_400_BAD_REQUEST;
        }
        if (policy.reason == HTTP_POLICY_METHOD) {
            ++serving->method_denied_count;
        } else {
            ++serving->rejected_count;
        }
        appliance_production_serving_error_response(
            connection,
            status,
            line.target,
            line.target_length);
        return;
    }
    if (!policy.method_allowed) {
        ++serving->method_denied_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            line.target,
            line.target_length);
        return;
    }

    /*
     * ITEM-504 increment 7: every completed request carries the name it was
     * addressed by.  The facade decides that name once, here, before the
     * management dispatch: the relay that follows uses this answer, and a
     * request that names no declared route is refused with the reason that
     * names it instead of being led to another backend.
     */
    (void)appliance_production_serving_resolve_route(connection);

    /*
     * Route and dispatch.  The three frozen public targets -- the rendered
     * panel sign-in document, the liveness target and the API discovery
     * target -- are served without a session; every other management target
     * resolves its route first and is then decided by the ITEM-365 production
     * request gate, which fails closed while no administrator credential is
     * published.  A target that resolves to no route is answered as an
     * unknown route without consulting the gate, so a peer cannot use the
     * status code to enumerate protected targets.
     */
    (void)appliance_production_serving_authorization_header(
        connection,
        authorization,
        sizeof(authorization),
        &authorization_length);
    {
        const bool public_target =
            appliance_production_serving_target_public(
                line.target, line.target_length);

        /*
         * ITEM-504: the route-configuration mutation.  It is checked before
         * the panel handler because the panel form posts to `/panel/routing`,
         * which is not one of the rendered sections; the scriptable target is
         * not under `/panel` at all.  The handler answers GET with `false`,
         * so the panel's own unknown-route response keeps being the one a
         * reader sees until the routing page exists.
         */
        if (appliance_production_serving_handle_routing(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-505: the network-configuration mutation, on the same terms.
         * The panel form posts to `/panel/network`, which the panel handler
         * would answer with its method refusal, so it is checked here first;
         * a GET falls through to the rendered page.
         */
        if (appliance_production_serving_handle_network(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-505 increment 3: the hostname mutation, on the same terms.
         * The panel form posts to `/panel/system`, which the panel handler
         * would answer with its method refusal, so it is checked here first;
         * a GET falls through to the rendered page.  The scriptable target is
         * `/api/v1/system` (POST only; its GET is the read-only system
         * status).
         */
        if (appliance_production_serving_handle_system(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-506 increment 6: the credential change, on the same terms.
         * The panel form posts to `/panel/account`, which the panel handler
         * would answer with its method refusal, so it is checked here first;
         * a GET falls through to the rendered page.  The scriptable target is
         * `/api/v1/system/account` (POST only; it has no read meaning).
         */
        if (appliance_production_serving_handle_account(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-507 increment 4: the name -> certificate/key mutation, on the
         * same terms.  The panel form posts to `/panel/certificates`, which
         * the panel handler would answer with its method refusal, so it is
         * checked here first; a GET falls through to the rendered page.  The
         * scriptable target is `/api/v1/system/certificate/name` (POST only;
         * it has no read meaning).
         */
        if (appliance_production_serving_handle_certificate_name(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-508 increment 34: the ACME control mutation, on the same terms.
         * The panel control page posts to `/panel/acme`, which the panel
         * handler would answer with its method refusal, so it is checked here
         * first; a GET falls through to the rendered control page.  The
         * scriptable target is `/api/v1/system/acme` (POST only; it has no
         * read meaning).
         */
        if (appliance_production_serving_handle_acme(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-516: the management-gate mutation, on the same terms.  The
         * panel page posts to `/panel/gate`, which the panel handler would
         * answer with its method refusal, so it is checked here first; a GET
         * falls through to the rendered page.  The scriptable target is
         * `/api/v1/system/gate` (POST only; it has no read meaning).
         */
        if (appliance_production_serving_handle_gate(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        if (appliance_production_serving_handle_panel(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        /*
         * ITEM-503: the scriptable certificate target.  It is not under
         * `/panel`, so the panel handler does not see it; the same mutation
         * plane request path serves it.
         */
        if (appliance_production_serving_handle_certificate(
                connection,
                &policy,
                &line,
                authorization,
                authorization_length,
                body,
                body_length)) {
            return;
        }

        if (appliance_production_serving_target_equal(
                line.target,
                line.target_length,
                APPLIANCE_MANAGEMENT_API_PREFIX,
                APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH) ||
            (line.target_length >
                 APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH &&
             memcmp(
                 line.target,
                 APPLIANCE_MANAGEMENT_API_PREFIX,
                 APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH) == 0 &&
             line.target[APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH] ==
                 (uint8_t)'/')) {
            if (!public_target) {
                /*
                 * ITEM-386: an unknown management API target is answered as an
                 * unknown route before the ITEM-365 gate is consulted, exactly
                 * as the fixed route table below already does.  The dispatcher
                 * owns the frozen unknown-route body, so asking it first keeps
                 * the documented admission order (dispatch, then authorization)
                 * and keeps a peer from telling a protected target from one
                 * that does not exist: while no administrator credential is
                 * published the gate reports UNAVAILABLE for every management
                 * target, and consulting it first turned the frozen
                 * unknown-route answer into 503 for /api/v1/... requests.
                 */
                struct appliance_management_api_result probe;
                enum appliance_management_api_status probe_status;

                memset(&probe, 0, sizeof(probe));
                probe_status = appliance_management_api_dispatch(
                    policy.method, line.target, line.target_length, &probe);
                if (probe_status == APPLIANCE_MANAGEMENT_API_NOT_FOUND &&
                    (serving->management_api == NULL ||
                     !appliance_production_management_api_owns(
                         serving->management_api,
                         line.target,
                         line.target_length))) {
                    ++serving->not_found_count;
                    appliance_production_serving_encode(
                        connection,
                        probe.response.status,
                        probe.response.content_type,
                        probe.response.body,
                        probe.response.body_length,
                        policy.method == HTTP_METHOD_HEAD);
                    return;
                }
                auth_status =
                    appliance_production_auth_authorize_request(
                        serving->auth,
                        authorization_length != 0U ? authorization : NULL,
                        authorization_length,
                        policy.method,
                        line.target,
                        line.target_length,
                        (uint64_t)hfoc_lwip_time_now(),
                        &decision,
                        &route);
                if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED) {
                    ++serving->denied_count;
                    ++serving->unauthorized_count;
                    status = auth_status ==
                            APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE
                        ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
                        : (decision.authenticated
                               ? HTTP_STATUS_403_FORBIDDEN
                               : HTTP_STATUS_401_UNAUTHORIZED);
                    appliance_production_serving_error_response(
                        connection,
                        status,
                        line.target,
                        line.target_length);
                    return;
                }
            }
            appliance_production_serving_handle_api(
                connection, &policy, &line);
            return;
        }

        route_status = http_route_match_line(&line, &route_id);
        if (route_status != HTTP_ROUTE_OK || route_id == HTTP_ROUTE_NONE) {
            /*
             * ITEM-504 increment 8: the target no fixed route claims is either
             * a management path that does not exist, which keeps the
             * management 404, or a request of the facade.  The facade uses the
             * name resolved before dispatch: a declared route without TLS to
             * the backend is relayed, a route that asks for TLS to the backend
             * is refused (the frozen profile has no TLS client), a name that
             * is not declared is a 404, and a name this table cannot carry is
             * a 400.  It never falls back to another backend.
             */
            if (!appliance_production_serving_target_management(
                    line.target, line.target_length)) {
                enum appliance_route_status name_status =
                    appliance_production_serving_resolve_route(connection);

                if (name_status == APPLIANCE_ROUTE_OK &&
                    !connection->route.tls_to_backend) {
                    if (appliance_production_serving_relay_start(connection)) {
                        return;
                    }
                    status = HTTP_STATUS_503_SERVICE_UNAVAILABLE;
                    ++serving->rejected_count;
                } else if (name_status == APPLIANCE_ROUTE_OK) {
                    status = HTTP_STATUS_503_SERVICE_UNAVAILABLE;
                    ++serving->rejected_count;
                } else if (name_status == APPLIANCE_ROUTE_BAD_NAME ||
                           name_status == APPLIANCE_ROUTE_NULL) {
                    status = HTTP_STATUS_400_BAD_REQUEST;
                    ++serving->rejected_count;
                } else {
                    status = HTTP_STATUS_404_NOT_FOUND;
                    ++serving->not_found_count;
                }
                appliance_production_serving_error_response(
                    connection,
                    status,
                    line.target,
                    line.target_length);
                return;
            }
            if (http_error_matrix_map_route(route_id, &status) !=
                HTTP_ERROR_MATRIX_OK) {
                status = HTTP_STATUS_404_NOT_FOUND;
            }
            ++serving->not_found_count;
            appliance_production_serving_error_response(
                connection,
                status,
                line.target,
                line.target_length);
            return;
        }
        if (!public_target) {
            auth_status = appliance_production_auth_authorize_request(
                serving->auth,
                authorization_length != 0U ? authorization : NULL,
                authorization_length,
                policy.method,
                line.target,
                line.target_length,
                (uint64_t)hfoc_lwip_time_now(),
                &decision,
                &route);
            if (auth_status != APPLIANCE_PRODUCTION_AUTH_ALLOWED) {
                ++serving->denied_count;
                ++serving->unauthorized_count;
                status = auth_status ==
                        APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE
                    ? HTTP_STATUS_503_SERVICE_UNAVAILABLE
                    : (decision.authenticated
                           ? HTTP_STATUS_403_FORBIDDEN
                           : HTTP_STATUS_401_UNAUTHORIZED);
                appliance_production_serving_error_response(
                    connection,
                    status,
                    line.target,
                    line.target_length);
                return;
            }
        }
    }

    /*
     * ITEM-503: the public certificate document is built from the active
     * certificate (a value the static handler table cannot carry) and served
     * before the fixed handler table, exactly like the other public targets.
     */
    if (route_id == HTTP_ROUTE_CERTIFICATE) {
        appliance_production_serving_certificate_response(
            connection, policy.method == HTTP_METHOD_HEAD);
        return;
    }
    if (https_handler_dispatch(
            &serving->parser, &connection->parser_token, &handler) !=
        HTTPS_HANDLER_OK) {
        ++serving->rejected_count;
        appliance_production_serving_error_response(
            connection,
            HTTP_STATUS_400_BAD_REQUEST,
            line.target,
            line.target_length);
        return;
    }
    if (handler.route == HTTP_ROUTE_ROOT) {
        ++serving->route_root_count;
    } else {
        ++serving->route_health_count;
    }
    appliance_production_serving_encode(
        connection,
        handler.response.status,
        handler.response.content_type,
        handler.response.body,
        handler.response.body_length,
        handler.method == HTTP_METHOD_HEAD);
}

static void appliance_production_serving_feed_request(
    struct appliance_production_serving_connection *connection,
    const uint8_t *bytes,
    size_t length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    size_t index;

    for (index = 0U; index < length; ++index) {
        enum http_parser_status status;
        bool complete = false;

        if (connection->request_parsed || connection->malformed) {
            return;
        }
        /*
         * ITEM-445 step 2: once the headers are complete and framing published
         * a body, the remaining bytes are that body until exactly
         * `body_expected` of them have arrived. The request is completed only
         * then, so a body byte is never parsed as a second request.
         */
        if (connection->body_pending) {
            if (connection->body_length >=
                sizeof(connection->body)) {
                connection->malformed = true;
                ++serving->rejected_count;
                appliance_production_serving_error_response(
                    connection,
                    HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
                    NULL,
                    0U);
                return;
            }
            connection->body[connection->body_length] = bytes[index];
            ++connection->body_length;
            if (connection->body_length == connection->body_expected) {
                connection->body_pending = false;
                connection->request_parsed = true;
                appliance_production_serving_handle_complete(
                    connection,
                    connection->body,
                    connection->body_length);
                return;
            }
            continue;
        }
        if (!connection->request_line_done) {
            status = http_request_line_feed(
                &serving->parser,
                &connection->parser_token,
                &bytes[index],
                1U,
                &complete);
            if (status != HTTP_PARSER_OK) {
                connection->malformed = true;
                ++serving->rejected_count;
                appliance_production_serving_error_response(
                    connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
                return;
            }
            if (!complete) {
                continue;
            }
            {
                struct http_parser_progress progress;

                if (http_parser_progress(
                        &serving->parser,
                        &connection->parser_token,
                        &progress) != HTTP_PARSER_OK) {
                    connection->malformed = true;
                    ++serving->rejected_count;
                    appliance_production_serving_error_response(
                        connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
                    return;
                }
                {
                    struct http_parser_progress next;

                    /*
                     * ITEM-386: the HEADERS state is entered from the
                     * request-line progress the parser just published, and
                     * http_parser_progress_shape_valid() requires the
                     * header_offset of that shape to start at the end of the
                     * request line (request_line_offset stays 0).  Writing the
                     * request-line count into request_line_offset instead left
                     * header_offset at 0, so the advance was rejected with
                     * HTTP_PARSER_INVALID_PROGRESS and every well-formed
                     * request was answered 400 after a successful handshake.
                     */
                    memset(&next, 0, sizeof(next));
                    next.request_line_count =
                        progress.request_line_count;
                    next.header_offset = progress.request_line_count;
                    next.total_count = progress.request_line_count;
                    if (http_parser_advance(
                            &serving->parser,
                            &connection->parser_token,
                            HTTP_PARSER_STATE_HEADERS,
                            &next) != HTTP_PARSER_OK) {
                        connection->malformed = true;
                        ++serving->rejected_count;
                        appliance_production_serving_error_response(
                            connection,
                            HTTP_STATUS_400_BAD_REQUEST,
                            NULL,
                            0U);
                        return;
                    }
                }
                connection->request_line_done = true;
            }
            continue;
        }
        status = http_header_feed(
            &serving->parser,
            &connection->parser_token,
            &bytes[index],
            1U,
            &complete);
        if (status != HTTP_PARSER_OK) {
            connection->malformed = true;
            ++serving->rejected_count;
            appliance_production_serving_error_response(
                connection, HTTP_STATUS_400_BAD_REQUEST, NULL, 0U);
            return;
        }
        if (complete) {
            struct http_framing_result framing;

            /*
             * ITEM-445 step 2: ask framing whether this request carries a
             * body before completing it. A POST within the bound stops here
             * and the loop collects the declared bytes; every other request
             * completes exactly as it did before this item, and a framing
             * denial still completes so the completion path answers it with
             * the frozen status.
             */
            memset(&framing, 0, sizeof(framing));
            if (http_framing_check(
                    &serving->parser,
                    &connection->parser_token,
                    &framing) == HTTP_PARSER_OK &&
                framing.body_present && framing.content_length != 0U) {
                connection->body_pending = true;
                connection->body_length = 0U;
                connection->body_expected = framing.content_length;
                continue;
            }
            connection->request_parsed = true;
            appliance_production_serving_handle_complete(
                connection, NULL, 0U);
            return;
        }
    }
}

static void appliance_production_serving_flush_response(
    struct appliance_production_serving_connection *connection)
{
    int result;

    if (!connection->response_ready || connection->response_sent) {
        return;
    }
    while (connection->response_offset < connection->response_length) {
        uint32_t remaining =
            connection->response_length - connection->response_offset;
        uint32_t chunk = remaining >
                APPLIANCE_PRODUCTION_SERVING_WRITE_CHUNK
            ? APPLIANCE_PRODUCTION_SERVING_WRITE_CHUNK
            : remaining;

        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            ++appliance_production_serving_owner.rejected_count;
            appliance_production_serving_release(connection, false);
            return;
        }
        result = mbedtls_ssl_write(
            &connection->ssl,
            &connection->response[connection->response_offset],
            chunk);
        appliance_production_serving_ssl_scope_exit();
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (result <= 0) {
            ++appliance_production_serving_owner.rejected_count;
            appliance_production_serving_release(connection, false);
            return;
        }
        connection->response_offset += (uint32_t)result;
    }
    connection->response_sent = true;
}

static void appliance_production_serving_close_connection(
    struct appliance_production_serving_connection *connection)
{
    int result;

    if (!connection->ssl_setup) {
        appliance_production_serving_release(connection, true);
        return;
    }
    if (!connection->close_notify_sent) {
        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            appliance_production_serving_release(connection, false);
            return;
        }
        result = mbedtls_ssl_close_notify(&connection->ssl);
        appliance_production_serving_ssl_scope_exit();
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        connection->close_notify_sent = true;
    }
    connection->closing = true;
    appliance_production_serving_release(connection, true);
}

/*
 * ITEM-650: advance the pending browser sign-in by one measured slice.
 *
 * The slice size is the one the previous slice's measured cost asked for.
 * The cycle clock is calibrated long before a panel login, but if it is not
 * ready the conversion answers 0 and the next slice keeps the proven
 * 64-round minimum rather than guessing a larger one.
 */
static uint32_t appliance_production_serving_clamp_us(uint64_t micros)
{
    return micros > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)micros;
}

static enum appliance_admin_credential_status
appliance_production_serving_advance_panel_auth(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    const uint32_t rounds = serving->panel_auth.slice_rounds;
    const uint64_t wait_started = timer_cycles();
    uint32_t wait_us = 0U;
    uint64_t started;
    uint64_t finished;
    uint32_t elapsed_us;
    enum appliance_admin_credential_status status;

    if (serving->panel_auth.slice_seen) {
        wait_us = appliance_production_serving_clamp_us(
            timer_cycles_to_us(
                wait_started - serving->panel_auth.last_slice_end_cycles));
    }
    started = timer_cycles();
    status = appliance_admin_credential_check_step(
        &serving->panel_auth.check, rounds);
    finished = timer_cycles();
    elapsed_us = appliance_production_serving_clamp_us(
        timer_cycles_to_us(finished - started));

    serving->panel_auth.slice_rounds =
        appliance_admin_credential_check_next_slice(rounds, elapsed_us);
    serving->panel_auth.last_slice_end_cycles = finished;
    serving->panel_auth.slice_seen = true;
    /*
     * ITEM-652: the production record now separates the measured derivation
     * from the wait for the next turn and keeps the job's own round totals.
     * It records scalars only; the checked secret never reaches the trace.
     */
    appliance_panel_auth_trace_slice(
        elapsed_us, wait_us,
        appliance_admin_credential_check_iterations_done(
            &serving->panel_auth.check),
        appliance_admin_credential_check_iterations_total(
            &serving->panel_auth.check));
    return status;
}

static void appliance_production_serving_step_connection(
    struct appliance_production_serving_connection *connection)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint8_t plaintext[APPLIANCE_PRODUCTION_SERVING_PLAINTEXT_CHUNK];
    uint32_t step;
    int result;

    if (!connection->active) {
        return;
    }
    if (connection->panel_auth_pending) {
        enum appliance_admin_credential_status credential_status;

        if (!serving->panel_auth.active) {
            appliance_production_serving_release(connection, false);
            return;
        }
        /*
         * ITEM-652: a job that finished on its first slice sets result_ready
         * immediately; do not run the finished check again just to count a
         * second, empty slice.
         */
        credential_status = serving->panel_auth.result_ready
            ? APPLIANCE_ADMIN_CREDENTIAL_OK
            : appliance_production_serving_advance_panel_auth();
        connection->last_activity_ms = hfoc_lwip_time_now();
        if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
            /*
             * ITEM-655: spend one bounded burst on the derivation instead of
             * exactly one slice per event-loop pass.  The cycle clock bounds
             * the burst to 100 ms and the slice count to four even if the
             * clock is not calibrated; the loop body then services network,
             * console and watchdog before the next burst.  A slice itself is
             * the ITEM-650 40 ms share, so a slow/TCG machine still yields
             * after one slice when that slice already exceeds the budget.
             */
            const uint64_t burst_started = timer_cycles();
            uint32_t burst_slices = 0U;

            while (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING &&
                   burst_slices <
                       APPLIANCE_PRODUCTION_SERVING_PANEL_AUTH_BURST_SLICES &&
                   timer_cycles_to_us(timer_cycles() - burst_started) <
                       APPLIANCE_PRODUCTION_SERVING_PANEL_AUTH_BURST_US) {
                credential_status =
                    appliance_production_serving_advance_panel_auth();
                connection->last_activity_ms = hfoc_lwip_time_now();
                ++burst_slices;
            }
        }
        if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
            /*
             * ITEM-619: publish bounded progress so the serial proof can see
             * the production job advancing while independent responses are
             * served.  The record carries round counts only.
             */
            appliance_panel_auth_trace_progress(
                hfoc_lwip_time_now(),
                appliance_admin_credential_check_iterations_done(
                    &serving->panel_auth.check),
                appliance_admin_credential_check_iterations_total(
                    &serving->panel_auth.check));
            return;
        }
        /*
         * ITEM-619: the job holds the proved result; re-dispatch the stored
         * request so the panel issues its one token/session and encodes the
         * same answer the blocking path did.  request_count is not counted a
         * second time.
         */
        serving->panel_auth.result_ready = true;
        connection->panel_auth_pending = false;
        connection->panel_auth_resuming = true;
        appliance_production_serving_handle_complete(
            connection, connection->body, connection->body_length);
        if (!connection->active) {
            return;
        }
        if (serving->panel_auth.result_ready) {
            /*
             * The resumed request never reached the credential adapter (for
             * example a framing refusal).  Drop the proved state instead of
             * letting the next sign-in inherit it.
             */
            appliance_admin_credential_check_cancel(
                &serving->panel_auth.check);
            serving->panel_auth.active = false;
            serving->panel_auth.result_ready = false;
            serving->panel_auth.owner = NULL;
        }
    }
    /*
     * ITEM-504 increment 8: once the facade owns the session, the byte pipe
     * replaces the HTTP exchange entirely; the connection never returns to
     * parsing after it.
     */
    if (connection->facade) {
        appliance_production_serving_relay_step(connection);
        return;
    }
    if (!connection->ssl_setup) {
        if (connection->rx_length == connection->rx_offset) {
            return;
        }
        ++serving->tls_attempts;
        if (!appliance_production_serving_setup_tls(connection)) {
            ++serving->handshake_failure_count;
            appliance_production_serving_release(connection, false);
            return;
        }
    }
    if (!connection->handshake_done) {
        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            ++serving->handshake_failure_count;
            appliance_production_serving_release(connection, false);
            return;
        }
        result = MBEDTLS_ERR_SSL_WANT_READ;
        for (step = 0U;
             step < APPLIANCE_PRODUCTION_SERVING_HANDSHAKE_STEP_LIMIT;
             ++step) {
            result = mbedtls_ssl_handshake(&connection->ssl);
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
        }
        appliance_production_serving_ssl_scope_exit();
        if (result == 0) {
            connection->handshake_done = true;
            ++serving->handshake_count;
            /*
             * A client that presented no name never reached the callback, so
             * the record says explicitly that the appliance's own identity was
             * served; a handshake the callback refused never gets here.
             */
            if (!connection->sni_present) {
                appliance_production_serving_sni_record(
                    serving, NULL, 0U, "DEFAULT", -1, NULL);
            }
            appliance_production_serving_measure_tls(&connection->ssl);
        } else if (result == MBEDTLS_ERR_SSL_WANT_READ ||
                   result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        } else {
            serving->tls_error_stage = 5U;
            serving->tls_error = result;
            ++serving->handshake_failure_count;
            appliance_production_serving_release(connection, false);
            return;
        }
    }
    if (!connection->request_parsed &&
        !connection->malformed &&
        !connection->response_ready) {
        memset(plaintext, 0, sizeof(plaintext));
        if (!appliance_production_serving_ssl_scope_enter(connection)) {
            appliance_production_serving_release(connection, false);
            return;
        }
        result = mbedtls_ssl_read(
            &connection->ssl, plaintext, sizeof(plaintext));
        appliance_production_serving_ssl_scope_exit();
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (result == 0 ||
            result == MBEDTLS_ERR_SSL_CONN_EOF ||
            result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            appliance_production_serving_release(connection, true);
            return;
        }
        if (result < 0) {
            ++serving->rejected_count;
            appliance_production_serving_release(connection, false);
            return;
        }
        /*
         * ITEM-504 increment 8: keep the bytes the client actually wrote.
         * The parser publishes fields, not bytes, and the backend of a facade
         * route must receive the request itself.  The capture stops as soon as
         * the head is complete (`body_pending`/`request_parsed`), so only the
         * bounded head plus at most one chunk lands here; the body is kept
         * once, in `connection->body`.
         */
        if (!connection->request_parsed && !connection->malformed &&
            !connection->body_pending) {
            const size_t available = (size_t)result;
            const size_t room =
                sizeof(connection->request_raw) -
                connection->request_raw_length;

            if (available > room) {
                connection->malformed = true;
                ++serving->rejected_count;
                appliance_production_serving_error_response(
                    connection,
                    HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
                    NULL,
                    0U);
            } else {
                memcpy(
                    &connection->request_raw[connection->request_raw_length],
                    plaintext,
                    available);
                connection->request_raw_length += available;
                appliance_production_serving_feed_request(
                    connection, plaintext, available);
            }
        } else {
            appliance_production_serving_feed_request(
                connection, plaintext, (size_t)result);
        }
        appliance_production_serving_zero(
            plaintext, sizeof(plaintext));
        if (!connection->active) {
            return;
        }
        if (connection->facade) {
            appliance_production_serving_relay_step(connection);
            return;
        }
    }
    appliance_production_serving_flush_response(connection);
    if (connection->response_sent) {
        appliance_production_serving_close_connection(connection);
    }
}

/* ---------------------------------------------------------------- lifecycle */

bool appliance_production_serving_listening(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    return serving->initialized &&
           serving->state == APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING &&
           hfoc_lwip_tcp_listening(&serving->listener);
}

enum appliance_production_serving_state
appliance_production_serving_state(void)
{
    return appliance_production_serving_owner.state;
}

const char *appliance_production_serving_reason(void)
{
    return appliance_production_serving_owner.reason != NULL
        ? appliance_production_serving_owner.reason
        : "none";
}

const char *appliance_production_serving_state_string(
    enum appliance_production_serving_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_SERVING_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK:
        return "AWAITING_NETWORK";
    case APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_ENTROPY:
        return "AWAITING_ENTROPY";
    case APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CERTIFICATE:
        return "AWAITING_CERTIFICATE";
    case APPLIANCE_PRODUCTION_SERVING_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING:
        return "LISTENING";
    case APPLIANCE_PRODUCTION_SERVING_STATE_FAILED:
        return "FAILED";
    case APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CREDENTIAL:
        return "AWAITING_CREDENTIAL";
    case APPLIANCE_PRODUCTION_SERVING_STATE_NONE:
    default:
        return "NONE";
    }
}

uint32_t appliance_production_serving_accept_count(void)
{
    return appliance_production_serving_owner.accept_count;
}

uint32_t appliance_production_serving_request_count(void)
{
    return appliance_production_serving_owner.request_count;
}

uint32_t appliance_production_serving_denied_count(void)
{
    return appliance_production_serving_owner.denied_count;
}

uint32_t appliance_production_serving_rejected_count(void)
{
    return appliance_production_serving_owner.rejected_count;
}

uint32_t appliance_production_serving_handshake_failure_count(void)
{
    return appliance_production_serving_owner.handshake_failure_count;
}

uint32_t appliance_production_serving_rx_callbacks(void)
{
    return appliance_production_serving_owner.rx_callbacks;
}

uint32_t appliance_production_serving_rx_bytes(void)
{
    return appliance_production_serving_owner.rx_bytes;
}

uint32_t appliance_production_serving_tls_attempts(void)
{
    return appliance_production_serving_owner.tls_attempts;
}

uint32_t appliance_production_serving_certificate_imports(void)
{
    return appliance_production_serving_owner.certificate_import_count;
}

bool appliance_production_serving_certificate_applies_on_reboot(void)
{
    return appliance_production_serving_owner.certificate_pending_reboot;
}

const struct appliance_management_reachability_selection *
appliance_production_serving_reachability(void)
{
    return appliance_production_serving_owner.reachability_published
        ? &appliance_production_serving_owner.reachability
        : NULL;
}

static bool appliance_production_serving_publish_tls_lifecycle(
    uint32_t now_epoch_seconds)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_management_tls *lifecycle =
        (struct appliance_management_tls *)serving->tls_lifecycle;
    struct appliance_management_tls_certificate certificate;
    struct appliance_management_tls_ingress ingress;
    enum appliance_management_tls_status status;
    bool ready = false;

    if (serving->tls_lifecycle == NULL ||
        serving->certificate == NULL) {
        return false;
    }
    if (now_epoch_seconds == 0U) {
        /*
         * The ITEM-375 lifecycle refuses an unvalidated wall clock by
         * contract, so ordinary boot does not push a certificate into it
         * while the clock is unvalidated.  The listener still serves the
         * same certificate: TLS does not consult the certificate's own
         * validity window, and the lifecycle record stays honest.
         */
        serving->reason = "certificate_awaits_validated_clock";
        return false;
    }
    /*
     * ITEM-404: an active validated certificate already published to the
     * lifecycle is the terminal state of this path.  The first certificate is
     * imported directly into the active slot, so there is no staged
     * predecessor to promote; a rotation staged by a management owner is that
     * owner's promotion, not this boot path's.
     */
    if (appliance_management_tls_certificate_ready(
            serving->tls_lifecycle, now_epoch_seconds)) {
        serving->tls_lifecycle_status = APPLIANCE_MANAGEMENT_TLS_OK;
        serving->tls_lifecycle_promoted = true;
        return true;
    }
    if (!serving->certificate->metadata_ready) {
        serving->reason = "certificate_metadata_unavailable";
        return false;
    }
    memset(&certificate, 0, sizeof(certificate));
    memset(&ingress, 0, sizeof(ingress));
    ingress.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    ingress.kind = APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL;
    certificate.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    certificate.present = true;
    certificate.key = APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256;
    certificate.common_name_length = (uint8_t)(
        serving->certificate->common_name_length >
                APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX
            ? APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX
            : serving->certificate->common_name_length);
    memcpy(
        certificate.common_name,
        serving->certificate->common_name,
        certificate.common_name_length);
    certificate.subject_alt_name_length =
        certificate.common_name_length;
    memcpy(
        certificate.subject_alt_name,
        serving->certificate->common_name,
        certificate.subject_alt_name_length);
    /*
     * ITEM-503: an imported certificate publishes the issuer its DER names;
     * the generated appliance certificate keeps the documented default.
     */
    if (serving->certificate->issuer_length != 0U) {
        appliance_production_serving_copy_string(
            certificate.issuer,
            sizeof(certificate.issuer),
            serving->certificate->issuer);
    } else {
        appliance_production_serving_copy_string(
            certificate.issuer,
            sizeof(certificate.issuer),
            "HFOC self-signed");
    }
    certificate.issuer_length = (uint8_t)strlen(certificate.issuer);
    /*
     * The lifecycle validates the certificate against its own validity window
     * and identifies it by the SHA-256 of the DER, so the metadata comes from
     * the certificate owner's parse of the served bytes.
     */
    certificate.not_before_epoch =
        serving->certificate->not_before_epoch;
    certificate.not_after_epoch =
        serving->certificate->not_after_epoch;
    memcpy(
        certificate.fingerprint,
        serving->certificate->fingerprint,
        sizeof(certificate.fingerprint));
    status = appliance_management_tls_import_certificate(
        lifecycle, NULL, &ingress, &certificate, now_epoch_seconds);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        serving->tls_lifecycle_status = (uint32_t)status;
        return false;
    }
    /*
     * A successful import leaves the lifecycle ACTIVE and ready under the same
     * validated clock, which is exactly the state the listener serves.  A
     * promotion would only apply to a staged rotation, which this boot path
     * never stages.
     */
    status = appliance_management_tls_readiness(
        lifecycle, now_epoch_seconds, &ready);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK || !ready) {
        serving->tls_lifecycle_status = APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
        return false;
    }
    serving->tls_lifecycle_status = APPLIANCE_MANAGEMENT_TLS_OK;
    serving->tls_lifecycle_promoted = true;
    return true;
}

/*
 * One bounded attempt to bring the management listener up.  Ordinary boot
 * calls it once and then once per event-loop turn while the path is still
 * awaiting the network, the entropy source or a certificate: the DHCP address,
 * the hardware entropy pool and the appliance certificate all arrive
 * asynchronously, so a single boot-time attempt would leave a healthy
 * appliance serving nothing at all.
 */
static void appliance_production_serving_apply_enrollment(void);
/* ITEM-505 increment 2: push a view published before the panel existed. */
static void appliance_production_serving_apply_view(void);

static enum appliance_production_serving_state
appliance_production_serving_attempt(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct hfoc_lwip_tcp_callback callback;
    struct network_ipv4_port_policy port_policy;
    struct network_ipv4_config config;
    enum hfoc_lwip_tcp_status listener_status;
    const struct network_ipv4_config *active;
    uint32_t now_ms;
    int result;

    if (serving->network == NULL || serving->auth == NULL) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
        serving->reason = "missing_composition";
        return serving->state;
    }
    /*
     * ITEM-506 increment 5: the account gates the management plane.  A machine
     * whose state volume is initialized but whose one account (login + PBKDF2
     * verifier) has not been set yet - an unattended install, the factory
     * placeholder, or a pre-ITEM-506 legacy fingerprint - withholds the
     * listener; the console conversation sets the account and the next poll
     * raises it.  A boot with no ready state volume keeps the documented
     * behaviour and serves with a per-boot certificate: there is no durable
     * store an account could live in, so there is nothing to gate on.
     *
     * The reason names which of the two the reader is looking at: the
     * enrollment owner is bound after `serving_init()` publishes the first
     * record, and an unbound owner says so instead of claiming the machine has
     * no account.
     */
    if (appliance_state_loader_ready(serving->loader) &&
        !appliance_production_admin_enrollment_serving_allowed(
            serving->admin_enrollment)) {
        serving->state =
            APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CREDENTIAL;
        serving->reason = serving->admin_enrollment == NULL
                              ? "admin_enrollment_unbound"
                              : "admin_credential_unset";
        return serving->state;
    }
    if (!hfoc_lwip_time_ready()) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK;
        serving->reason = "clock_unavailable";
        return serving->state;
    }
    now_ms = hfoc_lwip_time_now();
    if (!appliance_production_network_ready(serving->network)) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK;
        serving->reason = "network_not_ready";
        return serving->state;
    }
    active = hfoc_lwip_netif_config(&serving->network->netif);
    if (active == NULL || !hfoc_lwip_netif_live(&serving->network->netif)) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK;
        serving->reason = "netif_not_live";
        return serving->state;
    }
    /*
     * ITEM-416: a live interface with no address is the state a DHCP
     * acquisition starts from, and this plane used to borrow the QEMU fixture's
     * address to get past it.  Without the fixture the listener was initialised
     * against an all-zero configuration, `hfoc_lwip_tcp_init` refused it, and
     * the state became FAILED - which the poll never retries - so the panel
     * never came up on a machine that had simply not been given an address yet.
     * Waiting is what this state means: retry at the next poll.
     */
    if (!hfoc_lwip_netif_addressed(&serving->network->netif)) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_NETWORK;
        serving->reason = "address_not_assigned";
        return serving->state;
    }

    appliance_production_serving_publish_reachability();
    if (serving->certificate_upload_published) {
        (void)appliance_certificate_upload_set_clock(
            &serving->certificate_upload, serving->now_epoch_seconds);
    }
    (void)appliance_production_entropy_init(now_ms);
    if (!appliance_production_entropy_ready()) {
        serving->state =
            APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_ENTROPY;
        serving->reason = "no_hardware_entropy_source";
        return serving->state;
    }

    if (serving->certificate != NULL) {
        (void)appliance_production_certificate_prepare(
            serving->certificate,
            serving->loader,
            serving->now_epoch_seconds);
    }
    if (serving->certificate == NULL ||
        !appliance_production_certificate_available(
            serving->certificate)) {
        const char *status = serving->certificate != NULL
            ? appliance_production_certificate_status_string(
                  serving->certificate->last_status)
            : NULL;

        serving->state =
            APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CERTIFICATE;
        serving->reason = status != NULL ? status : "no_certificate";
        return serving->state;
    }

    if (!hfoc_mbedtls_alloc_ready()) {
        (void)hfoc_mbedtls_alloc_init();
    }
    if (!hfoc_lwip_alloc_ready()) {
        (void)hfoc_lwip_alloc_init();
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
        serving->reason = "psa_init_failed";
        return serving->state;
    }
    serving->crypto_published = true;

    if (!serving->tls_ready) {
        mbedtls_x509_crt_init(&serving->certificate_chain);
        mbedtls_pk_init(&serving->certificate_key);
        mbedtls_ssl_config_init(&serving->ssl_config);
        serving->certificate_published = true;
        serving->key_published = true;
        serving->ssl_config_published = true;

        result = mbedtls_x509_crt_parse_der(
            &serving->certificate_chain,
            serving->certificate->certificate_der,
            serving->certificate->certificate_length);
        if (result != 0) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "certificate_parse_failed";
            return serving->state;
        }
        /*
         * ITEM-503: an operator import may carry its issuing chain.  Each DER
         * certificate is appended to the same list after the leaf, which is
         * exactly the order the handshake sends; a bundle that does not walk
         * as a sequence of DER certificates refuses the whole configuration
         * instead of serving a half-chain.
         */
        if (serving->certificate->chain_length != 0U) {
            size_t chain_offset = 0U;
            uint32_t chain_count = 0U;

            while (chain_offset < serving->certificate->chain_length) {
                const size_t remaining =
                    serving->certificate->chain_length - chain_offset;
                const size_t certificate_bytes =
                    appliance_production_serving_der_sequence_length(
                        serving->certificate->chain_der + chain_offset,
                        remaining);

                if (certificate_bytes == 0U ||
                    certificate_bytes > remaining ||
                    chain_count >=
                        APPLIANCE_CERTIFICATE_IMPORT_CHAIN_MAX) {
                    serving->state =
                        APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
                    serving->reason = "certificate_chain_failed";
                    return serving->state;
                }
                result = mbedtls_x509_crt_parse_der(
                    &serving->certificate_chain,
                    serving->certificate->chain_der + chain_offset,
                    certificate_bytes);
                if (result != 0) {
                    serving->state =
                        APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
                    serving->reason = "certificate_chain_failed";
                    return serving->state;
                }
                chain_offset += certificate_bytes;
                ++chain_count;
            }
        }
        result = mbedtls_pk_parse_key(
            &serving->certificate_key,
            serving->certificate->key_der,
            serving->certificate->key_length,
            NULL,
            0U,
            appliance_production_entropy_random,
            NULL);
        if (result != 0) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "key_parse_failed";
            return serving->state;
        }
        result = mbedtls_ssl_config_defaults(
            &serving->ssl_config,
            MBEDTLS_SSL_IS_SERVER,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (result != 0) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "ssl_defaults_failed";
            return serving->state;
        }
        mbedtls_ssl_conf_rng(
            &serving->ssl_config,
            appliance_production_entropy_random,
            NULL);
        mbedtls_ssl_conf_min_tls_version(
            &serving->ssl_config, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(
            &serving->ssl_config, MBEDTLS_SSL_VERSION_TLS1_3);
        result = mbedtls_ssl_conf_own_cert(
            &serving->ssl_config,
            &serving->certificate_chain,
            &serving->certificate_key);
        if (result != 0) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "own_certificate_failed";
            return serving->state;
        }
        /*
         * ITEM-507 increment 3: the declared names and the callback that
         * selects their certificates.  The default own-certificate above is
         * what a client without SNI and a declared default both get.
         */
        appliance_production_serving_publish_name_table(serving);
        /*
         * ITEM-508 increment 34: the durable ACME control next to the
         * certificate record and the name table.  A missing or unreadable
         * record is the off state with a named source, never a guess.
         */
        appliance_production_serving_load_acme_control(serving);
        if (http_parser_init(&serving->parser) != HTTP_PARSER_OK) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "http_parser_init_failed";
            return serving->state;
        }
        if (hfoc_mbedtls_bio_init(&serving->bio) !=
            HFOC_MBEDTLS_BIO_OK) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "bio_init_failed";
            return serving->state;
        }
        serving->bio_policy.version = HFOC_MBEDTLS_BIO_VERSION;
        serving->bio_policy.max_send_bytes =
            HFOC_MBEDTLS_BIO_MAX_SEND_BYTES;
        serving->bio_policy.max_recv_bytes =
            HFOC_MBEDTLS_BIO_MAX_RECV_BYTES;
        serving->tls_ready = true;
    }

    if (appliance_management_panel_init(
            &serving->panel, serving->auth) ==
        APPLIANCE_MANAGEMENT_PANEL_OK) {
        serving->panel_published = true;
        /* ITEM-505 increment 2: the live view, if one was published before
         * the listener (and therefore the panel) existed. */
        appliance_production_serving_apply_view();
        appliance_production_serving_apply_enrollment();
    }
    (void)appliance_production_serving_publish_tls_lifecycle(
        serving->now_epoch_seconds);

    if (!serving->listener_published) {
        memset(&config, 0, sizeof(config));
        config = *active;
        memset(&port_policy, 0, sizeof(port_policy));
        port_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
        port_policy.port = APPLIANCE_PRODUCTION_SERVING_PORT;
        memset(&callback, 0, sizeof(callback));
        callback.accept = appliance_production_serving_accept;
        callback.context = serving;
        listener_status = hfoc_lwip_tcp_init(
            &serving->listener,
            &serving->network->netif,
            &config,
            &port_policy,
            &callback,
            APPLIANCE_PRODUCTION_SERVING_GENERATION);
        if (listener_status != HFOC_LWIP_TCP_OK) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "listener_init_failed";
            return serving->state;
        }
        serving->listener_callback = callback;
        serving->listener_published = true;
        listener_status = hfoc_lwip_tcp_bind(
            &serving->listener, &port_policy);
        if (listener_status != HFOC_LWIP_TCP_OK) {
            serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
            serving->reason = "listener_bind_failed";
            return serving->state;
        }
    }
    listener_status = hfoc_lwip_tcp_listen(&serving->listener);
    if (listener_status != HFOC_LWIP_TCP_OK &&
        listener_status != HFOC_LWIP_TCP_ALREADY_LISTENING) {
        serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
        serving->reason = "listener_listen_failed";
        return serving->state;
    }
    serving->state = APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING;
    serving->reason = "none";
    return serving->state;
}

enum appliance_production_serving_state appliance_production_serving_init(
    const struct appliance_production_network *network,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *plane,
    struct appliance_state_loader *loader,
    const struct appliance_management_tls *tls_lifecycle,
    struct appliance_production_certificate *certificate,
    uint32_t now_epoch_seconds)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (serving->initialized) {
        return serving->state;
    }
    memset(serving, 0, sizeof(*serving));
    serving->version = APPLIANCE_PRODUCTION_SERVING_VERSION;
    serving->network = network;
    serving->auth = auth;
    serving->plane = plane;
    serving->loader = loader;
    serving->certificate = certificate;
    serving->tls_lifecycle = tls_lifecycle;
    serving->now_epoch_seconds = now_epoch_seconds;
    serving->initialized = true;
    serving->state =
        APPLIANCE_PRODUCTION_SERVING_STATE_UNINITIALIZED;
    serving->reason = "uninitialized";
    /*
     * ITEM-503: publish the two-phase certificate import.  The upload
     * composition borrows the certificate owner (its store, its name and its
     * record) and the platform entropy source; the mutation plane borrows the
     * administrator gate and gets the upload composition as the certificate
     * executor.  No read-only management plane and no network transaction are
     * bound here, and the plane refuses exactly those mutations it cannot
     * execute instead of faking them.
     */
    if (auth != NULL && certificate != NULL &&
        appliance_certificate_upload_init(
            &serving->certificate_upload,
            certificate,
            appliance_production_entropy_random,
            NULL) == APPLIANCE_CERTIFICATE_UPLOAD_OK) {
        serving->certificate_upload_published = true;
        if (appliance_production_mutation_plane_init(
                &serving->mutations,
                auth,
                plane,
                NULL) == APPLIANCE_PRODUCTION_MUTATION_PLANE_OK) {
            struct appliance_production_mutation_plane_certificate_executor
                executor;

            memset(&executor, 0, sizeof(executor));
            executor.context = &serving->certificate_upload;
            executor.stage =
                appliance_production_serving_certificate_stage;
            executor.commit =
                appliance_production_serving_certificate_commit;
            executor.rollback =
                appliance_production_serving_certificate_rollback;
            serving->mutations_published =
                appliance_production_mutation_plane_bind_certificate(
                    &serving->mutations, &executor) ==
                APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            /*
             * ITEM-504: bind the routing-configuration executor on the same
             * plane.  The route configuration owns the applied table and the
             * candidate; the plane owns the token, the window and the audit.
             * The binding is independent of the certificate binding, and an
             * unbound executor is refused as an unsupported target.
             */
            appliance_route_config_init(&serving->route_config);
            /*
             * ITEM-504 increment 6: the boot reads the durable table before
             * anything can be shown or committed.  The store reports where the
             * table came from; a record that is missing, corrupt or of another
             * version yields the empty table, which refuses every name, and
             * the restore is refused whole rather than partially applied.
             */
            appliance_route_store_init(&serving->route_store);
            {
                struct appliance_route_table restored;

                (void)appliance_route_store_load(
                    &serving->route_store, serving->loader, &restored);
                serving->route_store_loaded = true;
                (void)appliance_route_config_restore(
                    &serving->route_config, &restored);
            }
            {
                struct appliance_production_mutation_plane_routing_executor
                    routing_executor;

                memset(&routing_executor, 0, sizeof(routing_executor));
                routing_executor.context = serving;
                routing_executor.stage =
                    appliance_production_serving_routing_stage;
                routing_executor.commit =
                    appliance_production_serving_routing_commit;
                routing_executor.rollback =
                    appliance_production_serving_routing_rollback;
                serving->routing_published =
                    appliance_production_mutation_plane_bind_routing(
                        &serving->mutations, &routing_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-505: bind the network-configuration executor on the same
             * plane.  The composition owns the candidate and the durable
             * network record; the plane owns the token, the window and the
             * audit, exactly as it does for the certificate and the routes.
             */
            {
                struct appliance_production_mutation_plane_network_executor
                    network_executor;

                memset(&network_executor, 0, sizeof(network_executor));
                network_executor.context = serving;
                network_executor.stage =
                    appliance_production_serving_network_stage;
                network_executor.commit =
                    appliance_production_serving_network_commit;
                network_executor.rollback =
                    appliance_production_serving_network_rollback;
                serving->network_published =
                    appliance_production_mutation_plane_bind_network_executor(
                        &serving->mutations, &network_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-505 increment 3: bind the system-hostname executor on the
             * same plane.  The composition owns the candidate and the durable
             * system record; the plane owns the token, the window and the
             * audit.  The identity the running system serves is captured here,
             * once, because the commit does not rebind it.
             */
            {
                struct appliance_production_mutation_plane_system_executor
                    system_executor;

                appliance_production_serving_system_capture_active(serving);
                memset(&system_executor, 0, sizeof(system_executor));
                system_executor.context = serving;
                system_executor.stage =
                    appliance_production_serving_system_stage;
                system_executor.commit =
                    appliance_production_serving_system_commit;
                system_executor.rollback =
                    appliance_production_serving_system_rollback;
                serving->system_published =
                    appliance_production_mutation_plane_bind_system(
                        &serving->mutations, &system_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-506 increment 6: bind the credential-change executor on
             * the same plane.  The enrollment owner is resolved at stage and
             * commit time because the console binding publishes it after this
             * module initialized; an unbound owner refuses the change.
             */
            {
                struct appliance_production_mutation_plane_account_executor
                    account_executor;

                memset(&account_executor, 0, sizeof(account_executor));
                account_executor.context = serving;
                account_executor.stage =
                    appliance_production_serving_account_stage;
                account_executor.commit =
                    appliance_production_serving_account_commit;
                account_executor.rollback =
                    appliance_production_serving_account_rollback;
                serving->account_published =
                    appliance_production_mutation_plane_bind_account(
                        &serving->mutations, &account_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-507 increment 4: bind the certificate-name executor on the
             * same plane.  The composition owns the saved table and the
             * candidate; the plane owns the token, the window and the audit.
             */
            {
                struct
                    appliance_production_mutation_plane_certificate_name_executor
                        certificate_name_executor;

                memset(&certificate_name_executor, 0,
                       sizeof(certificate_name_executor));
                certificate_name_executor.context = serving;
                certificate_name_executor.stage =
                    appliance_production_serving_certificate_name_stage;
                certificate_name_executor.commit =
                    appliance_production_serving_certificate_name_commit;
                certificate_name_executor.rollback =
                    appliance_production_serving_certificate_name_rollback;
                serving->certificate_name_published =
                    appliance_production_mutation_plane_bind_certificate_name(
                        &serving->mutations,
                        &certificate_name_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-508 increment 34: bind the ACME control executor on the
             * same plane.  The composition owns the durable record and the
             * candidate; the plane owns the token, the window and the audit.
             */
            {
                struct appliance_production_mutation_plane_acme_executor
                    acme_executor;

                memset(&acme_executor, 0, sizeof(acme_executor));
                acme_executor.context = serving;
                acme_executor.stage =
                    appliance_production_serving_acme_stage;
                acme_executor.commit =
                    appliance_production_serving_acme_commit;
                acme_executor.rollback =
                    appliance_production_serving_acme_rollback;
                serving->acme_executor_bound =
                    appliance_production_mutation_plane_bind_acme(
                        &serving->mutations, &acme_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
            /*
             * ITEM-516: bind the management-gate executor on the same plane.
             * The composition owns the durable record and the close-only
             * candidate; the plane owns the token, the window and the audit.
             */
            {
                struct appliance_production_mutation_plane_panel_gate_executor
                    gate_executor;

                memset(&gate_executor, 0, sizeof(gate_executor));
                gate_executor.context = serving;
                gate_executor.stage =
                    appliance_production_serving_gate_stage;
                gate_executor.commit =
                    appliance_production_serving_gate_commit;
                gate_executor.rollback =
                    appliance_production_serving_gate_rollback;
                serving->gate_executor_bound =
                    appliance_production_mutation_plane_bind_panel_gate(
                        &serving->mutations, &gate_executor) ==
                    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK;
            }
        }
    }
    appliance_production_serving_copy_string(
        serving->tls_version, sizeof(serving->tls_version), "TLS1.3");
    appliance_production_serving_copy_string(
        serving->tls_cipher, sizeof(serving->tls_cipher),
        "TLS_AES_128_GCM_SHA256");
    return appliance_production_serving_attempt();
}

/*
 * ITEM-400: bind the ordinary-boot read-only management snapshot owner.  The
 * owner is borrowed; the serving module resolves its four targets under the
 * same ITEM-365 request gate as the panel and the discovery target.
 */
enum appliance_production_serving_state
appliance_production_serving_publish_management_api(
    struct appliance_production_management_api *api)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (!serving->initialized || api == NULL) {
        return APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
    }
    serving->management_api = api;
    return serving->state;
}

/*
 * ITEM-505 increment 2: publish the live panel view.  The composition owns the
 * bytes every pointer names; this module keeps the borrowed struct and hands
 * it to the panel when the panel exists.  A publication that arrives before
 * the panel is initialized is kept - `appliance_production_serving_apply_view`
 * pushes it the moment the panel is published - so the first panel page of a
 * machine whose listener comes up on a later poll already shows live values.
 */
enum appliance_production_serving_state
appliance_production_serving_publish_panel_view(
    const struct appliance_management_web_ui_view *view)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (!serving->initialized) {
        return APPLIANCE_PRODUCTION_SERVING_STATE_UNINITIALIZED;
    }
    if (view == NULL) {
        memset(&serving->panel_view, 0, sizeof(serving->panel_view));
        serving->panel_view_published = false;
        if (serving->panel_published) {
            (void)appliance_management_panel_clear_view(&serving->panel);
        }
        return serving->state;
    }
    serving->panel_view = *view;
    serving->panel_view_published = true;
    if (serving->panel_published) {
        (void)appliance_management_panel_set_view(
            &serving->panel, &serving->panel_view);
    }
    return serving->state;
}

/*
 * ITEM-508 increment 32: publish the read-only ACME state block the
 * Certificates page shows.  The caller owns every byte the view names; this
 * module stores the borrowed copy and republishes the page, so the operator
 * reads the state that was just published.
 */
enum appliance_production_serving_state
appliance_production_serving_publish_acme(
    const struct appliance_management_web_ui_acme *acme)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    bool changed;

    if (!serving->initialized) {
        return APPLIANCE_PRODUCTION_SERVING_STATE_UNINITIALIZED;
    }
    changed = !serving->acme_published ||
        memcmp(&serving->acme, acme, sizeof(serving->acme)) != 0;
    if (acme == NULL) {
        memset(&serving->acme, 0, sizeof(serving->acme));
        serving->acme_published = false;
        changed = changed || serving->panel_published;
    } else {
        serving->acme = *acme;
        serving->acme_published = true;
    }
    if (changed && serving->panel_published) {
        appliance_production_serving_publish_certificates(serving);
    }
    return serving->state;
}

static void appliance_production_serving_apply_view(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (!serving->panel_published || !serving->panel_view_published) {
        return;
    }
    (void)appliance_management_panel_set_view(
        &serving->panel, &serving->panel_view);
}

/*
 * ITEM-445 step 2: bind the ITEM-401 administrator enrollment to the browser
 * sign-in form. The adapter is the exact local-console path: the enrollment
 * owns the verifier derivation, the persisted-fingerprint check, the
 * throttle, the token and the session, and the panel only turns its decision
 * into a page and a cookie.
 */
static enum appliance_management_panel_credential_status
appliance_production_serving_panel_credential_map(
    enum appliance_production_admin_enrollment_status status,
    enum appliance_panel_auth_trace_result *trace_result)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK:
        *trace_result = APPLIANCE_PANEL_AUTH_RESULT_OK;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK;
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED:
        *trace_result = APPLIANCE_PANEL_AUTH_RESULT_THROTTLED;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_THROTTLED;
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED:
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN:
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET:
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED:
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD:
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED:
        *trace_result = APPLIANCE_PANEL_AUTH_RESULT_DENIED;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_DENIED;
    default:
        *trace_result = APPLIANCE_PANEL_AUTH_RESULT_UNAVAILABLE;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_UNAVAILABLE;
    }
}

/*
 * ITEM-619: the browser sign-in credential check is a cooperative job.
 *
 * The first call performs the existing state, lockout and record refusals,
 * starts the same 200000-round PBKDF2 derivation and advances one bounded
 * slice.  While rounds remain the panel answers PENDING and encodes nothing;
 * the serving poll advances the job and re-dispatches the same request.  Only
 * the final call issues the token/session, so the production loop services
 * ARP/ICMP/SSH and the lwIP timers between slices without a reentrant network
 * pump inside the KDF.  Salt, iterations, constant-time comparisons, lockout
 * and the failure accounting are the existing ones.
 */
static enum appliance_management_panel_credential_status
appliance_production_serving_panel_authenticate(
    void *context,
    const uint8_t *user,
    uint32_t user_length,
    const uint8_t *secret,
    uint32_t secret_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_production_admin_enrollment *enrollment = context;
    enum appliance_production_admin_enrollment_status status;
    enum appliance_admin_credential_status credential_status;
    enum appliance_panel_auth_trace_result trace_result;

    if (enrollment == NULL || user == NULL || secret == NULL ||
        token_out == NULL || session_id_out == NULL) {
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_UNAVAILABLE;
    }
    /*
     * ITEM-619: the trace starts when the KDF job actually starts, after the
     * existing state/lockout/record reads.  Those reads are not the 200000
     * round derivation this proof bounds, and on the TCG proof they could
     * otherwise consume the first probe's whole two-second window before the
     * cooperative job had a slice to yield from.
     */
    if (serving->panel_auth.result_ready) {
        enum appliance_management_panel_credential_status answer;

        status = appliance_production_admin_enrollment_finish_credential_check(
            enrollment, &serving->panel_auth.check, now_tick, lifetime_ticks,
            token_out, session_id_out);
        serving->panel_auth.active = false;
        serving->panel_auth.result_ready = false;
        serving->panel_auth.owner = NULL;
        answer = appliance_production_serving_panel_credential_map(
            status, &trace_result);
        appliance_panel_auth_trace_commit(
            trace_result, appliance_production_serving_panel_auth_now_ms());
        return answer;
    }
    if (serving->panel_auth.active) {
        credential_status = appliance_production_serving_advance_panel_auth();
        if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
            return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING;
        }
        serving->panel_auth.result_ready = true;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING;
    }
    status = appliance_production_admin_enrollment_begin_credential_check(
        enrollment, (const char *)user, (size_t)user_length,
        (const char *)secret, (size_t)secret_length, now_tick,
        &serving->panel_auth.check);
    if (status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        serving->panel_auth.active = true;
        serving->panel_auth.slice_rounds =
            APPLIANCE_ADMIN_CREDENTIAL_CHECK_ROUNDS_DEFAULT;
        serving->panel_auth.slice_seen = false;
        serving->panel_auth.last_slice_end_cycles = 0U;
        appliance_panel_auth_trace_begin(
            appliance_production_serving_panel_auth_now_ms());
        credential_status = appliance_production_serving_advance_panel_auth();
        if (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING) {
            return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING;
        }
        serving->panel_auth.result_ready = true;
        return APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_PENDING;
    }
    {
        enum appliance_management_panel_credential_status answer =
            appliance_production_serving_panel_credential_map(
                status, &trace_result);

        appliance_panel_auth_trace_begin(
            appliance_production_serving_panel_auth_now_ms());
        appliance_panel_auth_trace_commit(
            trace_result, appliance_production_serving_panel_auth_now_ms());
        return answer;
    }
}

static void appliance_production_serving_apply_enrollment(void)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_management_panel_credential credential;

    if (!serving->panel_published ||
        serving->admin_enrollment == NULL) {
        return;
    }
    memset(&credential, 0, sizeof(credential));
    credential.context = serving->admin_enrollment;
    credential.authenticate =
        appliance_production_serving_panel_authenticate;
    if (appliance_management_panel_set_credential(
            &serving->panel, &credential) !=
        APPLIANCE_MANAGEMENT_PANEL_OK) {
        ++serving->rejected_count;
    }
}

enum appliance_production_serving_state
appliance_production_serving_bind_admin_enrollment(
    struct appliance_production_admin_enrollment *enrollment)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;

    if (!serving->initialized || enrollment == NULL) {
        return APPLIANCE_PRODUCTION_SERVING_STATE_FAILED;
    }
    serving->admin_enrollment = enrollment;
    appliance_production_serving_apply_enrollment();
    /*
     * ITEM-506 increment 5: the boot's first serving attempt runs inside
     * `serving_init()`, before this bind, so the state it left names an
     * unbound composition (`reason=admin_enrollment_unbound`).  Re-attempt
     * with the owner in place: the record the boot then publishes names the
     * machine's real account state (or moves on to the network, entropy and
     * certificate waits), never a composition artefact.
     */
    return appliance_production_serving_attempt();
}

void appliance_production_serving_poll(
    uint32_t now_ms,
    uint32_t now_epoch_seconds)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint32_t index;

    (void)now_ms;
    if (!serving->initialized) {
        return;
    }
    /*
     * ITEM-404: the ITEM-374 SNTP client validates the wall clock after the
     * listener may already be up, so the poll consumes the live reading.  A
     * validated reading is monotonic and never regresses; an unvalidated
     * reading (zero) never erases one that was already validated.
     */
    if (now_epoch_seconds != 0U &&
        (serving->now_epoch_seconds == 0U ||
         now_epoch_seconds > serving->now_epoch_seconds)) {
        serving->now_epoch_seconds = now_epoch_seconds;
    }
    if (serving->state != APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING) {
        /*
         * The DHCP address, the hardware entropy pool and the appliance
         * certificate all arrive asynchronously, so retry while the path is
         * honestly awaiting one of them and never after a hard failure.
         */
        if (serving->state != APPLIANCE_PRODUCTION_SERVING_STATE_FAILED) {
            (void)appliance_production_serving_attempt();
        }
        if (serving->state !=
            APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING) {
            return;
        }
    }
    /*
     * The listener is up and serving the certificate, but the validated clock
     * can arrive later: the lifecycle certificate is pushed whenever it does,
     * so the ITEM-375 record leaves AWAITING_CERTIFICATE on a machine whose
     * SNTP sample lands after the listener bound instead of staying there for
     * the whole boot.
     */
    if (!serving->tls_lifecycle_promoted &&
        serving->tls_lifecycle != NULL &&
        serving->certificate != NULL &&
        appliance_production_certificate_available(
            serving->certificate)) {
        (void)appliance_production_serving_publish_tls_lifecycle(
            serving->now_epoch_seconds);
    }
    if (serving->mutations_published) {
        const uint32_t epoch = serving->now_epoch_seconds;

        (void)appliance_production_mutation_plane_refresh(
            &serving->mutations);
        (void)appliance_certificate_upload_set_clock(
            &serving->certificate_upload, epoch);
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT;
         ++index) {
        struct appliance_production_serving_connection *connection =
            &serving->connections[index];

        if (connection->active) {
            appliance_production_serving_step_connection(connection);
        }
    }
    if (serving->plane != NULL) {
        (void)appliance_production_management_plane_refresh(
            serving->plane);
    }
}

bool appliance_production_serving_panel_auth_pending(void)
{
    const struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    uint32_t index;

    /*
     * ITEM-655: only a connection still waiting for the job can advance it.
     * The final `result_ready` call is short and is issued synchronously by
     * the same `step_connection()` pass, so `active && panel_auth_pending`
     * already covers it.  A released connection or a refused/cancelled job
     * answers false, so the loop parks again instead of spinning on a job
     * nobody owns.
     */
    if (!serving->initialized ||
        serving->state != APPLIANCE_PRODUCTION_SERVING_STATE_LISTENING) {
        return false;
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_SERVING_CONNECTION_COUNT;
         ++index) {
        const struct appliance_production_serving_connection *connection =
            &serving->connections[index];

        if (connection->active && connection->panel_auth_pending) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- rendering */

struct appliance_production_serving_writer {
    char *buffer;
    uint32_t capacity;
    uint32_t length;
};

static void appliance_production_serving_append(
    struct appliance_production_serving_writer *writer,
    const char *text)
{
    if (writer == NULL || text == NULL) {
        return;
    }
    while (*text != '\0' && writer->length + 1U < writer->capacity) {
        writer->buffer[writer->length++] = *text++;
    }
    writer->buffer[writer->length] = '\0';
}

static void appliance_production_serving_append_u32(
    struct appliance_production_serving_writer *writer,
    uint32_t value)
{
    char digits[12];
    uint32_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        char one[2];

        one[0] = digits[--count];
        one[1] = '\0';
        appliance_production_serving_append(writer, one);
    }
}

static void appliance_production_serving_append_i32(
    struct appliance_production_serving_writer *writer, int32_t value)
{
    if (value < 0) {
        appliance_production_serving_append(writer, "-");
        appliance_production_serving_append_u32(
            writer, (uint32_t)(-(int64_t)value));
    } else {
        appliance_production_serving_append_u32(writer, (uint32_t)value);
    }
}

static void appliance_production_serving_append_address(
    struct appliance_production_serving_writer *writer,
    const struct network_ipv4_config *config,
    uint16_t port)
{
    uint32_t index;

    if (config == NULL) {
        appliance_production_serving_append(writer, "unbound");
        return;
    }
    for (index = 0U; index < 4U; ++index) {
        char separator[2];

        appliance_production_serving_append_u32(
            writer, config->address.octets[index]);
        separator[0] = index == 3U ? ':' : '.';
        separator[1] = '\0';
        appliance_production_serving_append(writer, separator);
    }
    appliance_production_serving_append_u32(writer, port);
}

void appliance_production_serving_render(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_production_serving_writer writer;
    const struct network_ipv4_config *config;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return;
    }
    writer.buffer = output;
    writer.capacity = capacity;
    writer.length = 0U;
    output[0] = '\0';

    config = serving->network != NULL
        ? hfoc_lwip_netif_config(&serving->network->netif)
        : NULL;

    appliance_production_serving_append(&writer, "v=1 state=");
    appliance_production_serving_append(
        &writer,
        appliance_production_serving_state_string(serving->state));
    appliance_production_serving_append(&writer, " reason=");
    appliance_production_serving_append(
        &writer, appliance_production_serving_reason());
    appliance_production_serving_append(&writer, " listener=");
    appliance_production_serving_append(
        &writer,
        hfoc_lwip_tcp_listening(&serving->listener) ? "live" : "down");
    appliance_production_serving_append(&writer, " tls=");
    appliance_production_serving_append(&writer, serving->tls_version);
    appliance_production_serving_append(&writer, " cipher=");
    appliance_production_serving_append(&writer, serving->tls_cipher);
    appliance_production_serving_append(&writer, " measured=");
    appliance_production_serving_append(
        &writer, serving->tls_measured ? "handshake" : "profile");
    appliance_production_serving_append(&writer, " reachability=");
    appliance_production_serving_append(
        &writer,
        appliance_management_reachability_mode_string(
            serving->reachability.policy.mode));
    appliance_production_serving_append(&writer, " bind=");
    appliance_production_serving_append_address(
        &writer, config, APPLIANCE_PRODUCTION_SERVING_PORT);
    appliance_production_serving_append(&writer, " entropy=");
    appliance_production_serving_append(
        &writer, appliance_production_entropy_source_string());
    appliance_production_serving_append(&writer, " certificate=");
    if (serving->certificate == NULL ||
        !appliance_production_certificate_available(
            serving->certificate)) {
        appliance_production_serving_append(&writer, "unavailable");
        appliance_production_serving_append(&writer, ",error=");
        if (serving->certificate != NULL &&
            serving->certificate->last_result < 0) {
            appliance_production_serving_append(&writer, "-");
        }
        appliance_production_serving_append_u32(
            &writer,
            serving->certificate != NULL
                ? (uint32_t)(serving->certificate->last_result < 0
                      ? -serving->certificate->last_result
                      : serving->certificate->last_result)
                : 0U);
    } else {
        if (!serving->certificate->persisted) {
            appliance_production_serving_append(
                &writer, "ephemeral_self_signed");
        } else if (serving->certificate->acme_issued) {
            /*
             * ITEM-508 increment 23: a chain issued through ACME is not the
             * appliance's self-signed identity and not an operator upload;
             * the record names the provenance it actually has.
             */
            appliance_production_serving_append(
                &writer, "persisted_acme_issued");
        } else if (serving->certificate->imported) {
            appliance_production_serving_append(
                &writer, "persisted_operator");
        } else {
            appliance_production_serving_append(
                &writer, "persisted_self_signed");
        }
    }
    appliance_production_serving_append(&writer, " lifecycle=");
    appliance_production_serving_append(
        &writer,
        serving->tls_lifecycle != NULL
            ? appliance_management_tls_state_string(
                  appliance_management_tls_state(
                      serving->tls_lifecycle))
            : "unowned");
    appliance_production_serving_append(&writer, " panel=");
    appliance_production_serving_append(
        &writer, serving->panel_published ? "published" : "unpublished");
    appliance_production_serving_append(&writer, " counters=accept=");
    appliance_production_serving_append_u32(&writer, serving->accept_count);
    appliance_production_serving_append(&writer, ",request=");
    appliance_production_serving_append_u32(&writer, serving->request_count);
    appliance_production_serving_append(&writer, ",root=");
    appliance_production_serving_append_u32(&writer, serving->route_root_count);
    appliance_production_serving_append(&writer, ",health=");
    appliance_production_serving_append_u32(
        &writer, serving->route_health_count);
    appliance_production_serving_append(&writer, ",api=");
    appliance_production_serving_append_u32(&writer, serving->route_api_count);
    appliance_production_serving_append(&writer, ",not_found=");
    appliance_production_serving_append_u32(
        &writer, serving->not_found_count);
    appliance_production_serving_append(&writer, ",method_denied=");
    appliance_production_serving_append_u32(
        &writer, serving->method_denied_count);
    /*
     * ITEM-503: an operator import is durable immediately but the listener's
     * TLS configuration is built once per boot, so the record says both how
     * many imports this boot persisted and that they apply on the reboot.
     */
    appliance_production_serving_append(&writer, ",cert_imports=");
    appliance_production_serving_append_u32(
        &writer, serving->certificate_import_count);
    appliance_production_serving_append(&writer, ",cert_applies=");
    appliance_production_serving_append(
        &writer,
        serving->certificate_pending_reboot ? "reboot" : "none");
    appliance_production_serving_append(&writer, ",denied=");
    appliance_production_serving_append_u32(&writer, serving->denied_count);
    appliance_production_serving_append(&writer, ",reachability_denied=");
    appliance_production_serving_append_u32(
        &writer, serving->reachability_denied_count);
    appliance_production_serving_append(&writer, ",rejected=");
    appliance_production_serving_append_u32(&writer, serving->rejected_count);
    appliance_production_serving_append(&writer, ",handshake_failure=");
    appliance_production_serving_append_u32(
        &writer, serving->handshake_failure_count);
    appliance_production_serving_append(&writer, ",handshake=");
    appliance_production_serving_append_u32(&writer, serving->handshake_count);
    appliance_production_serving_append(&writer, ",rx_bytes=");
    appliance_production_serving_append_u32(&writer, serving->rx_bytes);
    appliance_production_serving_append(&writer, ",tls_attempts=");
    appliance_production_serving_append_u32(&writer, serving->tls_attempts);
    appliance_production_serving_append(&writer, ",tls_stage=");
    appliance_production_serving_append_u32(
        &writer, serving->tls_error_stage);
    appliance_production_serving_append(&writer, ",tls_error=");
    appliance_production_serving_append_i32(&writer, serving->tls_error);
    {
        const struct hfoc_mbedtls_alloc_snapshot arena =
            hfoc_mbedtls_alloc_snapshot();

        appliance_production_serving_append(&writer, ",tls_arena_used=");
        appliance_production_serving_append_u32(
            &writer, arena.total_used_bytes);
        appliance_production_serving_append(&writer, ",tls_arena_free=");
        appliance_production_serving_append_u32(
            &writer, arena.total_free_bytes);
    }
    appliance_production_serving_append(&writer, ",panel_served=");
    appliance_production_serving_append_u32(&writer, serving->panel_served_count);
    appliance_production_serving_append(&writer, ",panel_denied=");
    appliance_production_serving_append_u32(&writer, serving->panel_denied_count);
    *output_length = writer.length;
}

/*
 * ITEM-505 increment 2: the bounded panel line.  The durable record above is
 * what the journal keeps whole; this line is the short state reading the
 * panel's recent-log field shows, composed to fit the renderer's per-value
 * bound (`APPLIANCE_PRODUCTION_SERVING_PANEL_LINE_MAX`) whatever state the
 * path is in.  The state's reason is published by the panel's detail field, so
 * dropping it here loses nothing.
 */
void appliance_production_serving_render_panel_line(
    char *output,
    uint32_t capacity,
    uint32_t *output_length)
{
    struct appliance_production_serving *serving =
        &appliance_production_serving_owner;
    struct appliance_production_serving_writer writer;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return;
    }
    writer.buffer = output;
    writer.capacity = capacity;
    writer.length = 0U;
    output[0] = '\0';

    appliance_production_serving_append(&writer, "[PRODUCTION-SERVING] ");
    appliance_production_serving_append(&writer, "state=");
    appliance_production_serving_append(
        &writer,
        appliance_production_serving_state_string(serving->state));
    appliance_production_serving_append(&writer, " listener=");
    appliance_production_serving_append(
        &writer,
        hfoc_lwip_tcp_listening(&serving->listener) ? "live" : "down");
    appliance_production_serving_append(&writer, " panel=");
    appliance_production_serving_append(
        &writer, serving->panel_published ? "published" : "unpublished");
    *output_length = writer.length;
}

#else

typedef int appliance_production_serving_translation_unit_guard;

#endif
