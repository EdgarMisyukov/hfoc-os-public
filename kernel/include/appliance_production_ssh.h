#ifndef HFOC_APPLIANCE_PRODUCTION_SSH_H
#define HFOC_APPLIANCE_PRODUCTION_SSH_H

#include "appliance_production_admin_enrollment.h"
#include "appliance_production_network.h"
#include "appliance_ssh_identity.h"
#include "appliance_state_loader.h"
#include "hfoc_lwip_tcp.h"
#include "hfoc_ssh_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tty;
struct kernel_process;

/*
 * ITEM-601: the production SSH diagnostic owner.
 *
 * One listener on the management IPv4/port 22, one session at a time, one
 * persistent host identity and one locally enrolled client key.  The owner
 * starts only after the ordinary network, entropy, account and host-key gates
 * are ready; it never bypasses the local HOST_PROVISIONED unlock, never opens
 * a shell and never accepts an arbitrary key.  `hfoc status` is the one useful
 * command in this version; everything else is a named refusal.
 */
#define APPLIANCE_PRODUCTION_SSH_VERSION 1U
#define APPLIANCE_PRODUCTION_SSH_PORT 22U
#define APPLIANCE_PRODUCTION_SSH_TX_MAX 8192U

enum appliance_production_ssh_state {
    APPLIANCE_PRODUCTION_SSH_STATE_UNAVAILABLE = 0,
    APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_NETWORK = 1,
    APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ENTROPY = 2,
    APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_ACCOUNT = 3,
    APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_HOST_KEY = 4,
    APPLIANCE_PRODUCTION_SSH_STATE_AWAITING_CLIENT_KEY = 5,
    APPLIANCE_PRODUCTION_SSH_STATE_LISTENING = 6,
    APPLIANCE_PRODUCTION_SSH_STATE_FAILED = 7,
};

struct appliance_production_ssh {
    bool initialized;
    uint32_t version;
    struct appliance_state_loader *loader;
    const struct appliance_production_network *network;
    struct appliance_production_admin_enrollment *admin_enrollment;
    hfoc_ssh_rng_fn rng;
    void *rng_context;
    struct appliance_ssh_identity identity;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_ssh_session session;
    struct tty *tty;
    struct kernel_process *shell_child;
    struct tcp_pcb *pcb;
    struct network_ipv4_config config;
    bool config_valid;
    uint32_t generation;
    uint8_t tx[APPLIANCE_PRODUCTION_SSH_TX_MAX];
    size_t tx_length;
    size_t tx_offset;
    enum appliance_production_ssh_state state;
    const char *reason;
    uint32_t accept_count;
    uint32_t session_count;
    uint32_t refused_count;
    bool revoke_pending;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t last_attempt_ms;
    bool listening;
    bool session_active;
};

void appliance_production_ssh_init(
    struct appliance_production_ssh *ssh,
    struct appliance_state_loader *loader,
    const struct appliance_production_network *network,
    struct appliance_production_admin_enrollment *admin_enrollment,
    hfoc_ssh_rng_fn rng,
    void *rng_context);
void appliance_production_ssh_poll(
    struct appliance_production_ssh *ssh,
    uint32_t now_ms);
void appliance_production_ssh_shutdown(
    struct appliance_production_ssh *ssh);
/*
 * ITEM-601: credential reset revokes the enrolled client key and closes the
 * live session; the host key remains (it is the machine identity), and the
 * endpoint returns to AWAITING_CLIENT_KEY until a new key is enrolled.
 */
bool appliance_production_ssh_revoke(struct appliance_production_ssh *ssh);
enum appliance_production_ssh_state appliance_production_ssh_state(
    const struct appliance_production_ssh *ssh);
const char *appliance_production_ssh_reason(
    const struct appliance_production_ssh *ssh);
bool appliance_production_ssh_listening(
    const struct appliance_production_ssh *ssh);
const struct appliance_ssh_identity *
appliance_production_ssh_identity(
    const struct appliance_production_ssh *ssh);
size_t appliance_production_ssh_render(
    const struct appliance_production_ssh *ssh,
    char *buffer,
    size_t capacity);
/*
 * ITEM-613: the server's public host key in OpenSSH form, for the diagnostic
 * snapshot.  Only the public blob and its SHA256 fingerprint leave the machine;
 * the private scalar, the session keys and the authorized client key never do.
 * An unavailable identity is a named record, never an empty or invented key.
 */
size_t appliance_production_ssh_render_hostkey(
    const struct appliance_production_ssh *ssh,
    const char *address,
    uint16_t port,
    char *buffer,
    size_t capacity);
const char *appliance_production_ssh_state_string(
    enum appliance_production_ssh_state state);

#endif
