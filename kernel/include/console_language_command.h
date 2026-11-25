#ifndef HFOC_CONSOLE_LANGUAGE_COMMAND_H
#define HFOC_CONSOLE_LANGUAGE_COMMAND_H

#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-509 increment 4: the console's language vocabulary.
 *
 * `language` shows the language the console and the panel are set to, and
 * `language en|ru` changes it.  The item asks for one understandable command
 * with no silent fallback: a value the catalog does not name is refused with
 * the usage line, never quietly treated as English, and the change is one
 * durable write (the second line of the system record, increment 2) plus one
 * journal record.
 *
 * The classifier is pure: it reads the line and answers, so the host proof can
 * drive the whole table of spellings without a console, a framebuffer or a
 * store.  Matching is ASCII-case-insensitive for the word and byte-exact for
 * the argument, exactly like the account vocabulary ITEM-506 added.
 *
 *   `language`            -> SHOW
 *   `language en`         -> SET with APPLIANCE_LANGUAGE_EN
 *   `language ru`         -> SET with APPLIANCE_LANGUAGE_RU
 *   `language de`         -> REFUSE (the catalog does not name it)
 *   `language EN`         -> REFUSE (the value is `en`, not `EN`)
 *   `LANGUAGE RU`         -> REFUSE (the word folds, the value does not)
 *   `language  en`        -> REFUSE (one space, one value)
 *   `language en extra`   -> REFUSE (one value, and nothing else)
 *   `languages`, `lang`   -> NONE (not this command at all)
 */
#define CONSOLE_LANGUAGE_COMMAND_WORD "language"

enum console_language_command_kind {
    /* Not the language command: the ordinary command chain owns the line. */
    CONSOLE_LANGUAGE_COMMAND_NONE = 0,
    /* `LANGUAGE`: print the active language. */
    CONSOLE_LANGUAGE_COMMAND_SHOW = 1,
    /* `LANGUAGE en|ru`: write the language; `requested` names it. */
    CONSOLE_LANGUAGE_COMMAND_SET = 2,
    /* The word is `language` but the rest is not a language: refuse, visibly. */
    CONSOLE_LANGUAGE_COMMAND_REFUSE = 3,
};

static inline char console_language_command_fold(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

/* Byte-exact (up to ASCII case) comparison of a slice with a keyword. */
static inline bool console_language_command_word_equals(const char *line,
                                                        size_t length,
                                                        const char *keyword) {
    size_t index = 0U;

    if (line == NULL || keyword == NULL) {
        return false;
    }
    while (keyword[index] != '\0') {
        if (index >= length ||
            console_language_command_fold(line[index]) != keyword[index]) {
            return false;
        }
        ++index;
    }
    return index == length;
}

/*
 * Split the line at its first space: the word before it, the argument after
 * it.  A line with no space has no argument.  `argument == NULL` with a
 * non-zero `argument_length` means the argument is empty (`language `), which
 * is a refusal like any other value the catalog does not name.
 */
static inline void console_language_command_split(const char *line,
                                                  size_t length,
                                                  size_t *word_length,
                                                  const char **argument,
                                                  size_t *argument_length) {
    size_t index = 0U;

    *word_length = length;
    *argument = NULL;
    *argument_length = 0U;
    if (line == NULL) {
        *word_length = 0U;
        return;
    }
    while (index < length && line[index] != ' ') {
        ++index;
    }
    if (index == length) {
        return;
    }
    *word_length = index;
    *argument = line + index + 1U;
    *argument_length = length - index - 1U;
}

/*
 * Classify one submitted console line.  `requested` receives the language of a
 * SET; `value`/`value_length` receive the raw argument for a SET or a REFUSE
 * (NULL and 0 for `LANGUAGE`), so the record of a refusal can name what the
 * operator actually typed.  Any out-parameter may be NULL.
 */
static inline enum console_language_command_kind
console_language_command_classify(const char *line, size_t length,
                                  enum appliance_language *requested,
                                  const char **value,
                                  size_t *value_length) {
    size_t word_length = 0U;
    const char *argument = NULL;
    size_t argument_length = 0U;
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;

    if (requested != NULL) {
        *requested = APPLIANCE_LANGUAGE_DEFAULT;
    }
    if (value != NULL) {
        *value = NULL;
    }
    if (value_length != NULL) {
        *value_length = 0U;
    }
    console_language_command_split(line, length, &word_length, &argument,
                                   &argument_length);
    if (!console_language_command_word_equals(
            line, word_length, CONSOLE_LANGUAGE_COMMAND_WORD)) {
        return CONSOLE_LANGUAGE_COMMAND_NONE;
    }
    if (argument == NULL) {
        return CONSOLE_LANGUAGE_COMMAND_SHOW;
    }
    if (value != NULL) {
        *value = argument;
    }
    if (value_length != NULL) {
        *value_length = argument_length;
    }
    /*
     * One space, one value, and the value has to be one the catalog names:
     * `appliance_language_parse()` accepts exactly `en` and `ru`, so `EN`,
     * `de`, an empty argument and a second word are all the same refusal.
     */
    if (argument_length == 0U ||
        appliance_language_parse(argument, argument_length, &language) !=
            APPLIANCE_LANGUAGE_OK) {
        return CONSOLE_LANGUAGE_COMMAND_REFUSE;
    }
    if (requested != NULL) {
        *requested = language;
    }
    return CONSOLE_LANGUAGE_COMMAND_SET;
}

#endif
