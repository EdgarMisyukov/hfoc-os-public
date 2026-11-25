/*
 * ITEM-562: the two-controller verdict, as production code a host proof runs.
 *
 * See kernel/include/ehci_verdict.h for why this is not one global string: the
 * hardware run that reported `DESCRIPTOR_FAILED/NO_DEVICE` for the whole
 * machine had two controllers, and the answer came from the last one scanned
 * while the other one's state was never printed anywhere.
 */

#include "ehci_verdict.h"

#include <stddef.h>

static void verdict_copy(char *target, size_t size, const char *source) {
    size_t length = 0U;

    if (target == NULL || size == 0U) {
        return;
    }
    while (source != NULL && source[length] != '\0' && length + 1U < size) {
        target[length] = source[length];
        ++length;
    }
    target[length] = '\0';
}

/* How far a controller got.  Ordered: what the walk found outranks what it did
   not, a bound medium outranks every refusal, and an initialization refusal is
   concrete too - it is a named step, not an absence of information. */
static int verdict_rank(const struct ehci_verdict_entry *entry) {
    if (entry->bound && entry->status == EHCI_STATUS_OK) {
        return 5;
    }
    if (!entry->started) {
        return entry->status != EHCI_STATUS_UNKNOWN ? 4 : 0;
    }
    switch (entry->outcome) {
    case EHCI_WALK_OUTCOME_TRANSFER:
    case EHCI_WALK_OUTCOME_HUB:
    case EHCI_WALK_OUTCOME_PORT_RESET:
    case EHCI_WALK_OUTCOME_ATTACH:
        return 4;
    case EHCI_WALK_OUTCOME_MEDIUM:
        return 3;
    case EHCI_WALK_OUTCOME_NO_DEVICE:
        return 2;
    case EHCI_WALK_OUTCOME_NO_PORT:
        return 1;
    case EHCI_WALK_OUTCOME_OK:
    case EHCI_WALK_OUTCOME_NONE:
        break;
    }
    return 0;
}

/* The status and detail a controller's entry reports.  A specific status the
   driver already named is kept - the attach path, the port reset and the
   initialization module each name their own - and only a bare `OK`/`UNKNOWN`
   after a walked controller borrows the outcome's name. */
static void verdict_entry_answer(const struct ehci_verdict_entry *entry,
                                 enum ehci_status *status,
                                 const char **detail) {
    if (entry->bound && entry->status == EHCI_STATUS_OK) {
        *status = EHCI_STATUS_OK;
        *detail = "STORAGE_BOUND";
        return;
    }
    if (!entry->started) {
        *status = entry->status != EHCI_STATUS_UNKNOWN
                      ? entry->status
                      : EHCI_STATUS_CONTROLLER_ABSENT;
        *detail = entry->detail != NULL ? entry->detail : "NOT_STARTED";
        return;
    }
    if (entry->status != EHCI_STATUS_OK && entry->status != EHCI_STATUS_UNKNOWN) {
        *status = entry->status;
        *detail = entry->detail != NULL
                      ? entry->detail
                      : (entry->failure != NULL ? entry->failure : "REFUSED");
        return;
    }
    {
        enum ehci_status mapped = ehci_walk_outcome_status(entry->outcome);

        if (mapped == EHCI_STATUS_UNKNOWN) {
            /* ATTACH and an unfinished walk: the driver's own status is the
               only one that names it, and it was OK - say the walk found
               nothing it could use instead of inventing a stage. */
            mapped = EHCI_STATUS_NO_DEVICE;
        }
        *status = mapped;
        *detail = entry->failure != NULL ? entry->failure
                                         : (entry->detail != NULL
                                                ? entry->detail
                                                : "NO_DEVICE");
    }
}

void ehci_verdict_fill(struct ehci_verdict_entry *entry,
                       uint8_t bus,
                       uint8_t device,
                       uint8_t function,
                       bool started,
                       enum ehci_status status,
                       const char *detail,
                       uint32_t ports,
                       const struct ehci_walk_report *report,
                       bool bound) {
    if (entry == NULL) {
        return;
    }
    entry->bus = bus;
    entry->device = device;
    entry->function = function;
    entry->started = started;
    entry->status = status;
    entry->detail = detail;
    entry->ports = ports;
    entry->connected = 0U;
    entry->attached = 0U;
    entry->outcome = EHCI_WALK_OUTCOME_NONE;
    entry->failure = NULL;
    entry->failure_detail = NULL;
    entry->bound = bound;
    if (report == NULL) {
        return;
    }
    entry->connected = report->ports_connected;
    entry->attached = report->attached;
    entry->outcome = report->outcome;
    entry->failure = report->failure;
    entry->failure_detail = report->failure_detail;
    if (bound) {
        entry->status = EHCI_STATUS_OK;
    }
}

void ehci_verdict_choose(const struct ehci_verdict_entry *entries,
                         uint32_t count,
                         struct ehci_verdict_choice *choice) {
    int best = -1;
    int best_rank = -1;

    if (choice == NULL) {
        return;
    }
    choice->index = -1;
    choice->status = EHCI_STATUS_CONTROLLER_ABSENT;
    choice->detail = "NO_EHCI_FUNCTION";
    choice->storage_bound = false;
    choice->controllers = count;
    if (entries == NULL || count == 0U) {
        return;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        const int rank = verdict_rank(&entries[index]);

        /* Strictly greater: equal information keeps the earlier controller, so
           the answer is the first refusal in scan order, not the last one to
           be scanned. */
        if (rank > best_rank) {
            best_rank = rank;
            best = (int)index;
        }
    }
    if (best < 0) {
        return;
    }
    verdict_entry_answer(&entries[best], &choice->status, &choice->detail);
    choice->index = best;
    choice->storage_bound = entries[best].bound &&
                            entries[best].status == EHCI_STATUS_OK;
}

/* Append one `bus:dev.fn=status/detail` field whole, or not at all. */
static size_t verdict_append_entry(size_t length,
                                   char *buffer,
                                   size_t size,
                                   const struct ehci_verdict_entry *entry) {
    static const char digits[] = "0123456789ABCDEF";
    char field[96];
    char status_name[32];
    size_t used = 0U;
    size_t needed;
    enum ehci_status status;
    const char *detail = NULL;

    verdict_entry_answer(entry, &status, &detail);
    verdict_copy(status_name, sizeof(status_name), ehci_status_string(status));
    field[used++] = digits[(entry->bus >> 4U) & 0x0FU];
    field[used++] = digits[entry->bus & 0x0FU];
    field[used++] = ':';
    field[used++] = digits[(entry->device >> 4U) & 0x0FU];
    field[used++] = digits[entry->device & 0x0FU];
    field[used++] = '.';
    field[used++] = digits[entry->function & 0x0FU];
    field[used++] = '=';
    for (size_t index = 0U; status_name[index] != '\0' &&
                            used + 2U < sizeof(field); ++index) {
        field[used++] = status_name[index];
    }
    if (detail != NULL && detail[0] != '\0' && used + 2U < sizeof(field)) {
        field[used++] = '/';
        for (size_t index = 0U; detail[index] != '\0' &&
                                used + 2U < sizeof(field); ++index) {
            field[used++] = detail[index];
        }
    }
    field[used] = '\0';
    needed = used + (length != 0U ? 1U : 0U);
    if (length + needed + 1U > size) {
        return length;
    }
    if (length != 0U) {
        buffer[length++] = ';';
    }
    for (size_t index = 0U; index < used; ++index) {
        buffer[length + index] = field[index];
    }
    length += used;
    buffer[length] = '\0';
    return length;
}

size_t ehci_verdict_render(const struct ehci_verdict_entry *entries,
                           uint32_t count,
                           char *buffer,
                           size_t size) {
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (entries == NULL) {
        return 0U;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        length = verdict_append_entry(length, buffer, size, &entries[index]);
    }
    return length;
}
