/*
 * ITEM-521 increment 12: the placement seam, filled with the owner's answer to
 * the ITEM-520 fork - (c) the \HFOC\INSTALL.LOG the medium's build reserves in
 * the FAT12 partition.
 *
 * `install_journal_platform_bind_medium()` takes the partition the loader
 * named from the verdict when the loader read the file through a filesystem,
 * otherwise scans the MBR for a FAT12 partition, binds the reserved file and
 * hands it to the journal core.  Nothing is allocated: the module only accepts
 * a file the build already reserved.  A refusal names itself and never fails
 * the run.
 */

#include "install_journal_platform.h"

#include "appliance_boot_log_store.h"
#include "install_fat12_journal.h"
#include "install_medium.h"
#include "serial.h"

#include <string.h>

#define PLACEMENT_FAT12_SECTOR_BYTES 512U
#define PLACEMENT_MBR_PARTITION_OFFSET 446U
#define PLACEMENT_MBR_PARTITION_SIZE 16U
#define PLACEMENT_MBR_PARTITION_COUNT 4U
#define PLACEMENT_MBR_SIGNATURE_OFFSET 510U

static struct install_journal platform_journal;
static struct fat12_journal platform_fat12;
static struct install_journal_backend platform_fat12_backend;

static void platform_notify(void *context, const char *text) {
    uint64_t serial_flags;

    (void)context;
    if (text == NULL) {
        return;
    }
    /* One line, one sequence: an IRQ-context print may not split the journal's
       own status record (scripts/check-serial-sequences.py). */
    serial_flags = serial_sequence_begin();
    serial_write(text);
    serial_write("\n");
    serial_sequence_end(serial_flags);
    /* The durable store, not the funnel: the journal's own status must not
       come back as a record it would then have to write. */
    appliance_boot_log_store_observe(text, strlen(text));
}

void install_journal_platform_init(void) {
    install_journal_init(&platform_journal, platform_notify, NULL);
}

/* ITEM-582: the export suspends the console funnel while it writes its own
   contiguous session. */
static bool platform_observe_suspended;

void install_journal_platform_suspend_observe(bool suspend) {
    platform_observe_suspended = suspend;
}

void install_journal_platform_observe(const char *record, size_t length) {
    if (platform_observe_suspended) {
        return;
    }

    install_journal_observe(&platform_journal, record, length);
}

/* ------------------------------------------------- glue over the prototype --- */

static uint32_t placement_fat12_committed(void *context) {
    return fat12_journal_write_slot((const struct fat12_journal *)context);
}

static uint32_t placement_fat12_capacity(void *context) {
    return fat12_journal_capacity_slots(
        (const struct fat12_journal *)context);
}

static bool placement_fat12_write(
    void *context, const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES]) {
    return fat12_journal_append((struct fat12_journal *)context, slot) ==
           FAT12_JOURNAL_OK;
}

static bool placement_fat12_flush(void *context) {
    return fat12_journal_flush((struct fat12_journal *)context) ==
           FAT12_JOURNAL_OK;
}

/* ITEM-552: the export session's own hooks.  The rewind rings inside the
   reserved clusters, the read-back is the placement's own geometry (never an
   offset arithmetic repeated in the export), and the sequence is the counter
   the header persists. */
static bool placement_fat12_rewind(void *context, uint64_t sequence) {
    return fat12_journal_rewind((struct fat12_journal *)context, sequence) ==
           FAT12_JOURNAL_OK;
}

static bool placement_fat12_read_slot(void *context,
                                      uint32_t slot,
                                      uint8_t *bytes) {
    return fat12_journal_read_slot((struct fat12_journal *)context, slot,
                                   bytes) == FAT12_JOURNAL_OK;
}

static bool placement_fat12_commit_sequence(void *context, uint64_t sequence) {
    return fat12_journal_commit_sequence((struct fat12_journal *)context,
                                         sequence) == FAT12_JOURNAL_OK;
}

static uint64_t placement_fat12_sequence(void *context) {
    return fat12_journal_sequence((const struct fat12_journal *)context);
}

static struct install_journal_backend placement_fat12_backend(
    struct fat12_journal *journal) {
    struct install_journal_backend backend;
    backend.name = "fat12";
    backend.context = journal;
    backend.committed_slots = placement_fat12_committed;
    backend.capacity_slots = placement_fat12_capacity;
    backend.write_slot = placement_fat12_write;
    backend.flush = placement_fat12_flush;
    backend.rewind = placement_fat12_rewind;
    backend.read_slot = placement_fat12_read_slot;
    backend.sequence = placement_fat12_sequence;
    backend.commit_sequence = placement_fat12_commit_sequence;
    return backend;
}

static uint32_t placement_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static const char *placement_fat12_token(enum fat12_journal_status status) {
    switch (status) {
    case FAT12_JOURNAL_OK:
        return "OK";
    case FAT12_JOURNAL_REFUSED_NO_SPACE:
        return "NO_SPACE";
    case FAT12_JOURNAL_REFUSED_BAD_CHAIN:
        return "CHAIN";
    case FAT12_JOURNAL_REFUSED_BAD_GEOMETRY:
    case FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME:
        return "GEOMETRY";
    case FAT12_JOURNAL_REFUSED_NO_SIGNATURE:
    case FAT12_JOURNAL_REFUSED_NOT_FAT12:
        return "NOT_FAT12";
    case FAT12_JOURNAL_REFUSED_NO_ROOT_SLOT:
        return "NO_ROOT";
    case FAT12_JOURNAL_REFUSED_FULL:
        return "CAPACITY";
    case FAT12_JOURNAL_REFUSED_NOT_RESERVED:
        return "NOT_RESERVED";
    case FAT12_JOURNAL_REFUSED_BAD_HEADER:
        return "HEADER";
    case FAT12_JOURNAL_REFUSED_READ:
    case FAT12_JOURNAL_REFUSED_WRITE:
        return "IO";
    case FAT12_JOURNAL_REFUSED_HEADER_WRITE:
        return "HEADER_WRITE";
    case FAT12_JOURNAL_REFUSED_FLUSH:
        return "FLUSH";
    case FAT12_JOURNAL_REFUSED_BOUND:
        return "BOUND";
    case FAT12_JOURNAL_REFUSED_NULL:
    case FAT12_JOURNAL_REFUSED_DEVICE:
    case FAT12_JOURNAL_REFUSED_BLOCK_SIZE:
    case FAT12_JOURNAL_REFUSED_NOT_BOUND:
    default:
        return "FAT12";
    }
}

/* A candidate that is not a FAT volume at all (NO_SIGNATURE, NOT_FAT12) must
   not shadow the failure of the medium's real FAT volume.  The operator reads
   why the journal is missing, and when a volume was recognized as FAT and the
   audit refused it, "this partition is not FAT" is the wrong name - on the
   isohybrid medium the first MBR entry is always such a non-FAT partition, so
   the shadowing would be the rule and the audit's own reason (the F10 case,
   INCONSISTENT_VOLUME) would never reach the log.  The first *specific*
   failure wins; a non-FAT failure is reported only when no candidate was
   recognized as FAT at all. */
static bool placement_fat12_specific(enum fat12_journal_status status) {
    return status != FAT12_JOURNAL_OK &&
           status != FAT12_JOURNAL_REFUSED_NO_SIGNATURE &&
           status != FAT12_JOURNAL_REFUSED_NOT_FAT12;
}

/* Bind one candidate partition and, on success, hand the backend to the core.
   A refused candidate has written nothing: binding only reads the reserved
   file's directory entry, chain and header. */
static bool placement_fat12_try(struct block_device *device,
                                uint64_t partition_lba,
                                uint64_t partition_blocks,
                                enum fat12_journal_status *reported_status) {
    enum fat12_journal_status status = fat12_journal_bind(
        &platform_fat12, device, partition_lba, partition_blocks);
    if (status != FAT12_JOURNAL_OK) {
        if (*reported_status == FAT12_JOURNAL_OK ||
            (!placement_fat12_specific(*reported_status) &&
             placement_fat12_specific(status))) {
            *reported_status = status;
        }
        return false;
    }
    platform_fat12_backend = placement_fat12_backend(&platform_fat12);
    return install_journal_bind(&platform_journal, &platform_fat12_backend);
}

bool install_journal_platform_bind_medium(
    struct block_device *device,
    const struct install_medium_verdict *verdict) {
    enum fat12_journal_status reported_status = FAT12_JOURNAL_OK;

    /* ITEM-551: more than one controller can carry the boot medium - the xHCI
       path probes it when its storage device binds, the EHCI path does the
       same - and the file has one header.  A journal that is already bound is
       left exactly as it is; binding it a second time would write a header
       into the middle of the record stream (the F6 failure the core's own
       comment names). */
    if (platform_journal.bound) {
        return true;
    }
    if (device == NULL || verdict == NULL ||
        block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != PLACEMENT_FAT12_SECTOR_BYTES) {
        return install_journal_platform_refuse("MEDIUM");
    }
    /* The loader read the kernel through a partition's filesystem: the guard
       already named that partition's extent, so no table is scanned. */
    if (verdict->partition_index != 0U) {
        if (placement_fat12_try(device, verdict->first_lba,
                                verdict->length_lba, &reported_status)) {
            return true;
        }
        return install_journal_platform_refuse(
            placement_fat12_token(reported_status));
    }
    /* The loader read the file through the medium's own filesystem (an
       isohybrid medium): the partition is found by the table. */
    uint8_t sector[PLACEMENT_FAT12_SECTOR_BYTES];
    if (block_device_read(device, 0U, sector, sizeof(sector)) !=
            BLOCK_DEVICE_OK ||
        sector[PLACEMENT_MBR_SIGNATURE_OFFSET] != 0x55U ||
        sector[PLACEMENT_MBR_SIGNATURE_OFFSET + 1U] != 0xAAU) {
        return install_journal_platform_refuse("SIGNATURE");
    }
    for (uint32_t i = 0U; i < PLACEMENT_MBR_PARTITION_COUNT; ++i) {
        const uint8_t *entry =
            &sector[PLACEMENT_MBR_PARTITION_OFFSET +
                    (i * PLACEMENT_MBR_PARTITION_SIZE)];
        uint32_t sectors = placement_le32(&entry[12]);
        if (entry[4] == 0U || sectors == 0U) {
            continue;
        }
        if (placement_fat12_try(device, placement_le32(&entry[8]),
                                sectors, &reported_status)) {
            return true;
        }
    }
    if (reported_status == FAT12_JOURNAL_OK) {
        return install_journal_platform_refuse("NO_PARTITION");
    }
    return install_journal_platform_refuse(
        placement_fat12_token(reported_status));
}

/* ITEM-582: the last diagnostic-medium attempt, kept for the report. */
static struct {
    bool attempted;
    const char *result;
    const char *reason;
    const char *placement;
} platform_diagnostic;

bool install_journal_platform_bind_diagnostic(
    struct block_device *device,
    const struct install_medium_verdict *boot_verdict) {
    struct install_medium_verdict verdict;
    enum fat12_journal_status reported_status = FAT12_JOURNAL_OK;
    static uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES];

    platform_diagnostic.attempted = true;
    platform_diagnostic.result = "UNAVAILABLE";
    platform_diagnostic.reason = "NO_DEVICE";
    platform_diagnostic.placement = "NONE";
    if (device == NULL || block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != PLACEMENT_FAT12_SECTOR_BYTES) {
        return false;
    }
    if (platform_journal.bound) {
        platform_diagnostic.result = "BOUND";
        platform_diagnostic.reason = "ALREADY_BOUND";
        return true;
    }
    /*
     * The installer's own guard runs first and wins: the probe already decided
     * whether this is the device the machine booted from, and that verdict is
     * handed in so the platform never re-derives a placement of its own.  The
     * boot medium is never re-bound as a diagnostic medium, whatever its table
     * says.
     */
    if (boot_verdict != NULL &&
        boot_verdict->result == INSTALL_MEDIUM_MATCH) {
        platform_diagnostic.result = "REFUSED";
        platform_diagnostic.reason = "BOOT_MEDIUM";
        return false;
    }
    if (block_device_read(device, 0U, sector, sizeof(sector)) !=
        BLOCK_DEVICE_OK) {
        platform_diagnostic.result = "REFUSED";
        platform_diagnostic.reason = "READ";
        return false;
    }
    verdict = install_medium_diagnostic_identify(sector,
                                                 device->block_count);
    platform_diagnostic.reason = verdict.reason;
    if (verdict.result != INSTALL_MEDIUM_MATCH) {
        platform_diagnostic.result =
            install_medium_result_string(verdict.result);
        return false;
    }
    if (placement_fat12_try(device, verdict.first_lba, verdict.length_lba,
                            &reported_status)) {
        platform_diagnostic.result = "BOUND";
        platform_diagnostic.reason = "DIAGNOSTIC";
        platform_diagnostic.placement = "FAT12_PREALLOCATED";
        return true;
    }
    platform_diagnostic.result = "REFUSED";
    platform_diagnostic.reason = placement_fat12_token(reported_status);
    return false;
}

void install_journal_platform_diagnostic_state(bool *attempted,
                                               const char **result,
                                               const char **reason,
                                               const char **placement) {
    if (attempted != NULL) {
        *attempted = platform_diagnostic.attempted;
    }
    if (result != NULL) {
        *result = platform_diagnostic.result;
    }
    if (reason != NULL) {
        *reason = platform_diagnostic.reason;
    }
    if (placement != NULL) {
        *placement = platform_diagnostic.placement;
    }
}

bool install_journal_platform_medium_unbound(void) {
    return install_journal_refuse(&platform_journal, "NO_PLACEMENT");
}

bool install_journal_platform_refuse(const char *reason) {
    return install_journal_refuse(&platform_journal, reason);
}

bool install_journal_platform_ready(void) {
    return platform_journal.state == INSTALL_JOURNAL_READY;
}

/* ITEM-590: the exported session's question.  A bound journal that filled its
   declared capacity is still the medium the operator asked for; only a refused
   placement is not. */
bool install_journal_platform_saveable(void) {
    return install_journal_saveable(&platform_journal);
}

bool install_journal_platform_write(const char *text, size_t length) {
    return install_journal_write(&platform_journal, text, length);
}

bool install_journal_platform_rewind(uint64_t sequence) {
    return install_journal_rewind(&platform_journal, sequence);
}

/* ITEM-571: the commit point's receipt, and one bounded line for the diag
   refusal.  The line separates the header write from the flush and names the
   block result each of them got; it needs no journal to be readable, which is
   the point - the refusal is printed by `diag` on a machine whose journal
   medium is exactly what failed. */
const struct fat12_journal_commit *install_journal_platform_commit_evidence(void) {
    if (!platform_journal.bound || platform_fat12.device == NULL) {
        return NULL;
    }
    return fat12_journal_commit_evidence(&platform_fat12);
}

static size_t commit_append(char *buffer, size_t size, size_t length,
                            const char *text) {
    size_t needed = 0U;

    while (text != NULL && text[needed] != '\0') {
        ++needed;
    }
    if (buffer == NULL || length + needed + 1U > size) {
        return length;
    }
    for (size_t index = 0U; index < needed; ++index) {
        buffer[length + index] = text[index];
    }
    length += needed;
    buffer[length] = '\0';
    return length;
}

static const char *commit_block_status_name(enum block_device_status status) {
    switch (status) {
    case BLOCK_DEVICE_OK:
        return "OK";
    case BLOCK_DEVICE_NULL:
        return "NULL";
    case BLOCK_DEVICE_BAD_DEVICE:
        return "BAD_DEVICE";
    case BLOCK_DEVICE_BAD_BLOCK_SIZE:
        return "BAD_BLOCK_SIZE";
    case BLOCK_DEVICE_BAD_BLOCK_COUNT:
        return "BAD_BLOCK_COUNT";
    case BLOCK_DEVICE_UNALIGNED:
        return "UNALIGNED";
    case BLOCK_DEVICE_INVALID_LENGTH:
        return "INVALID_LENGTH";
    case BLOCK_DEVICE_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case BLOCK_DEVICE_IO_ERROR:
        return "IO_ERROR";
    case BLOCK_DEVICE_UNSUPPORTED:
        break;
    }
    return "UNSUPPORTED";
}

size_t install_journal_platform_commit_text(char *buffer, size_t size) {
    const struct fat12_journal_commit *commit =
        install_journal_platform_commit_evidence();
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (commit == NULL) {
        return 0U;
    }
    if (!commit->header_written) {
        length = commit_append(buffer, size, length,
                               commit->header_status ==
                                       FAT12_JOURNAL_REFUSED_HEADER_WRITE
                                   ? "HEADER_WRITE"
                                   : fat12_journal_status_string(
                                         commit->header_status));
        length = commit_append(buffer, size, length, " blk=");
        length = commit_append(buffer, size, length,
                               commit_block_status_name(commit->write_status));
    } else if (commit->flush_status != BLOCK_DEVICE_OK) {
        length = commit_append(buffer, size, length, "FLUSH blk=");
        length = commit_append(buffer, size, length,
                               commit_block_status_name(commit->flush_status));
    } else {
        length = commit_append(buffer, size, length, "OK");
    }
    return length;
}

bool install_journal_platform_commit_sequence(uint64_t sequence) {
    return install_journal_commit_sequence(&platform_journal, sequence);
}

bool install_journal_platform_read_slot(
    uint32_t slot, uint8_t bytes[INSTALL_JOURNAL_SLOT_BYTES]) {
    return install_journal_read_slot(&platform_journal, slot, bytes);
}

uint32_t install_journal_platform_written(void) {
    return install_journal_written(&platform_journal);
}

uint32_t install_journal_platform_capacity_slots(void) {
    return install_journal_capacity_slots(&platform_journal);
}

uint64_t install_journal_platform_sequence(void) {
    return install_journal_sequence(&platform_journal);
}

bool install_journal_platform_flush(void) {
    return install_journal_flush(&platform_journal);
}

bool install_journal_platform_panic_flush(void) {
    return install_journal_panic_flush(&platform_journal);
}

/*
 * ITEM-541: the refusal page reads the journal's own answer - the state, the
 * first name that decided it and the counters of what never reached a medium -
 * through one call, and the staged tail through the record accessor.  The page
 * prints these next to the medium verdict so a machine without serial says
 * whether the medium was recognised and whether the journal is bound to it,
 * instead of leaving an operator with an empty stick and no reason.
 */
void install_journal_platform_status(struct install_journal_status *status) {
    if (status == NULL) {
        return;
    }
    status->state = platform_journal.state;
    status->reason = install_journal_reason(&platform_journal);
    status->pending = install_journal_pending_count(&platform_journal);
    status->dropped = platform_journal.dropped;
    status->refused = platform_journal.refused;
}

const char *install_journal_platform_record(uint32_t newest_index,
                                            size_t *length) {
    return install_journal_pending_record(&platform_journal, newest_index,
                                          length);
}
