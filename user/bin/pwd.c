/* ITEM-626: pwd over the native getcwd call. */
#include "hfoc.h"

int main(int argc, char **argv)
{
    char path[HFOC_PATH_MAX];
    long result;

    (void)argv;
    if (argc > 1) {
        (void)hfoc_eprintf("pwd: no options are supported\n");
        return 2;
    }
    result = hfoc_getcwd(path, sizeof(path));
    if (result < 0L) {
        (void)hfoc_eprintf("pwd: %s\n", hfoc_strerror(result));
        return 1;
    }
    return hfoc_puts(path) == 0 ? 0 : 1;
}
