/*
 * ITEM-394 product install run on the installer medium.
 *
 * The controller (ITEM-351), the layout contract (ITEM-391), the planner and
 * writer (ITEM-340/341), the signed-media gate (ITEM-371) and the persistent
 * state initializer all existed; this module is the missing product entry
 * point that ordinary boot reaches, plus the audit records the hardware
 * acceptance run quotes.
 *
 * Order of decisions, all published before any write:
 *
 *   1. the boot entry's command line selects whether this is an install run;
 *   2. the media declaration names the confirmation source and the one disk;
 *   3. every candidate disk is published with its identity and geometry;
 *   4. the controller re-checks the identity, the signed media and the
 *      hardware policy, derives the ITEM-391 layout, and publishes the
 *      preview with the contracted numbers;
 *   5. only then does the writer run, and the layout record it publishes is
 *      read back from the GPT the writer produced.
 *
 * ITEM-411 adds the end of the run: once the layout, the ESP payload set, the
 * state volume and their records are all verified, the flow publishes one
 * `complete` record - which disk, that the installation medium must be removed,
 * which mechanism resets the machine and how many seconds that takes - and
 * reboots.  A run that refused, failed or is still waiting for a console
 * confirmation never reaches that point and leaves the machine running.
 */

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA

#include "framebuffer.h"
#include "installer_notice.h"
#include "installer_storage.h"
#include "production_install_flow.h"
#include "appliance_boot_log_store.h"

#include "appliance_production_storage.h"
#include "nvme.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "arch/x86_64/timer.h"
#include "console.h"
#include "nic_backends.h"
#include "nic_discovery.h"
#include "nic_registry.h"
#include "pci_io.h"
#include "efi_boot_environment.h"
#include "image_signature.h"
#include "installer_hardware_discovery.h"
#include "installer_storage_screen.h"
#include "installer_layout.h"
#include "installer_supported_hardware.h"
#include "kstring.h"
#include "platform_reset.h"
#include "production_installer.h"
#include "production_installer_bios_boot.h"
#include "production_installer_boot_stage.h"
#include "production_installer_efi.h"
#include "production_installer_esp.h"
#include "installer_boot_menu.h"
#include "installer_gpt_preserve.h"
#include "production_install_media.h"
#include "production_installer_layout.h"
#include "production_installer_preserve.h"
#include "production_installer_state.h"
#include "appliance_admin_credential.h"
#include "appliance_host_seed.h"
#include "appliance_ssh_authorized_key.h"
#include "serial.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct production_install_flow_state {
    bool engaged;
    bool prepared;
    const struct production_install_flow_media *media;
    struct production_install_media_declaration declaration;
    struct production_install_media_plan plan;
    struct installer_discovery_report report;
    char models[INSTALLER_DISCOVERY_DISK_CAPACITY]
               [APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH];
    struct block_device *devices[INSTALLER_DISCOVERY_DISK_CAPACITY];
    /*
     * ITEM-409: what the discovery made of every disk it was handed.  Until
     * this item the status was discarded, so a disk whose partition table
     * could not be read was published as a candidate with no partitions and
     * nothing said why - and the boot menu it should have contributed to was
     * installed without it, silently.  The status is published per disk now.
     */
    enum installer_discovery_status discovery[INSTALLER_DISCOVERY_DISK_CAPACITY];
    /*
     * ITEM-409: whether the menu was built, and - when it was not - the token
     * the record names as the reason the sealed configuration is installed.
     */
    bool menu_built;
    const char *menu_reason;
    /*
     * ITEM-446: the command line the generated menu entry carries.  It holds
     * the management scope and the declared network in one bounded buffer,
     * because the menu entry borrows the bytes for the whole planning call.
     */
    char cmdline[INSTALLER_BOOT_MENU_CMDLINE_LENGTH];
    /*
     * ITEM-490: whether the operator entry's storage screen has already named
     * the disk.  Until it does, the pending record stands and the console
     * confirmation has nothing to confirm.
     */
    bool choice_applied;
    /*
     * ITEM-500: what the run refused with, the entry it was on, and the line
     * the storage screen shows when the declared disk is not on this machine.
     * The refusal is remembered so the boot loop can paint the reason on the
     * screen the operator is looking at, instead of the machine silently
     * booting from the medium.
     */
    bool refused;
    enum production_install_media_refusal refusal;
    enum production_install_media_mode mode;
    /*
     * ITEM-586: the medium declared `layout=update`, so this run keeps the
     * installed appliance's state.  Only the explicit declaration sets it; a
     * disk that happens to carry HFOC partitions never selects it.
     */
    bool preserve_state;
    char choice_note[PRODUCTION_INSTALL_FLOW_CHOICE_NOTE_MAX];
    /*
     * ITEM-501: the partition the operator gave up on the chosen disk, when he
     * did.  `release_chosen` is set from the partition screen (or, on the
     * unattended path, from the declaration) before the plan is built, and the
     * install then removes exactly that entry and puts the three appliance
     * roles in the space it occupied.  Zero/NULL means the ordinary free-space
     * path.
     */
    bool release_chosen;
    uint32_t release_index;
    /*
     * ITEM-514: the scheme the operator chose on the scheme screen, NONE until
     * then.  It overrides the declaration (an operator may choose a more
     * destructive scheme than the medium declared), and the layout record
     * names both sources.
     */
    enum installer_storage_scheme chosen_scheme;
    /*
     * ITEM-501: the controller's own status of a plan that refused after the
     * operator had chosen - the text the refusal page paints.  Empty when no
     * such refusal happened.
     */
    char controller_refusal[PRODUCTION_INSTALL_FLOW_CHOICE_NOTE_MAX];
    /*
     * ITEM-490: the media the flow was handed, copied.  The boot function that
     * builds it returns to its caller before the operator's screen runs, so a
     * pointer to it would dangle by the time the choice is applied - and a
     * re-plan built on a dangling read would refuse a machine that is fine.
     */
    struct production_install_flow_media media_storage;
    /*
     * ITEM-506 increment 4: the account the install screen collected, kept as
     * the model's record only.  `operator_credential_set` means `apply()` must
     * commit it and publish `admin=SET`; the password itself never reaches
     * this structure.  The salt source is a seam because the installer kernel
     * has no hardware entropy owner.
     */
    struct appliance_admin_credential operator_credential;
    bool operator_credential_set;
    /*
     * ITEM-598: the password is retained only in this non-exported RAM buffer
     * between the account screen and apply(), where it rewraps the host seed.
     * It is never copied into a record, journal, command line or diagnostic.
     */
    char operator_password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    size_t operator_password_length;
    bool existing_credential_verified;
    struct appliance_host_seed_media_record host_seed_record;
    bool host_seed_record_valid;
    /*
     * ITEM-606: the validated public key and the fingerprint the medium's
     * token declared.  The blob is public; it is cleared when the run ends.
     */
    bool ssh_key_present;
    uint8_t ssh_key_blob[192];
    size_t ssh_key_blob_length;
    char ssh_key_fingerprint[PRODUCTION_INSTALL_MEDIA_SSH_KEY_MAX];
    production_install_flow_random_fn credential_random;
    void *credential_random_context;
    enum production_install_flow_credential_status credential_status;
    /*
     * ITEM-518: the observed storage report - the probe's evidence with the
     * discovery's own verdict folded into every disk row.  The refusal page and
     * the console `storage` command both read this one model, so the screen a
     * person reads and the command he types say the same numbers.
     */
    struct appliance_storage_report storage_report;
};

static struct production_install_flow_state production_install_flow;

/*
 * ITEM-398: the tables the preserving install and the boot menu need are
 * kilobytes, not bytes (the GPT bookkeeping is about 12 KiB), so they live here
 * instead of on the boot stack the kernel was handed.
 */
static struct installer_gpt_preserve_table production_install_flow_gpt;
static struct installer_boot_menu production_install_flow_menu;
static struct installer_boot_menu_inventory production_install_flow_inventory;
static struct installer_dual_boot_plan production_install_flow_dual;
static struct installer_gpt_preserve_reuse production_install_flow_reuse;

/*
 * The menu scan's view of the machine: one block device per discovered disk.
 * The scan only ever reads through it, so a disk that is not the target - the
 * acceptance host's Debian disk, for instance - is read and never written.
 */
static struct block_device *production_install_flow_device_lookup(
    void *context,
    uint32_t disk_id)
{
    (void)context;
    if (disk_id == 0U ||
        disk_id > INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return NULL;
    }
    return production_install_flow.devices[disk_id - 1U];
}

static void production_install_flow_publish(const char *text)
{
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write(text);
    serial_sequence_end(serial_flags);
    /*
     * ITEM-405: the same record used to be painted on the local console for an
     * acceptance host with no serial capture.  ITEM-478 sends it to the durable
     * journal instead: the serial sequence above stays one contiguous IRQ-off
     * critical section, the journal write is additive and never touches the
     * framebuffer, and the `BOOTLOG` command still shows the record on demand.
     */
    console_publish_journal(text, strlen(text));
}

/*
 * ITEM-405: one record, assembled once.
 *
 * The records below used to be written to serial fragment by fragment inside a
 * sequence.  They are assembled into one bounded buffer instead, so the mirror
 * above can publish exactly the bytes that went to serial - the same fragments,
 * in the same order, in one serial sequence.  The bound is a bound, not a
 * truncation: the longest record this flow publishes is well under it, and an
 * overflow is refused rather than silently cut.
 */
#define PRODUCTION_INSTALL_FLOW_LINE_MAX 1200U

struct production_install_flow_line {
    char text[PRODUCTION_INSTALL_FLOW_LINE_MAX];
    size_t length;
    bool overflow;
};

static void production_install_flow_line_begin(
    struct production_install_flow_line *line)
{
    memset(line, 0, sizeof(*line));
}

static void production_install_flow_line_append(
    struct production_install_flow_line *line,
    const char *text)
{
    size_t length = 0U;

    if (line->overflow || text == NULL) {
        line->overflow = true;
        return;
    }
    while (text[length] != '\0') {
        ++length;
    }
    if (line->length + length >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    memcpy(line->text + line->length, text, length);
    line->length += length;
}

static void production_install_flow_line_u64(
    struct production_install_flow_line *line,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (line->overflow) {
        return;
    }
    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    if (line->length + count >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    while (count > 0U) {
        line->text[line->length++] = digits[--count];
    }
}

static void production_install_flow_line_publish(
    struct production_install_flow_line *line)
{
    if (line->overflow) {
        return;
    }
    production_install_flow_publish(line->text);
}

static void production_install_flow_publish_record(
    enum production_install_media_status status,
    const char *record)
{
    if (status != PRODUCTION_INSTALL_MEDIA_OK) {
        production_install_flow_publish(
            "[PRODUCTION-INSTALL] record=UNAVAILABLE\n");
        return;
    }
    production_install_flow_publish(record);
}

static void production_install_flow_clear_operator_secret(void)
{
    volatile unsigned char *password =
        (volatile unsigned char *)production_install_flow.operator_password;
    size_t index;

    for (index = 0U; index < sizeof(production_install_flow.operator_password);
         ++index) {
        password[index] = 0U;
    }
    production_install_flow.operator_password_length = 0U;
    production_install_flow.existing_credential_verified = false;
}

static void production_install_flow_clear_host_seed(void)
{
    appliance_host_seed_clear_media(
        &production_install_flow.host_seed_record);
    production_install_flow.host_seed_record_valid = false;
}

static void production_install_flow_clear_ssh_key(void)
{
    memset(production_install_flow.ssh_key_blob, 0,
           sizeof(production_install_flow.ssh_key_blob));
    production_install_flow.ssh_key_blob_length = 0U;
    production_install_flow.ssh_key_present = false;
    memset(production_install_flow.ssh_key_fingerprint, 0,
           sizeof(production_install_flow.ssh_key_fingerprint));
}

static void production_install_flow_refuse(
    enum production_install_media_refusal refusal)
{
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    /*
     * ITEM-500: remember the refusal so the boot loop can put the reason on
     * the screen of the operator entry.  Nothing here changes the decision:
     * the record is published exactly as before.
     */
    production_install_flow.refused = true;
    production_install_flow.refusal = refusal;
    production_install_flow_clear_operator_secret();
    production_install_flow_clear_host_seed();
    production_install_flow_clear_ssh_key();
    production_install_flow_publish_record(
        production_install_media_render_refusal(
            refusal, &production_install_flow.declaration, record,
            sizeof(record), &length),
        record);
}

static void production_install_flow_refuse_controller(
    const char *status_text,
    const char *detail_text)
{
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    production_install_flow_clear_operator_secret();
    production_install_flow_clear_host_seed();
    production_install_flow_clear_ssh_key();
    production_install_flow_publish_record(
        production_install_media_render_controller_refusal(
            status_text, detail_text, record, sizeof(record), &length),
        record);
}

/*
 * ITEM-501: the bounded copy of a controller status the refusal page paints.
 */
static void production_install_flow_remember_controller_refusal(
    const char *status_text)
{
    size_t index = 0U;

    if (status_text == NULL) {
        production_install_flow.controller_refusal[0] = '\0';
        return;
    }
    while (status_text[index] != '\0' &&
           index + 1U < sizeof(production_install_flow.controller_refusal)) {
        production_install_flow.controller_refusal[index] =
            status_text[index];
        ++index;
    }
    production_install_flow.controller_refusal[index] = '\0';
}

static void production_install_flow_failed(
    const char *stage,
    const char *status_text)
{
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    production_install_flow_clear_operator_secret();
    production_install_flow_clear_host_seed();
    production_install_flow_clear_ssh_key();
    production_install_flow_publish_record(
        production_install_media_render_failure(
            stage, status_text, record, sizeof(record), &length),
        record);
}

static enum installer_disk_controller_kind
production_install_flow_controller_kind(
    enum appliance_production_boot_backend_kind kind)
{
    switch (kind) {
    case APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI:
        return INSTALLER_DISK_CONTROLLER_SATA;
    case APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME:
        return INSTALLER_DISK_CONTROLLER_NVME;
    default:
        return INSTALLER_DISK_CONTROLLER_UNKNOWN;
    }
}

/*
 * The discovery report is built from the disks the storage owner probed, one
 * identity string each.  A disk that cannot be identified is not published as
 * a candidate, so it can never be selected by accident.
 */
/*
 * ITEM-492: the installer's own view of the machine's network devices.
 *
 * The install medium's kernel carries no network stack, and it must not need
 * one: the controllers are enumerated on the PCI bus and matched against the
 * registry of drivers this image carries, so the installer learns which NICs
 * exist and whether it could drive each one without asking any driver.  A
 * machine with no controller, a controller nobody wrote a driver for and a
 * controller whose driver refused are three different readings, and none of
 * them stops the install - `install=CONTINUES` is the product rule, and the
 * record states it rather than leaving it to be inferred.
 */
static void production_install_flow_publish_nic(void)
{
    static struct production_install_flow_line line;
    struct nic_device devices[NIC_DEVICES_MAX];
    enum nic_discovery_status status;
    size_t count = 0U;
    size_t index;

    nic_backends_register_product();
    status = nic_discovery_scan(pci_io_kernel(), NULL, devices, NIC_DEVICES_MAX,
                                &count);
    if (status == NIC_DISCOVERY_OK) {
        for (index = 0U; index < count; ++index) {
            /* One line per controller, like every other record this flow
             * publishes: the writer appends nothing of its own. */
            char record[NIC_RECORD_BYTES + 2U];
            const size_t length = nic_device_record(
                &devices[index], record, NIC_RECORD_BYTES);

            if (length == 0U) {
                continue;
            }
            record[length] = '\n';
            record[length + 1U] = '\0';
            production_install_flow_publish(record);
        }
    }
    production_install_flow_line_begin(&line);
    production_install_flow_line_append(&line, "[INSTALLER-NIC] devices=");
    production_install_flow_line_u64(&line, (uint64_t)count);
    production_install_flow_line_append(&line, " network=");
    production_install_flow_line_append(
        &line, status == NIC_DISCOVERY_OK
                   ? nic_state_name(
                         nic_discovery_overall_state(devices, count))
                   : nic_discovery_status_string(status));
    production_install_flow_line_append(&line, " install=CONTINUES\n");
    production_install_flow_line_publish(&line);
}

/*
 * ITEM-518: fold the discovery's verdict into the observed storage row the
 * probe filled.  The probe's reason wins when it already rejects the disk - the
 * scan never saw an identity or a geometry the probe could not read - and a
 * table the scan could not read stays an annotation, not a rejection: the erase
 * scheme exists for exactly that disk.
 */
static enum appliance_storage_report_reason
production_install_flow_observed_reason(
    enum appliance_storage_report_reason probe_reason,
    enum installer_discovery_status scan)
{
    if (probe_reason != APPLIANCE_STORAGE_REPORT_OK) {
        return probe_reason;
    }
    switch (scan) {
    case INSTALLER_DISCOVERY_OK:
        return APPLIANCE_STORAGE_REPORT_OK;
    case INSTALLER_DISCOVERY_IO_ERROR:
        return APPLIANCE_STORAGE_REPORT_IO_ERROR;
    case INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE:
        return APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE;
    case INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE:
        return APPLIANCE_STORAGE_REPORT_INVALID_DEVICE;
    default:
        /* Any other parser verdict leaves the table unread, nothing worse. */
        return APPLIANCE_STORAGE_REPORT_INVALID_PARTITION_TABLE;
    }
}

/* ITEM-518: the accepted count is the model's own rule, not a second opinion. */
static void production_install_flow_observed_recount(
    struct appliance_storage_report *report)
{
    size_t accepted = 0U;
    size_t index;

    if (report == NULL) {
        return;
    }
    for (index = 0U; index < report->disk_count &&
                     index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY;
         ++index) {
        if (report->disks[index].present &&
            appliance_storage_report_reason_usable(
                report->disks[index].reason)) {
            ++accepted;
        }
    }
    report->accepted = accepted;
}

static bool production_install_flow_report_build(
    const struct production_install_flow_media *media)
{
    struct appliance_production_boot_backend
        backends[APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY];
    size_t backend_count = 0U;
    size_t disk_count;
    size_t index;

    memset(&production_install_flow.report, 0,
           sizeof(production_install_flow.report));
    memset(production_install_flow.models, 0,
           sizeof(production_install_flow.models));
    memset(production_install_flow.devices, 0,
           sizeof(production_install_flow.devices));
    memset(production_install_flow.discovery, 0,
           sizeof(production_install_flow.discovery));
    appliance_storage_report_init(&production_install_flow.storage_report);
    if (installer_discovery_init(&production_install_flow.report) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_firmware(
            &production_install_flow.report,
            media->firmware_mode) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_cpu(
            &production_install_flow.report,
            media->cpu_cores != 0U ? media->cpu_cores : 1U) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_memory(
            &production_install_flow.report,
            media->ram_bytes != 0U ? media->ram_bytes : UINT64_C(1073741824)) !=
            INSTALLER_DISCOVERY_OK) {
        return false;
    }

    /* The boot coordinator probes the same list; a cached probe returns it. */
    (void)appliance_production_storage_probe(
        NULL, backends, APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY,
        &backend_count);
    disk_count = appliance_production_storage_disk_count();
    if (disk_count > INSTALLER_DISCOVERY_DISK_CAPACITY) {
        disk_count = INSTALLER_DISCOVERY_DISK_CAPACITY;
    }
    /*
     * ITEM-518: the probe's own report first - controllers matched, disks
     * brought up, and the reason each disk already carries - and the discovery's
     * verdict is folded into the same rows below, so the refusal page and the
     * `storage` command quote one model.
     */
    appliance_production_storage_report(&production_install_flow.storage_report);
    /*
     * The discovery holds four disks; a probe that brought up more reports the
     * rest as never enumerated instead of counting them as accepted.
     */
    for (index = disk_count;
         index < production_install_flow.storage_report.disk_count &&
         index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY;
         ++index) {
        if (production_install_flow.storage_report.disks[index].present) {
            production_install_flow.storage_report.disks[index].reason =
                APPLIANCE_STORAGE_REPORT_NOT_PROBED;
        }
    }
    for (index = 0U; index < disk_count; ++index) {
        struct appliance_production_storage_disk disk;
        size_t identity_length = 0U;

        if (!appliance_production_storage_disk_info(index, &disk) ||
            !disk.present || disk.device == NULL ||
            disk.identity[0] == '\0') {
            continue;
        }
        while (identity_length <
                   APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH &&
               disk.identity[identity_length] != '\0') {
            ++identity_length;
        }
        production_install_flow.devices[index] = disk.device;
        memcpy(production_install_flow.models[index], disk.model,
               sizeof(production_install_flow.models[index]));
        /*
         * ITEM-409: the scan's own status is kept, not discarded.  A disk the
         * storage layer enumerated whose partition table cannot be read is
         * exactly the case the boot menu has to name: without it the disk
         * contributes no partitions, no ESP and no chainload entry, and the
         * install used to say nothing at all about it.
         */
        production_install_flow.discovery[index] =
            installer_discovery_scan_block_device(
                &production_install_flow.report, disk.device,
                (uint32_t)(index + 1U),
                production_install_flow_controller_kind(disk.kind),
                disk.identity, identity_length);
        /*
         * ITEM-518: the same verdict in the observed row.  The probe's reason
         * wins when it already rejects the disk - the scan never saw an
         * identity or a geometry the probe could not read - and a table the
         * scan could not read stays an annotation, not a rejection: the erase
         * scheme exists for exactly that disk.
         */
        if (index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY) {
            enum appliance_storage_report_reason *reason =
                &production_install_flow.storage_report.disks[index].reason;

            *reason = production_install_flow_observed_reason(
                *reason, production_install_flow.discovery[index]);
        }
        /*
         * ITEM-495: the name the hardware reported, so the screen can show the
         * disk the operator recognises ("HFOC Example SATA Fixture") beside the
         * identity the plan writes by.  A disk whose name was not read keeps an
         * empty model, and the screen then shows the identity alone.
         */
        if (disk.model[0] != '\0') {
            size_t model_length = 0U;

            while (model_length < APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH &&
                   disk.model[model_length] != '\0') {
                ++model_length;
            }
            (void)installer_discovery_set_disk_model(
                &production_install_flow.report, (uint32_t)(index + 1U),
                disk.model, model_length);
        }
    }
    production_install_flow_observed_recount(
        &production_install_flow.storage_report);
    if (installer_discovery_finalize(&production_install_flow.report) !=
        INSTALLER_DISCOVERY_OK) {
        return false;
    }
    /*
     * ITEM-489: what the storage screen would show, published once the
     * discovery is complete.  It names every disk by the identity the hardware
     * reported and says, in the same record, that **no disk is chosen** - the
     * installer never picks one by itself, and the operator's choice is the
     * subject of the screen this record describes.
     */
    installer_storage_publish(&production_install_flow.report);
    /*
     * ITEM-518: the same observed storage in one machine record, so the
     * acceptance log carries the numbers the refusal page prints, and the
     * console command is armed even when the run ends in a refusal.
     */
    {
        char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
        const size_t length = appliance_storage_report_record(
            &production_install_flow.storage_report, record, sizeof(record));

        if (length + 1U < sizeof(record)) {
            record[length] = '\n';
            record[length + 1U] = '\0';
        }
        production_install_flow_publish(record);
        console_publish_storage_report(&production_install_flow.storage_report);
    }
    /*
     * ITEM-492: the installer's network reading comes from the general NIC
     * interface - the controllers the machine reported and whether this image
     * carries a driver for each - and never from a vendor-specific driver.  The
     * record also states the rule the item exists for: the network is not a
     * precondition of installing, so a machine with no NIC, an unsupported NIC
     * or a refused NIC installs exactly like one with a working link.
     */
    production_install_flow_publish_nic();
    return true;
}

static void production_install_flow_publish_candidates(void)
{
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t count = production_install_media_candidate_count(
        &production_install_flow.report);
    size_t index;

    for (index = 0U; index < count; ++index) {
        const struct installer_discovery_disk *disk =
            production_install_media_candidate(
                &production_install_flow.report, index);
        size_t length = 0U;
        bool selected;

        if (disk == NULL) {
            continue;
        }
        selected = production_install_flow.plan.selected &&
                   production_install_flow.plan.target.disk_id == disk->id;
        production_install_flow_publish_record(
            production_install_media_render_candidate(
                disk,
                disk->id != 0U && disk->id <= INSTALLER_DISCOVERY_DISK_CAPACITY
                    ? production_install_flow.models[disk->id - 1U]
                    : NULL,
                selected, record, sizeof(record), &length),
            record);
    }
}

/*
 * ITEM-490: the operator entry's mandatory step.
 *
 * The install flow runs before the machine has a console and before interrupts
 * are enabled, so this is not where a screen can wait for a person: the flow
 * publishes what it prepared and returns, and the boot's own loop - where local
 * input is live - asks `production_install_flow_awaiting_choice()` and calls
 * `production_install_flow_choose_disk()` with the disk the operator named.
 * That keeps one order for both entries: discovery, plan, pending record, and
 * then - only on the operator entry - the choice.
 *
 * The chosen disk becomes the declared target, so the plan that the console
 * confirmation applies is the plan for the disk the operator saw.  Everything
 * else the medium declared stays exactly as it was, and a plan that cannot be
 * rebuilt for the chosen disk refuses the run instead of installing something
 * nobody asked for.
 */
bool production_install_flow_awaiting_choice(void)
{
    return production_install_flow.prepared &&
           production_install_flow.plan.mode ==
               PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE &&
           !production_install_flow.choice_applied;
}

const char *production_install_flow_choice_note(void)
{
    return production_install_flow.choice_note;
}

const char *production_install_flow_operator_refusal(void)
{
    if (production_install_flow.mode != PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE ||
        !production_install_flow.refused) {
        return NULL;
    }
    /*
     * ITEM-501: a plan that refused after the operator had already chosen -
     * "this disk has no room for the three roles", "the table cannot be read" -
     * is painted with the controller's own status, so the person who just
     * confirmed a partition reads what stopped the install instead of a machine
     * that quietly did nothing.
     */
    if (production_install_flow.controller_refusal[0] != '\0') {
        return production_install_flow.controller_refusal;
    }
    if (production_install_flow.refusal ==
            PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE ||
        /*
         * The operator left the storage screen: that decision was made on the
         * screen the person was reading, and a second page telling him the run
         * was cancelled would be noise, not an explanation.
         */
        production_install_flow.refusal ==
            PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR) {
        return NULL;
    }
    return production_install_media_refusal_string(
        production_install_flow.refusal);
}

const struct installer_discovery_report *production_install_flow_report(void)
{
    return &production_install_flow.report;
}

void production_install_flow_storage_report(struct appliance_storage_report *out)
{
    if (out == NULL) {
        return;
    }
    *out = production_install_flow.storage_report;
}

bool production_install_flow_choose_disk(uint32_t disk_id)
{
    struct installer_storage_model model;
    const struct installer_storage_disk *disk;
    const struct production_install_flow_media *media =
        production_install_flow.media;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    if (!production_install_flow_awaiting_choice() || media == NULL) {
        return false;
    }
    /*
     * The choice goes through the storage model's own fail-closed rule, so a
     * disk whose identity or geometry was not read cannot become the target
     * even if a caller asks for it by id.
     */
    installer_storage_inventory(&model, &production_install_flow.report);
    if (!installer_storage_select_id(&model, disk_id)) {
        return false;
    }
    disk = installer_storage_selected(&model);
    if (disk == NULL ||
        !production_install_media_declaration_set_target(
            &production_install_flow.declaration, disk->identity)) {
        return false;
    }
    if (production_install_media_plan_build(
            PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE,
            &production_install_flow.declaration, media->firmware_mode,
            &production_install_flow.report,
            &production_install_flow.plan) != PRODUCTION_INSTALL_MEDIA_OK ||
        production_install_flow.plan.action !=
            PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE ||
        !production_install_flow.plan.selected) {
        production_install_flow_refuse(
            production_install_flow.plan.refusal ==
                    PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE
                ? PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE
                : production_install_flow.plan.refusal);
        return false;
    }
    production_install_flow.choice_applied = true;
    /*
     * The reading says which disk the operator chose, and the pending record is
     * published again so the confirmation the operator is about to type names
     * the disk the plan will write.
     */
    installer_storage_publish_choice(&production_install_flow.report,
                                     disk->id);
    production_install_flow_publish_record(
        production_install_media_render_pending(
            &production_install_flow.declaration,
            &production_install_flow.plan.target, record, sizeof(record),
            &length),
        record);
    return true;
}

void production_install_flow_cancel_by_operator(void)
{
    if (!production_install_flow.prepared) {
        return;
    }
    production_install_flow.prepared = false;
    production_install_flow_refuse(
        PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR);
}

/*
 * ITEM-501: the partition the operator gave up.  The screen has already
 * validated the row through its own model; this only remembers the decision, and
 * the plan the confirmation applies is the one built from it.  Nothing is
 * written here, and a caller that never calls this keeps the ordinary
 * free-space behaviour byte for byte.
 */
void production_install_flow_release_partition(uint32_t partition_index)
{
    if (partition_index == 0U) {
        production_install_flow.release_chosen = false;
        production_install_flow.release_index = 0U;
        return;
    }
    production_install_flow.release_chosen = true;
    production_install_flow.release_index = partition_index;
}

static enum installer_media_signature_status
production_install_flow_signature_verify(void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    switch (image_signature_verify(
        context, algorithm, signer_id, digest, signature,
        signature_length)) {
    case IMAGE_SIGNATURE_OK:
        return INSTALLER_MEDIA_SIGNATURE_OK;
    case IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM:
    case IMAGE_SIGNATURE_UNSUPPORTED_KEY:
        return INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM;
    default:
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
}

static enum efi_boot_variable_io_status
production_install_flow_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    (void)length;
    /* The kernel has no EFI runtime: the platform's boot variables are not
     * reachable from here, so the environment reports them unavailable
     * instead of pretending an entry was written. */
    return EFI_BOOT_VARIABLE_IO_ACCESS_DENIED;
}

static enum efi_boot_variable_io_status
production_install_flow_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_ACCESS_DENIED;
}

static const struct installer_layout_partition *
production_install_flow_role_partition(
    const struct installer_layout_plan *plan,
    enum installer_layout_role role)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_LAYOUT_PARTITION_CAPACITY; ++index) {
        if (plan->partitions[index].present &&
            plan->partitions[index].role == role) {
            return &plan->partitions[index];
        }
    }
    return NULL;
}

static void production_install_flow_publish_preview(
    const struct production_installer *installer)
{
    static struct production_install_flow_line line;

    /*
     * ITEM-405: the preview is assembled once and published through the one
     * path, so the console mirror carries the same bytes the serial stream
     * does.  The mirror paints the first line of the block; the rest of the
     * preview stays on serial.
     */
    production_install_flow_line_begin(&line);
    production_install_flow_line_append(
        &line, "[PRODUCTION-INSTALL] preview ");
    production_install_flow_line_append(&line, installer->preview);
    if (installer->preview[0] == '\0' ||
        installer->preview[strlen(installer->preview) - 1U] != '\n') {
        production_install_flow_line_append(&line, "\n");
    }
    production_install_flow_line_publish(&line);
}

/*
 * ITEM-398: which layout mode this run uses, decided and published before the
 * first write.
 *
 * The operator's declaration wins when it is present.  Without one the run
 * preserves by default as soon as the selected disk carries anything that is
 * not an EFI System Partition, or a foreign loader on its ESP, or a system the
 * discovery found: an install must never silently destroy a system, and a disk
 * with an existing system is exactly the case the destructive mode is not for.
 * A disk with no partition table at all - the acceptance disk after a declared
 * whole-disk wipe - has nothing to preserve and takes the whole-disk path.
 */
static bool production_install_flow_target_has_foreign_system(
    uint32_t disk_id)
{
    const struct installer_discovery_report *report =
        &production_install_flow.report;
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (!partition->present || partition->disk_id != disk_id) {
            continue;
        }
        if (!partition->efi_system) {
            /* A partition that is not an ESP is somebody's data. */
            return true;
        }
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        const struct installer_discovery_os *os = &report->oss[index];
        size_t partition_index;

        if (!os->present) {
            continue;
        }
        for (partition_index = 0U;
             partition_index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
             ++partition_index) {
            const struct installer_discovery_partition *partition =
                &report->partitions[partition_index];

            if (partition->present &&
                partition->id == os->partition_id &&
                partition->disk_id == disk_id) {
                return true;
            }
        }
    }
    return false;
}

/*
 * ITEM-514: the scheme the medium declared, and the scheme the disk suggests
 * when it declared none.  Both are read here, in the flow that owns the
 * declaration and the discovery, so the screen and the records cannot tell
 * different stories about what the default was.
 */
enum installer_storage_scheme production_install_flow_declared_scheme(void)
{
    if (!production_install_flow.declaration.layout_declared) {
        return INSTALLER_STORAGE_SCHEME_NONE;
    }
    return production_install_flow.declaration.layout_preserve
               ? INSTALLER_STORAGE_SCHEME_ALONGSIDE
               : INSTALLER_STORAGE_SCHEME_ERASE;
}

enum installer_storage_scheme production_install_flow_default_scheme(void)
{
    const enum installer_storage_scheme declared =
        production_install_flow_declared_scheme();

    if (declared != INSTALLER_STORAGE_SCHEME_NONE) {
        return declared;
    }
    /*
     * Without a declaration the disk decides the default: preserve a system
     * that is here, erase a disk that carries none.  This is the same rule the
     * unattended path applies, so the cursor the operator sees is the decision
     * the medium would have made.
     */
    if (production_install_flow.plan.selected &&
        production_install_flow_target_has_foreign_system(
            production_install_flow.plan.target.disk_id)) {
        return INSTALLER_STORAGE_SCHEME_ALONGSIDE;
    }
    return INSTALLER_STORAGE_SCHEME_ERASE;
}

bool production_install_flow_contents_unknown(void)
{
    const uint32_t disk_id = production_install_flow.plan.target.disk_id;

    if (!production_install_flow.plan.selected || disk_id == 0U ||
        disk_id > INSTALLER_DISCOVERY_DISK_CAPACITY) {
        return false;
    }
    /*
     * ITEM-514 (R03): the discovery's own status of this disk.  A disk whose
     * partition table could not be read is not an empty disk, and the erase
     * page says so.
     */
    return production_install_flow.discovery[disk_id - 1U] !=
           INSTALLER_DISCOVERY_OK;
}

bool production_install_flow_choose_scheme(
    enum installer_storage_scheme scheme)
{
    if (scheme == INSTALLER_STORAGE_SCHEME_NONE ||
        scheme > INSTALLER_STORAGE_SCHEME_MANUAL) {
        return false;
    }
    production_install_flow.chosen_scheme = scheme;
    /*
     * The two decisions are one decision: a scheme that is not "give up a
     * partition" withdraws a partition given up before it, so a scheme change
     * can never leave the release half of an older choice behind.
     */
    if (scheme != INSTALLER_STORAGE_SCHEME_MANUAL) {
        production_install_flow.release_chosen = false;
        production_install_flow.release_index = 0U;
    }
    {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] scheme declared=");
        production_install_flow_line_append(
            &line, installer_storage_scheme_text(
                       production_install_flow_declared_scheme()));
        production_install_flow_line_append(&line, " chosen=");
        production_install_flow_line_append(
            &line, installer_storage_scheme_text(scheme));
        production_install_flow_line_append(
            &line, " source=operator writes=0\n");
        production_install_flow_line_publish(&line);
    }
    return true;
}

static const char *production_install_flow_mode_string(
    enum production_installer_mode mode)
{
    if (production_install_flow.preserve_state) {
        return "update";
    }
    if (mode != PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
        return "whole-disk";
    }
    /* ITEM-501: giving up a partition is the preserving install that had to
     * make room for itself, and the record says which one it was. */
    return production_install_flow.release_chosen ? "release" : "preserve";
}

/*
 * The decision and its reason, in one bounded record, published before the
 * first write: the acceptance run quotes it and the operator can see which path
 * the run took without reading the plan.
 */
static bool production_install_flow_select_layout(
    enum production_installer_mode *mode)
{
    const uint32_t disk_id = production_install_flow.plan.target.disk_id;
    const bool foreign =
        production_install_flow_target_has_foreign_system(disk_id);
    const enum installer_storage_scheme chosen =
        production_install_flow.chosen_scheme;
    const char *reason;
    enum production_installer_mode selected;

    production_install_flow.preserve_state = false;
    /*
     * ITEM-514: for ordinary media the operator's scheme wins when there is
     * one.  ITEM-598 makes the explicit `update` declaration non-destructive
     * and sticky, because an account/seed-preserving medium must never turn
     * into an erase merely because the screen defaulted to that row.
     */
    if (production_install_flow.declaration.layout_update) {
        /*
         * ITEM-598: an update medium is a state-preserving contract, not a
         * destructive default.  The storage screen cannot turn it into an
         * erase: that would discard the account and seed the update promised
         * to keep.
         */
        selected = PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
        production_install_flow.preserve_state = true;
        reason = "declared-update";
    } else if (chosen != INSTALLER_STORAGE_SCHEME_NONE) {
        switch (chosen) {
        case INSTALLER_STORAGE_SCHEME_MANUAL:
            selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
            reason = "operator-release";
            break;
        case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
            selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
            reason = "operator-alongside";
            break;
        case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
            selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
            reason = "operator-free-space";
            break;
        case INSTALLER_STORAGE_SCHEME_ERASE:
        default:
            selected = PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
            reason = "operator-erase";
            break;
        }
    } else if (production_install_flow.release_chosen) {
        /*
         * ITEM-501 kept as the API path for a caller that gave up a partition
         * without choosing a scheme: the gesture itself makes the preserving
         * install non-optional.
         */
        selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
        reason = "operator-release";
    } else if (production_install_flow.declaration.layout_declared) {
        if (production_install_flow.declaration.layout_update) {
            selected = PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
            production_install_flow.preserve_state = true;
            reason = "declared-update";
        } else if (production_install_flow.declaration.layout_preserve) {
            selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
            reason = "declared-preserve";
        } else {
            selected = PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
            reason = "declared-whole-disk";
        }
    } else if (foreign) {
        selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
        reason = "existing-system";
    } else {
        selected = PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
        reason = "no-existing-system";
    }
    /*
     * An ESP carrying a loader that is not ours is a system even when the
     * discovery could not name it, so the read-only inventory decides too.
     */
    if (selected == PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK &&
        chosen == INSTALLER_STORAGE_SCHEME_NONE &&
        !production_install_flow.release_chosen &&
        !production_install_flow.declaration.layout_declared) {
        production_install_flow_inventory.version =
            INSTALLER_BOOT_MENU_VERSION;
        production_install_flow_inventory.disk_count = 0U;
        if (installer_boot_menu_scan(
                &production_install_flow.report, disk_id,
                production_install_flow_device_lookup, NULL,
                &production_install_flow_inventory) ==
            INSTALLER_BOOT_MENU_OK) {
            uint32_t index;

            for (index = 0U;
                 index < production_install_flow_inventory.disk_count;
                 ++index) {
                const struct installer_boot_menu_disk *record =
                    &production_install_flow_inventory.disks[index];

                if (record->disk_id == disk_id &&
                    record->loader_count != 0U) {
                    selected = PRODUCTION_INSTALLER_MODE_DUAL_BOOT;
                    reason = "foreign-loader";
                }
            }
        }
    }
    /*
     * ITEM-514: the conflict is judged against the mode this run will use, not
     * against the medium's declaration.  Giving up a partition and erasing the
     * whole disk contradict each other whatever the medium said; a partition
     * given up on a disk the medium declared destructive is exactly the
     * operator's right to decide otherwise.
     */
    if (production_install_flow.release_chosen &&
        selected == PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_RELEASE_CONFLICT);
        return false;
    }
    {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] layout mode=");
        production_install_flow_line_append(
            &line, production_install_flow_mode_string(selected));
        production_install_flow_line_append(&line, " reason=");
        production_install_flow_line_append(&line, reason);
        production_install_flow_line_append(&line, " declared=");
        production_install_flow_line_append(
            &line,
            production_install_flow.declaration.layout_declared ? "1"
                                                               : "0");
        /* ITEM-501: the record names the partition the operator gave up. */
        if (production_install_flow.release_chosen) {
            production_install_flow_line_append(
                &line, " released_index=");
            production_install_flow_line_u64(
                &line, production_install_flow.release_index);
        }
        /*
         * ITEM-514: the scheme's source.  `none` means no operator chose and
         * the declaration or the discovery decided; the reason above already
         * names which one.
         */
        production_install_flow_line_append(&line, " chosen=");
        production_install_flow_line_append(
            &line, installer_storage_scheme_text(
                       production_install_flow.chosen_scheme));
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    }
    *mode = selected;
    return true;
}

/*
 * ITEM-409: the menu is the installed configuration for every install.
 *
 * Until this item the generated menu was staged only when it carried a chainload
 * entry, and a machine that carried nothing but this appliance got the sealed
 * single-entry configuration instead - which is not a menu at all, and which is
 * what an operator who had to choose between two systems saw.  The owner's rule
 * is the opposite one: the installed appliance always boots through a menu, a
 * machine that carries only our system gets a menu that shows our system, and
 * the sealed configuration is the fallback only for the case where no menu could
 * be built at all - which the record then states.
 */
static bool production_install_flow_menu_available(void)
{
    return production_install_flow.menu_built &&
           production_install_flow_menu.version ==
               INSTALLER_BOOT_MENU_VERSION &&
           production_install_flow_menu.entry_count != 0U &&
           production_install_flow_menu.text[0] != '\0';
}

static bool production_install_flow_menu_has_chainload_entry(void)
{
    uint32_t index;

    for (index = 0U;
         index < production_install_flow_menu.entry_count; ++index) {
        if (production_install_flow_menu.entries[index].present &&
            production_install_flow_menu.entries[index].protocol ==
                INSTALLER_BOOT_MENU_PROTOCOL_EFI) {
            return true;
        }
    }
    return false;
}

static const char *production_install_flow_table_string(
    enum installer_partition_table_kind table)
{
    switch (table) {
    case INSTALLER_PARTITION_TABLE_MBR:
        return "MBR";
    case INSTALLER_PARTITION_TABLE_GPT:
        return "GPT";
    case INSTALLER_PARTITION_TABLE_NONE:
    default:
        return "NONE";
    }
}

/*
 * ITEM-409: how many disks *besides* the one this run owns carry an ESP the
 * read-only scan could not read.  The menu's own `unreadable_disk_count` counts
 * the target too, and a whole-disk install drops the target's device on purpose
 * - reporting that as an unreadable disk would be a false alarm about the one
 * disk the run is about to erase.
 */
static uint32_t production_install_flow_unreadable_other_disks(
    uint32_t target_disk_id)
{
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U;
         index < production_install_flow_inventory.disk_count; ++index) {
        const struct installer_boot_menu_disk *disk =
            &production_install_flow_inventory.disks[index];

        if (disk->disk_id == target_disk_id) {
            continue;
        }
        if (disk->esp_present &&
            disk->scan != INSTALLER_BOOT_MENU_SCAN_OK) {
            ++count;
        }
    }
    return count;
}

/*
 * ITEM-409: which stage left the menu without a system to offer besides ours.
 *
 * The answer is read off the records the run already has - the discovery's
 * per-disk status and the read-only menu scan's per-disk status - and it names
 * the earliest stage of the chain that decided, so an operator reads where it
 * broke instead of a menu that silently offers one entry:
 *
 *   discovery-read    the disk's partition table could not be read at all;
 *   partition-table   the disk was read and carries no EFI System Partition;
 *   esp-read          the ESP is there and could not be read (or is not FAT32);
 *   loader-inventory  the ESP was read and holds no loader we recognise;
 *   device-lookup     the scan had no block device for that disk;
 *   entry-capacity    a loader was found and the menu could not hold it;
 *   target-erased     the only system on the machine is on the disk this run
 *                     erases (a whole-disk install drops it on purpose);
 *   no-other-disk     the machine has no disk besides the one being installed.
 */
static const char *production_install_flow_menu_stage(
    uint32_t target_disk_id,
    bool chainload_entry)
{
    bool saw_other_disk = false;
    bool target_has_esp = false;
    uint32_t index;

    if (!production_install_flow_menu_available()) {
        /* The menu never materialised: that is the stage that decided. */
        return "menu-unavailable";
    }
    if (chainload_entry) {
        return "chainload-entry";
    }
    for (index = 0U;
         index < production_install_flow_inventory.disk_count; ++index) {
        const struct installer_boot_menu_disk *disk =
            &production_install_flow_inventory.disks[index];
        enum installer_discovery_status discovery =
            INSTALLER_DISCOVERY_OK;

        if (disk->disk_id != 0U &&
            disk->disk_id <= INSTALLER_DISCOVERY_DISK_CAPACITY) {
            discovery =
                production_install_flow.discovery[disk->disk_id - 1U];
        }
        if (disk->disk_id == target_disk_id) {
            target_has_esp = disk->esp_present;
            continue;
        }
        saw_other_disk = true;
        if (discovery != INSTALLER_DISCOVERY_OK) {
            /* The disk is there and its partition table never read. */
            return "discovery-read";
        }
        switch (disk->scan) {
        case INSTALLER_BOOT_MENU_SCAN_UNREADABLE:
        case INSTALLER_BOOT_MENU_SCAN_CORRUPT:
        case INSTALLER_BOOT_MENU_SCAN_UNSUPPORTED:
            return "esp-read";
        case INSTALLER_BOOT_MENU_SCAN_NO_LOADER:
            return "loader-inventory";
        case INSTALLER_BOOT_MENU_SCAN_NO_DEVICE:
            return "device-lookup";
        case INSTALLER_BOOT_MENU_SCAN_OK:
            /* Read, with loaders, and none of them claimed: capacity. */
            return "entry-capacity";
        case INSTALLER_BOOT_MENU_SCAN_NO_ESP:
        default:
            if (disk->esp_present) {
                return "esp-read";
            }
            break;
        }
    }
    if (saw_other_disk) {
        /* Every other disk was read and carries no ESP of anybody's. */
        return "partition-table";
    }
    if (target_has_esp) {
        /*
         * The only system on the machine is on the target disk, and this run
         * is a whole-disk install: offering what it erases would offer exactly
         * what it destroys.
         */
        return "target-erased";
    }
    return "no-other-disk";
}

/*
 * ITEM-409: one bounded line, before the first write, that states what the
 * installed configuration will be and - when it offers nothing but this
 * appliance - which disk and which stage left it that way.  The ITEM-405
 * console mirror paints it, so the operator reads the answer on the machine
 * instead of only in a serial capture the acceptance host does not have.
 */
static void production_install_flow_publish_boot_menu(uint32_t target_disk_id)
{
    static struct production_install_flow_line line;
    const bool chainload = production_install_flow_menu_has_chainload_entry();
    uint32_t index;

    production_install_flow_line_begin(&line);
    production_install_flow_line_append(
        &line, "[PRODUCTION-INSTALL] boot-menu state=");
    production_install_flow_line_append(
        &line, production_install_flow_menu_available() ? "GENERATED"
                                                       : "SEALED");
    production_install_flow_line_append(&line, " reason=");
    if (!production_install_flow_menu_available()) {
        production_install_flow_line_append(
            &line, production_install_flow.menu_reason != NULL
                       ? production_install_flow.menu_reason
                       : "menu-unavailable");
    } else if (chainload) {
        production_install_flow_line_append(&line, "chainload-entry");
    } else {
        production_install_flow_line_append(&line, "appliance-only");
    }
    production_install_flow_line_append(&line, " stage=");
    production_install_flow_line_append(
        &line, production_install_flow_menu_stage(target_disk_id, chainload));
    production_install_flow_line_append(&line, " entries=");
    production_install_flow_line_u64(
        &line, (uint64_t)production_install_flow_menu.entry_count);
    production_install_flow_line_append(&line, " loaders=");
    production_install_flow_line_u64(
        &line, (uint64_t)production_install_flow_menu.loader_count);
    production_install_flow_line_append(&line, " missing=");
    production_install_flow_line_u64(
        &line, (uint64_t)production_install_flow_menu.missing_count);
    production_install_flow_line_append(&line, " unreadable=");
    production_install_flow_line_u64(
        &line, (uint64_t)production_install_flow_unreadable_other_disks(
                   target_disk_id));
    production_install_flow_line_append(&line, " disks=");
    production_install_flow_line_u64(
        &line, (uint64_t)production_install_flow_inventory.disk_count);
    production_install_flow_line_append(&line, " target=");
    production_install_flow_line_u64(&line, (uint64_t)target_disk_id);
    /*
     * ITEM-423: what the storage probe saw, not only what it kept.  A menu that
     * offers one system on a machine that carries two has three different
     * causes - the controller was never matched, it was matched and stopped
     * inside the NVMe bring-up, or it was brought up and produced no namespace
     * - and this is what tells them apart on a machine with no serial capture.
     */
    {
        struct appliance_production_storage_evidence evidence;

        appliance_production_storage_evidence(&evidence);
        production_install_flow_line_append(&line, " pci_ahci=");
        production_install_flow_line_u64(&line, (uint64_t)evidence.pci_ahci);
        production_install_flow_line_append(&line, " pci_nvme=");
        production_install_flow_line_u64(&line, (uint64_t)evidence.pci_nvme);
        production_install_flow_line_append(&line, " ahci=");
        production_install_flow_line_u64(&line, (uint64_t)evidence.ahci);
        production_install_flow_line_append(&line, " nvme=");
        production_install_flow_line_u64(&line, (uint64_t)evidence.nvme);
        production_install_flow_line_append(&line, " nvme_status=");
        production_install_flow_line_append(
            &line, nvme_status_string((enum nvme_status)evidence.nvme_status));
        /*
         * ITEM-429: the step and the wait that go with the status.  A
         * `NVME_TIMEOUT` is two findings - a controller that never reported
         * ready, and a driver that never waited long enough to know - and the
         * hardware run of 2026-09-16 could not tell them apart.
         */
        production_install_flow_line_append(&line, " nvme_step=");
        production_install_flow_line_append(
            &line, evidence.nvme_step != NULL ? evidence.nvme_step : "NONE");
        production_install_flow_line_append(&line, " nvme_wait_ms=");
        production_install_flow_line_u64(
            &line, (uint64_t)evidence.nvme_wait_ms);
    }
    /*
     * Per disk: the table the discovery found, whether an ESP was classified,
     * what the read-only scan made of it and how many loaders it holds - the
     * four answers the chain from the report to the menu is made of.
     */
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        const struct installer_discovery_disk *disk =
            &production_install_flow.report.disks[index];
        const struct installer_boot_menu_disk *scanned = NULL;
        uint32_t scan_index;

        if (!disk->present || disk->id == 0U) {
            continue;
        }
        for (scan_index = 0U;
             scan_index < production_install_flow_inventory.disk_count;
             ++scan_index) {
            if (production_install_flow_inventory.disks[scan_index].disk_id ==
                disk->id) {
                scanned =
                    &production_install_flow_inventory.disks[scan_index];
                break;
            }
        }
        production_install_flow_line_append(&line, " d");
        production_install_flow_line_u64(&line, (uint64_t)disk->id);
        production_install_flow_line_append(&line, "=");
        production_install_flow_line_append(
            &line, production_install_flow_table_string(disk->partition_table));
        production_install_flow_line_append(
            &line, disk->has_efi_system_partition ? "/esp1/" : "/esp0/");
        production_install_flow_line_append(
            &line,
            scanned != NULL
                ? installer_boot_menu_scan_status_string(scanned->scan)
                : "NOT_SCANNED");
        production_install_flow_line_append(&line, "/");
        production_install_flow_line_u64(
            &line, scanned != NULL ? (uint64_t)scanned->loader_count : 0U);
        if (disk->id <= INSTALLER_DISCOVERY_DISK_CAPACITY &&
            production_install_flow.discovery[disk->id - 1U] !=
                INSTALLER_DISCOVERY_OK) {
            /*
             * The discovery's own status, when it is not OK: this is the disk
             * whose sectors did not read, and it is the reason its partition
             * table is NONE above.
             */
            production_install_flow_line_append(
                &line, ",discovery=");
            production_install_flow_line_append(
                &line,
                installer_discovery_status_string(
                    production_install_flow.discovery[disk->id - 1U]) != NULL
                    ? installer_discovery_status_string(
                          production_install_flow.discovery[disk->id - 1U])
                    : "ERROR");
        }
    }
    production_install_flow_line_append(&line, "\n");
    production_install_flow_line_publish(&line);
}

/*
 * ITEM-517: the legacy-BIOS install publishes the menu it generated in the
 * same record vocabulary the preserving install uses - the
 * `[INSTALLER-BOOT-MENU] menu ...` record, its per-disk and per-entry lines
 * and the configuration between `config-begin` and `config-end`.  Without it
 * the one install that has no ESP would be the one install whose menu the
 * acceptance log cannot quote, and the record is also what proves the BIOS
 * menu was built with `firmware=bios-legacy` rather than an EFI one.  Only a
 * BIOS run calls this: every UEFI install keeps its exact records and bytes.
 */
static bool production_install_flow_publish_bios_menu(void)
{
    char menu_record[INSTALLER_BOOT_MENU_RECORD_LENGTH];
    size_t menu_length = 0U;
    uint64_t serial_flags;

    if (installer_boot_menu_render_record(
            &production_install_flow_menu,
            &production_install_flow_inventory, menu_record,
            sizeof(menu_record), &menu_length) != INSTALLER_BOOT_MENU_OK ||
        menu_length == 0U) {
        return false;
    }
    /*
     * One serial sequence, so neither the record nor the configuration it
     * brackets can be split by an interrupt-context print - the same rule the
     * preserving install's menu publication follows.
     */
    serial_flags = serial_sequence_begin();
    serial_write(menu_record);
    serial_write("[INSTALLER-BOOT-MENU] config-begin\n");
    serial_write(production_install_flow_menu.text);
    serial_write("[INSTALLER-BOOT-MENU] config-end\n");
    serial_sequence_end(serial_flags);
    return true;
}

static bool production_install_flow_provision_host_seed(
    struct appliance_state_loader *loader,
    bool preserve_state)
{
    const struct appliance_state_layout *layout;
    struct block_device *partition_device;
    uint8_t unlocked_seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    enum appliance_host_seed_status status;
    struct production_install_flow_line line;

    if (!production_install_flow.declaration.host_seed_required) {
        return true;
    }
    if (!production_install_flow.host_seed_record_valid ||
        production_install_flow.operator_password_length == 0U ||
        (preserve_state &&
         !production_install_flow.existing_credential_verified) ||
        (!preserve_state &&
         !production_install_flow.operator_credential_set) ||
        loader == NULL) {
        production_install_flow_refuse_controller(
            production_install_media_refusal_string(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_CREDENTIAL_REQUIRED),
            NULL);
        return false;
    }
    layout = appliance_state_loader_layout(loader);
    partition_device =
        (struct block_device *)appliance_state_loader_partition_device(loader);
    if (layout == NULL || partition_device == NULL) {
        production_install_flow_failed(
            "host-seed", "PERSISTENCE_UNAVAILABLE");
        return false;
    }
    memset(unlocked_seed, 0, sizeof(unlocked_seed));
    status = APPLIANCE_HOST_SEED_MISSING;
    if (preserve_state) {
        status = appliance_host_seed_store_unlock(
            partition_device, layout->block_capacity,
            production_install_flow.operator_password,
            production_install_flow.operator_password_length,
            unlocked_seed, &epoch);
    }
    if (status == APPLIANCE_HOST_SEED_OK) {
        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] host-seed=PRESERVED epoch=");
        production_install_flow_line_u64(&line, epoch);
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    } else {
        if (preserve_state && status != APPLIANCE_HOST_SEED_MISSING) {
            memset(unlocked_seed, 0, sizeof(unlocked_seed));
            production_install_flow_failed(
                "host-seed", appliance_host_seed_status_string(status));
            return false;
        }
        status = appliance_host_seed_store_provision(
            partition_device, layout->block_capacity,
            &production_install_flow.host_seed_record,
            production_install_flow.operator_password,
            production_install_flow.operator_password_length);
        if (status != APPLIANCE_HOST_SEED_OK) {
            memset(unlocked_seed, 0, sizeof(unlocked_seed));
            production_install_flow_failed(
                "host-seed", appliance_host_seed_status_string(status));
            return false;
        }
        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] host-seed=PROVISIONED epoch=1\n");
        production_install_flow_line_publish(&line);
    }
    memset(unlocked_seed, 0, sizeof(unlocked_seed));
    production_install_flow_clear_host_seed();
    return true;
}

static bool production_install_flow_apply(
    enum production_install_media_confirmation confirmation)
{
    struct production_installer installer;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct production_installer_source source;
    struct production_installer_boot_media boot_media;
    struct production_installer_boot_outcome boot_outcome;
    struct production_installer_boot_esp boot_esp;
    /*
     * ITEM-517: the legacy-BIOS path has no ESP at all.  Its payload set goes
     * to the FAT boot volume and its stage1/stage2 go to LBA0 and the BIOS boot
     * partition, so the two roles and the BIOS outcome are selected by the
     * firmware mode the discovery measured.
     */
    struct production_installer_bios_boot_outcome bios_outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_media_verifier verifier;
    struct image_signature_store store;
    struct verification_policy policy;
    struct appliance_state_layout state_layout;
    struct appliance_state_loader loader;
    struct production_installer_state_preserve_evidence preserve_evidence;
    struct efi_boot_environment environment;
    const struct production_install_flow_media *media =
        production_install_flow.media;
    const struct installer_layout_partition *esp;
    struct block_device *device;
    enum production_installer_status status;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;
    enum production_installer_mode mode =
        PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
    uint32_t disk_id = production_install_flow.plan.target.disk_id;
    /*
     * ITEM-517: which boot chain this run installs.  The mode was detected at
     * boot (Limine's firmware type) and refused already if it was UNKNOWN; a
     * legacy-BIOS machine takes the MBR stage path and never constructs an EFI
     * environment, a UEFI machine keeps its ESP path byte for byte.
     */
    const bool bios_mode =
        media != NULL &&
        media->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY;

    if (media == NULL || disk_id == 0U ||
        disk_id > INSTALLER_DISCOVERY_DISK_CAPACITY) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_UNSAFE_EVIDENCE),
            NULL);
        return false;
    }
    device = production_install_flow.devices[disk_id - 1U];
    if (device == NULL) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_UNSAFE_EVIDENCE),
            NULL);
        return false;
    }
    /*
     * ITEM-399: an install run stages the *installed* kernel the medium
     * carries as a module - the product appliance - never the installer kernel
     * that is running.  A medium without it (a hand-built one, or one built
     * before this item) has nothing to stage, so the run refuses with zero
     * writes before the writer is reached.  The check lives here, in the apply
     * path both confirmation sources use, so it can never shadow an earlier
     * refusal of the medium itself (a medium with no declaration still refuses
     * with TOKEN_MISSING).
     */
    if (media->kernel == NULL || media->kernel_size == 0U) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE),
            "INSTALLED_KERNEL_MISSING");
        return false;
    }
    /*
     * ITEM-623: the installed system's root filesystem travels with the
     * medium as the same validated initramfs the installer booted from.  A
     * medium without it would install a kernel whose VFS is an empty RAM
     * root, so the run refuses with zero writes before the writer is reached.
     */
    if (media->rootfs == NULL || media->rootfs_size == 0U) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE),
            "INSTALLED_ROOTFS_MISSING");
        return false;
    }

    memset(&boot_esp, 0, sizeof(boot_esp));
    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = media->boot_payload;
    source.boot_payload_size = media->boot_payload_size;
    source.limine_config = media->limine_config;
    source.limine_config_size = media->limine_config_size;
    source.kernel = media->kernel;
    source.kernel_size = media->kernel_size;

    /*
     * ITEM-397: the set the installed ESP must carry.  The first three
     * components are the source the sealed media manifest binds, in its
     * declared order; the loader is the medium's own Limine binary on the
     * removable-media path, and the manifest is the sealed boot chain the
     * installed configuration declares.
     */
    memset(&boot_media, 0, sizeof(boot_media));
    boot_media.version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    boot_media.payload = source.boot_payload;
    boot_media.payload_size = source.boot_payload_size;
    boot_media.config = source.limine_config;
    boot_media.config_size = source.limine_config_size;
    boot_media.kernel = source.kernel;
    boot_media.kernel_size = source.kernel_size;
    boot_media.loader = media->loader;
    boot_media.loader_size = media->loader_size;
    boot_media.manifest = media->manifest;
    boot_media.manifest_size = media->manifest_size;
    boot_media.rootfs = media->rootfs;
    boot_media.rootfs_size = media->rootfs_size;

    /*
     * The confirmation record is published before the first write, in the
     * single-line marker style: who or what confirmed, which entry, which
     * token, which disk and its identity, and when in this boot.
     */
    production_install_flow_publish_record(
        production_install_media_render_confirmation(
            &production_install_flow.declaration,
            production_install_flow.plan.mode,
            &production_install_flow.plan.target,
            confirmation, timer_uptime_seconds(), record,
            sizeof(record), &length),
        record);

    /*
     * The measured evidence is published before the controller sees it: the
     * source payload sizes, the sealed envelope and the payload blob the
     * signed manifest binds.  The acceptance record quotes this line.
     */
    {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] source boot_payload=");
        production_install_flow_line_u64(
            &line, (uint64_t)source.boot_payload_size);
        production_install_flow_line_append(&line, " limine_config=");
        production_install_flow_line_u64(
            &line, (uint64_t)source.limine_config_size);
        production_install_flow_line_append(&line, " kernel=");
        production_install_flow_line_u64(
            &line, (uint64_t)source.kernel_size);
        production_install_flow_line_append(&line, " envelope=");
        production_install_flow_line_u64(
            &line, (uint64_t)media->envelope_size);
        production_install_flow_line_append(&line, " payload_blob=");
        production_install_flow_line_u64(
            &line, (uint64_t)media->payload_blob_size);
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    }

    if (!production_install_flow_select_layout(&mode)) {
        return false;
    }
    status = production_installer_init(&installer);
    if (status == PRODUCTION_INSTALLER_OK) {
        /*
         * ITEM-517: `efi_ready` is the controller's "the boot-chain source for
         * this firmware mode is present" flag.  Both boot chains have one - the
         * UEFI loader on the medium, the pinned Limine BIOS blob in the
         * installer kernel - so only an UNKNOWN firmware (which the media plan
         * refuses before this point) fails it.  A UEFI run passes true exactly
         * as it did.
         */
        status = production_installer_prepare(
            &installer, &production_install_flow.report, mode, disk_id,
            production_install_flow.plan.target.identity,
            strlen(production_install_flow.plan.target.identity),
            &source, media->firmware_mode != INSTALLER_FIRMWARE_UNKNOWN);
    }
    if (status != PRODUCTION_INSTALLER_OK) {
        production_install_flow_refuse_controller(
            production_installer_status_string(status) != NULL
                ? production_installer_status_string(status)
                : "PRODUCTION_INSTALLER_BAD_STATE",
            NULL);
        return false;
    }
    /*
     * ITEM-501: the operator gave up a partition, so the preserving layout is
     * planned from that partition's extent.  The controller plans it in the
     * same call as before; only the contract entry point differs, and it
     * refuses a partition that is not on this disk, is the shared ESP, or is
     * too small for the three roles - before the first write.
     */
    if (mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT &&
        production_install_flow.release_chosen) {
        installer.release_partition = true;
        installer.release_partition_index =
            production_install_flow.release_index;
    }

    /*
     * ITEM-371 stays exactly as strong as it was: the media is accepted only
     * when its sealed manifest verifies under the declared trust anchors, the
     * signed media digest matches the payload blob the medium carries, and the
     * source payloads match the signed source digest.  The verifier is the
     * same asymmetric boundary the signed boot chain uses.
     */
    memset(&store, 0, sizeof(store));
    memset(&policy, 0, sizeof(policy));
    if (image_signature_store_declared(&store, false) !=
            IMAGE_SIGNATURE_OK ||
        image_signature_policy_init(
            &store, &policy,
            PRODUCTION_INSTALL_FLOW_POLICY_GENERATION) !=
            IMAGE_SIGNATURE_OK) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_MEDIA_UNVERIFIED),
            "POLICY_UNAVAILABLE");
        return false;
    }
    memset(&verifier, 0, sizeof(verifier));
    verifier.context = &store;
    verifier.verify = production_install_flow_signature_verify;
    status = production_installer_verify_media(
        &installer, &policy, &verifier,
        PRODUCTION_INSTALL_FLOW_RELEASE_ID, media->envelope,
        media->envelope_size, media->payload_blob,
        media->payload_blob_size);
    if (status != PRODUCTION_INSTALLER_OK) {
        production_install_flow_refuse_controller(
            production_installer_status_string(status),
            installer_media_verification_status_string(
                installer.media_status));
        return false;
    }
    if (production_install_flow.declaration.host_seed_required) {
        enum appliance_host_seed_status host_seed_status;

        if (!production_install_flow.host_seed_record_valid) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_MISSING);
            return false;
        }
        host_seed_status = appliance_host_seed_media_match(
            &production_install_flow.host_seed_record,
            production_install_flow.declaration.target,
            installer.media_release_id);
        if (host_seed_status == APPLIANCE_HOST_SEED_BAD_TARGET ||
            (host_seed_status == APPLIANCE_HOST_SEED_OK &&
             strcmp(
                 production_install_flow.host_seed_record.target,
                 production_install_flow.plan.target.identity) != 0)) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_TARGET_MISMATCH);
            return false;
        }
        if (host_seed_status == APPLIANCE_HOST_SEED_BAD_BUILD) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_BUILD_MISMATCH);
            return false;
        }
        if (host_seed_status != APPLIANCE_HOST_SEED_OK) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID);
            return false;
        }
        if (production_install_flow.operator_password_length == 0U ||
            (production_install_flow.preserve_state &&
             !production_install_flow.existing_credential_verified) ||
            (!production_install_flow.preserve_state &&
             !production_install_flow.operator_credential_set)) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_CREDENTIAL_REQUIRED);
            return false;
        }
    }
    {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] media sealed=VERIFIED release=");
        production_install_flow_line_u64(
            &line, PRODUCTION_INSTALL_FLOW_RELEASE_ID);
        production_install_flow_line_append(&line, " signer=");
        production_install_flow_line_u64(
            &line, installer.media_signer_id);
        production_install_flow_line_append(&line, " payload_bytes=");
        production_install_flow_line_u64(
            &line, (uint64_t)media->payload_blob_size);
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    }

    memset(&hardware_policy, 0, sizeof(hardware_policy));
    if (installer_supported_hardware_policy_default(&hardware_policy) !=
            INSTALLER_SUPPORTED_HARDWARE_OK ||
        production_installer_verify_hardware(
            &installer, &production_install_flow.report,
            &hardware_policy) != PRODUCTION_INSTALLER_OK) {
        /* The policy's own reason is published, so an operator reads which
         * resource the machine is short of instead of a bare refusal. */
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE),
            installer_supported_hardware_reason_string(
                installer.hardware_reason));
        return false;
    }
    {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] machine cpu_cores=");
        production_install_flow_line_u64(
            &line, (uint64_t)production_install_flow.report.cpu_cores);
        production_install_flow_line_append(&line, " ram_bytes=");
        production_install_flow_line_u64(
            &line, production_install_flow.report.ram_bytes);
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    }
    status = production_installer_build_plan(&installer);
    if (status != PRODUCTION_INSTALLER_OK) {
        /*
         * The controller's own status is published, not a generic one: the
         * operator reads which precondition the run is missing.
         */
        production_install_flow_refuse_controller(
            production_installer_status_string(status) != NULL
                ? production_installer_status_string(status)
                : "PRODUCTION_INSTALLER_BAD_STATE",
            NULL);
        return false;
    }

    memset(&plan, 0, sizeof(plan));
    memset(&production_install_flow_dual, 0,
           sizeof(production_install_flow_dual));
    memset(&production_install_flow_reuse, 0,
           sizeof(production_install_flow_reuse));
    /*
     * ITEM-396 in the product flow: a preserving install plans against the
     * dual-boot contract (the ESP is the previous system's and is reused, so the
     * plan carries the three roles we add) instead of the clean whole-disk one.
     */
    status = production_installer_plan_layout(
        &installer, &production_install_flow.report, &plan,
        mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT
            ? &production_install_flow_dual
            : NULL);
    if (status != PRODUCTION_INSTALLER_OK) {
        /* A plan that does not fit is refused here, before any write. */
        const char *status_text = production_installer_status_string(status);

        production_install_flow_refuse_controller(status_text, NULL);
        /*
         * ITEM-501: on the operator entry this refusal happened while the
         * person was standing at the machine, so it is painted with the
         * controller's own status - the same token the record carries - rather
         * than leaving a machine that did nothing visible.
         */
        if (production_install_flow.mode ==
            PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE) {
            production_install_flow.refused = true;
            production_install_flow_remember_controller_refusal(status_text);
        }
        return false;
    }
    /*
     * ITEM-398: the boot menu is planned whenever anything on the machine has
     * to stay selectable - the systems already on the target disk, or a system
     * that lives on another disk (owner Case B) - and in preserve mode always.
     * The appliance entry carries the same module paths the sealed installed
     * configuration declares, because those are what make the installed
     * system's own boot chain verifiable.
     *
     * It is planned here, while the layout plan is still the state the
     * controller holds: a menu plan is accepted only in the state the layout
     * planning left behind (`PLAN_BUILT`), and publishing the preview below
     * advances that state.  This is the order the ITEM-396 controller proof
     * drives, and it is also what lets the preview carry the preservation
     * summary this planning fills in.
     */
    {
        static const char *const appliance_modules[] = {
            "boot():/boot/hfoc-boot.man",
            "boot():/boot/limine/limine.conf",
            "boot():/boot/hfoc-rootfs.arc"
        };
        struct installer_boot_menu_entry_source appliance;
        struct installer_boot_menu_entry_source recovery;

        memset(&appliance, 0, sizeof(appliance));
        memset(&recovery, 0, sizeof(recovery));
        appliance.label = "HFOC OS";
        appliance.path = "boot():/boot/hfoc-kernel.elf";
        appliance.module_paths[0U] = appliance_modules[0U];
        appliance.module_paths[1U] = appliance_modules[1U];
        appliance.module_paths[2U] = appliance_modules[2U];
        /*
         * ITEM-517: the legacy-BIOS menu is the file Limine reads
         * (`\boot\limine\limine.conf`), and the sealed configuration it names
         * as a module is a second file (`\boot\hfoc\limine.conf`) staged
         * beside it.  Naming the menu itself would hand the kernel the
         * generated bytes as the boot-chain configuration component, and the
         * signed manifest does not seal those.  On UEFI the two roles are
         * already separate files (`\EFI\BOOT\limine.conf` read by the
         * firmware, `\boot\limine\limine.conf` the module), so every UEFI
         * entry keeps the exact paths it had.
         */
        if (bios_mode) {
            appliance.module_paths[1U] =
                PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_REFERENCE;
        }
        appliance.module_count = 3U;
        /*
         * ITEM-399: the entry carries the management scope the operator
         * declared on the medium, spelled out either way.  The kernel reads the
         * scope from the boot entry's command line, so a generated menu without
         * it would make the installed appliance fall back to the default
         * instead of the scope the operator asked for.
         *
         * ITEM-446: the same entry carries the network the operator declared.
         * The sealed configuration states it directly; a generated menu is the
         * other path that can reach the installed ESP, and an entry without it
         * would make the appliance discover an address the operator had
         * already chosen.
         */
        {
            size_t cmdline_length = 0U;

            memset(production_install_flow.cmdline, 0,
                   sizeof(production_install_flow.cmdline));
            if (production_install_media_installed_cmdline(
                    &production_install_flow.declaration,
                    production_install_flow.cmdline,
                    sizeof(production_install_flow.cmdline),
                    &cmdline_length) != PRODUCTION_INSTALL_MEDIA_OK ||
                cmdline_length == 0U) {
                /*
                 * The plan already refused an unhonourable declaration, so a
                 * failure here is a bounded-buffer defect, not operator
                 * input: the run stops instead of installing a menu that lost
                 * the network.
                 */
                production_install_flow.menu_reason = "CMDLINE_UNAVAILABLE";
                production_install_flow_refuse_controller(
                    "CMDLINE_UNAVAILABLE", "MENU_UNAVAILABLE");
                return false;
            }
            appliance.cmdline = production_install_flow.cmdline;
        }
        /*
         * No recovery image is staged by this flow, so no recovery entry is
         * offered: an entry pointing at a file that is not there would not
         * boot, and the menu does not invent paths.
         */
        recovery.label = "HFOC OS (recovery)";
        recovery.path = NULL;
        recovery.module_count = 0U;
        /*
         * ITEM-398: the menu is planned in both modes.  A preserving install
         * offers the systems that are on the target disk; a whole-disk install
         * erases that disk, so the plan offers the systems that live on *other*
         * disks (owner Case B: Debian on the machine's other disk, chainloaded
         * through its GPT partition GUID) and never the ones it is about to
         * destroy.
         *
         * ITEM-409: the menu this call builds is the installed configuration,
         * whether or not it found a system to offer (see below), so what it
         * found - and what it could not - is published as one record before the
         * first write.
         */
        status = production_installer_plan_preserve(
            &installer, &production_install_flow.report,
            production_install_flow_device_lookup, NULL, &appliance,
            &recovery, &production_install_flow_menu,
            &production_install_flow_inventory);
        if (status != PRODUCTION_INSTALLER_OK) {
            /*
             * A menu that cannot be built at all is the one case where the
             * sealed configuration is the installed one, and the run says so:
             * the controller's status is the reason the record names.
             */
            production_install_flow.menu_reason =
                production_installer_status_string(status) != NULL
                    ? production_installer_status_string(status)
                    : "MENU_UNAVAILABLE";
            production_install_flow_refuse_controller(
                production_install_flow.menu_reason, "MENU_UNAVAILABLE");
            return false;
        }
        production_install_flow.menu_built = true;
    }
    /*
     * ITEM-409: the generated menu is the installed configuration, always.
     *
     * Until this item a menu without a chainload entry was replaced by the
     * sealed single-entry configuration, so a machine that carries nothing but
     * this appliance booted with no menu at all - and the operator who had to
     * choose between two systems saw a system he could not choose.  The owner's
     * rule is that the installed appliance always boots through a menu: the
     * appliance entry (with its module paths and its declared management scope)
     * plus one chainload entry per system the discovery found, and the sealed
     * configuration only when no menu could be built at all - which the record
     * below states, on the console as well as on serial.
     */
    production_install_flow.menu_reason = "menu-unavailable";
    if (production_install_flow_menu_available()) {
        production_install_flow.menu_reason = NULL;
        boot_media.menu =
            (const unsigned char *)production_install_flow_menu.text;
        boot_media.menu_size = strlen(production_install_flow_menu.text);
        boot_media.menu_entries = production_install_flow_menu.entry_count;
    }
    boot_media.menu_timeout_seconds = INSTALLER_BOOT_MENU_TIMEOUT_SECONDS;
    production_install_flow_publish_boot_menu(disk_id);
    /*
     * ITEM-517: a legacy-BIOS install publishes the menu record the preserving
     * UEFI install publishes, before the first write and before the
     * confirmation, so the acceptance log names the menu and the
     * `firmware=bios-legacy` decision that produced it.  The renderer is the
     * same one; only a BIOS run calls it.
     */
    if (bios_mode && production_install_flow_menu_available() &&
        !production_install_flow_publish_bios_menu()) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_VERIFICATION_FAILED),
            "MENU_RECORD_UNAVAILABLE");
        return false;
    }
    if (mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
        /*
         * The ESP of the system that is already there, and the table on the
         * disk the preserving writer will check its plan against.  Both are
         * read before the confirmation; the record below states what is
         * preserved, what is added and the menu.
         */
        status = production_installer_preserve_reuse(
            &installer, &production_install_flow_reuse);
        if (status != PRODUCTION_INSTALLER_OK) {
            production_install_flow_refuse_controller(
                production_installer_status_string(status), NULL);
            return false;
        }
        memset(&production_install_flow_gpt, 0,
               sizeof(production_install_flow_gpt));
        status = installer_gpt_preserve_read(
            device, disk_id, &production_install_flow_gpt) ==
                     INSTALLER_GPT_PRESERVE_OK
                 ? PRODUCTION_INSTALLER_OK
                 : PRODUCTION_INSTALLER_TABLE_CORRUPT;
        if (status != PRODUCTION_INSTALLER_OK) {
            production_install_flow_refuse_controller(
                production_installer_status_string(status), NULL);
            return false;
        }
    }
    /*
     * The preview is published once the layout, the menu and the preservation
     * are planned, so the operator confirms the contracted footprint together
     * with what is kept, what is added and what the menu offers.
     */
    if (production_installer_publish_preview(&installer) !=
        PRODUCTION_INSTALLER_OK) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_VERIFICATION_FAILED),
            NULL);
        return false;
    }
    if (mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
        /*
         * The preservation record, published before the confirmation and the
         * first write.  The controller publishes it against the preview it
         * belongs to, which is the state the call above left behind.
         */
        status = production_installer_publish_preserve_record(
            &installer, &plan, &production_install_flow_reuse,
            &production_install_flow_gpt, &production_install_flow_menu,
            &production_install_flow_inventory);
        if (status != PRODUCTION_INSTALLER_OK) {
            production_install_flow_refuse_controller(
                production_installer_status_string(status), NULL);
            return false;
        }
    }
    production_install_flow_publish_preview(&installer);

    if (production_installer_confirm(&installer) !=
            PRODUCTION_INSTALLER_OK ||
        installer_layout_confirm(&plan) != INSTALLER_LAYOUT_OK) {
        production_install_flow_refuse_controller(
            production_installer_status_string(
                PRODUCTION_INSTALLER_BAD_STATE),
            NULL);
        return false;
    }

    /*
     * ITEM-398: which ESP the installed boot set goes to, decided before the
     * pre-write gate so the gate judges the volume the stage will actually use.
     * A preserving install reuses the ESP of the system that is already there -
     * it is not one of the three roles the plan adds, so it comes from the
     * reservation - and a whole-disk install owns the role its own plan
     * declares.
     *
     * ITEM-517: a legacy-BIOS machine has no ESP and no UEFI removable-media
     * fallback, so the same decision selects the two roles the measured Limine
     * BIOS chain needs instead - the BIOS boot partition for stage2 and the FAT
     * boot volume for the payload set.  Both come from the plan the BIOS
     * contract built; a plan that does not carry them is refused here, before
     * the first write.  The ESP branch below is the pre-item code.
     */
    memset(&bios_boot, 0, sizeof(bios_boot));
    memset(&boot_volume, 0, sizeof(boot_volume));
    memset(&boot_esp, 0, sizeof(boot_esp));
    if (bios_mode) {
        const struct installer_layout_partition *bios_role =
            production_install_flow_role_partition(
                &plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
        const struct installer_layout_partition *volume_role =
            production_install_flow_role_partition(
                &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);

        if (bios_role == NULL || volume_role == NULL) {
            production_install_flow_failed(
                "bios-payload",
                production_installer_status_string(
                    PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE));
            return false;
        }
        bios_boot.present = true;
        bios_boot.first_lba = bios_role->first_lba;
        bios_boot.length_lba = bios_role->length_lba;
        boot_volume.present = true;
        boot_volume.first_lba = volume_role->first_lba;
        boot_volume.length_lba = volume_role->length_lba;
    } else {
        esp = production_install_flow_role_partition(
            &plan, INSTALLER_LAYOUT_ROLE_EFI_SYSTEM);
        if (mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
            boot_esp.present = production_install_flow_reuse.present;
            boot_esp.reused = production_install_flow_reuse.present;
            boot_esp.first_lba = production_install_flow_reuse.first_lba;
            boot_esp.length_lba = production_install_flow_reuse.length_lba;
        } else if (esp != NULL) {
            boot_esp.present = true;
            boot_esp.reused = false;
            boot_esp.first_lba = esp->first_lba;
            boot_esp.length_lba = esp->length_lba;
        }
        if (!boot_esp.present) {
            production_install_flow_failed(
                "esp-payload",
                production_installer_status_string(
                    PRODUCTION_INSTALLER_INVALID_ARGUMENT));
            return false;
        }
    }

    /*
     * ITEM-397 pre-write gate: an ESP that cannot hold the installed boot set
     * is refused here, before the layout write, so a disk that could not boot
     * is never partitioned.  The refusal is published as the boot-entry record
     * the acceptance INSTALLER stage quotes.
     *
     * ITEM-517: the BIOS gate is the same idea against the measured contract -
     * the stage blob must be the pinned release, the BIOS boot partition must
     * hold stage2 and the FAT boot volume must hold the payload set - and its
     * refusal is published on the same boot-entry vocabulary.
     */
    memset(&boot_outcome, 0, sizeof(boot_outcome));
    memset(&bios_outcome, 0, sizeof(bios_outcome));
    if (bios_mode) {
        status = production_installer_bios_boot_requirements(
            &installer, &bios_boot, &boot_volume, &boot_media,
            &bios_outcome);
        if (status != PRODUCTION_INSTALLER_OK) {
            (void)production_installer_bios_boot_publish(
                &bios_outcome, status);
            production_install_flow_refuse_controller(
                production_installer_status_string(status) != NULL
                    ? production_installer_status_string(status)
                    : "PRODUCTION_INSTALLER_BAD_STATE",
                NULL);
            return false;
        }
    } else {
        status = production_installer_boot_stage_requirements(
            &installer, &boot_esp, &boot_media, &boot_outcome);
        if (status != PRODUCTION_INSTALLER_OK) {
            (void)production_installer_boot_stage_publish(
                &boot_outcome, status);
            production_install_flow_refuse_controller(
                production_installer_status_string(status) != NULL
                    ? production_installer_status_string(status)
                    : "PRODUCTION_INSTALLER_BAD_STATE",
                NULL);
            return false;
        }
    }

    /*
     * ITEM-586: an update is a state-preserving reinstall.  Before the first
     * write, prove that the current table carries the appliance state in
     * exactly the persistence range this plan is about to write and that the
     * installed reader can load it.  A mismatch refuses here, so a medium that
     * names `update` can never erase or move the state it promised to keep.
     */
    memset(&preserve_evidence, 0, sizeof(preserve_evidence));
    memset(&loader, 0, sizeof(loader));
    if (production_install_flow.preserve_state) {
        memset(&state_layout, 0, sizeof(state_layout));
        status = appliance_state_layout_default(&state_layout) ==
                         APPLIANCE_STATE_LAYOUT_OK
                     ? production_installer_state_preserve(
                           &installer, device, &state_layout, &plan,
                           &loader, &preserve_evidence)
                     : PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        if (status != PRODUCTION_INSTALLER_OK ||
            !preserve_evidence.verify_loaded) {
            const char *status_text =
                production_installer_status_string(status) != NULL
                    ? production_installer_status_string(status)
                    : "PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE";

            /*
             * ITEM-588: the refusal names its rule and the two ranges, so the
             * console and the durable journal answer "why" without a second
             * run.  The record is published before the refused line and the
             * run still writes nothing.
             */
            {
                static struct production_install_flow_line refused_line;

                production_install_flow_line_begin(&refused_line);
                production_install_flow_line_append(
                    &refused_line,
                    "[PRODUCTION-INSTALL] state preserve refused=");
                production_install_flow_line_append(
                    &refused_line,
                    production_installer_state_preserve_refusal_string(
                        preserve_evidence.refusal));
                production_install_flow_line_append(&refused_line, " current=");
                production_install_flow_line_append(
                    &refused_line,
                    preserve_evidence.current_present ? "PRESENT" : "MISSING");
                if (preserve_evidence.current_present) {
                    production_install_flow_line_append(&refused_line, " ");
                    production_install_flow_line_u64(
                        &refused_line,
                        preserve_evidence.current_first_lba);
                    production_install_flow_line_append(&refused_line, "+");
                    production_install_flow_line_u64(
                        &refused_line,
                        preserve_evidence.current_length_lba);
                }
                production_install_flow_line_append(&refused_line, " plan=");
                if (preserve_evidence.plan_length_lba == 0U) {
                    production_install_flow_line_append(&refused_line, "NONE");
                } else {
                    production_install_flow_line_u64(
                        &refused_line, preserve_evidence.plan_first_lba);
                    production_install_flow_line_append(&refused_line, "+");
                    production_install_flow_line_u64(
                        &refused_line, preserve_evidence.plan_length_lba);
                }
                production_install_flow_line_append(&refused_line, " loader=");
                production_install_flow_line_append(
                    &refused_line,
                    preserve_evidence.loader_ready ? "READY"
                                                   : "NOT_READY");
                production_install_flow_line_append(&refused_line, " config=");
                production_install_flow_line_append(
                    &refused_line,
                    preserve_evidence.config_preserved ? "PRESENT"
                                                       : "DEFAULT");
                production_install_flow_line_append(&refused_line, " admin=");
                production_install_flow_line_append(
                    &refused_line,
                    preserve_evidence.admin_preserved ? "PRESENT" : "ABSENT");
                production_install_flow_line_append(&refused_line, " writes=0\n");
                production_install_flow_line_publish(&refused_line);
            }
            production_install_flow_refuse_controller(status_text, NULL);
            if (production_install_flow.mode ==
                PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE) {
                production_install_flow.refused = true;
                production_install_flow_remember_controller_refusal(
                    status_text);
            }
            return false;
        }
        {
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line, "[PRODUCTION-INSTALL] state preserve preflight=VERIFIED stores=");
            production_install_flow_line_u64(
                &line, preserve_evidence.stores);
            production_install_flow_line_append(&line, " current=");
            production_install_flow_line_u64(
                &line, preserve_evidence.current_first_lba);
            production_install_flow_line_append(&line, "+");
            production_install_flow_line_u64(
                &line, preserve_evidence.current_length_lba);
            production_install_flow_line_append(&line, " plan=");
            production_install_flow_line_u64(
                &line, preserve_evidence.plan_first_lba);
            production_install_flow_line_append(&line, "+");
            production_install_flow_line_u64(
                &line, preserve_evidence.plan_length_lba);
            production_install_flow_line_append(&line, " loader=READY");
            /* ITEM-588: the same two words the refusal record uses - the
               update keeps a default-only state as it is, so `DEFAULT` is a
               fact about the volume, not a failure. */
            production_install_flow_line_append(
                &line, " config=");
            production_install_flow_line_append(
                &line,
                preserve_evidence.config_preserved ? "PRESENT"
                                                   : "DEFAULT");
            production_install_flow_line_append(&line, " admin=");
            production_install_flow_line_append(
                &line,
                preserve_evidence.admin_preserved ? "PRESENT"
                                                  : "ABSENT");
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
        }
    }

    memset(&snapshot, 0, sizeof(snapshot));
    status = mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT
                 ? production_installer_execute_layout_preserving(
                       &installer, device, &plan,
                       &production_install_flow_reuse,
                       &production_install_flow_gpt)
                 : production_installer_execute_layout(
                       &installer, device, &plan, &snapshot);
    if (status != PRODUCTION_INSTALLER_OK) {
        production_install_flow_failed(
            "layout-write", production_installer_status_string(status));
        return false;
    }

    /*
     * ITEM-517: the legacy-BIOS chain needs no EFI environment - the machine
     * has no UEFI and the boot volume is a plain FAT32 volume, not an ESP - so
     * only the UEFI branch builds one.  The BIOS call takes the block device
     * directly and writes the measured MBR/stage2 recipe.
     */
    if (!bios_mode) {
        memset(&environment, 0, sizeof(environment));
        if (efi_boot_environment_init(
                &environment, EFI_BOOT_FIRMWARE_UEFI, device, NULL,
                production_install_flow_variable_read,
                production_install_flow_variable_write) !=
                EFI_BOOT_ENVIRONMENT_OK) {
            production_install_flow_failed(
                "esp-payload",
                production_installer_status_string(
                    PRODUCTION_INSTALLER_EFI_UNAVAILABLE));
            return false;
        }
    }

    /*
     * ITEM-397: stage, verify and publish the installed boot set.  One call
     * writes the bounded set (the loader on the removable-media path, the
     * configuration, the sealed boot chain and the kernel), re-reads it and
     * compares it with the sealed media source digest, requires the staged
     * configuration to name the staged kernel, and publishes exactly one
     * boot-entry record naming the mechanism that makes the firmware find the
     * disk.  The kernel has no EFI runtime, so that mechanism is the
     * removable-media path here; a run whose runtime can write the boot
     * variables reports EFI_BOOT_VARIABLE instead.
     *
     * ITEM-517: the BIOS branch is the same shape against the other boot chain:
     * the payload set goes to the FAT boot volume, stage1 to LBA0 and stage2 to
     * the BIOS boot partition, the whole set is verified by read-back, and the
     * record names mechanism=BIOS_MBR with the BIOS configuration path.
     */
    memset(&boot_outcome, 0, sizeof(boot_outcome));
    if (bios_mode) {
        status = production_installer_bios_boot_run(
            &installer, device, &bios_boot, &boot_volume, &boot_media,
            &bios_outcome);
        if (status != PRODUCTION_INSTALLER_OK) {
            (void)production_installer_bios_boot_publish(
                &bios_outcome, status);
            production_install_flow_failed(
                "bios-payload", production_installer_status_string(status));
            return false;
        }
        {
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line,
                "[PRODUCTION-INSTALL] bios payload=VERIFIED first_lba=");
            production_install_flow_line_u64(&line, boot_volume.first_lba);
            production_install_flow_line_append(&line, " length_lba=");
            production_install_flow_line_u64(
                &line, boot_volume.length_lba);
            production_install_flow_line_append(&line, " payload_bytes=");
            production_install_flow_line_u64(
                &line, bios_outcome.payload_bytes);
            production_install_flow_line_append(&line, " mode=");
            production_install_flow_line_append(
                &line, production_install_flow_mode_string(mode));
            production_install_flow_line_append(&line, " staged_bytes=");
            production_install_flow_line_u64(
                &line, bios_outcome.payload_bytes);
            production_install_flow_line_append(&line, " payload_set=");
            production_install_flow_line_u64(
                &line, bios_outcome.payload_count);
            production_install_flow_line_append(&line, " mechanism=");
            production_install_flow_line_append(
                &line, production_installer_boot_mechanism_string(
                           bios_outcome.mechanism));
            production_install_flow_line_append(
                &line, " bios_boot_first_lba=");
            production_install_flow_line_u64(&line, bios_boot.first_lba);
            production_install_flow_line_append(
                &line, " bios_boot_sectors=");
            production_install_flow_line_u64(
                &line, bios_boot.length_lba);
            production_install_flow_line_append(&line, " stage2_bytes=");
            production_install_flow_line_u64(
                &line, bios_outcome.stage2_bytes);
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
        }
    } else {
        status = production_installer_boot_stage_run(
            &installer, &environment, &boot_esp, &boot_media,
            &boot_outcome);
        if (status != PRODUCTION_INSTALLER_OK) {
            (void)production_installer_boot_stage_publish(
                &boot_outcome, status);
            production_install_flow_failed(
                "esp-payload", production_installer_status_string(status));
            return false;
        }
        {
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line,
                "[PRODUCTION-INSTALL] esp payload=VERIFIED first_lba=");
            production_install_flow_line_u64(&line, boot_esp.first_lba);
            production_install_flow_line_append(&line, " length_lba=");
            production_install_flow_line_u64(&line, boot_esp.length_lba);
            production_install_flow_line_append(&line, " payload_bytes=");
            production_install_flow_line_u64(
                &line, (uint64_t)source.boot_payload_size);
            production_install_flow_line_append(&line, " mode=");
            production_install_flow_line_append(
                &line, production_install_flow_mode_string(mode));
            production_install_flow_line_append(&line, " staged_bytes=");
            production_install_flow_line_u64(
                &line, boot_outcome.sealed_length);
            production_install_flow_line_append(&line, " mechanism=");
            production_install_flow_line_append(
                &line, production_installer_boot_mechanism_string(
                           boot_outcome.mechanism));
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
        }
    }

    if (!production_install_flow.preserve_state) {
    /*
     * ITEM-502: the explicit named erase step.
     *
     * The layout writers only touch the GPT and the old state initializer only
     * rewrote each store's block-0 header, so the journals - the previous run's
     * static address, its administrator credential, its certificates - survived
     * every reinstall and the next boot replayed them.  Here, before anything
     * of the new system is initialized and before the boot log flushes (so this
     * record is durable on the installed ESP), the appliance's own state is
     * erased and read back empty: the raw volumes and the real loader must both
     * agree that nothing loads.  A failure refuses the install before the
     * `applied` record, and `state_initialize` refuses a run whose erase did
     * not verify.
     */
    {
        struct production_installer_state_erase_evidence erase_evidence;

        memset(&state_layout, 0, sizeof(state_layout));
        memset(&loader, 0, sizeof(loader));
        memset(&erase_evidence, 0, sizeof(erase_evidence));
        status = appliance_state_layout_default(&state_layout) ==
                         APPLIANCE_STATE_LAYOUT_OK
                     ? production_installer_state_erase(
                           &installer, device, &state_layout, &plan,
                           &loader, &erase_evidence)
                     : PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        if (status != PRODUCTION_INSTALLER_OK ||
            !erase_evidence.verify_clean) {
            production_install_flow_failed(
                "state-erase",
                production_installer_status_string(status) != NULL
                    ? production_installer_status_string(status)
                    : "PRODUCTION_INSTALLER_VERIFICATION_FAILED");
            return false;
        }
        {
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line, "[PRODUCTION-INSTALL] state erase stores=");
            production_install_flow_line_u64(
                &line, erase_evidence.stores);
            production_install_flow_line_append(&line, " blocks=");
            production_install_flow_line_u64(
                &line, erase_evidence.blocks);
            production_install_flow_line_append(
                &line, " verify=CLEAN\n");
            production_install_flow_line_publish(&line);
        }
    }
    }

    /*
     * ITEM-410: the disk the operator will boot from exists now, so the boot
     * log attaches to the boot volume this run just staged into - the ESP role
     * on a UEFI install, the FAT boot volume on a legacy-BIOS one - and writes
     * the records the install published: the plan, the confirmation, the
     * layout, the staged boot set, the state erase.  That is what makes the
     * INSTALLER stage of a hardware run readable afterwards instead of
     * photographed, and it is the only disk this run ever attaches the log to:
     * the medium the installer booted from is never a write target.
     *
     * ITEM-517: the two boot chains reach that volume differently - the UEFI
     * install through the EFI environment it built for the boot stage, the
     * legacy-BIOS install by device and extent, because a BIOS machine has no
     * EFI environment to construct.  The attach status is not discarded: an
     * attach that fails publishes `[BOOTLOG] state=UNAVAILABLE reason=<status>`
     * here, so a boot volume the log could not bind is named in the install's
     * own records instead of failing silently.
     */
    {
        const enum appliance_boot_log_store_status boot_log_status =
            appliance_boot_log_store_attach_extent(
                device,
                bios_mode ? boot_volume.first_lba : boot_esp.first_lba,
                bios_mode ? boot_volume.length_lba : boot_esp.length_lba,
                "install");

        if (boot_log_status != APPLIANCE_BOOT_LOG_STORE_OK) {
            const char *const boot_log_reason =
                appliance_boot_log_store_status_string(boot_log_status);
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line, "[BOOTLOG] state=UNAVAILABLE reason=");
            production_install_flow_line_append(
                &line, boot_log_reason != NULL ? boot_log_reason
                                               : "UNREADABLE");
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
        }
    }
    appliance_boot_log_store_flush("install");

    if (!production_install_flow.preserve_state) {
        memset(&state_layout, 0, sizeof(state_layout));
        memset(&loader, 0, sizeof(loader));
        status = appliance_state_layout_default(&state_layout) ==
                         APPLIANCE_STATE_LAYOUT_OK
                     ? production_installer_state_initialize(
                           &installer, device, &state_layout, &loader)
                     : PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        if (status != PRODUCTION_INSTALLER_OK) {
            production_install_flow_failed(
                "persistent-state",
                production_installer_status_string(status));
            return false;
        }
        {
            static struct production_install_flow_line line;

            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line, "[PRODUCTION-INSTALL] state initialized=VERIFIED ready=");
            production_install_flow_line_append(
                &line, appliance_state_loader_ready(&loader) ? "1" : "0");
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
        }
    } else {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] state preserved=VERIFIED stores=");
        production_install_flow_line_u64(&line, preserve_evidence.stores);
        production_install_flow_line_append(&line, " config=");
        production_install_flow_line_append(
            &line,
            preserve_evidence.config_preserved ? "PRESERVED" : "MISSING");
        production_install_flow_line_append(&line, " admin=");
        production_install_flow_line_append(
            &line,
            preserve_evidence.admin_preserved ? "PRESERVED" : "ABSENT");
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);
    }
    if (!production_install_flow_provision_host_seed(
            &loader, production_install_flow.preserve_state)) {
        return false;
    }
    /*
     * ITEM-606: the owner-confirmed public key travels on the medium as one
     * Limine module and is written into the installed system's state here,
     * after the state exists and before the install is applied.  The write
     * path flushes and reads the record back through the installed reader; a
     * failure refuses the run instead of promising an SSH key the next boot
     * does not have.  The private key never leaves the owner's machine.
     */
    if (production_install_flow.ssh_key_present) {
        enum appliance_ssh_authorized_key_status key_status =
            appliance_ssh_authorized_key_store(
                &loader, production_install_flow.ssh_key_blob,
                production_install_flow.ssh_key_blob_length);
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] ssh-key fingerprint=");
        production_install_flow_line_append(
            &line, production_install_flow.ssh_key_fingerprint);
        production_install_flow_line_append(
            &line, key_status == APPLIANCE_SSH_AUTHORIZED_KEY_OK
                       ? " persisted=VERIFIED\n"
                       : " persisted=FAILED\n");
        production_install_flow_line_publish(&line);
        if (key_status != APPLIANCE_SSH_AUTHORIZED_KEY_OK) {
            production_install_flow_clear_ssh_key();
            production_install_flow_failed(
                "ssh-key",
                appliance_ssh_authorized_key_status_string(key_status) != NULL
                    ? appliance_ssh_authorized_key_status_string(key_status)
                    : "VERIFICATION_FAILED");
            return false;
        }
        production_install_flow_clear_ssh_key();
    }

    /*
     * ITEM-509 increment 27 (509.7c-2): the language the medium declared is
     * written into the installed system's state now - immediately after the
     * state exists and before anything else commits into it - read back through
     * the same reader the installed system uses, and only then does the run
     * publish the record.  The default language writes nothing at all: an
     * English install keeps the record the previous increments produced, byte
     * for byte, and the installed system reads `en` from its absence exactly as
     * it always has.  A read-back that does not match the declared language
     * fails the install instead of promising a language the record does not
     * hold.
     */
    if (production_install_flow.declaration.language !=
        APPLIANCE_LANGUAGE_DEFAULT) {
        enum production_installer_status language_status =
            production_installer_state_commit_language(
                &loader, production_install_flow.declaration.language);
        const bool language_persisted =
            language_status == PRODUCTION_INSTALLER_OK;
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] language=");
        production_install_flow_line_append(
            &line, appliance_language_code(
                       production_install_flow.declaration.language));
        production_install_flow_line_append(
            &line, language_persisted ? " persisted=VERIFIED\n"
                                      : " persisted=FAILED\n");
        production_install_flow_line_publish(&line);
        if (!language_persisted) {
            production_install_flow_failed(
                "language",
                production_installer_status_string(language_status) != NULL
                    ? production_installer_status_string(language_status)
                    : "PRODUCTION_INSTALLER_VERIFICATION_FAILED");
            return false;
        }
    }

    /*
     * ITEM-506 increment 4: the account the operator set on the install screen
     * is written into the installed system's state now, read back through the
     * installed system's own reader, and only then does the run promise
     * `admin=SET`.  An install with no operator account writes no
     * administrator record at all and says which entry left it unset; the
     * first-run gate that refuses to serve until such a machine has an account
     * is the next increment, and until then the pre-existing behaviour stands:
     * the listener starts with the management gate closed.
     */
    if (production_install_flow.operator_credential_set) {
        enum production_installer_status admin_status =
            production_installer_state_commit_admin(
                &loader, &production_install_flow.operator_credential);
        const bool admin_persisted =
            admin_status == PRODUCTION_INSTALLER_OK;
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] admin=SET login=");
        production_install_flow_line_append(
            &line, production_install_flow.operator_credential.login);
        production_install_flow_line_append(
            &line, admin_persisted ? " persisted=VERIFIED\n"
                                   : " persisted=FAILED\n");
        production_install_flow_line_publish(&line);
        memset(&production_install_flow.operator_credential, 0,
               sizeof(production_install_flow.operator_credential));
        production_install_flow.operator_credential_set = false;
        if (!admin_persisted) {
            production_install_flow_failed(
                "admin-account",
                production_installer_status_string(admin_status) != NULL
                    ? production_installer_status_string(admin_status)
                    : "PRODUCTION_INSTALLER_VERIFICATION_FAILED");
            return false;
        }
    } else if (production_install_flow.preserve_state) {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] admin=");
        production_install_flow_line_append(
            &line,
            preserve_evidence.admin_preserved ? "PRESERVED" : "ABSENT");
        production_install_flow_line_append(&line, " reason=UPDATE\n");
        production_install_flow_line_publish(&line);
    } else {
        static struct production_install_flow_line line;

        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] admin=UNSET reason=");
        production_install_flow_line_append(
            &line,
            production_install_flow.mode ==
                    PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED
                ? "UNATTENDED\n"
                : "OPERATOR_DECLINED\n");
        production_install_flow_line_publish(&line);
    }
    production_install_flow_clear_operator_secret();

    status = production_installer_verify_layout(&installer, device, &plan);
    if (status != PRODUCTION_INSTALLER_OK) {
        production_install_flow_failed(
            "layout-verify", production_installer_status_string(status));
        return false;
    }
    /*
     * The record the acceptance INSTALLER stage quotes: the contracted layout
     * the run planned, re-read from the GPT the writer produced.
     */
    if (production_installer_publish_layout_record(&installer) !=
        PRODUCTION_INSTALLER_OK) {
        production_install_flow_failed(
            "layout-record",
            production_installer_status_string(
                PRODUCTION_INSTALLER_VERIFICATION_FAILED));
        return false;
    }

    production_install_flow_publish_record(
        production_install_media_render_applied(
            &production_install_flow.declaration,
            &production_install_flow.plan.target,
            timer_uptime_seconds(), record, sizeof(record), &length),
        record);

    /*
     * ITEM-411: the install is complete and the machine reboots itself.
     *
     * Everything above this line verified what it wrote: the layout, the ESP
     * payload set, the state volume and the layout record.  Nothing below it
     * may run on a path that did not get here, which is why the reset lives at
     * the end of the success path and nowhere else - a refused, failed or
     * console-pending run keeps its current behaviour and leaves the machine
     * running.
     *
     * The operator is told in one record, mirrored on the local console: the
     * disk and its identity, that the installation medium has to come out (the
     * firmware prefers the removable medium, so a machine left with the stick
     * in boots the installer again), which mechanism the machine will be reset
     * through, and how many seconds that takes.  No keypress is asked for,
     * because the local keyboard may be unavailable (ITEM-392/395).
     */
    {
        static struct production_install_flow_line line;
        struct platform_reset_plan reset_plan;

        if (platform_reset_plan_current(&reset_plan) != PLATFORM_RESET_OK) {
            /* The plan always ends in the triple fault, so this cannot happen
             * on a machine that reached this point; if it ever did, the run
             * refuses to reboot rather than reboot blind. */
            production_install_flow_failed(
                "reset-plan",
                production_installer_status_string(
                    PRODUCTION_INSTALLER_BAD_STATE));
            return false;
        }
        production_install_flow_line_begin(&line);
        production_install_flow_line_append(
            &line, "[PRODUCTION-INSTALL] complete disk=");
        production_install_flow_line_u64(&line, (uint64_t)disk_id);
        production_install_flow_line_append(&line, " identity=");
        production_install_flow_line_append(
            &line, production_install_flow.plan.target.identity);
        production_install_flow_line_append(&line, " medium=REMOVE reset=");
        production_install_flow_line_append(
            &line,
            platform_reset_mechanism_string(
                platform_reset_plan_primary(&reset_plan)));
        production_install_flow_line_append(&line, " in=");
        production_install_flow_line_u64(
            &line, (uint64_t)(PLATFORM_RESET_OPERATOR_DELAY_MS / 1000U));
        production_install_flow_line_append(&line, "s screen=");
        /*
         * ITEM-417: whether the operator is being told on the screen at all.
         * The flow runs before the kernel's console exists, so its records
         * reach the serial stream and are held back from the display; the
         * banner this item paints is what the person standing at the machine
         * reads, and the record says which of the two happened.
         */
        production_install_flow_line_append(
            &line, framebuffer_ready() ? "BANNER" : "NONE");
        production_install_flow_line_append(&line, "\n");
        production_install_flow_line_publish(&line);

        /*
         * The boot log (ITEM-410) writes this boot's records to the appliance's
         * own disk; a flush of that facility belongs here, after the last
         * record and immediately before the machine goes down, so nothing this
         * run published is lost.  The facility is not in this tree yet, so
         * there is nothing to call: the records above are published before the
         * reset, which is what makes them recoverable from the serial capture
         * whatever the next item binds here.
         */
        /*
         * The countdown on the screen and the delay before the reset are the
         * same wait: a banner that could not be painted falls back to the
         * delay, so the machine never reboots a second early because its
         * display was missing.
         */
        if (!installer_notice_banner(
                production_install_flow.declaration.language,
                (uint32_t)(PLATFORM_RESET_OPERATOR_DELAY_MS / 1000U))) {
            production_install_flow_line_begin(&line);
            production_install_flow_line_append(
                &line, "[PRODUCTION-INSTALL] notice screen=NONE reason=");
            production_install_flow_line_append(
                &line, framebuffer_ready() ? "BANNER_FAILED"
                                           : "NO_FRAMEBUFFER");
            production_install_flow_line_append(&line, "\n");
            production_install_flow_line_publish(&line);
            platform_reset_delay_ms(PLATFORM_RESET_OPERATOR_DELAY_MS);
        }
        platform_reset_execute(&reset_plan);
    }
    return true;
}

bool production_install_flow_engaged(void)
{
    return production_install_flow.engaged;
}

void production_install_flow_run(
    const struct production_install_flow_media *media)
{
    enum production_install_media_mode mode = PRODUCTION_INSTALL_MEDIA_MODE_NONE;
    enum production_install_media_status status;
    char record[PRODUCTION_INSTALL_MEDIA_RECORD_MAX];
    size_t length = 0U;

    production_install_flow.engaged = false;
    production_install_flow.prepared = false;
    production_install_flow.choice_applied = false;
    production_install_flow_clear_operator_secret();
    production_install_flow_clear_host_seed();
    production_install_flow_clear_ssh_key();
    memset(&production_install_flow.operator_credential, 0,
           sizeof(production_install_flow.operator_credential));
    production_install_flow.operator_credential_set = false;
    production_install_flow.media = NULL;
    production_install_flow.menu_built = false;
    production_install_flow.menu_reason = NULL;
    /*
     * ITEM-500: what the operator entry refused with, when it refused before
     * the screen could open.  The boot loop paints that reason where the
     * person can read it instead of leaving the machine to boot from the
     * medium in silence.
     */
    production_install_flow.refused = false;
    production_install_flow.refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE;
    production_install_flow.mode = PRODUCTION_INSTALL_MEDIA_MODE_NONE;
    production_install_flow.choice_note[0] = '\0';
    /* ITEM-501: a new run starts with no partition given up and no
     * plan refusal to paint. */
    production_install_flow.release_chosen = false;
    production_install_flow.release_index = 0U;
    /* ITEM-514: a new run starts with no scheme chosen by an operator. */
    production_install_flow.chosen_scheme = INSTALLER_STORAGE_SCHEME_NONE;
    production_install_flow.controller_refusal[0] = '\0';
    if (media == NULL || media->version != PRODUCTION_INSTALL_FLOW_VERSION) {
        return;
    }
    production_install_flow.media_storage = *media;
    production_install_flow.media = &production_install_flow.media_storage;

    if (production_install_media_mode_parse(
            media->command_line, media->command_line_length,
            &mode) != PRODUCTION_INSTALL_MEDIA_OK) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_MODE_MALFORMED);
        return;
    }
    production_install_flow.mode = mode;
    if (mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE) {
        /* The appliance entry: the installer medium boots as a console. */
        return;
    }
    /*
     * ITEM-638: an oversized declaration is its own refusal.  The parser
     * already refuses it, but mapping every non-OK status to TOKEN_MISSING
     * hid the real reason on the owner's screen; missing and malformed keep
     * their old meanings below.
     */
    if (media->declaration_length > PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX) {
        /*
         * ITEM-640: an overlong product declaration owns this boot even though
         * it is refused here.  Without the flag the in-process QEMU installer
         * fixture runs after the refusal and commits state, so the update
         * proof's zero-write result depends on how fast the harness stops
         * QEMU.  The refusal still returns before any install write; a medium
         * with a complete declaration and a medium without a product mode keep
         * their previous ownership rule below.
         */
        production_install_flow.engaged = true;
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_TOO_LARGE);
        return;
    }
    status = production_install_media_declaration_parse(
        media->declaration, media->declaration_length,
        &production_install_flow.declaration);
    if (status == PRODUCTION_INSTALL_MEDIA_BAD_TEXT) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MALFORMED);
        return;
    }
    if (status != PRODUCTION_INSTALL_MEDIA_OK) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING);
        return;
    }
    if (production_install_flow.declaration.host_seed_required) {
        enum appliance_host_seed_status host_seed_status;

        if (media->host_seed == NULL || media->host_seed_size == 0U) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_MISSING);
            return;
        }
        host_seed_status = appliance_host_seed_media_parse(
            media->host_seed, media->host_seed_size,
            &production_install_flow.host_seed_record);
        if (host_seed_status != APPLIANCE_HOST_SEED_OK) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID);
            return;
        }
        production_install_flow.host_seed_record_valid = true;
    } else if (media->host_seed != NULL || media->host_seed_size != 0U) {
        /*
         * A seed module on an undeclared medium is ambiguous provenance.
         * Refuse it instead of silently ignoring secret-looking media state.
         */
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID);
        return;
    }
    if (production_install_flow.declaration.ssh_key_declared) {
        uint8_t fingerprint[32];
        char fingerprint_text[PRODUCTION_INSTALL_MEDIA_SSH_KEY_MAX];

        if (media->ssh_key == NULL || media->ssh_key_size == 0U) {
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_MISSING);
            return;
        }
        memset(fingerprint, 0, sizeof(fingerprint));
        memset(fingerprint_text, 0, sizeof(fingerprint_text));
        if (!appliance_ssh_authorized_key_from_point(
                (const char *)media->ssh_key, media->ssh_key_size,
                production_install_flow.ssh_key_blob,
                &production_install_flow.ssh_key_blob_length,
                fingerprint) ||
            appliance_ssh_authorized_key_fingerprint_text(
                fingerprint, fingerprint_text,
                sizeof(fingerprint_text)) == 0U ||
            strcmp(fingerprint_text,
                   production_install_flow.declaration.ssh_key_fingerprint) !=
                0) {
            production_install_flow_clear_ssh_key();
            production_install_flow_refuse(
                PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_INVALID);
            return;
        }
        production_install_flow.ssh_key_present = true;
        memcpy(production_install_flow.ssh_key_fingerprint, fingerprint_text,
               sizeof(production_install_flow.ssh_key_fingerprint));
    } else if (media->ssh_key != NULL || media->ssh_key_size != 0U) {
        /*
         * A public key module on an undeclared medium is as ambiguous as an
         * undeclared seed: refuse it instead of silently importing a key the
         * owner's token never confirmed.
         */
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_INVALID);
        return;
    }

    /*
     * A product install run owns this boot only when the medium carries a
     * complete declaration.  An unauthorised medium refuses and leaves the
     * rest of the boot alone, which is also what keeps the installer image's
     * QEMU fixture proof running on its own media.
     */
    production_install_flow.engaged =
        production_install_flow.declaration.present &&
        production_install_flow.declaration.confirm_declared &&
        production_install_flow.declaration.target_declared;

    if (!production_install_flow_report_build(media)) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE);
        return;
    }
    if (production_install_media_plan_build(
            mode, &production_install_flow.declaration,
            media->firmware_mode, &production_install_flow.report,
            &production_install_flow.plan) !=
        PRODUCTION_INSTALL_MEDIA_OK) {
        production_install_flow_refuse(
            PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE);
        return;
    }

    production_install_flow_publish_record(
        production_install_media_render_declaration(
            &production_install_flow.declaration, mode, record,
            sizeof(record), &length),
        record);
    production_install_flow_publish_candidates();

    if (production_install_flow.plan.action ==
        PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED) {
        production_install_flow_refuse(
            production_install_flow.plan.refusal);
        return;
    }
    if (production_install_flow.plan.action ==
        PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE) {
        /*
         * ITEM-495: the operator entry prepares the run and waits for the
         * person at the storage screen - where local input is live - not for a
         * line typed at the console.  A machine with no local input keeps this
         * record as the statement of what it is waiting for, and never
         * installs; the record no longer names a console command, because
         * there is none.
         *
         * ITEM-500: on a machine that does not carry the disk the medium
         * declared, the plan has no selected disk and the operator's choice is
         * the only target there is.  The run is prepared all the same - the
         * screen opens - and the record names the declared disk, that this
         * machine does not have it, and that no disk is chosen yet.
         */
        production_install_flow.prepared = true;
        if (production_install_flow.plan.selected) {
            production_install_flow_publish_record(
                production_install_media_render_pending(
                    &production_install_flow.declaration,
                    &production_install_flow.plan.target, record,
                    sizeof(record), &length),
                record);
        } else {
            /*
             * The screen explains itself: the first line names the disk the
             * medium declared and says this machine does not have it.
             */
            (void)production_install_media_render_choice_note(
                &production_install_flow.declaration,
                production_install_flow.declaration.language,
                production_install_flow.choice_note,
                sizeof(production_install_flow.choice_note), &length);
            production_install_flow_publish_record(
                production_install_media_render_awaiting_choice(
                    &production_install_flow.declaration, record,
                    sizeof(record), &length),
                record);
        }
        return;
    }
    (void)production_install_flow_apply(
        PRODUCTION_INSTALL_MEDIA_CONFIRMATION_MEDIA);
}

/*
 * ITEM-495: the operator's decision, taken where the person is - at the storage
 * screen that showed the warning.  The declaration the medium carries is still
 * what the plan was built from, and the disk is the one the operator chose, so
 * a decision made on the screen cannot widen the write: it can only start the
 * plan the operator was shown.  Returns false - and the run is refused with a
 * named reason - when there is no prepared run to confirm.
 */
bool production_install_flow_confirm_by_operator(void)
{
    if (!production_install_flow.prepared ||
        !production_install_flow.choice_applied) {
        return false;
    }
    production_install_flow.prepared = false;
    return production_install_flow_apply(
        PRODUCTION_INSTALL_MEDIA_CONFIRMATION_OPERATOR);
}

/* ------------------------------------------- ITEM-506 increment 4 account */

enum production_install_flow_credential_status
production_install_flow_set_credential_salt(
    production_install_flow_random_fn random,
    void *random_context)
{
    production_install_flow.credential_random = random;
    production_install_flow.credential_random_context = random_context;
    return PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK;
}

enum production_install_flow_credential_requirement
production_install_flow_credential_requirement(void)
{
    if (!production_install_flow.declaration.host_seed_required) {
        return PRODUCTION_INSTALL_FLOW_CREDENTIAL_OPTIONAL_SET;
    }
    return production_install_flow.declaration.layout_update
               ? PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY
               : PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_SET;
}

enum production_install_flow_credential_status
production_install_flow_set_operator_credential(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length)
{
    struct appliance_admin_credential credential;
    enum appliance_admin_credential_status credential_status;
    uint8_t salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    enum production_install_flow_credential_status status;

    production_install_flow_clear_operator_secret();
    if (login == NULL || password == NULL) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_NULL;
        return production_install_flow.credential_status;
    }
    credential_status = appliance_admin_credential_validate_login(
        login, login_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_LOGIN;
        return production_install_flow.credential_status;
    }
    credential_status = appliance_admin_credential_validate_password(
        password, password_length, login, login_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD;
        return production_install_flow.credential_status;
    }
    /*
     * No salt source means no record: the installer does not invent a salt any
     * more than the console path does.
     */
    if (production_install_flow.credential_random == NULL) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_NO_SALT;
        return production_install_flow.credential_status;
    }
    memset(salt, 0, sizeof(salt));
    if (!production_install_flow.credential_random(
            production_install_flow.credential_random_context, salt,
            sizeof(salt))) {
        memset(salt, 0, sizeof(salt));
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_NO_SALT;
        return production_install_flow.credential_status;
    }
    memset(&credential, 0, sizeof(credential));
    credential_status = appliance_admin_credential_create(
        login, login_length, password, password_length, salt,
        APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT, &credential);
    memset(salt, 0, sizeof(salt));
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        memset(&credential, 0, sizeof(credential));
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD;
        return production_install_flow.credential_status;
    }
    memset(&production_install_flow.operator_credential, 0,
           sizeof(production_install_flow.operator_credential));
    production_install_flow.operator_credential = credential;
    production_install_flow.operator_credential_set = true;
    if (password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        memset(&credential, 0, sizeof(credential));
        memset(&production_install_flow.operator_credential, 0,
               sizeof(production_install_flow.operator_credential));
        production_install_flow.operator_credential_set = false;
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD;
        return production_install_flow.credential_status;
    }
    for (size_t index = 0U; index < password_length; ++index) {
        production_install_flow.operator_password[index] = password[index];
    }
    production_install_flow.operator_password[password_length] = '\0';
    production_install_flow.operator_password_length = password_length;
    memset(&credential, 0, sizeof(credential));
    status = PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK;
    production_install_flow.credential_status = status;
    return status;
}

enum production_install_flow_credential_status
production_install_flow_set_existing_operator_credential(
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length)
{
    struct appliance_state_loader loader;
    struct appliance_state_layout layout;
    struct appliance_admin_credential credential;
    struct block_device *device;
    uint32_t disk_id;
    enum appliance_admin_credential_status credential_status;
    enum appliance_state_layout_status layout_status;
    enum appliance_state_loader_status loader_status;
    enum appliance_persistent_secret_config_status secret_status;

    production_install_flow_clear_operator_secret();
    if (login == NULL || password == NULL) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_NULL;
        return production_install_flow.credential_status;
    }
    if (production_install_flow_credential_requirement() !=
        PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_STATE;
        return production_install_flow.credential_status;
    }
    credential_status = appliance_admin_credential_validate_login(
        login, login_length);
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_LOGIN;
        return production_install_flow.credential_status;
    }
    if (password_length == 0U ||
        password_length > APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD;
        return production_install_flow.credential_status;
    }
    disk_id = production_install_flow.plan.target.disk_id;
    if (!production_install_flow.prepared || disk_id == 0U ||
        disk_id > INSTALLER_DISCOVERY_DISK_CAPACITY) {
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_STATE;
        return production_install_flow.credential_status;
    }
    device = production_install_flow.devices[disk_id - 1U];
    memset(&layout, 0, sizeof(layout));
    memset(&loader, 0, sizeof(loader));
    memset(&credential, 0, sizeof(credential));
    layout_status = appliance_state_layout_default(&layout);
    loader_status =
        layout_status == APPLIANCE_STATE_LAYOUT_OK &&
                device != NULL &&
                appliance_state_loader_init(&loader) ==
                    APPLIANCE_STATE_LOADER_OK
            ? appliance_state_loader_load(&loader, device, &layout)
            : APPLIANCE_STATE_LOADER_BAD_STATE;
    if (loader_status != APPLIANCE_STATE_LOADER_OK) {
        memset(&credential, 0, sizeof(credential));
        memset(&loader, 0, sizeof(loader));
        production_install_flow.credential_status =
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_UNAVAILABLE;
        return production_install_flow.credential_status;
    }
    secret_status = appliance_persistent_secret_config_admin_credential(
        &loader.secret_config, &credential);
    credential_status =
        secret_status == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK
            ? appliance_admin_credential_verify(
                  &credential, login, login_length, password,
                  password_length)
            : APPLIANCE_ADMIN_CREDENTIAL_UNAVAILABLE;
    memset(&loader, 0, sizeof(loader));
    memset(&credential, 0, sizeof(credential));
    if (credential_status != APPLIANCE_ADMIN_CREDENTIAL_OK) {
        production_install_flow.credential_status =
            credential_status == APPLIANCE_ADMIN_CREDENTIAL_DENIED
                ? PRODUCTION_INSTALL_FLOW_CREDENTIAL_DENIED
                : PRODUCTION_INSTALL_FLOW_CREDENTIAL_UNAVAILABLE;
        return production_install_flow.credential_status;
    }
    for (size_t index = 0U; index < password_length; ++index) {
        production_install_flow.operator_password[index] = password[index];
    }
    production_install_flow.operator_password[password_length] = '\0';
    production_install_flow.operator_password_length = password_length;
    production_install_flow.existing_credential_verified = true;
    production_install_flow.credential_status =
        PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK;
    return production_install_flow.credential_status;
}

const char *production_install_flow_credential_status_string(
    enum production_install_flow_credential_status status)
{
    switch (status) {
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK:
        return "OK";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_NULL:
        return "NULL";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_STATE:
        return "BAD_STATE";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_LOGIN:
        return "BAD_LOGIN";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_BAD_PASSWORD:
        return "BAD_PASSWORD";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_NO_SALT:
        return "NO_SALT";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_DENIED:
        return "DENIED";
    case PRODUCTION_INSTALL_FLOW_CREDENTIAL_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return NULL;
    }
}

#else
/* Keep this production translation unit non-empty off the install medium. */
int hfoc_production_install_flow_translation_unit_marker;
#endif
