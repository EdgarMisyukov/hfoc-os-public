#ifndef HFOC_APPLIANCE_SSH_COMMAND_H
#define HFOC_APPLIANCE_SSH_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-601/602: the exact SSH exec allowlist.
 *
 * The SSH channel never invokes the console parser, a shell or an
 * interpreter.  The four approved commands are answered from the same owners
 * the console and `diag save` use:
 *
 *   hfoc status    the complete production status composition;
 *   hfoc network   its live network/panel/SSH records;
 *   hfoc journal   the boot log's bounded staged reading;
 *   hfoc diag      the same record sequence `diag save` verifies, framed with
 *                  the shared BEGIN/END/count/CRC session and marked STREAM -
 *                  never SAVED, because no medium was written.
 *
 * Every other command, shell, PTY, subsystem and forwarding request gets a
 * stable refusal and a nonzero exit status.
 */
#define HFOC_SSH_COMMAND_OUTPUT_MAX 24576U

bool appliance_ssh_command_dispatch(
    const char *command,
    size_t command_length,
    uint8_t *output,
    size_t capacity,
    size_t *output_length,
    int *status);
const char *appliance_ssh_command_refusal(void);

#endif
