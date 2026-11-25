/*
 * ITEM-627: target fixture for the pipe streaming proof.  It is built into the
 * test image only, never the production rootfs.
 */
#include "hfoc.h"

#define PIPE_FD 3

static int produce(unsigned long total)
{
    unsigned char buffer[512];
    unsigned long written = 0UL;

    while (written < total) {
        unsigned long chunk = total - written;
        long result;

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        for (unsigned long index = 0UL; index < chunk; ++index) {
            buffer[index] = (unsigned char)('A' + (int)((written + index) % 26UL));
        }
        result = hfoc_write(PIPE_FD, buffer, chunk);
        if (result == HFOC_EAGAIN) {
            (void)hfoc_yield();
            continue;
        }
        if (result < 0L) {
            /* A reader that left early turns the next write into EPIPE. */
            return result == HFOC_EPIPE ? 42 : 1;
        }
        if ((unsigned long)result != chunk) {
            return 1;
        }
        written += chunk;
    }
    (void)hfoc_close(PIPE_FD);
    return 0;
}

static int consume(unsigned long total)
{
    unsigned char buffer[128];
    unsigned long seen = 0UL;

    for (;;) {
        const long got = hfoc_read(PIPE_FD, buffer, sizeof(buffer));

        if (got == HFOC_EAGAIN) {
            (void)hfoc_yield();
            continue;
        }
        if (got < 0L) {
            (void)hfoc_eprintf("pipetest: read %s\n", hfoc_strerror(got));
            return 1;
        }
        if (got == 0L) {
            break;
        }
        for (long index = 0L; index < got; ++index) {
            const unsigned char expected =
                (unsigned char)('A' + (int)((seen + (unsigned long)index) % 26UL));
            if (buffer[index] != expected) {
                (void)hfoc_eprintf("pipetest: byte mismatch at %lu\n", seen);
                return 1;
            }
        }
        seen += (unsigned long)got;
    }
    (void)hfoc_close(PIPE_FD);
    (void)hfoc_printf("PIPE-READ %lu\n", seen);
    return seen == total ? 0 : 1;
}

static int duplicate(void)
{
    long result = hfoc_dup2(PIPE_FD, 4);

    if (result < 0L) {
        (void)hfoc_eprintf("pipetest: dup2 %s\n", hfoc_strerror(result));
        return 1;
    }
    if (hfoc_close(PIPE_FD) < 0L) {
        return 1;
    }
    for (unsigned long index = 0UL; index < 1024UL; ++index) {
        const unsigned char byte = (unsigned char)('A' + (int)(index % 26UL));
        if (hfoc_write(4, &byte, 1U) != 1L) {
            return 1;
        }
    }
    (void)hfoc_close(4);
    return 0;
}

static int early(void)
{
    unsigned char byte = 0U;

    (void)hfoc_read(PIPE_FD, &byte, 1U);
    (void)hfoc_close(PIPE_FD);
    (void)hfoc_printf("PIPE-EARLY-CLOSE\n");
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long total = 8192UL;
    int ok = 0;
    long parsed;

    if (argc < 2) {
        return 2;
    }
    if (argc > 2) {
        parsed = hfoc_parse_int(argv[2], &ok);
        if (ok && parsed > 0) {
            total = (unsigned long)parsed;
        }
    }
    if (hfoc_strcmp(argv[1], "produce") == 0) {
        return produce(total);
    }
    if (hfoc_strcmp(argv[1], "consume") == 0) {
        return consume(total);
    }
    if (hfoc_strcmp(argv[1], "dup") == 0) {
        return duplicate();
    }
    if (hfoc_strcmp(argv[1], "early") == 0) {
        return early();
    }
    return 2;
}
