/* ITEM-626: cat.  Files, or stdin when no file is named. */
#include "hfoc.h"

static int cat_descriptor(int fd)
{
    static char buffer[4096];

    for (;;) {
        const long got = hfoc_read(fd, buffer, sizeof(buffer));

        if (got < 0L) {
            (void)hfoc_eprintf("cat: read: %s\n", hfoc_strerror(got));
            return 1;
        }
        if (got == 0L) {
            return 0;
        }
        if (hfoc_write_all(1, buffer, (size_t)got) != 0) {
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    int index = 1;
    int failures = 0;

    if (index < argc && hfoc_is_option(argv[index])) {
        (void)hfoc_eprintf("cat: unsupported option: %s\n", argv[index]);
        return 2;
    }
    if (index == argc) {
        return cat_descriptor(0);
    }
    for (; index < argc; ++index) {
        const long fd = hfoc_open(argv[index], HFOC_OPEN_READ);

        if (fd < 0L) {
            (void)hfoc_eprintf("cat: %s: %s\n", argv[index],
                              hfoc_strerror(fd));
            ++failures;
            continue;
        }
        failures += cat_descriptor((int)fd);
        if (hfoc_close((int)fd) != 0L) {
            ++failures;
        }
    }
    return failures != 0 ? 1 : 0;
}
