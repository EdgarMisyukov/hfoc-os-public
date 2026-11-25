/*
 * ITEM-509 increment 1: the one string catalog of the appliance interface.
 *
 * Every human-readable string of the console, the panel and the help topics
 * belongs here with a stable key and exactly two languages: `en` (the default)
 * and `ru`.  The catalog is data, not policy - it allocates nothing, reads no
 * state, opens nothing and prints nothing - so the same table serves the
 * freestanding kernel and the host proofs.
 *
 * Two rules are enforced by the host proof rather than promised in a comment:
 *   * a missing translation is never silently English: the lookup returns NULL
 *     for an unknown key and for a value that is absent, and the caller must
 *     answer with a named refusal (increment 2 wires that answer);
 *   * the Russian values are the inventory of code points the console glyph
 *     tables have to cover (increment 3): `appliance_language_ru_codepoints()`
 *     is derived from this catalog, so a translation that introduces a new
 *     letter fails the coverage proof instead of printing an empty cell.
 *
 * The catalog is consumed increment by increment (console in 2, glyphs in 3,
 * panel in 4, help in 5/6, installer in 7); increment 1 defines the table, the
 * bounded lookup and the code-point inventory, and wires no surface yet, so
 * every existing pin and every existing rendering stays byte-for-byte as it
 * was.
 */
#ifndef HFOC_APPLIANCE_LANGUAGE_H
#define HFOC_APPLIANCE_LANGUAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum appliance_language {
    APPLIANCE_LANGUAGE_EN = 0,
    APPLIANCE_LANGUAGE_RU = 1,
    APPLIANCE_LANGUAGE_COUNT = 2
};

/* The language the system uses until the operator chooses another one. */
#define APPLIANCE_LANGUAGE_DEFAULT APPLIANCE_LANGUAGE_EN

/*
 * One setting for the whole system: the console and the panel read the same
 * durable value, in the existing `v1:system.` namespace next to
 * `v1:system.hostname=`.
 */
#define APPLIANCE_LANGUAGE_SETTING_KEY "v1:system.language"

/*
 * The most distinct code points one full Russian catalog may use.  Increment 7
 * measured the catalogue it grows: 70 code points before the help topics and
 * 127 after them, because a help body is the first Russian prose in the
 * catalog (Latin lower case, digits, brackets and the newline that separates
 * body lines).  The bound is the caller's buffer, not a glyph rule - the glyph
 * proof still fails on a code point it cannot draw - so it carries headroom
 * for the installer surface of increment 9 rather than sitting one string away
 * from a broken proof.
 */
#define APPLIANCE_LANGUAGE_CODEPOINT_MAX 192U

/*
 * ITEM-509 increment 7: the help catalogue.
 *
 * A topic is one id - the word the operator types after `help` - plus four
 * strings: a title, a one-line summary for the table of contents, the body and
 * the related commands and sections.  The strings themselves live in the one
 * catalog above, so every help sentence is translated by the same table and
 * covered by the same code-point proof as the rest of the interface; the topic
 * table below names the keys, it does not duplicate the text.  The same four
 * strings feed the console renderer (this increment) and the panel renderer
 * (increment 8), so the two surfaces cannot tell different stories.
 */
enum appliance_help_field {
    APPLIANCE_HELP_FIELD_TITLE = 0,
    APPLIANCE_HELP_FIELD_SUMMARY = 1,
    APPLIANCE_HELP_FIELD_BODY = 2,
    APPLIANCE_HELP_FIELD_RELATED = 3,
    APPLIANCE_HELP_FIELD_COUNT = 4
};

enum appliance_language_status {
    APPLIANCE_LANGUAGE_OK = 0,
    APPLIANCE_LANGUAGE_UNKNOWN_LANGUAGE
};

/* `en` or `ru`; never NULL for a value inside APPLIANCE_LANGUAGE_COUNT. */
const char *appliance_language_code(enum appliance_language language);

/*
 * Parses exactly `en` or `ru` (case-sensitive, as the console command spells
 * it).  On any other text the output is untouched and the status names the
 * refusal.
 */
enum appliance_language_status appliance_language_parse(
    const char *text, size_t length, enum appliance_language *language);

/*
 * The one lookup.  NULL means "this key or this translation does not exist";
 * it never returns an empty string and never falls back to another language.
 */
const char *appliance_language_string(enum appliance_language language,
                                      const char *key);

/* The catalog, walked by the proof and by the code-point inventory. */
size_t appliance_language_catalog_count(void);
const char *appliance_language_catalog_key(size_t index);
const char *appliance_language_catalog_value(size_t index,
                                             enum appliance_language language);

/*
 * Bounded UTF-8: decodes one code point at `*offset` and advances the offset.
 * Returns false on a malformed, overlong, surrogate or truncated sequence,
 * leaving the offset where it was.  No allocation, no locale.
 */
bool appliance_language_utf8_next(const char *text, size_t length,
                                  size_t *offset, uint32_t *codepoint);

/*
 * The sorted set of distinct code points used by the `ru` values, derived from
 * the catalog itself.  Returns the count, or (size_t)-1 when `capacity` is too
 * small; the caller never gets a partial set that looks complete.
 */
size_t appliance_language_ru_codepoints(uint32_t *codepoints, size_t capacity);

/*
 * The help topics, in the order the table of contents prints them.
 * `appliance_help_topic_find()` matches the id byte-exactly up to ASCII case
 * and returns (size_t)-1 when no topic has it; a topic whose four strings are
 * not all in the catalog cannot be rendered and the caller says so instead of
 * printing a half answer.
 */
size_t appliance_help_topic_count(void);
const char *appliance_help_topic_id(size_t index);
size_t appliance_help_topic_find(const char *id, size_t length);
const char *appliance_help_topic_key(size_t index,
                                     enum appliance_help_field field);
const char *appliance_help_topic_string(size_t index,
                                        enum appliance_help_field field,
                                        enum appliance_language language);

#endif
