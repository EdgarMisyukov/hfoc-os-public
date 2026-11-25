/*
 * Host-only POSIX driver for the ITEM-601 interop proof: one TCP
 * connection, one real SSH session.  The production endpoint lives in
 * appliance_production_ssh.c; this program exists so a stock OpenSSH client
 * exercises the same wire/crypto/transport modules in `make test`.
 */
#include "hfoc_ssh_transport.h"
#include "hfoc_ssh_wire.h"

#include "mbedtls/ecp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

static int urandom_fd = -1;

static int host_rng(void *context, unsigned char *output, size_t length)
{
    size_t done = 0U;

    (void)context;
    while (done < length) {
        ssize_t got = read(urandom_fd, output + done, length - done);
        if (got <= 0) {
            return -1;
        }
        done += (size_t)got;
    }
    return 0;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t base64_decode(const char *text, uint8_t *out, size_t capacity)
{
    size_t length = 0U;
    int bits = 0;
    unsigned accumulator = 0U;

    for (; *text != '\0'; ++text) {
        int value;
        if (*text == '=' || *text == '\n' || *text == '\r') {
            continue;
        }
        value = base64_value(*text);
        if (value < 0) {
            return 0U;
        }
        accumulator = (accumulator << 6) | (unsigned)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (length >= capacity) {
                return 0U;
            }
            out[length++] = (uint8_t)(accumulator >> bits);
        }
    }
    return length;
}

static size_t base64_encode(const uint8_t *data, size_t length, char *out,
                            size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0U;

    for (size_t index = 0U; index < length; index += 3U) {
        uint32_t value = (uint32_t)data[index] << 16;
        size_t remaining = length - index;
        if (remaining > 1U) value |= (uint32_t)data[index + 1U] << 8;
        if (remaining > 2U) value |= data[index + 2U];
        if (at + 4U >= capacity) {
            return 0U;
        }
        out[at++] = alphabet[(value >> 18) & 0x3fU];
        out[at++] = alphabet[(value >> 12) & 0x3fU];
        out[at++] = remaining > 1U ? alphabet[(value >> 6) & 0x3fU] : '=';
        out[at++] = remaining > 2U ? alphabet[value & 0x3fU] : '=';
    }
    out[at] = '\0';
    return at;
}

static int command_dispatch(void *context, const char *command, uint8_t *out,
                            size_t capacity, size_t *out_length)
{
    const char *text;

    (void)context;
    if (strcmp(command, "hfoc status") == 0) {
        text = "HFOC-SPIKE status\n"
               "transport=sshv2 session=1\n"
               "kex=ecdh-sha2-nistp256 hostkey=ecdsa-sha2-nistp256\n"
               "cipher=aes128-gcm@openssh.com\n"
               "account=hfoc auth=publickey\n"
               "state=READY\n";
        *out_length = strlen(text);
        if (*out_length > capacity) {
            return 127;
        }
        memcpy(out, text, *out_length);
        return 0;
    }
    if (strcmp(command, "hfoc diag") == 0) {
        /* The shared BEGIN/END/count/CRC frame the collector verifies.  The
         * records are synthetic; the production QEMU proof runs the real
         * owners.  CRC = ~crc32 over each record + '\n' = AA9B5C3F. */
        text = "[DIAG] id=00000001 state=BEGIN build=spike version=test"
               " slots=0/0\n"
               "[NET-STATUS] device=spike readiness=READY link=UP\n"
               "[PANEL] state=READY listener=up\n"
               "[DIAG] id=00000001 state=END records=2 crc=5564A3C0\n"
               "[DIAG] transport=ssh mode=STREAM source=RAM"
               " verified=COMPOSED saved=no records=2 crc=5564A3C0\n";
        *out_length = strlen(text);
        if (*out_length > capacity) {
            return 127;
        }
        memcpy(out, text, *out_length);
        return 0;
    }
    if (strcmp(command, "hfoc network") == 0) {
        text = "[NET-STATUS] device=spike readiness=READY link=UP\n"
               "[NET-ADDR] state=ACTIVE address=192.0.2.13\n"
               "[PANEL] state=READY listener=up\n"
               "[PRODUCTION-SSH] state=LISTENING port=22\n";
        *out_length = strlen(text);
        if (*out_length > capacity) {
            return 127;
        }
        memcpy(out, text, *out_length);
        return 0;
    }
    if (strcmp(command, "hfoc journal") == 0) {
        text = "BOOTLOG: state=READY records=2 pending=0 dropped=0\n"
               "#40 first record\n"
               "#41 second record\n";
        *out_length = strlen(text);
        if (*out_length > capacity) {
            return 127;
        }
        memcpy(out, text, *out_length);
        return 0;
    }
    text = "hfoc: refused: command not allowed\n";
    *out_length = strlen(text);
    if (*out_length > capacity) {
        return 127;
    }
    memcpy(out, text, *out_length);
    return 127;
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
    while (length != 0U) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool load_authorized_key(const char *path, uint8_t *blob, size_t *length)
{
    FILE *file = fopen(path, "r");
    char line[1024];

    if (file == NULL) {
        return false;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);
    {
        char *field = line;
        char *space = strchr(line, ' ');
        if (space != NULL) {
            field = space + 1;
        }
        while (*field == ' ') {
            ++field;
        }
        {
            char *end = field;
            while (*end != '\0' && *end != ' ' && *end != '\r' &&
                   *end != '\n') {
                ++end;
            }
            *end = '\0';
        }
        *length = base64_decode(field, blob, 192U);
        return *length != 0U;
    }
}

int main(int argc, char **argv)
{
    int port = 2222;
    const char *authorized_path = NULL;
    const char *host_out = NULL;
    const char *scalar_path = NULL;
    uint8_t scalar[32];
    bool have_scalar = false;
    struct hfoc_ssh_session session;
    struct hfoc_ssh_crypto probe;
    uint8_t host_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    size_t host_blob_length = 0U;
    char host_base64[256];
    int server = -1;
    int client = -1;
    int option = 1;
    bool fragment = false;

    (void)option;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--authorized-key") == 0 &&
                   index + 1 < argc) {
            authorized_path = argv[++index];
        } else if (strcmp(argv[index], "--host-key-out") == 0 &&
                   index + 1 < argc) {
            host_out = argv[++index];
        } else if (strcmp(argv[index], "--scalar") == 0 &&
                   index + 1 < argc) {
            scalar_path = argv[++index];
        } else if (strcmp(argv[index], "--fragment") == 0) {
            fragment = true;
        }
    }
    if (authorized_path == NULL || host_out == NULL || scalar_path == NULL) {
        fprintf(stderr,
                "usage: %s --authorized-key FILE --host-key-out FILE "
                "--scalar FILE [--port N] [--fragment]\n",
                argv[0]);
        return 2;
    }
    urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) {
        perror("open /dev/urandom");
        return 2;
    }
    {
        FILE *file = fopen(scalar_path, "rb");
        if (file != NULL) {
            have_scalar = fread(scalar, 1U, sizeof(scalar), file) ==
                          sizeof(scalar);
            fclose(file);
        }
    }
    if (!hfoc_ssh_crypto_init(&probe)) {
        fprintf(stderr, "crypto init failed\n");
        return 2;
    }
    if (have_scalar) {
        if (!hfoc_ssh_crypto_host_key_load(&probe, scalar, host_rng, NULL)) {
            fprintf(stderr, "host key load failed\n");
            return 2;
        }
    } else {
        if (!hfoc_ssh_crypto_host_key_generate(&probe, host_rng, NULL)) {
            fprintf(stderr, "host key generate failed\n");
            return 2;
        }
        if (mbedtls_mpi_write_binary(&probe.host.MBEDTLS_PRIVATE(d), scalar,
                                     sizeof(scalar)) != 0) {
            fprintf(stderr, "host key export failed\n");
            return 2;
        }
        {
            FILE *file = fopen(scalar_path, "wb");
            if (file == NULL || fwrite(scalar, 1U, sizeof(scalar), file) !=
                                    sizeof(scalar)) {
                fprintf(stderr, "host key write failed\n");
                return 2;
            }
            fclose(file);
        }
    }
    host_blob_length = probe.host_blob_length;
    memcpy(host_blob, probe.host_blob, host_blob_length);
    hfoc_ssh_crypto_free(&probe);
    if (base64_encode(host_blob, host_blob_length, host_base64,
                      sizeof(host_base64)) == 0U) {
        fprintf(stderr, "host key encode failed\n");
        return 2;
    }
    {
        FILE *file = fopen(host_out, "w");
        if (file == NULL) {
            perror("host-key-out");
            return 2;
        }
        fprintf(file, "%s\n", host_base64);
        fclose(file);
    }
    fprintf(stderr, "HOSTKEY %s\n", host_base64);

    {
        uint8_t authorized[192];
        size_t authorized_length = 0U;
        if (!load_authorized_key(authorized_path, authorized,
                                 &authorized_length)) {
            fprintf(stderr, "authorized key parse failed\n");
            return 2;
        }
        if (!hfoc_ssh_session_init(&session, host_rng, NULL, command_dispatch,
                              NULL)) {
            fprintf(stderr, "session init failed\n");
            return 2;
        }
        memcpy(session.authorized_key, authorized, authorized_length);
        session.authorized_key_length = authorized_length;
        if (!hfoc_ssh_crypto_host_key_load(&session.crypto, scalar, host_rng, NULL)) {
            fprintf(stderr, "session host key load failed\n");
            return 2;
        }
        hfoc_ssh_session_start(&session, (uint32_t)(monotonic_seconds() * 1000.0));
    }
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 2;
    }
    {
        int one = 1;
        struct sockaddr_in address;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)port);
        if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("bind");
            return 2;
        }
        if (listen(server, 4) < 0) {
            perror("listen");
            return 2;
        }
    }
    fprintf(stderr, "LISTENING 127.0.0.1:%d\n", port);
    {
        struct pollfd waiter;
        waiter.fd = server;
        waiter.events = POLLIN;
        waiter.revents = 0;
        if (poll(&waiter, 1, 30000) <= 0) {
            fprintf(stderr, "accept timeout\n");
            return 3;
        }
    }
    client = accept(server, NULL, NULL);
    if (client < 0) {
        perror("accept");
        return 3;
    }
    fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
    {
        uint8_t buffer[8192];
        bool connection_closed = false;
        double deadline = monotonic_seconds() + 30.0;

        while (!hfoc_ssh_session_done(&session) &&
               monotonic_seconds() < deadline) {
            uint8_t output[8192];
            size_t output_length;
            struct pollfd fds[2];
            int ready;

            if (!hfoc_ssh_session_flush_output(&session)) {
                fprintf(stderr, "session flush failed: %s\n",
                        hfoc_ssh_session_reason(&session));
                break;
            }
            while ((output_length = hfoc_ssh_session_take_output(
                        &session, output, sizeof(output))) != 0U) {
                if (write_all(client, output, output_length) != 0) {
                    fprintf(stderr, "write failed\n");
                    connection_closed = true;
                    break;
                }
            }
            if (connection_closed) {
                break;
            }
            fds[0].fd = client;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            fds[1].fd = -1;
            ready = poll(fds, 1, 50);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }
            {
                ssize_t got = read(client, buffer, sizeof(buffer));
                if (got > 0) {
                    size_t offset = 0U;
                    while (offset < (size_t)got) {
                        const size_t step = fragment ? 1U : (size_t)got;
                        const enum hfoc_ssh_session_result result =
                            hfoc_ssh_session_feed(
                                &session, buffer + offset, step,
                                (uint32_t)(monotonic_seconds() * 1000.0));

                        offset += step;
                        if (result == HFOC_SSH_SESSION_PROTOCOL_ERROR ||
                            result == HFOC_SSH_SESSION_CRYPTO_ERROR) {
                            fprintf(stderr, "session error: %s\n",
                                    hfoc_ssh_session_reason(&session));
                            break;
                        }
                    }
                    if (hfoc_ssh_session_done(&session)) {
                        break;
                    }
                } else if (got == 0) {
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    break;
                }
            }
        }
        {
            uint8_t output[8192];
            size_t output_length;
            (void)hfoc_ssh_session_flush_output(&session);
            while ((output_length = hfoc_ssh_session_take_output(
                        &session, output, sizeof(output))) != 0U) {
                if (write_all(client, output, output_length) != 0) {
                    break;
                }
            }
        }
        fprintf(stderr, "SESSION state=%d reason=%s\n", (int)session.state,
                hfoc_ssh_session_reason(&session));
    }
    close(client);
    close(server);
    hfoc_ssh_session_free(&session);
    close(urandom_fd);
    return 0;
}
