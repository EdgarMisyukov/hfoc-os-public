#ifndef HFOC_SYSCALL_FILE_H
#define HFOC_SYSCALL_FILE_H

#include "syscall.h"

/* ITEM-624: the native file-call surface appended after EXIT. */
bool syscall_file_number(uint64_t number);
uint64_t syscall_file_handle(const struct syscall_origin *origin,
                             uint64_t number, uint64_t arg0, uint64_t arg1,
                             uint64_t arg2);

#endif
