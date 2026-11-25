/*
 * ITEM-626: the native user ABI and the runtime helpers, driven on the host.
 *
 * The static assertions compare the userland header with the kernel header so
 * a syscall number, flag or error value cannot drift silently.  The string and
 * format checks drive the same sources the target programs link.
 */
#include "hfoc.h"
#include "syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

#define ABI(name) \
    _Static_assert(HFOC_SYSCALL_##name == SYSCALL_##name, \
                   "user/runtime ABI drift: " #name)

ABI(WRITE);
ABI(YIELD);
ABI(EXIT);
ABI(READ);
ABI(OPEN);
ABI(CLOSE);
ABI(STAT);
ABI(GETDENTS);
ABI(LSEEK);
ABI(CHDIR);
ABI(GETCWD);
ABI(MKDIR);
ABI(UNLINK);
ABI(RENAME);
ABI(FILE_WRITE);
ABI(EXEC);
ABI(RESULT);
ABI(PIPE);
ABI(DUP2);
ABI(SPAWN);
ABI(WAIT);

_Static_assert(HFOC_SYSCALL_RESULT == 16U,
               "SYSCALL_RESULT keeps its published number");
_Static_assert(HFOC_SYSCALL_DUP2 == 18U,
               "SYSCALL_DUP2 keeps its published number");
_Static_assert(HFOC_SYSCALL_WAIT == 20U,
               "SYSCALL_WAIT keeps its published number");
_Static_assert(HFOC_SYSCALL_SLEEP + 1U == SYSCALL_COUNT,
               "SYSCALL_SLEEP must be the last appended number");
_Static_assert(HFOC_OPEN_READ == SYSCALL_OPEN_READ &&
                   HFOC_OPEN_WRITE == SYSCALL_OPEN_WRITE &&
                   HFOC_OPEN_CREATE == SYSCALL_OPEN_CREATE &&
                   HFOC_OPEN_TRUNC == SYSCALL_OPEN_TRUNC,
               "open flags must match the kernel ABI");
_Static_assert(HFOC_SEEK_SET == SYSCALL_SEEK_SET &&
                   HFOC_SEEK_CUR == SYSCALL_SEEK_CUR &&
                   HFOC_SEEK_END == SYSCALL_SEEK_END,
               "seek values must match the kernel ABI");
_Static_assert(HFOC_STAT_FILE == SYSCALL_STAT_FILE &&
                   HFOC_STAT_DIR == SYSCALL_STAT_DIR,
               "stat types must match the kernel ABI");
_Static_assert(-HFOC_EINVAL == (int)SYSCALL_RET_EINVAL &&
                   -HFOC_EFAULT == (int)SYSCALL_RET_EFAULT &&
                   -HFOC_ENOENT == (int)SYSCALL_RET_ENOENT &&
                   -HFOC_EBADF == (int)SYSCALL_RET_EBADF &&
                   -HFOC_ENOSPC == (int)SYSCALL_RET_ENOSPC &&
                   -HFOC_EROFS == (int)SYSCALL_RET_EROFS &&
                   -HFOC_EMFILE == (int)SYSCALL_RET_EMFILE,
               "runtime errors must negate the kernel codes");
_Static_assert(HFOC_PATH_MAX == SYSCALL_PATH_MAX,
               "the runtime path bound must match the syscall bound");
_Static_assert(HFOC_NAME_MAX == VFS_NAME_MAX,
               "the runtime name bound must match the VFS bound");

static void test_strings(void)
{
    char buffer[16];

    CHECK(hfoc_strlen("native") == 6U, "strlen counts the bytes");
    CHECK(hfoc_strcmp("abc", "abc") == 0 && hfoc_strcmp("abc", "abd") < 0,
          "strcmp orders lexicographically");
    CHECK(hfoc_strncmp("abcdef", "abcxyz", 3U) == 0,
          "strncmp stops at the requested count");
    CHECK(hfoc_strcpy(buffer, "copy") == buffer &&
              strcmp(buffer, "copy") == 0,
          "strcpy returns the destination");
    CHECK(hfoc_strchr("a/b/c", '/') != NULL &&
              hfoc_strrchr("a/b/c", '/') == hfoc_strchr("a/b/c", '/') + 2,
          "strchr and strrchr find the first and last byte");
    CHECK(hfoc_strstr("HFOC OS", "OC O") != NULL &&
              hfoc_strstr("HFOC OS", "linux") == NULL,
          "strstr finds a bounded byte substring");
    CHECK(hfoc_strcasecmp("HFOC", "hfoc") == 0,
          "strcasecmp folds ASCII case");
    {
        const char overlap[] = "abcdef";
        char moved[8];

        memcpy(moved, overlap, sizeof(overlap));
        CHECK(hfoc_memmove(moved + 1, moved, 4U) == moved + 1 &&
                  memcmp(moved, "aabcd", 5U) == 0,
              "memmove preserves an overlapping copy");
    }
    CHECK(hfoc_memcmp("abc", "abc", 3U) == 0 &&
              hfoc_memcmp("abc", "abd", 3U) < 0,
          "memcmp compares unsigned bytes");
}

static void test_format(void)
{
    char buffer[128];
    int length;

    length = hfoc_snprintf(buffer, sizeof(buffer), "%s %d %u %x %c %%",
                           "file", -7, 42U, 0x2aU, 'Z');
    CHECK(length == 17 && strcmp(buffer, "file -7 42 2a Z %") == 0,
          "snprintf renders the supported conversions");
    length = hfoc_snprintf(buffer, sizeof(buffer), "[%5u][%-4d][%04x]",
                           7U, 3, 0x2aU);
    CHECK(length == 19 && strcmp(buffer, "[    7][3   ][002a]") == 0,
          "snprintf honours width, left and zero flags");
    length = hfoc_snprintf(buffer, 5U, "abcdefgh");
    CHECK(length == 8 && strcmp(buffer, "abcd") == 0,
          "snprintf truncates but reports the full length");
    length = hfoc_snprintf(buffer, sizeof(buffer), "%lu", 123456789UL);
    CHECK(strcmp(buffer, "123456789") == 0,
          "snprintf renders an unsigned long");
}

static void test_errors(void)
{
    CHECK(strcmp(hfoc_strerror(HFOC_ENOENT), "no such file or directory") == 0,
          "a negative error names its text");
    CHECK(strcmp(hfoc_strerror(-HFOC_EROFS), "read-only filesystem") == 0,
          "the runtime error text does not depend on the sign");
    CHECK(strcmp(hfoc_strerror(0), "success") == 0,
          "zero is success");
}

static void test_alloc(void)
{
    void *first;
    void *second;

    hfoc_alloc_reset();
    first = hfoc_alloc(1U);
    second = hfoc_alloc(4096U);
    CHECK(first != NULL && second != NULL && second > first,
          "the bounded arena hands out distinct aligned blocks");
    CHECK(hfoc_alloc(HFOC_ARENA_BYTES) == NULL,
          "the arena refuses a request past its fixed bound");
    hfoc_alloc_reset();
    CHECK(hfoc_alloc(HFOC_ARENA_BYTES) != NULL,
          "resetting releases the arena without growing it");
}

int main(void)
{
    test_strings();
    test_format();
    test_errors();
    test_alloc();
    if (failures != 0) {
        fprintf(stderr, "user runtime host test: %d FAILURES\n", failures);
        return 1;
    }
    printf("user runtime host test: ALL OK\n");
    return 0;
}
