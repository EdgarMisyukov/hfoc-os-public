#include "production_installer_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "installer_layout_contract.h"
#include "serial.h"

enum production_installer_status
production_installer_publish_layout_record(
    const struct production_installer *installer)
{
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    enum installer_layout_contract_status contract_status;
    size_t length = 0U;
    uint64_t serial_flags;

    if (installer == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (!installer->layout_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    contract_status = installer_layout_contract_render_record(
        &installer->layout, record, sizeof(record), &length);
    if (contract_status != INSTALLER_LAYOUT_CONTRACT_OK ||
        length == 0U) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    serial_flags = serial_sequence_begin();
    serial_write(record);
    serial_sequence_end(serial_flags);
    /*
     * ITEM-405: the same record used to be painted on the local console, after
     * the serial sequence.  ITEM-478 sends it to the durable journal: the
     * record is the layout this run wrote, read back from the table it
     * produced, and `BOOTLOG` shows it on demand.
     */
    console_publish_journal(record, length);
    return PRODUCTION_INSTALLER_OK;
}
