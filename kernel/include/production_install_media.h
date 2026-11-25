#ifndef HFOC_PRODUCTION_INSTALL_MEDIA_H
#define HFOC_PRODUCTION_INSTALL_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"
#include "installer_hardware_discovery.h"
#include "forward_config.h"
#include "ipv4_config.h"

/*
 * ITEM-394 product install entry point on the installer media.
 *
 * ITEM-351 built the production installer controller and ITEM-391 gave it the
 * declared product layout, but the only thing that ever drove it was the QEMU
 * fixture under `HFOC_PRODUCTION_INSTALLER_QEMU_TEST`, and
 * `production_installer_confirm()` needs a confirmation nothing on real media
 * could give: the acceptance host has no PS/2 keyboard and no USB HID path
 * yet (ITEM-395), so an interactive prompt is not reachable on hardware.
 *
 * This module owns the product decision that turns an ordinary boot of the
 * installer medium into an install run, and nothing else: it does not write a
 * sector, does not plan a layout and does not know about the controller.  It
 * parses the two declarations the operator makes - the boot entry's command
 * line and the media declaration file - selects the one disk the operator
 * named, and renders the bounded single-line records the flow publishes.
 *
 * Two declarations are required, and neither one alone can start a write:
 *
 *   * the boot entry selects the install mode (`hfoc.install=unattended` or
 *     `hfoc.install=console`); an entry without that directive boots the
 *     appliance and never installs;
 *   * the media declaration file carries the confirmation source and the
 *     serial of the one disk the operator authorised.  A medium without it,
 *     or with a declaration that does not name a present disk exactly once,
 *     refuses instead of guessing a target.
 *
 * "Never touch a disk the operator did not select" is therefore a property of
 * the parsed declaration plus the discovery report: the selected disk id and
 * its identity string travel into the ITEM-340/341 plan, and the controller
 * re-checks both before the first write.
 */

#include "ipv4_config.h"

#define PRODUCTION_INSTALL_MEDIA_VERSION 1U
#define PRODUCTION_INSTALL_MEDIA_TOKEN_MAX 32U
#define PRODUCTION_INSTALL_MEDIA_RECORD_MAX 512U
/*
 * ITEM-638: two different inputs, two different bounds.  The media
 * declaration is the token file the stage script writes: the largest standard
 * declaration (confirmation, target, management, update layout, SSH key, a
 * static address and an enabled forward with four clients) is 540 bytes on the
 * owner medium and 580 bytes with the widest four-client test values, so 512
 * refused the owner's own medium before the flow looked at a disk.  Every
 * standard field at its own maximum still fits below 800 bytes.  The
 * finite 1024-byte bound keeps the parser allocation-free and still rejects a
 * file one byte over it.  The boot command line is not that file and keeps its
 * own 512-byte bound; it must not grow just because the token did.
 */
#define PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX 1024U
#define PRODUCTION_INSTALL_MEDIA_COMMAND_LINE_MAX 512U
#define PRODUCTION_INSTALL_MEDIA_NETWORK_CMDLINE_MAX 128U
#define PRODUCTION_INSTALL_MEDIA_FORWARD_CMDLINE_MAX 256U
#define PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_MAX 64U

/* The declaration directives; one per line, `#` starts a comment. */
#define PRODUCTION_INSTALL_MEDIA_CONFIRM_DIRECTIVE "HFOC-INSTALL-CONFIRM="
#define PRODUCTION_INSTALL_MEDIA_TARGET_DIRECTIVE "HFOC-INSTALL-TARGET="
#define PRODUCTION_INSTALL_MEDIA_MANAGEMENT_DIRECTIVE "HFOC-INSTALL-MANAGEMENT="
/*
 * ITEM-398: the layout the operator declares.  `whole-disk` is the destructive
 * clean install - it is the deliberate choice for a disk whose content is
 * expendable - and `preserve` keeps every entry, the MBR and the ESP contents of
 * the system that is already there.  Without the directive the run decides from
 * the disk: a target that carries anything but an ESP is preserved, never wiped.
 */
#define PRODUCTION_INSTALL_MEDIA_LAYOUT_DIRECTIVE "HFOC-INSTALL-LAYOUT="
#define PRODUCTION_INSTALL_MEDIA_LAYOUT_WHOLE_DISK "whole-disk"
#define PRODUCTION_INSTALL_MEDIA_LAYOUT_PRESERVE "preserve"
#define PRODUCTION_INSTALL_MEDIA_HOST_SEED_DIRECTIVE \
    "HFOC-INSTALL-HOST-SEED="
#define PRODUCTION_INSTALL_MEDIA_HOST_SEED_REQUIRED "required"
/*
 * ITEM-606: the owner-confirmed SSH client public-key fingerprint carried by
 * the medium.  The module itself is the 88-character base64 P-256 point; this
 * directive binds it to the fingerprint the operator confirmed on Debian, so a
 * key module cannot be accepted silently by a medium that did not declare it.
 */
#define PRODUCTION_INSTALL_MEDIA_SSH_KEY_DIRECTIVE "HFOC-INSTALL-SSH-KEY="
#define PRODUCTION_INSTALL_MEDIA_SSH_KEY_MAX 64U
/*
 * ITEM-586: `update` is the explicit state-preserving reinstall of an
 * appliance that is already on the target disk.  It is deliberately a third
 * value, not an inference: `whole-disk` still erases the previous run's
 * state, and an update is never selected because a disk happened to carry an
 * HFOC partition.
 */
#define PRODUCTION_INSTALL_MEDIA_LAYOUT_UPDATE "update"
/*
 * ITEM-509 increment 27 (509.7c-2): the language the medium declares.  It is a
 * property of the medium, not of a boot entry: one flash drive carries one
 * language, and the stage script writes the directive from a single variable.
 * The value is parsed by the one catalog owner (`appliance_language_parse()`:
 * exactly `en` or `ru`); an absent directive is `en`, and a misspelled value
 * refuses the whole declaration with BAD_TEXT - the same fail-closed rule
 * LAYOUT and NETWORK follow, never a guess.
 */
#define PRODUCTION_INSTALL_MEDIA_LANGUAGE_DIRECTIVE "HFOC-INSTALL-LANGUAGE="
#define PRODUCTION_INSTALL_MEDIA_MANAGEMENT_LOCAL "local-only"
#define PRODUCTION_INSTALL_MEDIA_MANAGEMENT_INTERFACE "management-interface"
/*
 * ITEM-446: the network the operator declares on the medium.  `dhcp` is the
 * pre-item behaviour and stays the default when nothing is declared; `static`
 * requires the address (in `a.b.c.d/prefix` form), the gateway and the DNS, is
 * validated before the first write through the same IPv4 owner every other
 * network surface uses, and travels into the installed boot entry so the
 * appliance comes up on the declared address instead of discovering one.
 */
#define PRODUCTION_INSTALL_MEDIA_NETWORK_DIRECTIVE "HFOC-INSTALL-NETWORK="
#define PRODUCTION_INSTALL_MEDIA_ADDRESS_DIRECTIVE "HFOC-INSTALL-ADDRESS="
#define PRODUCTION_INSTALL_MEDIA_GATEWAY_DIRECTIVE "HFOC-INSTALL-GATEWAY="
#define PRODUCTION_INSTALL_MEDIA_DNS_DIRECTIVE "HFOC-INSTALL-DNS="
/*
 * ITEM-654: one explicit SNTP server for the installed appliance.  It is a
 * literal unicast IPv4 address, independent of the addressing mode: the
 * operator may name it with DHCP or with a static address.  An absent
 * directive keeps the pre-item behaviour, the active gateway.
 */
#define PRODUCTION_INSTALL_MEDIA_SNTP_DIRECTIVE "HFOC-INSTALL-SNTP="
#define PRODUCTION_INSTALL_MEDIA_NETWORK_DHCP "dhcp"
#define PRODUCTION_INSTALL_MEDIA_NETWORK_STATIC "static"
/*
 * ITEM-449: the forward the operator declares on the medium.  `on` requires the
 * target, both ports and at least one client; `off` is the explicit "no
 * forward".  The declaration is validated before the first write through the
 * one forward owner, and it travels into the installed boot entry, so the
 * installed appliance states the rule it will serve instead of reading it from
 * a default.
 */
#define PRODUCTION_INSTALL_MEDIA_FORWARD_DIRECTIVE "HFOC-INSTALL-FORWARD="
#define PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_DIRECTIVE \
    "HFOC-INSTALL-FORWARD-TARGET="
#define PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_PORT_DIRECTIVE \
    "HFOC-INSTALL-FORWARD-TARGET-PORT="
#define PRODUCTION_INSTALL_MEDIA_FORWARD_LISTEN_PORT_DIRECTIVE \
    "HFOC-INSTALL-FORWARD-LISTEN-PORT="
#define PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_DIRECTIVE \
    "HFOC-INSTALL-FORWARD-CLIENTS="
#define PRODUCTION_INSTALL_MEDIA_FORWARD_ON "on"
#define PRODUCTION_INSTALL_MEDIA_FORWARD_OFF "off"

/* The boot entry command lines the installer medium declares. */
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_UNATTENDED "hfoc.install=unattended"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_CONSOLE "hfoc.install=console"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_NONE "hfoc.install=none"
/*
 * The management scope the running appliance publishes.  The default stays
 * deny-by-default (LOCAL_ONLY, no network ingress); only an explicit
 * `hfoc.management=management-interface` declaration on the boot entry opts the
 * management plane in to the operator's network, and the console record states
 * which of the two is in force.
 */
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_LOCAL \
    "hfoc.management=local-only"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_INTERFACE \
    "hfoc.management=management-interface"
/*
 * ITEM-446: the network the installed boot entry declares.  The installed
 * configuration carries these directives either way, so the appliance states
 * its address instead of falling back to a default, exactly as ITEM-399 did
 * for the management scope.
 */
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_DHCP "hfoc.network=dhcp"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_STATIC "hfoc.network=static"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_ADDRESS_PREFIX "hfoc.address="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_GATEWAY_PREFIX "hfoc.gateway="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_DNS_PREFIX "hfoc.dns="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_SNTP_PREFIX "hfoc.sntp="
/*
 * ITEM-449: the forward the installed boot entry declares.  The installed
 * configuration carries it, so the appliance states the forwarding rule it was
 * installed with - or states `hfoc.forward=off` - instead of falling back to a
 * default.  Every value is a separate bounded token; the access rule is the
 * client list, and a list that is absent is not "everyone".
 */
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_ON "hfoc.forward=on"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_OFF "hfoc.forward=off"
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PREFIX \
    "hfoc.forward.target="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PORT_PREFIX \
    "hfoc.forward.target-port="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_LISTEN_PORT_PREFIX \
    "hfoc.forward.listen-port="
#define PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_CLIENTS_PREFIX \
    "hfoc.forward.clients="

enum production_install_media_mode {
    PRODUCTION_INSTALL_MEDIA_MODE_NONE = 0,
    PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED = 1,
    PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE = 2,
};

enum production_install_media_action {
    PRODUCTION_INSTALL_MEDIA_ACTION_NONE = 0,
    PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED = 1,
    PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE = 2,
    PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED = 3,
};

enum production_install_media_refusal {
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE = 0,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NOT_INSTALL_MEDIA = 1,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING = 2,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MALFORMED = 3,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_CONFIRM_MISSING = 4,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_MISSING = 5,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_FIRMWARE_UNSUPPORTED = 6,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE = 7,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NO_DISK = 8,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND = 9,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_AMBIGUOUS = 10,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_MODE_MALFORMED = 11,
    /*
     * 12 was CONSOLE_TOKEN_MISMATCH: ITEM-495 removed the console token, so
     * nothing produces that refusal any more.  The number stays retired - the
     * refusals after it keep the values their records and slices pin.
     */
    /* ITEM-446: the declared network cannot be honoured; nothing is written. */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE = 13,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED = 14,
    /* ITEM-449: the declared forward cannot be honoured; nothing is written. */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_INCOMPLETE = 15,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_MALFORMED = 16,
    /*
     * ITEM-490: the operator entry showed the storage screen and the operator
     * left it without naming a disk.  Nothing is chosen and nothing is written:
     * an install nobody asked for is not an install.
     */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR = 17,
    /*
     * ITEM-501: the operator gave up a partition on the screen (a preserving
     * install), but the medium declared the destructive whole-disk layout.  The
     * two decisions contradict each other, so nothing is written and the
     * conflict is named instead of one side silently winning.
     */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_RELEASE_CONFLICT = 18,
    /* ITEM-598: the personalised medium did not carry a usable host seed. */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_MISSING = 19,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID = 20,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_TARGET_MISMATCH = 21,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_BUILD_MISMATCH = 22,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_CREDENTIAL_REQUIRED = 23,
    /* ITEM-606: the declared SSH public key is absent, malformed or mismatched. */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_MISSING = 24,
    PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_INVALID = 25,
    /*
     * ITEM-638: the declaration file exists but is larger than the finite
     * format bound.  This is not a missing token and not a malformed value:
     * the operator has to read the real reason the write did not start.
     */
    PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_TOO_LARGE = 26,
};

enum production_install_media_status {
    PRODUCTION_INSTALL_MEDIA_OK = 0,
    PRODUCTION_INSTALL_MEDIA_NULL = 1,
    PRODUCTION_INSTALL_MEDIA_BAD_VERSION = 2,
    PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT = 3,
    PRODUCTION_INSTALL_MEDIA_BAD_TEXT = 4,
    PRODUCTION_INSTALL_MEDIA_OVERFLOW = 5,
};

struct production_install_media_declaration {
    uint32_t version;
    bool present;
    bool confirm_declared;
    char confirm[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    bool target_declared;
    char target[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    bool management_interface;
    /* ITEM-398: the declared layout, when the operator declared one. */
    bool layout_declared;
    bool layout_preserve;
    bool layout_update;
    bool host_seed_required;
    /* ITEM-606: the owner-confirmed SSH client key the medium may import. */
    bool ssh_key_declared;
    char ssh_key_fingerprint[PRODUCTION_INSTALL_MEDIA_SSH_KEY_MAX];
    /*
     * ITEM-509 increment 27 (509.7c-2): the language the medium declares.
     * `language` is zero (APPLIANCE_LANGUAGE_EN) when the directive is absent,
     * so every existing host call that zeroes the declaration and every medium
     * built before the directive keeps the default language.  The install
     * persists it only when it is not the default.
     */
    bool language_declared;
    enum appliance_language language;
    /*
     * ITEM-446: the declared network.  `network_static` is only meaningful
     * when `network_declared` is true; the three values are the operator's
     * own bytes, validated by the IPv4 owner before the first write.
     */
    bool network_declared;
    bool network_static;
    bool address_declared;
    char address[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    bool gateway_declared;
    char gateway[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    bool dns_declared;
    char dns[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    /* ITEM-654: the optional explicit SNTP server, a literal IPv4. */
    bool sntp_declared;
    char sntp[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    /*
     * ITEM-449: the declared forward.  `forward_enabled` is only meaningful
     * when `forward_declared` is true; the four values are the operator's own
     * bytes, validated by the forward owner before the first write.
     */
    bool forward_declared;
    bool forward_enabled;
    char forward_target[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char forward_target_port[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char forward_listen_port[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char forward_clients[PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_MAX];
};

/*
 * ITEM-446: the network an installed boot entry declares.  It is parsed from
 * the running boot command line, not from the media declaration, because that
 * is where the installed configuration states it.
 */
struct production_install_media_boot_network {
    bool declared;
    bool is_static;
    char address[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char gateway[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char dns[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    /*
     * A static declaration that passed the one IPv4 validator.  It is only
     * meaningful when `is_static` and `config_valid` are both true; the caller
     * applies it without parsing anything again.
     */
    bool config_valid;
    struct network_ipv4_config config;
    /*
     * ITEM-654: `hfoc.sntp=` is parsed independently of the addressing mode.
     * A malformed value is recorded as declared-but-invalid instead of
     * refusing the whole network declaration: the appliance comes up on the
     * declared address, reports `sntp=INVALID`, and leaves the clock
     * unsynchronized rather than silently changing the source.
     */
    bool sntp_declared;
    bool sntp_valid;
    char sntp[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    struct network_ipv4_address sntp_address;
};

/*
 * ITEM-449: the forward an installed boot entry declares.  It is parsed from
 * the running boot command line, not from the media declaration, because that
 * is where the installed configuration states it.
 */
struct production_install_media_boot_forward {
    bool declared;
    bool enabled;
    char target[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char target_port[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char listen_port[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX];
    char clients[PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_MAX];
    /*
     * An enabled declaration that passed the one forward owner.  It is only
     * meaningful when `enabled` and `config_valid` are both true; the caller
     * applies it without parsing anything again.
     */
    bool config_valid;
    struct forward_config config;
};

struct production_install_media_target {
    uint32_t disk_id;
    enum installer_disk_controller_kind controller;
    uint64_t block_count;
    char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH];
};

struct production_install_media_plan {
    uint32_t version;
    enum production_install_media_mode mode;
    enum production_install_media_action action;
    enum production_install_media_refusal refusal;
    bool selected;
    struct production_install_media_target target;
};

enum production_install_media_status production_install_media_mode_parse(
    const char *command_line,
    size_t command_line_length,
    enum production_install_media_mode *mode);
/*
 * The management-scope opt-in carried by the boot entry.  Returns true only
 * when the entry declares the management interface explicitly; an entry that
 * declares local-only, declares nothing, or declares both keeps the default
 * deny-by-default scope.
 */
bool production_install_media_management_opt_in(
    const char *command_line,
    size_t command_line_length);
/*
 * ITEM-399: the scope the boot entry *declares*, when it declares exactly one.
 * An installed appliance always declares one - the installed configuration (or
 * the generated menu that replaced it) carries
 * `cmdline: hfoc.management=...` - so the binding record can state that the
 * scope came from the boot declaration even when the declared scope is
 * local-only, instead of reporting an explicit declaration as a default.
 * Returns false for an entry that declares nothing or declares both scopes.
 */
bool production_install_media_management_declared(
    const char *command_line,
    size_t command_line_length,
    bool *management_interface);
enum production_install_media_status
production_install_media_declaration_parse(
    const char *text,
    size_t text_length,
    struct production_install_media_declaration *declaration);
/*
 * ITEM-490: the operator's choice, applied to the declaration.  The disk the
 * operator named replaces the disk the medium declared, so the plan that follows
 * selects it; every other declared word stays as the medium wrote it.  False -
 * and the declaration unchanged - when the identity cannot be carried.
 */
bool production_install_media_declaration_set_target(
    struct production_install_media_declaration *declaration,
    const char *identity);
enum production_install_media_status production_install_media_plan_build(
    enum production_install_media_mode mode,
    const struct production_install_media_declaration *declaration,
    enum installer_firmware_mode firmware_mode,
    const struct installer_discovery_report *report,
    struct production_install_media_plan *plan);
bool production_install_media_confirm_matches(
    const struct production_install_media_declaration *declaration,
    const char *token,
    size_t token_length);
size_t production_install_media_candidate_count(
    const struct installer_discovery_report *report);
const struct installer_discovery_disk *production_install_media_candidate(
    const struct installer_discovery_report *report,
    size_t index);

enum production_install_media_status
production_install_media_render_declaration(
    const struct production_install_media_declaration *declaration,
    enum production_install_media_mode mode,
    char *buffer,
    size_t capacity,
    size_t *length);
enum production_install_media_status
production_install_media_render_candidate(
    const struct installer_discovery_disk *disk,
    const char *model,
    bool selected,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * ITEM-495: who authorized the write.  The unattended entry is authorized by
 * the declaration the medium carries; the operator entry is authorized on the
 * machine, by the person who read the warning and accepted it - the console
 * token that used to carry that decision is gone, and the record still says
 * which of the two it was.
 */
enum production_install_media_confirmation {
    /* The medium's own declaration: the unattended entry. */
    PRODUCTION_INSTALL_MEDIA_CONFIRMATION_MEDIA = 0,
    /* The operator accepted the warning on the storage screen. */
    PRODUCTION_INSTALL_MEDIA_CONFIRMATION_OPERATOR = 1,
};

enum production_install_media_status
production_install_media_render_confirmation(
    const struct production_install_media_declaration *declaration,
    enum production_install_media_mode mode,
    const struct production_install_media_target *target,
    enum production_install_media_confirmation confirmation,
    uint64_t uptime_seconds,
    char *buffer,
    size_t capacity,
    size_t *length);
enum production_install_media_status
production_install_media_render_refusal(
    enum production_install_media_refusal refusal,
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length);
enum production_install_media_status
production_install_media_render_applied(
    const struct production_install_media_declaration *declaration,
    const struct production_install_media_target *target,
    uint64_t uptime_seconds,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * ITEM-446: build the IPv4 configuration the declaration asks for.  A static
 * declaration is only accepted when it is complete and when the address, the
 * prefix, the gateway and the DNS pass the one IPv4 validator the tree already
 * has; a DHCP declaration and an absent declaration produce OK with
 * `is_static_out` false, which is the pre-item behaviour.
 */
enum production_install_media_status production_install_media_network_config(
    const struct production_install_media_declaration *declaration,
    bool *is_static_out,
    struct network_ipv4_config *config_out);
/*
 * Parse the network directives of a boot command line.  The function never
 * guesses: an unknown mode, a missing value for a declared one, or a value
 * that is not a valid IPv4 configuration is reported through the status, and
 * `declared` stays false when the command line says nothing about the network.
 */
enum production_install_media_status production_install_media_network_boot(
    const char *command_line,
    size_t command_line_length,
    struct production_install_media_boot_network *network_out);
/*
 * Render the whole command line the installed boot entry carries: the
 * management scope the operator declared, then the declared network when one
 * was declared.  This is the one place that composes the two, so the sealed
 * configuration path and the generated-menu path cannot disagree.
 */
enum production_install_media_status
production_install_media_installed_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out);
/*
 * Render the command-line fragment the installed boot entry carries for the
 * declared network: `hfoc.network=dhcp`, or
 * `hfoc.network=static hfoc.address=... hfoc.gateway=... hfoc.dns=...`.
 * Nothing declared renders the empty string.
 */
enum production_install_media_status production_install_media_network_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out);
/*
 * ITEM-449: build the forward the declaration asks for.  A declaration that is
 * absent or explicitly off produces OK with the forward disabled, which is the
 * pre-item behaviour; an enabled declaration is only accepted when it is
 * complete and when the one forward owner accepts every part.
 */
enum production_install_media_status
production_install_media_forward_config(
    const struct production_install_media_declaration *declaration,
    struct forward_config *config_out);
/*
 * Parse the forward directives of a boot command line.  The function never
 * guesses: an unknown mode, a missing value for a declared one, or a value
 * that is not a valid forward is reported through the status, and `declared`
 * stays false when the command line says nothing about forwarding.
 */
enum production_install_media_status production_install_media_forward_boot(
    const char *command_line,
    size_t command_line_length,
    struct production_install_media_boot_forward *forward_out);
/*
 * Render the command-line fragment the installed boot entry carries for the
 * declared forward: `hfoc.forward=on hfoc.forward.target=... ...`, or
 * `hfoc.forward=off`.  Nothing declared renders the empty string.
 */
enum production_install_media_status production_install_media_forward_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out);
/*
 * The console path publishes what the operator must type and waits.
 */
enum production_install_media_status
production_install_media_render_pending(
    const struct production_install_media_declaration *declaration,
    const struct production_install_media_target *target,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * ITEM-500: the operator entry is prepared and the disk is still the
 * operator's to name, because the disk the medium declared is not on this
 * machine.  The record names the declared disk, says it is not here, and
 * publishes `disk=NONE` until the screen hands one back.
 */
enum production_install_media_status
production_install_media_render_awaiting_choice(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * ITEM-500: the one screen line that says why the operator is being asked to
 * choose - the declared disk is not on this machine.  It is a screen line, not
 * a record, so it stays short, mixed case and free of markers.
 */
/*
 * ITEM-509 increment 15: the one line the storage screen shows above the disks
 * is rendered in the caller's language; the declared target itself stays the
 * machine value the medium was built with.
 */
enum production_install_media_status
production_install_media_render_choice_note(
    const struct production_install_media_declaration *declaration,
    enum appliance_language language,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * A refusal that comes from the controller or the planner after the media
 * selected a target (an unfit plan, an unverified media, unsupported
 * hardware) states the module's own status instead of inventing a reason.
 */
enum production_install_media_status
production_install_media_render_controller_refusal(
    const char *status_text,
    const char *detail_text,
    char *buffer,
    size_t capacity,
    size_t *length);
/*
 * A failure after the first write has happened is never published as a
 * refusal with `writes=0`; the stage and the module status are stated.
 */
enum production_install_media_status
production_install_media_render_failure(
    const char *stage,
    const char *status_text,
    char *buffer,
    size_t capacity,
    size_t *length);

const char *production_install_media_mode_string(
    enum production_install_media_mode mode);
const char *production_install_media_action_string(
    enum production_install_media_action action);
const char *production_install_media_refusal_string(
    enum production_install_media_refusal refusal);
const char *production_install_media_status_string(
    enum production_install_media_status status);
const char *production_install_media_controller_string(
    enum installer_disk_controller_kind controller);
const char *production_install_media_management_string(
    const struct production_install_media_declaration *declaration);

#endif
