/*
 * ITEM-626: hfoc, the native diagnostic adapter and exec helper.
 *
 *   hfoc                 print the runtime identity and current directory
 *   hfoc env             print the initial environment
 *   hfoc exec PATH ...   replace this process with PATH
 *
 * exec reports the shell-compatible statuses the later sh uses: 127 when the
 * path does not exist and 126 when it exists but is not a loadable native
 * program.  There is no command concatenation and no kernel command table.
 */
#include "hfoc.h"

static int print_environment(void)
{
    int index = 0;

    while (environ != NULL && environ[index] != NULL) {
        (void)hfoc_printf("%s\n", environ[index]);
        ++index;
    }
    return 0;
}

static int run_exec(int argc, char **argv)
{
    long result;

    if (argc < 3) {
        (void)hfoc_eprintf("hfoc: exec needs a path\n");
        return 2;
    }
    result = hfoc_exec(argv[2], &argv[2], environ);
    (void)hfoc_eprintf("hfoc: exec: %s: %s\n", argv[2],
                      hfoc_strerror(result));
    return result == HFOC_ENOENT ? 127 : 126;
}

int main(int argc, char **argv)
{
    char path[HFOC_PATH_MAX];

    if (argc < 2) {
        (void)hfoc_printf("HFOC native user runtime (not Linux, not POSIX)\n");
        (void)hfoc_printf("argc=%d argv0=%s\n", argc,
                          argc > 0 ? argv[0] : "(none)");
        (void)hfoc_printf("cwd=%s\n",
                          hfoc_getcwd(path, sizeof(path)) < 0L ? "?" : path);
        (void)hfoc_printf("commands: env, exec PATH [ARG...]\n");
        return 0;
    }
    if (hfoc_strcmp(argv[1], "env") == 0) {
        return print_environment();
    }
    if (hfoc_strcmp(argv[1], "exec") == 0) {
        return run_exec(argc, argv);
    }
    (void)hfoc_eprintf("hfoc: unsupported command: %s\n", argv[1]);
    return 2;
}
