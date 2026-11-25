/* ITEM-626: mkdir, one or more paths. */
#include "hfoc.h"

int main(int argc, char **argv)
{
    int index = 1;
    int failures = 0;

    if (index < argc && hfoc_is_option(argv[index])) {
        (void)hfoc_eprintf("mkdir: unsupported option: %s\n", argv[index]);
        return 2;
    }
    if (index == argc) {
        (void)hfoc_eprintf("mkdir: missing operand\n");
        return 2;
    }
    for (; index < argc; ++index) {
        const long result = hfoc_mkdir(argv[index]);

        if (result < 0L) {
            (void)hfoc_eprintf("mkdir: %s: %s\n", argv[index],
                              hfoc_strerror(result));
            ++failures;
        }
    }
    return failures != 0 ? 1 : 0;
}
