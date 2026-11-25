/* ITEM-626: ls.  Supported flags: -l (type and size). */
#include "hfoc.h"

static int list_file(const char *path, const char *name, int long_format)
{
    struct hfoc_stat info;

    if (hfoc_stat(path, &info) < 0L) {
        (void)hfoc_eprintf("ls: %s: cannot stat\n", path);
        return 1;
    }
    if (long_format) {
        (void)hfoc_printf("%c %8u %s\n",
                          info.type == HFOC_STAT_DIR ? 'd' : '-',
                          (unsigned)info.size, name);
    } else {
        (void)hfoc_printf("%s\n", name);
    }
    return 0;
}

static int list_directory(const char *path, int long_format)
{
    struct hfoc_dirent entry;
    char child[HFOC_PATH_MAX];
    uint32_t index = 0U;
    int failures = 0;

    for (;;) {
        const long result = hfoc_getdents(path, index, &entry);
        size_t length;
        size_t name_length;

        if (result < 0L) {
            if (result == HFOC_ENOENT) {
                break;
            }
            (void)hfoc_eprintf("ls: %s: %s\n", path, hfoc_strerror(result));
            return 1;
        }
        length = hfoc_strlen(path);
        name_length = hfoc_strlen(entry.name);
        if (length + 1U + name_length + 1U > sizeof(child)) {
            (void)hfoc_eprintf("ls: path too long: %s/%s\n", path, entry.name);
            ++failures;
            ++index;
            continue;
        }
        hfoc_memcpy(child, path, length);
        if (length == 0U || path[length - 1U] != '/') {
            child[length++] = '/';
        }
        hfoc_memcpy(child + length, entry.name, name_length + 1U);
        if (list_file(child, entry.name, long_format) != 0) {
            ++failures;
        }
        ++index;
    }
    return failures != 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int long_format = 0;
    int index = 1;
    int failures = 0;
    int listed = 0;

    while (index < argc && hfoc_is_option(argv[index])) {
        if (hfoc_strcmp(argv[index], "-l") == 0) {
            long_format = 1;
        } else if (hfoc_strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        } else {
            (void)hfoc_eprintf("ls: unsupported option: %s\n", argv[index]);
            return 2;
        }
        ++index;
    }
    for (; index < argc; ++index) {
        struct hfoc_stat info;

        if (hfoc_stat(argv[index], &info) < 0L) {
            (void)hfoc_eprintf("ls: %s: %s\n", argv[index], "not found");
            ++failures;
            continue;
        }
        if (info.type == HFOC_STAT_DIR) {
            failures += list_directory(argv[index], long_format);
        } else {
            failures += list_file(argv[index], argv[index], long_format);
        }
        ++listed;
    }
    if (listed == 0) {
        failures += list_directory(".", long_format);
    }
    return failures != 0 ? 1 : 0;
}
