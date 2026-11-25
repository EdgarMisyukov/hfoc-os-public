#ifndef HFOC_PRODUCTION_INSTALLER_STATE_H
#define HFOC_PRODUCTION_INSTALLER_STATE_H

#include <stdbool.h>

#include "appliance_admin_credential.h"
#include "appliance_language.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "block_device.h"
#include "installer_layout.h"
#include "production_installer.h"

/*
 * ITEM-502: how many blocks at the start and at the tail of each appliance-owned
 * partition the erase zeroes.  The stores live at the start of the persistence
 * partition (512 blocks of the default layout since ITEM-504 increment 6) and
 * the encryption custody record lives in the very last block; 64 blocks cover
 * never touch a foreign partition, because only the plan's own roles are
 * written.
 */
#define PRODUCTION_INSTALLER_STATE_ERASE_EDGE_BLOCKS UINT64_C(64)

/*
 * What the erase step publishes: how many stores were rebuilt, how many blocks
 * were written, whether the partition edges were cleared, and whether the
 * read-back proved the state empty.  `verify_clean` is true only when both the
 * raw read-back and the loader read-back agreed.
 */
struct production_installer_state_erase_evidence {
    uint32_t version;
    uint32_t stores;
    uint64_t blocks;
    bool edges;
    bool verify_clean;
};

#define PRODUCTION_INSTALLER_STATE_ERASE_VERSION 1U

/*
 * Why a state-preserving update preflight refused - `NONE` means it did not.
 * `NO_PLAN_RANGE` is the plan itself carrying no hfoc-persistence role;
 * `CURRENT_MISSING` is the target's table carrying no single HFOC persistence
 * partition to read; `RANGE_MISMATCH` is that partition not being the range
 * this plan writes; `LOADER_NOT_READY` is the installed reader not loading the
 * volume (empty, corrupt or unreadable) - only the last one is about the
 * content, which is why a default-only volume is not refused for it.
 */
enum production_installer_state_preserve_refusal {
    PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NONE = 0,
    PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NO_PLAN_RANGE = 1,
    PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_CURRENT_MISSING = 2,
    PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_RANGE_MISMATCH = 3,
    PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY = 4,
};

/*
 * ITEM-586/ITEM-588: what a state-preserving update proved before it wrote
 * anything, on both outcomes.  `stores` is the number of persistent stores the
 * existing volume carries; `config_preserved` and `admin_preserved` say which
 * of the two owners a reinstall would otherwise have erased came back
 * loadable.  A healthy volume that holds only the defaults is a valid update
 * target - the state is loadable and the update keeps it as it is - so the two
 * `preserved` flags are evidence, not a condition.
 *
 * The ranges are the answer to the only question the refusal has to explain:
 * where the appliance state was found (`current_*`, when the table still
 * carries exactly one HFOC persistence partition) and where this plan is
 * about to write (`plan_*`).  `loader_ready` and `loader_status` name the
 * state reader's verdict, and `refusal` names why the preflight stopped when
 * it did - so the console and the durable journal say which of the two rules
 * refused instead of one sentence for four different causes.
 */
struct production_installer_state_preserve_evidence {
    uint32_t version;
    uint32_t stores;
    bool config_preserved;
    bool admin_preserved;
    bool verify_loaded;
    bool current_present;
    uint32_t current_first_lba;
    uint32_t current_length_lba;
    uint32_t plan_first_lba;
    uint32_t plan_length_lba;
    bool loader_ready;
    uint32_t loader_status;
    enum production_installer_state_preserve_refusal refusal;
};

#define PRODUCTION_INSTALLER_STATE_PRESERVE_VERSION 2U

const char *production_installer_state_preserve_refusal_string(
    enum production_installer_state_preserve_refusal refusal);

enum production_installer_status production_installer_state_initialize(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    struct appliance_state_loader *loader);
/*
 * ITEM-502: the explicit named erase step.  Every store of the layout is
 * rebuilt as an empty data region (journal zeroed, block-0 header rewritten),
 * the start and tail of the appliance's own roles in `plan` are zeroed, the
 * whole thing is flushed, and then read back twice: raw (every journal block
 * must be zero and every header valid) and through the real loader (all four
 * loaded stores must publish their `DEFAULT_MISSING` source and the
 * administrator must be absent).  Any mismatch fails closed: the installer's
 * `state_erased` flag is not set, the caller must refuse the install, and the
 * machine never reaches `applied` with the previous run's state readable.
 */
enum production_installer_status production_installer_state_erase(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    const struct installer_layout_plan *plan,
    struct appliance_state_loader *loader,
    struct production_installer_state_erase_evidence *evidence);
/*
 * ITEM-586: the state-preserving update preflight.  It reads the appliance
 * partition the *current* table carries, requires it to be exactly the
 * persistence range the confirmed plan is about to write, and loads every
 * store through the installed system's own reader.  A target with no HFOC
 * state, a range that would move under the new table, or a loader that is not
 * ready is `STATE_NOT_PRESERVABLE` and nothing has been written.  On success
 * the caller executes the layout and skips the ITEM-502 erase/initialize pair;
 * the old records stay exactly where they were.
 */
enum production_installer_status production_installer_state_preserve(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    const struct installer_layout_plan *plan,
    struct appliance_state_loader *loader,
    struct production_installer_state_preserve_evidence *evidence);
/*
 * ITEM-506 increment 4: commit the operator's account into the installed
 * system's state, and prove it by reading the record back through the same
 * reader the installed system uses.  The record is the credential the model
 * built (login, salt, iteration count, PBKDF2 verifier); the password is not
 * an argument and is never written.  A write or read-back mismatch fails
 * closed with VERIFICATION_FAILED, so an install that promised `admin=SET` can
 * never leave a machine without a verifiable account.
 */
enum production_installer_status production_installer_state_commit_admin(
    struct appliance_state_loader *loader,
    const struct appliance_admin_credential *credential);
/*
 * ITEM-509 increment 27 (509.7c-2): commit the language the installation medium
 * declared into the installed system's state, and prove it by reading the
 * record back through the same reader the installed system uses.  The default
 * language has no line in the record, so the caller only reaches this function
 * for a declared non-default language; a write or read-back mismatch fails
 * closed with VERIFICATION_FAILED.
 */
enum production_installer_status production_installer_state_commit_language(
    struct appliance_state_loader *loader,
    enum appliance_language language);

#endif
