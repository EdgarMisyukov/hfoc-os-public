/*
 * ITEM-508 increment 35 host proof for the order path ordinary boot runs.
 *
 * The driver is driven end to end against a scripted link: the same state
 * machine that speaks to a real CA from ordinary boot is stepped here with
 * canned replies - directory, account, order, authorization, `http-01`
 * challenge (processing, then valid), finalize (processing, then the order
 * poll) and the certificate - so the whole order is proven on the host with
 * no socket and no TLS.  The proof then checks what the item requires of the
 * success path: the listener was armed from the parsed challenge before the
 * CA was told to validate, the verified chain was adopted by the production
 * certificate owner with the ACME provenance, the renewal policy moved to the
 * leaf's own `notAfter`, and the record names the terminal state without ever
 * carrying a nonce, a token or a key.
 *
 * The refusals are proven too: an unconfigured driver, a missing or empty
 * identifier set, an unvalidated instant, an order started while one is in
 * flight, a listener that cannot answer, a chain under a foreign anchor, a
 * transport that fails and an order that outlives its backstop are each a
 * named failure that adopts nothing and moves no expiry.
 */
#include "appliance_production_acme_order.h"

#include "appliance_acme_account.h"
#include "appliance_acme_http.h"
#include "appliance_production_certificate.h"
#include "appliance_state_layout.h"
#include "block_device.h"
#include "hfoc_mbedtls_alloc.h"

#include "appliance-acme-directory-fixtures.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_ACME_ORDER_HOST_TEST

#define HOSTNAME "hfoc-appliance"
#define HOSTNAME_LENGTH 14U
#define FIXTURE_NOW UINT32_C(1800000000)
#define DIRECTORY_URL "https://acme.example/directory"

#define FIXTURE_BLOCKS 8192U
#define FIXTURE_BYTES (FIXTURE_BLOCKS * APPLIANCE_STATE_BLOCK_SIZE)

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

/* ------------------------------------------------------- platform stubs */

bool appliance_production_entropy_ready(void)
{
    return true;
}

int appliance_production_entropy_random(
    void *context,
    unsigned char *output,
    size_t length)
{
    size_t index;

    (void)context;
    for (index = 0U; index < length; ++index) {
        output[index] = (unsigned char)(0x5AU ^ (unsigned char)index);
    }
    return 0;
}

/*
 * The certificate owner reads the stored hostname; this proof has no system
 * record, so the owner falls back to its default identity name - which is the
 * name the fixture leaf's DNS SAN carries, so the same fixture proves the
 * whole path.
 */
enum appliance_persistent_config_status
appliance_persistent_config_hostname(
    const struct appliance_persistent_config *config,
    char *hostname,
    size_t capacity,
    size_t *hostname_length)
{
    (void)config;
    if (hostname == NULL || hostname_length == NULL ||
        capacity <= HOSTNAME_LENGTH) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    memcpy(hostname, HOSTNAME, HOSTNAME_LENGTH + 1U);
    *hostname_length = HOSTNAME_LENGTH;
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

static uint8_t fixture_volume[FIXTURE_BYTES];

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (buffer == NULL || offset + length > sizeof(fixture_volume)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, fixture_volume + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (buffer == NULL || offset + length > sizeof(fixture_volume)) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(fixture_volume + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static bool fixture_loader(
    struct appliance_state_loader *loader,
    struct block_device *device)
{
    memset(loader, 0, sizeof(*loader));
    memset(device, 0, sizeof(*device));
    device->block_size = APPLIANCE_STATE_BLOCK_SIZE;
    device->block_count = FIXTURE_BLOCKS;
    device->read_fn = fixture_read;
    device->write_fn = fixture_write;
    device->flush_fn = fixture_flush;
    loader->partition.device = *device;
    if (appliance_state_layout_default(&loader->layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return false;
    }
    loader->ready = true;
    return true;
}

/* ------------------------------------------------------- PEM helper */

static size_t host_pem_encode(
    const unsigned char *der,
    size_t der_length,
    const char *label,
    char *output,
    size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t written = 0U;
    size_t index = 0U;
    size_t line = 0U;

    written += (size_t)snprintf(
        output + written, capacity - written,
        "-----BEGIN %s-----\n", label);
    for (index = 0U; index < der_length; index += 3U) {
        const size_t remaining = der_length - index;
        const uint32_t triple =
            ((uint32_t)der[index] << 16U) |
            (remaining > 1U ? ((uint32_t)der[index + 1U] << 8U) : 0U) |
            (remaining > 2U ? (uint32_t)der[index + 2U] : 0U);
        char block[4U];
        size_t part;

        block[0] = alphabet[(triple >> 18U) & 0x3FU];
        block[1] = alphabet[(triple >> 12U) & 0x3FU];
        block[2] = remaining > 1U ? alphabet[(triple >> 6U) & 0x3FU] : '=';
        block[3] = remaining > 2U ? alphabet[triple & 0x3FU] : '=';
        for (part = 0U; part < 4U; ++part) {
            output[written++] = block[part];
            if (++line == 64U) {
                output[written++] = '\n';
                line = 0U;
            }
        }
    }
    if (line != 0U) {
        output[written++] = '\n';
    }
    written += (size_t)snprintf(
        output + written, capacity - written,
        "-----END %s-----\n", label);
    output[written] = '\0';
    return written;
}

/* ------------------------------------------------------- scripted link */

/*
 * The link the driver is given: it records the request line of every exchange
 * and answers it from a fixed script, so the proof sees exactly the sequence
 * the state machine asked for.  A reply is complete on its `Content-Length`,
 * and the link reports the peer's close afterwards, exactly as the transport
 * contract allows.
 */
struct scripted_link {
    char request_line[256];
    size_t request_line_length;
    char reply[8192];
    size_t reply_length;
    size_t reply_offset;
    bool reply_ready;
    bool fail_open;
    bool fail_send;
    bool fail_recv;
    bool stall;
    uint32_t challenge_answers;
    uint32_t finalize_answers;
    uint32_t exchange_count;
    bool saw_arm;
    uint32_t arm_calls;
    uint32_t disarm_calls;
    bool listening;
    bool refuse_arm;
    char challenge_token[APPLIANCE_ACME_NAME_MAX + 1U];
};

static struct scripted_link link;

static void scripted_reply(
    uint32_t status,
    const char *reason,
    const char *content_type,
    const char *body,
    const char *location,
    const char *nonce)
{
    char location_header[256];
    int written;

    location_header[0] = '\0';
    if (location != NULL) {
        snprintf(location_header, sizeof(location_header), "Location: %s\r\n",
                 location);
    }
    written = snprintf(
        link.reply, sizeof(link.reply),
        "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Replay-Nonce: %s\r\n%sConnection: close\r\n\r\n%s",
        (unsigned)status, reason, content_type, (unsigned)strlen(body), nonce,
        location_header, body);
    if (written < 0) {
        written = 0;
    }
    link.reply_length = (size_t)written;
    link.reply_offset = 0U;
    link.reply_ready = true;
}

static const char *scripted_directory_body(void)
{
    return "{\"newNonce\":\"https://acme.example/acme/new-nonce\","
           "\"newAccount\":\"https://acme.example/acme/new-account\","
           "\"newOrder\":\"https://acme.example/acme/new-order\"}";
}

static const char *scripted_account_body(void)
{
    return "{\"status\":\"valid\"}";
}

static const char *scripted_order_body(void)
{
    return "{\"status\":\"pending\","
           "\"identifiers\":[{\"type\":\"dns\",\"value\":\"" HOSTNAME "\"}],"
           "\"authorizations\":[\"https://acme.example/acme/authz/1\"],"
           "\"finalize\":\"https://acme.example/acme/finalize/1\"}";
}

static const char *scripted_authorization_body(void)
{
    return "{\"status\":\"pending\","
           "\"identifier\":{\"type\":\"dns\",\"value\":\"" HOSTNAME "\"},"
           "\"challenges\":[{\"type\":\"http-01\","
           "\"url\":\"https://acme.example/acme/chall/1\","
           "\"token\":\"tok3n-ABC_123\",\"status\":\"pending\"}]}";
}

static const char *scripted_challenge_body(const char *status)
{
    static char body[256];

    snprintf(body, sizeof(body),
             "{\"type\":\"http-01\","
             "\"url\":\"https://acme.example/acme/chall/1\","
             "\"token\":\"tok3n-ABC_123\",\"status\":\"%s\"}", status);
    return body;
}

static const char *scripted_finalized_order_body(void)
{
    return "{\"status\":\"valid\","
           "\"identifiers\":[{\"type\":\"dns\",\"value\":\"" HOSTNAME "\"}],"
           "\"authorizations\":[\"https://acme.example/acme/authz/1\"],"
           "\"finalize\":\"https://acme.example/acme/finalize/1\","
           "\"certificate\":\"https://acme.example/acme/cert/1\"}";
}

static void scripted_route(void)
{
    const char *target = link.request_line;
    const char *method_end = strchr(target, ' ');
    char method[8];
    size_t method_length;
    const char *path;
    size_t path_length;

    if (method_end == NULL) {
        scripted_reply(400U, "Bad Request", "application/json",
                       "{\"refused\":true}", NULL, "nonce-0");
        return;
    }
    method_length = (size_t)(method_end - target);
    if (method_length >= sizeof(method)) {
        method_length = sizeof(method) - 1U;
    }
    memcpy(method, target, method_length);
    method[method_length] = '\0';
    path = method_end + 1;
    {
        const char *path_end = strchr(path, ' ');

        path_length = path_end != NULL ? (size_t)(path_end - path)
                                       : strlen(path);
    }
    if (strcmp(method, "GET") == 0 && path_length == 10U &&
        strncmp(path, "/directory", 10U) == 0) {
        scripted_reply(200U, "OK", "application/json",
                       scripted_directory_body(), NULL, "nonce-directory");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 17U &&
        strncmp(path, "/acme/new-account", 17U) == 0) {
        scripted_reply(200U, "OK", "application/json",
                       scripted_account_body(),
                       "https://acme.example/acme/acct/1", "nonce-account");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 15U &&
        strncmp(path, "/acme/new-order", 15U) == 0) {
        scripted_reply(200U, "OK", "application/json",
                       scripted_order_body(),
                       "https://acme.example/acme/order/1", "nonce-order");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 13U &&
        strncmp(path, "/acme/authz/1", 13U) == 0) {
        scripted_reply(200U, "OK", "application/json",
                       scripted_authorization_body(), NULL, "nonce-authz");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 13U &&
        strncmp(path, "/acme/chall/1", 13U) == 0) {
        ++link.challenge_answers;
        scripted_reply(
            200U, "OK", "application/json",
            scripted_challenge_body(
                link.challenge_answers == 1U ? "processing" : "valid"),
            NULL, "nonce-challenge");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 16U &&
        strncmp(path, "/acme/finalize/1", 16U) == 0) {
        ++link.finalize_answers;
        scripted_reply(
            200U, "OK", "application/json",
            link.finalize_answers == 1U ? scripted_order_body()
                                        : scripted_finalized_order_body(),
            link.finalize_answers == 1U
                ? "https://acme.example/acme/order/1"
                : NULL,
            "nonce-finalize");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 13U &&
        strncmp(path, "/acme/order/1", 13U) == 0) {
        scripted_reply(200U, "OK", "application/json",
                       scripted_finalized_order_body(), NULL,
                       "nonce-order-poll");
        return;
    }
    if (strcmp(method, "POST") == 0 && path_length == 12U &&
        strncmp(path, "/acme/cert/1", 12U) == 0) {
        static char chain[4096];

        chain[0] = '\0';
        host_pem_encode(directory_fixture_leaf,
                        sizeof(directory_fixture_leaf), "CERTIFICATE", chain,
                        sizeof(chain));
        host_pem_encode(directory_fixture_ca, sizeof(directory_fixture_ca),
                        "CERTIFICATE", chain + strlen(chain),
                        sizeof(chain) - strlen(chain));
        scripted_reply(200U, "OK", "application/pem-certificate-chain", chain,
                       NULL, "nonce-certificate");
        return;
    }
    scripted_reply(404U, "Not Found", "application/json",
                   "{\"refused\":true}", NULL, "nonce-404");
}

static enum appliance_acme_transport_link_status scripted_open(
    void *context,
    const char *host,
    uint16_t port,
    bool literal_ipv4)
{
    (void)context;
    (void)host;
    (void)port;
    (void)literal_ipv4;
    if (link.fail_open) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    ++link.exchange_count;
    link.request_line_length = 0U;
    link.request_line[0] = '\0';
    link.reply_ready = false;
    link.reply_offset = 0U;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static enum appliance_acme_transport_link_status scripted_advance(void *context)
{
    (void)context;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static enum appliance_acme_transport_link_status scripted_send(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    size_t index;

    (void)context;
    if (link.fail_send) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    for (index = 0U; index < length; ++index) {
        const uint8_t byte = bytes[index];

        if (link.reply_ready) {
            /* The body or the headers; the request line is already complete. */
            continue;
        }
        if (byte == (uint8_t)'\n') {
            scripted_route();
            continue;
        }
        if (link.request_line_length + 1U < sizeof(link.request_line)) {
            link.request_line[link.request_line_length++] = (char)byte;
            link.request_line[link.request_line_length] = '\0';
        }
    }
    *bytes_accepted = length;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static enum appliance_acme_transport_link_status scripted_recv(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    size_t available;

    (void)context;
    if (link.fail_recv) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    if (link.stall) {
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    }
    if (!link.reply_ready || link.reply_offset >= link.reply_length) {
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    }
    available = link.reply_length - link.reply_offset;
    if (available > capacity) {
        available = capacity;
    }
    memcpy(bytes, link.reply + link.reply_offset, available);
    link.reply_offset += available;
    *bytes_received = available;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static void scripted_close(void *context)
{
    (void)context;
    link.reply_ready = false;
    link.reply_offset = 0U;
}

static const struct appliance_acme_transport_link scripted_link = {
    .context = &link,
    .open = scripted_open,
    .advance = scripted_advance,
    .send = scripted_send,
    .recv = scripted_recv,
    .close = scripted_close,
};

/* ------------------------------------------------------- listener stub */

static bool order_listener_listening(void *context)
{
    (void)context;
    return link.listening;
}

static enum appliance_acme_challenge_status order_listener_arm(
    void *context,
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    uint32_t now_epoch_seconds)
{
    (void)context;
    (void)key;
    (void)now_epoch_seconds;
    ++link.arm_calls;
    link.saw_arm = true;
    if (link.refuse_arm) {
        return APPLIANCE_ACME_CHALLENGE_BAD_STATE;
    }
    memset(link.challenge_token, 0, sizeof(link.challenge_token));
    if (flow != NULL && flow->authorization.http01.token_length != 0U &&
        flow->authorization.http01.token_length <
            sizeof(link.challenge_token)) {
        memcpy(link.challenge_token, flow->authorization.http01.token,
               flow->authorization.http01.token_length);
    }
    return APPLIANCE_ACME_CHALLENGE_OK;
}

static void order_listener_disarm(void *context)
{
    (void)context;
    ++link.disarm_calls;
}

static const struct appliance_production_acme_order_listener
    order_listener = {
        .context = &link,
        .listening = order_listener_listening,
        .arm_flow = order_listener_arm,
        .disarm = order_listener_disarm,
};

/* ------------------------------------------------------- fixtures */

static void reset_link(void)
{
    memset(&link, 0, sizeof(link));
    link.listening = true;
}

static void reset_volume(void)
{
    memset(fixture_volume, 0, sizeof(fixture_volume));
}

static void tick_until_terminal(
    struct appliance_production_acme_order *order,
    uint32_t *now_ms)
{
    uint32_t turns = 0U;

    while (appliance_production_acme_order_state(order) ==
               APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING &&
           turns < 1000U) {
        *now_ms += 10U;
        (void)appliance_production_acme_order_tick(order, *now_ms);
        ++turns;
    }
}

static void start_full_order(
    struct appliance_production_acme_order *order,
    struct appliance_production_certificate *owner,
    struct appliance_acme_renewal *renewal,
    struct appliance_state_loader *loader,
    struct block_device *device)
{
    static const char *const names[] = { HOSTNAME };
    static const size_t lengths[] = { HOSTNAME_LENGTH };

    reset_link();
    reset_volume();
    memset(owner, 0, sizeof(*owner));
    CHECK(fixture_loader(loader, device),
          "the fixture state layout is valid for the order");
    CHECK(appliance_production_certificate_prepare(owner, loader,
                                                   FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(owner),
          "the certificate owner generates and persists its identity");
    appliance_acme_renewal_init(renewal);
    appliance_production_acme_order_init(order);
    CHECK(appliance_production_acme_order_configure(
              order, &scripted_link, owner, renewal, &order_listener,
              directory_fixture_ca, sizeof(directory_fixture_ca),
              DIRECTORY_URL) == APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "the order driver binds the scripted link and the fixture anchor");
    CHECK(appliance_production_acme_order_start(
              order, names, lengths, 1U, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "the order starts on a validated instant with one identifier");
}

/* ------------------------------------------------------- tests */

static void test_start_refusals(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;
    static const char *const names[] = { HOSTNAME, HOSTNAME };
    static const size_t lengths[] = { HOSTNAME_LENGTH, HOSTNAME_LENGTH };
    size_t index;

    reset_link();
    reset_volume();
    CHECK(fixture_loader(&loader, &device),
          "the fixture state layout is valid for the refusal cases");
    memset(&owner, 0, sizeof(owner));
    CHECK(appliance_production_certificate_prepare(&owner, &loader,
                                                   FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK,
          "the refusal cases have a persisted identity");
    appliance_acme_renewal_init(&renewal);
    appliance_production_acme_order_init(&order);

    CHECK(appliance_production_acme_order_configure(
              NULL, &scripted_link, &owner, &renewal, &order_listener,
              directory_fixture_ca, sizeof(directory_fixture_ca),
              DIRECTORY_URL) == APPLIANCE_PRODUCTION_ACME_ORDER_NULL,
          "a null driver is refused");
    CHECK(appliance_production_acme_order_configure(
              &order, NULL, &owner, &renewal, &order_listener,
              directory_fixture_ca, sizeof(directory_fixture_ca),
              DIRECTORY_URL) == APPLIANCE_PRODUCTION_ACME_ORDER_NULL,
          "a null link is refused");
    CHECK(appliance_production_acme_order_start(
              &order, names, lengths, 1U, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE,
          "an unconfigured driver refuses to start");
    CHECK(appliance_production_acme_order_configure(
              &order, &scripted_link, &owner, &renewal, &order_listener,
              directory_fixture_ca, sizeof(directory_fixture_ca),
              DIRECTORY_URL) == APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "the driver configures for the start refusals");

    CHECK(appliance_production_acme_order_start(
              &order, names, lengths, 0U, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS,
          "an order without identifiers is refused by name");
    CHECK(appliance_production_acme_order_start(
              &order, names, lengths,
              APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX + 1U,
              FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_TOO_MANY_IDENTIFIERS,
          "an order over the identifier bound is refused by name");
    CHECK(appliance_production_acme_order_start(
              &order, names, lengths, 1U, 0U) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_BAD_CLOCK,
          "an unvalidated instant starts nothing");
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_IDLE,
          "a refused start leaves the driver idle");

    /*
     * A name that is not a name is refused too, and no byte of the refused
     * order was sent anywhere.
     */
    {
        static const char *const empty_names[] = { "" };
        static const size_t empty_lengths[] = { 0U };

        CHECK(appliance_production_acme_order_start(
                  &order, empty_names, empty_lengths, 1U, FIXTURE_NOW) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS,
              "an empty identifier is refused by name");
    }
    CHECK(link.exchange_count == 0U,
          "a refused start opens no exchange");
    for (index = 0U; index < sizeof(order.record); ++index) {
        if (order.record[index] == '\0') {
            break;
        }
    }
    CHECK(index == strlen(order.record),
          "the refused driver's record is one bounded string");
}

static void test_full_order_and_adoption(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;
    uint32_t now_ms = 1000U;
    uint32_t expires;

    start_full_order(&order, &owner, &renewal, &loader, &device);
    tick_until_terminal(&order, &now_ms);

    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_DONE,
          "the scripted order reaches the terminal DONE state");
    CHECK(link.saw_arm && link.arm_calls == 1U &&
              strcmp(link.challenge_token, "tok3n-ABC_123") == 0,
          "the listener was armed once from the parsed challenge token");
    CHECK(link.challenge_answers == 2U && link.finalize_answers == 1U,
          "the challenge was polled once and the finalize answered");
    CHECK(appliance_production_acme_order_certificate_members(&order) == 2U,
          "the verified chain carries the leaf and its issuer");
    expires = appliance_production_acme_order_leaf_not_after(&order);
    CHECK(expires > FIXTURE_NOW,
          "the order publishes the leaf's own expiry");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == expires,
          "the renewal policy moved to the verified leaf expiry");
    CHECK(appliance_acme_renewal_evaluate(&renewal, FIXTURE_NOW) ==
              APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED,
          "the fresh certificate is not due");
    CHECK(appliance_acme_renewal_evaluate(
              &renewal,
              expires - APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS + 1U) ==
              APPLIANCE_ACME_RENEWAL_DECISION_DUE,
          "the policy's renewal window opens before the leaf expires");
    CHECK(link.disarm_calls >= 1U,
          "the listener is disarmed when the order finishes");

    /*
     * The chain was adopted by the production owner with its own provenance:
     * a fresh owner on the same volume loads it as ACME-issued, never as the
     * operator's upload and never as the generated identity.
     */
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(&reloaded, &loader,
                                                   FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              appliance_production_certificate_available(&reloaded),
          "the next boot loads the issued record");
    CHECK(reloaded.acme_issued && !reloaded.imported,
          "the loaded record keeps the ACME provenance");
    CHECK(reloaded.certificate_length == sizeof(directory_fixture_leaf) &&
              memcmp(reloaded.certificate_der, directory_fixture_leaf,
                     sizeof(directory_fixture_leaf)) == 0,
          "the adopted leaf is the certificate the order downloaded");
    CHECK(reloaded.chain_length == sizeof(directory_fixture_ca) &&
              memcmp(reloaded.chain_der, directory_fixture_ca,
                     sizeof(directory_fixture_ca)) == 0,
          "the adopted chain is the issuer the order downloaded");

    /* The record names the terminal state and carries no material. */
    CHECK(strstr(appliance_production_acme_order_record(&order),
                 "[PRODUCTION-ACME-ORDER] state=DONE step=CERTIFICATE") !=
              NULL,
          "the order record names the terminal state and step");
    CHECK(strstr(appliance_production_acme_order_record(&order),
                 "tok3n") == NULL &&
              strstr(appliance_production_acme_order_record(&order),
                     "nonce") == NULL &&
              strstr(appliance_production_acme_order_record(&order),
                     "expires=") != NULL,
          "the order record carries the expiry and no token or nonce");
}

static void test_listener_refusals(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;
    uint32_t now_ms = 1000U;

    /* The listener is not up yet. */
    start_full_order(&order, &owner, &renewal, &loader, &device);
    link.listening = false;
    tick_until_terminal(&order, &now_ms);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED &&
              appliance_production_acme_order_last_status(&order) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER,
          "a challenge no listener can answer fails the order by name");
    CHECK(strstr(appliance_production_acme_order_record(&order),
                 "reason=NO_LISTENER") != NULL,
          "the no-listener failure is named in the record");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "a failed order moves no renewal expiry");

    /* The listener is up but refuses the token. */
    start_full_order(&order, &owner, &renewal, &loader, &device);
    link.refuse_arm = true;
    tick_until_terminal(&order, &now_ms);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED &&
              appliance_production_acme_order_last_status(&order) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER,
          "a listener that refuses the challenge fails the order by name");
}

static void test_chain_refusal(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    static struct appliance_production_certificate reloaded;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;
    static const unsigned char foreign_anchor[32] = { 1U };
    uint32_t now_ms = 1000U;

    reset_link();
    reset_volume();
    memset(&owner, 0, sizeof(owner));
    CHECK(fixture_loader(&loader, &device),
          "the fixture state layout is valid for the foreign anchor");
    CHECK(appliance_production_certificate_prepare(&owner, &loader,
                                                   FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK,
          "the foreign-anchor case has a persisted identity");
    appliance_acme_renewal_init(&renewal);
    appliance_production_acme_order_init(&order);
    /*
     * The driver is bound to an anchor that cannot have signed the served
     * chain: the order must fail at the certificate step and adopt nothing.
     */
    CHECK(appliance_production_acme_order_configure(
              &order, &scripted_link, &owner, &renewal, &order_listener,
              foreign_anchor, sizeof(foreign_anchor), DIRECTORY_URL) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "the driver binds a foreign anchor for the refusal");
    CHECK(appliance_production_acme_order_start(
              &order, (const char *const[]){ HOSTNAME },
              (const size_t[]){ HOSTNAME_LENGTH }, 1U, FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "the order starts under the foreign anchor");
    tick_until_terminal(&order, &now_ms);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED &&
              appliance_production_acme_order_last_status(&order) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED,
          "a chain under a foreign anchor fails the order by name");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "the refused chain moves no renewal expiry");
    memset(&reloaded, 0, sizeof(reloaded));
    CHECK(appliance_production_certificate_prepare(&reloaded, &loader,
                                                   FIXTURE_NOW) ==
              APPLIANCE_PRODUCTION_CERTIFICATE_OK &&
              !reloaded.acme_issued,
          "the refused chain is never adopted");
}

static void test_transport_failure_and_timeout(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;
    uint32_t now_ms = 1000U;

    /* The second exchange fails at the link. */
    start_full_order(&order, &owner, &renewal, &loader, &device);
    (void)appliance_production_acme_order_tick(&order, now_ms);
    link.fail_recv = true;
    tick_until_terminal(&order, &now_ms);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED &&
              appliance_production_acme_order_last_status(&order) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED,
          "a link failure fails the order by name");
    CHECK(strstr(appliance_production_acme_order_record(&order),
                 "reason=TRANSPORT_FAILED") != NULL,
          "the transport failure is named in the record");

    /* The link never completes; the backstop turns it into TIMEOUT. */
    start_full_order(&order, &owner, &renewal, &loader, &device);
    link.stall = true;
    now_ms = 1000U;
    (void)appliance_production_acme_order_tick(&order, now_ms);
    now_ms += APPLIANCE_PRODUCTION_ACME_ORDER_DEADLINE_MS + 1U;
    (void)appliance_production_acme_order_tick(&order, now_ms);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED &&
              appliance_production_acme_order_last_status(&order) ==
                  APPLIANCE_PRODUCTION_ACME_ORDER_TIMEOUT,
          "an order that outlives its backstop fails instead of waiting");
    CHECK(appliance_acme_renewal_expires_at(&renewal) == 0U,
          "a timed-out order moves no renewal expiry");
}

static void test_abort(void)
{
    static struct appliance_production_acme_order order;
    static struct appliance_production_certificate owner;
    struct appliance_state_loader loader;
    struct block_device device;
    struct appliance_acme_renewal renewal;

    start_full_order(&order, &owner, &renewal, &loader, &device);
    (void)appliance_production_acme_order_tick(&order, 1000U);
    appliance_production_acme_order_abort(&order);
    CHECK(appliance_production_acme_order_state(&order) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED,
          "an aborted order is failed, not left running");
    /* A finished driver refuses a second order until it is restarted. */
    CHECK(appliance_production_acme_order_tick(&order, 2000U) ==
              APPLIANCE_PRODUCTION_ACME_ORDER_OK,
          "ticking a terminal order is a no-op");
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: the mbedTLS pools do not initialize\n");
        return 1;
    }
    test_start_refusals();
    test_full_order_and_adoption();
    test_listener_refusals();
    test_chain_refusal();
    test_transport_failure_and_timeout();
    test_abort();
    if (failures != 0) {
        fprintf(stderr, "appliance production ACME order host test: %d failures\n",
                failures);
        return 1;
    }
    printf("appliance production ACME order host test: ALL OK\n");
    return 0;
}

#endif
