/*
 * ITEM-508 increment 17: the ACME order flow over the proven transport.
 *
 * One boot, one order: when the production network has an active
 * configuration, this driver publishes the increment-13 binding with the
 * fixture anchor, imports the fixture account key, and drives one complete
 * ACME order through the increment-11 exchange driver - directory, account,
 * order, authorization, `http-01` challenge, finalize and the certificate
 * download - feeding every reply to `appliance_acme_flow`, which owns the
 * steps and the records.  The stub on the host is the fixed counterpart of
 * one fixed order (increment 17 endpoints); it verifies nothing.
 *
 * The driver decides nothing the modules below it already decide: the flow
 * builds every request and reads every reply, the transport owns the clock
 * windows and the bytes, the binding owns the connection and the handshake.
 * Its own decisions are the one URL family, the one fixture epoch (the proof
 * has no validated wall clock; ITEM-503's gate keeps the real clock honest),
 * the one declared name and the records the harness reads.
 *
 * The driver runs in the `ACME_ORDER_*` kernel family only; every other kernel
 * build compiles an empty object, and the harness asserts that
 * `[ACME-ORDER]` appears in neither the production kernel nor the production
 * ISO.
 */
#ifdef HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST

#include "appliance_acme_account.h"
#include "appliance_acme_fixture_clock_qemu.h"
#include "appliance_acme_flow.h"
#include "appliance_acme_jws.h"
#include "appliance_acme_renewal.h"
#include "appliance_acme_transport.h"
#include "appliance_acme_transport_lwip.h"
#include "appliance_acme_trust.h"
#include "appliance_network_wire.h"
#include "appliance_production_acme_http01.h"
#include "appliance_production_entropy.h"
#include "appliance_production_network_wire.h"
#include "appliance-acme-directory-fixtures.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The port is fixed on both sides of the proof: the guest dials the slirp
 * gateway at this port and the harness serves the stub on it.  It differs
 * from the increment-15 directory proof's port because both targets can run
 * concurrently in the parallel QEMU phase, and the stub binds the host port.
 */
#define APPLIANCE_ACME_ORDER_QEMU_PORT 18446U
#define APPLIANCE_ACME_ORDER_QEMU_DIRECTORY_URL \
    "https://10.0.2.2:18446/directory"
/*
 * ITEM-508 increment 17: the proof epoch is a fixture constant inside the
 * fixture chain's validity window; the flow refuses an unvalidated clock, and
 * a real SNTP-validated clock is ITEM-503's gate, not this proof's.
 *
 * ITEM-508 increment 31: the constant itself lives in the shared fixture clock
 * unit (`appliance_acme_fixture_epoch_seconds()`), because the HTTP-01 wire
 * proof needs the same nonzero instant and the listener's `poll` refuses a
 * zero clock by name.  This name is the driver's local spelling of that one
 * value; there is no second literal.
 */
#define APPLIANCE_ACME_ORDER_QEMU_EPOCH \
    appliance_acme_fixture_epoch_seconds()
/* The transport's own windows are 30 s and 60 s; this is only a backstop. */
#define APPLIANCE_ACME_ORDER_QEMU_DEADLINE_MS 240000U
#define APPLIANCE_ACME_ORDER_QEMU_DER_MAX 2048U
/*
 * ITEM-508 increment 20: the fixture certificate's lifetime in the proof.  The
 * verified chain feeds the renewal policy, which must say "not yet" on the
 * order's own instant and "due" once the 30-day window has opened.
 */
#define APPLIANCE_ACME_ORDER_QEMU_RENEWAL_SECONDS \
    (UINT32_C(90) * UINT32_C(24) * UINT32_C(3600))

static const char acme_order_identifier[] = "hfoc-appliance";

enum acme_order_stage {
    ACME_ORDER_STAGE_DIRECTORY = 0,
    ACME_ORDER_STAGE_ACCOUNT = 1,
    ACME_ORDER_STAGE_ORDER = 2,
    ACME_ORDER_STAGE_AUTHORIZATION = 3,
    ACME_ORDER_STAGE_CHALLENGE = 4,
    /* The CA answered `processing`; POST-as-GET to the same challenge URL. */
    ACME_ORDER_STAGE_CHALLENGE_POLL = 5,
    ACME_ORDER_STAGE_FINALIZE = 6,
    /* The order is still pending; POST-as-GET to the order URL. */
    ACME_ORDER_STAGE_FINALIZE_POLL = 7,
    ACME_ORDER_STAGE_CERTIFICATE = 8,
    ACME_ORDER_STAGE_DONE = 9,
};

static struct appliance_acme_transport_lwip acme_order_binding;
static struct appliance_acme_transport acme_order_transport;
static struct appliance_acme_flow acme_order_flow;
static struct appliance_acme_key acme_order_key;
/*
 * ITEM-508 increment 25: the identity view the account module reads.  The
 * fixture fills it with the same P-256 key the stub's canned chain is for, so
 * the proof keeps its counterpart while the binding itself goes through the
 * production call path.
 */
static struct appliance_production_certificate acme_order_identity;
static struct appliance_acme_trust acme_order_trust;
static struct appliance_acme_renewal acme_order_renewal;
static unsigned char acme_order_der[APPLIANCE_ACME_ORDER_QEMU_DER_MAX];
static enum acme_order_stage acme_order_stage_value;
static char acme_order_nonce[APPLIANCE_ACME_HTTP_NONCE_MAX + 1U];
static uint32_t acme_order_nonce_length;
static uint32_t acme_order_deadline_ms;
static bool acme_order_started;
static bool acme_order_finished;
static bool acme_order_exchange_active;
/*
 * ITEM-508 increment 22: the PKCS#10 the finalize carries.  It is written once
 * from the account key (which never leaves the JWS module) before the request
 * and its length is the proof's own record.
 */
static unsigned char acme_order_csr[APPLIANCE_ACME_CSR_MAX];
static size_t acme_order_csr_length;
/*
 * ITEM-508 increment 31: the production HTTP-01 listener the fixture arms from
 * the flow it is driving, and the two edges of that arming.  The listener is
 * armed once, at the authorization the flow has just parsed, and disarmed once
 * the listener really served the answer (`served_count`), so the host fetch is
 * by construction inside the armed window instead of racing a timer.
 */
static struct appliance_production_acme_http01 *acme_order_listener;
static bool acme_order_listener_armed;
static bool acme_order_listener_disarmed;

static void acme_order_failure(
    const char *message)
    __attribute__((noreturn));

static size_t acme_order_text_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

/*
 * One record is one line: the sequence pair keeps a concurrent writer from
 * splitting it, exactly as the other fixture drivers do.
 */
static void acme_order_failure(
    const char *message)
{
    const uint64_t serial_flags = serial_sequence_begin();

    /*
     * ITEM-508 increment 31: a failed order leaves no armed token behind.  The
     * disarm is the production call and stays silent here because the failure
     * record below is the verdict the harness reads.
     */
    if (acme_order_listener != NULL && acme_order_listener_armed &&
        !acme_order_listener_disarmed) {
        appliance_production_acme_http01_disarm(acme_order_listener);
        acme_order_listener_disarmed = true;
    }
    serial_write("[ACME-ORDER] failed: ");
    serial_write(message);
    serial_write(" step=");
    serial_write(appliance_acme_flow_step_string(acme_order_flow.last_step));
    serial_write(" status=");
    serial_write(appliance_acme_flow_status_string(acme_order_flow.last_status));
    serial_write(" transport=");
    serial_write(appliance_acme_transport_status_string(
        acme_order_transport.last_status));
    serial_write(" tls=");
    serial_write_hex((uint64_t)(uint32_t)
        appliance_acme_transport_lwip_last_tls_error(&acme_order_binding));
    serial_write("\n");
    serial_sequence_end(serial_flags);
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void acme_order_publish(
    const char *label)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[ACME-ORDER] ");
    serial_write(label);
    serial_write(" step=");
    serial_write(appliance_acme_flow_step_string(acme_order_flow.last_step));
    serial_write(" state=");
    serial_write(appliance_acme_flow_state_string(acme_order_flow.state));
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-508 increment 22: the CSR record is its own pair, because it is
 * published before the finalize request exists - a request that never reaches
 * the stub still leaves the written request's size and subject in the journal.
 */
static void acme_order_publish_csr(
    size_t bytes)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[ACME-ORDER] csr bytes=");
    serial_write_u64((uint64_t)bytes);
    serial_write(" subject=");
    serial_write(acme_order_identifier);
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-508 increment 25: the account record names the public RFC 7638
 * thumbprint the binding published - the account's stable name at the CA, and
 * never a key byte.  It is its own pair because it is published before the
 * account request exists.
 */
static void acme_order_publish_account(void)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[ACME-ORDER] account ");
    serial_write(appliance_acme_account_record());
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-508 increment 31: the listener's own record.  It names one edge of the
 * arming and nothing else: the token and the answer stay the flow's, and the
 * host side knows both from the stub and the published thumbprint.
 */
static void acme_order_publish_listener(
    const char *state)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[ACME-ORDER] challenge ");
    serial_write(state);
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * Arm the production listener from the flow the driver is really driving: the
 * token and the answer come from the parsed `http-01` challenge through the
 * increment-30 connector, never from a literal here.  The listener is not up
 * on the first turns of the boot, so an attempt before that is simply deferred;
 * a real refusal is a named failure, not a silently unarmed proof.
 */
static void acme_order_arm_listener(void)
{
    enum appliance_acme_challenge_status status;

    if (acme_order_listener == NULL || acme_order_listener_armed ||
        acme_order_listener_disarmed) {
        return;
    }
    if (!appliance_production_acme_http01_listening(acme_order_listener)) {
        return;
    }
    status = appliance_production_acme_http01_arm_flow(
        acme_order_listener, &acme_order_flow, &acme_order_key,
        appliance_acme_fixture_epoch_seconds());
    if (status != APPLIANCE_ACME_CHALLENGE_OK) {
        acme_order_failure("the listener refused the challenge from the flow");
    }
    acme_order_listener_armed = true;
    acme_order_publish_listener("armed=1");
}

/*
 * Disarm once the listener really served the armed answer.  A timer would race
 * the host fetch; the served counter cannot: the token stays armed exactly
 * until the answer left the wire, and the disarm then goes through the
 * production call.
 */
static void acme_order_disarm_served_listener(void)
{
    if (acme_order_listener == NULL || !acme_order_listener_armed ||
        acme_order_listener_disarmed) {
        return;
    }
    if (appliance_production_acme_http01_served_count(
            acme_order_listener) == 0U) {
        return;
    }
    appliance_production_acme_http01_disarm(acme_order_listener);
    acme_order_listener_disarmed = true;
    acme_order_publish_listener("armed=0 served=1");
}

static void acme_order_flow_check(
    enum appliance_acme_flow_status status,
    const char *reason)
{
    if (status != APPLIANCE_ACME_FLOW_OK) {
        acme_order_failure(reason);
    }
}

/*
 * Start one exchange over the one binding.  The transport is re-published for
 * every exchange because `close` detaches the link; the binding itself is
 * reused, and its `open` releases the previous exchange.
 */
static void acme_order_begin(
    const char *method,
    const char *url,
    const char *body,
    size_t body_length,
    uint32_t now_ms)
{
    enum appliance_acme_transport_status status;

    if (appliance_acme_transport_init(
            &acme_order_transport,
            appliance_acme_transport_lwip_link(&acme_order_binding)) !=
        APPLIANCE_ACME_TRANSPORT_OK) {
        acme_order_failure("the transport refused the binding");
    }
    status = appliance_acme_transport_begin(
        &acme_order_transport, method, acme_order_text_length(method), url,
        acme_order_text_length(url), "application/jose+json",
        acme_order_text_length("application/jose+json"), (const uint8_t *)body,
        body_length, now_ms);
    /*
     * `begin` returns PENDING for an accepted open: the connection and the
     * handshake belong to `advance`, and only a named refusal is a failure.
     */
    if (status != APPLIANCE_ACME_TRANSPORT_OK &&
        status != APPLIANCE_ACME_TRANSPORT_PENDING) {
        acme_order_failure(
            appliance_acme_transport_status_string(status));
    }
    acme_order_exchange_active = true;
}

static void acme_order_start_exchange(
    uint32_t now_ms)
{
    enum appliance_acme_flow_status status;

    if (acme_order_nonce_length == 0U && acme_order_stage_value !=
            ACME_ORDER_STAGE_DIRECTORY) {
        acme_order_failure("the previous reply carried no Replay-Nonce");
    }
    switch (acme_order_stage_value) {
    case ACME_ORDER_STAGE_DIRECTORY:
        acme_order_begin(
            "GET", APPLIANCE_ACME_ORDER_QEMU_DIRECTORY_URL, NULL, 0U, now_ms);
        return;
    case ACME_ORDER_STAGE_ACCOUNT:
        acme_order_publish_account();
        status = appliance_acme_flow_begin_account(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the account request was refused");
        acme_order_begin(
            "POST", acme_order_flow.directory.new_account.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_ORDER:
        status = appliance_acme_flow_begin_order(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the order request was refused");
        acme_order_begin(
            "POST", acme_order_flow.directory.new_order.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_AUTHORIZATION:
        status = appliance_acme_flow_begin_authorization(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the authorization request was refused");
        acme_order_begin(
            "POST", acme_order_flow.order.authorizations[0].text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_CHALLENGE:
        status = appliance_acme_flow_begin_challenge(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the challenge request was refused");
        acme_order_begin(
            "POST", acme_order_flow.authorization.http01.url.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_CHALLENGE_POLL:
        status = appliance_acme_flow_begin_challenge_poll(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the challenge poll was refused");
        acme_order_begin(
            "POST", acme_order_flow.authorization.http01.url.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_FINALIZE:
        /*
         * ITEM-508 increment 22: the request the CA validates is a PKCS#10,
         * written from the account key inside the JWS module.  The record
         * names the subject and the size before the request is sent, so a
         * finalize that never arrives still shows what was written.
         */
        status = appliance_acme_key_write_csr(
            &acme_order_key, acme_order_identifier,
            appliance_production_entropy_random, NULL, acme_order_csr,
            sizeof(acme_order_csr), &acme_order_csr_length);
        if (status != APPLIANCE_ACME_JWS_OK) {
            acme_order_failure("the CSR was refused");
        }
        acme_order_publish_csr(acme_order_csr_length);
        status = appliance_acme_flow_begin_finalize(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, acme_order_csr, acme_order_csr_length,
            appliance_production_entropy_random, NULL);
        acme_order_flow_check(status, "the finalize request was refused");
        acme_order_begin(
            "POST", acme_order_flow.order.finalize.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_FINALIZE_POLL:
        status = appliance_acme_flow_begin_order_poll(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the order poll was refused");
        acme_order_begin(
            "POST", acme_order_flow.order_url.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_CERTIFICATE:
        status = appliance_acme_flow_begin_certificate(
            &acme_order_flow, &acme_order_key, acme_order_nonce,
            acme_order_nonce_length, appliance_production_entropy_random,
            NULL);
        acme_order_flow_check(status, "the certificate request was refused");
        acme_order_begin(
            "POST", acme_order_flow.certificate_url.text,
            acme_order_flow.request, acme_order_flow.request_length, now_ms);
        return;
    case ACME_ORDER_STAGE_DONE:
        return;
    }
    acme_order_failure("the order stage is unknown");
}

static void acme_order_remember_nonce(
    const struct appliance_acme_http_response *response)
{
    if (!response->has_nonce || response->nonce_length == 0U ||
        response->nonce_length > APPLIANCE_ACME_HTTP_NONCE_MAX) {
        acme_order_nonce_length = 0U;
        return;
    }
    for (uint32_t index = 0U; index < response->nonce_length; ++index) {
        acme_order_nonce[index] = response->nonce[index];
    }
    acme_order_nonce[response->nonce_length] = '\0';
    acme_order_nonce_length = response->nonce_length;
}

static void acme_order_handle_response(
    const struct appliance_acme_http_response *response)
{
    enum appliance_acme_flow_status status = APPLIANCE_ACME_FLOW_OK;

    if (!appliance_acme_http_status_is_success(response->status_code)) {
        acme_order_failure("the ACME endpoint answered a non-success status");
    }
    acme_order_remember_nonce(response);
    switch (acme_order_stage_value) {
    case ACME_ORDER_STAGE_DIRECTORY: {
        const char *const declared_names[] = { acme_order_identifier };
        const size_t declared_lengths[] = {
            sizeof(acme_order_identifier) - 1U
        };

        status = appliance_acme_flow_begin(
            &acme_order_flow, (const char *)response->body,
            response->body_length, APPLIANCE_ACME_ORDER_QEMU_EPOCH);
        acme_order_flow_check(status, "the directory document was refused");
        status = appliance_acme_flow_declare_identifiers(
            &acme_order_flow, declared_names, declared_lengths, 1U);
        acme_order_flow_check(status, "the identifier was refused");
        acme_order_publish("directory");
        acme_order_stage_value = ACME_ORDER_STAGE_ACCOUNT;
        return;
    }
    case ACME_ORDER_STAGE_ACCOUNT:
        if (!response->has_location || response->location_length == 0U) {
            acme_order_failure("the account reply carried no Location");
        }
        status = appliance_acme_flow_account_result(
            &acme_order_flow, (const char *)response->body,
            response->body_length, response->location,
            response->location_length);
        acme_order_flow_check(status, "the account reply was refused");
        acme_order_publish("account");
        acme_order_stage_value = ACME_ORDER_STAGE_ORDER;
        return;
    case ACME_ORDER_STAGE_ORDER:
        if (!response->has_location || response->location_length == 0U) {
            acme_order_failure("the order reply carried no Location");
        }
        status = appliance_acme_flow_order_result(
            &acme_order_flow, (const char *)response->body,
            response->body_length, response->location,
            response->location_length);
        acme_order_flow_check(status, "the order reply was refused");
        acme_order_publish("order");
        acme_order_stage_value = ACME_ORDER_STAGE_AUTHORIZATION;
        return;
    case ACME_ORDER_STAGE_AUTHORIZATION:
        status = appliance_acme_flow_authorization_result(
            &acme_order_flow, (const char *)response->body,
            response->body_length);
        acme_order_flow_check(status, "the authorization reply was refused");
        acme_order_publish("authorization");
        /*
         * ITEM-508 increment 31: the authorization has just been parsed, so
         * the flow now carries the real `http-01` challenge - this is the
         * instant the production listener can be armed from it, before the
         * request that tells the CA to validate.
         */
        acme_order_arm_listener();
        acme_order_stage_value = ACME_ORDER_STAGE_CHALLENGE;
        return;
    case ACME_ORDER_STAGE_CHALLENGE:
    case ACME_ORDER_STAGE_CHALLENGE_POLL:
        status = appliance_acme_flow_challenge_result(
            &acme_order_flow, (const char *)response->body,
            response->body_length);
        acme_order_flow_check(status, "the challenge reply was refused");
        if (acme_order_flow.state == APPLIANCE_ACME_FLOW_STATE_READY) {
            acme_order_publish("challenge");
            acme_order_stage_value = ACME_ORDER_STAGE_FINALIZE;
            return;
        }
        if (acme_order_flow.state ==
                APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING &&
            acme_order_stage_value == ACME_ORDER_STAGE_CHALLENGE) {
            /*
             * The CA is still validating: poll the challenge URL with
             * POST-as-GET.  A second pending answer after the poll is not
             * retried again in this proof; the fixture stub answers `valid`
             * on it.
             */
            acme_order_publish("challenge processing");
            acme_order_stage_value = ACME_ORDER_STAGE_CHALLENGE_POLL;
            return;
        }
        acme_order_failure("the challenge is not valid yet");
    case ACME_ORDER_STAGE_FINALIZE:
    case ACME_ORDER_STAGE_FINALIZE_POLL:
        status = appliance_acme_flow_finalize_result(
            &acme_order_flow, (const char *)response->body,
            response->body_length);
        acme_order_flow_check(status, "the finalize reply was refused");
        if (acme_order_flow.state ==
            APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY) {
            acme_order_publish("finalize");
            acme_order_stage_value = ACME_ORDER_STAGE_CERTIFICATE;
            return;
        }
        if (acme_order_flow.state ==
                APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING &&
            acme_order_stage_value == ACME_ORDER_STAGE_FINALIZE) {
            /*
             * The CA is still issuing: poll the order URL with POST-as-GET.
             * A second pending answer after the poll is a named refusal, not
             * an endless loop; the fixture stub answers the poll with the
             * finalized order.
             */
            acme_order_publish("finalize processing");
            acme_order_stage_value = ACME_ORDER_STAGE_FINALIZE_POLL;
            return;
        }
        acme_order_failure("the finalized order is not valid yet");
    case ACME_ORDER_STAGE_CERTIFICATE: {
        size_t der_length = 0U;
        uint32_t members = 0U;
        enum appliance_acme_renewal_decision fresh_decision;
        enum appliance_acme_renewal_decision due_decision;
        enum appliance_acme_renewal_decision retry_decision;
        /*
         * The policy's record describes its latest evaluation only, so the due
         * state is copied before the attempt and the retry evaluation
         * overwrite it; the printed line never claims a state the policy is
         * no longer in.
         */
        char due_record[APPLIANCE_ACME_RENEWAL_RECORD_MAX];
        size_t due_record_length = 0U;
        const uint32_t expires_at =
            APPLIANCE_ACME_ORDER_QEMU_EPOCH +
            APPLIANCE_ACME_ORDER_QEMU_RENEWAL_SECONDS;
        const uint32_t due_instant =
            expires_at - APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS + 1U;

        status = appliance_acme_flow_certificate_result(
            &acme_order_flow, &acme_order_trust,
            (const char *)response->body, response->body_length,
            APPLIANCE_ACME_ORDER_QEMU_EPOCH, acme_order_der,
            sizeof(acme_order_der), &der_length, &members);
        acme_order_flow_check(status, "the certificate chain was refused");
        /*
         * ITEM-508 increment 20: the one call that moves the renewal expiry is
         * the verified certificate, and the policy must then say "not yet" at
         * the order's instant and "due" once its window has opened.
         */
        if (appliance_acme_renewal_certificate_obtained(
                &acme_order_renewal, APPLIANCE_ACME_ORDER_QEMU_EPOCH,
                expires_at) != APPLIANCE_ACME_RENEWAL_OK) {
            acme_order_failure("the renewal policy refused the certificate");
        }
        fresh_decision = appliance_acme_renewal_evaluate(
            &acme_order_renewal, APPLIANCE_ACME_ORDER_QEMU_EPOCH);
        if (fresh_decision != APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED) {
            acme_order_failure("the fresh certificate is not fresh");
        }
        due_decision = appliance_acme_renewal_evaluate(
            &acme_order_renewal, due_instant);
        if (due_decision != APPLIANCE_ACME_RENEWAL_DECISION_DUE) {
            acme_order_failure("the renewal window did not open");
        }
        {
            const char *record =
                appliance_acme_renewal_record(&acme_order_renewal);

            while (record[due_record_length] != '\0' &&
                   due_record_length + 1U < sizeof(due_record)) {
                due_record[due_record_length] = record[due_record_length];
                ++due_record_length;
            }
            due_record[due_record_length] = '\0';
        }
        /*
         * ITEM-508 increment 21: the failure path of the same policy.  One
         * attempt starts at the due instant, fails there, and the next
         * evaluation must sit in the retry window rather than start another.
         */
        if (appliance_acme_renewal_attempt_started(
                &acme_order_renewal,
                due_instant) != APPLIANCE_ACME_RENEWAL_OK) {
            acme_order_failure("the renewal attempt did not start");
        }
        if (appliance_acme_renewal_attempt_failed(
                &acme_order_renewal,
                due_instant) != APPLIANCE_ACME_RENEWAL_OK) {
            acme_order_failure("the renewal attempt did not finish");
        }
        retry_decision = appliance_acme_renewal_evaluate(
            &acme_order_renewal, due_instant);
        if (retry_decision != APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER) {
            acme_order_failure("the failed attempt is not in its retry window");
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[ACME-ORDER] renewal decision=");
            serial_write(appliance_acme_renewal_decision_string(
                fresh_decision));
            /*
             * The policy's own record describes its latest evaluation, so the
             * fresh line names the expiry instead of a record that the due
             * evaluation has already overwritten.
             */
            serial_write(" expires=");
            serial_write_u64((uint64_t)appliance_acme_renewal_expires_at(
                &acme_order_renewal));
            serial_write("\n[ACME-ORDER] renewal decision=");
            serial_write(appliance_acme_renewal_decision_string(
                due_decision));
            serial_write(" record=");
            serial_write(due_record);
            serial_write("\n[ACME-ORDER] renewal decision=");
            serial_write(appliance_acme_renewal_decision_string(
                retry_decision));
            serial_write(" attempts=");
            serial_write_u64((uint64_t)appliance_acme_renewal_attempts(
                &acme_order_renewal));
            serial_write(" record=");
            serial_write(appliance_acme_renewal_record(&acme_order_renewal));
            serial_write("\n[ACME-ORDER] order ok state=");
            serial_write(appliance_acme_flow_state_string(
                acme_order_flow.state));
            serial_write(" members=");
            serial_write_u64((uint64_t)members);
            serial_write(" der=");
            serial_write_u64((uint64_t)der_length);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        acme_order_finished = true;
        acme_order_stage_value = ACME_ORDER_STAGE_DONE;
        return;
    }
    case ACME_ORDER_STAGE_DONE:
        return;
    }
    acme_order_failure("the order stage is unknown");
}

static void acme_order_start(
    uint32_t now_ms)
{
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    const struct appliance_network_lifecycle *lifecycle =
        wire != NULL ? appliance_network_wire_lifecycle(wire) : NULL;

    /*
     * The order starts only on a network that is actually configured, and
     * only on a clock the transport will accept: an unset clock is a refusal
     * by contract, not something to retry forever.
     */
    if (active == NULL || lifecycle == NULL || now_ms == 0U) {
        return;
    }
    /*
     * ITEM-508 increment 25: the account key is bound through the production
     * binding point from the identity view the certificate owner publishes.
     * The fixture identity carries the same P-256 key the stub's chain is for,
     * so the account the CA sees is the one this proof names.
     */
    memset(&acme_order_identity, 0, sizeof(acme_order_identity));
    acme_order_identity.initialized = true;
    acme_order_identity.available = true;
    acme_order_identity.key_length = sizeof(directory_fixture_leaf_key);
    memcpy(acme_order_identity.key_der, directory_fixture_leaf_key,
           sizeof(directory_fixture_leaf_key));
    if (appliance_acme_account_bind(&acme_order_identity, &acme_order_key) !=
        APPLIANCE_ACME_ACCOUNT_OK) {
        acme_order_failure("the fixture identity account key was refused");
    }
    if (appliance_acme_trust_init(
            &acme_order_trust, appliance_acme_trust_anchor_der_data(),
            appliance_acme_trust_anchor_der_length()) !=
        APPLIANCE_ACME_TRUST_OK) {
        acme_order_failure("the trust anchor was refused");
    }
    if (appliance_acme_transport_lwip_init(
            &acme_order_binding,
            (struct appliance_network_lifecycle *)lifecycle,
            appliance_acme_trust_anchor_der_data(),
            appliance_acme_trust_anchor_der_length()) !=
        APPLIANCE_ACME_TRANSPORT_LWIP_OK) {
        acme_order_failure("the binding refused the fixture anchor");
    }
    if (!appliance_acme_transport_lwip_published(&acme_order_binding)) {
        acme_order_failure("the binding published nothing");
    }
    acme_order_stage_value = ACME_ORDER_STAGE_DIRECTORY;
    acme_order_nonce_length = 0U;
    appliance_acme_renewal_init(&acme_order_renewal);
    acme_order_deadline_ms = now_ms + APPLIANCE_ACME_ORDER_QEMU_DEADLINE_MS;
    acme_order_started = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[ACME-ORDER] proof init port=");
        serial_write_u64((uint64_t)APPLIANCE_ACME_ORDER_QEMU_PORT);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

void appliance_acme_order_qemu_test_tick(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_ms)
{
    enum appliance_acme_transport_status status;
    struct appliance_acme_http_response response;

    /*
     * ITEM-508 increment 31: the listener is owned by the production loop; the
     * fixture only borrows it to arm from the flow and to observe the served
     * counter.  The disarm is checked on every turn, including after the order
     * finished, because the host fetches the answer after the order's own
     * records are complete.
     */
    if (acme_order_listener == NULL) {
        acme_order_listener = http01;
    }
    acme_order_disarm_served_listener();
    if (acme_order_finished) {
        return;
    }
    if (!acme_order_started) {
        acme_order_start(now_ms);
        return;
    }
    if (now_ms == 0U) {
        return;
    }
    if (!acme_order_exchange_active) {
        acme_order_start_exchange(now_ms);
        return;
    }
    status = appliance_acme_transport_advance(&acme_order_transport, now_ms);
    if (status == APPLIANCE_ACME_TRANSPORT_PENDING) {
        if (now_ms > acme_order_deadline_ms) {
            acme_order_failure("the order outlived its backstop");
        }
        return;
    }
    if (status != APPLIANCE_ACME_TRANSPORT_OK) {
        acme_order_failure(
            appliance_acme_transport_status_string(status));
    }
    if (appliance_acme_transport_take_response(
            &acme_order_transport, &response) !=
        APPLIANCE_ACME_TRANSPORT_OK) {
        acme_order_failure("the completed exchange has no reply");
    }
    acme_order_exchange_active = false;
    acme_order_handle_response(&response);
    appliance_acme_transport_close(&acme_order_transport);
}

#endif /* HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST */

/*
 * Every kernel build compiles this wildcard source, and only the order family
 * defines the test macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_acme_order_qemu_translation_unit_guard;
