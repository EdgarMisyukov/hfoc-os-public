/* ITEM-626: wc.  Supported flags: -l lines, -w words, -c bytes. */
#include "hfoc.h"

struct counts {
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
};

static int count_descriptor(int fd, struct counts *counts)
{
    static char buffer[4096];
    int in_word = 0;

    for (;;) {
        const long got = hfoc_read(fd, buffer, sizeof(buffer));
        long index;

        if (got < 0L) {
            return (int)got;
        }
        if (got == 0L) {
            return 0;
        }
        counts->bytes += (unsigned long)got;
        for (index = 0; index < got; ++index) {
            const char character = buffer[index];

            if (character == '\n') {
                ++counts->lines;
            }
            if (character == ' ' || character == '\t' || character == '\n' ||
                character == '\r' || character == '\f' || character == '\v') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                ++counts->words;
            }
        }
    }
}

static void print_counts(const struct counts *counts, int lines, int words,
                         int bytes, const char *name)
{
    char buffer[512];
    size_t used = 0U;
    int first = 1;

    if (lines) {
        used += (size_t)hfoc_snprintf(buffer + used, sizeof(buffer) - used,
                                      "%s%lu", first ? "" : " ",
                                      counts->lines);
        first = 0;
    }
    if (words) {
        used += (size_t)hfoc_snprintf(buffer + used, sizeof(buffer) - used,
                                      "%s%lu", first ? "" : " ",
                                      counts->words);
        first = 0;
    }
    if (bytes) {
        used += (size_t)hfoc_snprintf(buffer + used, sizeof(buffer) - used,
                                      "%s%lu", first ? "" : " ",
                                      counts->bytes);
        first = 0;
    }
    if (name != NULL) {
        used += (size_t)hfoc_snprintf(buffer + used, sizeof(buffer) - used,
                                      "%s%s", first ? "" : " ", name);
    }
    if (used + 1U < sizeof(buffer)) {
        buffer[used++] = '\n';
        buffer[used] = '\0';
    }
    (void)hfoc_write_all(1, buffer, used);
}

int main(int argc, char **argv)
{
    int lines = 0;
    int words = 0;
    int bytes = 0;
    int any = 0;
    int index = 1;
    int failures = 0;

    while (index < argc && hfoc_is_option(argv[index])) {
        const char *option = argv[index];
        int position;

        if (hfoc_strcmp(option, "--") == 0) {
            ++index;
            break;
        }
        for (position = 1; option[position] != '\0'; ++position) {
            if (option[position] == 'l') {
                lines = 1;
            } else if (option[position] == 'w') {
                words = 1;
            } else if (option[position] == 'c') {
                bytes = 1;
            } else {
                (void)hfoc_eprintf("wc: unsupported option: %s\n", option);
                return 2;
            }
            any = 1;
        }
        ++index;
    }
    if (!any) {
        lines = 1;
        words = 1;
        bytes = 1;
    }
    if (index == argc) {
        struct counts counts = { 0UL, 0UL, 0UL };
        const int result = count_descriptor(0, &counts);

        if (result != 0) {
            (void)hfoc_eprintf("wc: %s\n", hfoc_strerror(result));
            return 1;
        }
        print_counts(&counts, lines, words, bytes, NULL);
        return 0;
    }
    for (; index < argc; ++index) {
        struct counts counts = { 0UL, 0UL, 0UL };
        const long fd = hfoc_open(argv[index], HFOC_OPEN_READ);
        int result;

        if (fd < 0L) {
            (void)hfoc_eprintf("wc: %s: %s\n", argv[index],
                              hfoc_strerror(fd));
            ++failures;
            continue;
        }
        result = count_descriptor((int)fd, &counts);
        if (hfoc_close((int)fd) != 0L || result != 0) {
            (void)hfoc_eprintf("wc: %s: read failed\n", argv[index]);
            ++failures;
            continue;
        }
        print_counts(&counts, lines, words, bytes, argv[index]);
    }
    return failures != 0 ? 1 : 0;
}
