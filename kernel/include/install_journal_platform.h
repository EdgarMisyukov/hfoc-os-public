#ifndef HFOC_INSTALL_JOURNAL_PLATFORM_H
#define HFOC_INSTALL_JOURNAL_PLATFORM_H

#include "install_journal_core.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * ITEM-521 (increment 5, wiring): the one place where the placement-independent
 * journal core meets this kernel.
 *
 * The core owns the slot format and the naming of every loss; this module owns
 * the instance, the funnel subscription and the moment a placement is
 * attached.  The placement itself is the answer to the ITEM-520 fork - (c) the
 * \HFOC\INSTALL.LOG the medium's build reserves in the FAT12 partition - so
 * `install_journal_platform_bind_medium()` below is the single seam that
 * answer fills in.  A medium that cannot carry the file (no reservation, a
 * FAT that does not back it, an optical boot) is refused by name; the journal
 * stays UNBOUND, stages records in its bounded ring and never pretends: no
 * byte is written to the medium and the run is not failed.
 */

/* Called once at boot, before any record can reach the funnel. */
void install_journal_platform_init(void);

/* The funnel side: every record the console observes reaches the journal too. */
void install_journal_platform_observe(const char *record, size_t length);

/*
 * ITEM-582: while the operator's export writes and verifies its session, the
 * console's own records must not land between the session's slots - the
 * installed system publishes periodic records, and an interleaved one shifts
 * the read-back window past the session (observed as a CRC mismatch on the
 * medium).  Suspending the funnel for the duration of the save keeps the
 * session contiguous; every other consumer of the console record stream is
 * untouched.
 */
void install_journal_platform_suspend_observe(bool suspend);

/*
 * The placement seam.  Called once the boot-medium identity guard said MATCH,
 * so `device` is the block view of the medium the loader booted from - the
 * same view the guard proved - and `verdict` is the guard's own verdict for
 * it: partition extent, type and disk id.  The context is handed over here so
 * no answer to the fork has to look a device up again or repeat an identity
 * check.  `device` outlives the call (the machine owns the block view), but
 * `verdict` is the probe's own and is valid only for the duration of the call:
 * a backend that needs the extent keeps a copy rather than the pointer.
 * Returns true when a placement was bound and the journal may be
 * written; the ITEM-520 answer - here the reserved FAT12 file - decides what
 * happens inside, and a medium the reservation cannot have is refused by
 * name.
 */
struct block_device;
struct install_medium_verdict;

bool install_journal_platform_bind_medium(
    struct block_device *device,
    const struct install_medium_verdict *verdict);

/*
 * ITEM-582: the operator's diagnostic stick, bound by its own identity.
 *
 * The installed machine booted from its SSD, so the boot-medium guard can
 * never match the stick - and the installer's write guard must not be
 * weakened to let it.  This seam reads the device itself: it refuses a device
 * whose MBR does not carry the diagnostic disk id, refuses the boot medium
 * even if it carried it, and requires a FAT12 partition whose reserved
 * \HFOC\INSTALL.LOG passes the same binder the install medium uses.  The
 * result is named whether it binds or not; nothing is written before the
 * binder accepts, and a bound journal is never rebound.
 */
struct install_medium_verdict;

bool install_journal_platform_bind_diagnostic(
    struct block_device *device,
    const struct install_medium_verdict *boot_verdict);

/*
 * The outcome of the last diagnostic attempt, for the diag report: whether a
 * diagnostic medium was tried, what its identity said and what the placement
 * said.  A NULL output is left untouched.
 */
void install_journal_platform_diagnostic_state(bool *attempted,
                                               const char **result,
                                               const char **reason,
                                               const char **placement);

/*
 * Requirement 3: the medium cannot carry the journal at all (an optical boot),
 * so nothing is bound and nothing is written.  The refusal is named once in
 * the ordinary records and never fails the run.
 */
bool install_journal_platform_refuse(const char *reason);

/*
 * The identity guard proved the medium and the seam bound no placement on it
 * (no partition carries FAT12, the volume carries no reserved file, the FAT
 * does not back the reservation, the table has no signature): the journal stays UNBOUND, so nothing it staged will ever
 * reach the medium.  That is the state every answer to the ITEM-520 fork
 * starts from - every refusal leaves the journal there - so it is named once,
 * in the ordinary records, instead of being a silent loss (requirement 3).  A
 * placement that refused for a reason of its own has already named it and
 * keeps that name; the run is never failed.
 */
bool install_journal_platform_medium_unbound(void);

/*
 * ITEM-552: the operator's export session, over the same bound journal.
 *
 * `install_journal_platform_ready()` answers whether records can be written at
 * all; `install_journal_platform_write()` writes one record slot directly;
 * `install_journal_platform_rewind()` restarts the stream at slot 1 with the
 * session counter it is given; and the read-back half - `written`, `capacity`,
 * `sequence` and `read_slot` - lets the export prove that the medium holds the
 * bytes it wrote before it claims a save.  Every call is bounded and NOP when
 * no placement is bound.
 */
bool install_journal_platform_ready(void);
/*
 * ITEM-590: a healthy placement that can take the operator's session - the
 * stream as it stands, or the FULL stream the export restarts through the
 * journal's own rewind.  The medium is bound and its identity checks passed;
 * a placement that failed is not this, and neither is no placement at all.
 * `ready()` alone is not the same question: the installed Toshiba's journal
 * was FULL when `diag save` ran, `ready()` said no, and the export refused the
 * medium instead of rewinding a journal that had done nothing wrong.
 */
bool install_journal_platform_saveable(void);
bool install_journal_platform_write(const char *text, size_t length);
bool install_journal_platform_rewind(uint64_t sequence);
/* ITEM-552: persist the session counter before the session's first record. */
bool install_journal_platform_commit_sequence(uint64_t sequence);
/* ITEM-571: the commit point's receipt and its one-line rendering.  Both work
   without a journal medium being writable: the receipt is the attempt's own
   RAM record, which is what the diag refusal must print. */
const struct fat12_journal_commit *install_journal_platform_commit_evidence(void);
size_t install_journal_platform_commit_text(char *buffer, size_t size);
bool install_journal_platform_read_slot(
    uint32_t slot, uint8_t bytes[INSTALL_JOURNAL_SLOT_BYTES]);
uint32_t install_journal_platform_written(void);
uint32_t install_journal_platform_capacity_slots(void);
uint64_t install_journal_platform_sequence(void);

/* Requirement 2: the log is on the medium before the refusal page is painted
 * and before a panic stops the machine. */
bool install_journal_platform_flush(void);
bool install_journal_platform_panic_flush(void);

/*
 * ITEM-541: the journal's own answer for the refusal page - the state, the
 * name that decided it, and the counters of what never reached a medium.  The
 * page reads this through one call instead of reaching into the core, and the
 * records it shows come from `install_journal_platform_record()` below.
 */
struct install_journal_status {
    enum install_journal_state state;
    const char *reason; /* never NULL */
    uint32_t pending;   /* records staged in the RAM ring */
    uint32_t dropped;   /* oldest staged records lost to the ring bound */
    uint32_t refused;   /* records that arrived after UNAVAILABLE or FULL */
};

void install_journal_platform_status(struct install_journal_status *status);
/*
 * The `newest_index`-th staged record, 0 being the newest; NULL past the ring.
 * A borrowed pointer into the ring, valid until the next observe.
 */
const char *install_journal_platform_record(uint32_t newest_index,
                                            size_t *length);

#endif
