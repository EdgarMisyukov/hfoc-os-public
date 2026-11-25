/*
 * ITEM-508 increment 15: the directory exchange proof driver.
 *
 * One boot, one exchange: when the production network has an active
 * configuration, this driver publishes the increment-13 binding with the
 * fixture anchor, starts `GET https://10.0.2.2:<port>/directory` through the
 * increment-11 transport, and drives it from the kernel main loop until the
 * reply is complete or a named refusal is published.  It decides nothing the
 * two modules below it already decide; it only owns the one URL, the one
 * expected document and the record the harness reads.
 *
 * The driver runs in the `ACME_DIRECTORY_*` kernel family only; every other
 * kernel build compiles an empty object, and the production ISO marker checks
 * assert that `[ACME-DIRECTORY]` appears in neither the production kernel nor
 * the production ISO.
 */
#ifdef HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST

#include "appliance_acme_transport.h"
#include "appliance_acme_transport_lwip.h"
#include "appliance_acme_trust.h"
#include "appliance_network_wire.h"
#include "appliance_production_network_wire.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The port is fixed on both sides of the proof: the guest dials the slirp
 * gateway at this port and the harness serves the stub on it, so a mismatch
 * cannot quietly pass.  The harness asserts the host port is free before the
 * boot.
 */
#define APPLIANCE_ACME_DIRECTORY_QEMU_PORT 18445U
#define APPLIANCE_ACME_DIRECTORY_QEMU_URL \
    "https://10.0.2.2:18445/directory"
#define APPLIANCE_ACME_DIRECTORY_QEMU_BODY \
    "{\"newNonce\":\"https://10.0.2.2:18445/acme/new-nonce\"," \
    "\"newAccount\":\"https://10.0.2.2:18445/acme/new-account\"," \
    "\"newOrder\":\"https://10.0.2.2:18445/acme/new-order\"}"
/* The transport's own windows are 30 s and 60 s; this is only a backstop. */
#define APPLIANCE_ACME_DIRECTORY_QEMU_DEADLINE_MS 180000U

static struct appliance_acme_transport_lwip acme_directory_binding;
static struct appliance_acme_transport acme_directory_transport;
static bool acme_directory_started;
static bool acme_directory_finished;
static uint32_t acme_directory_deadline_ms;

static void acme_directory_failure(
    const char *message)
    __attribute__((noreturn));

static void acme_directory_failure(
    const char *message)
{
    /*
     * One record is one line: the sequence pair keeps a concurrent writer
     * from splitting it, exactly as the other fixture drivers do.
     */
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[ACME-DIRECTORY] failed: ");
    serial_write(message);
    serial_write(" binding=");
    serial_write(appliance_acme_transport_lwip_status_string(
        appliance_acme_transport_lwip_last_status(&acme_directory_binding)));
    serial_write(" tls=");
    serial_write_hex((uint64_t)(uint32_t)
        appliance_acme_transport_lwip_last_tls_error(
            &acme_directory_binding));
    serial_write("\n");
    serial_sequence_end(serial_flags);
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static size_t acme_directory_text_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool acme_directory_body_is(
    const struct appliance_acme_http_response *response,
    const char *expected)
{
    const size_t expected_length = acme_directory_text_length(expected);
    size_t index;

    if (response->body_length != expected_length) {
        return false;
    }
    for (index = 0U; index < expected_length; ++index) {
        if (response->body[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return true;
}

static void acme_directory_start(
    uint32_t now_ms)
{
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    const struct appliance_network_lifecycle *lifecycle =
        wire != NULL ? appliance_network_wire_lifecycle(wire) : NULL;
    const struct appliance_acme_transport_link *link;
    enum appliance_acme_transport_status status;

    /*
     * The exchange starts only on a network that is actually configured, and
     * only on a clock the transport will accept: an unset clock is a refusal
     * by contract, not something to retry forever.
     */
    if (active == NULL || lifecycle == NULL || now_ms == 0U) {
        return;
    }
    serial_write("[ACME-DIRECTORY] proof init\n");
    if (appliance_acme_transport_lwip_init(
            &acme_directory_binding,
            (struct appliance_network_lifecycle *)lifecycle,
            appliance_acme_trust_anchor_der_data(),
            appliance_acme_trust_anchor_der_length()) !=
        APPLIANCE_ACME_TRANSPORT_LWIP_OK) {
        acme_directory_failure("the binding refused the fixture anchor");
    }
    if (!appliance_acme_transport_lwip_published(&acme_directory_binding)) {
        acme_directory_failure("the binding published nothing");
    }
    link = appliance_acme_transport_lwip_link(&acme_directory_binding);
    if (appliance_acme_transport_init(
            &acme_directory_transport, link) != APPLIANCE_ACME_TRANSPORT_OK) {
        acme_directory_failure("the transport refused the link");
    }
    status = appliance_acme_transport_begin(
        &acme_directory_transport,
        "GET", 3U,
        APPLIANCE_ACME_DIRECTORY_QEMU_URL,
        acme_directory_text_length(APPLIANCE_ACME_DIRECTORY_QEMU_URL),
        NULL, 0U, NULL, 0U,
        now_ms);
    /*
     * `begin` returns PENDING for an accepted open: the connection and the
     * handshake belong to `advance`, and only a named refusal is a failure.
     */
    if (status != APPLIANCE_ACME_TRANSPORT_OK &&
        status != APPLIANCE_ACME_TRANSPORT_PENDING) {
        acme_directory_failure(
            appliance_acme_transport_status_string(status));
    }
    acme_directory_deadline_ms = now_ms +
        APPLIANCE_ACME_DIRECTORY_QEMU_DEADLINE_MS;
    acme_directory_started = true;
}

void appliance_acme_directory_qemu_test_tick(
    uint32_t now_ms)
{
    enum appliance_acme_transport_status status;
    struct appliance_acme_http_response response;

    if (acme_directory_finished) {
        return;
    }
    if (!acme_directory_started) {
        acme_directory_start(now_ms);
        return;
    }
    if (now_ms == 0U) {
        return;
    }
    status = appliance_acme_transport_advance(
        &acme_directory_transport, now_ms);
    if (status == APPLIANCE_ACME_TRANSPORT_PENDING) {
        if (now_ms > acme_directory_deadline_ms) {
            acme_directory_failure("the exchange outlived its backstop");
        }
        return;
    }
    if (status != APPLIANCE_ACME_TRANSPORT_OK) {
        acme_directory_failure(
            appliance_acme_transport_status_string(status));
    }
    if (appliance_acme_transport_take_response(
            &acme_directory_transport, &response) !=
        APPLIANCE_ACME_TRANSPORT_OK) {
        acme_directory_failure("the completed exchange has no reply");
    }
    if (response.status_code != 200U) {
        acme_directory_failure("the directory answered a non-200 status");
    }
    if (!acme_directory_body_is(
            &response, APPLIANCE_ACME_DIRECTORY_QEMU_BODY)) {
        acme_directory_failure("the directory body is not the served one");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[ACME-DIRECTORY] exchange ok status=200 bytes=");
        serial_write_u64((uint64_t)response.body_length);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    acme_directory_finished = true;
}

#endif /* HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST */

/*
 * Every kernel build compiles this wildcard source, and only the directory
 * family defines the test macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_acme_directory_qemu_translation_unit_guard;
