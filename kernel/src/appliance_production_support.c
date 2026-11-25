#include "appliance_production_support.h"

#include "appliance_management_bind.h"
#include "appliance_support_break_glass.h"
#include "appliance_support_bundle.h"
#include "appliance_support_session.h"
#include "support_bundle_exporter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_production_support_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_production_support_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_production_support_state_valid(
    enum appliance_production_support_state state)
{
    return state >= APPLIANCE_PRODUCTION_SUPPORT_STATE_NONE &&
           state <= APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED;
}

static bool appliance_production_support_status_valid(
    enum appliance_production_support_status status)
{
    return status >= APPLIANCE_PRODUCTION_SUPPORT_OK &&
           status <= APPLIANCE_PRODUCTION_SUPPORT_BUFFER_TOO_SMALL;
}

static bool appliance_production_support_initialized(
    const struct appliance_production_support *support)
{
    return support != NULL &&
           support->initialized &&
           support->version == APPLIANCE_PRODUCTION_SUPPORT_VERSION &&
           appliance_production_support_state_valid(support->state) &&
           appliance_production_support_status_valid(support->last_status);
}

static bool appliance_production_support_bundle_sealed(
    const struct appliance_production_support *support)
{
    return support->bundle_published &&
           appliance_support_bundle_is_sealed(&support->bundle);
}

static void appliance_production_support_derive_state(
    struct appliance_production_support *support)
{
    if (support->session.state ==
            APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE ||
        support->break_glass.state ==
            APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
        return;
    }
    if (support->break_glass_partial_count != 0U ||
        !appliance_production_support_bundle_sealed(support)) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED;
        return;
    }
    support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_READY;
}

static enum appliance_production_support_status
appliance_production_support_map_session(
    enum appliance_support_session_status status)
{
    switch (status) {
    case APPLIANCE_SUPPORT_SESSION_OK:
        return APPLIANCE_PRODUCTION_SUPPORT_OK;
    case APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED:
    case APPLIANCE_SUPPORT_SESSION_DENIED:
    case APPLIANCE_SUPPORT_SESSION_BAD_SCOPE:
        return APPLIANCE_PRODUCTION_SUPPORT_DENIED;
    case APPLIANCE_SUPPORT_SESSION_UNAVAILABLE:
    case APPLIANCE_SUPPORT_SESSION_NULL:
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    default:
        return APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
    }
}

static enum appliance_production_support_status
appliance_production_support_map_break_glass(
    enum appliance_support_break_glass_status status)
{
    switch (status) {
    case APPLIANCE_SUPPORT_BREAK_GLASS_OK:
        return APPLIANCE_PRODUCTION_SUPPORT_OK;
    case APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED:
    case APPLIANCE_SUPPORT_BREAK_GLASS_DENIED:
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN:
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS:
    case APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED:
        return APPLIANCE_PRODUCTION_SUPPORT_DENIED;
    case APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE:
    case APPLIANCE_SUPPORT_BREAK_GLASS_NULL:
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    default:
        return APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
    }
}

enum appliance_production_support_status appliance_production_support_init(
    struct appliance_production_support *support,
    struct appliance_management_bind *management_bind)
{
    if (support == NULL) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    appliance_production_support_zero_bytes((unsigned char *)support,
                                            sizeof(*support));
    support->version = APPLIANCE_PRODUCTION_SUPPORT_VERSION;
    support->management_bind = management_bind;
    if (appliance_support_session_init(&support->session) !=
            APPLIANCE_SUPPORT_SESSION_OK ||
        appliance_support_break_glass_init(&support->break_glass) !=
            APPLIANCE_SUPPORT_BREAK_GLASS_OK) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
        return APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
    }
    support->initialized = true;
    support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED;
    support->last_status = APPLIANCE_PRODUCTION_SUPPORT_OK;
    return APPLIANCE_PRODUCTION_SUPPORT_OK;
}

enum appliance_production_support_status
appliance_production_support_publish_bundle(
    struct appliance_production_support *support,
    const struct support_bundle_manifest_source *source,
    appliance_support_bundle_mac_fn mac,
    void *context,
    const uint8_t *seal_key,
    size_t seal_key_length)
{
    struct support_bundle_exporter exporter;
    struct appliance_support_bundle_redaction report;
    size_t offset = 0U;
    enum appliance_support_bundle_status bundle_status;

    if (!appliance_production_support_initialized(support) ||
        source == NULL) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    if (support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED ||
        support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED) {
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
        return APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
    }

    if (support_bundle_exporter_init(&exporter, source) !=
        SUPPORT_BUNDLE_EXPORTER_OK) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    }
    for (;;) {
        size_t unit_length = 0U;
        enum support_bundle_exporter_status step =
            support_bundle_exporter_next(
                &exporter,
                support->bundle_buffer + offset,
                sizeof(support->bundle_buffer) - offset,
                &unit_length);

        if (step == SUPPORT_BUNDLE_EXPORTER_DONE) {
            break;
        }
        if (step == SUPPORT_BUNDLE_EXPORTER_TOO_SMALL) {
            (void)support_bundle_exporter_close(&exporter);
            support->bundle_published = false;
            support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
            support->last_status =
                APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_TOO_LARGE;
            return APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_TOO_LARGE;
        }
        if (step != SUPPORT_BUNDLE_EXPORTER_OK) {
            (void)support_bundle_exporter_close(&exporter);
            support->bundle_published = false;
            support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
            support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
            return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        }
        offset += unit_length;
    }
    (void)support_bundle_exporter_close(&exporter);

    if (appliance_support_bundle_redaction_eval(source, &report) !=
        APPLIANCE_SUPPORT_BUNDLE_OK) {
        support->bundle_published = false;
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    }

    bundle_status = appliance_support_bundle_seal(
        &support->bundle, support->bundle_buffer, offset, &report, mac,
        context, seal_key, seal_key_length);
    support->bundle_bytes = (uint64_t)offset;
    support->bundle_sections = support->bundle.binding.entry_count;
    support->bundle_withheld = support->bundle.binding.redaction.withheld;
    ++support->bundle_count;

    switch (bundle_status) {
    case APPLIANCE_SUPPORT_BUNDLE_OK:
        if (appliance_support_bundle_verify(
                &support->bundle, support->bundle_buffer, offset, mac,
                context, seal_key, seal_key_length) !=
            APPLIANCE_SUPPORT_BUNDLE_OK) {
            support->bundle_published = false;
            support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
            support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
            return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        }
        support->bundle_published = true;
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_READY;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_OK;
        return APPLIANCE_PRODUCTION_SUPPORT_OK;
    case APPLIANCE_SUPPORT_BUNDLE_NO_SEAL_KEY:
    case APPLIANCE_SUPPORT_BUNDLE_MAC_UNAVAILABLE:
        support->bundle_published = true;
        appliance_production_support_derive_state(support);
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
        return APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
    case APPLIANCE_SUPPORT_BUNDLE_REDACTION_FAILED:
        support->bundle_published = false;
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_DENIED;
        return APPLIANCE_PRODUCTION_SUPPORT_DENIED;
    default:
        support->bundle_published = false;
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    }
}

enum appliance_production_support_status
appliance_production_support_session_request(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t requester_id,
    uint32_t scopes,
    uint64_t now_tick)
{
    enum appliance_production_support_status status;

    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    status = appliance_production_support_map_session(
        appliance_support_session_request(&support->session, origin,
                                          requester_id, scopes,
                                          now_tick));
    support->last_status = status;
    if (status == APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
    }
    return status;
}

enum appliance_production_support_status
appliance_production_support_session_consent(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t ttl_ticks,
    uint64_t now_tick)
{
    enum appliance_production_support_status status;

    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    status = appliance_production_support_map_session(
        appliance_support_session_consent(
            &support->session, origin, principal_id, ttl_ticks,
            now_tick));
    support->last_status = status;
    if (status == APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
    }
    return status;
}

enum appliance_production_support_status
appliance_production_support_session_revoke(
    struct appliance_production_support *support,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t now_tick)
{
    enum appliance_production_support_status status;

    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    status = appliance_production_support_map_session(
        appliance_support_session_revoke(&support->session, origin,
                                         principal_id, now_tick));
    support->last_status = status;
    return status;
}

enum appliance_production_support_status
appliance_production_support_break_glass_arm(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint32_t principal_id,
    uint32_t actions,
    uint64_t now_tick,
    uint64_t *token_out)
{
    enum appliance_production_support_status status;

    if (!appliance_production_support_initialized(support) ||
        token_out == NULL) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    status = appliance_production_support_map_break_glass(
        appliance_support_break_glass_arm(
            &support->break_glass, origin, principal_id, actions,
            now_tick, token_out));
    support->last_status = status;
    if (status == APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE) {
        support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE;
    }
    return status;
}

enum appliance_production_support_status
appliance_production_support_break_glass_apply(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint64_t token,
    uint64_t now_tick,
    uint32_t *actions_out)
{
    enum appliance_production_support_status status;
    uint32_t actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    bool partial = false;

    if (!appliance_production_support_initialized(support) ||
        actions_out == NULL) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    *actions_out = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    status = appliance_production_support_map_break_glass(
        appliance_support_break_glass_apply(
            &support->break_glass, origin, token, now_tick, &actions));
    if (status != APPLIANCE_PRODUCTION_SUPPORT_OK) {
        support->last_status = status;
        return status;
    }
    *actions_out = actions;

    if ((actions &
         APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY) !=
        0U) {
        if (support->management_bind == NULL ||
            appliance_management_bind_publish_local_only(
                support->management_bind) !=
                APPLIANCE_MANAGEMENT_BIND_OK) {
            partial = true;
        }
    }
    if ((actions &
         APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT) !=
        0U) {
        if (support->session.state ==
                APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE ||
            support->session.state ==
                APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED) {
            if (appliance_support_session_revoke(
                    &support->session,
                    APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE,
                    support->break_glass.applied_principal,
                    now_tick) != APPLIANCE_SUPPORT_SESSION_OK) {
                partial = true;
            }
        }
    }

    ++support->break_glass_effect_count;
    if (partial) {
        ++support->break_glass_partial_count;
        appliance_production_support_derive_state(support);
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
        return APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
    }
    appliance_production_support_derive_state(support);
    support->last_status = APPLIANCE_PRODUCTION_SUPPORT_OK;
    return APPLIANCE_PRODUCTION_SUPPORT_OK;
}

enum appliance_production_support_status
appliance_production_support_break_glass_cancel(
    struct appliance_production_support *support,
    enum appliance_support_break_glass_origin origin,
    uint64_t now_tick)
{
    enum appliance_production_support_status status;

    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    status = appliance_production_support_map_break_glass(
        appliance_support_break_glass_cancel(&support->break_glass,
                                             origin, now_tick));
    support->last_status = status;
    return status;
}

enum appliance_production_support_status appliance_production_support_refresh(
    struct appliance_production_support *support,
    uint64_t now_tick)
{
    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    if (support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED ||
        support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED) {
        return APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE;
    }
    (void)appliance_support_session_refresh(&support->session, now_tick);
    (void)appliance_support_break_glass_refresh(&support->break_glass,
                                                now_tick);
    appliance_production_support_derive_state(support);
    if (support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_READY) {
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_OK;
        return APPLIANCE_PRODUCTION_SUPPORT_OK;
    }
    if (support->state == APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE) {
        support->last_status = APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
        return APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE;
    }
    support->last_status = APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
    return APPLIANCE_PRODUCTION_SUPPORT_DEGRADED;
}

enum appliance_production_support_status appliance_production_support_stop(
    struct appliance_production_support *support,
    uint64_t now_tick)
{
    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    (void)appliance_support_session_stop(&support->session, now_tick);
    (void)appliance_support_break_glass_set_available(
        &support->break_glass, false);
    support->state = APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED;
    support->last_status = APPLIANCE_PRODUCTION_SUPPORT_OK;
    return APPLIANCE_PRODUCTION_SUPPORT_OK;
}

const struct appliance_support_bundle *
appliance_production_support_bundle(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return NULL;
    }
    return &support->bundle;
}

const struct appliance_support_session *
appliance_production_support_session(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return NULL;
    }
    return &support->session;
}

const struct appliance_support_break_glass *
appliance_production_support_break_glass(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return NULL;
    }
    return &support->break_glass;
}

bool appliance_production_support_bundle_published(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return false;
    }
    return support->bundle_published;
}

uint64_t appliance_production_support_bundle_bytes(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return 0U;
    }
    return support->bundle_bytes;
}

uint32_t appliance_production_support_sections(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return 0U;
    }
    return support->bundle_sections;
}

uint32_t appliance_production_support_withheld(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return 0U;
    }
    return support->bundle_withheld;
}

uint32_t appliance_production_support_break_glass_effects(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return 0U;
    }
    return support->break_glass_effect_count;
}

enum appliance_production_support_state appliance_production_support_state(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_STATE_NONE;
    }
    return support->state;
}

enum appliance_production_support_status
appliance_production_support_last_status(
    const struct appliance_production_support *support)
{
    if (!appliance_production_support_initialized(support)) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }
    return support->last_status;
}

struct appliance_production_support_render_buffer {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_production_support_render_text(
    struct appliance_production_support_render_buffer *buffer,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (buffer->length + 1U < buffer->capacity) {
            buffer->data[buffer->length] = text[index];
            ++buffer->length;
        } else {
            buffer->overflow = true;
        }
        ++index;
    }
}

static void appliance_production_support_render_u64(
    struct appliance_production_support_render_buffer *buffer,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        appliance_production_support_render_text(buffer, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        appliance_production_support_render_text(buffer, single);
    }
}

enum appliance_production_support_status appliance_production_support_render(
    const struct appliance_production_support *support,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_production_support_render_buffer buffer;
    char staging[APPLIANCE_PRODUCTION_SUPPORT_RENDER_MAX];

    if (!appliance_production_support_initialized(support) ||
        output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_SUPPORT_NULL;
    }

    buffer.data = staging;
    buffer.capacity = sizeof(staging);
    buffer.length = 0U;
    buffer.overflow = false;

    appliance_production_support_render_text(&buffer,
                                             "support_plane v=1 state=");
    appliance_production_support_render_text(
        &buffer, appliance_production_support_state_string(support->state));
    appliance_production_support_render_text(&buffer, " bundle=");
    appliance_production_support_render_text(
        &buffer, support->bundle_published
                     ? appliance_support_bundle_state_string(
                           support->bundle.state)
                     : "NONE");
    appliance_production_support_render_text(&buffer, " sections=");
    appliance_production_support_render_u64(&buffer,
                                            support->bundle_sections);
    appliance_production_support_render_text(&buffer, " withheld=");
    appliance_production_support_render_u64(&buffer,
                                            support->bundle_withheld);
    appliance_production_support_render_text(&buffer, " bytes=");
    appliance_production_support_render_u64(&buffer,
                                            support->bundle_bytes);
    appliance_production_support_render_text(&buffer, " session=");
    appliance_production_support_render_text(
        &buffer, appliance_support_session_state_string(
                     support->session.state));
    appliance_production_support_render_text(&buffer, " session_active=");
    appliance_production_support_render_text(
        &buffer,
        appliance_support_session_active(&support->session, now_tick)
            ? "YES"
            : "NO");
    appliance_production_support_render_text(&buffer, " break_glass=");
    appliance_production_support_render_text(
        &buffer, appliance_support_break_glass_state_string(
                     support->break_glass.state));
    appliance_production_support_render_text(&buffer, " break_glass_armed=");
    appliance_production_support_render_text(
        &buffer,
        appliance_support_break_glass_armed(&support->break_glass,
                                            now_tick)
            ? "YES"
            : "NO");
    appliance_production_support_render_text(&buffer, " effects=");
    appliance_production_support_render_u64(
        &buffer, support->break_glass_effect_count);
    appliance_production_support_render_text(&buffer, " shell=FORBIDDEN");

    if (buffer.overflow || buffer.length + 1U > capacity) {
        *output_length = 0U;
        return APPLIANCE_PRODUCTION_SUPPORT_BUFFER_TOO_SMALL;
    }
    appliance_production_support_copy_bytes(
        (unsigned char *)output, (const unsigned char *)staging,
        buffer.length);
    output[buffer.length] = '\0';
    *output_length = buffer.length;
    return APPLIANCE_PRODUCTION_SUPPORT_OK;
}

const char *appliance_production_support_state_string(
    enum appliance_production_support_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_READY:
        return "READY";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_STOPPED:
        return "STOPPED";
    case APPLIANCE_PRODUCTION_SUPPORT_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_support_status_string(
    enum appliance_production_support_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_SUPPORT_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_SUPPORT_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_SUPPORT_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_SUPPORT_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_SUPPORT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_SUPPORT_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_SUPPORT_DENIED:
        return "DENIED";
    case APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_TOO_LARGE:
        return "BUNDLE_TOO_LARGE";
    case APPLIANCE_PRODUCTION_SUPPORT_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_production_support_schema_id(void)
{
    return APPLIANCE_PRODUCTION_SUPPORT_SCHEMA_ID;
}
