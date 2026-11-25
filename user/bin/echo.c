/* ITEM-626: echo, the first external native program. */
#include "hfoc.h"

int main(int argc, char **argv)
{
    int newline = 1;
    int index = 1;

    if (argc > 1 && hfoc_strcmp(argv[1], "-n") == 0) {
        newline = 0;
        index = 2;
    } else if (argc > 1 && hfoc_is_option(argv[1])) {
        (void)hfoc_eprintf("echo: unsupported option: %s\n", argv[1]);
        return 2;
    }
    for (; index < argc; ++index) {
        if (index > (newline ? 1 : 2)) {
            if (hfoc_write_all(1, " ", 1U) != 0) {
                return 1;
            }
        }
        if (hfoc_write_all(1, argv[index], hfoc_strlen(argv[index])) != 0) {
            return 1;
        }
    }
    if (newline && hfoc_write_all(1, "\n", 1U) != 0) {
        return 1;
    }
    return 0;
}
