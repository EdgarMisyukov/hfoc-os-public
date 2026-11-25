/*
 * ITEM-508 increment 29: the fixture token of the HTTP-01 wire proof.
 *
 * The increment-28 listener comes up unarmed on every boot, and on an ordinary
 * boot nothing arms it: the order path that will arm it is the increment after
 * this one.  The wire proof therefore needs one boot on which a token is
 * really armed, with a key authorization the host side knows byte for byte.
 *
 * This translation unit is that boot's fixture.  It decides nothing: once the
 * production listener is up it calls the production
 * `appliance_production_acme_http01_arm()` with one fixed bounded pair and the
 * one fixture epoch of `appliance_acme_fixture_epoch_seconds()` (increment 31
 * moved the epoch to its own unit so the order fixture reads the same value),
 * and the kernel loop hands the listener's `poll` that same epoch, so the
 * armed window is the production window and the answer on the wire is the
 * production answer.
 *
 * The epoch is a fixture constant for the same reason the increment-17 order
 * proof names one: the proof has no SNTP peer, the production wall clock is
 * therefore zero, and the decision module refuses a zero clock by name
 * (`BAD_STATE` at arming, `EXPIRED` on a request) instead of arming an answer
 * that never expires.  A fixture that read its own clock could arm an answer
 * that is already over; a validated production clock stays ITEM-503's gate.
 *
 * The define is set by the `ACME_HTTP01_*` fixture build and never by the
 * production build; every other kernel build compiles an empty object, and the
 * harness asserts that `[ACME-HTTP01-FIXTURE]` appears in neither the
 * production kernel nor the production ISO.
 */
#ifdef HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST

#include "appliance_production_acme_http01.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * One fixed pair the host side of the proof carries verbatim.  The token has
 * to be a valid ACME token by the decision module's rules and the key
 * authorization is `token "." thumbprint` as the module expects; the proof
 * compares the served bytes against this exact constant, so a wrong answer
 * cannot pass as a right one.
 */
#define APPLIANCE_ACME_HTTP01_FIXTURE_TOKEN "hfoc-fixture-token"
#define APPLIANCE_ACME_HTTP01_FIXTURE_KEY_AUTHORIZATION \
    "hfoc-fixture-token.hfoc-fixture-thumbprint"

#define APPLIANCE_ACME_HTTP01_FIXTURE_RECORD_MAX 128U

static bool appliance_acme_http01_fixture_done;
static char appliance_acme_http01_fixture_record[
    APPLIANCE_ACME_HTTP01_FIXTURE_RECORD_MAX + 1U];
static bool appliance_acme_http01_fixture_logged;

/*
 * One bounded record per outcome, written in a single serial sequence so the
 * harness reads a whole line: a fixture that never armed has to be
 * distinguishable from a listener that never came up, and neither may look
 * like a silent timeout.
 */
static void appliance_acme_http01_fixture_publish(
    const char *record, size_t length)
{
    if (record == NULL || length == 0U ||
        length > APPLIANCE_ACME_HTTP01_FIXTURE_RECORD_MAX) {
        return;
    }
    if (appliance_acme_http01_fixture_logged &&
        strcmp(record, appliance_acme_http01_fixture_record) == 0) {
        return;
    }
    memcpy(appliance_acme_http01_fixture_record, record, length);
    appliance_acme_http01_fixture_record[length] = '\0';
    appliance_acme_http01_fixture_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(appliance_acme_http01_fixture_record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

void appliance_acme_http01_fixture_qemu_tick(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_epoch_seconds)
{
    static const char armed_record[] =
        "[ACME-HTTP01-FIXTURE] v=1 armed=1 token="
        APPLIANCE_ACME_HTTP01_FIXTURE_TOKEN;
    enum appliance_acme_challenge_status status;

    if (http01 == NULL || appliance_acme_http01_fixture_done) {
        return;
    }
    /*
     * The listener owns its own start: the fixture arms only once the
     * production listener is really listening, because arming before that is
     * `BAD_STATE` by the module's own contract.
     */
    if (!appliance_production_acme_http01_listening(http01)) {
        return;
    }
    status = appliance_production_acme_http01_arm(
        http01,
        APPLIANCE_ACME_HTTP01_FIXTURE_TOKEN,
        sizeof(APPLIANCE_ACME_HTTP01_FIXTURE_TOKEN) - 1U,
        APPLIANCE_ACME_HTTP01_FIXTURE_KEY_AUTHORIZATION,
        sizeof(APPLIANCE_ACME_HTTP01_FIXTURE_KEY_AUTHORIZATION) - 1U,
        now_epoch_seconds);
    appliance_acme_http01_fixture_done = true;
    if (status == APPLIANCE_ACME_CHALLENGE_OK) {
        appliance_acme_http01_fixture_publish(
            armed_record, sizeof(armed_record) - 1U);
        return;
    }
    /*
     * The refusal names the module's own status string, so a failed arm is
     * never mistaken for a request that never arrived.
     */
    {
        const char *reason = appliance_acme_challenge_status_string(status);
        const char *prefix = "[ACME-HTTP01-FIXTURE] v=1 armed=0 status=";

        if (reason != NULL &&
            strlen(prefix) + strlen(reason) <=
                APPLIANCE_ACME_HTTP01_FIXTURE_RECORD_MAX) {
            char record[APPLIANCE_ACME_HTTP01_FIXTURE_RECORD_MAX + 1U];
            size_t length = strlen(prefix);

            memcpy(record, prefix, length);
            memcpy(&record[length], reason, strlen(reason));
            length += strlen(reason);
            appliance_acme_http01_fixture_publish(record, length);
        }
    }
}

const char *appliance_acme_http01_fixture_qemu_state(void)
{
    return appliance_acme_http01_fixture_logged
               ? appliance_acme_http01_fixture_record
               : "";
}

#endif /* HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST */

/*
 * Every kernel build compiles this wildcard source, and only the fixture
 * family defines a test macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_acme_http01_fixture_qemu_translation_unit_guard;
