#ifndef HFOC_INSTALLER_GPT_PRESERVE_H
#define HFOC_INSTALLER_GPT_PRESERVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "installer_layout.h"

/*
 * ITEM-396 GPT writer that preserves foreign entries.
 *
 * ITEM-391 sized the appliance's layout and ITEM-341 planned a dual-boot
 * reservation, but the only executable partition-table writer in the tree
 * (`installer_layout_execute()`) rebuilds the table from the plan: it zeroes
 * the entry array and writes the appliance's roles, so an entry the plan does
 * not carry is deleted and the MBR boot code is zeroed.  The RED reproduction
 * in `docs/hfoc-installer-preservation.md` shows a Debian root entry and a
 * second Debian entry disappearing that way.
 *
 * This module is the writer that does not do that.  It reads the target's
 * existing GPT, keeps every foreign entry byte-for-byte in its own slot, adds
 * the appliance's roles in the free region, and refuses before the first write
 * when it cannot preserve what is there.
 *
 * Read side.  `installer_gpt_preserve_read()` validates the protective MBR, the
 * primary header, the primary array, the backup header and the backup array of
 * one disk, and refuses rather than assuming anything:
 *
 *   * an I/O failure while reading any of them is UNREADABLE_TABLE;
 *   * a bad signature, a bad header or array CRC, a header whose `my_lba`,
 *     `alternate_lba`, usable range or array location does not describe this
 *     disk, an absent or non-protective MBR, a present entry whose extent is
 *     outside the usable region or overlaps another present entry, or a primary
 *     array that differs from the backup array is CORRUPT_TABLE;
 *   * a table whose `number_of_partition_entries` is not 128 or whose
 *     `size_of_partition_entry` is not 128 is UNSUPPORTED_TABLE.  128 entries of
 *     128 bytes is 32 sectors, which is the GPT's own standard bound and the
 *     documented, tested bound of this writer: preserved entries are bounded by
 *     the table, not by a constant.  ITEM-517 raised the layout capacity to 5
 *     for the legacy-BIOS role set, but the roles this preserving writer adds
 *     are still the UEFI ones - the ESP reuse plus the three HFOC roles - and a
 *     request above `INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY` is refused with
 *     INVALID_ARGUMENT (a BIOS clean plan is written by the destructive
 *     `installer_layout_execute()`, never by this writer).
 *
 * A slot is EMPTY only when all 128 of its bytes are zero.  Everything else is
 * a foreign entry: it is preserved byte-for-byte, whatever it contains, and it
 * takes part in the overlap and free-space arithmetic.  That rule is what makes
 * "byte-for-byte" mechanical instead of best-effort - the writer never rewrites
 * a non-zero slot.
 *
 * Write side.  `installer_gpt_preserve_write()` re-runs the whole check, then
 * writes the backup array and header, the primary array and header, and finally
 * the validated protective MBR, and reads each written sector back and compares
 * it.  Order and failure behaviour:
 *
 *   * the backup GPT is completed first, so a failure always leaves at least
 *     one complete, self-consistent table that still contains every foreign
 *     entry;
 *   * our entries only ever land in slots that were all-zero, so a partial
 *     write can never remove or rewrite a foreign entry;
 *   * the MBR is written last, is byte-identical to the one that was validated,
 *     and the boot code and disk signature in bytes 0..445 are therefore
 *     untouched;
 *   * the operation is idempotent: re-running it replaces our own entries (they
 *     are recognised by their type GUID and unique GUID) instead of adding
 *     duplicates, so a failed run is completed by running it again.
 *
 * The bookkeeping structure is caller-owned storage, not a stack object: it
 * records one 128-byte slot per entry as a bounded record plus the three GPT
 * sectors and the MBR, so it is written by the caller as a `static` (or other
 * non-stack) object.
 */

#define INSTALLER_GPT_PRESERVE_VERSION 1U
#define INSTALLER_GPT_PRESERVE_BLOCK_SIZE 512U
#define INSTALLER_GPT_PRESERVE_HEADER_SIZE 92U
#define INSTALLER_GPT_PRESERVE_ENTRY_SIZE 128U
#define INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY 128U
#define INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR 4U
#define INSTALLER_GPT_PRESERVE_ARRAY_SECTORS 32U
#define INSTALLER_GPT_PRESERVE_FIRST_USABLE_LBA 34U
#define INSTALLER_GPT_PRESERVE_TAIL_RESERVE_LBA 34U
#define INSTALLER_GPT_PRESERVE_ADDED_CAPACITY \
    INSTALLER_LAYOUT_PARTITION_CAPACITY
#define INSTALLER_GPT_PRESERVE_RECORD_LENGTH 8192U
#define INSTALLER_GPT_PRESERVE_NO_SLOT 0xFFFFFFFFU

enum installer_gpt_preserve_status {
    INSTALLER_GPT_PRESERVE_OK = 0,
    INSTALLER_GPT_PRESERVE_NULL = 1,
    INSTALLER_GPT_PRESERVE_BAD_VERSION = 2,
    INSTALLER_GPT_PRESERVE_BAD_STATE = 3,
    INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT = 4,
    INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE = 5,
    INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE = 6,
    INSTALLER_GPT_PRESERVE_CORRUPT_TABLE = 7,
    INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE = 8,
    INSTALLER_GPT_PRESERVE_OUT_OF_RANGE = 9,
    INSTALLER_GPT_PRESERVE_CAPACITY = 10,
    INSTALLER_GPT_PRESERVE_NO_FREE_SPACE = 11,
    INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP = 12,
    INSTALLER_GPT_PRESERVE_NOT_CONFIRMED = 13,
    INSTALLER_GPT_PRESERVE_ALREADY_EXECUTED = 14,
    INSTALLER_GPT_PRESERVE_IO_ERROR = 15,
    INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED = 16,
};

enum installer_gpt_preserve_origin {
    INSTALLER_GPT_PRESERVE_SLOT_EMPTY = 0,
    INSTALLER_GPT_PRESERVE_SLOT_PRESERVED = 1,
    INSTALLER_GPT_PRESERVE_SLOT_ADDED = 2,
    /*
     * ITEM-501: the one entry this install was explicitly told to give up.  It
     * is written back as all-zero bytes, and only a plan that names it may do
     * that - every other non-zero slot stays byte-for-byte.
     */
    INSTALLER_GPT_PRESERVE_SLOT_RELEASED = 3,
};

/*
 * One entry of the table as read, plus what this install does with it.  The
 * raw 128 bytes are not kept: a preserved slot is never rewritten, and both
 * proofs compare the array on the device before and after the write.
 */
struct installer_gpt_preserve_entry {
    uint32_t index;
    enum installer_gpt_preserve_origin origin;
    enum installer_layout_role role;
    uint32_t partition_index;
    bool has_extent;
    bool reused;
    /* An added entry whose slot already carried this install's own entry. */
    bool rewritten;
    uint64_t first_lba;
    uint64_t last_lba;
    unsigned char type_guid[16U];
    unsigned char unique_guid[16U];
};

/*
 * An entry this install keeps instead of adding: the shared EFI System
 * Partition of a dual-boot install.  The range must be covered by an entry that
 * is already on the disk, byte-for-byte; nothing is created for it.
 */
struct installer_gpt_preserve_reuse {
    bool present;
    enum installer_layout_role role;
    uint64_t first_lba;
    uint64_t length_lba;
};

struct installer_gpt_preserve_table {
    uint32_t version;
    /*
     * Set only when `installer_gpt_preserve_read()` returned OK.  Every other
     * entry point refuses a table that was not completely read, so a refused or
     * half-read table can never be written.
     */
    bool read_complete;
    uint32_t disk_id;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    unsigned char disk_guid[16U];
    unsigned char mbr[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char primary_header[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char backup_header[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t present_count;
    uint32_t preserved_count;
    uint32_t added_count;
    uint32_t esp_index;
    bool esp_reused;
    /*
     * ITEM-501: the slot the plan gives up, or NO_SLOT.  It is set by
     * `installer_gpt_preserve_check()` and stays set through the write, so the
     * verification knows which slot must now be all-zero.
     */
    uint32_t released_index;
    uint64_t preserved_first_lba;
    uint64_t preserved_last_lba;
    uint64_t free_first_lba;
    uint64_t free_length_lba;
    struct installer_gpt_preserve_entry
        entries[INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY];
};

/*
 * Read and validate the target's existing table.  `table` is caller-owned
 * storage; nothing is written to the device.  A disk that is not a valid,
 * well-formed GPT is refused here, before any plan is checked or any byte is
 * written.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_read(
    struct block_device *device,
    uint32_t disk_id,
    struct installer_gpt_preserve_table *table);

/*
 * The maximal free region of the table that contains `first_lba`: it starts
 * after the last preserved entry that ends before it and ends at the first
 * preserved entry that starts after it, inside the GPT usable range.  This is
 * computed from the entries actually on the disk, not from a discovery report,
 * so a disk carrying more entries than the report could hold cannot be
 * under-counted.  A point inside a preserved entry has no free region and is
 * refused with NO_FREE_SPACE.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_free_region(
    const struct installer_gpt_preserve_table *table,
    uint64_t first_lba,
    uint64_t *free_first_lba,
    uint64_t *free_length_lba);

/*
 * Check the plan against the read table without writing anything: every added
 * role must lie inside the usable region, must not overlap any entry that is
 * present (including entries the discovery report never carried), and must not
 * overlap another added role; when `reuse` is given, its range must be covered
 * exactly by an entry that is already on the disk, and that entry is kept
 * byte-for-byte instead of being added.  `plan` carries the appliance's roles
 * only (`INSTALLER_LAYOUT_PARTITION_CAPACITY` is their bound) and must not be
 * the destructive whole-disk plan: a clean install that intends to wipe the
 * table uses `installer_layout_execute()`, this writer refuses it with
 * BAD_STATE.
 *
 * The refusals this returns are the ITEM-396 refusals: NO_FREE_SPACE when the
 * declared layout does not fit next to what is preserved, EXISTING_SYSTEM_OVERLAP
 * when a request collides with a preserved entry or when the reused range is not
 * the entry the plan believed it was, OUT_OF_RANGE when a request leaves the
 * usable region, CAPACITY when more entries are requested than the table can
 * hold.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_check(
    struct installer_gpt_preserve_table *table,
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse);

/*
 * Write the table back: every preserved entry byte-for-byte, the appliance's
 * roles added, primary and backup GPT, protective MBR, checksums recomputed.
 * `installer_gpt_preserve_check()` runs first and every refusal it can return is
 * returned before the first write.  On success `plan->executed` is set.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_write(
    struct block_device *device,
    struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table);

/*
 * Verify what is on the disk now against the table that was read: both headers
 * parse and their CRCs hold, both arrays are byte-identical, every preserved
 * entry is still present in its own slot with its own extent, and every added
 * entry is present.  Read-only.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_verify(
    struct block_device *device,
    const struct installer_gpt_preserve_table *table);

/*
 * ITEM-396, owner refinement (Case B): read-only probe of *another* disk's GPT
 * for the one thing a cross-disk boot menu entry needs - the GPT partition GUID
 * of the EFI System Partition a foreign loader lives on, so the entry can name
 * it with Limine's `guid(...)` resource instead of a firmware-dependent drive
 * number.  It reads the disk it is given and never writes anything: the
 * acceptance host's Debian disk must stay untouched while its loader is offered
 * in the appliance's menu.  A corrupt or unreadable table is refused with the
 * same statuses as `_read()`, and the caller publishes that reason.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_esp_guid(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t length_lba,
    unsigned char unique_guid[16U]);

/*
 * Render a GPT GUID the way Limine's `guid(...)` resource parses it: the mixed
 * endian text form of the on-disk bytes, uppercase, with dashes.  `buffer` holds
 * at least INSTALLER_GPT_PRESERVE_GUID_TEXT_LENGTH bytes.
 */
#define INSTALLER_GPT_PRESERVE_GUID_TEXT_LENGTH 37U
enum installer_gpt_preserve_status installer_gpt_preserve_format_guid(
    const unsigned char guid[16U],
    char *buffer,
    size_t capacity);

/*
 * The machine-readable publication, in the house single-line style:
 *
 *   [INSTALLER-PRESERVE] plan mode=dual-boot disk=1 preserved=3 added=3
 *     esp_reused=1 free_first_lba=... free_lba=... existing_first_lba=...
 *     existing_lba=... menu_entries=...\n
 *   [INSTALLER-PRESERVE] preserved index=0 first_lba=... length_lba=... role=foreign\n
 *   [INSTALLER-PRESERVE] added index=4 role=hfoc-system first_lba=... length_lba=...\n
 *
 * One buffer, always terminated; the caller writes it inside one
 * serial_sequence_begin()/serial_sequence_end() pair.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_render_record(
    const struct installer_gpt_preserve_table *table,
    const struct installer_layout_plan *plan,
    uint32_t menu_entry_count,
    char *buffer,
    size_t capacity,
    size_t *length);

const char *installer_gpt_preserve_status_string(
    enum installer_gpt_preserve_status status);
const char *installer_gpt_preserve_origin_string(
    enum installer_gpt_preserve_origin origin);

#endif
