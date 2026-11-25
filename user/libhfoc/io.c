/* ITEM-626: buffered output, file copy and small option helpers. */
#include "hfoc.h"

#include <stdarg.h>

int hfoc_write_all(int fd, const void *buffer, size_t length)
{
    const char *bytes = (const char *)buffer;
    size_t done = 0U;

    while (done < length) {
        const long written = hfoc_write(fd, bytes + done, length - done);

        if (written <= 0L) {
            return written < 0L ? (int)written : HFOC_EIO;
        }
        done += (size_t)written;
    }
    return 0;
}

static int hfoc_puts_fd(int fd, const char *text)
{
    const size_t length = hfoc_strlen(text);
    int status = hfoc_write_all(fd, text, length);

    if (status != 0) {
        return status;
    }
    return hfoc_write_all(fd, "\n", 1U);
}

static int hfoc_vprintf_fd(int fd, const char *format, va_list arguments)
{
    char buffer[2048];
    int length;

    length = hfoc_vsnprintf(buffer, sizeof(buffer), format, arguments);
    if (length < 0) {
        return length;
    }
    if ((size_t)length >= sizeof(buffer)) {
        length = (int)(sizeof(buffer) - 1U);
    }
    return hfoc_write_all(fd, buffer, (size_t)length);
}

int hfoc_puts(const char *text)
{
    return hfoc_puts_fd(1, text);
}

int hfoc_printf(const char *format, ...)
{
    va_list arguments;
    int status;

    va_start(arguments, format);
    status = hfoc_vprintf_fd(1, format, arguments);
    va_end(arguments);
    return status;
}

/* Diagnostics go to the process's descriptor 2, so `2>file` in the native
   shell captures them and does not mix them with normal stdout output. */
int hfoc_eputs(const char *text)
{
    return hfoc_puts_fd(2, text);
}

int hfoc_eprintf(const char *format, ...)
{
    va_list arguments;
    int status;

    va_start(arguments, format);
    status = hfoc_vprintf_fd(2, format, arguments);
    va_end(arguments);
    return status;
}

int hfoc_is_option(const char *argument)
{
    return argument != NULL && argument[0] == '-' && argument[1] != '\0';
}

long hfoc_copy_file(const char *source, const char *destination)
{
    static char buffer[4096];
    long input;
    long output;
    long status = 0L;

    input = hfoc_open(source, HFOC_OPEN_READ);
    if (input < 0L) {
        return input;
    }
    output = hfoc_open(destination, HFOC_OPEN_WRITE | HFOC_OPEN_CREATE |
                                     HFOC_OPEN_TRUNC);
    if (output < 0L) {
        (void)hfoc_close((int)input);
        return output;
    }
    for (;;) {
        const long got = hfoc_read((int)input, buffer, sizeof(buffer));

        if (got < 0L) {
            status = got;
            break;
        }
        if (got == 0L) {
            break;
        }
        {
            const long written = hfoc_write((int)output, buffer, (size_t)got);

            if (written != got) {
                status = written < 0L ? written : HFOC_EIO;
                break;
            }
        }
    }
    if (hfoc_close((int)input) != 0L && status == 0L) {
        status = HFOC_EIO;
    }
    if (hfoc_close((int)output) != 0L && status == 0L) {
        status = HFOC_EIO;
    }
    if (status != 0L) {
        (void)hfoc_unlink(destination);
    }
    return status;
}
