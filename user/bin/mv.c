/* ITEM-626: mv within one mounted filesystem (native rename). */
#include "hfoc.h"

int main(int argc, char **argv)
{
    long result;

    if (argc != 3) {
        (void)hfoc_eprintf("usage: mv SOURCE DESTINATION\n");
        return 2;
    }
    if (hfoc_is_option(argv[1]) || hfoc_is_option(argv[2])) {
        (void)hfoc_eprintf("mv: no options are supported\n");
        return 2;
    }
    result = hfoc_rename(argv[1], argv[2]);
    if (result < 0L) {
        (void)hfoc_eprintf("mv: %s: %s\n", argv[1], hfoc_strerror(result));
        return 1;
    }
    return 0;
}
