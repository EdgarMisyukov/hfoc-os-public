#include "appliance_acme_json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 3: the bounded JSON reader, implemented as one cursor
 * walk.  Every function here works on the caller's buffer and hands back
 * spans into it; nothing is copied, allocated or normalised, and every loop
 * is bounded by the declared length.
 */

struct appliance_acme_json_cursor {
    const char *text;
    size_t length;
    size_t offset;
};

static void appliance_acme_json_skip_space(
    struct appliance_acme_json_cursor *cursor)
{
    while (cursor->offset < cursor->length) {
        const char value = cursor->text[cursor->offset];

        if (value != ' ' && value != '\t' && value != '\n' &&
            value != '\r') {
            break;
        }
        cursor->offset += 1U;
    }
}

static bool appliance_acme_json_peek(
    const struct appliance_acme_json_cursor *cursor,
    char *value_out)
{
    if (cursor->offset >= cursor->length || value_out == NULL) {
        return false;
    }
    *value_out = cursor->text[cursor->offset];
    return true;
}

static bool appliance_acme_json_expect(
    struct appliance_acme_json_cursor *cursor,
    char expected)
{
    char value = '\0';

    appliance_acme_json_skip_space(cursor);
    if (!appliance_acme_json_peek(cursor, &value) || value != expected) {
        return false;
    }
    cursor->offset += 1U;
    return true;
}

/*
 * One string.  The content between the quotes is handed back as it appears in
 * the buffer; the escapes this reader accepts are the ones JSON defines for
 * ASCII text, and `\u` refuses (an ACME reply the item parses carries none,
 * and silently keeping a raw `\uXXXX` would hand a caller a name or a URL
 * that is not the one the server sent).
 */
static bool appliance_acme_json_read_string(
    struct appliance_acme_json_cursor *cursor,
    struct appliance_acme_json_value *value_out)
{
    size_t start;

    if (!appliance_acme_json_expect(cursor, '"')) {
        return false;
    }
    start = cursor->offset;
    while (cursor->offset < cursor->length) {
        const unsigned char value =
            (unsigned char)cursor->text[cursor->offset];

        if (value == (unsigned char)'"') {
            if (value_out != NULL) {
                value_out->kind = APPLIANCE_ACME_JSON_STRING;
                value_out->text = &cursor->text[start];
                value_out->length = cursor->offset - start;
            }
            cursor->offset += 1U;
            return true;
        }
        if (value == (unsigned char)'\\') {
            char escaped = '\0';

            cursor->offset += 1U;
            if (!appliance_acme_json_peek(cursor, &escaped)) {
                return false;
            }
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                cursor->offset += 1U;
                break;
            default:
                return false;
            }
            continue;
        }
        if (value < 0x20U) {
            return false;
        }
        cursor->offset += 1U;
    }
    return false;
}

static bool appliance_acme_json_skip_value(
    struct appliance_acme_json_cursor *cursor,
    uint32_t depth);

static bool appliance_acme_json_skip_members(
    struct appliance_acme_json_cursor *cursor,
    char closing,
    uint32_t depth)
{
    appliance_acme_json_skip_space(cursor);
    {
        char value = '\0';

        if (!appliance_acme_json_peek(cursor, &value)) {
            return false;
        }
        if (value == closing) {
            cursor->offset += 1U;
            return true;
        }
    }
    for (;;) {
        struct appliance_acme_json_value ignored;

        if (closing == '}') {
            if (!appliance_acme_json_read_string(cursor, &ignored) ||
                !appliance_acme_json_expect(cursor, ':')) {
                return false;
            }
        }
        if (!appliance_acme_json_skip_value(cursor, depth - 1U)) {
            return false;
        }
        appliance_acme_json_skip_space(cursor);
        {
            char value = '\0';

            if (!appliance_acme_json_peek(cursor, &value)) {
                return false;
            }
            if (value == closing) {
                cursor->offset += 1U;
                return true;
            }
            if (value != ',') {
                return false;
            }
            cursor->offset += 1U;
        }
    }
}

static bool appliance_acme_json_skip_literal(
    struct appliance_acme_json_cursor *cursor,
    const char *literal)
{
    const size_t length = 4U; /* "true", "null" */
    size_t index;

    if (literal[0] == 'f') {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (cursor->offset + index >= cursor->length ||
            cursor->text[cursor->offset + index] != literal[index]) {
            return false;
        }
    }
    cursor->offset += length;
    return true;
}

static bool appliance_acme_json_skip_number(
    struct appliance_acme_json_cursor *cursor)
{
    const size_t start = cursor->offset;

    if (cursor->offset < cursor->length &&
        cursor->text[cursor->offset] == '-') {
        cursor->offset += 1U;
    }
    while (cursor->offset < cursor->length) {
        const char value = cursor->text[cursor->offset];

        if ((value >= '0' && value <= '9') || value == '.' ||
            value == 'e' || value == 'E' || value == '+' || value == '-') {
            cursor->offset += 1U;
            continue;
        }
        break;
    }
    return cursor->offset != start;
}

static bool appliance_acme_json_skip_value(
    struct appliance_acme_json_cursor *cursor,
    uint32_t depth)
{
    char value = '\0';

    if (depth == 0U) {
        return false;
    }
    appliance_acme_json_skip_space(cursor);
    if (!appliance_acme_json_peek(cursor, &value)) {
        return false;
    }
    switch (value) {
    case '{':
        cursor->offset += 1U;
        return appliance_acme_json_skip_members(cursor, '}', depth);
    case '[':
        cursor->offset += 1U;
        return appliance_acme_json_skip_members(cursor, ']', depth);
    case '"':
        return appliance_acme_json_read_string(cursor, NULL);
    case 't':
        return appliance_acme_json_skip_literal(cursor, "true");
    case 'n':
        return appliance_acme_json_skip_literal(cursor, "null");
    default:
        return appliance_acme_json_skip_number(cursor);
    }
}

/*
 * The value of one member of the object the cursor stands in.  A name matches
 * only byte for byte, and the value is captured - string contents, the raw
 * span for an object or an array, the raw token for a number, boolean or null.
 */
static bool appliance_acme_json_find_member(
    struct appliance_acme_json_cursor *cursor,
    const char *name,
    size_t name_length,
    struct appliance_acme_json_value *value_out);

static bool appliance_acme_json_capture_value(
    struct appliance_acme_json_cursor *cursor,
    struct appliance_acme_json_value *value_out)
{
    size_t start;
    char value = '\0';

    appliance_acme_json_skip_space(cursor);
    start = cursor->offset;
    if (!appliance_acme_json_peek(cursor, &value)) {
        return false;
    }
    if (value == '"') {
        return appliance_acme_json_read_string(cursor, value_out);
    }
    if (!appliance_acme_json_skip_value(
            cursor, APPLIANCE_ACME_JSON_DEPTH_MAX)) {
        return false;
    }
    if (value_out != NULL) {
        value_out->text = &cursor->text[start];
        value_out->length = cursor->offset - start;
        switch (value) {
        case '{':
            value_out->kind = APPLIANCE_ACME_JSON_OBJECT;
            break;
        case '[':
            value_out->kind = APPLIANCE_ACME_JSON_ARRAY;
            break;
        case 't':
        case 'f':
            value_out->kind = APPLIANCE_ACME_JSON_BOOL;
            break;
        case 'n':
            value_out->kind = APPLIANCE_ACME_JSON_NULL;
            break;
        default:
            value_out->kind = APPLIANCE_ACME_JSON_NUMBER;
            break;
        }
    }
    return true;
}

static bool appliance_acme_json_find_member(
    struct appliance_acme_json_cursor *cursor,
    const char *name,
    size_t name_length,
    struct appliance_acme_json_value *value_out)
{
    char closing = '\0';

    appliance_acme_json_skip_space(cursor);
    if (!appliance_acme_json_peek(cursor, &closing) || closing != '{') {
        return false;
    }
    cursor->offset += 1U;
    appliance_acme_json_skip_space(cursor);
    {
        char value = '\0';

        if (!appliance_acme_json_peek(cursor, &value)) {
            return false;
        }
        if (value == '}') {
            cursor->offset += 1U;
            return false;
        }
    }
    for (;;) {
        struct appliance_acme_json_value member_name;
        bool matches;

        if (!appliance_acme_json_read_string(cursor, &member_name) ||
            !appliance_acme_json_expect(cursor, ':')) {
            return false;
        }
        matches = member_name.length == name_length &&
                  name != NULL &&
                  memcmp(member_name.text, name, name_length) == 0;
        if (matches) {
            return appliance_acme_json_capture_value(cursor, value_out);
        }
        if (!appliance_acme_json_skip_value(
                cursor, APPLIANCE_ACME_JSON_DEPTH_MAX)) {
            return false;
        }
        appliance_acme_json_skip_space(cursor);
        {
            char value = '\0';

            if (!appliance_acme_json_peek(cursor, &value)) {
                return false;
            }
            if (value == '}') {
                cursor->offset += 1U;
                return false;
            }
            if (value != ',') {
                return false;
            }
            cursor->offset += 1U;
        }
    }
}

static bool appliance_acme_json_name_length(
    const char *name,
    size_t *length_out)
{
    size_t length = 0U;

    if (name == NULL || length_out == NULL) {
        return false;
    }
    while (name[length] != '\0') {
        if (length == 64U) {
            return false;
        }
        ++length;
    }
    if (length == 0U) {
        return false;
    }
    *length_out = length;
    return true;
}

bool appliance_acme_json_is_object(const char *json, size_t length)
{
    struct appliance_acme_json_cursor cursor;

    if (json == NULL || length == 0U) {
        return false;
    }
    cursor.text = json;
    cursor.length = length;
    cursor.offset = 0U;
    if (!appliance_acme_json_expect(&cursor, '{')) {
        return false;
    }
    cursor.offset -= 1U;
    if (!appliance_acme_json_skip_value(
            &cursor, APPLIANCE_ACME_JSON_DEPTH_MAX)) {
        return false;
    }
    appliance_acme_json_skip_space(&cursor);
    return cursor.offset == cursor.length;
}

bool appliance_acme_json_member(
    const char *json,
    size_t length,
    const char *name,
    struct appliance_acme_json_value *value_out)
{
    struct appliance_acme_json_cursor cursor;
    size_t name_length = 0U;

    if (json == NULL || length == 0U ||
        !appliance_acme_json_name_length(name, &name_length)) {
        return false;
    }
    cursor.text = json;
    cursor.length = length;
    cursor.offset = 0U;
    return appliance_acme_json_find_member(
        &cursor, name, name_length, value_out);
}

bool appliance_acme_json_array_count(
    const char *json,
    size_t length,
    const char *name,
    uint32_t *count_out)
{
    struct appliance_acme_json_value array;
    struct appliance_acme_json_cursor cursor;
    uint32_t count = 0U;

    if (count_out == NULL ||
        !appliance_acme_json_member(json, length, name, &array) ||
        array.kind != APPLIANCE_ACME_JSON_ARRAY) {
        return false;
    }
    cursor.text = array.text;
    cursor.length = array.length;
    cursor.offset = 0U;
    if (!appliance_acme_json_expect(&cursor, '[')) {
        return false;
    }
    appliance_acme_json_skip_space(&cursor);
    {
        char value = '\0';

        if (!appliance_acme_json_peek(&cursor, &value)) {
            return false;
        }
        if (value == ']') {
            *count_out = 0U;
            return true;
        }
    }
    for (;;) {
        if (count == UINT32_MAX) {
            return false;
        }
        if (!appliance_acme_json_skip_value(
                &cursor, APPLIANCE_ACME_JSON_DEPTH_MAX)) {
            return false;
        }
        count += 1U;
        appliance_acme_json_skip_space(&cursor);
        {
            char value = '\0';

            if (!appliance_acme_json_peek(&cursor, &value)) {
                return false;
            }
            if (value == ']') {
                *count_out = count;
                return true;
            }
            if (value != ',') {
                return false;
            }
            cursor.offset += 1U;
        }
    }
}

bool appliance_acme_json_array_element(
    const char *json,
    size_t length,
    const char *name,
    uint32_t index,
    struct appliance_acme_json_value *value_out)
{
    struct appliance_acme_json_value array;
    struct appliance_acme_json_cursor cursor;
    uint32_t position = 0U;

    if (!appliance_acme_json_member(json, length, name, &array) ||
        array.kind != APPLIANCE_ACME_JSON_ARRAY) {
        return false;
    }
    cursor.text = array.text;
    cursor.length = array.length;
    cursor.offset = 0U;
    if (!appliance_acme_json_expect(&cursor, '[')) {
        return false;
    }
    appliance_acme_json_skip_space(&cursor);
    {
        char value = '\0';

        if (!appliance_acme_json_peek(&cursor, &value)) {
            return false;
        }
        if (value == ']') {
            return false;
        }
    }
    for (;;) {
        if (position == index) {
            return appliance_acme_json_capture_value(&cursor, value_out);
        }
        if (!appliance_acme_json_skip_value(
                &cursor, APPLIANCE_ACME_JSON_DEPTH_MAX)) {
            return false;
        }
        position += 1U;
        appliance_acme_json_skip_space(&cursor);
        {
            char value = '\0';

            if (!appliance_acme_json_peek(&cursor, &value)) {
                return false;
            }
            if (value == ']') {
                return false;
            }
            if (value != ',') {
                return false;
            }
            cursor.offset += 1U;
        }
    }
}

bool appliance_acme_json_object_member(
    const struct appliance_acme_json_value *object,
    const char *name,
    struct appliance_acme_json_value *value_out)
{
    struct appliance_acme_json_cursor cursor;
    size_t name_length = 0U;

    if (object == NULL || object->kind != APPLIANCE_ACME_JSON_OBJECT ||
        object->text == NULL ||
        !appliance_acme_json_name_length(name, &name_length)) {
        return false;
    }
    cursor.text = object->text;
    cursor.length = object->length;
    cursor.offset = 0U;
    return appliance_acme_json_find_member(
        &cursor, name, name_length, value_out);
}
