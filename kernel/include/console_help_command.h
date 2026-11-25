#ifndef HFOC_CONSOLE_HELP_COMMAND_H
#define HFOC_CONSOLE_HELP_COMMAND_H

#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-509 increment 7: the console's help vocabulary.
 *
 * The item asks for one content with two renderers.  This is the console's
 * half: `help` prints the table of contents, `help <topic>` prints one topic
 * and `help all` prints every topic, each from the same four strings the panel
 * will render in increment 8.
 *
 * The classifier is pure, so the whole table of spellings is provable without
 * a console or a catalog reader:
 *
 *   `help`             -> LIST (the table of contents)
 *   `HELP`             -> LIST (the word folds, like every other command)
 *   `help all`         -> ALL
 *   `help install`     -> TOPIC with the index of `install`
 *   `help INSTALL`     -> TOPIC (the id folds too)
 *   `help unknown`     -> REFUSE (no topic has that id)
 *   `help `            -> REFUSE (an empty argument names nothing)
 *   `help  install`    -> REFUSE (one space, one argument)
 *   `help a b`         -> REFUSE (one argument, and nothing else)
 *   `helps`, `help1`   -> NONE (not this command at all)
 */
#define CONSOLE_HELP_COMMAND_WORD "help"
#define CONSOLE_HELP_COMMAND_ALL_WORD "all"

/*
 * The console renderer's line buffer.  One rendered line of a help topic has
 * to fit it; a string that would not fit is a refusal, not a silent cut, and
 * the host proof measures every authored line against this bound.
 */
#define CONSOLE_HELP_LINE_MAX 192U

enum console_help_command_kind {
    /* Not the help command: the ordinary command chain owns the line. */
    CONSOLE_HELP_COMMAND_NONE = 0,
    /* `HELP`: print the table of contents. */
    CONSOLE_HELP_COMMAND_LIST = 1,
    /* `HELP <topic>`: print one topic; `topic` names its index. */
    CONSOLE_HELP_COMMAND_TOPIC = 2,
    /* `HELP ALL`: print every topic. */
    CONSOLE_HELP_COMMAND_ALL = 3,
    /* The word is `help` but the argument names no topic: refuse, visibly. */
    CONSOLE_HELP_COMMAND_REFUSE = 4
};

static inline char console_help_command_fold(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

/* Byte-exact (up to ASCII case) comparison of a slice with a keyword. */
static inline bool console_help_command_word_equals(const char *line,
                                                    size_t length,
                                                    const char *keyword) {
    size_t index = 0U;

    if (line == NULL || keyword == NULL) {
        return false;
    }
    while (keyword[index] != '\0') {
        if (index >= length ||
            console_help_command_fold(line[index]) != keyword[index]) {
            return false;
        }
        ++index;
    }
    return index == length;
}

/*
 * Split the line at its first space: the word before it, the argument after
 * it.  A line with no space has no argument.  `argument == NULL` with a
 * non-zero `argument_length` means the argument is empty (`help `), which is a
 * refusal like any other id no topic carries.
 */
static inline void console_help_command_split(const char *line, size_t length,
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
 * Classify one submitted console line.  `topic` receives the topic index of a
 * TOPIC and is left at (size_t)-1 otherwise; it may be NULL.
 */
static inline enum console_help_command_kind
console_help_command_classify(const char *line, size_t length, size_t *topic) {
    size_t word_length = 0U;
    const char *argument = NULL;
    size_t argument_length = 0U;
    size_t found = (size_t)-1;

    if (topic != NULL) {
        *topic = (size_t)-1;
    }
    console_help_command_split(line, length, &word_length, &argument,
                               &argument_length);
    if (!console_help_command_word_equals(line, word_length,
                                          CONSOLE_HELP_COMMAND_WORD)) {
        return CONSOLE_HELP_COMMAND_NONE;
    }
    if (argument == NULL) {
        return CONSOLE_HELP_COMMAND_LIST;
    }
    if (console_help_command_word_equals(argument, argument_length,
                                         CONSOLE_HELP_COMMAND_ALL_WORD)) {
        return CONSOLE_HELP_COMMAND_ALL;
    }
    /*
     * One argument, one id, and the id has to be one the catalogue names: a
     * second word, a leading space or an unknown id is the same refusal.
     */
    found = appliance_help_topic_find(argument, argument_length);
    if (found == (size_t)-1) {
        return CONSOLE_HELP_COMMAND_REFUSE;
    }
    if (topic != NULL) {
        *topic = found;
    }
    return CONSOLE_HELP_COMMAND_TOPIC;
}

#endif
