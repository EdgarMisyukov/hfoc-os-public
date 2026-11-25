/*
 * ITEM-624: bounded copy of user buffers through the page-table validator.
 *
 * The range is validated completely before the copy starts.  A caller that
 * ignores the status cannot get a buffer that was partly written from a
 * range the kernel refused.
 */
#include "user_copy.h"

#include "vmm.h"

#include <string.h>

#ifdef HFOC_USER_COPY_HOST_TEST
extern bool user_copy_host_range_ok(uint64_t address, size_t length,
                                    bool write);
#endif

static enum user_copy_status user_copy_validate(uint64_t address,
                                                size_t length, bool write)
{
    if (address == 0U) {
        return USER_COPY_NULL;
    }
    if (length == 0U) {
        return USER_COPY_RANGE;
    }
    if (length > VMM_USER_BUFFER_MAX ||
        address > UINT64_MAX - (uint64_t)length) {
        return USER_COPY_RANGE;
    }
#ifdef HFOC_USER_COPY_HOST_TEST
    return user_copy_host_range_ok(address, length, write)
               ? USER_COPY_OK
               : USER_COPY_NOT_MAPPED;
#else
    switch (vmm_validate_user_buffer(vmm_current_address_space(), address,
                                     (uint64_t)length, write)) {
    case VMM_OK:
        return USER_COPY_OK;
    case VMM_NOT_MAPPED:
        return USER_COPY_NOT_MAPPED;
    case VMM_BAD_FLAGS:
        return USER_COPY_BAD_ACCESS;
    case VMM_BAD_ADDRESS_SPACE:
        return USER_COPY_BAD_ACCESS;
    default:
        return USER_COPY_RANGE;
    }
#endif
}

enum user_copy_status user_copy_check(uint64_t address, size_t length,
                                      bool write)
{
    return user_copy_validate(address, length, write);
}

enum user_copy_status user_copy_in(void *destination, uint64_t user_source,
                                   size_t length)
{
    const enum user_copy_status status =
        user_copy_validate(user_source, length, false);

    if (status != USER_COPY_OK) {
        return status;
    }
    if (destination == NULL) {
        return USER_COPY_NULL;
    }
    memcpy(destination, (const void *)(uintptr_t)user_source, length);
    return USER_COPY_OK;
}

enum user_copy_status user_copy_out(uint64_t user_destination,
                                    const void *source, size_t length)
{
    const enum user_copy_status status =
        user_copy_validate(user_destination, length, true);

    if (status != USER_COPY_OK) {
        return status;
    }
    if (source == NULL) {
        return USER_COPY_NULL;
    }
    memcpy((void *)(uintptr_t)user_destination, source, length);
    return USER_COPY_OK;
}

enum user_copy_status user_copy_string_in(char *destination, size_t capacity,
                                          uint64_t user_source,
                                          size_t *length_out)
{
    enum user_copy_status status;
    size_t length = 0U;

    if (destination == NULL || capacity == 0U) {
        return USER_COPY_NULL;
    }
    if (length_out != NULL) {
        *length_out = 0U;
    }
    if (user_source == 0U) {
        return USER_COPY_NULL;
    }
    /*
     * ITEM-626: validate one byte at a time.  A fixed capacity window is not
     * the string: a path in the last bytes of an executable segment is valid,
     * and refusing it because the window would cross into the next unmapped
     * page made every such path fail with EFAULT.  The scan still stops at
     * `capacity` and still reports USER_COPY_NO_NUL.
     */
    while (length < capacity) {
        uint8_t byte = 0U;

        status = user_copy_in(&byte, user_source + length, 1U);
        if (status != USER_COPY_OK) {
            return status;
        }
        destination[length] = (char)byte;
        if (byte == 0U) {
            if (length_out != NULL) {
                *length_out = length;
            }
            return USER_COPY_OK;
        }
        ++length;
    }
    return USER_COPY_NO_NUL;
}

const char *user_copy_status_string(enum user_copy_status status)
{
    switch (status) {
    case USER_COPY_OK:
        return "USER_COPY_OK";
    case USER_COPY_NULL:
        return "USER_COPY_NULL";
    case USER_COPY_RANGE:
        return "USER_COPY_RANGE";
    case USER_COPY_NOT_MAPPED:
        return "USER_COPY_NOT_MAPPED";
    case USER_COPY_BAD_ACCESS:
        return "USER_COPY_BAD_ACCESS";
    case USER_COPY_NO_NUL:
        return "USER_COPY_NO_NUL";
    default:
        return "USER_COPY_UNKNOWN";
    }
}
