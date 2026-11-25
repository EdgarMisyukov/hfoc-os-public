#ifndef HFOC_APPLIANCE_ACME_JSON_H
#define HFOC_APPLIANCE_ACME_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 3: the bounded JSON reader the ACME client role parses
 * replies with.
 *
 * The kernel carries no JSON library and the profile carries no filesystem, so
 * this module owns exactly what the item needs and nothing more: walk one
 * object, find a named member, walk an array and read its elements.  It is a
 * reader, not a document model - it never copies and never allocates, it hands
 * back borrowed spans into the caller's own buffer, and every walk is bounded
 * by the buffer length the caller declares.
 *
 * The reader refuses rather than guesses:
 *
 *   * a document that is not one object, a member without a name, a missing
 *     colon or comma, an unterminated string, a control character inside a
 *     string, an escape the reader does not know, a number that is not a
 *     number and a trailing non-space byte are all refusals;
 *   * strings are compared byte for byte; an escape in a *name* refuses,
 *     because an ACME member name is ASCII and an escaped name is not one this
 *     reader promises to understand;
 *   * nesting is bounded (`APPLIANCE_ACME_JSON_DEPTH_MAX`), so a hostile reply
 *     cannot make the walk unbounded.
 *
 * Only what the item parses is modelled: objects, arrays, strings, booleans
 * and null.  Numbers are recognised so a document containing them can be
 * skipped over, and their raw text is handed back for a caller that needs it.
 */

#define APPLIANCE_ACME_JSON_DEPTH_MAX 8U

enum appliance_acme_json_kind {
    APPLIANCE_ACME_JSON_NONE = 0,
    APPLIANCE_ACME_JSON_OBJECT = 1,
    APPLIANCE_ACME_JSON_ARRAY = 2,
    APPLIANCE_ACME_JSON_STRING = 3,
    APPLIANCE_ACME_JSON_NUMBER = 4,
    APPLIANCE_ACME_JSON_BOOL = 5,
    APPLIANCE_ACME_JSON_NULL = 6,
};

struct appliance_acme_json_value {
    enum appliance_acme_json_kind kind;
    /* For STRING: the content between the quotes.  For NUMBER/BOOL/NULL: the
     * raw token.  For OBJECT/ARRAY: the raw span including its brackets. */
    const char *text;
    size_t length;
};

/* True when the whole buffer is one JSON object (spaces around it allowed). */
bool appliance_acme_json_is_object(const char *json, size_t length);

/* The named member of the top-level object.  False when absent or malformed. */
bool appliance_acme_json_member(
    const char *json,
    size_t length,
    const char *name,
    struct appliance_acme_json_value *value_out);

/* How many elements the named top-level array carries. */
bool appliance_acme_json_array_count(
    const char *json,
    size_t length,
    const char *name,
    uint32_t *count_out);

/* The element at `index` of the named top-level array. */
bool appliance_acme_json_array_element(
    const char *json,
    size_t length,
    const char *name,
    uint32_t index,
    struct appliance_acme_json_value *value_out);

/* The named member of an OBJECT value already in hand (one nesting step). */
bool appliance_acme_json_object_member(
    const struct appliance_acme_json_value *object,
    const char *name,
    struct appliance_acme_json_value *value_out);

#endif
