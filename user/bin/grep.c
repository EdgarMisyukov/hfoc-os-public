/* ITEM-626: grep.  Supported flags: -i (case fold), -n (line numbers). */
#include "hfoc.h"

static int lower_byte(int character)
{
    if (character >= 'A' && character <= 'Z') {
        return character + ('a' - 'A');
    }
    return character;
}

static int match_line(const char *line, const char *pattern, int fold)
{
    if (!fold) {
        return hfoc_strstr(line, pattern) != NULL;
    }
    while (*line != '\0') {
        const char *left = line;
        const char *right = pattern;

        while (*left != '\0' && *right != '\0' &&
               lower_byte((unsigned char)*left) == lower_byte((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return 1;
        }
        ++line;
    }
    return 0;
}

static int grep_descriptor(int fd, const char *pattern, int fold, int number,
                           const char *name, int show_name)
{
    static char buffer[4096];
    static char line[4096];
    unsigned long line_number = 1UL;
    size_t used = 0U;
    int matched = 0;

    for (;;) {
        const long got = hfoc_read(fd, buffer, sizeof(buffer));
        long index;

        if (got < 0L) {
            return (int)got;
        }
        if (got == 0L) {
            break;
        }
        for (index = 0; index < got; ++index) {
            const char character = buffer[index];

            if (character == '\n') {
                line[used] = '\0';
                if (match_line(line, pattern, fold)) {
                    if (show_name) {
                        (void)hfoc_printf("%s:", name);
                    }
                    if (number) {
                        (void)hfoc_printf("%lu:", line_number);
                    }
                    (void)hfoc_printf("%s\n", line);
                    ++matched;
                }
                ++line_number;
                used = 0U;
            } else if (used + 1U < sizeof(line)) {
                line[used++] = character;
            }
        }
    }
    if (used != 0U) {
        line[used] = '\0';
        if (match_line(line, pattern, fold)) {
            if (show_name) {
                (void)hfoc_printf("%s:", name);
            }
            if (number) {
                (void)hfoc_printf("%lu:", line_number);
            }
            (void)hfoc_printf("%s\n", line);
            ++matched;
        }
    }
    return matched;
}

int main(int argc, char **argv)
{
    int fold = 0;
    int number = 0;
    int index = 1;
    int files = 0;
    int matched = 0;
    int failures = 0;
    const char *pattern;

    while (index < argc && hfoc_is_option(argv[index])) {
        const char *option = argv[index];
        int position;

        if (hfoc_strcmp(option, "--") == 0) {
            ++index;
            break;
        }
        for (position = 1; option[position] != '\0'; ++position) {
            if (option[position] == 'i') {
                fold = 1;
            } else if (option[position] == 'n') {
                number = 1;
            } else {
                (void)hfoc_eprintf("grep: unsupported option: %s\n", option);
                return 2;
            }
        }
        ++index;
    }
    if (index == argc) {
        (void)hfoc_eprintf("usage: grep [-i] [-n] PATTERN [FILE...]\n");
        return 2;
    }
    pattern = argv[index++];
    for (; index < argc; ++index) {
        ++files;
    }
    if (files == 0) {
        const int result = grep_descriptor(0, pattern, fold, number, NULL, 0);

        if (result < 0) {
            (void)hfoc_eprintf("grep: %s\n", hfoc_strerror(result));
            return 2;
        }
        return result > 0 ? 0 : 1;
    }
    index = argc - files;
    for (; index < argc; ++index) {
        const long fd = hfoc_open(argv[index], HFOC_OPEN_READ);
        int result;

        if (fd < 0L) {
            (void)hfoc_eprintf("grep: %s: %s\n", argv[index],
                              hfoc_strerror(fd));
            ++failures;
            continue;
        }
        result = grep_descriptor((int)fd, pattern, fold, number, argv[index],
                                 files > 1);
        if (hfoc_close((int)fd) != 0L || result < 0) {
            ++failures;
            continue;
        }
        matched += result;
    }
    if (failures != 0) {
        return 2;
    }
    return matched > 0 ? 0 : 1;
}
