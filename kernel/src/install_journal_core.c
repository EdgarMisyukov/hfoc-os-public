/*
 * ITEM-521 (increment 1): the placement-independent install-journal core.
 *
 * Host-compiled beside install_journal_core.h; nothing here touches a device
 * or a kernel service.  The invariants this file is responsible for:
 *
 *   * a record published before a medium exists is not lost - it waits in the
 *     bounded ring and is replayed, in order, at bind (requirement 1);
 *   * a loss is never silent: a dropped ring entry is counted and named by an
 *     `state=OVERFLOW dropped=N` record, a clamped record by
 *     `state=TRUNCATED limit=N`, a full journal by `state=FULL`, an unusable
 *     medium by `state=UNAVAILABLE`;
 *   * the journal never feeds itself: status that the funnel would re-observe
 *     goes through the notify callback instead (F7);
 *   * the medium is written only through the backend, and a backend refusal
 *     turns the journal off instead of failing the install run (requirement 3).
 */

#include "install_journal_core.h"

#include <string.h>

static void le32_write(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

uint32_t install_journal_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

const char *install_journal_state_string(enum install_journal_state state)
{
    switch (state) {
    case INSTALL_JOURNAL_UNBOUND:
        return "UNBOUND";
    case INSTALL_JOURNAL_READY:
        return "READY";
    case INSTALL_JOURNAL_UNAVAILABLE:
        return "UNAVAILABLE";
    case INSTALL_JOURNAL_FULL:
        return "FULL";
    }
    return "UNKNOWN";
}

void install_journal_init(struct install_journal *journal,
                          install_journal_notify_fn notify,
                          void *notify_context)
{
    if (journal == NULL) {
        return;
    }
    memset(journal, 0, sizeof(*journal));
    journal->state = INSTALL_JOURNAL_UNBOUND;
    journal->reason = "NONE";
    journal->notify = notify;
    journal->notify_context = notify_context;
}

static void notify(struct install_journal *journal, const char *text)
{
    if (journal->notify != NULL) {
        journal->notify(journal->notify_context, text);
    }
}

static void format_header(const struct install_journal *journal, uint8_t *slot)
{
    memset(slot, 0, INSTALL_JOURNAL_SLOT_BYTES);
    memcpy(slot, INSTALL_JOURNAL_MAGIC, INSTALL_JOURNAL_MAGIC_BYTES);
    le32_write(&slot[9], INSTALL_JOURNAL_VERSION);
    le32_write(&slot[13], INSTALL_JOURNAL_SLOT_BYTES);
    le32_write(&slot[17], journal->backend.capacity_slots != NULL
                              ? journal->backend.capacity_slots(
                                    journal->backend.context)
                              : 0U);
    le32_write(&slot[21], journal->written);
    le32_write(&slot[36], install_journal_crc32(slot, 36U));
}

static void format_record(const char *text, size_t length, uint8_t *slot)
{
    memset(slot, 0, INSTALL_JOURNAL_SLOT_BYTES);
    le32_write(&slot[0], (uint32_t)length);
    le32_write(&slot[4],
               install_journal_crc32((const uint8_t *)text, length));
    memcpy(&slot[8], text, length);
}

/* One slot to the placement; the caller owns the state transitions. */
static bool write_text(struct install_journal *journal, const char *text,
                       size_t length)
{
    uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES];
    if (length > INSTALL_JOURNAL_TEXT_MAX) {
        length = INSTALL_JOURNAL_TEXT_MAX;
    }
    format_record(text, length, slot);
    if (journal->backend.write_slot == NULL ||
        !journal->backend.write_slot(journal->backend.context, slot)) {
        return false;
    }
    journal->written += 1U;
    return true;
}

/* A named status record, written directly and never through observe (F7). */
static bool publish_status(struct install_journal *journal, const char *text)
{
    if (!journal->bound || write_text(journal, text, strlen(text))) {
        return true;
    }
    return false;
}

static void go_unavailable(struct install_journal *journal, const char *reason)
{
    journal->state = INSTALL_JOURNAL_UNAVAILABLE;
    if (journal->unavailable_named == 0U) {
        char line[96];
        journal->unavailable_named = 1U;
        /* ITEM-541: the first name that decided the state is the one the
           refusal page prints; a later failure does not rewrite it. */
        journal->reason = reason;
        /* The backend just refused a write, so this goes to the notify side
           (serial and the boot-log store), not to the journal. */
        size_t length = 0U;
        const char *prefix = "[INSTALL-LOG] state=UNAVAILABLE reason=";
        while (prefix[length] != '\0' &&
               length + 1U < sizeof(line)) {
            line[length] = prefix[length];
            ++length;
        }
        size_t i = 0U;
        while (reason[i] != '\0' && length + 1U < sizeof(line)) {
            line[length++] = reason[i++];
        }
        line[length] = '\0';
        notify(journal, line);
    }
}

/*
 * How many slots this placement can still take.  `capacity` is what the
 * placement declared, header included; 0 means it did not say, and then the
 * placement's own refusal is the only bound.  Every write path asks this
 * before it hands a slot over - ITEM-521 increment 9: the replay at bind used
 * to write until the placement refused, which handed a bounded region a slot
 * past its end, named the refusal `state=UNAVAILABLE reason=WRITE` instead of
 * `state=FULL reason=CAPACITY`, and left the staged records that no longer
 * fitted uncounted.
 */
static bool room_for(const struct install_journal *journal, uint32_t slots)
{
    return journal->capacity == 0U ||
           journal->written + slots <= journal->capacity;
}

/*
 * The one FULL transition: the state, the end marker on the medium while the
 * reserved slot is still there, and the notify line.  The marker never writes
 * past the declared capacity; when it cannot fit, the end of the journal is
 * still named - on the notify side - so a full journal is never a silence.
 */
static void go_full(struct install_journal *journal)
{
    journal->state = INSTALL_JOURNAL_FULL;
    if (journal->reason == NULL || strcmp(journal->reason, "NONE") == 0 ||
        strcmp(journal->reason, "OK") == 0) {
        journal->reason = "CAPACITY";
    }
    if (journal->full_named == 0U) {
        journal->full_named = 1U;
        if (room_for(journal, 1U)) {
            publish_status(
                journal,
                "[INSTALL-LOG] state=FULL reason=CAPACITY slots=0");
        }
        notify(journal, "[INSTALL-LOG] state=FULL reason=CAPACITY");
    }
}

static void ring_push(struct install_journal *journal, const char *text,
                      size_t length)
{
    if (journal->pending_count == INSTALL_JOURNAL_PENDING_CAPACITY) {
        /* the oldest unwritten record is the one that goes */
        journal->pending_head =
            (journal->pending_head + 1U) % INSTALL_JOURNAL_PENDING_CAPACITY;
        journal->pending_count -= 1U;
        journal->dropped += 1U;
    }
    uint32_t index =
        (journal->pending_head + journal->pending_count) %
        INSTALL_JOURNAL_PENDING_CAPACITY;
    memcpy(journal->pending[index], text, length);
    journal->pending_length[index] = (uint16_t)length;
    journal->pending_count += 1U;
}

static bool ring_pop(struct install_journal *journal, const char **text,
                     size_t *length)
{
    if (journal->pending_count == 0U) {
        return false;
    }
    uint32_t index = journal->pending_head;
    *text = (const char *)journal->pending[index];
    *length = journal->pending_length[index];
    journal->pending_head =
        (journal->pending_head + 1U) % INSTALL_JOURNAL_PENDING_CAPACITY;
    journal->pending_count -= 1U;
    return true;
}

/* Put back the record ring_pop() just handed over: the slot still holds it, so
   a placement that refused the write does not cost the record itself. */
static void ring_unpop(struct install_journal *journal)
{
    journal->pending_head =
        (journal->pending_head + INSTALL_JOURNAL_PENDING_CAPACITY - 1U) %
        INSTALL_JOURNAL_PENDING_CAPACITY;
    journal->pending_count += 1U;
}

/* Replay the ring into a bound journal, naming the overflow first. */
static void replay_pending(struct install_journal *journal)
{
    if (journal->dropped > 0U && journal->overflow_named == 0U) {
        char line[64];
        const char *prefix = "[INSTALL-LOG] state=OVERFLOW dropped=";
        size_t length = 0U;
        while (prefix[length] != '\0' && length + 1U < sizeof(line)) {
            line[length] = prefix[length];
            ++length;
        }
        uint32_t value = journal->dropped;
        char digits[12];
        size_t digit_count = 0U;
        do {
            digits[digit_count++] = (char)('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (digit_count > 0U && length + 1U < sizeof(line)) {
            line[length++] = digits[--digit_count];
        }
        line[length] = '\0';
        if (room_for(journal, 1U) && publish_status(journal, line)) {
            journal->overflow_named = 1U;
        } else {
            /* No slot is left for the notice (or the placement refused it):
               the loss is still a record, so it is named on the notify side
               and not repeated. */
            journal->overflow_named = 1U;
            notify(journal, line);
        }
    }
    const char *text = NULL;
    size_t length = 0U;
    while (journal->state == INSTALL_JOURNAL_READY) {
        /* The same rule as the live path: a record may only go out while the
           slot reserved for the end marker stays free, so the replay can stop
           at the declared capacity and still name the end. */
        if (!room_for(journal, 2U)) {
            go_full(journal);
            break;
        }
        if (!ring_pop(journal, &text, &length)) {
            break;
        }
        if (!write_text(journal, text, length)) {
            /* The placement refused this record: it goes back to the front of
               the ring, in order, and leaves with the next bind. */
            ring_unpop(journal);
            go_unavailable(journal, "WRITE");
            break;
        }
    }
    /*
     * A placement that ran out of room will never take these records: nothing
     * can free a slot while the journal is bound to it, so the rest of the
     * ring is counted in the same counter the live path uses for records that
     * arrive after the journal stopped - the end of the journal is named by
     * go_full() above, and this is the number of records that never reached
     * it.  A journal that stopped on a refused write keeps its ring instead:
     * the placement is still there, a later bind resumes it (the seam is
     * reached again by the console's `storage medium` probe), and requirement
     * 1 staged these records precisely so that one failed write does not lose
     * the machine announcement or the observed storage.
     */
    if (journal->state == INSTALL_JOURNAL_FULL &&
        journal->pending_count > 0U) {
        journal->refused += journal->pending_count;
        journal->pending_count = 0U;
        journal->pending_head = 0U;
    }
}

void install_journal_observe(struct install_journal *journal, const char *text,
                             size_t length)
{
    if (journal == NULL || text == NULL || length == 0U) {
        return;
    }
    /* The funnel may be re-entered from inside a backend write; the record
       that caused it is already being written, so this one is a duplicate. */
    if (journal->publishing) {
        return;
    }
    journal->publishing = true;
    if (length > INSTALL_JOURNAL_TEXT_MAX) {
        length = INSTALL_JOURNAL_TEXT_MAX;
        journal->truncated += 1U;
        if (journal->truncated_named == 0U) {
            journal->truncated_named = 1U;
            /* Name the clamp once, on the medium if there is one and in the
               ring otherwise, so a shortened record is never silent. */
            static const char notice[] =
                "[INSTALL-LOG] state=TRUNCATED limit=440";
            if (journal->state == INSTALL_JOURNAL_UNBOUND) {
                ring_push(journal, notice, sizeof(notice) - 1U);
            } else if (journal->state == INSTALL_JOURNAL_READY &&
                       !write_text(journal, notice, sizeof(notice) - 1U)) {
                go_unavailable(journal, "WRITE");
            }
        }
    }
    switch (journal->state) {
    case INSTALL_JOURNAL_UNBOUND:
        ring_push(journal, text, length);
        break;
    case INSTALL_JOURNAL_READY:
        /* One slot is reserved for the end marker: requirement 5 asks the end
           of the journal to be named, not for the last record to eat the slot
           the naming would need - and the bound is the placement's own
           declared capacity, never a write past it. */
        if (!room_for(journal, 2U)) {
            go_full(journal);
            journal->refused += 1U;
            break;
        }
        if (!write_text(journal, text, length)) {
            go_unavailable(journal, "WRITE");
        }
        break;
    case INSTALL_JOURNAL_UNAVAILABLE:
    case INSTALL_JOURNAL_FULL:
        journal->refused += 1U;
        break;
    }
    journal->publishing = false;
}

bool install_journal_bind(struct install_journal *journal,
                          const struct install_journal_backend *backend)
{
    if (journal == NULL || backend == NULL || backend->write_slot == NULL) {
        return false;
    }
    journal->backend = *backend;
    journal->bound = true;

    uint32_t committed = backend->committed_slots != NULL
                             ? backend->committed_slots(backend->context)
                             : 0U;
    uint32_t capacity = backend->capacity_slots != NULL
                            ? backend->capacity_slots(backend->context)
                            : 0U;
    if (committed == 0U) {
        /* A fresh journal gets its header; a resumed one must not have a
           second header written into the middle of the record stream (F6). */
        uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES];
        format_header(journal, slot);
        if (!backend->write_slot(backend->context, slot)) {
            go_unavailable(journal, "HEADER");
            return false;
        }
        journal->written += 1U;
    } else {
        journal->written = committed;
    }
    journal->capacity = capacity;
    if (backend->sequence != NULL) {
        journal->sequence = backend->sequence(backend->context);
    }
    if (!room_for(journal, 2U)) {
        /* The placement cannot hold a record and the end marker: nothing is
           written past its declared capacity, and the journal says FULL
           rather than letting the placement's refusal read as a write
           failure. */
        go_full(journal);
        return false;
    }
    journal->state = INSTALL_JOURNAL_READY;
    journal->reason = "OK";
    /* A fresh episode may name its own first failure: a journal that was
       unavailable before bind (nothing to write to) is a different story from
       one that fails after the medium was found. */
    journal->unavailable_named = 0U;
    replay_pending(journal);
    return journal->state == INSTALL_JOURNAL_READY;
}

bool install_journal_refuse(struct install_journal *journal, const char *reason)
{
    if (journal == NULL || reason == NULL) {
        return false;
    }
    /*
     * Only the pre-placement decision is refused here: a journal that is
     * already bound (or already unavailable) keeps its own state and its own
     * one-time naming, so this cannot silently stop a running journal.
     */
    if (journal->state != INSTALL_JOURNAL_UNBOUND) {
        return false;
    }
    go_unavailable(journal, reason);
    return false;
}

bool install_journal_flush(struct install_journal *journal)
{
    if (journal == NULL || !journal->bound) {
        return false;
    }
    if (journal->backend.flush == NULL ||
        !journal->backend.flush(journal->backend.context)) {
        go_unavailable(journal, "FLUSH");
        return false;
    }
    return true;
}

/*
 * ITEM-552: the export session's own two calls.
 *
 * `install_journal_write()` is the direct slot write: the caller has already
 * composed the text and owns its length, so there is no truncation notice and
 * no re-entry into the funnel.  It still honours the slot the end marker needs,
 * which is what keeps a full journal from eating the record that would have
 * named its own end.
 */
bool install_journal_write(struct install_journal *journal, const char *text,
                           size_t length)
{
    if (journal == NULL || text == NULL || length == 0U) {
        return false;
    }
    if (!journal->bound || journal->state != INSTALL_JOURNAL_READY) {
        journal->refused += 1U;
        return false;
    }
    if (length > INSTALL_JOURNAL_TEXT_MAX) {
        journal->truncated += 1U;
        length = INSTALL_JOURNAL_TEXT_MAX;
    }
    if (!room_for(journal, 2U)) {
        go_full(journal);
        journal->refused += 1U;
        return false;
    }
    if (!write_text(journal, text, length)) {
        go_unavailable(journal, "WRITE");
        return false;
    }
    return true;
}

/*
 * ITEM-552: restart the stream at slot 1 with the session's own counter.  The
 * placement writes its header as the commit point, so a rewind that is itself
 * interrupted leaves an empty journal rather than a report that mixes two
 * sessions; the export reads the slots back before it says SAVED.
 */
bool install_journal_rewind(struct install_journal *journal, uint64_t sequence)
{
    if (journal == NULL || !journal->bound ||
        journal->backend.rewind == NULL) {
        return false;
    }
    if (!journal->backend.rewind(journal->backend.context, sequence)) {
        go_unavailable(journal, "REWIND");
        return false;
    }
    journal->sequence = sequence;
    journal->written = 1U;
    journal->state = INSTALL_JOURNAL_READY;
    journal->reason = "REWOUND";
    return true;
}

/*
 * ITEM-590: the export's own question - "can a session be written here?" - is
 * broader than "is a record accepted right now?".  A FULL journal is a healthy
 * placement whose declared capacity was reached; `install_journal_rewind()`
 * restarts it, which is why the operator's save must not be refused there.
 * The failed placement (a refused write, a refused flush, a rewind the backend
 * would not take) and the pre-placement state are the two answers that are not
 * saveable, and they keep their own names for the console.
 */
bool install_journal_saveable(const struct install_journal *journal)
{
    if (journal == NULL || !journal->bound) {
        return false;
    }
    return journal->state == INSTALL_JOURNAL_READY ||
           journal->state == INSTALL_JOURNAL_FULL;
}

bool install_journal_commit_sequence(struct install_journal *journal,
                                     uint64_t sequence)
{
    if (journal == NULL || !journal->bound ||
        journal->backend.commit_sequence == NULL) {
        return false;
    }
    if (!journal->backend.commit_sequence(journal->backend.context, sequence)) {
        go_unavailable(journal, "SEQUENCE");
        return false;
    }
    journal->sequence = sequence;
    return true;
}

uint32_t install_journal_written(const struct install_journal *journal)
{
    return journal != NULL ? journal->written : 0U;
}

uint32_t install_journal_capacity_slots(const struct install_journal *journal)
{
    return journal != NULL ? journal->capacity : 0U;
}

uint64_t install_journal_sequence(const struct install_journal *journal)
{
    return journal != NULL ? journal->sequence : 0U;
}

bool install_journal_read_slot(const struct install_journal *journal,
                               uint32_t slot,
                               uint8_t bytes[INSTALL_JOURNAL_SLOT_BYTES])
{
    if (journal == NULL || bytes == NULL || !journal->bound ||
        journal->backend.read_slot == NULL) {
        return false;
    }
    if (slot >= journal->written) {
        /* Only a committed slot is read back: the head is the commit point. */
        return false;
    }
    return journal->backend.read_slot(journal->backend.context, slot, bytes);
}

bool install_journal_panic_flush(struct install_journal *journal)
{
    if (journal == NULL) {
        return false;
    }
    if (!journal->bound) {
        /* Nothing was ever written: say so once, on the notify side, instead
           of pretending the tail is on the medium. */
        if (journal->unavailable_named == 0U) {
            journal->unavailable_named = 1U;
            notify(journal, "[INSTALL-LOG] state=UNAVAILABLE reason=PANIC_UNBOUND");
        }
        return false;
    }
    if (journal->state == INSTALL_JOURNAL_READY) {
        replay_pending(journal);
    }
    return install_journal_flush(journal);
}

/*
 * ITEM-541: the read-only side the refusal page uses.  The reason is never
 * NULL, and the ring accessors walk from the newest staged record backwards so
 * a caller prints a bounded tail without owning (or reordering) the ring.
 */
const char *install_journal_reason(const struct install_journal *journal)
{
    if (journal == NULL || journal->reason == NULL) {
        return "NONE";
    }
    return journal->reason;
}

uint32_t install_journal_pending_count(const struct install_journal *journal)
{
    if (journal == NULL) {
        return 0U;
    }
    return journal->pending_count;
}

const char *install_journal_pending_record(
    const struct install_journal *journal,
    uint32_t newest_index,
    size_t *length)
{
    uint32_t index;

    if (journal == NULL || newest_index >= journal->pending_count) {
        return NULL;
    }
    index = (journal->pending_head + journal->pending_count - 1U -
             newest_index) %
            INSTALL_JOURNAL_PENDING_CAPACITY;
    if (length != NULL) {
        *length = journal->pending_length[index];
    }
    return (const char *)journal->pending[index];
}
