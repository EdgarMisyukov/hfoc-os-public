/*
 * ITEM-509 increment 1: host proof of the one string catalog.
 *
 * The proof owns nothing but the catalog: it walks every entry and refuses an
 * empty key, an empty English or Russian value, a duplicate key, a value that
 * is not valid UTF-8, an unknown key that resolves, and a translation that
 * silently falls back to English.  It then drives the bounded UTF-8 reader on
 * valid, truncated, overlong, surrogate and out-of-range sequences and proves
 * that the Russian code-point inventory is sorted, unique, complete with
 * respect to the catalog and refuses to answer with a partial set.
 */

#include "appliance_language.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        ++failures; \
    } else { \
        printf("PASS: "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static bool text_is_empty(const char *text)
{
    return text == NULL || text[0] == '\0';
}

static bool value_has_multibyte(const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if ((unsigned char)text[index] >= 0x80U) {
            return true;
        }
        ++index;
    }
    return false;
}

static bool inventory_has(const uint32_t *codepoints, size_t count,
                          uint32_t codepoint)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (codepoints[index] == codepoint) {
            return true;
        }
    }
    return false;
}

static void check_catalog_shape(void)
{
    const size_t count = appliance_language_catalog_count();
    size_t index;
    size_t duplicates = 0U;

    CHECK(count > 0U, "catalog has entries (%zu)", count);
    /*
     * ITEM-509 increment 13 raised the ceiling from 256 to 384: the installer
     * screens add 75 entries (289 measured), increment 19 adds the install
     * banner title (290 measured), increment 20 adds the shared credential
     * conversation (304 measured), increment 21 the console's local-menu lines
     * (308 measured), increment 22 the console's field labels (321 measured),
     * increment 23 the console's remaining operator prose (325 measured),
     * increment 24 the power refusal and the busy account conversation
     * (327 measured), increment 25 the panel Help index sentence and the way
     * back (330 measured), ITEM-516 the management gate's console vocabulary,
     * its warning and its panel page (401 measured),
     * and the rest of 509.7d still has the console responses to
     * add.  The ceiling is a proof bound, not a product limit -
     * the catalog stays a static table of `const char *` rows either way - so
     * it moves deliberately, with the count recorded here and in the slice.
     * ITEM-647 raised it to 448 with 432 measured: nineteen field labels and
     * the one confirmation-code note are now shared by every panel form.
     */
    CHECK(count <= 448U, "catalog stays a small table (%zu)", count);
    for (index = 0U; index < count; ++index) {
        const char *key = appliance_language_catalog_key(index);
        const char *en = appliance_language_catalog_value(
            index, APPLIANCE_LANGUAGE_EN);
        const char *ru = appliance_language_catalog_value(
            index, APPLIANCE_LANGUAGE_RU);
        size_t other;

        CHECK(!text_is_empty(key), "entry %zu has a key", index);
        CHECK(!text_is_empty(en), "entry %zu has English text", index);
        CHECK(!text_is_empty(ru), "entry %zu has Russian text", index);
        CHECK(key != NULL && strchr(key, ' ') == NULL,
              "entry %zu key %s has no spaces", index,
              key == NULL ? "(null)" : key);
        for (other = 0U; other < index; ++other) {
            const char *other_key = appliance_language_catalog_key(other);

            if (key != NULL && other_key != NULL &&
                strcmp(key, other_key) == 0) {
                ++duplicates;
            }
        }
    }
    CHECK(duplicates == 0U, "all %zu catalog keys are unique", count);
}

static void check_lookup(void)
{
    const size_t count = appliance_language_catalog_count();
    size_t index;
    bool saw_multibyte = false;
    bool saw_lowercase = false;
    bool saw_uppercase = false;

    for (index = 0U; index < count; ++index) {
        const char *key = appliance_language_catalog_key(index);
        const char *en = appliance_language_catalog_value(
            index, APPLIANCE_LANGUAGE_EN);
        const char *ru = appliance_language_catalog_value(
            index, APPLIANCE_LANGUAGE_RU);

        CHECK(appliance_language_string(APPLIANCE_LANGUAGE_EN, key) == en,
              "lookup en %s", key);
        CHECK(appliance_language_string(APPLIANCE_LANGUAGE_RU, key) == ru,
              "lookup ru %s", key);
        if (en == NULL || ru == NULL) {
            continue;
        }
        if (value_has_multibyte(ru)) {
            saw_multibyte = true;
        }
        if (strstr(ru, "О") != NULL || strstr(ru, "Я") != NULL ||
            strstr(ru, "С") != NULL || strstr(ru, "Д") != NULL) {
            saw_uppercase = true;
        }
        if (strstr(ru, "о") != NULL || strstr(ru, "я") != NULL ||
            strstr(ru, "е") != NULL || strstr(ru, "и") != NULL) {
            saw_lowercase = true;
        }
        CHECK(strcmp(en, ru) != 0 || !value_has_multibyte(ru),
              "entry %s does not answer English for Russian", key);
    }
    CHECK(saw_multibyte, "the Russian catalog carries Cyrillic");
    CHECK(saw_uppercase, "the Russian catalog exercises uppercase letters");
    CHECK(saw_lowercase, "the Russian catalog exercises lowercase letters");
    CHECK(appliance_language_string(APPLIANCE_LANGUAGE_EN, "no.such.key") ==
              NULL,
          "an unknown key resolves to nothing");
    CHECK(appliance_language_string(APPLIANCE_LANGUAGE_RU, "no.such.key") ==
              NULL,
          "an unknown key resolves to nothing in Russian too");
    CHECK(appliance_language_string(APPLIANCE_LANGUAGE_EN, "") == NULL,
          "an empty key resolves to nothing");
    CHECK(appliance_language_string(APPLIANCE_LANGUAGE_EN, NULL) == NULL,
          "a NULL key resolves to nothing");
    CHECK(appliance_language_string(APPLIANCE_LANGUAGE_COUNT,
                                    "help.title") == NULL,
          "an out-of-range language resolves to nothing");
    CHECK(appliance_language_catalog_key(count) == NULL,
          "the catalog key walk ends at the count");
    CHECK(appliance_language_catalog_value(count, APPLIANCE_LANGUAGE_EN) ==
              NULL,
          "the catalog value walk ends at the count");
    CHECK(strcmp(appliance_language_code(APPLIANCE_LANGUAGE_EN), "en") == 0,
          "the English code is en");
    CHECK(strcmp(appliance_language_code(APPLIANCE_LANGUAGE_RU), "ru") == 0,
          "the Russian code is ru");
    CHECK(appliance_language_code(APPLIANCE_LANGUAGE_COUNT) == NULL,
          "an out-of-range language has no code");
}

static void check_parse(void)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_RU;

    CHECK(appliance_language_parse("en", 2U, &language) ==
              APPLIANCE_LANGUAGE_OK &&
              language == APPLIANCE_LANGUAGE_EN,
          "parse en");
    CHECK(appliance_language_parse("ru", 2U, &language) ==
              APPLIANCE_LANGUAGE_OK &&
              language == APPLIANCE_LANGUAGE_RU,
          "parse ru");
    language = APPLIANCE_LANGUAGE_RU;
    CHECK(appliance_language_parse("EN", 2U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE &&
              language == APPLIANCE_LANGUAGE_RU,
          "parse refuses EN and leaves the output untouched");
    CHECK(appliance_language_parse("Ru", 2U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses Ru");
    CHECK(appliance_language_parse("de", 2U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses de");
    CHECK(appliance_language_parse("ru ", 3U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses ru with a trailing space");
    CHECK(appliance_language_parse("", 0U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses the empty string");
    CHECK(appliance_language_parse(NULL, 0U, &language) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses NULL");
    CHECK(appliance_language_parse("en", 2U, NULL) ==
              APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE,
          "parse refuses a NULL output");
}

struct utf8_case {
    const char *name;
    const char *bytes;
    size_t length;
    bool valid;
    uint32_t codepoint;
};

static void check_utf8(void)
{
    static const struct utf8_case cases[] = {
        {"ASCII A", "A", 1U, true, 0x41U},
        {"Cyrillic capital O", "\xD0\x9E", 2U, true, 0x41EU},
        {"Cyrillic small a", "\xD0\xB0", 2U, true, 0x430U},
        {"Cyrillic capital Yo", "\xD0\x81", 2U, true, 0x401U},
        {"Cyrillic small yo", "\xD1\x91", 2U, true, 0x451U},
        {"em dash", "\xE2\x80\x94", 3U, true, 0x2014U},
        {"ellipsis", "\xE2\x80\xA6", 3U, true, 0x2026U},
        {"numero", "\xE2\x84\x96", 3U, true, 0x2116U},
        {"four-byte", "\xF0\x9F\x98\x80", 4U, true, 0x1F600U},
        {"lone continuation", "\x80", 1U, false, 0U},
        {"truncated two-byte", "\xD0", 1U, false, 0U},
        {"truncated three-byte", "\xE2\x80", 2U, false, 0U},
        {"overlong two-byte", "\xC0\x80", 2U, false, 0U},
        {"overlong three-byte", "\xE0\x80\x80", 3U, false, 0U},
        {"surrogate", "\xED\xA0\x80", 3U, false, 0U},
        {"beyond U+10FFFF", "\xF4\x90\x80\x80", 4U, false, 0U},
        {"invalid byte", "\xFF", 1U, false, 0U}
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const struct utf8_case *test = &cases[index];
        size_t offset = 0U;
        uint32_t codepoint = 0U;
        bool decoded = appliance_language_utf8_next(
            test->bytes, test->length, &offset, &codepoint);

        CHECK(decoded == test->valid, "UTF-8 %s: decoded=%d expected=%d",
              test->name, (int)decoded, (int)test->valid);
        if (test->valid) {
            CHECK(codepoint == test->codepoint,
                  "UTF-8 %s: U+%04X expected U+%04X", test->name,
                  (unsigned)codepoint, (unsigned)test->codepoint);
            CHECK(offset == test->length, "UTF-8 %s: offset advanced",
                  test->name);
        } else {
            CHECK(offset == 0U, "UTF-8 %s: offset stayed put", test->name);
        }
    }
    {
        static const char word[] = "Обзор";
        static const uint32_t expected[] = {0x41EU, 0x431U, 0x437U, 0x43EU,
                                            0x440U};
        size_t offset = 0U;
        size_t position = 0U;

        while (offset < sizeof(word) - 1U) {
            uint32_t codepoint = 0U;

            CHECK(appliance_language_utf8_next(word, sizeof(word) - 1U,
                                               &offset, &codepoint),
                  "UTF-8 word decodes at %zu", position);
            CHECK(position < sizeof(expected) / sizeof(expected[0]) &&
                      codepoint == expected[position],
                  "UTF-8 word code point %zu is U+%04X", position,
                  (unsigned)codepoint);
            ++position;
        }
        CHECK(position == sizeof(expected) / sizeof(expected[0]),
              "UTF-8 word has %zu code points", position);
    }
    {
        size_t offset = 5U;
        uint32_t codepoint = 0U;

        CHECK(!appliance_language_utf8_next("abc", 3U, &offset, &codepoint),
              "UTF-8 reading past the end is refused");
        CHECK(offset == 5U, "UTF-8 refusal leaves the offset untouched");
    }
}

static void check_inventory(void)
{
    static uint32_t collected[APPLIANCE_LANGUAGE_CODEPOINT_MAX];
    uint32_t small[APPLIANCE_LANGUAGE_CODEPOINT_MAX];
    size_t count;
    size_t index;

    CHECK(appliance_language_ru_codepoints(NULL, 4U) == (size_t)-1,
          "the inventory refuses a NULL buffer");
    CHECK(appliance_language_ru_codepoints(small, 0U) == (size_t)-1,
          "the inventory refuses zero capacity");
    count = appliance_language_ru_codepoints(collected,
                                             APPLIANCE_LANGUAGE_CODEPOINT_MAX);
    CHECK(count != (size_t)-1, "the inventory fits its bound");
    CHECK(count > 0U && count <= APPLIANCE_LANGUAGE_CODEPOINT_MAX,
          "the inventory has %zu code points", count);
    for (index = 1U; index < count; ++index) {
        CHECK(collected[index - 1U] < collected[index],
              "the inventory is sorted and unique at %zu", index);
    }
    CHECK(inventory_has(collected, count, 0x401U),
          "the inventory names the capital Yo");
    CHECK(inventory_has(collected, count, 0x451U),
          "the inventory names the small yo");
    CHECK(inventory_has(collected, count, 0x2014U),
          "the inventory names the em dash");
    CHECK(!inventory_has(collected, count, 0x1F600U),
          "the inventory carries only what the catalog uses");

    /* Independently: every code point of every Russian value is in the set. */
    for (index = 0U; index < appliance_language_catalog_count(); ++index) {
        const char *value = appliance_language_catalog_value(
            index, APPLIANCE_LANGUAGE_RU);
        size_t length = strlen(value);
        size_t offset = 0U;

        while (offset < length) {
            uint32_t codepoint = 0U;

            CHECK(appliance_language_utf8_next(value, length, &offset,
                                               &codepoint),
                  "catalog value %zu is valid UTF-8", index);
            CHECK(inventory_has(collected, count, codepoint),
                  "the inventory covers U+%04X of value %zu",
                  (unsigned)codepoint, index);
        }
    }
    if (count > 0U) {
        CHECK(appliance_language_ru_codepoints(small, count - 1U) ==
                  (size_t)-1,
              "a capacity one short is refused, not truncated");
    }
    CHECK(strcmp(APPLIANCE_LANGUAGE_SETTING_KEY, "v1:system.language") == 0,
          "the one setting key is v1:system.language");
    CHECK(APPLIANCE_LANGUAGE_DEFAULT == APPLIANCE_LANGUAGE_EN,
          "English is the default language");
}

/*
 * ITEM-509 increment 7: the help catalogue.  The topics are the one content
 * the console renderer (this increment) and the panel renderer (increment 8)
 * both print, so the proof owns the content itself: every topic has a unique
 * id and all four strings in both languages, the Russian is a translation and
 * not a copy of the English, and every body is long enough to be the detailed
 * answer the item asks for.  What the console does with these strings is
 * proved in tests/console-host.c against the same accessors.
 */
static void check_help_topics(void)
{
    static const char *const required[] = {
        "install", "network", "storage", "account", "certificates",
        "names", "panel", "recovery", "records"
    };
    static const char unknown[] = "no-such-topic";
    const size_t count = appliance_help_topic_count();
    size_t index;
    size_t field;

    CHECK(count >= sizeof(required) / sizeof(required[0]),
          "the help catalogue carries the nine required topics (%zu)", count);
    for (index = 0U; index < sizeof(required) / sizeof(required[0]); ++index) {
        CHECK(appliance_help_topic_find(required[index],
                                        strlen(required[index])) != (size_t)-1,
              "the help catalogue carries topic %s", required[index]);
    }
    CHECK(appliance_help_topic_find(unknown, sizeof(unknown) - 1U) ==
              (size_t)-1,
          "an unknown id names no topic");
    CHECK(appliance_help_topic_find(NULL, 0U) == (size_t)-1,
          "a missing id names no topic");
    CHECK(appliance_help_topic_find(required[0], 0U) == (size_t)-1,
          "an empty id names no topic");
    CHECK(appliance_help_topic_id(count) == NULL &&
              appliance_help_topic_key(
                  count, APPLIANCE_HELP_FIELD_TITLE) == NULL,
          "the topic accessors refuse an index past the end");
    CHECK(appliance_help_topic_key(0U, APPLIANCE_HELP_FIELD_COUNT) == NULL,
          "the topic accessor refuses a field past the end");

    for (index = 0U; index < count; ++index) {
        const char *id = appliance_help_topic_id(index);
        const char *body;
        const char *body_ru;
        size_t other;

        CHECK(!text_is_empty(id), "topic %zu has an id", index);
        for (other = index + 1U; other < count; ++other) {
            CHECK(id != NULL && appliance_help_topic_id(other) != NULL &&
                      strcmp(id, appliance_help_topic_id(other)) != 0,
                  "topic id %zu is unique", index);
        }
        for (field = 0U; field < (size_t)APPLIANCE_HELP_FIELD_COUNT; ++field) {
            const char *en = appliance_help_topic_string(
                index, (enum appliance_help_field)field,
                APPLIANCE_LANGUAGE_EN);
            const char *ru = appliance_help_topic_string(
                index, (enum appliance_help_field)field,
                APPLIANCE_LANGUAGE_RU);

            CHECK(!text_is_empty(en) && !text_is_empty(ru),
                  "topic %zu field %zu is translated", index, field);
            if (!text_is_empty(en) && !text_is_empty(ru)) {
                CHECK(strcmp(en, ru) != 0,
                      "topic %zu field %zu really is translated", index,
                      field);
            }
        }
        body = appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_BODY, APPLIANCE_LANGUAGE_EN);
        body_ru = appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_BODY, APPLIANCE_LANGUAGE_RU);
        CHECK(body != NULL && strlen(body) >= 200U,
              "topic %zu has a detailed English body", index);
        CHECK(body_ru != NULL && strlen(body_ru) >= 200U &&
                  value_has_multibyte(body_ru),
              "topic %zu has a detailed Russian body", index);
    }
}

int main(void)
{
    check_catalog_shape();
    check_lookup();
    check_parse();
    check_utf8();
    check_inventory();
    check_help_topics();

    if (failures != 0) {
        fprintf(stderr, "appliance language catalog: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance language catalog: OK\n");
    return 0;
}
