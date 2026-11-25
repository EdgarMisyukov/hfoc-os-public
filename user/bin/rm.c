/* ITEM-626: rm.  Supported flag: -f (a missing file is not an error). */
#include "hfoc.h"

int main(int argc, char **argv)
{
    int force = 0;
    int index = 1;
    int failures = 0;

    while (index < argc && hfoc_is_option(argv[index])) {
        if (hfoc_strcmp(argv[index], "-f") == 0) {
            force = 1;
        } else if (hfoc_strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        } else {
            (void)hfoc_eprintf("rm: unsupported option: %s\n", argv[index]);
            return 2;
        }
        ++index;
    }
    if (index == argc) {
        (void)hfoc_eprintf("rm: missing operand\n");
        return 2;
    }
    for (; index < argc; ++index) {
        const long result = hfoc_unlink(argv[index]);

        if (result < 0L && !(force && result == HFOC_ENOENT)) {
            (void)hfoc_eprintf("rm: %s: %s\n", argv[index],
                              hfoc_strerror(result));
            ++failures;
        }
    }
    return failures != 0 ? 1 : 0;
}
