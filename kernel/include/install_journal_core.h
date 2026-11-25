#ifndef HFOC_INSTALL_JOURNAL_CORE_H
#define HFOC_INSTALL_JOURNAL_CORE_H

/*
 * ITEM-521 (increment 1): the placement-independent core of the install
 * journal.
 *
 * The owner wants the install run and every refusal recorded on the medium
 * itself (requirement 1 in /srv/hfoc-example/queue/current.md), including the
 * records published long before the medium is identified: the machine
 * announcement, the observed storage, the flow decisions.  The wiring plan
 * (/srv/hfoc-example/queue/item-521-wiring-plan.md, F8) notes that a journal which
 * only starts writing at `state=MATCH` loses exactly those records - so the
 * core keeps a bounded in-RAM staging ring and replays it into the journal as
 * soon as one is bound.  This increment is deliberately not wired to any
 * caller: the placement fork ITEM-520 (FAT12 file, raw extent, preallocated
 * file) stays open, and no attachment point is chosen here.
 * The same ring and the same state machine serve every placement the owner may
 * choose; only `struct install_journal_backend` differs:
 *
 *   (a) FAT12 file created by the installer (item-520-fat12-c/),
 *   (b) a raw extent at the end of the medium,
 *   (c) a preallocated file the installer only writes inside.
 *
 * Nothing here touches a device, and no policy is decided by this file: it
 * formats records, bounds the ring, names every loss and refuses to lose a
 * record silently.
 *
 * Slot layout (version 1).  Slot 0 is the header, slots 1..N are records:
 *
 *   header: 0..8 magic "HFOC-ILOG", 9..12 version, 13..16 slot_size,
 *           17..20 slot_count, 21..24 write_slot, 25..32 sequence,
 *           33..35 reserved, 36..39 CRC32 over bytes 0..35;
 *   record: 0..3 length, 4..7 CRC32 over the text, 8.. text, NUL padding.
 *
 * A slot whose length or CRC does not check out is a torn or foreign slot and
 * is not printed as text.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALL_JOURNAL_SLOT_BYTES 512U
#define INSTALL_JOURNAL_TEXT_MAX 440U
#define INSTALL_JOURNAL_PENDING_CAPACITY 64U
#define INSTALL_JOURNAL_MAGIC "HFOC-ILOG"
#define INSTALL_JOURNAL_MAGIC_BYTES 9U
#define INSTALL_JOURNAL_VERSION 1U

enum install_journal_state {
    INSTALL_JOURNAL_UNBOUND = 0, /* no medium yet: records go to the ring */
    INSTALL_JOURNAL_READY,       /* bound: records go to the backend */
    INSTALL_JOURNAL_UNAVAILABLE, /* the medium cannot carry the log */
    INSTALL_JOURNAL_FULL,        /* the backend is out of slots */
};

/*
 * The placement.  `committed_slots` tells the core whether slot 0 already
 * carries a header (a resumed journal) and how far a fresh one has to start;
 * `capacity_slots` is the number of slots the placement can hold, header
 * included.  Both may be NULL, which means "unknown" (0).
 */
struct install_journal_backend {
    const char *name;
    void *context;
    uint32_t (*committed_slots)(void *context);
    uint32_t (*capacity_slots)(void *context);
    bool (*write_slot)(void *context,
                       const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES]);
    bool (*flush)(void *context);
    /*
     * ITEM-552: the operator's export session.  A placement that only appends
     * cannot ring, so `rewind` may be NULL and the session then lives with
     * whatever room is left; the fat12 placement can ring (its clusters are
     * reserved and its header is the commit point).  `read_slot` reads one
     * committed slot back so the export can prove what the medium holds;
     * `sequence` answers the persisted session counter, which is what makes a
     * report's identifier unique across saves on one medium.
     */
    bool (*rewind)(void *context, uint64_t sequence);
    bool (*read_slot)(void *context, uint32_t slot, uint8_t *bytes);
    uint64_t (*sequence)(void *context);
    bool (*commit_sequence)(void *context, uint64_t sequence);
};

/*
 * Status that must not travel through the same funnel that feeds the journal:
 * publishing "the journal is unavailable" as a normal record would re-enter
 * `observe` (item-521-wiring-plan.md, F7).  The wiring points this at the
 * serial stream and the boot-log store.
 */
typedef void (*install_journal_notify_fn)(void *context, const char *text);

struct install_journal {
    enum install_journal_state state;
    /* ITEM-541: the first fact that decided a non-READY state ("NONE" before
       anything was decided, "OK" while bound and writing).  Never NULL, so the
       refusal page prints the name instead of guessing at one. */
    const char *reason;
    struct install_journal_backend backend;
    bool bound;
    bool publishing; /* re-entrancy guard for the funnel */
    uint32_t written;      /* slots handed to the backend, header included */
    uint32_t capacity;     /* slots the placement can hold, 0 = unknown */
    uint32_t dropped;      /* oldest pending records lost to the ring bound */
    uint32_t refused;      /* records that arrived after UNAVAILABLE or FULL */
    uint32_t truncated;    /* records clamped to INSTALL_JOURNAL_TEXT_MAX */
    /* ITEM-552: the persisted session counter and the export's own head. */
    uint64_t sequence;
    uint32_t overflow_named;
    uint32_t full_named;
    uint32_t unavailable_named;
    uint32_t truncated_named;
    install_journal_notify_fn notify;
    void *notify_context;
    uint8_t pending[INSTALL_JOURNAL_PENDING_CAPACITY]
                   [INSTALL_JOURNAL_TEXT_MAX];
    uint16_t pending_length[INSTALL_JOURNAL_PENDING_CAPACITY];
    uint32_t pending_head;
    uint32_t pending_count;
};

void install_journal_init(struct install_journal *journal,
                          install_journal_notify_fn notify,
                          void *notify_context);
/* The funnel hook: every sanitised record, bound or not. */
void install_journal_observe(struct install_journal *journal, const char *text,
                             size_t length);
/* Bind after the medium identity guard said MATCH; replays the ring. */
bool install_journal_bind(struct install_journal *journal,
                          const struct install_journal_backend *backend);
/*
 * The placement is refused before any write is attempted: the medium cannot
 * carry the journal at all (an optical boot, a medium declared read-only), so
 * nothing may be bound and no device may be touched.  The run is not failed;
 * the refusal is named once on the notify side, exactly like a backend
 * refusal, and records that arrive afterwards are counted in `refused`.
 * A journal that is already bound is left alone: this is the pre-placement
 * decision, not a way to stop a running journal.
 */
bool install_journal_refuse(struct install_journal *journal,
                            const char *reason);
/* Push what has been written to the medium.  Safe in every state. */
bool install_journal_flush(struct install_journal *journal);
/* The panic path: replay the tail, then flush, as far as the backend allows. */
bool install_journal_panic_flush(struct install_journal *journal);

const char *install_journal_state_string(enum install_journal_state state);
uint32_t install_journal_crc32(const uint8_t *bytes, size_t length);

/*
 * ITEM-590: whether the operator's session can be written into this journal -
 * into the stream as it stands, or by restarting it.
 *
 * A bound placement that used every slot it declares is FULL, not broken:
 * every record reached the medium, the capacity is the placement's own, and
 * the export's rewind is exactly what recovers the room.  What the export must
 * treat as a missing medium is the placement that failed; the state with no
 * placement at all is UNBOUND.  Reading FULL as "no medium" is what lost the
 * installed Toshiba's report: the journal was full and healthy, and the save
 * never reached its own rewind.
 */
bool install_journal_saveable(const struct install_journal *journal);

/*
 * ITEM-552: the operator's export session.
 *
 * `install_journal_write()` writes one record slot directly - the caller owns
 * the text and its own session markers, so the export's begin/end pair is
 * written as it is composed instead of being pushed through the funnel that
 * may stage it.  It refuses when the journal is not READY or when the slot
 * reserved for the end marker would be crossed.
 *
 * `install_journal_rewind()` restarts the record stream at slot 1 with the
 * sequence the caller names, through the backend's own rewind (the fat12
 * placement rings inside its reserved clusters).  A placement that cannot ring
 * refuses and the journal stays as it was; nothing is ever written past the
 * placement's declared capacity.
 *
 * The read-back half is what turns "the write returned true" into "the medium
 * holds these bytes": `install_journal_written()`/`capacity()` say the head,
 * `install_journal_read_slot()` reads a committed slot back through the
 * placement, and `install_journal_sequence()` is the persisted counter.
 */
bool install_journal_write(struct install_journal *journal, const char *text,
                           size_t length);
bool install_journal_rewind(struct install_journal *journal,
                            uint64_t sequence);
bool install_journal_commit_sequence(struct install_journal *journal,
                                     uint64_t sequence);
uint32_t install_journal_written(const struct install_journal *journal);
uint32_t install_journal_capacity_slots(const struct install_journal *journal);
uint64_t install_journal_sequence(const struct install_journal *journal);
bool install_journal_read_slot(const struct install_journal *journal,
                               uint32_t slot,
                               uint8_t bytes[INSTALL_JOURNAL_SLOT_BYTES]);

/*
 * ITEM-541: what the refusal page reads.  `install_journal_reason()` is never
 * NULL; the ring accessors expose the bounded tail of what is still staged in
 * RAM (the records no medium has taken), newest first, without lending the ring
 * out.  `install_journal_pending_record()` returns NULL once `newest_index` is
 * past the staged records; `length` is optional.
 */
const char *install_journal_reason(const struct install_journal *journal);
uint32_t install_journal_pending_count(const struct install_journal *journal);
const char *install_journal_pending_record(
    const struct install_journal *journal,
    uint32_t newest_index,
    size_t *length);

#endif
