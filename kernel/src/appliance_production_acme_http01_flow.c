/*
 * ITEM-508 increment 30: the order path arms the HTTP-01 listener.
 *
 * The two halves already exist and are separately proven: the flow parses the
 * authorization and can produce the challenge's key authorization
 * (increment 5/7), and the listener carries the decision over the wire
 * (increment 28, wire-proven in increment 29).  This translation unit is the
 * join: it takes the token from the flow's own parsed `http-01` challenge and
 * the answer from the flow's own key authorization, then delegates the arm to
 * `appliance_production_acme_http01_arm()`.  It decides nothing itself - no
 * path, no method, no lifetime and no answer byte come from here - so a
 * request can only ever be answered with the challenge the flow is actually
 * answering.
 *
 * The unit is deliberately separate from the listener: the listener must link
 * and be provable without the flow, and the flow must be provable without the
 * socket.  The guard mirrors the listener's own: a build without the
 * production TLS profile (the profile that compiles the flow) gets an empty
 * translation unit, never a dangling reference.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_HTTP01_HOST_TEST)

#include "appliance_production_acme_http01.h"

#include <stddef.h>
#include <stdint.h>

/*
 * The two bounded copies exist because the decision module copies into its own
 * arm block anyway; a refusal before the arm leaves the listener's current
 * token untouched.  The flow refusals are named and mapped, never swallowed:
 * `NULL` stays `NULL`, a buffer that cannot hold the parsed values stays
 * `BUFFER_TOO_SMALL`, and every other flow refusal is `BAD_STATE` because the
 * flow cannot hand out a challenge to answer.
 */
enum appliance_acme_challenge_status
appliance_production_acme_http01_arm_flow(
    struct appliance_production_acme_http01 *http01,
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    uint32_t now_epoch_seconds)
{
    char token[APPLIANCE_ACME_TOKEN_MAX + 1U];
    char answer[APPLIANCE_ACME_CHALLENGE_ANSWER_MAX + 1U];
    size_t token_length = 0U;
    size_t answer_length = 0U;
    enum appliance_acme_flow_status status;

    if (http01 == NULL || flow == NULL || key == NULL) {
        return APPLIANCE_ACME_CHALLENGE_NULL;
    }
    status = appliance_acme_flow_http01_token(
        flow, token, sizeof(token), &token_length);
    if (status == APPLIANCE_ACME_FLOW_OK) {
        status = appliance_acme_flow_key_authorization(
            flow, key, answer, sizeof(answer), &answer_length);
    }
    if (status != APPLIANCE_ACME_FLOW_OK) {
        switch (status) {
        case APPLIANCE_ACME_FLOW_NULL:
            return APPLIANCE_ACME_CHALLENGE_NULL;
        case APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL:
            return APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL;
        default:
            return APPLIANCE_ACME_CHALLENGE_BAD_STATE;
        }
    }
    return appliance_production_acme_http01_arm(
        http01, token, token_length, answer, answer_length, now_epoch_seconds);
}

#endif /* HFOC_PRODUCTION_TLS || HFOC_APPLIANCE_ACME_HTTP01_HOST_TEST */

/*
 * Every kernel build compiles this wildcard source, and the builds outside the
 * production TLS profile guard the body above out: the guard keeps those
 * translation units non-empty and warning-free.
 */
typedef int appliance_production_acme_http01_flow_translation_unit_guard;
