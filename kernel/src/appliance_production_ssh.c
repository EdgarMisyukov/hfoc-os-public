#ifdef HFOC_PRODUCTION_TLS

#include "appliance_production_ssh.h"
#include "hfoc_build.h"

#include "appliance_production_entropy.h"
#include "appliance_ssh_command.h"
#include "console.h"
#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_time.h"
#include "serial.h"
#include "syscall.h"
#include "tty.h"
#include "user_process.h"

#ifdef HFOC_APPLIANCE_SSH_QEMU_TEST
#include "qemu-test-key.h"
#endif

#include "lwip/tcp.h"

#include <string.h>

#define APPLIANCE_PRODUCTION_SSH_RETRY_MS 1000U
#define APPLIANCE_PRODUCTION_SSH_INPUT_CHUNK 1024U
extern uint64_t vmm_hhdm_offset(void);

static void appliance_production_ssh_shell_release(
    struct appliance_production_ssh *ssh);

static void appliance_production_ssh_zero(void *data, size_t length)
{
    unsigned char *bytes = data;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static size_t appliance_production_ssh_append(
    char *buffer, size_t capacity, size_t length, const char *text)
{
    size_t index;

    if (buffer == NULL || text == NULL || length >= capacity) {
        return length;
    }
    for (index = 0U; text[index] != '\0' && length + 1U < capacity; ++index) {
        buffer[length++] = text[index];
    }
    buffer[length] = '\0';
    return length;
}

static size_t appliance_production_ssh_append_u32(
    char *buffer, size_t capacity, size_t length, uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U && length + 1U < capacity) {
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

static size_t appliance_production_ssh_append_base64(
    char *buffer, size_t capacity, size_t length, const uint8_t *data,
    size_t data_length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t index;

    for (index = 0U; index < data_length; index += 3U) {
        const size_t remaining = data_length - index;
        uint32_t value = (uint32_t)data[index] << 16U;
        char encoded[4];

        if (remaining > 1U) {
            value |= (uint32_t)data[index + 1U] << 8U;
        }
        if (remaining > 2U) {
            value |= data[index + 2U];
        }
        encoded[0] = alphabet[(value >> 18U) & 0x3FU];
        encoded[1] = alphabet[(value >> 12U) & 0x3FU];
        encoded[2] = remaining > 1U
            ? alphabet[(value >> 6U) & 0x3FU]
            : '=';
        encoded[3] = remaining > 2U
            ? alphabet[value & 0x3FU]
            : '=';
        for (size_t part = 0U; part < sizeof(encoded); ++part) {
            if (length + 1U < capacity) {
                buffer[length++] = encoded[part];
            }
        }
    }
    if (length < capacity) {
        buffer[length] = '\0';
    }
    return length;
}

#ifdef HFOC_APPLIANCE_SSH_QEMU_TEST
/*
 * ITEM-601 QEMU fixture: no installed state volume, so the test-only host key
 * is generated in memory from the production entropy owner and the one
 * authorized client key is the committed test fixture.  The fixture prints the
 * host public blob on serial so the script can pin it in known_hosts; it never
 * writes a key to an owner volume and never enters the production ELF.
 */
static bool appliance_production_ssh_qemu_seed(
    struct appliance_production_ssh *ssh)
{
    char record[256];
    size_t length = 0U;

    if (ssh->identity.host_ready) {
        return true;
    }
    if (!hfoc_ssh_crypto_host_key_create(
            ssh->identity.host_scalar, ssh->identity.host_blob,
            &ssh->identity.host_blob_length,
            ssh->identity.host_fingerprint, ssh->rng,
            ssh->rng_context)) {
        return false;
    }
    ssh->identity.host_ready = true;
    memcpy(ssh->identity.authorized_blob, hfoc_ssh_qemu_authorized_key,
           sizeof(hfoc_ssh_qemu_authorized_key));
    ssh->identity.authorized_length =
        sizeof(hfoc_ssh_qemu_authorized_key);
    ssh->identity.authorized_ready = true;
    length = appliance_production_ssh_append(
        record, sizeof(record), length, "[SSH-QEMU] hostkey=");
    length = appliance_production_ssh_append_base64(
        record, sizeof(record), length, ssh->identity.host_blob,
        ssh->identity.host_blob_length);
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
    return true;
}
#endif

#ifndef HFOC_APPLIANCE_SSH_QEMU_TEST
static const char *appliance_production_ssh_identity_status(
    const struct appliance_ssh_identity *identity)
{
    return appliance_ssh_identity_status_string(identity->last_status);
}
#endif

static void appliance_production_ssh_publish(
    struct appliance_production_ssh *ssh)
{
    char record[320];
    size_t length = 0U;

    length = appliance_production_ssh_append(
        record, sizeof(record), length, "[PRODUCTION-SSH] state=");
    length = appliance_production_ssh_append(
        record, sizeof(record), length,
        appliance_production_ssh_state_string(ssh->state));
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " reason=");
    length = appliance_production_ssh_append(
        record, sizeof(record), length,
        ssh->reason != NULL ? ssh->reason : "none");
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " port=22 listener=");
    length = appliance_production_ssh_append(
        record, sizeof(record), length, ssh->listening ? "up" : "down");
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " accept=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->accept_count);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " sessions=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->session_count);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " refused=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->refused_count);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " hostkey=");
    if (appliance_ssh_identity_host_ready(&ssh->identity)) {
        char fingerprint[64];

        fingerprint[0] = '\0';
        if (appliance_ssh_identity_fingerprint_text(
                ssh->identity.host_fingerprint, fingerprint,
                sizeof(fingerprint)) == 0U) {
            length = appliance_production_ssh_append(
                record, sizeof(record), length, "UNAVAILABLE");
        } else {
            length = appliance_production_ssh_append(
                record, sizeof(record), length, fingerprint);
        }
    } else {
        length = appliance_production_ssh_append(
            record, sizeof(record), length, "UNAVAILABLE");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

static void appliance_production_ssh_set_state(
    struct appliance_production_ssh *ssh,
    enum appliance_production_ssh_state state,
    const char *reason)
{
    if (ssh->state == state && ssh->reason == reason) {
        return;
    }
    ssh->state = state;
    ssh->reason = reason;
    appliance_production_ssh_publish(ssh);
}

static bool appliance_production_ssh_release(
    struct appliance_production_ssh *ssh,
    bool graceful)
{
    if (ssh == NULL) {
        return false;
    }
    if (ssh->pcb != NULL) {
        tcp_arg(ssh->pcb, NULL);
        tcp_recv(ssh->pcb, NULL);
        tcp_sent(ssh->pcb, NULL);
        tcp_err(ssh->pcb, NULL);
        tcp_poll(ssh->pcb, NULL, 0U);
        if (graceful && tcp_close(ssh->pcb) == ERR_OK) {
            /* closed */
        } else {
            tcp_abort(ssh->pcb);
        }
        ssh->pcb = NULL;
    }
    appliance_production_ssh_shell_release(ssh);
    if (ssh->session_active) {
        const int session_state = (int)ssh->session.state;
        const char *session_reason = hfoc_ssh_session_reason(&ssh->session);
        const uint32_t session_output =
            (uint32_t)ssh->session.output_length;
        const uint32_t session_last_message =
            (uint32_t)ssh->session.last_message;
        char record[192];
        size_t length = 0U;

        hfoc_ssh_session_free(&ssh->session);
        ssh->session_active = false;
        ++ssh->session_count;
        length = appliance_production_ssh_append(
            record, sizeof(record), length,
            "[PRODUCTION-SSH-SESSION] state=");
        length = appliance_production_ssh_append_u32(
            record, sizeof(record), length, (uint32_t)session_state);
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " reason=");
        length = appliance_production_ssh_append(
            record, sizeof(record), length, session_reason);
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " out=");
        length = appliance_production_ssh_append_u32(
            record, sizeof(record), length, session_output);
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " tx=");
        length = appliance_production_ssh_append_u32(
            record, sizeof(record), length, (uint32_t)ssh->tx_length);
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " msg=");
        length = appliance_production_ssh_append_u32(
            record, sizeof(record), length,
            session_last_message);
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " graceful=");
        length = appliance_production_ssh_append(
            record, sizeof(record), length, graceful ? "yes" : "no");
        length = appliance_production_ssh_append(
            record, sizeof(record), length, " sessions=");
        length = appliance_production_ssh_append_u32(
            record, sizeof(record), length, ssh->session_count);
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write(record);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        console_publish_journal(record, length);
    }
    ssh->tx_length = 0U;
    ssh->tx_offset = 0U;
    return true;
}

static bool appliance_production_ssh_pump(struct appliance_production_ssh *ssh)
{
    if (ssh == NULL || ssh->pcb == NULL || !ssh->session_active) {
        return false;
    }
    for (;;) {
        if (ssh->tx_offset < ssh->tx_length) {
            const size_t remaining = ssh->tx_length - ssh->tx_offset;
            const u16_t room = tcp_sndbuf(ssh->pcb);
            size_t chunk;
            err_t error;

            if (room == 0U) {
                break;
            }
            chunk = remaining < (size_t)room ? remaining : (size_t)room;
            error = tcp_write(ssh->pcb, ssh->tx + ssh->tx_offset,
                              (u16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (error == ERR_MEM) {
                break;
            }
            if (error != ERR_OK) {
                (void)appliance_production_ssh_release(ssh, false);
                return false;
            }
            ssh->tx_offset += chunk;
            ssh->tx_bytes += (uint32_t)chunk;
            if (ssh->tx_offset == ssh->tx_length) {
                ssh->tx_length = 0U;
                ssh->tx_offset = 0U;
            }
            (void)tcp_output(ssh->pcb);
            continue;
        }
        {
            size_t taken;

            /*
             * ITEM-602: a diag snapshot is larger than one channel window.
             * Hand as much as the peer's window and this session's output
             * buffer take, then drain it; the next loop turn continues.
             */
            if (!hfoc_ssh_session_flush_output(&ssh->session)) {
                (void)appliance_production_ssh_release(ssh, false);
                return false;
            }
            taken = hfoc_ssh_session_take_output(
                &ssh->session, ssh->tx, sizeof(ssh->tx));
            if (taken == 0U) {
                break;
            }
            ssh->tx_length = taken;
            ssh->tx_offset = 0U;
        }
    }
    (void)tcp_output(ssh->pcb);
    return ssh->pcb != NULL;
}

static void appliance_production_ssh_shell_log(
    struct appliance_production_ssh *ssh, const char *state)
{
    char record[192];
    size_t length = 0U;

    length = appliance_production_ssh_append(
        record, sizeof(record), length, "[PRODUCTION-SSH-SHELL] state=");
    length = appliance_production_ssh_append(
        record, sizeof(record), length, state);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " cols=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->tty != NULL
            ? tty_columns(ssh->tty) : 0U);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " rows=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->tty != NULL
            ? tty_rows(ssh->tty) : 0U);
    length = appliance_production_ssh_append(
        record, sizeof(record), length, " pid=");
    length = appliance_production_ssh_append_u32(
        record, sizeof(record), length, ssh->shell_child != NULL
            ? ssh->shell_child->pcb.pid : 0U);
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

static bool appliance_production_ssh_shell_start(
    void *context, const char *term, uint32_t columns, uint32_t rows)
{
    struct appliance_production_ssh *ssh = context;
    static const char *const argv[] = { "/bin/sh", NULL };
    char columns_env[sizeof("HFOC_TTY_COLUMNS=") + 4U] = "HFOC_TTY_COLUMNS=";
    const char *const envp[] = {
        "HOME=/tmp", "PATH=/bin", "TERM=xterm", columns_env, NULL
    };
    enum user_process_status status;
    struct tty *tty = NULL;
    struct kernel_process *child = NULL;

    (void)term;
    if (ssh == NULL || ssh->tty != NULL || ssh->shell_child != NULL) {
        return false;
    }
    /*
     * ITEM-629: the ordinary production loop did not need the task scheduler
     * before the owner shell existed.  Bring up the existing cooperative
     * scheduler lazily on the first shell, then let the ordinary loop yield
     * to it; no interrupt/timer ownership changes.
     */
    if (!sched_initialized() && sched_init() != SCHEDULER_OK) {
        appliance_production_ssh_shell_log(ssh, "SCHED_FAILED");
        return false;
    }
    /*
     * The ordinary boot did not publish the SYSCALL/SYSRET MSRs because it
     * had no user process.  Publish and verify the existing native ABI before
     * the first ELF can execute; a failure refuses the shell by name.
     */
    if (!syscall_msr_published()) {
        if (syscall_msr_prepare() != SYSCALL_ABI_OK ||
            syscall_msr_verify() != SYSCALL_ABI_OK) {
            appliance_production_ssh_shell_log(ssh, "SYSCALL_ABI_FAILED");
            return false;
        }
    }
    if (tty_create(&tty) != TTY_OK) {
        return false;
    }
    tty_resize(tty, columns, rows);
    (void)appliance_production_ssh_append_u32(
        columns_env, sizeof(columns_env), sizeof("HFOC_TTY_COLUMNS=") - 1U,
        tty_columns(tty));
    status = user_process_spawn_tty(&child, NULL, "/bin/sh", argv, envp,
                                    tty, vmm_hhdm_offset());
    if (status != USER_PROCESS_OK || child == NULL) {
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[PRODUCTION-SSH-SHELL] state=SPAWN_FAILED reason=");
            serial_write(user_process_status_string(status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        tty_destroy(tty);
        appliance_production_ssh_shell_log(ssh, "SPAWN_FAILED");
        return false;
    }
    ssh->tty = tty;
    ssh->shell_child = child;
    appliance_production_ssh_shell_log(ssh, "STARTED");
    return true;
}

static bool appliance_production_ssh_shell_input(
    void *context, const uint8_t *data, size_t length, size_t *accepted_out)
{
    struct appliance_production_ssh *ssh = context;
    enum tty_status status;

    if (ssh == NULL || ssh->tty == NULL || accepted_out == NULL) {
        return false;
    }
    status = tty_input(ssh->tty, data, length, accepted_out);
    return status == TTY_OK || status == TTY_FULL;
}

static uint32_t appliance_production_ssh_shell_input_space(void *context)
{
    struct appliance_production_ssh *ssh = context;

    return ssh != NULL && ssh->tty != NULL
        ? tty_input_free(ssh->tty) : 0U;
}

static bool appliance_production_ssh_shell_resize(
    void *context, uint32_t columns, uint32_t rows)
{
    struct appliance_production_ssh *ssh = context;

    if (ssh == NULL || ssh->tty == NULL) {
        return false;
    }
    tty_resize(ssh->tty, columns, rows);
    return true;
}

static bool appliance_production_ssh_shell_signal(
    void *context, const char *name, size_t name_length)
{
    struct appliance_production_ssh *ssh = context;
    uint32_t signal = PROCESS_SIGNAL_NONE;
    uint32_t status = 129U;

    if (ssh == NULL || ssh->shell_child == NULL || name == NULL) {
        return false;
    }
    if (name_length == 3U && memcmp(name, "INT", 3U) == 0) {
        signal = PROCESS_SIGNAL_INT;
        status = 130U;
    } else if (name_length == 4U && memcmp(name, "TERM", 4U) == 0) {
        signal = PROCESS_SIGNAL_TERM;
        status = 143U;
    } else if (name_length == 4U && memcmp(name, "QUIT", 4U) == 0) {
        signal = PROCESS_SIGNAL_QUIT;
        status = 131U;
    } else if (name_length == 3U && memcmp(name, "HUP", 3U) == 0) {
        signal = PROCESS_SIGNAL_HUP;
        status = 129U;
    } else if (name_length == 4U && memcmp(name, "TSTP", 4U) == 0) {
        signal = PROCESS_SIGNAL_TSTP;
        status = 148U;
    } else if (name_length == 4U && memcmp(name, "CONT", 4U) == 0) {
        signal = PROCESS_SIGNAL_CONT;
        status = 0U;
    } else {
        return false;
    }
    /*
     * ITEM-633: a foreground job owns the terminal.  Signal that group and
     * leave the shell alive; only TERM/HUP without a foreground job end the
     * session.  INT/QUIT/TSTP at the prompt are ignored, as a shell does.
     */
    if (ssh->tty != NULL) {
        const uint32_t pgid = tty_foreground(ssh->tty);

        if (pgid != 0U &&
            process_signal_group(pgid, signal) == PROCESS_OK) {
            return true;
        }
    }
    if (signal == PROCESS_SIGNAL_TERM || signal == PROCESS_SIGNAL_HUP) {
        if (ssh->shell_child->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
            process_cancel_kernel(ssh->shell_child, status) != PROCESS_OK) {
            return false;
        }
    }
    return true;
}

static size_t appliance_production_ssh_shell_output(
    void *context, uint8_t *buffer, size_t capacity)
{
    struct appliance_production_ssh *ssh = context;
    size_t done = 0U;

    if (ssh == NULL || ssh->tty == NULL ||
        tty_output_take(ssh->tty, buffer, capacity, &done) != TTY_OK) {
        return 0U;
    }
    return done;
}

static bool appliance_production_ssh_shell_exit_status(
    void *context, int *status_out)
{
    struct appliance_production_ssh *ssh = context;

    if (ssh == NULL || ssh->shell_child == NULL || status_out == NULL) {
        return false;
    }
    if (ssh->shell_child->lifecycle != PROCESS_LIFECYCLE_EXITED) {
        return false;
    }
    *status_out = (int)ssh->shell_child->exit_status;
    return true;
}

static void appliance_production_ssh_shell_eof(void *context)
{
    struct appliance_production_ssh *ssh = context;

    if (ssh != NULL && ssh->tty != NULL) {
        tty_input_eof(ssh->tty);
    }
}

static void appliance_production_ssh_shell_hangup(void *context)
{
    struct appliance_production_ssh *ssh = context;

    if (ssh == NULL) {
        return;
    }
    if (ssh->tty != NULL) {
        tty_set_foreground(ssh->tty, 0U);
        tty_hangup(ssh->tty);
    }
    if (ssh->shell_child != NULL) {
        const uint32_t session = ssh->shell_child->pcb.session_id;

        (void)process_signal_session(session, PROCESS_SIGNAL_HUP);
        if (ssh->shell_child->lifecycle == PROCESS_LIFECYCLE_RUNNING) {
            (void)process_cancel_kernel(ssh->shell_child, 129U);
        }
    }
    appliance_production_ssh_shell_log(ssh, "HANGUP");
}

static const struct hfoc_ssh_shell_ops appliance_production_ssh_shell_ops = {
    .start = appliance_production_ssh_shell_start,
    .input = appliance_production_ssh_shell_input,
    .input_space = appliance_production_ssh_shell_input_space,
    .resize = appliance_production_ssh_shell_resize,
    .signal = appliance_production_ssh_shell_signal,
    .output = appliance_production_ssh_shell_output,
    .exit_status = appliance_production_ssh_shell_exit_status,
    .eof = appliance_production_ssh_shell_eof,
    .hangup = appliance_production_ssh_shell_hangup,
};

static void appliance_production_ssh_shell_release(
    struct appliance_production_ssh *ssh)
{
    uint32_t exit_status = 0U;

    if (ssh == NULL) {
        return;
    }
    if (ssh->shell_child != NULL) {
        const uint32_t session = ssh->shell_child->pcb.session_id;

        (void)process_signal_session(session, PROCESS_SIGNAL_HUP);
        if (ssh->shell_child->lifecycle == PROCESS_LIFECYCLE_RUNNING) {
            (void)process_cancel_kernel(ssh->shell_child, 129U);
        }
        if (ssh->shell_child->lifecycle == PROCESS_LIFECYCLE_EXITED) {
            (void)process_reap_kernel(ssh->shell_child, &exit_status,
                                      vmm_hhdm_offset());
        }
        ssh->shell_child = NULL;
    }
    if (ssh->tty != NULL) {
        tty_destroy(ssh->tty);
        ssh->tty = NULL;
    }
}

static int appliance_production_ssh_command(
    void *context, const char *command, uint8_t *output, size_t capacity,
    size_t *output_length)
{
    int status = 127;

    (void)context;
    if (command == NULL || output == NULL || output_length == NULL ||
        capacity == 0U) {
        return 127;
    }
    if (!appliance_ssh_command_dispatch(
            command, strlen(command), output, capacity, output_length,
            &status)) {
        return 127;
    }
    return status;
}

static err_t appliance_production_ssh_recv(
    void *context, struct tcp_pcb *pcb, struct pbuf *packet, err_t error)
{
    struct appliance_production_ssh *ssh = context;
    uint8_t chunk[APPLIANCE_PRODUCTION_SSH_INPUT_CHUNK];
    u16_t offset = 0U;
    const u16_t total = packet != NULL ? packet->tot_len : 0U;

    (void)error;
    if (ssh == NULL || !ssh->session_active || ssh->pcb == NULL) {
        if (packet != NULL) {
            pbuf_free(packet);
        }
        return ERR_OK;
    }
    if (packet == NULL) {
        (void)appliance_production_ssh_release(ssh, true);
        return ERR_ABRT;
    }
    while (offset < total) {
        const u16_t chunk_max =
            (u16_t)APPLIANCE_PRODUCTION_SSH_INPUT_CHUNK;
        const u16_t wanted = (u16_t)((total - offset) < chunk_max
                                         ? (total - offset)
                                         : chunk_max);
        const enum hfoc_ssh_session_result result =
            (pbuf_copy_partial(packet, chunk, wanted, offset) == wanted)
                ? hfoc_ssh_session_feed(
                      &ssh->session, chunk, wanted,
                      hfoc_lwip_time_now())
                : HFOC_SSH_SESSION_PROTOCOL_ERROR;

        offset = (u16_t)(offset + wanted);
        ssh->rx_bytes += wanted;
        if (result != HFOC_SSH_SESSION_NEED_MORE &&
            result != HFOC_SSH_SESSION_OK) {
            /*
             * The command's output, exit-status and EOF may already be
             * queued when a later message ends the session.  Flush them
             * before closing, or the client sees a bare disconnect and a
             * completed command looks like a transport failure.
             *
             * pbuf_free/tcp_recved happen before release: tcp_abort() frees
             * the PCB, and lwIP must be told (ERR_ABRT) that the callback
             * consumed it, or tcp_input continues using freed memory.
             */
            pbuf_free(packet);
            tcp_recved(pcb, total);
            (void)appliance_production_ssh_pump(ssh);
            if (ssh->pcb != NULL || ssh->session_active) {
                (void)appliance_production_ssh_release(
                    ssh, result == HFOC_SSH_SESSION_CLOSED);
            }
            return ERR_ABRT;
        }
        (void)appliance_production_ssh_pump(ssh);
        if (ssh->pcb == NULL) {
            /* pump() aborted the PCB on a write/flush error. */
            pbuf_free(packet);
            return ERR_ABRT;
        }
    }
    pbuf_free(packet);
    tcp_recved(pcb, total);
    return ERR_OK;
}

static err_t appliance_production_ssh_sent(
    void *context, struct tcp_pcb *pcb, u16_t length)
{
    struct appliance_production_ssh *ssh = context;

    (void)pcb;
    (void)length;
    if (ssh != NULL && !appliance_production_ssh_pump(ssh)) {
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t appliance_production_ssh_poll_callback(
    void *context, struct tcp_pcb *pcb)
{
    struct appliance_production_ssh *ssh = context;

    (void)pcb;
    if (ssh == NULL || !ssh->session_active) {
        return ERR_OK;
    }
    if (!hfoc_ssh_session_tick(&ssh->session, hfoc_lwip_time_now())) {
        (void)appliance_production_ssh_release(
            ssh, ssh->session.state == HFOC_SSH_SESSION_STATE_DONE);
        return ERR_ABRT;
    }
    if (!appliance_production_ssh_pump(ssh)) {
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void appliance_production_ssh_error(void *context, err_t error)
{
    struct appliance_production_ssh *ssh = context;

    (void)error;
    if (ssh != NULL) {
        /*
         * lwIP has already released the PCB when it calls the error callback;
         * clearing the pointer keeps release() from aborting freed memory.
         */
        ssh->pcb = NULL;
        appliance_production_ssh_release(ssh, false);
    }
}


/*
 * ITEM-603: accept only while the ordinary gates are still live.  A network,
 * entropy or account transition between two poll turns must not let a stale
 * published listener start a session with the old credentials.
 */
static bool appliance_production_ssh_gates_live(
    const struct appliance_production_ssh *ssh)
{
    if (ssh->state != APPLIANCE_PRODUCTION_SSH_STATE_LISTENING) {
        return false;
    }
    if (!appliance_production_network_ready(ssh->network) ||
        !hfoc_lwip_netif_live(&ssh->network->netif) ||
        !hfoc_lwip_netif_addressed(&ssh->network->netif)) {
        return false;
    }
    if (!appliance_production_entropy_ready()) {
        return false;
    }
#ifndef HFOC_APPLIANCE_SSH_QEMU_TEST
    if (ssh->loader != NULL && ssh->admin_enrollment != NULL &&
        appliance_state_loader_ready(ssh->loader) &&
        !appliance_production_admin_enrollment_serving_allowed(
            ssh->admin_enrollment)) {
        return false;
    }
#endif
    return appliance_ssh_identity_host_ready(&ssh->identity) &&
           appliance_ssh_identity_authorized_ready(&ssh->identity);
}

static err_t appliance_production_ssh_accept(
    void *context, struct tcp_pcb *pcb, uint32_t generation)
{
    struct appliance_production_ssh *ssh = context;
    size_t authorized_length = 0U;
    const uint8_t *authorized;

    (void)generation;
    if (ssh == NULL || pcb == NULL) {
        if (pcb != NULL) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    if (!appliance_production_ssh_gates_live(ssh)) {
        ++ssh->refused_count;
        serial_write("[PRODUCTION-SSH-ACCEPT] refused reason=gates\n");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (ssh->session_active || ssh->pcb != NULL) {
        ++ssh->refused_count;
        serial_write("[PRODUCTION-SSH-ACCEPT] refused reason=busy\n");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    authorized = appliance_ssh_identity_authorized_blob(
        &ssh->identity, &authorized_length);
    if (!appliance_ssh_identity_host_ready(&ssh->identity) ||
        authorized == NULL || authorized_length == 0U ||
        authorized_length > sizeof(ssh->session.authorized_key) ||
        !hfoc_ssh_session_init(
            &ssh->session, ssh->rng, ssh->rng_context,
            appliance_production_ssh_command, NULL)) {
        ++ssh->refused_count;
        serial_write("[PRODUCTION-SSH-ACCEPT] refused reason=session_init\n");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    hfoc_ssh_session_set_shell(&ssh->session,
                               &appliance_production_ssh_shell_ops, ssh);
    if (!hfoc_ssh_crypto_host_key_load(
            &ssh->session.crypto, ssh->identity.host_scalar, ssh->rng,
            ssh->rng_context)) {
        hfoc_ssh_session_free(&ssh->session);
        ++ssh->refused_count;
        serial_write("[PRODUCTION-SSH-ACCEPT] refused reason=host_key\n");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memcpy(ssh->session.authorized_key, authorized, authorized_length);
    ssh->session.authorized_key_length = authorized_length;
    ssh->pcb = pcb;
    ssh->session_active = true;
    ssh->tx_length = 0U;
    ssh->tx_offset = 0U;
    hfoc_ssh_session_start(&ssh->session, hfoc_lwip_time_now());
    tcp_arg(pcb, ssh);
    tcp_recv(pcb, appliance_production_ssh_recv);
    tcp_sent(pcb, appliance_production_ssh_sent);
    tcp_poll(pcb, appliance_production_ssh_poll_callback, 1U);
    tcp_err(pcb, appliance_production_ssh_error);
    ++ssh->accept_count;
    if (!appliance_production_ssh_pump(ssh)) {
        return ERR_ABRT;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[PRODUCTION-SSH-ACCEPT] state=started tx=");
        serial_write_u64(ssh->tx_length);
        serial_write(" out=");
        serial_write_u64(ssh->session.output_length);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    return ERR_OK;
}

static void appliance_production_ssh_listener_stop(
    struct appliance_production_ssh *ssh)
{
    if (ssh->listener.published) {
        (void)hfoc_lwip_tcp_shutdown(&ssh->listener);
    }
    ssh->listening = false;
}

static bool appliance_production_ssh_listener_start(
    struct appliance_production_ssh *ssh,
    const struct network_ipv4_config *config,
    uint32_t generation)
{
    struct network_ipv4_port_policy policy;
    struct hfoc_lwip_tcp_callback callback;
    enum hfoc_lwip_tcp_status status;

    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    policy.port = APPLIANCE_PRODUCTION_SSH_PORT;
    memset(&callback, 0, sizeof(callback));
    callback.accept = appliance_production_ssh_accept;
    callback.context = ssh;
    ssh->listening = false;
    if (ssh->listener.published) {
        (void)hfoc_lwip_tcp_shutdown(&ssh->listener);
    }
    status = hfoc_lwip_tcp_init(
        &ssh->listener, &ssh->network->netif, config, &policy, &callback,
        generation);
    if (status != HFOC_LWIP_TCP_OK) {
        ssh->reason = hfoc_lwip_tcp_status_string(status);
        return false;
    }
    status = hfoc_lwip_tcp_bind(&ssh->listener, &policy);
    if (status == HFOC_LWIP_TCP_OK) {
        status = hfoc_lwip_tcp_listen(&ssh->listener);
    }
    if (status != HFOC_LWIP_TCP_OK) {
        ssh->reason = hfoc_lwip_tcp_status_string(status);
        (void)hfoc_lwip_tcp_shutdown(&ssh->listener);
        return false;
    }
    ssh->config = *config;
    ssh->config_valid = true;
    ssh->generation = generation;
    ssh->listening = true;
    return true;
}

static bool appliance_production_ssh_prepare(
    struct appliance_production_ssh *ssh, uint32_t now_ms)
{
    const struct network_ipv4_config *active;
#ifndef HFOC_APPLIANCE_SSH_QEMU_TEST
    enum appliance_ssh_identity_status identity_status;
#endif

    if (!ssh->listening &&
        ssh->last_attempt_ms != 0U &&
        (uint32_t)(now_ms - ssh->last_attempt_ms) <
            APPLIANCE_PRODUCTION_SSH_RETRY_MS) {
        return false;
    }
    ssh->last_attempt_ms = now_ms;
#ifndef HFOC_APPLIANCE_SSH_QEMU_TEST
    if (ssh->loader == NULL || ssh->network == NULL ||
        ssh->admin_enrollment == NULL || ssh->rng == NULL) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE,
            "missing_composition");
        return false;
    }
    if (appliance_state_loader_ready(ssh->loader) &&
        !appliance_production_admin_enrollment_serving_allowed(
            ssh->admin_enrollment)) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ACCOUNT,
            "admin_credential_unset");
        return false;
    }
#else
    if (ssh->network == NULL || ssh->rng == NULL) {
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE,
            "missing_composition");
        return false;
    }
#endif
    if (!appliance_production_network_ready(ssh->network) ||
        !hfoc_lwip_netif_live(&ssh->network->netif) ||
        !hfoc_lwip_netif_addressed(&ssh->network->netif)) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_NETWORK,
            "network_not_ready");
        return false;
    }
    (void)appliance_production_entropy_init(now_ms);
    if (!appliance_production_entropy_ready()) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ENTROPY,
            "no_hardware_entropy_source");
        return false;
    }
#ifdef HFOC_APPLIANCE_SSH_QEMU_TEST
    if (!appliance_production_ssh_qemu_seed(ssh)) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_FAILED,
            "qemu_key_generate_failed");
        return false;
    }
#else
    if (!appliance_ssh_identity_host_ready(&ssh->identity)) {
        identity_status = appliance_ssh_identity_prepare(
            &ssh->identity, ssh->loader, ssh->rng, ssh->rng_context);
        if (identity_status != APPLIANCE_SSH_IDENTITY_OK ||
            !appliance_ssh_identity_host_ready(&ssh->identity)) {
            appliance_production_ssh_listener_stop(ssh);
            appliance_production_ssh_set_state(
                ssh, APPLIANCE_PRODUCTION_SSH_STATE_FAILED,
                appliance_production_ssh_identity_status(&ssh->identity));
            return false;
        }
    }
    if (!appliance_ssh_identity_authorized_ready(&ssh->identity)) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_CLIENT_KEY,
            "authorized_key_missing");
        return false;
    }
#endif
    active = hfoc_lwip_netif_config(&ssh->network->netif);
    if (active == NULL) {
        appliance_production_ssh_listener_stop(ssh);
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_NETWORK,
            "address_not_assigned");
        return false;
    }
    if (ssh->listening && ssh->config_valid &&
        ssh->generation == ssh->network->generation &&
        memcmp(&ssh->config, active, sizeof(ssh->config)) == 0) {
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_LISTENING, "listening");
        return true;
    }
    appliance_production_ssh_listener_stop(ssh);
    if (!appliance_production_ssh_listener_start(
            ssh, active, ssh->network->generation)) {
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_FAILED,
            ssh->reason != NULL ? ssh->reason : "listener_failed");
        return false;
    }
    appliance_production_ssh_set_state(
        ssh, APPLIANCE_PRODUCTION_SSH_STATE_LISTENING, "listening");
    return true;
}

void appliance_production_ssh_init(
    struct appliance_production_ssh *ssh,
    struct appliance_state_loader *loader,
    const struct appliance_production_network *network,
    struct appliance_production_admin_enrollment *admin_enrollment,
    hfoc_ssh_rng_fn rng,
    void *rng_context)
{
    if (ssh == NULL) {
        return;
    }
    appliance_production_ssh_zero(ssh, sizeof(*ssh));
    ssh->initialized = true;
    ssh->version = APPLIANCE_PRODUCTION_SSH_VERSION;
    ssh->loader = loader;
    ssh->network = network;
    ssh->admin_enrollment = admin_enrollment;
    ssh->rng = rng;
    ssh->rng_context = rng_context;
    ssh->state = APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE;
    ssh->reason = "not_started";
    appliance_ssh_identity_init(&ssh->identity);
}

void appliance_production_ssh_poll(
    struct appliance_production_ssh *ssh, uint32_t now_ms)
{
    if (ssh == NULL || !ssh->initialized ||
        ssh->version != APPLIANCE_PRODUCTION_SSH_VERSION) {
        return;
    }
    if (ssh->revoke_pending) {
        /*
         * The durable clear failed during a credential reset.  The in-memory
         * key is already gone, so keep retrying the tombstone while the
         * listener stays stopped instead of leaving the store silently stale.
         */
        if (appliance_ssh_identity_authorized_key_remove(&ssh->identity) ==
            APPLIANCE_SSH_IDENTITY_OK) {
            ssh->revoke_pending = false;
            appliance_production_ssh_set_state(
                ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_CLIENT_KEY,
                "authorized_key_revoked");
        } else {
            appliance_production_ssh_set_state(
                ssh, APPLIANCE_PRODUCTION_SSH_STATE_FAILED,
                "authorized_key_revoke_failed");
        }
        return;
    }
    if (ssh->session_active) {
        if (!hfoc_ssh_session_tick(&ssh->session, now_ms)) {
            appliance_production_ssh_release(
                ssh, ssh->session.state == HFOC_SSH_SESSION_STATE_DONE);
        } else {
            appliance_production_ssh_pump(ssh);
        }
        return;
    }
    if (ssh->state == APPLIANCE_PRODUCTION_SSH_STATE_FAILED) {
        return;
    }
    (void)appliance_production_ssh_prepare(ssh, now_ms);
}

bool appliance_production_ssh_revoke(struct appliance_production_ssh *ssh)
{
    enum appliance_ssh_identity_status status;

    if (ssh == NULL || !ssh->initialized) {
        return false;
    }
    (void)appliance_production_ssh_release(ssh, false);
    appliance_production_ssh_listener_stop(ssh);
    status = appliance_ssh_identity_authorized_key_remove(&ssh->identity);
    if (status != APPLIANCE_SSH_IDENTITY_OK) {
        /*
         * The in-memory key is already cleared, so the old key cannot
         * authenticate this boot.  Keep the listener stopped and name the
         * failed durable clear instead of pretending the revoke completed.
         */
        ssh->revoke_pending = true;
        appliance_production_ssh_set_state(
            ssh, APPLIANCE_PRODUCTION_SSH_STATE_FAILED,
            "authorized_key_revoke_failed");
        return false;
    }
    ssh->revoke_pending = false;
    appliance_production_ssh_set_state(
        ssh, APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_CLIENT_KEY,
        "authorized_key_revoked");
    return true;
}

void appliance_production_ssh_shutdown(struct appliance_production_ssh *ssh)
{
    if (ssh == NULL || !ssh->initialized) {
        return;
    }
    appliance_production_ssh_release(ssh, false);
    appliance_production_ssh_listener_stop(ssh);
    appliance_ssh_identity_init(&ssh->identity);
    ssh->state = APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE;
    ssh->reason = "stopped";
    ssh->initialized = false;
}

enum appliance_production_ssh_state appliance_production_ssh_state(
    const struct appliance_production_ssh *ssh)
{
    return ssh != NULL ? ssh->state
                       : APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE;
}

const char *appliance_production_ssh_reason(
    const struct appliance_production_ssh *ssh)
{
    return ssh != NULL && ssh->reason != NULL ? ssh->reason : "unavailable";
}

bool appliance_production_ssh_listening(
    const struct appliance_production_ssh *ssh)
{
    return ssh != NULL && ssh->listening;
}

const struct appliance_ssh_identity *appliance_production_ssh_identity(
    const struct appliance_production_ssh *ssh)
{
    return ssh != NULL && ssh->initialized ? &ssh->identity : NULL;
}

const char *appliance_production_ssh_state_string(
    enum appliance_production_ssh_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_NETWORK:
        return "AWAITING_NETWORK";
    case APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ENTROPY:
        return "AWAITING_ENTROPY";
    case APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ACCOUNT:
        return "AWAITING_ACCOUNT";
    case APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_HOST_KEY:
        return "AWAITING_HOST_KEY";
    case APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_CLIENT_KEY:
        return "AWAITING_CLIENT_KEY";
    case APPLIANCE_PRODUCTION_SSH_STATE_LISTENING:
        return "LISTENING";
    case APPLIANCE_PRODUCTION_SSH_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

size_t appliance_production_ssh_render(
    const struct appliance_production_ssh *ssh, char *buffer,
    size_t capacity)
{
    size_t length = 0U;

    if (buffer == NULL || capacity == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    length = appliance_production_ssh_append(
        buffer, capacity, length, "state=");
    length = appliance_production_ssh_append(
        buffer, capacity, length,
        appliance_production_ssh_state_string(
            appliance_production_ssh_state(ssh)));
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " reason=");
    length = appliance_production_ssh_append(
        buffer, capacity, length,
        appliance_production_ssh_reason(ssh));
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " port=22 listener=");
    length = appliance_production_ssh_append(
        buffer, capacity, length,
        appliance_production_ssh_listening(ssh) ? "up" : "down");
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " accept=");
    length = appliance_production_ssh_append_u32(
        buffer, capacity, length,
        ssh != NULL ? ssh->accept_count : 0U);
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " sessions=");
    length = appliance_production_ssh_append_u32(
        buffer, capacity, length,
        ssh != NULL ? ssh->session_count : 0U);
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " refused=");
    length = appliance_production_ssh_append_u32(
        buffer, capacity, length,
        ssh != NULL ? ssh->refused_count : 0U);
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " lacc=");
    length = appliance_production_ssh_append_u32(
        buffer, capacity, length,
        ssh != NULL ? ssh->listener.accept_count : 0U);
    length = appliance_production_ssh_append(buffer, capacity, length,
                                             " hostkey=");
    if (ssh != NULL && appliance_ssh_identity_host_ready(&ssh->identity)) {
        length = appliance_production_ssh_append(buffer, capacity, length,
                                                 "SHA256:");
        length = appliance_production_ssh_append_base64(
            buffer, capacity, length, ssh->identity.host_fingerprint, 32U);
    } else {
        length = appliance_production_ssh_append(buffer, capacity, length,
                                                 "UNAVAILABLE");
    }
    return length;
}

size_t appliance_production_ssh_render_hostkey(
    const struct appliance_production_ssh *ssh,
    const char *address,
    uint16_t port,
    char *buffer,
    size_t capacity)
{
    size_t length = 0U;
    const struct appliance_ssh_identity *identity =
        ssh != NULL ? &ssh->identity : NULL;
    const char *host_address = address != NULL && address[0] != '\0'
        ? address : "none";

    length = appliance_production_ssh_append(
        buffer, capacity, length, "v=1 ");
    if (identity == NULL || !appliance_ssh_identity_host_ready(identity) ||
        identity->host_blob_length == 0U ||
        identity->host_blob_length > sizeof(identity->host_blob)) {
        length = appliance_production_ssh_append(
            buffer, capacity, length, "state=UNAVAILABLE reason=");
        length = appliance_production_ssh_append(
            buffer, capacity, length,
            ssh != NULL && ssh->reason != NULL ? ssh->reason : "NO_IDENTITY");
        length = appliance_production_ssh_append(
            buffer, capacity, length, " address=");
        length = appliance_production_ssh_append(
            buffer, capacity, length, host_address);
        length = appliance_production_ssh_append(
            buffer, capacity, length, " port=");
        length = appliance_production_ssh_append_u32(
            buffer, capacity, length, port);
        return length;
    }
    length = appliance_production_ssh_append(
        buffer, capacity, length, "state=READY algorithm=");
    length = appliance_production_ssh_append(
        buffer, capacity, length, "ecdsa-sha2-nistp256 blob=");
    length = appliance_production_ssh_append_base64(
        buffer, capacity, length, identity->host_blob,
        identity->host_blob_length);
    {
        char fingerprint[64];

        fingerprint[0] = '\0';
        if (appliance_ssh_identity_fingerprint_text(
                identity->host_fingerprint, fingerprint,
                sizeof(fingerprint)) == 0U) {
            length = appliance_production_ssh_append(
                buffer, capacity, length, " fingerprint=UNAVAILABLE");
        } else {
            length = appliance_production_ssh_append(
                buffer, capacity, length, " fingerprint=");
            length = appliance_production_ssh_append(
                buffer, capacity, length, fingerprint);
        }
    }
    length = appliance_production_ssh_append(
        buffer, capacity, length, " address=");
    length = appliance_production_ssh_append(
        buffer, capacity, length, host_address);
    length = appliance_production_ssh_append(
        buffer, capacity, length, " port=");
    length = appliance_production_ssh_append_u32(
        buffer, capacity, length, port);
    length = appliance_production_ssh_append(
        buffer, capacity, length, " build=");
    length = appliance_production_ssh_append(
        buffer, capacity, length, hfoc_build_id());
    return length;
}


#else
typedef int appliance_production_ssh_translation_unit_guard;
#endif
