#ifndef HFOC_APPLIANCE_PANEL_FORM_H
#define HFOC_APPLIANCE_PANEL_FORM_H

/*
 * ITEM-515: the bodies the panel's mutation forms submit, read by the same
 * code the serving path runs.
 *
 * ITEM-513 fixed the class once for the System page: the panel's tests posted
 * hand-written bodies while a browser submits every control the page draws,
 * empty ones included, so a form could be drawn that its own stage could never
 * accept.  The network form was the second instance and it reached the
 * hardware: `mode=dhcp` was refused whenever the address, prefix, gateway and
 * DNS inputs arrived empty, which is exactly what a browser sends, and the
 * refusal never named the field that failed.
 *
 * This module owns the one field reader and the form grammars, so a host proof
 * can render the page, derive the body from the drawn controls and drive the
 * same parser the stage drives.  Every refusal here is named: the HTTP layer
 * quotes the name instead of one generic `REFUSED`, as the installer's own
 * refusals already do.
 */

#include "appliance_network_config.h"
#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One `application/x-www-form-urlencoded` field.  The first occurrence wins and
 * a duplicated field is ignored, exactly as the panel's sign-in and language
 * bodies are read: a body that carries two values for one name must not be
 * resolved by guessing.
 */
bool appliance_panel_form_field(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    const uint8_t **value_out,
    uint32_t *value_length_out);

/* True when the field is present with the exact value `1`. */
bool appliance_panel_form_flag(
    const uint8_t *body,
    uint32_t body_length,
    const char *name);

/*
 * Decode one form value: `+` is a space and `%XX` is one byte.  A truncated or
 * non-hex escape and an output that does not fit are refused instead of
 * guessed; the output is NUL-terminated within the caller's buffer.
 */
bool appliance_panel_form_decode(
    const uint8_t *value,
    uint32_t value_length,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length);

/* `appliance_panel_form_field` followed by `appliance_panel_form_decode`. */
bool appliance_panel_form_text(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length);

/*
 * The named refusals of the panel's mutation forms.  The strings are the
 * contract the panel's refusal document and the QEMU proof pin; the installer
 * names its own refusals the same way (`NO_RELEASABLE_PARTITION`).
 */
enum appliance_panel_form_status {
    APPLIANCE_PANEL_FORM_OK = 0,
    APPLIANCE_PANEL_FORM_EMPTY_BODY = 1,
    APPLIANCE_PANEL_FORM_MODE_MISSING = 2,
    APPLIANCE_PANEL_FORM_MODE_UNKNOWN = 3,
    APPLIANCE_PANEL_FORM_EXTRA_FIELD = 4,
    APPLIANCE_PANEL_FORM_ADDRESS_MISSING = 5,
    APPLIANCE_PANEL_FORM_PREFIX_MISSING = 6,
    APPLIANCE_PANEL_FORM_GATEWAY_MISSING = 7,
    APPLIANCE_PANEL_FORM_DNS_MISSING = 8,
    APPLIANCE_PANEL_FORM_ADDRESS_INVALID = 9,
    APPLIANCE_PANEL_FORM_PREFIX_INVALID = 10,
    APPLIANCE_PANEL_FORM_GATEWAY_INVALID = 11,
    APPLIANCE_PANEL_FORM_DNS_INVALID = 12,
    APPLIANCE_PANEL_FORM_VALUE_INVALID = 13,
    APPLIANCE_PANEL_FORM_FIELD_MISSING = 14,
    APPLIANCE_PANEL_FORM_FIELD_DECODE = 15,
    APPLIANCE_PANEL_FORM_FIELD_TOO_LARGE = 16,
    APPLIANCE_PANEL_FORM_ACTION_UNKNOWN = 17,
    APPLIANCE_PANEL_FORM_HOSTNAME_INVALID = 18,
    APPLIANCE_PANEL_FORM_LANGUAGE_INVALID = 19,
};

const char *appliance_panel_form_status_string(
    enum appliance_panel_form_status status);

/*
 * The Network page's stage form.  A browser submits every drawn field, so
 * `mode=dhcp` arrives with four empty optional fields: an empty value is the
 * absence of a value, and a non-empty one on the DHCP branch is the stray
 * field `EXTRA_FIELD` names.  `mode=static` requires all four, and each missing
 * or malformed value has its own name.
 */
struct appliance_panel_form_network {
    enum appliance_network_config_mode mode;
    struct network_ipv4_config static_config;
};

enum appliance_panel_form_status appliance_panel_form_network_parse(
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_form_network *parsed_out);

/*
 * The System page's stage forms.  Each form draws exactly one field, and the
 * body is that one `name=value` pair; the field reader is the prefix form the
 * stage has always used, so a body that carries two fields or a second `=` is
 * refused rather than reordered.
 */
enum appliance_panel_form_system_field {
    APPLIANCE_PANEL_FORM_SYSTEM_NONE = 0,
    APPLIANCE_PANEL_FORM_SYSTEM_HOSTNAME = 1,
    APPLIANCE_PANEL_FORM_SYSTEM_LANGUAGE = 2,
};

enum appliance_panel_form_status appliance_panel_form_system_parse(
    const uint8_t *body,
    uint32_t body_length,
    enum appliance_panel_form_system_field *field_out,
    const uint8_t **value_out,
    uint32_t *value_length_out);

/*
 * The one-field action forms: ACME (`enable`, `disable`, `request`) and the
 * certificate page (`import`, `delete`, `default`).  The action is decoded
 * into the caller's buffer; the caller decides which actions it serves, so an
 * unknown action is named the same way for both pages.
 */
enum appliance_panel_form_status appliance_panel_form_action_parse(
    const uint8_t *body,
    uint32_t body_length,
    uint8_t *action_out,
    uint32_t action_capacity,
    uint32_t *action_length_out);

#endif
