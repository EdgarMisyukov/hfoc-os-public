#ifndef HFOC_INSTALL_FAT12_JOURNAL_H
#define HFOC_INSTALL_FAT12_JOURNAL_H

/*
 * ITEM-521 (increment 12), the owner's answer (c) to the ITEM-520 fork: the
 * medium's build reserves \HFOC\INSTALL.LOG in the FAT12 partition, and the
 * kernel only reads what the build reserved.
 *
 * The kernel implements no FAT12 allocation at all.  `fat12_journal_bind()`
 * reads the BPB, the root directory, the HFOC directory entry and the 12-bit
 * chain the build wrote, and it accepts the file only when the volume, the
 * chain and the journal header the build also wrote all check out.  From then
 * on every write - the record slots and the slot-0 header that carries the
 * commit point - lands inside the clusters the FAT chain already assigns to
 * the file.  No FAT sector and no directory entry is written by the kernel,
 * and a medium without a reserved file gets a named refusal instead of an
 * allocated one (the install run is never failed by it).
 *
 * The build side is `scripts/hfoc-install-media-prealloc.py`; the reader side
 * is `scripts/hfoc-install-log-read.py`, which reports this placement as
 * `fat12-preallocated`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define FAT12_JOURNAL_SLOT_BYTES 512U
#define FAT12_JOURNAL_MAX_CLUSTERS 512U
#define FAT12_JOURNAL_MAGIC "HFOC-ILOG"
#define FAT12_JOURNAL_MAGIC_BYTES 9U
#define FAT12_JOURNAL_VERSION 1U
/* The header the build writes into slot 0: magic, version, slot size and the
   commit point.  `write_slot` is the number of committed slots, header
   included; the build writes 1, a resumed journal carries what the run last
   committed. */
#define FAT12_JOURNAL_HEADER_OFFSET 0U
#define FAT12_JOURNAL_HEADER_VERSION_OFFSET 9U
#define FAT12_JOURNAL_HEADER_SLOT_OFFSET 13U
#define FAT12_JOURNAL_HEADER_CAPACITY_OFFSET 17U
#define FAT12_JOURNAL_HEADER_WRITE_OFFSET 21U
/* ITEM-552: the persisted session counter the export's identifiers come from.
   The header layout reserves bytes 25..32 for it, and the header CRC covers it
   like every other field, so a torn counter is a torn header. */
#define FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET 25U
#define FAT12_JOURNAL_HEADER_CRC_OFFSET 36U
#define FAT12_JOURNAL_HEADER_CRC_BYTES 36U

enum fat12_journal_status {
    FAT12_JOURNAL_OK = 0,
    FAT12_JOURNAL_REFUSED_NULL,
    FAT12_JOURNAL_REFUSED_DEVICE,
    FAT12_JOURNAL_REFUSED_BLOCK_SIZE,
    FAT12_JOURNAL_REFUSED_READ,
    FAT12_JOURNAL_REFUSED_WRITE,
    FAT12_JOURNAL_REFUSED_NO_SIGNATURE,
    FAT12_JOURNAL_REFUSED_NOT_FAT12,
    FAT12_JOURNAL_REFUSED_BAD_GEOMETRY,
    FAT12_JOURNAL_REFUSED_NO_ROOT_SLOT,
    FAT12_JOURNAL_REFUSED_NO_SPACE,
    FAT12_JOURNAL_REFUSED_BAD_CHAIN,
    FAT12_JOURNAL_REFUSED_FULL,
    FAT12_JOURNAL_REFUSED_NOT_BOUND,
    FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME,
    /* (c): the volume carries no reserved \HFOC\INSTALL.LOG.  The build was
       not asked to reserve one, or the medium predates the reservation; the
       journal refuses by this name and the run continues. */
    FAT12_JOURNAL_REFUSED_NOT_RESERVED,
    /* (c): the reserved file exists but its slot 0 is not the header the
       build writes, so the kernel cannot trust the commit point and refuses
       rather than overwriting the reserve. */
    FAT12_JOURNAL_REFUSED_BAD_HEADER,
    /* ITEM-552: a rewind or a read-back outside the committed slots. */
    FAT12_JOURNAL_REFUSED_BOUND,
    /* ITEM-571: the commit point's two steps are different failures.  The
       header write did not reach the medium, or the header reached it and the
       flush that makes it durable did not.  They were one value
       (REFUSED_WRITE), which is why the diag refusal could not say which. */
    FAT12_JOURNAL_REFUSED_HEADER_WRITE,
    FAT12_JOURNAL_REFUSED_FLUSH,
};

/*
 * ITEM-571: the last commit point's own receipt, kept in RAM.
 *
 * `commit_sequence` writes the header (the session's commit point) and then
 * flushes the medium.  A failure of either is now named, and the block result
 * the device gave is kept beside it, so the diag refusal can say
 * "HEADER_WRITE / IO_ERROR" or "FLUSH / IO_ERROR" instead of one bool.
 */
struct fat12_journal_commit {
    bool attempted;
    bool header_attempted;
    enum fat12_journal_status header_status;
    bool header_written;
    bool flush_attempted;
    enum block_device_status write_status;   /* the header write's block result */
    enum block_device_status flush_status;   /* the flush's block result */
};

/* One sector of FAT is enough to compare the two copies over the file's own
   chain. */
#define FAT12_JOURNAL_FAT_SECTOR_BYTES 512U
#define FAT12_JOURNAL_FAT_COPIES 2U

struct fat12_journal {
    struct block_device *device;
    uint64_t base_offset;
    uint64_t partition_bytes;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t root_entries;
    uint32_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t fat_bits;
    uint32_t cluster_count;
    uint32_t cluster_bytes;
    uint32_t slots_per_cluster;

    uint64_t fat_offset;
    uint64_t root_offset;
    uint64_t data_offset;

    uint32_t dir_cluster;
    uint64_t dir_entry_offset;
    uint32_t first_cluster;
    uint32_t file_clusters;
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    uint32_t chain_length;
    uint32_t capacity_slots;
    uint32_t write_slot;
    uint64_t sequence;
    /* ITEM-571: the commit point's receipt and the last block-level result. */
    struct fat12_journal_commit commit;
    enum block_device_status last_block_status;

    bool bound;
};

/*
 * Bind the file the build reserved.  `partition_lba` and `partition_blocks`
 * are the FAT12 partition's own extent from the identity guard's verdict.
 * Nothing is written before the header, the chain and the file's directory
 * entry have all been read and accepted; on any refusal `journal->bound`
 * stays false and the caller names the reason.
 */
enum fat12_journal_status fat12_journal_bind(
    struct fat12_journal *journal,
    struct block_device *device,
    uint64_t partition_lba,
    uint64_t partition_blocks);

/*
 * Append one committed slot: the record goes to the next slot of the reserved
 * chain, then slot 0 (the header, inside the same chain) is rewritten with the
 * advanced commit point.  Both writes stay inside the file's own clusters.
 */
enum fat12_journal_status fat12_journal_append(
    struct fat12_journal *journal,
    const uint8_t slot[FAT12_JOURNAL_SLOT_BYTES]);

enum fat12_journal_status fat12_journal_flush(
    struct fat12_journal *journal);

uint32_t fat12_journal_capacity_slots(const struct fat12_journal *journal);
uint32_t fat12_journal_write_slot(const struct fat12_journal *journal);
uint64_t fat12_journal_sequence(const struct fat12_journal *journal);

/*
 * ITEM-552: the operator's export session inside the reserved file.
 *
 * `fat12_journal_rewind()` restarts the stream at slot 1 with the sequence the
 * caller names and writes the header at once, so the commit point never points
 * past a record another session wrote.  `fat12_journal_read_slot()` reads one
 * committed slot back - the export compares it with what it wrote before it
 * says SAVED.  Neither allocates: both stay inside the clusters the build
 * reserved for the file.
 */
enum fat12_journal_status fat12_journal_rewind(struct fat12_journal *journal,
                                               uint64_t sequence);
/*
 * ITEM-552: persist the session counter without touching the record stream -
 * the header is the commit point, so the counter a session will use is on the
 * medium before the session's first record is.
 */
enum fat12_journal_status fat12_journal_commit_sequence(
    struct fat12_journal *journal,
    uint64_t sequence);

/* The last commit point's receipt (ITEM-571), or NULL before one was tried. */
const struct fat12_journal_commit *fat12_journal_commit_evidence(
    const struct fat12_journal *journal);
enum fat12_journal_status fat12_journal_read_slot(
    struct fat12_journal *journal,
    uint32_t slot,
    uint8_t bytes[FAT12_JOURNAL_SLOT_BYTES]);
const char *fat12_journal_status_string(enum fat12_journal_status status);

#endif
