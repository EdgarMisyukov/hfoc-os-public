#include "appliance_support_break_glass.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_support_break_glass_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_support_break_glass_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_support_break_glass_state_valid(
    enum appliance_support_break_glass_state state)
{
    return state >= APPLIANCE_SUPPORT_BREAK_GLASS_STATE_NONE &&
           state <= APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED;
}

static bool appliance_support_break_glass_status_valid(
    enum appliance_support_break_glass_status status)
{
    return status >= APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
           status <= APPLIANCE_SUPPORT_BREAK_GLASS_BUFFER_TOO_SMALL;
}

static bool appliance_support_break_glass_origin_valid(
    enum appliance_support_break_glass_origin origin)
{
    return origin >= APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NONE &&
           origin <= APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK;
}

static bool appliance_support_break_glass_initialized(
    const struct appliance_support_break_glass *gate)
{
    return gate != NULL &&
           gate->initialized &&
           gate->version == APPLIANCE_SUPPORT_BREAK_GLASS_VERSION &&
           appliance_support_break_glass_state_valid(gate->state) &&
           appliance_support_break_glass_status_valid(gate->last_status);
}

static bool appliance_support_break_glass_actions_declared(uint32_t actions)
{
    return actions != APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE &&
           (actions &
            ~APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED) == 0U;
}

static void appliance_support_break_glass_audit(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_event event,
    enum appliance_support_break_glass_status outcome,
    uint32_t origin,
    uint32_t principal_id,
    uint32_t actions,
    uint64_t token,
    uint64_t tick)
{
    struct appliance_support_break_glass_audit_entry entry;
    uint32_t index;

    appliance_support_break_glass_zero_bytes((unsigned char *)&entry,
                                             sizeof(entry));
    entry.tick = tick;
    entry.token = token;
    entry.event = (uint32_t)event;
    entry.outcome = (uint32_t)outcome;
    entry.principal_id = principal_id;
    entry.actions = actions;
    entry.origin = origin;

    if (gate->audit_count < APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX) {
        gate->audit[gate->audit_count] = entry;
        ++gate->audit_count;
        return;
    }
    for (index = 1U; index < APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX;
         ++index) {
        gate->audit[index - 1U] = gate->audit[index];
    }
    gate->audit[APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX - 1U] = entry;
    ++gate->audit_dropped;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_init(
    struct appliance_support_break_glass *gate)
{
    if (gate == NULL) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    appliance_support_break_glass_zero_bytes((unsigned char *)gate,
                                             sizeof(*gate));
    gate->initialized = true;
    gate->version = APPLIANCE_SUPPORT_BREAK_GLASS_VERSION;
    gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY;
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    gate->available = true;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_set_available(
    struct appliance_support_break_glass *gate,
    bool available)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    if (available) {
        gate->available = true;
        if (gate->state ==
            APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE) {
            gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY;
            gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
        }
        return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    }

    gate->available = false;
    if (gate->state == APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED) {
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_UNAVAILABLE,
            APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE,
            (uint32_t)APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NONE, 0U,
            gate->pending_actions, gate->pending_token, 0U);
        ++gate->unavailable_count;
    }
    gate->pending_token = 0U;
    gate->pending_actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE;
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_arm(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint32_t principal_id,
    uint32_t actions,
    uint64_t now_tick,
    uint64_t *token_out)
{
    if (!appliance_support_break_glass_initialized(gate) ||
        !appliance_support_break_glass_origin_valid(origin) ||
        token_out == NULL) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    if (!gate->available ||
        gate->state == APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE;
        return APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE;
    }
    if (origin != APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE) {
        gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED;
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
        ++gate->deny_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY,
            APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
            (uint32_t)origin, principal_id, actions, 0U, now_tick);
        return APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
    }
    if (principal_id == 0U) {
        gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED;
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_DENIED;
        ++gate->deny_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY,
            APPLIANCE_SUPPORT_BREAK_GLASS_DENIED, (uint32_t)origin,
            principal_id, actions, 0U, now_tick);
        return APPLIANCE_SUPPORT_BREAK_GLASS_DENIED;
    }
    if (!appliance_support_break_glass_actions_declared(actions)) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS;
        return APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS;
    }
    if (gate->state == APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED;
        return APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED;
    }

    ++gate->token_sequence;
    if (gate->token_sequence == 0U) {
        ++gate->token_sequence;
    }
    gate->pending_token = gate->token_sequence;
    gate->pending_actions = actions;
    gate->principal_id = principal_id;
    gate->arm_tick = now_tick;
    gate->expiry_tick =
        now_tick + APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS;
    gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED;
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    ++gate->arm_count;
    appliance_support_break_glass_audit(
        gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_ARM,
        APPLIANCE_SUPPORT_BREAK_GLASS_OK, (uint32_t)origin,
        principal_id, actions, gate->pending_token, now_tick);
    *token_out = gate->pending_token;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_apply(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint64_t token,
    uint64_t now_tick,
    uint32_t *actions_out)
{
    if (!appliance_support_break_glass_initialized(gate) ||
        !appliance_support_break_glass_origin_valid(origin) ||
        actions_out == NULL) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    *actions_out = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    if (gate->state != APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM;
        return APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM;
    }
    if (origin != APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
        ++gate->deny_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY,
            APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
            (uint32_t)origin, gate->principal_id,
            gate->pending_actions, token, now_tick);
        return APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
    }
    if (now_tick >= gate->expiry_tick) {
        gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_EXPIRED;
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED;
        ++gate->expire_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_EXPIRE,
            APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED,
            (uint32_t)origin, gate->principal_id,
            gate->pending_actions, gate->pending_token, now_tick);
        gate->pending_token = 0U;
        gate->pending_actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
        return APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED;
    }
    if (token == 0U || token != gate->pending_token) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN;
        ++gate->deny_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY,
            APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN, (uint32_t)origin,
            gate->principal_id, gate->pending_actions, token, now_tick);
        return APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN;
    }

    gate->applied_actions = gate->pending_actions;
    gate->applied_tick = now_tick;
    gate->applied_principal = gate->principal_id;
    *actions_out = gate->applied_actions;
    gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_APPLIED;
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    ++gate->apply_count;
    appliance_support_break_glass_audit(
        gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_APPLY,
        APPLIANCE_SUPPORT_BREAK_GLASS_OK, (uint32_t)origin,
        gate->applied_principal, gate->applied_actions,
        gate->pending_token, now_tick);
    gate->pending_token = 0U;
    gate->pending_actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_cancel(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint64_t now_tick)
{
    if (!appliance_support_break_glass_initialized(gate) ||
        !appliance_support_break_glass_origin_valid(origin)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    if (gate->state != APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM;
        return APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM;
    }
    if (origin != APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
        ++gate->deny_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY,
            APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
            (uint32_t)origin, gate->principal_id,
            gate->pending_actions, gate->pending_token, now_tick);
        return APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED;
    }

    gate->pending_token = 0U;
    gate->pending_actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY;
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    ++gate->cancel_count;
    appliance_support_break_glass_audit(
        gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_CANCEL,
        APPLIANCE_SUPPORT_BREAK_GLASS_OK, (uint32_t)origin,
        gate->principal_id, APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE,
        0U, now_tick);
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_refresh(
    struct appliance_support_break_glass *gate,
    uint64_t now_tick)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    if (gate->state != APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED) {
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
        return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    }
    if (now_tick >= gate->expiry_tick) {
        gate->state = APPLIANCE_SUPPORT_BREAK_GLASS_STATE_EXPIRED;
        gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED;
        ++gate->expire_count;
        appliance_support_break_glass_audit(
            gate, APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_EXPIRE,
            APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED,
            (uint32_t)APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NONE,
            gate->principal_id, gate->pending_actions,
            gate->pending_token, now_tick);
        gate->pending_token = 0U;
        gate->pending_actions = APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
        return APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED;
    }
    gate->last_status = APPLIANCE_SUPPORT_BREAK_GLASS_OK;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

bool appliance_support_break_glass_armed(
    const struct appliance_support_break_glass *gate,
    uint64_t now_tick)
{
    return appliance_support_break_glass_initialized(gate) &&
           gate->available &&
           gate->state == APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED &&
           now_tick < gate->expiry_tick;
}

bool appliance_support_break_glass_shell_required(void)
{
    return false;
}

uint32_t appliance_support_break_glass_shell_action(void)
{
    return APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_SHELL;
}

uint32_t appliance_support_break_glass_declared_actions(void)
{
    return APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED;
}

uint32_t appliance_support_break_glass_applied_actions(
    const struct appliance_support_break_glass *gate)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE;
    }
    return gate->applied_actions;
}

enum appliance_support_break_glass_state
appliance_support_break_glass_state(
    const struct appliance_support_break_glass *gate)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_STATE_NONE;
    }
    return gate->state;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_last_status(
    const struct appliance_support_break_glass *gate)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    return gate->last_status;
}

uint32_t appliance_support_break_glass_audit_count(
    const struct appliance_support_break_glass *gate)
{
    if (!appliance_support_break_glass_initialized(gate)) {
        return 0U;
    }
    return gate->audit_count;
}

enum appliance_support_break_glass_status
appliance_support_break_glass_audit_entry(
    const struct appliance_support_break_glass *gate,
    uint32_t index,
    struct appliance_support_break_glass_audit_entry *entry_out)
{
    if (!appliance_support_break_glass_initialized(gate) ||
        entry_out == NULL) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    if (index >= gate->audit_count) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_BAD_STATE;
    }
    *entry_out = gate->audit[index];
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

struct appliance_support_break_glass_render_buffer {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_support_break_glass_render_text(
    struct appliance_support_break_glass_render_buffer *buffer,
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

static void appliance_support_break_glass_render_u64(
    struct appliance_support_break_glass_render_buffer *buffer,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        appliance_support_break_glass_render_text(buffer, "0");
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
        appliance_support_break_glass_render_text(buffer, single);
    }
}

static void appliance_support_break_glass_render_actions(
    struct appliance_support_break_glass_render_buffer *buffer,
    uint32_t actions)
{
    bool first = true;

    if (actions == APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE) {
        appliance_support_break_glass_render_text(buffer, "NONE");
        return;
    }
    if ((actions &
         APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY) !=
        0U) {
        appliance_support_break_glass_render_text(
            buffer, "RESTORE_MANAGEMENT_LOCAL_ONLY");
        first = false;
    }
    if ((actions &
         APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT) !=
        0U) {
        if (!first) {
            appliance_support_break_glass_render_text(buffer, "|");
        }
        appliance_support_break_glass_render_text(
            buffer, "REVOKE_REMOTE_SUPPORT");
    }
}

enum appliance_support_break_glass_status
appliance_support_break_glass_render(
    const struct appliance_support_break_glass *gate,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_support_break_glass_render_buffer buffer;
    char staging[APPLIANCE_SUPPORT_BREAK_GLASS_RENDER_MAX];
    bool armed;
    uint64_t remaining = 0U;

    if (!appliance_support_break_glass_initialized(gate) ||
        output == NULL || output_length == NULL) {
        return APPLIANCE_SUPPORT_BREAK_GLASS_NULL;
    }
    armed = appliance_support_break_glass_armed(gate, now_tick);
    if (armed && gate->expiry_tick > now_tick) {
        remaining = gate->expiry_tick - now_tick;
    }

    buffer.data = staging;
    buffer.capacity = sizeof(staging);
    buffer.length = 0U;
    buffer.overflow = false;

    appliance_support_break_glass_render_text(&buffer,
                                              "break_glass v=1 state=");
    appliance_support_break_glass_render_text(
        &buffer,
        appliance_support_break_glass_state_string(gate->state));
    appliance_support_break_glass_render_text(&buffer, " available=");
    appliance_support_break_glass_render_text(
        &buffer, gate->available ? "YES" : "NO");
    appliance_support_break_glass_render_text(&buffer, " armed=");
    appliance_support_break_glass_render_text(&buffer,
                                              armed ? "YES" : "NO");
    appliance_support_break_glass_render_text(&buffer, " actions=");
    appliance_support_break_glass_render_actions(
        &buffer,
        armed ? gate->pending_actions
              : APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE);
    appliance_support_break_glass_render_text(&buffer, " remaining=");
    appliance_support_break_glass_render_u64(&buffer, remaining);
    appliance_support_break_glass_render_text(&buffer, " applied=");
    appliance_support_break_glass_render_actions(
        &buffer, gate->applied_actions);
    appliance_support_break_glass_render_text(&buffer,
                                              " shell=FORBIDDEN");

    if (buffer.overflow || buffer.length + 1U > capacity) {
        *output_length = 0U;
        return APPLIANCE_SUPPORT_BREAK_GLASS_BUFFER_TOO_SMALL;
    }
    appliance_support_break_glass_copy_bytes(
        (unsigned char *)output, (const unsigned char *)staging,
        buffer.length);
    output[buffer.length] = '\0';
    *output_length = buffer.length;
    return APPLIANCE_SUPPORT_BREAK_GLASS_OK;
}

const char *appliance_support_break_glass_state_string(
    enum appliance_support_break_glass_state state)
{
    switch (state) {
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY:
        return "STANDBY";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED:
        return "ARMED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_APPLIED:
        return "APPLIED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED:
        return "DENIED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_break_glass_status_string(
    enum appliance_support_break_glass_status status)
{
    switch (status) {
    case APPLIANCE_SUPPORT_BREAK_GLASS_OK:
        return "OK";
    case APPLIANCE_SUPPORT_BREAK_GLASS_NULL:
        return "NULL";
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED:
        return "REMOTE_REFUSED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_DENIED:
        return "DENIED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM:
        return "NO_ARM";
    case APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED:
        return "ALREADY_ARMED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN:
        return "BAD_TOKEN";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS:
        return "BAD_ACTIONS";
    case APPLIANCE_SUPPORT_BREAK_GLASS_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_break_glass_event_string(
    enum appliance_support_break_glass_event event)
{
    switch (event) {
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_ARM:
        return "ARM";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_APPLY:
        return "APPLY";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_CANCEL:
        return "CANCEL";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY:
        return "DENY";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_EXPIRE:
        return "EXPIRE";
    case APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_break_glass_action_string(uint32_t action)
{
    switch (action) {
    case APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY:
        return "RESTORE_MANAGEMENT_LOCAL_ONLY";
    case APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT:
        return "REVOKE_REMOTE_SUPPORT";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_break_glass_schema_id(void)
{
    return APPLIANCE_SUPPORT_BREAK_GLASS_SCHEMA_ID;
}
