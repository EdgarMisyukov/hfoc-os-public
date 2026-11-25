#ifndef HFOC_SYSCALL_EXEC_H
#define HFOC_SYSCALL_EXEC_H

#include "syscall.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-625: native exec.
 *
 * arg0 points at a NUL-terminated path in the caller's address space, arg1 at
 * a NULL-terminated argv pointer vector and arg2 at an optional envp vector.
 * Strings and vectors are copied byte by byte through user_copy, never
 * dereferenced directly.  On success the syscall return context is retargeted
 * at the new image; on failure the caller keeps running its old image and the
 * call returns the documented error value.
 */
bool syscall_exec_number(uint64_t number);
uint64_t syscall_exec_handle(const struct syscall_origin *origin,
                             uint64_t arg0, uint64_t arg1, uint64_t arg2);

/*
 * ITEM-628: SPAWN copies one bounded request (path, argv, envp, up to eight
 * ordered fd actions), creates the child, applies the actions while the child
 * is still unpublished, and returns its pid.  WAIT blocks cooperatively until
 * one child exits and writes {pid, status} to the caller.  Both set the
 * caller's last error so the native runtime can tell a pid from an error.
 */
uint64_t syscall_spawn_handle(const struct syscall_origin *origin,
                              uint64_t arg0);
uint64_t syscall_wait_handle(const struct syscall_origin *origin,
                             uint64_t arg0, uint64_t arg1);

#endif
