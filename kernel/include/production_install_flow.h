#ifndef HFOC_PRODUCTION_INSTALL_FLOW_H
#define HFOC_PRODUCTION_INSTALL_FLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "installer_hardware_discovery.h"
#include "installer_scheme.h"
#include "appliance_storage_report.h"

/*
 * ITEM-394 product install run on the installer medium.
 *
 * `production_install_flow_run()` is the ordinary-boot entry point the
 * installer medium reaches: it consumes the declarations the kernel collected
 * from the medium (the boot entry's command line, the media declaration file,
 * the installer payloads and their sealed manifest, and the ITEM-397 installed
 * boot set), publishes the candidate disks and the plan preview in the
 * single-line marker style, and performs the install through the existing
 * ITEM-351 controller, ITEM-340/341 planner and writer, the ITEM-391 layout
 * contract and the ITEM-397 installed boot stage.
 *
 * It never invents a target: the disk it writes is the one the media
 * declaration names by serial, re-checked against the identity string the
 * controller carries into the plan before the first write.
 *
 * ITEM-411: a run that completes reboots the machine, after publishing
 * `[PRODUCTION-INSTALL] complete disk=… identity=… medium=REMOVE reset=… in=…s`
 * and giving the operator a bounded few seconds to pull the installation medium
 * out (the firmware prefers the removable medium, so a machine left with the
 * stick in boots the installer again).  The reset happens only at the end of
 * the success path: a refused, failed or console-pending run keeps its current
 * behaviour and leaves the machine running.
 *
 * `production_install_flow_engaged()` reports whether this boot is a product
 * install run (an install entry on a media that carries a complete
 * declaration).  The installer image's QEMU fixture is skipped then, so the
 * fixture and the product flow never run in the same boot.
 */

#define PRODUCTION_INSTALL_FLOW_VERSION 1U
#define PRODUCTION_INSTALL_FLOW_RELEASE_ID 1U
#define PRODUCTION_INSTALL_FLOW_POLICY_GENERATION 1U

/*
 * ITEM-500: the one screen line the storage screen shows when the disk the
 * medium declared is not on this machine.  It is bounded by the storage
 * screen's own line width, so the note can never be cut in half by the screen
 * that prints it.
 */
#define PRODUCTION_INSTALL_FLOW_CHOICE_NOTE_MAX 128U

struct production_install_flow_media {
    uint32_t version;
    const char *command_line;
    size_t command_line_length;
    const char *declaration;
    size_t declaration_length;
    enum installer_firmware_mode firmware_mode;
    uint32_t cpu_cores;
    uint64_t ram_bytes;
    const unsigned char *boot_payload;
    size_t boot_payload_size;
    const unsigned char *limine_config;
    size_t limine_config_size;
    const unsigned char *kernel;
    size_t kernel_size;
    /*
     * ITEM-397: the loader the installed ESP carries on the removable-media
     * path (the medium's own Limine UEFI binary) and the sealed boot chain of
     * the installed set, both staged with the kernel and the configuration.
     */
    const unsigned char *loader;
    size_t loader_size;
    const unsigned char *manifest;
    size_t manifest_size;
    /*
     * ITEM-623: the root initramfs module the medium carries.  The install
     * stages it beside the kernel so the installed kernel mounts its own `/`
     * instead of falling back to the empty RAM root.
     */
    const unsigned char *rootfs;
    size_t rootfs_size;
    const unsigned char *envelope;
    size_t envelope_size;
    const unsigned char *payload_blob;
    size_t payload_blob_size;
    /*
     * ITEM-598: the bounded host-provisioned seed record carried only by a
     * personalised install medium.  The flow parses it before the operator can
     * confirm a write and never copies it into a log or command line.
     */
    const unsigned char *host_seed;
    size_t host_seed_size;
    /*
     * ITEM-606: the owner-confirmed SSH client public key carried as the
     * medium's `hfoc-ssh-key.point` module.  The payload is public; the
     * private key never reaches the appliance or this structure.
     */
    const unsigned char *ssh_key;
    size_t ssh_key_size;
};

void production_install_flow_run(
    const struct production_install_flow_media *media);
bool production_install_flow_engaged(void);
/*
 * ITEM-490: the operator entry's mandatory storage screen.
 *
 * The flow runs before the machine has a console - and before interrupts are
 * enabled - so it cannot wait for a person.  On the operator entry it publishes
 * what it prepared and returns; the boot's own loop, where local input is live,
 * asks whether the choice is still pending, shows the screen, and hands the disk
 * the operator named back to the flow.  The chosen disk then becomes the
 * declared target, which is the disk the console confirmation writes.
 */
bool production_install_flow_awaiting_choice(void);
/*
 * ITEM-500: the line the storage screen shows above the disks when the medium's
 * declared disk is not on this machine.  Empty when there is nothing to
 * explain; never null.
 */
const char *production_install_flow_choice_note(void);
/*
 * ITEM-500: the refusal of an operator entry that refused before the storage
 * screen could open (no disk to choose from, an unsupported firmware, an
 * unreadable report, a declaration that cannot be honoured).  The boot loop
 * paints that reason on the screen and waits for a key, so a machine that
 * cannot install says so instead of booting on in silence.  The token is the
 * stable refusal name the records use (`FIRMWARE_UNSUPPORTED`, `NO_DISK`, ...);
 * NULL means "nothing to show".
 */
const char *production_install_flow_operator_refusal(void);
/* The discovery report the screen lists; never null on the install medium. */
const struct installer_discovery_report *production_install_flow_report(void);
/*
 * ITEM-518: the observed storage report the refusal page prints - the
 * controllers the probe matched, the disks it brought up and the reason each
 * unusable disk carries.  Filled when the run built its discovery; a run that
 * refused before that returns an unprobed report, which the page prints as
 * "not probed" instead of inventing zeroes.
 */
void production_install_flow_storage_report(struct appliance_storage_report *out);
/*
 * Apply the operator's choice by discovery id.  False - and no change to the
 * run - when the id names no selectable disk, when the choice cannot be carried
 * into the declaration, or when the plan refuses it; a plan that refuses
 * refuses the run itself.
 */
bool production_install_flow_choose_disk(uint32_t disk_id);
/*
 * ITEM-501: the operator gave up an existing partition of the chosen disk on the
 * partition screen.  The install becomes a preserving install whose three roles
 * are created in the space that partition occupied, and `partition_index` is the
 * discovery's own index the warning page named.  It must be called before the
 * choice is confirmed; a later call replaces the earlier partition, and zero
 * clears it back to the free-space path.
 */
void production_install_flow_release_partition(uint32_t partition_index);
/*
 * The operator left the screen without choosing: nothing is prepared any more,
 * the run is refused with CANCELLED_BY_OPERATOR and nothing is written.
 */
void production_install_flow_cancel_by_operator(void);
/*
 * ITEM-495: the operator accepting the warning on the storage screen confirms
 * the prepared install.  There is no console token any more: the person who
 * read "everything on this disk will be deleted" and pressed ENTER is the
 * confirmation, and the record names that source (`confirm
 * source=operator-screen`) exactly as it named the medium's token before.  The
 * disk is still the one the operator chose on the screen.
 */
bool production_install_flow_confirm_by_operator(void);

/*
 * ITEM-514: the install scheme the operator chose on the scheme screen.
 *
 * The medium's declaration and the disk's contents still produce the default
 * (`production_install_flow_default_scheme()`), but they are a default and not
 * a lock: an operator may choose a scheme that is more destructive than the
 * medium declared, and the record then names both sources -
 * `[PRODUCTION-INSTALL] scheme declared=… chosen=… source=operator` and the
 * layout record's `chosen=` field.  `choose_scheme()` is the only way a
 * scheme is set; NONE is refused, and setting a scheme that is not MANUAL
 * withdraws any previously given-up partition, because the two decisions are
 * one decision.
 */
enum installer_storage_scheme production_install_flow_declared_scheme(void);
enum installer_storage_scheme production_install_flow_default_scheme(void);
/*
 * ITEM-514 (R03): the chosen disk's partition table could not be read, so its
 * contents are unknown.  The erase page says so; a machine that never chose a
 * disk reports false.
 */
bool production_install_flow_contents_unknown(void);
bool production_install_flow_choose_scheme(
    enum installer_storage_scheme scheme);

/*
 * ITEM-506 increment 4: the account the operator set on the install screen.
 *
 * The screen collects the login and the password; the flow owns the policy and
 * the record.  `set_operator_credential()` validates the pair with the
 * credential model, derives the PBKDF2 verifier with a salt from the bound
 * source, and keeps only the model's record (login, salt, iteration count,
 * verifier) - the password is never stored.  The installer kernel has no
 * hardware entropy owner, so the salt source is a caller seam: the ordinary
 * install binds a unique derived salt and the host proof binds a fixed one.
 *
 * `apply()` then writes the record into the installed system's state, proves
 * it by reading it back, and publishes
 * `[PRODUCTION-INSTALL] admin=SET login=<login> persisted=VERIFIED`; an
 * install with no operator credential publishes
 * `admin=UNSET reason=UNATTENDED` (the unattended entry) or
 * `admin=UNSET reason=OPERATOR_DECLINED` (the operator cancelled the screen),
 * and writes no administrator record at all.
 */
enum production_install_flow_credential_status {
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK = 0,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_NULL = 1,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_STATE = 2,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_LOGIN = 3,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD = 4,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_NO_SALT = 5,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_DENIED = 6,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_UNAVAILABLE = 7,
};
enum production_install_flow_credential_requirement {
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_OPTIONAL_SET = 0,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_SET = 1,
    PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY = 2,
};
typedef bool (*production_install_flow_random_fn)(
    void *context,
    uint8_t *output,
    size_t length);
enum production_install_flow_credential_status
production_install_flow_set_credential_salt(
    production_install_flow_random_fn random,
    void *random_context);
enum production_install_flow_credential_status
production_install_flow_set_operator_credential(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length);
enum production_install_flow_credential_requirement
production_install_flow_credential_requirement(void);
enum production_install_flow_credential_status
production_install_flow_set_existing_operator_credential(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length);
const char *production_install_flow_credential_status_string(
    enum production_install_flow_credential_status status);

#endif
