/*
 * ITEM-590: the `[NET-PHY]` record - one composition, one record of its own.
 *
 * See the header for why both halves exist.  The composition is deliberately
 * byte-for-byte the line the boot journal and the diagnostic report printed
 * before this module: the fields, their order and the "0x" + four hex digits
 * form are the operator's contract with these records, and a refactor that
 * changed them would be a different record.
 */

#include "appliance_production_network_phy_record.h"

#include "console.h"
#include "serial.h"

#include <string.h>

const char *appliance_production_network_phy_state_string(
    enum appliance_production_network_phy_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED:
        return "CONFIGURED";
    case APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED:
        return "REFUSED";
    case APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_SKIPPED:
        return "SKIPPED";
    default:
        return "UNAVAILABLE";
    }
}

/*
 * One bounded line.  A piece that does not fit whole is a refusal, not a cut:
 * the record is either the whole truth about this PHY or it is not printed
 * (the caller's zero answer), and the buffer is left empty.
 */
struct phy_record {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void phy_record_text(struct phy_record *record, const char *text)
{
    if (record->overflow || text == NULL) {
        return;
    }
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (record->length + 1U >= record->size) {
            record->overflow = true;
            return;
        }
        record->buffer[record->length++] = text[index];
    }
    record->buffer[record->length] = '\0';
}

static void phy_record_hex16(struct phy_record *record, uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[7];

    text[0] = '0';
    text[1] = 'x';
    text[2] = digits[(value >> 12U) & 0xFU];
    text[3] = digits[(value >> 8U) & 0xFU];
    text[4] = digits[(value >> 4U) & 0xFU];
    text[5] = digits[value & 0xFU];
    text[6] = '\0';
    phy_record_text(record, text);
}

static void phy_record_hex32(struct phy_record *record, uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[11] = "0x00000000";
    for (uint32_t i = 0U; i < 8U; ++i) {
        text[2U + i] = digits[(value >> (28U - i * 4U)) & 0xfU];
    }
    phy_record_text(record, text);
}

static void phy_record_u64(struct phy_record *record, uint64_t value)
{
    char digits[20];
    char text[21];
    size_t count = 0U;

    if (value == 0U) {
        phy_record_text(record, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (uint32_t)(value % 10U));
        value /= 10U;
    }
    for (size_t index = 0U; index < count; ++index) {
        text[index] = digits[count - 1U - index];
    }
    text[count] = '\0';
    phy_record_text(record, text);
}

void appliance_production_network_phy_info_from_driver(
    const struct rtl8168_chip *chip,
    const struct rtl8168_phy_state *phy,
    const struct rtl8168_cold_boot_report *boot,
    uint32_t link_attempts,
    struct appliance_production_network_phy_info *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (chip == NULL || phy == NULL) {
        return;
    }
    /*
     * ITEM-589: a Realtek part is described even when its PHY was skipped or
     * refused - the owner's Toshiba published `link=DOWN` with a live cable and
     * no record at all, because the access rule had skipped the PHY.
     */
    info->available = true;
    info->failure = phy->failure;
    info->page_selection = phy->page_selection;
    info->xid = chip->xid;
    info->access = rtl8168_phy_access_string(phy->access);
    /* ITEM-592: the confirmed profile and the cold start's stage results are
       part of every answer, including a skipped one. */
    if (boot != NULL) {
        info->profile = boot->profile_name;
        info->boot_failed = boot->failed;
        info->failed_stage = rtl8168_stage_string(boot->failed_stage);
        info->first_failure = rtl8168_status_string(boot->first_failure);
        info->firmware_skipped = boot->firmware_skipped;
        for (size_t index = 0U; index < RTL8168_STAGE_COUNT; ++index) {
            info->stages[index] =
                rtl8168_stage_result_string(boot->stages[index]);
        }
    } else {
        info->profile = rtl8168_profile_string(chip->profile);
    }
    info->configure_attempted = phy->configure_attempted;
    info->config_status = rtl8168_status_string(phy->config_status);
    /* No error is not the name of one: the record says NONE. */
    info->first_error = phy->first_error == RTL8168_OK
                            ? NULL
                            : rtl8168_status_string(phy->first_error);
    if (!phy->configure_attempted) {
        info->state = APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_SKIPPED;
        info->reason = phy->access == RTL8168_PHY_ACCESS_MII_PHYAR
                           ? "NOT_ATTEMPTED"
                           : "NO_PHY_MANAGEMENT";
    } else {
        info->state = phy->config_status == RTL8168_OK && phy->valid
                          ? APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED
                          : APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED;
        info->reason =
            info->state == APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED
                ? "PHY_REFUSED" : NULL;
    }
    info->status_valid = phy->status_valid;
    info->link_up = phy->link_up;
    info->autoneg_complete = phy->autoneg_complete;
    info->powered_down = phy->powered_down;
    info->phy_id1 = phy->phy_id1;
    info->phy_id2 = phy->phy_id2;
    info->bmcr = phy->bmcr;
    info->bmsr = phy->bmsr;
    info->advertise = phy->advertise;
    info->advertise_written = phy->advertise_written;
    info->advertise_valid = phy->advertise_valid;
    info->lpa = phy->lpa;
    info->page = phy->page;
    info->page_valid = phy->page_valid;
    info->phy_status_initial = phy->phy_status_initial;
    info->phy_status_latest = phy->phy_status_latest;
    info->phy_status_initial_valid = phy->phy_status_initial_valid;
    info->phy_status_latest_valid = phy->phy_status_latest_valid;
    info->bmcr_initial = phy->bmcr_initial;
    info->bmcr_initial_valid = phy->bmcr_initial_valid;
    info->bmsr_reads = phy->bmsr_reads;
    info->link_seen = phy->link_seen;
    info->aneg_complete_seen = phy->aneg_complete_seen;
    info->advertise_1000 = phy->advertise_1000;
    info->advertise_1000_written = phy->advertise_1000_written;
    info->advertise_1000_valid = phy->advertise_1000_valid;
    info->lpa_1000 = phy->lpa_1000;
    info->lpa_1000_valid = phy->lpa_1000_valid;
    info->bmsr_sequence_count = phy->bmsr_sequence_count;
    for (size_t index = 0U;
         index < phy->bmsr_sequence_count &&
         index < RTL8168_PHY_BMSR_SAMPLES;
         ++index) {
        info->bmsr_sequence[index] = phy->bmsr_sequence[index];
    }

    info->link_attempts = link_attempts;
    info->configures = phy->configures;
    info->refreshes = phy->refreshes;
}

size_t appliance_production_network_phy_record_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size)
{
    struct phy_record record;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available) {
        return 0U;
    }
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    /*
     * Every field the record promises is written through the bounded builder,
     * so a line that cannot fit is refused below instead of published cut.
     */
    phy_record_text(&record, "[NET-PHY] state=");
    phy_record_text(&record,
                    appliance_production_network_phy_state_string(info->state));
    phy_record_text(&record, " xid=");
    phy_record_hex16(&record, info->xid);
    phy_record_text(&record, " access=");
    phy_record_text(&record, info->access != NULL ? info->access : "NONE");
    /* ITEM-592: the confirmed profile travels with the reading, so the record
       can never be read against the wrong generation's expectations. */
    phy_record_text(&record, " profile=");
    phy_record_text(&record,
                    info->profile != NULL ? info->profile : "UNKNOWN");
    phy_record_text(&record, " first_error=");
    phy_record_text(&record,
                    info->first_error != NULL ? info->first_error : "NONE");
    if (info->reason != NULL) {
        phy_record_text(&record, " reason=");
        phy_record_text(&record, info->reason);
    }
    if (info->state == APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED) {
        /* A skipped or refused part prints no registers: zeroed readings would
           claim a PHY this driver never talked to. */
        phy_record_text(&record, " phyid=");
        phy_record_hex16(&record, info->phy_id1);
        phy_record_text(&record, ":");
        phy_record_hex16(&record, info->phy_id2);
        phy_record_text(&record, " bmcr=");
        phy_record_hex16(&record, info->bmcr);
        phy_record_text(&record, " bmsr=");
        phy_record_hex16(&record, info->bmsr);
        /*
         * ITEM-591: the advertisement the part answered, the value the driver
         * programmed and whether the read answered.  The hardware run of
         * 49ac1ffc printed `adv=0x03E1` from the constant, so a PHY that
         * masked the gigabit pairs looked identical to one that took them.
         */
        phy_record_text(&record, " adv=");
        phy_record_hex16(&record, info->advertise);
        phy_record_text(&record, " adv_written=");
        phy_record_hex16(&record, info->advertise_written);
        phy_record_text(&record, " adv_read=");
        phy_record_text(&record, info->advertise_valid ? "OK" : "FAILED");
        phy_record_text(&record, " lpa=");
        phy_record_hex16(&record, info->lpa);
        phy_record_text(&record, " link=");
        phy_record_text(&record, !info->status_valid ? "UNKNOWN" :
                                 info->link_up ? "UP" : "DOWN");
        phy_record_text(&record, " aneg=");
        phy_record_text(&record, !info->status_valid ? "UNKNOWN" :
                                 info->autoneg_complete ? "1" : "0");
        phy_record_text(&record, " pdown=");
        phy_record_u64(&record, info->powered_down ? 1U : 0U);
        phy_record_text(&record, " attempts=");
        phy_record_u64(&record, info->link_attempts);
        phy_record_text(&record, " configured=");
        phy_record_u64(&record, info->configures);
        phy_record_text(&record, " refreshes=");
        phy_record_u64(&record, info->refreshes);
        phy_record_text(&record, " status_read=");
        phy_record_text(&record, info->status_valid ? "OK" : "FAILED");
    }
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

bool appliance_production_network_phy_record_publish(
    const struct appliance_production_network_phy_info *info,
    appliance_production_network_phy_serial_fn serial)
{
    char line[APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX];
    size_t length;
    uint64_t serial_flags;

    length = appliance_production_network_phy_record_line(info, line,
                                                          sizeof(line));
    if (length == 0U) {
        return false;
    }
    /*
     * The record opens and ends here.  Appending these fragments to whatever
     * record the caller had open was the defect: the console's journal mirror
     * ignores fragments that arrive after a record's terminator, so the medium
     * kept no `[NET-PHY]` line at all while the serial stream printed one.
     */
    serial_flags = serial_sequence_begin();
    console_journal_begin();
    if (serial != NULL) {
        serial(line);
        /* The serial copy is a line too: without its own terminator the next
           record would share this one's line, which is what the journal copy
           had already stopped doing. */
        serial("\n");
    }
    console_record_text(line);
    console_record_text("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();
    return true;
}

/*
 * ITEM-592: the cold start's evidence, one bounded record per fact group.
 *
 * The `[NET-PHY]` record has carried the PHY's own reading since ITEM-589/591;
 * the Toshiba's link never came up and that record could not say which stage
 * of the start ran, what the part reported before negotiation, or whether the
 * gigabit advertisement ever left the driver.  These three records answer
 * exactly those questions, and each stays inside one journal slot's text.
 */
size_t appliance_production_network_phy_boot_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size)
{
    struct phy_record record;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available || info->profile == NULL) {
        return 0U;
    }
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    phy_record_text(&record, "[NET-PHY-BOOT] profile=");
    phy_record_text(&record, info->profile);
    phy_record_text(&record, " stages=");
    for (size_t index = 0U; index < RTL8168_STAGE_COUNT; ++index) {
        if (index != 0U) {
            phy_record_text(&record, ",");
        }
        phy_record_text(&record, rtl8168_stage_string(
                                     (enum rtl8168_cold_boot_stage)index));
        phy_record_text(&record, ":");
        phy_record_text(&record, info->stages[index] != NULL
                                     ? info->stages[index]
                                     : "UNKNOWN");
    }
    phy_record_text(&record, " first=");
    phy_record_text(&record, info->boot_failed ? info->failed_stage
                                               : "NONE");
    phy_record_text(&record, " firmware=");
    phy_record_text(&record, info->firmware_skipped ? "SKIPPED" : "NONE");
    phy_record_text(&record, " phy_status0=");
    if (info->phy_status_initial_valid) {
        phy_record_hex16(&record, info->phy_status_initial);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " phy_status=");
    if (info->phy_status_latest_valid) {
        phy_record_hex16(&record, info->phy_status_latest);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " bmcr0=");
    if (info->bmcr_initial_valid) {
        phy_record_hex16(&record, info->bmcr_initial);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " aneg_done=");
    phy_record_text(&record, !info->status_valid ? "UNKNOWN" :
                             info->autoneg_complete ? "YES" : "NO");
    if (info->boot_failed) {
        phy_record_text(&record, " error=");
        phy_record_text(&record, info->first_failure);
    }
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

size_t appliance_production_network_phy_gigabit_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size)
{
    struct phy_record record;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available ||
        info->state != APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED) {
        return 0U;
    }
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    phy_record_text(&record, "[NET-PHY-GIG] adv1000=");
    phy_record_hex16(&record, info->advertise_1000);
    phy_record_text(&record, " adv1000_written=");
    phy_record_hex16(&record, info->advertise_1000_written);
    phy_record_text(&record, " adv1000_read=");
    phy_record_text(&record, info->advertise_1000_valid ? "OK" : "FAILED");
    phy_record_text(&record, " lpa1000=");
    phy_record_hex16(&record, info->lpa_1000);
    phy_record_text(&record, " lpa1000_read=");
    phy_record_text(&record, info->lpa_1000_valid ? "OK" : "FAILED");
    phy_record_text(&record, " lpa=");
    phy_record_hex16(&record, info->lpa);
    phy_record_text(&record, " page=");
    phy_record_hex16(&record, info->page_valid ? info->page : 0xFFFFU);
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

size_t appliance_production_network_phy_sample_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer,
    size_t size)
{
    struct phy_record record;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available || info->bmsr_reads == 0U) {
        return 0U;
    }
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    phy_record_text(&record, "[NET-PHY-SAMPLES] bmsr=");
    phy_record_hex16(&record, info->bmsr);
    phy_record_text(&record, " bmsr_seq=");
    for (size_t index = 0U; index < info->bmsr_sequence_count; ++index) {
        if (index != 0U) {
            phy_record_text(&record, ",");
        }
        phy_record_hex16(&record, info->bmsr_sequence[index]);
    }
    if (info->bmsr_sequence_count == 0U) {
        phy_record_text(&record, "none");
    }
    phy_record_text(&record, " reads=");
    phy_record_u64(&record, info->bmsr_reads);
    phy_record_text(&record, " link_seen=");
    phy_record_text(&record, info->link_seen ? "1" : "0");
    phy_record_text(&record, " aneg_seen=");
    phy_record_text(&record, info->aneg_complete_seen ? "1" : "0");
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

size_t appliance_production_network_phy_failure_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer, size_t size)
{
    struct phy_record record;
    const struct rtl8168_phy_failure *failure;
    const struct rtl8168_phy_transaction *transaction;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available || !info->failure.valid) {
        return 0U;
    }
    record = (struct phy_record){buffer, size, 0U, false};
    failure = &info->failure;
    transaction = &failure->transaction;
    phy_record_text(&record, "[NET-PHY-FAIL] step=");
    phy_record_u64(&record, failure->step);
    phy_record_text(&record, " phase=");
    phy_record_text(&record, rtl8168_phy_phase_string(failure->phase));
    phy_record_text(&record, " op=");
    phy_record_text(&record, transaction->write ? "WRITE" : "READ");
    phy_record_text(&record, " page_req=");
    phy_record_hex16(&record, failure->page);
    phy_record_text(&record, " selector_req=");
    if (failure->selector_valid) {
        phy_record_hex16(&record, failure->selector);
    } else {
        phy_record_text(&record, "NONE");
    }
    phy_record_text(&record, " reg=");
    phy_record_hex16(&record, transaction->reg);
    phy_record_text(&record, " reason=");
    phy_record_text(&record, rtl8168_phy_failure_reason_string(transaction->reason));
    phy_record_text(&record, " cmd=");
    phy_record_hex32(&record, transaction->command);
    phy_record_text(&record, " last=");
    if (transaction->last_valid) {
        phy_record_hex32(&record, transaction->last);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " polls=");
    phy_record_u64(&record, transaction->polls);
    phy_record_text(&record, " expected=");
    if (transaction->expected_valid) {
        phy_record_hex16(&record, transaction->expected);
    } else {
        phy_record_text(&record, "NONE");
    }
    phy_record_text(&record, " observed=");
    if (transaction->observed_valid) {
        phy_record_hex16(&record, transaction->observed);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " restore=");
    phy_record_text(&record, !failure->restore_attempted ? "SKIPPED" :
                             failure->restore_ok ? "OK" : "FAILED");
    phy_record_text(&record, " restore_page=");
    if (failure->restore_page_valid) {
        phy_record_hex16(&record, failure->restore_page);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

/* ITEM-596: both sides of the first refused page selection, using only
 * samples already obtained by the command path. No extra device access. */
size_t appliance_production_network_phy_page_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer, size_t size)
{
    struct phy_record record;
    const struct rtl8168_phy_failure *failure;
    const struct rtl8168_phy_transaction *write;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available || !info->failure.valid ||
        !info->failure.page_write_valid) {
        return 0U;
    }
    record = (struct phy_record){buffer, size, 0U, false};
    failure = &info->failure;
    write = &failure->page_write;
    phy_record_text(&record, "[NET-PHY-PAGE] write_cmd=");
    phy_record_hex32(&record, write->command);
    phy_record_text(&record, " write_last=");
    if (write->last_valid) {
        phy_record_hex32(&record, write->last);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " write_polls=");
    phy_record_u64(&record, write->polls);
    phy_record_text(&record, " read_payload=");
    if (failure->transaction.payload_valid) {
        phy_record_hex32(&record, failure->transaction.payload);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

/* First completed non-echo observation, not a claim that the bank changed. */
size_t appliance_production_network_phy_select_line(
    const struct appliance_production_network_phy_info *info,
    char *buffer, size_t size)
{
    struct phy_record record;
    const struct rtl8168_phy_page_observation *selection;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (info == NULL || !info->available || info->page_selection.zero_reads == 0U) {
        return 0U;
    }
    selection = &info->page_selection;
    record = (struct phy_record){buffer, size, 0U, false};
    phy_record_text(&record, "[NET-PHY-SELECT] policy=EVL_ZERO_UNVERIFIED zero_reads=");
    phy_record_u64(&record, selection->zero_reads);
    phy_record_text(&record, " requested=");
    phy_record_hex16(&record, selection->requested);
    phy_record_text(&record, " observed=");
    if (selection->read.observed_valid) {
        phy_record_hex16(&record, selection->read.observed);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " write_cmd=");
    phy_record_hex32(&record, selection->write.command);
    phy_record_text(&record, " write_last=");
    if (selection->write.last_valid) {
        phy_record_hex32(&record, selection->write.last);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    phy_record_text(&record, " write_polls=");
    phy_record_u64(&record, selection->write.polls);
    phy_record_text(&record, " read_payload=");
    if (selection->read.payload_valid) {
        phy_record_hex32(&record, selection->read.payload);
    } else {
        phy_record_text(&record, "UNREAD");
    }
    if (record.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return record.length;
}

void appliance_production_network_phy_detail_publish(
    const struct appliance_production_network_phy_info *info,
    appliance_production_network_phy_serial_fn serial)
{
    char line[APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX];
    static const struct {
        size_t (*compose)(const struct appliance_production_network_phy_info *,
                          char *, size_t);
    } lines[] = {
        { appliance_production_network_phy_boot_line },
        { appliance_production_network_phy_gigabit_line },
        { appliance_production_network_phy_sample_line },
        { appliance_production_network_phy_failure_line },
        { appliance_production_network_phy_page_line },
        { appliance_production_network_phy_select_line },
    };

    if (info == NULL) {
        return;
    }
    for (size_t index = 0U; index < sizeof(lines) / sizeof(lines[0]);
         ++index) {
        size_t length =
            lines[index].compose(info, line, sizeof(line));
        uint64_t serial_flags;

        if (length == 0U) {
            continue;
        }
        serial_flags = serial_sequence_begin();
        console_journal_begin();
        if (serial != NULL) {
            serial(line);
            serial("\n");
        }
        console_record_text(line);
        console_record_text("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}
