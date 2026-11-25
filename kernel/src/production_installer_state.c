#include "production_installer_state.h"

#include "appliance_gpt_discovery.h"
#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_persistent_service_config.h"
#include "appliance_state_block_device.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned char production_state_magic[DATA_REGION_MAGIC_SIZE] =
    DATA_REGION_MAGIC_BYTES;

static void production_state_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static enum production_installer_status
production_state_map_gpt(
    enum appliance_gpt_discovery_status status)
{
    switch (status) {
    case APPLIANCE_GPT_DISCOVERY_OK:
        return PRODUCTION_INSTALLER_OK;
    case APPLIANCE_GPT_DISCOVERY_IO_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    default:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
}

static enum production_installer_status
production_state_map_loader(
    enum appliance_state_loader_status status)
{
    switch (status) {
    case APPLIANCE_STATE_LOADER_OK:
        return PRODUCTION_INSTALLER_OK;
    case APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR:
    case APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED:
        return PRODUCTION_INSTALLER_IO_ERROR;
    default:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
}

static enum production_installer_status
production_state_write_data_header(
    struct block_device *store)
{
    unsigned char header[DATA_REGION_HEADER_BLOCK_SIZE];
    enum block_device_status io_status;

    memset(header, 0, sizeof(header));
    memcpy(header, production_state_magic,
           sizeof(production_state_magic));
    header[8U] = DATA_REGION_FORMAT_VERSION;
    production_state_store_le64(header + 16U, 1U);
    production_state_store_le64(
        header + 24U, store->block_count - 1U);
    io_status = block_device_write(
        store, 0U, header, sizeof(header));
    if (io_status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_state_initialize(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    struct appliance_state_loader *loader)
{
    enum appliance_gpt_discovery_status gpt_status;
    enum appliance_state_block_device_status bind_status;
    enum appliance_state_loader_status loader_status;
    struct appliance_gpt_hfoc_partition partition;
    struct appliance_state_block_device partition_device;
    struct appliance_state_block_device store;
    uint32_t index;
    enum production_installer_status status;

    if (installer == NULL || device == NULL ||
        layout == NULL || loader == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage !=
            PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    /*
     * ITEM-502: a run may only initialize state it has erased first.  The flag
     * is set by the explicit erase step and by nothing else, so a reinstalled
     * machine cannot come up on the previous run's address, administrator
     * credential or certificates even if a caller skips the step by accident.
     */
    if (!installer->state_erased) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != APPLIANCE_STATE_BLOCK_SIZE ||
        appliance_state_layout_validate(layout) !=
            APPLIANCE_STATE_LAYOUT_OK) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }

    memset(&partition, 0, sizeof(partition));
    gpt_status = appliance_gpt_find_hfoc_partition(
        device, &partition);
    status = production_state_map_gpt(gpt_status);
    if (status != PRODUCTION_INSTALLER_OK) {
        return (enum production_installer_status)(100 + (int)gpt_status);
    }
    if (appliance_gpt_validate_hfoc_partition(
            &partition) != APPLIANCE_GPT_DISCOVERY_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }

    bind_status = appliance_state_block_device_bind_range(
        device, partition.first_lba, partition.block_count,
        &partition_device);
    if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }

    for (index = 0U; index < layout->entry_count; ++index) {
        bind_status = appliance_state_block_device_bind(
            &partition_device.device,
            &layout->entries[index], &store);
        if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        status = production_state_write_data_header(
            &store.device);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }

    /*
     * The loader is this call's out-param: the loader API refuses to re-init a
     * loader that is already ready, and the erase step before this one leaves
     * one behind, so the buffer is cleared here rather than relied upon.
     */
    memset(loader, 0, sizeof(*loader));
    loader_status = appliance_state_loader_init(loader);
    if (loader_status != APPLIANCE_STATE_LOADER_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    loader_status = appliance_state_loader_load(
        loader, device, layout);
    status = production_state_map_loader(loader_status);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    return PRODUCTION_INSTALLER_OK;
}

/* ------------------------------------------------- ITEM-502: state erase -- */

static bool production_state_is_zero(
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

/*
 * Rebuild one store as an empty data region: every journal block is zeroed and
 * the block-0 header is rewritten, which is the same REWRITE_FACTORY_DEFAULT
 * semantics the recovery executor already proves.  The header write alone was
 * the whole of the old "initialize", and it is exactly why the previous run's
 * records replayed: the journal body was never touched.
 */
static enum production_installer_status production_state_erase_store(
    struct block_device *store,
    uint64_t *blocks)
{
    unsigned char zero[DATA_REGION_HEADER_BLOCK_SIZE];
    uint64_t index;

    memset(zero, 0, sizeof(zero));
    for (index = 1U; index < store->block_count; ++index) {
        if (block_device_write(
                store, index * store->block_size, zero,
                sizeof(zero)) != BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        ++*blocks;
    }
    if (production_state_write_data_header(store) !=
        PRODUCTION_INSTALLER_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    *blocks += 1U;
    return PRODUCTION_INSTALLER_OK;
}

/*
 * Zero the start and the tail of one appliance-owned role.  The bound is 64
 * blocks at each edge, halved when a role is so small that the two edges would
 * meet, so the function can never zero a block twice or walk past the role.
 * Only roles named by the plan are ever touched: a foreign partition, a reused
 * ESP and the machine's own table are not in this list.
 *
 * ITEM-517: the roles that carry the staged boot chain are not erase targets
 * either.  The boot chain is staged *before* this step - into the ESP on UEFI,
 * into the BIOS boot partition and the FAT boot volume on legacy BIOS - and
 * zeroing the edges of the volume that holds it destroys the boot sector, the
 * FSInfo and the start of the FAT (or stage2), which is exactly what the BIOS
 * install proof found: the boot log could no longer bind the volume it had
 * just staged into.  The three chain roles are skipped for the same reason the
 * ESP always was.
 */
static enum production_installer_status production_state_erase_edge(
    struct block_device *device,
    const struct installer_layout_partition *partition,
    uint64_t *blocks)
{
    unsigned char zero[DATA_REGION_HEADER_BLOCK_SIZE];
    uint64_t edge = PRODUCTION_INSTALLER_STATE_ERASE_EDGE_BLOCKS;
    uint64_t index;

    if (!partition->present || partition->length_lba == 0U ||
        partition->role == INSTALLER_LAYOUT_ROLE_EFI_SYSTEM ||
        partition->role == INSTALLER_LAYOUT_ROLE_BIOS_BOOT ||
        partition->role == INSTALLER_LAYOUT_ROLE_BOOT_VOLUME ||
        partition->role == INSTALLER_LAYOUT_ROLE_UNKNOWN) {
        return PRODUCTION_INSTALLER_OK;
    }
    if (edge > partition->length_lba / 2U) {
        edge = partition->length_lba / 2U;
    }
    if (edge == 0U) {
        return PRODUCTION_INSTALLER_OK;
    }
    memset(zero, 0, sizeof(zero));
    for (index = 0U; index < edge; ++index) {
        if (block_device_write(
                device,
                (partition->first_lba + index) * device->block_size,
                zero, sizeof(zero)) != BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        if (block_device_write(
                device,
                (partition->first_lba + partition->length_lba - 1U - index) *
                    device->block_size,
                zero, sizeof(zero)) != BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        *blocks += 2U;
    }
    return PRODUCTION_INSTALLER_OK;
}

/*
 * The raw read-back: every store's journal must be zero and its header must be
 * the one this step wrote.  It covers the stores the loader never loads - the
 * certificate, diagnostic and recovery-metadata stores - so "the state is
 * empty" is a statement about the volume and not only about four readers.
 */
static enum production_installer_status production_state_verify_stores(
    struct block_device *partition_device,
    const struct appliance_state_layout *layout)
{
    unsigned char block[DATA_REGION_HEADER_BLOCK_SIZE];
    struct appliance_state_block_device store;
    uint64_t index;

    for (index = 0U; index < layout->entry_count; ++index) {
        uint64_t journal;
        unsigned char expected[DATA_REGION_HEADER_BLOCK_SIZE];

        if (appliance_state_block_device_bind(
                partition_device, &layout->entries[index], &store) !=
            APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        memset(expected, 0, sizeof(expected));
        memcpy(expected, production_state_magic,
               sizeof(production_state_magic));
        expected[8U] = DATA_REGION_FORMAT_VERSION;
        production_state_store_le64(expected + 16U, 1U);
        production_state_store_le64(
            expected + 24U, store.device.block_count - 1U);
        memset(block, 0, sizeof(block));
        if (block_device_read(&store.device, 0U, block,
                              sizeof(block)) != BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        if (memcmp(block, expected, sizeof(expected)) != 0) {
            return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        }
        for (journal = 1U; journal < store.device.block_count;
             ++journal) {
            if (block_device_read(
                    &store.device,
                    journal * store.device.block_size, block,
                    sizeof(block)) != BLOCK_DEVICE_OK) {
                return PRODUCTION_INSTALLER_IO_ERROR;
            }
            if (!production_state_is_zero(block, sizeof(block))) {
                return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
            }
        }
    }
    return PRODUCTION_INSTALLER_OK;
}

/*
 * The loader read-back the owner asked for: the real loader must come up ready
 * with every source it reads reporting its missing default, and the
 * administrator must not be enrolled.  A store that still replays a record
 * reports COMMITTED here and the erase fails closed.
 */
static enum production_installer_status production_state_verify_empty(
    struct block_device *device,
    const struct appliance_state_layout *layout,
    struct appliance_state_loader *loader)
{
    const struct appliance_persistent_config *system_config;
    const struct appliance_persistent_network_config *network_config;
    const struct appliance_persistent_secret_config *secret_config;
    const struct appliance_persistent_service_config *service_config;
    unsigned char fingerprint[SHA256_DIGEST_SIZE];

    memset(loader, 0, sizeof(*loader));
    if (appliance_state_loader_init(loader) !=
            APPLIANCE_STATE_LOADER_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    if (appliance_state_loader_load(loader, device, layout) !=
            APPLIANCE_STATE_LOADER_OK ||
        !appliance_state_loader_ready(loader)) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    system_config = appliance_state_loader_system_config(loader);
    network_config = appliance_state_loader_network_config(loader);
    secret_config = appliance_state_loader_secret_config(loader);
    service_config = appliance_state_loader_service_config(loader);
    if (system_config == NULL || network_config == NULL ||
        secret_config == NULL || service_config == NULL ||
        appliance_persistent_config_source(system_config) !=
            APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING ||
        network_config->source !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING ||
        service_config->source !=
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING ||
        secret_config->source !=
            APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_MISSING ||
        appliance_persistent_secret_config_admin(
            secret_config, fingerprint)) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_state_erase(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    const struct installer_layout_plan *plan,
    struct appliance_state_loader *loader,
    struct production_installer_state_erase_evidence *evidence)
{
    enum appliance_gpt_discovery_status gpt_status;
    enum appliance_state_block_device_status bind_status;
    struct appliance_gpt_hfoc_partition partition;
    struct appliance_state_block_device partition_device;
    struct appliance_state_block_device store;
    enum production_installer_status status;
    uint64_t blocks = 0U;
    uint32_t index;

    if (installer == NULL || device == NULL || layout == NULL ||
        plan == NULL || loader == NULL || evidence == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage != PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (installer->state_erased || installer->state_preserved) {
        /* One state decision per install: a second call is a caller error,
         * not a second clean-up. */
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != APPLIANCE_STATE_BLOCK_SIZE ||
        appliance_state_layout_validate(layout) !=
            APPLIANCE_STATE_LAYOUT_OK ||
        plan->version != INSTALLER_LAYOUT_VERSION ||
        plan->disk_id != installer->disk_id ||
        plan->block_size != device->block_size ||
        plan->block_count != device->block_count) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }

    memset(&partition, 0, sizeof(partition));
    gpt_status = appliance_gpt_find_hfoc_partition(device, &partition);
    status = production_state_map_gpt(gpt_status);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    if (appliance_gpt_validate_hfoc_partition(&partition) !=
        APPLIANCE_GPT_DISCOVERY_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    bind_status = appliance_state_block_device_bind_range(
        device, partition.first_lba, partition.block_count,
        &partition_device);
    if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }

    /*
     * Edges first, then the stores: the tail of the persistence partition
     * carries the encryption custody record, and it must be gone before the
     * keyless loader read-back below.  The stores own the first 512 blocks of
     * the same partition (ITEM-504 increment 6 appended the route store) and
     * are rebuilt immediately after.
     */
    for (index = 0U; index < plan->partition_count; ++index) {
        status = production_state_erase_edge(
            device, &plan->partitions[index], &blocks);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }
    for (index = 0U; index < layout->entry_count; ++index) {
        bind_status = appliance_state_block_device_bind(
            &partition_device.device, &layout->entries[index], &store);
        if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        status = production_state_erase_store(&store.device, &blocks);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }
    if (block_device_flush(device) != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    if (production_state_verify_stores(
            &partition_device.device, layout) !=
        PRODUCTION_INSTALLER_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    if (production_state_verify_empty(device, layout, loader) !=
        PRODUCTION_INSTALLER_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }

    memset(evidence, 0, sizeof(*evidence));
    evidence->version = PRODUCTION_INSTALLER_STATE_ERASE_VERSION;
    evidence->stores = layout->entry_count;
    evidence->blocks = blocks;
    evidence->edges = true;
    evidence->verify_clean = true;
    installer->state_erased = true;
    return PRODUCTION_INSTALLER_OK;
}

/*
 * ITEM-586/ITEM-588: prove that an update can keep the state it is about to
 * leave alone.  The current table must still carry exactly one HFOC
 * persistence partition, and it must be the same range the confirmed plan is
 * going to write: a plan that would move persistence elsewhere while keeping
 * the old bytes is not an update, it is a clean install with a misleading
 * label.  The installed system's own loader then has to come up ready; an
 * empty or unreadable volume is refused before the first write.
 *
 * What the volume *contains* is evidence, not a condition: an installed
 * appliance whose stores still hold the defaults (no configuration, no
 * account) is exactly the machine the owner's Toshiba was - the account could
 * not be set there because the CPU has no entropy source - and refusing the
 * update for it kept the appliance on the old build.  A loadable default-only
 * volume is updated and its defaults are kept; the refusal stays for the plan
 * that moves the range and for the volume the reader cannot load.
 *
 * Every outcome fills `evidence`: the ranges on both sides, the reader's
 * verdict and the refusal reason, so the console and the durable journal name
 * the cause instead of one sentence for four different rules.
 */
enum production_installer_status production_installer_state_preserve(
    struct production_installer *installer,
    struct block_device *device,
    const struct appliance_state_layout *layout,
    const struct installer_layout_plan *plan,
    struct appliance_state_loader *loader,
    struct production_installer_state_preserve_evidence *evidence)
{
    enum appliance_gpt_discovery_status gpt_status;
    struct appliance_gpt_hfoc_partition partition;
    struct appliance_state_block_device partition_device;
    const struct appliance_persistent_config *system_config;
    const struct appliance_persistent_network_config *network_config;
    const struct appliance_persistent_secret_config *secret_config;
    const struct appliance_persistent_service_config *service_config;
    const struct installer_layout_partition *persistence = NULL;
    unsigned char admin_fingerprint[SHA256_DIGEST_SIZE];
    enum appliance_state_loader_status loader_status;
    uint32_t index;

    if (evidence != NULL) {
        memset(evidence, 0, sizeof(*evidence));
        evidence->version = PRODUCTION_INSTALLER_STATE_PRESERVE_VERSION;
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NONE;
    }
    if (installer == NULL || device == NULL || layout == NULL ||
        plan == NULL || loader == NULL || evidence == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        !installer->confirmed ||
        (installer->stage != PRODUCTION_INSTALLER_STAGE_CONFIRMED &&
         installer->stage != PRODUCTION_INSTALLER_STAGE_EXECUTING)) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (installer->state_erased || installer->state_preserved) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != APPLIANCE_STATE_BLOCK_SIZE ||
        appliance_state_layout_validate(layout) !=
            APPLIANCE_STATE_LAYOUT_OK ||
        plan->version != INSTALLER_LAYOUT_VERSION ||
        plan->disk_id != installer->disk_id ||
        plan->block_size != device->block_size ||
        plan->block_count != device->block_count ||
        !plan->destructive) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    evidence->stores = layout->entry_count;
    for (index = 0U; index < plan->partition_count; ++index) {
        if (plan->partitions[index].present &&
            plan->partitions[index].role ==
                INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) {
            persistence = &plan->partitions[index];
            break;
        }
    }
    if (persistence == NULL || persistence->length_lba == 0U) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NO_PLAN_RANGE;
        return PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE;
    }
    evidence->plan_first_lba = (uint32_t)persistence->first_lba;
    evidence->plan_length_lba = (uint32_t)persistence->length_lba;
    memset(&partition, 0, sizeof(partition));
    gpt_status = appliance_gpt_find_hfoc_partition(device, &partition);
    if (gpt_status != APPLIANCE_GPT_DISCOVERY_OK ||
        appliance_gpt_validate_hfoc_partition(&partition) !=
            APPLIANCE_GPT_DISCOVERY_OK) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_CURRENT_MISSING;
        return PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE;
    }
    evidence->current_present = true;
    evidence->current_first_lba = (uint32_t)partition.first_lba;
    evidence->current_length_lba = (uint32_t)partition.block_count;
    if (partition.first_lba != persistence->first_lba ||
        partition.block_count != persistence->length_lba) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_RANGE_MISMATCH;
        return PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE;
    }
    if (appliance_state_block_device_bind_range(
            device, partition.first_lba, partition.block_count,
            &partition_device) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY;
        evidence->loader_status = APPLIANCE_STATE_LOADER_PARTITION_BIND_FAILED;
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    memset(loader, 0, sizeof(*loader));
    loader_status = appliance_state_loader_init(loader);
    if (loader_status != APPLIANCE_STATE_LOADER_OK) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY;
        evidence->loader_status = (uint32_t)loader_status;
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    loader_status = appliance_state_loader_load(loader, device, layout);
    evidence->loader_status = (uint32_t)loader_status;
    evidence->loader_ready = appliance_state_loader_ready(loader);
    if (loader_status != APPLIANCE_STATE_LOADER_OK ||
        !evidence->loader_ready) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY;
        return PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE;
    }
    system_config = appliance_state_loader_system_config(loader);
    network_config = appliance_state_loader_network_config(loader);
    secret_config = appliance_state_loader_secret_config(loader);
    service_config = appliance_state_loader_service_config(loader);
    if (system_config == NULL || network_config == NULL ||
        secret_config == NULL || service_config == NULL) {
        evidence->refusal =
            PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY;
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    memset(admin_fingerprint, 0, sizeof(admin_fingerprint));
    evidence->admin_preserved =
        appliance_persistent_secret_config_admin(
            secret_config, admin_fingerprint);
    evidence->config_preserved =
        appliance_persistent_config_source(system_config) !=
            APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING ||
        network_config->source !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING ||
        service_config->source !=
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING ||
        evidence->admin_preserved;
    /*
     * A loadable volume is what the update promised to keep: the defaults it
     * holds are preserved just like a configured store would be, and the
     * `config=`/`admin=` fields tell the operator which of the two it was.
     */
    evidence->verify_loaded = true;
    installer->state_preserved = true;
    return PRODUCTION_INSTALLER_OK;
}

const char *production_installer_state_preserve_refusal_string(
    enum production_installer_state_preserve_refusal refusal)
{
    switch (refusal) {
    case PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NONE:
        return "NONE";
    case PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_NO_PLAN_RANGE:
        return "NO_PLAN_RANGE";
    case PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_CURRENT_MISSING:
        return "CURRENT_MISSING";
    case PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_RANGE_MISMATCH:
        return "RANGE_MISMATCH";
    case PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_LOADER_NOT_READY:
        return "LOADER_NOT_READY";
    default:
        return "UNKNOWN";
    }
}

/*
 * ITEM-506 increment 4: the operator set the account on the install screen, so
 * the record is written here and read back through the reader the installed
 * system will use.  The comparison is field by field - the decode validates
 * the canonical form, so a record that survives this comparison is exactly the
 * credential the model built.  A mismatch fails closed: the caller refuses the
 * install instead of promising `admin=SET` over a store that does not hold it.
 */
enum production_installer_status production_installer_state_commit_admin(
    struct appliance_state_loader *loader,
    const struct appliance_admin_credential *credential)
{
    struct appliance_persistent_secret_config *secret_config;
    struct appliance_admin_credential readback;
    enum appliance_persistent_secret_config_status status;
    bool matches;

    if (loader == NULL || credential == NULL) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    /*
     * The writer needs the mutable store the loader owns; the const accessor
     * is for readers.
     */
    secret_config = &loader->secret_config;
    if (secret_config == NULL) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    status = appliance_persistent_secret_config_commit_admin_credential(
        secret_config, credential);
    if (status == APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    memset(&readback, 0, sizeof(readback));
    status = appliance_persistent_secret_config_admin_credential(
        secret_config, &readback);
    if (status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
        memset(&readback, 0, sizeof(readback));
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    matches = readback.version == credential->version &&
              readback.iterations == credential->iterations &&
              memcmp(readback.login, credential->login,
                     sizeof(readback.login)) == 0 &&
              memcmp(readback.salt, credential->salt,
                     sizeof(readback.salt)) == 0 &&
              memcmp(readback.verifier, credential->verifier,
                     sizeof(readback.verifier)) == 0;
    memset(&readback, 0, sizeof(readback));
    return matches ? PRODUCTION_INSTALLER_OK
                   : PRODUCTION_INSTALLER_VERIFICATION_FAILED;
}

/*
 * ITEM-509 increment 27 (509.7c-2): the medium declared a language, so the
 * installed system's own record is written here and read back through the
 * reader the installed system will use.  The writer already knows the default
 * language is absence - it writes no language line for `en` - and the caller
 * only reaches this function for a declared non-default language, so an English
 * install keeps the record it always produced, byte for byte.  A write error or
 * a read-back that does not equal the declared language fails closed with
 * VERIFICATION_FAILED: an install that names a language in its record must
 * leave a machine whose reader agrees.
 */
enum production_installer_status production_installer_state_commit_language(
    struct appliance_state_loader *loader,
    enum appliance_language language)
{
    struct appliance_persistent_config *system_config;
    enum appliance_persistent_config_status status;
    enum appliance_language readback = APPLIANCE_LANGUAGE_DEFAULT;

    if (loader == NULL || appliance_language_code(language) == NULL) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    /*
     * The writer needs the mutable store the loader owns; the const accessor
     * is for readers.
     */
    system_config = &loader->system_config;
    status = appliance_persistent_config_set_language(system_config, language);
    if (status == APPLIANCE_PERSISTENT_CONFIG_IO_ERROR) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    status = appliance_persistent_config_language(system_config, &readback);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    return readback == language ? PRODUCTION_INSTALLER_OK
                                : PRODUCTION_INSTALLER_VERIFICATION_FAILED;
}
