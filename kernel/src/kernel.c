#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "acpi.h"
#include "appliance_identity.h"
#include "appliance_host_seed.h"
#include "hfoc_build.h"
#include "appliance_management_bind.h"
#include "appliance_boot_console.h"
#include "appliance_boot_log_store.h"
#include "install_journal_platform.h"
#include "machine_facts.h"
#include "appliance_management_tls.h"
#include "appliance_loop_heartbeat.h"
#include "appliance_production_audit_boot.h"
#include "appliance_production_admin_enrollment.h"
#include "appliance_panel_gate.h"
#include "appliance_production_acme_service.h"
#include "appliance_production_acme_http01.h"
#include "appliance_production_acme_order.h"
#include "appliance_production_acme_order_boot.h"
#include "appliance_acme_control.h"
#include "appliance_production_certificate.h"
#include "appliance_production_entropy.h"
#include "appliance_production_boot.h"
#include "appliance_production_first_boot.h"
#include "appliance_production_management_api.h"
#include "appliance_panel_auth_trace.h"
#include "appliance_production_network_boot.h"
#include "appliance_production_network_io_record.h"
#include "appliance_production_network_phy_record.h"
#include "hfoc_lwip_forward.h"
#include "hfocfs_workspace.h"
#include "appliance_production_network_wire.h"
#include "appliance_production_panel_view.h"
#include "appliance_production_service_activation.h"
#include "appliance_production_service_boot.h"
#include "appliance_production_serving.h"
#ifdef HFOC_PRODUCTION_TLS
#include "appliance_production_ssh.h"
#endif
#include "kernel_status_format.h"
#include "kernel_status_report.h"
#include "diag_export.h"
#include "kernel_login_trace.h"
#include "appliance_route_config.h"
#include "appliance_production_storage.h"
#include "appliance_production_update_boot.h"
#include "https_service.h"
#include "boot_attestation.h"
#include "boot_chain_boot.h"
#include "boot_measurements.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/timer.h"
#include "arch/x86_64/tss.h"
#include "console.h"
#include "cpu.h"
#include "event_ring.h"
#include "framebuffer.h"
#include "heap.h"
#include "hfoc_lwip_time.h"
#include "hfoc_lwip_timeouts.h"
#include "input_path.h"
#include "install_medium.h"
#include "keyboard.h"
#include "local_input.h"
#include "loop_budget.h"
#include "usb_hid.h"
#include "ehci.h"
#include "xhci.h"
#include "xhci_trb.h"
#include "ioapic.h"
#include "isa_irq3_7.h"
#include "isa_irq8_15.h"
#include "lapic.h"
#include "lapic_timer.h"
#include "display_firmware.h"
#include "limine_min.h"
#include "panic.h"
#include "pmm.h"
#include "production_install_media.h"
#include "process.h"

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
#include "production_install_flow.h"
#include "installer_credential_screen.h"
#include "installer_layout_contract.h"
#include "installer_storage_screen.h"
#include "sha256.h"
#endif
/*
 * ITEM-492: the NIC description is part of every kernel, not of the install
 * medium: a machine's network controllers are discovered and described the same
 * way whether the image installs or runs.
 */
#include "nic_backends.h"
#include "nic_discovery.h"
#include "nic_registry.h"
#include "pci_io.h"
#include "scheduler.h"
#include "serial.h"
#include "service_supervisor.h"
#include "smp.h"
#include "syscall.h"
#include "user.h"
#include "user_init.h"
#include "vmm.h"
#include "initramfs.h"
#include "kernel_vfs.h"
#include "vfs.h"
#include "vfs_mount.h"

/*
 * ITEM-492: the machine's network controllers, described before anything is
 * installed and without asking a driver whether it is there.
 *
 * One `[NIC]` record per controller - the address, the ids, the driver this
 * image carries for it and the state - and one summary line.  A machine with no
 * controller reads `devices=0 ... absent=1`, which is a fact about the machine
 * and not a failure of the boot; a controller nobody wrote a driver for reads
 * `state=UNSUPPORTED reason=NO_DRIVER`, which is a fact about this image.  The
 * drivers are bound later, and the binding updates the record it belongs to.
 */
static void kernel_publish_nic_record(const char *text, size_t length)
{
    const uint64_t serial_flags = serial_sequence_begin();

    if (length == 0U) {
        return;
    }
    serial_write(text);
    serial_write("\n");
    serial_sequence_end(serial_flags);
    console_publish_journal(text, length);
}

static void kernel_publish_nic_capabilities(void)
{
    struct nic_device devices[NIC_DEVICES_MAX];
    enum nic_discovery_status status;
    char record[NIC_RECORD_BYTES];
    size_t count = 0U;
    size_t index;

    nic_backends_register_product();
    status = nic_discovery_scan(pci_io_kernel(), NULL, devices, NIC_DEVICES_MAX,
                                &count);
    if (status == NIC_DISCOVERY_OK) {
        for (index = 0U; index < count; ++index) {
            kernel_publish_nic_record(
                record, nic_device_record(&devices[index], record,
                                          sizeof(record)));
        }
    } else {
        kernel_publish_nic_record(
            record,
            nic_summary_record(NULL, 0U, record, sizeof(record)));
    }
    kernel_publish_nic_record(
        record,
        nic_summary_record(devices, status == NIC_DISCOVERY_OK ? count : 0U,
                           record, sizeof(record)));
}

#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST
extern void appliance_service_manager_qemu_test_init(void);
extern void appliance_https_service_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_QEMU_TEST
extern void appliance_production_service_https_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_QEMU_TEST
extern void appliance_production_management_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_QEMU_TEST
extern void appliance_first_boot_integration_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_QEMU_TEST
extern void appliance_recovery_factory_reset_integration_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_QEMU_TEST
extern void appliance_production_recovery_executor_qemu_test_init(
    const struct appliance_state_loader *loader,
    enum appliance_production_boot_readiness readiness);
#endif
#ifdef HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_QEMU_TEST
extern void appliance_service_failure_handler_qemu_test_init(
    uint64_t hhdm_offset);
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_QEMU_TEST
extern void appliance_persistent_service_config_qemu_test_init(
    uint64_t hhdm_offset);
#endif

#ifdef HFOC_BOOT_POLICY_QEMU_TEST
extern void boot_policy_qemu_test_init(void);
#endif
#ifdef HFOC_BLOCK_DEVICE_QEMU_TEST
extern void block_device_qemu_test_init(void);
#endif
#ifdef HFOC_DATA_REGION_QEMU_TEST
extern void data_region_qemu_test_init(void);
#endif
#ifdef HFOC_BLOCK_CACHE_QEMU_TEST
extern void block_cache_qemu_test_init(void);
#endif
#ifdef HFOC_CONFIG_JOURNAL_QEMU_TEST
extern void config_journal_qemu_test_init(void);
#endif
#ifdef HFOC_PHYSICAL_STORAGE_QEMU_TEST
extern void physical_storage_qemu_test_init(void);
extern void appliance_state_loader_qemu_test_init(void);
extern void ahci_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_HFOCFS_QEMU_TEST
extern void hfocfs_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_HFOCFS_WORKSPACE_QEMU_TEST
extern void hfocfs_workspace_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_PRODUCTION_INSTALLER_QEMU_TEST
extern void production_installer_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_CONFIG_QEMU_TEST
extern void appliance_persistent_config_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST
extern void appliance_network_config_qemu_test_init(void);
extern void appliance_network_diagnostics_qemu_test_init(void);
extern void appliance_dhcp_client_qemu_test_init(void);
extern void appliance_dns_resolver_qemu_test_init(void);
extern void appliance_network_lifecycle_qemu_test_init(void);
#endif
#ifdef HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST
extern void appliance_acme_directory_qemu_test_tick(uint32_t now_ms);
#endif
#ifdef HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST
extern void appliance_acme_order_qemu_test_tick(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_ms);
#endif
/*
 * ITEM-508 increment 29: the HTTP-01 wire proof's fixture token.  It is
 * declared here, and called next to the production listener's own poll, which
 * is what makes the listener and the arming hook share one clock on the
 * fixture boot.
 */
#ifdef HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST
extern void appliance_acme_http01_fixture_qemu_tick(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_epoch_seconds);
#endif
/*
 * ITEM-508 increment 31: the one wall clock of both ACME fixture families.
 * The two proofs have no SNTP peer, and the decision module refuses a zero
 * clock by name; each family names the same nonzero fixture epoch, and the
 * listener's `poll` below reads it in either family.  Production builds keep
 * the validated clock and never call this.
 */
#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST)
extern uint32_t appliance_acme_fixture_epoch_seconds(void);
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_TEST
extern void appliance_persistent_network_config_qemu_test_init(void);
#endif
#ifdef HFOC_BACKUP_RESTORE_QEMU_TEST
extern void backup_restore_qemu_test_init(void);
#endif
#ifdef HFOC_LOCAL_CONTROL_QEMU_TEST
extern void local_control_qemu_test_init(void);
#endif
#ifdef HFOC_NETWORK_FOUNDATION_QEMU_TEST
extern void network_foundation_qemu_test_init(void);
#endif
#ifdef HFOC_ARP_ICMP_QEMU_TEST
extern void arp_icmp_qemu_test_init(void);
#endif
#ifdef HFOC_VIRTIO_FOUNDATION_QEMU_TEST
extern void virtio_foundation_qemu_test_init(void);
#endif
#ifdef HFOC_VIRTIO_NET_FRAME_QEMU_TEST
extern void virtio_net_frame_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_TRUST_POLICY_QEMU_TEST
extern void trust_policy_qemu_test_init(void);
#endif
#ifdef HFOC_BOOT_CHAIN_QEMU_TEST
extern void boot_chain_qemu_test_init(void);
#endif
#ifdef HFOC_UPDATE_LIFECYCLE_QEMU_TEST
extern void update_lifecycle_qemu_test_init(void);
#endif
#ifdef HFOC_SUPPORTABILITY_QEMU_TEST
extern void supportability_qemu_test_init(void);
#endif
#ifdef HFOC_ACCOUNTING_BOUNDARY_QEMU_TEST
extern void accounting_boundary_qemu_test_init(void);
#endif
#ifdef HFOC_STORAGE_INTERRUPTION_QEMU_TEST
extern void storage_interruption_qemu_test_init(void);
#endif
#ifdef HFOC_SERVICE_CASCADE_DRILL_QEMU_TEST
extern void service_cascade_drill_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_LOCAL_CONTROL_OUTAGE_DRILL_QEMU_TEST
extern void local_control_outage_drill_qemu_test_init(void);
#endif
#ifdef HFOC_NETWORK_PARTITION_DRILL_QEMU_TEST
extern void network_partition_drill_qemu_test_init(void);
#endif
#ifdef HFOC_TRUST_FAILURE_DRILL_QEMU_TEST
extern void trust_failure_drill_qemu_test_init(void);
#endif
#ifdef HFOC_UPDATE_FAILURE_DRILL_QEMU_TEST
extern void update_failure_drill_qemu_test_init(void);
#endif
#ifdef HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_QEMU_TEST
extern void integration_replay_outage_drill_qemu_test_init(void);
#endif
#ifdef HFOC_POWER_CYCLE_CONTINUITY_DRILL_QEMU_TEST
extern void power_cycle_continuity_drill_qemu_test_init(void);
#endif
#ifdef HFOC_ENTROPY_QEMU_TEST
extern void entropy_qemu_test_init(uint64_t hhdm_offset);
#endif
#ifdef HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST
extern void appliance_certificate_name_fixture_seed(
    struct appliance_production_admin_enrollment *enrollment,
    struct appliance_state_loader *loader,
    uint64_t now_tick);
#endif

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .flags = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request
    executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/*
 * ITEM-372: the boot chain is measured from the bytes the loader actually
 * loaded.  The manifest and the loader configuration are declared as
 * modules in the signed media's Limine configuration; a medium without them
 * boots and publishes an UNAVAILABLE attestation instead of claiming a
 * verified chain.
 */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = NULL,
    .internal_module_count = 0,
    .internal_modules = NULL
};

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
/*
 * ITEM-394: only the installer medium asks for the firmware type, so the
 * appliance kernel's request set is unchanged.
 */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
    .response = NULL
};
#endif

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

static struct event_ring kernel_event_ring;
static struct appliance_production_boot production_boot;

/*
 * ITEM-519 increment 5: what the loader said about the file it loaded.  The
 * executable and the manifest modules live on the same medium as the kernel, so
 * the executable's own identity describes the medium the machine booted from:
 * the disk id and the partition index the loader read it from.  ITEM-520's
 * journal guard compares this with the bytes of a candidate device and never
 * writes to a device this did not name.
 *
 * This lives beside the loader request, outside every kernel variant's feature
 * conditionals, because the driver that probes the medium is compiled in the
 * fallback and oneshot kernels too and must always find the definition.
 */
bool install_medium_boot_identity(struct install_medium_boot_identity *out)
{
    const struct limine_file *executable = NULL;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (executable_file_request.response != NULL) {
        executable = executable_file_request.response->executable_file;
    }
    if (executable == NULL) {
        return false;
    }
    out->media_type = executable->media_type;
    out->partition_index = executable->partition_index;
    out->mbr_disk_id = executable->mbr_disk_id;
    out->mbr_disk_id_valid = executable->mbr_disk_id != 0U;
    return true;
}

#ifdef HFOC_PRODUCTION_NETWORK
/*
 * ITEM-447: the one commit function the machine's own menu uses.  It reaches
 * the same persistent record the boot applies, through the same setter the
 * install declaration uses, so a menu change and a medium declaration cannot
 * write different things.
 */
static bool console_network_commit(
    void *context,
    bool is_static,
    const struct network_ipv4_config *config) {
    struct appliance_persistent_network_config *record = context;
    const char *interface = APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE;
    const size_t interface_length = (size_t)strlen(interface);

    if (record == NULL) {
        return false;
    }
    if (is_static) {
        if (config == NULL) {
            return false;
        }
        return appliance_persistent_network_config_set_static(
                   record, interface, interface_length, config) ==
               APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    }
    return appliance_persistent_network_config_set_dhcp(
               record, interface, interface_length) ==
           APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

/*
 * ITEM-496: the reading the questions start from.  It is the wire's own
 * active configuration - what the interface has - not the record the menu is
 * about to replace: after a commit the two differ until the reboot, and a
 * record that printed the stored value as if it were running would be a
 * promise, not a reading.  `appliance_network_wire_active_config()` is the
 * same accessor the boot's own `[PRODUCTION-NET-MODE]` record reads.
 */
static size_t console_network_current_append(
    char *output,
    size_t capacity,
    size_t length,
    const char *text) {
    while (text != NULL && *text != '\0' && length + 1U < capacity) {
        output[length++] = *text++;
    }
    if (length < capacity) {
        output[length] = '\0';
    }
    return length;
}

static size_t console_network_current_append_u8(
    char *output,
    size_t capacity,
    size_t length,
    uint8_t value) {
    char digits[3U];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value = (uint8_t)(value / 10U);
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        char one[2U];

        one[0U] = digits[--count];
        one[1U] = '\0';
        length = console_network_current_append(
            output, capacity, length, one);
    }
    return length;
}

static bool console_network_current(
    void *context,
    char *output,
    size_t capacity,
    size_t *output_length) {
    const enum appliance_network_mode_action mode =
        appliance_production_network_wire_boot_mode();
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    const char *word = appliance_network_mode_action_string(mode);
    size_t length = 0U;

    (void)context;
    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    output[0] = '\0';
    if (mode == APPLIANCE_NETWORK_MODE_ACTION_NONE || word == NULL) {
        word = "UNAVAILABLE";
    }
    length = console_network_current_append(
        output, capacity, length, word);
    if (mode == APPLIANCE_NETWORK_MODE_ACTION_STATIC && active != NULL) {
        length = console_network_current_append(
            output, capacity, length, " address=");
        for (uint32_t octet = 0U; octet < NETWORK_IPV4_ADDRESS_OCTETS;
             ++octet) {
            if (octet != 0U) {
                length = console_network_current_append(
                    output, capacity, length, ".");
            }
            length = console_network_current_append_u8(
                output, capacity, length, active->address.octets[octet]);
        }
        length = console_network_current_append(
            output, capacity, length, "/");
        length = console_network_current_append_u8(
            output, capacity, length, active->prefix_length);
    }
    *output_length = length;
    return length != 0U;
}

static struct console_network_binding console_network_binding = {
    &production_boot.state.network_config,
    console_network_commit,
    console_network_current,
};
#endif

static struct vfs kernel_vfs;
static struct vfs_context kernel_vfs_context;
static struct vfs_mount_table kernel_vfs_mounts;
static bool kernel_vfs_ready;
/*
 * ITEM-635: the one production workspace.  The state buffer is the HFOCFS
 * allocator's bounded metadata arena; the image itself is the declared
 * partition range, never a host path.
 */
static unsigned char kernel_workspace_state[256U * 1024U]
    __attribute__((aligned(64)));
static struct hfocfs_workspace kernel_workspace;
#ifdef HFOC_PRODUCTION_TLS
/*
 * ITEM-386 ordinary-boot serving owners.  The request gate and the first-boot
 * certificate are owned here, beside the production boot coordinator, and the
 * serving composition borrows them.  The certificate is generated only when
 * the appliance has none, and the private key never leaves the mbedTLS
 * context and the appliance state volume.
 */
static struct appliance_production_auth production_serving_auth;
#ifdef HFOC_PRODUCTION_TLS
static struct appliance_production_ssh production_ssh;

static bool kernel_ssh_revoke(void *context) {
    return appliance_production_ssh_revoke(
        (struct appliance_production_ssh *)context);
}
#endif
static struct appliance_production_certificate production_serving_certificate;

/*
 * ITEM-508 increment 26: the ordinary-boot ACME service.  It borrows the
 * certificate owner's identity view, binds the persisted identity key as the
 * ACME account key as soon as the identity is prepared, and owns the renewal
 * policy the boot record and the panel read.  It opens no socket and speaks to
 * no CA; the order driver and the HTTP-01 listener are following increments.
 */
static struct appliance_production_acme_service production_acme_service;

/*
 * ITEM-508 increment 28: the HTTP-01 listener.  The ACME server fetches
 * `/.well-known/acme-challenge/<token>` over the wire while the order is
 * running, so the listener owns the second `hfoc_lwip_tcp` slot next to the
 * management listener on 443.  It is started once an active wire configuration
 * exists and comes up unarmed: until the order path arms a token every request
 * is the decision module's named refusal, and the record says `armed=0`.
 */
static struct appliance_production_acme_http01 production_acme_http01;

static bool production_acme_http01_started;

/* The last order record mirrored into the journal, so only a move logs. */
static bool last_production_acme_order_logged;
static char last_production_acme_order_record[
    APPLIANCE_PRODUCTION_ACME_ORDER_RECORD_MAX + 1U];

/*
 * ITEM-401 administrator credential enrollment owner.  It borrows the live
 * request gate and the loaded secret record; the local console commands
 * provision the record and publish the verifier, and the owner never keeps
 * the presented secret.
 */
static struct appliance_production_admin_enrollment production_admin_enrollment;

/*
 * ITEM-506: the account's salt comes from the hardware entropy owner the
 * management certificate already uses.  The callback returns true only when
 * the whole buffer was filled, so a machine whose entropy is not ready refuses
 * a credential change (NO_RANDOM) instead of inventing a salt.
 */
static bool kernel_admin_credential_random(
    void *context,
    uint8_t *output,
    size_t length)
{
    (void)context;
    return output != NULL && length != 0U &&
           appliance_production_entropy_random(NULL, output, length) == 0;
}

/*
 * ITEM-598: the ordinary local login is also the host-seed unlock.  Account
 * verification uses the same enrollment owner, failure counter and lockout as
 * the panel login; the password is then used only to authenticate the sealed
 * seed slot.  A missing/corrupt/wrong-machine store leaves the listener down.
 */
static struct console_entropy_login_result kernel_entropy_login_result(
    enum console_entropy_login_stage stage)
{
    struct console_entropy_login_result result;

    memset(&result, 0, sizeof(result));
    result.stage = stage;
    result.seed_status = APPLIANCE_HOST_SEED_UNSUPPORTED;
    return result;
}

/*
 * ITEM-614: the serial and journal copy of the finished attempt uses the same
 * renderer as the shared status report, and the attempt itself is kept by
 * kernel/src/kernel_login_trace.c for a snapshot composed later.
 */
static bool kernel_login_trace_serial_fragment(void *context,
                                               const char *fragment)
{
    (void)context;
    serial_write(fragment);
    console_record_text(fragment);
    return true;
}

static void kernel_login_trace_publish(void)
{
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    (void)kernel_login_trace_emit_state(kernel_login_trace_serial_fragment,
                                        NULL);
    serial_write("\n");
    console_record_text("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

static struct console_entropy_login_result kernel_production_entropy_unlock(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick)
{
    struct appliance_production_admin_enrollment *enrollment = context;
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const struct appliance_state_layout *layout;
    struct block_device *partition_device;
    struct appliance_production_admin_enrollment_verification verification;
    uint8_t seed[APPLIANCE_HOST_SEED_LENGTH];
    uint64_t epoch = 0U;
    enum appliance_production_admin_enrollment_status account_status;
    enum appliance_host_seed_status seed_status;
    enum appliance_production_entropy_seed_status entropy_status;
    struct console_entropy_login_result result =
        kernel_entropy_login_result(CONSOLE_ENTROPY_LOGIN_STAGE_SEED);
    struct kernel_login_trace trace;
    uint64_t stage_started;
    const uint64_t trace_started = timer_cycles();

    memset(&trace, 0, sizeof(trace));
    if (enrollment == NULL || loader == NULL) {
        result.seed_status = APPLIANCE_HOST_SEED_BAD_DEVICE;
        return result;
    }
    /* ITEM-614: a report taken while this attempt runs says IN_PROGRESS, not
       the previous attempt's success. */
    kernel_login_trace_begin();
    memset(&verification, 0, sizeof(verification));
    memset(seed, 0, sizeof(seed));
    /*
     * Prove the account without publishing the request gate or opening a
     * bearer session.  The gate must remain closed while the seed operation
     * can still fail.
     */
    result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT;
    stage_started = timer_cycles();
    account_status =
        appliance_production_admin_enrollment_verify_credential_pair(
            enrollment, login, login_length, password, password_length,
            now_tick, &verification);
    trace.account_cycles = timer_cycles() - stage_started;
    trace.account_iterations = verification.valid ? verification.iterations : 0U;
    result.account_status = account_status;
    if (account_status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        goto finish;
    }
    layout = appliance_state_loader_layout(loader);
    partition_device =
        (struct block_device *)appliance_state_loader_partition_device(loader);
    if (layout == NULL || partition_device == NULL) {
        result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_SEED;
        result.seed_status = APPLIANCE_HOST_SEED_BAD_DEVICE;
        goto finish;
    }
    result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_SEED;
    appliance_host_seed_accounting_begin(timer_cycles);
    stage_started = timer_cycles();
    seed_status = appliance_host_seed_store_unlock(
        partition_device, layout->block_capacity, password, password_length,
        seed, &epoch);
    trace.seed_cycles = timer_cycles() - stage_started;
    appliance_host_seed_accounting_read(&trace.seed);
    appliance_host_seed_accounting_end();
    result.seed_status = seed_status;
    if (seed_status != APPLIANCE_HOST_SEED_OK) {
        goto finish;
    }
    /*
     * The seed epoch is durable now.  Publish the already-proved account
     * without re-running the password KDF and without issuing a session.
     */
    result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT_PUBLISH;
    stage_started = timer_cycles();
    account_status =
        appliance_production_admin_enrollment_publish_verification(
            enrollment, &verification);
    trace.publish_cycles = timer_cycles() - stage_started;
    result.account_status = account_status;
    if (account_status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        goto finish;
    }
    result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_ENTROPY;
    stage_started = timer_cycles();
    entropy_status = appliance_production_entropy_seed_host(
        seed, sizeof(seed), epoch, (uint32_t)now_tick);
    trace.entropy_cycles = timer_cycles() - stage_started;
    result.entropy_status = entropy_status;
    if (entropy_status == APPLIANCE_PRODUCTION_ENTROPY_SEED_OK ||
        entropy_status == APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY) {
        result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE;
        result.account_status = APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
        result.seed_status = APPLIANCE_HOST_SEED_OK;
    }

finish:
    trace.ok = result.stage == CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE;
    trace.stage = (uint32_t)result.stage;
    trace.total_cycles = timer_cycles() - trace_started;
    kernel_login_trace_commit(&trace);
    kernel_login_trace_publish();
    appliance_production_admin_enrollment_verification_clear(&verification);
    memset(seed, 0, sizeof(seed));
    return result;
}

static struct console_entropy_login_binding production_entropy_login_binding = {
    &production_admin_enrollment,
    kernel_production_entropy_unlock,
};

static bool kernel_production_host_seed_present(void)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const struct appliance_state_layout *layout;
    struct block_device *partition_device;
    bool present = false;

    if (loader == NULL) {
        return false;
    }
    layout = appliance_state_loader_layout(loader);
    partition_device =
        (struct block_device *)appliance_state_loader_partition_device(loader);
    return layout != NULL && partition_device != NULL &&
           appliance_host_seed_store_present(
               partition_device, layout->block_capacity, &present) ==
               APPLIANCE_HOST_SEED_OK &&
           present;
}

static bool kernel_production_host_seed_rekey(
    void *context,
    const char *current_password,
    size_t current_password_length,
    const char *new_password,
    size_t new_password_length)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const struct appliance_state_layout *layout;
    struct block_device *partition_device;
    bool present = false;

    (void)context;
    if (loader == NULL) {
        return false;
    }
    layout = appliance_state_loader_layout(loader);
    partition_device =
        (struct block_device *)appliance_state_loader_partition_device(loader);
    if (layout == NULL || partition_device == NULL ||
        appliance_host_seed_store_present(
            partition_device, layout->block_capacity, &present) !=
            APPLIANCE_HOST_SEED_OK) {
        return false;
    }
    if (!present) {
        return true;
    }
    return appliance_host_seed_store_rekey(
               partition_device, layout->block_capacity,
               current_password, current_password_length,
               new_password, new_password_length) ==
           APPLIANCE_HOST_SEED_OK;
}

static void log_production_admin_enrollment(void)
{
    char record[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
    size_t record_length = 0U;
    const uint64_t serial_flags = serial_sequence_begin();

    if (appliance_production_admin_enrollment_render(
            &production_admin_enrollment, record, sizeof(record),
            &record_length) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        serial_write(record);
    } else {
        serial_write("[ADMIN-ENROLLMENT] state=UNAVAILABLE");
    }
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-516: the console's durable gate binding.
 *
 * `gate open confirm` and `gate close` are the only console commands that
 * write, and the one thing they may write is the system record's
 * `v1:security.panel.after-boot` line.  These two callbacks are the console's
 * whole access to it: the read answers the record's setting (and refuses when
 * the record cannot be read), the write commits the line through the same
 * setter the boot reads.  They are bound in `publish_production_panel_gate()`
 * only when the record exists, so a machine without one answers that the
 * setting is unavailable rather than claiming a write.
 */
static bool production_panel_gate_read(void *context, bool *open_out)
{
    struct appliance_persistent_config *config = context;
    bool open = false;

    if (config == NULL || open_out == NULL ||
        appliance_persistent_config_panel_after_boot(config, &open) !=
            APPLIANCE_PERSISTENT_CONFIG_OK) {
        return false;
    }
    *open_out = open;
    return true;
}

static bool production_panel_gate_write(void *context, bool open)
{
    struct appliance_persistent_config *config = context;

    return config != NULL &&
           appliance_persistent_config_set_panel_after_boot(config, open) ==
               APPLIANCE_PERSISTENT_CONFIG_OK;
}

static struct console_gate_binding production_panel_gate_binding;

/*
 * ITEM-516: the boot-time gate decision.
 *
 * The durable record decides.  With `v1:security.panel.after-boot=open` the
 * boot publishes the stored credential itself (source=POLICY) and names the
 * fact; with the default `closed` - or a record whose credential cannot be
 * published - the listener is withheld exactly as ITEM-401 has always
 * withheld it, and the record says so with the reason.  The one line is what
 * the acceptance report reads, so it is written whether the policy opened the
 * gate or not.
 */
static void publish_production_panel_gate(void)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const char *record;
    bool open = false;
    bool durable = false;
    bool published = false;
    uint64_t serial_flags;

    if (loader != NULL &&
        appliance_persistent_config_panel_after_boot(
            &loader->system_config, &open) ==
            APPLIANCE_PERSISTENT_CONFIG_OK) {
        durable = true;
        if (open) {
            published =
                appliance_production_admin_enrollment_publish_policy(
                    &production_admin_enrollment) ==
                APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
        }
    }
    /*
     * ITEM-516: the console's own gate binding.  The durable record is the
     * only thing `gate open`/`gate close` may write, so the console is bound
     * to the loader's system record here, where the boot knows whether that
     * record exists at all.  A boot with no readable record binds nothing:
     * the console then answers that the setting is unavailable instead of
     * pretending a write happened.  The binding is published with the state
     * the record carries, so `gate` names the setting in force.
     */
    if (durable) {
        production_panel_gate_binding.context =
            &production_boot.state.system_config;
        production_panel_gate_binding.read = production_panel_gate_read;
        production_panel_gate_binding.write = production_panel_gate_write;
        console_bind_gate(&production_panel_gate_binding);
    } else {
        console_bind_gate(NULL);
    }
    console_publish_gate(open, durable);
    record = appliance_panel_gate_record(
        published ? APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED
                  : APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED);
    if (record == NULL) {
        return;
    }
    serial_flags = serial_sequence_begin();
    console_journal_begin();
    serial_write(record);
    console_record_text(record);
    serial_write("\n");
    console_journal_end();
    serial_sequence_end(serial_flags);
}

/*
 * ITEM-400 ordinary-boot read-only management snapshot owner.  Ordinary boot
 * assembles the live evidence from the ITEM-360 boot coordinator, the
 * ITEM-362 network wire and the ITEM-364 service/HTTPS activation, and
 * republishes it from the event loop so link, lease and listener changes stay
 * visible.  Nothing here fabricates a PASS: a missing owner is published as
 * UNAVAILABLE and an awaiting one as DEGRADED by the snapshot module.
 */
static struct appliance_production_management_api production_management_api;
static char production_management_hostname[
    APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];

static enum appliance_network_api_config_source
production_management_network_source(
    const struct appliance_persistent_network_config *config)
{
    switch (config != NULL
                ? appliance_persistent_network_config_source(config)
                : APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE) {
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED:
        return APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED;
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING:
        return APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING;
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID:
        return APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_INVALID;
    default:
        return APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE;
    }
}

static void publish_production_management_evidence(void)
{
    struct appliance_production_management_api_evidence evidence;
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    const struct network_device_snapshot *device =
        network != NULL ? appliance_production_network_device(network)
                        : NULL;
    const struct appliance_production_service_activation *activation =
        appliance_production_service_boot_activation();
    const struct appliance_dhcp_client *dhcp =
        wire != NULL ? appliance_network_wire_dhcp(wire) : NULL;
    size_t hostname_length = 0U;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION;
    evidence.identity = appliance_identity_get();
    evidence.boot_readiness =
        appliance_production_boot_readiness(&production_boot);
    evidence.boot_mode = appliance_production_boot_mode(&production_boot);
    evidence.storage_ready =
        loader != NULL && appliance_state_loader_ready(loader);
    evidence.config_source =
        loader != NULL
            ? appliance_persistent_config_source(&loader->system_config)
            : APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE;
    if (loader != NULL &&
        appliance_persistent_config_hostname(
            &loader->system_config,
            production_management_hostname,
            sizeof(production_management_hostname),
            &hostname_length) == APPLIANCE_PERSISTENT_CONFIG_OK) {
        evidence.config_hostname = production_management_hostname;
        evidence.config_hostname_length = hostname_length;
    }
    /*
     * The system snapshot requires a hostname for every configuration state
     * but UNAVAILABLE.  A committed or default configuration without a
     * readable hostname therefore falls back to the effective ITEM-302
     * identity hostname instead of turning the whole target into a 503.
     */
    if (evidence.config_hostname_length == 0U) {
        evidence.config_hostname = NULL;
    }
    if (evidence.config_hostname == NULL &&
        evidence.config_source != APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE &&
        evidence.identity != NULL) {
        size_t identity_length = 0U;

        while (identity_length < APPLIANCE_IDENTITY_FIELD_MAX &&
               evidence.identity->hostname[identity_length] != '\0') {
            ++identity_length;
        }
        evidence.config_hostname = evidence.identity->hostname;
        evidence.config_hostname_length = identity_length;
    }
    if (loader != NULL) {
        const struct appliance_persistent_network_config *network_config =
            appliance_state_loader_network_config(loader);

        evidence.network_desired =
            network_config != NULL
                ? appliance_persistent_network_config_current(network_config)
                : NULL;
        evidence.network_config_source =
            production_management_network_source(network_config);
    }
    evidence.network_active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    evidence.network_device = device;
    evidence.network_link_up =
        appliance_production_network_boot_link_up();
    evidence.gateway_reachable =
        dhcp != NULL && appliance_dhcp_client_lease_published(dhcp);
    evidence.dns_reachable =
        wire != NULL && appliance_network_wire_dns_answer(wire) != NULL;
    evidence.https_listening = appliance_production_serving_listening();
    evidence.service_present = activation != NULL;
    evidence.service_state =
        activation != NULL
            ? appliance_production_service_activation_state(activation)
            : APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_STATE_NONE;
    evidence.uptime_ticks = (uint64_t)hfoc_lwip_time_now();
    (void)appliance_production_management_api_publish(
        &production_management_api, &evidence);
}

/*
 * ITEM-505 increment 2: the live view the panel's generic sections render.
 *
 * The owners are the ones the appliance already publishes: the loaded
 * configuration's hostname (with the ITEM-302 identity as the same fallback
 * the management snapshot uses), the production network composition's own MAC
 * reading, the one service the management listener serves, and the serving
 * path's state with its reason.  A missing owner stays NULL and the panel
 * renders `unavailable`; nothing here invents a value the machine did not
 * publish.  The recent-log line is the serving path's own bounded
 * `[PRODUCTION-SERVING]` panel line, so the field can never grow a section
 * page past its recorded budget.
 */
static struct appliance_production_panel_view_storage
    production_panel_view_storage;
static char production_panel_line[APPLIANCE_PRODUCTION_SERVING_PANEL_LINE_MAX];

static const char *production_panel_hostname(void)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    size_t hostname_length = 0U;

    if (loader != NULL &&
        appliance_persistent_config_hostname(
            &loader->system_config,
            production_management_hostname,
            sizeof(production_management_hostname),
            &hostname_length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
        hostname_length != 0U) {
        return production_management_hostname;
    }
    /*
     * The identity fallback applies only where the management snapshot applies
     * it: a machine with no configuration state at all publishes no hostname
     * rather than a name from an owner it did not read.
     */
    if (appliance_persistent_config_source(
            loader != NULL ? &loader->system_config : NULL) !=
        APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        const struct appliance_identity *identity = appliance_identity_get();

        if (identity != NULL && identity->hostname[0] != '\0') {
            return identity->hostname;
        }
    }
    return NULL;
}

static void publish_production_panel_view(void)
{
    struct appliance_production_panel_view_owners owners;
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    const struct appliance_production_service_activation *activation =
        appliance_production_service_boot_activation();
    uint32_t line_length = 0U;

    memset(&owners, 0, sizeof(owners));
    owners.version = APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION;
    owners.hostname = production_panel_hostname();
    owners.mac_address =
        network != NULL ? appliance_production_network_mac(network) : NULL;
    owners.service_name = activation != NULL ? HTTPS_SERVICE_NAME : NULL;
    owners.serving_state = appliance_production_serving_state_string(
        appliance_production_serving_state());
    owners.serving_reason = appliance_production_serving_reason();
    appliance_production_serving_render_panel_line(
        production_panel_line, (uint32_t)sizeof(production_panel_line),
        &line_length);
    owners.recent_log = line_length != 0U ? production_panel_line : NULL;

    if (appliance_production_panel_view_compose(
            &owners, &production_panel_view_storage) !=
        APPLIANCE_PRODUCTION_PANEL_VIEW_OK) {
        return;
    }
    (void)appliance_production_serving_publish_panel_view(
        &production_panel_view_storage.view);
}
#endif

struct event_ring *kernel_event_ring_get(void) {
    return &kernel_event_ring;
}

/*
 * ITEM-410: the appliance boot log.
 *
 * The console is the funnel every published record passes through, so it is
 * bound here to the durable log before the first record is published: the log
 * stages what the console accepted in a bounded RAM ring and writes that ring
 * into a preallocated file on the appliance's own ESP at the moments that
 * matter.  Nothing here can fail a boot - the log publishes
 * `[BOOTLOG] state=UNAVAILABLE reason=...` instead, and the machine boots on.
 */
static uint64_t kernel_boot_log_ticks(void) {
    return timer_ticks();
}

/*
 * The `[BOOTLOG]` records this module publishes are records like every other
 * one this kernel owns: written to the serial stream first, and handed to the
 * console's journal afterwards.  The journal is what hands the record back to
 * the log (the boot log observes the funnel), so the log records its own state
 * without a second write path.
 *
 * ITEM-461: unlike the other records, a `[BOOTLOG]` record is NOT painted.  It
 * is the log's own bookkeeping - a state line per flush - and on the owner's
 * 2026-09-20 screen it was the flood that scrolled the console continuously:
 * `[BOOTLOG] state=READY ...` printed after every record, which both pushed
 * the honest records off the screen and kept the main loop painting.  The
 * record is still stored, so the next boot reads it from the disk.
 */
static void kernel_boot_log_publish(const char *record, size_t length) {
    const uint64_t serial_flags = serial_sequence_begin();
    size_t index;

    for (index = 0U; index < length; ++index) {
        serial_putc(record[index]);
    }
    serial_putc('\n');
    serial_sequence_end(serial_flags);
    console_publish_journal(record, length);
}

/*
 * ITEM-581: the interactive `bootlog` reading is painted and mirrored to the
 * serial stream, but it is not published to the console journal: the journal
 * is what hands records back to the log, and a reading that fed itself back
 * would either grow the log it is reading or be lost behind the funnel's own
 * dedup.
 */
static void kernel_boot_log_display(const char *text) {
    size_t index;

    if (text == NULL) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        serial_putc(text[index]);
    }
    console_write(text);
}

static void kernel_boot_log_boot_init(void) {
    const struct appliance_boot_log_store_sink sink = {
        kernel_boot_log_publish,
        kernel_boot_log_display,
        kernel_boot_log_ticks,
        /*
         * ITEM-521 (F-A of item-521-wiring-audit-2026-10-12.md): the install
         * journal on the medium subscribes to the store's *admitted* records,
         * not to the console funnel.  The funnel republishes an unchanged
         * state on every poll and the store's policy - the repeated-record
         * dedup and the periodic-family gate - is what keeps a bounded log a
         * history of the boot; a journal wired to the funnel itself would fill
         * with heartbeats and push the machine announcement and the observed
         * storage out of its ring, which is exactly what requirement 1 names.
         * The platform's init runs before the store can observe anything, so
         * the first admitted record already finds the journal instance ready.
         */
        install_journal_platform_observe
    };
    const struct console_boot_log_binding binding = {
        appliance_boot_log_store_observe,
        appliance_boot_log_store_dump
    };

    install_journal_platform_init();
    if (appliance_boot_log_store_init(&sink) ==
        APPLIANCE_BOOT_LOG_STORE_OK) {
        console_bind_boot_log(&binding);
    }
    /*
     * ITEM-521 requirement 3 (F-B): a medium that cannot carry the journal is
     * not a failed run.  An optical boot is read-only for all three ITEM-520
     * placements, so the journal is refused before any placement is tried and
     * the refusal is named once in the ordinary records - the serial stream and
     * the durable log that was just bound - rather than left for the medium to
     * discover.
     */
    {
        struct install_medium_boot_identity medium_identity;

        if (install_medium_boot_identity(&medium_identity) &&
            medium_identity.media_type == INSTALL_MEDIUM_MEDIA_OPTICAL) {
            (void)install_journal_platform_refuse("READ_ONLY");
        }
    }
}

/*
 * Find the appliance's own ESP on the disks this boot selected.  Every
 * candidate is probed read-only first: the log attaches to an ESP that carries
 * this install's own sealed boot manifest, and a disk that is not ours - a data
 * disk, another system's ESP - is left exactly as it is found.  The installer
 * medium never takes this path: the medium is not the declared target, so the
 * install flow attaches the log to the disk it installed to instead.
 */
static void kernel_boot_log_attach_disks(void) {
    const size_t disk_count = appliance_production_storage_disk_count();
    size_t index;

    for (index = 0U; index < disk_count; ++index) {
        struct appliance_production_storage_disk disk;

        if (!appliance_production_storage_disk_info(index, &disk) ||
            !disk.present || disk.device == NULL) {
            continue;
        }
        if (appliance_boot_log_store_attach_disk(disk.device, "boot") ==
            APPLIANCE_BOOT_LOG_STORE_OK) {
            return;
        }
    }
}

static void log_bootloader(void) {
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /*
         * ITEM-410: the boot record is mirrored like the records ITEM-405
         * already mirrors, and the boot log stores it.  It is published before
         * the banner, so the console buffers it and paints it once the screen
         * is up - and a hardware run that has no serial capture can read which
         * loader started the machine.
         */
        console_journal_begin();
        serial_write("[BOOT] Bootloader: ");
        console_record_text("[BOOT] Bootloader: ");
        if (bootloader_request.response != NULL) {
            serial_write(bootloader_request.response->name);
            console_record_text(bootloader_request.response->name);
            serial_write(" ");
            console_record_text(" ");
            serial_write(bootloader_request.response->version);
            console_record_text(bootloader_request.response->version);
        } else {
            serial_write("unknown");
            console_record_text("unknown");
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

static void log_pic_masks(void) {
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PIC ] Master mask: ");
        serial_write_hex(pic_master_mask());
        serial_write("; slave mask: ");
        serial_write_hex(pic_slave_mask());
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static const char *production_boot_config_source(
    const struct appliance_production_boot *boot)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(boot);

    if (loader == NULL) {
        return "UNAVAILABLE";
    }
    switch (loader->system_config.values.source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE:
    default:
        return "UNAVAILABLE";
    }
}

static struct boot_measurements boot_measurements_state;
static struct boot_attestation boot_attestation_state;

static bool boot_chain_path_ends_with(const char *path,
                                      const char *suffix)
{
    size_t path_length;
    size_t suffix_length;
    size_t index;

    if (path == NULL || suffix == NULL) {
        return false;
    }
    path_length = 0U;
    while (path[path_length] != '\0') {
        ++path_length;
    }
    suffix_length = 0U;
    while (suffix[suffix_length] != '\0') {
        ++suffix_length;
    }
    if (suffix_length > path_length) {
        return false;
    }
    for (index = 0U; index < suffix_length; ++index) {
        if (path[path_length - suffix_length + index] != suffix[index]) {
            return false;
        }
    }
    return true;
}

static void boot_chain_collect(struct boot_chain_boot_source *source,
                               struct boot_chain_component components[3])
{
    const struct limine_file *executable = NULL;
    const struct limine_file *manifest = NULL;
    const struct limine_file *config = NULL;
    uint64_t index;

    memset(source, 0, sizeof(*source));
    memset(components, 0, sizeof(struct boot_chain_component) * 3U);

    if (executable_file_request.response != NULL) {
        executable = executable_file_request.response->executable_file;
    }
    if (module_request.response != NULL) {
        for (index = 0U;
             index < module_request.response->module_count; ++index) {
            const struct limine_file *module =
                module_request.response->modules[index];

            if (module == NULL) {
                continue;
            }
            if (boot_chain_path_ends_with(module->path,
                                          "hfoc-boot.man")) {
                manifest = module;
            } else if (boot_chain_path_ends_with(module->path,
                                                 "limine.conf")) {
                config = module;
            }
        }
    }

    if (executable != NULL && executable->address != NULL &&
        executable->size != 0U) {
        components[source->component_count].kind =
            BOOT_CHAIN_COMPONENT_KERNEL;
        components[source->component_count].data = executable->address;
        components[source->component_count].length =
            (size_t)executable->size;
        ++source->component_count;
    }
    if (config != NULL && config->address != NULL && config->size != 0U) {
        components[source->component_count].kind =
            BOOT_CHAIN_COMPONENT_LOADER_CONFIG;
        components[source->component_count].data = config->address;
        components[source->component_count].length = (size_t)config->size;
        ++source->component_count;
    }
    if (manifest != NULL && manifest->address != NULL &&
        manifest->size != 0U) {
        source->manifest = manifest->address;
        source->manifest_length = (size_t)manifest->size;
    }
    source->components = components;
}

static void boot_chain_run(void)
{
    struct boot_chain_boot_source source;
    struct boot_chain_component components[3];

    boot_chain_collect(&source, components);
    (void)boot_chain_boot_run(&source, &boot_measurements_state,
                              &boot_attestation_state);

    {
        const uint64_t serial_flags = serial_sequence_begin();
        unsigned char aggregate[SHA256_DIGEST_SIZE];
        uint64_t prefix = 0U;
        unsigned int index;

        /*
         * ITEM-410: the boot chain record is the one the FIRST_BOOT stage of
         * the hardware run quotes, so it is mirrored and stored like the other
         * records an operator needs - the attestation verdict used to reach the
         * serial stream alone, which is the stream the acceptance host cannot
         * capture.
         */
        console_journal_begin();
        serial_write("[BOOT-CHAIN] verdict=");
        console_record_text("[BOOT-CHAIN] verdict=");
        serial_write(boot_attestation_verdict_string(
            boot_attestation_state.verdict));
        console_record_text(boot_attestation_verdict_string(
            boot_attestation_state.verdict));
        serial_write(" reason=");
        console_record_text(" reason=");
        serial_write(boot_attestation_reason_string(
            boot_attestation_state.reason));
        console_record_text(boot_attestation_reason_string(
            boot_attestation_state.reason));
        serial_write(" release=");
        console_record_text(" release=");
        serial_write_u64(boot_attestation_state.release_id);
        console_record_u64(boot_attestation_state.release_id);
        serial_write(" signer=");
        console_record_text(" signer=");
        serial_write_u64(boot_attestation_state.signer_id);
        console_record_u64(boot_attestation_state.signer_id);
        serial_write(" components=");
        console_record_text(" components=");
        serial_write_u64(
            (uint64_t)boot_attestation_state.component_count);
        console_record_u64(
            (uint64_t)boot_attestation_state.component_count);
        serial_write(" chain=");
        console_record_text(" chain=");
        for (index = 0U; index < 8U; ++index) {
            prefix = (prefix << 8U) |
                     boot_attestation_state.chain_digest[index];
        }
        serial_write_hex(prefix);
        console_record_hex64(prefix);
        if (boot_measurements_aggregate(&boot_measurements_state,
                                        aggregate) ==
            BOOT_MEASUREMENTS_OK) {
            prefix = 0U;
            for (index = 0U; index < 8U; ++index) {
                prefix = (prefix << 8U) | aggregate[index];
            }
            serial_write(" measurements=");
            console_record_text(" measurements=");
            serial_write_hex(prefix);
            console_record_hex64(prefix);
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
/*
 * ITEM-394 product install run on the installer medium.
 *
 * The medium declares its installer payloads as Limine modules and selects the
 * install entry on its command line; an ordinary appliance medium declares
 * none of them, so the flow is never reached there.  Nothing is written from
 * this function: it hands the collected evidence to the product flow, which
 * decides, publishes and only then installs.
 */
static const struct limine_file *production_install_media_module(
    const char *suffix)
{
    uint64_t index;

    if (module_request.response == NULL || suffix == NULL) {
        return NULL;
    }
    for (index = 0U; index < module_request.response->module_count;
         ++index) {
        const struct limine_file *module =
            module_request.response->modules[index];

        if (module != NULL && module->path != NULL &&
            module->address != NULL && module->size != 0U &&
            boot_chain_path_ends_with(module->path, suffix)) {
            return module;
        }
    }
    return NULL;
}

static enum installer_firmware_mode production_install_media_firmware(void)
{
    if (firmware_type_request.response == NULL) {
        return INSTALLER_FIRMWARE_UNKNOWN;
    }
    switch (firmware_type_request.response->firmware_type) {
    case LIMINE_FIRMWARE_TYPE_EFI32:
    case LIMINE_FIRMWARE_TYPE_EFI64:
        return INSTALLER_FIRMWARE_UEFI;
    case LIMINE_FIRMWARE_TYPE_X86BIOS:
        return INSTALLER_FIRMWARE_BIOS_LEGACY;
    default:
        return INSTALLER_FIRMWARE_UNKNOWN;
    }
}

/*
 * ITEM-509 increment 27 (509.7c-2): the language this boot's medium declares.
 *
 * The declaration is parsed once, before the first installer screen is drawn,
 * into this boot's copy; the screens that run before the flow owns the
 * declaration (the foundation screen) read it here, and `publish_console_language()`
 * reads it after the flow returned.  The parse is the flow's own parser, so a
 * declaration the flow refuses leaves the default language in force - the
 * refusal, not a half-declared screen, is what the operator sees.
 *
 * `known` is set only for an entry that installs (`hfoc.install=unattended` or
 * `hfoc.install=console`): the appliance entry of the same medium keeps reading
 * the machine's own record, which is the behaviour it had before the directive.
 */
static struct production_install_media_declaration
    kernel_installer_declaration;
static bool kernel_installer_language_known;

static enum appliance_language kernel_installer_declared_language(void)
{
    return kernel_installer_language_known
               ? kernel_installer_declaration.language
               : APPLIANCE_LANGUAGE_DEFAULT;
}

static void kernel_installer_language_prepare(
    const struct production_install_flow_media *media)
{
    enum production_install_media_mode mode = PRODUCTION_INSTALL_MEDIA_MODE_NONE;

    kernel_installer_language_known = false;
    memset(&kernel_installer_declaration, 0,
           sizeof(kernel_installer_declaration));
    if (media == NULL ||
        production_install_media_declaration_parse(
            media->declaration, media->declaration_length,
            &kernel_installer_declaration) != PRODUCTION_INSTALL_MEDIA_OK ||
        production_install_media_mode_parse(
            media->command_line, media->command_line_length, &mode) !=
            PRODUCTION_INSTALL_MEDIA_OK ||
        mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE) {
        return;
    }
    kernel_installer_language_known = true;
}

static void production_install_media_boot(uint64_t usable_mib)
{
    const struct limine_file *executable = NULL;
    const struct limine_file *declaration;
    const struct limine_file *boot_payload;
    const struct limine_file *installed_kernel;
    const struct limine_file *installed_config;
    const struct limine_file *installed_chain;
    const struct limine_file *rootfs;
    const struct limine_file *envelope;
    const struct limine_file *payload_blob;
    const struct limine_file *host_seed;
    const struct limine_file *ssh_key;
    struct production_install_flow_media media;

    if (executable_file_request.response != NULL) {
        executable = executable_file_request.response->executable_file;
    }
    if (executable == NULL || executable->address == NULL ||
        executable->size == 0U) {
        return;
    }
    declaration = production_install_media_module("hfoc-install.token");
    boot_payload = production_install_media_module("BOOTX64.EFI");
    /*
     * ITEM-399: the kernel the install stages is the medium's *installed*
     * kernel module, not this running kernel.  Before this item the flow
     * staged `executable` - the installer build, carrying the install flow,
     * the QEMU fixture and the installer macro - so every installed appliance
     * booted an installer and published `[PRODUCTION-INSTALLER] proof OK`.
     * The medium now carries the product kernel at this path and the installer
     * boots from boot/hfoc-installer-kernel.elf.
     *
     * Limine reports a module's path with its resource prefix resolved: the
     * medium declares `module_path: boot():/boot/hfoc-kernel.elf` and the
     * response carries `/boot/hfoc-kernel.elf`, so the match is on the
     * volume-relative path.  It cannot be confused with the installer kernel -
     * `/boot/hfoc-installer-kernel.elf` does not end in `boot/hfoc-kernel.elf`
     * - and `scripts/hfoc-install-media-stage.sh` refuses a medium whose two
     * kernels are the same file.
     */
    installed_kernel =
        production_install_media_module("boot/hfoc-kernel.elf");
    /*
     * ITEM-397: the medium carries the *installed* system's boot set - the
     * configuration whose paths resolve on the installed ESP, and the sealed
     * boot chain of the kernel it names - and the flow stages both.  The
     * loader the removable-media path needs is the same Limine UEFI binary
     * the medium boots from.
     */
    installed_config =
        production_install_media_module("hfoc-installed.conf");
    installed_chain =
        production_install_media_module("hfoc-installed.chain");
    rootfs = production_install_media_module("hfoc-rootfs.arc");
    envelope = production_install_media_module("hfoc-install.man");
    payload_blob = production_install_media_module("hfoc-install.payload");
    host_seed = production_install_media_module("hfoc-seed.bin");
    ssh_key = production_install_media_module("hfoc-ssh-key.point");

    memset(&media, 0, sizeof(media));
    media.version = PRODUCTION_INSTALL_FLOW_VERSION;
    if (executable->string != NULL) {
        media.command_line = executable->string;
        media.command_line_length = strlen(executable->string);
    }
    if (declaration != NULL) {
        media.declaration = declaration->address;
        media.declaration_length = (size_t)declaration->size;
    }
    media.firmware_mode = production_install_media_firmware();
    media.cpu_cores =
        mp_request.response != NULL &&
                mp_request.response->cpu_count != 0U
            ? (uint32_t)mp_request.response->cpu_count
            : 1U;
    media.ram_bytes = usable_mib * UINT64_C(1048576);
    if (boot_payload != NULL) {
        media.boot_payload = boot_payload->address;
        media.boot_payload_size = (size_t)boot_payload->size;
    }
    if (installed_config != NULL) {
        media.limine_config = installed_config->address;
        media.limine_config_size = (size_t)installed_config->size;
    }
    if (installed_chain != NULL) {
        media.manifest = installed_chain->address;
        media.manifest_size = (size_t)installed_chain->size;
    }
    if (rootfs != NULL) {
        media.rootfs = rootfs->address;
        media.rootfs_size = (size_t)rootfs->size;
    }
    media.loader = media.boot_payload;
    media.loader_size = media.boot_payload_size;
    if (installed_kernel != NULL) {
        media.kernel = installed_kernel->address;
        media.kernel_size = (size_t)installed_kernel->size;
    }
    if (envelope != NULL) {
        media.envelope = envelope->address;
        media.envelope_size = (size_t)envelope->size;
    }
    if (payload_blob != NULL) {
        media.payload_blob = payload_blob->address;
        media.payload_blob_size = (size_t)payload_blob->size;
    }
    if (host_seed != NULL) {
        media.host_seed = host_seed->address;
        media.host_seed_size = (size_t)host_seed->size;
    }
    if (ssh_key != NULL) {
        media.ssh_key = ssh_key->address;
        media.ssh_key_size = (size_t)ssh_key->size;
    }
    /*
     * ITEM-488: the installer's own screen, drawn through the display
     * abstraction - on the firmware's framebuffer when this machine has one it
     * can write, and as the same lines on the console and the serial stream
     * when it does not.
     */
    /*
     * ITEM-509 increment 27 (509.7c-2): the language of every installer screen
     * comes from the medium's declaration, parsed before the first screen is
     * drawn.  The renderer chain already takes it as an explicit parameter
     * (increments 14-19); this boot is where that parameter finally gets its
     * source.  A medium built before the directive declares nothing and keeps
     * the default language, so every existing screen stays byte for byte.
     */
    kernel_installer_language_prepare(&media);
    display_show_installer_foundation(kernel_installer_declared_language());
    production_install_flow_run(&media);
}

/*
 * ITEM-490: the operator entry's storage screen.
 *
 * The install flow runs before this machine has a console and before interrupts
 * are enabled, so it publishes what it prepared and returns; this is the first
 * place in the boot where local input is live, and it is where the operator
 * names the disk.  The screen waits with no deadline - the entry that opened it
 * is the entry a person chose - and it is also where the person confirms:
 * ITEM-495 draws the warning page for the chosen disk, and ENTER on that page is
 * the confirmation the flow applies.  The console token of ITEM-394 is gone;
 * leaving the screen without accepting the warning refuses the run and writes
 * nothing.
 */
/*
 * ITEM-506 increment 4: the operator entry's account screen and its salt.
 *
 * The installer kernel has no hardware entropy owner (the TLS-gated owner is
 * not compiled here), and a salt has to be unique per stored verifier, not
 * secret: this derivation hashes a domain string, the identity of the target
 * disk the operator chose (the same identity the plan writes by, empty when
 * the hardware reported none) and the boot-relative tick count with a per-run
 * counter.  The owner's 2026-10-03 decision names exactly those inputs, so two
 * sets in one boot cannot collide and two machines do not derive the same salt
 * from the same tick by accident.  The console path keeps using the hardware
 * entropy owner; this seam exists so the operator's install can set a real
 * PBKDF2 record instead of inventing a fixed salt.
 */
struct kernel_installer_credential_salt_context {
    char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH];
};

static struct kernel_installer_credential_salt_context
    kernel_installer_credential_salt_context;

static bool kernel_installer_credential_salt(
    void *context,
    uint8_t *output,
    size_t length)
{
    static const char domain[] = "HFOC-INSTALL-ADMIN-SALT-V1:";
    static uint32_t salt_counter;
    const struct kernel_installer_credential_salt_context *salt_context =
        context;
    struct sha256 sha;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t material[16];
    const uint64_t ticks = timer_ticks();
    size_t identity_length = 0U;
    size_t index;

    if (output == NULL || length == 0U || length > sizeof(digest)) {
        return false;
    }
    if (salt_context != NULL) {
        while (identity_length < sizeof(salt_context->identity) &&
               salt_context->identity[identity_length] != '\0') {
            ++identity_length;
        }
    }
    memset(material, 0, sizeof(material));
    for (index = 0U; index < 8U; ++index) {
        material[index] = (uint8_t)(ticks >> (8U * index));
    }
    ++salt_counter;
    for (index = 0U; index < 4U; ++index) {
        material[8U + index] = (uint8_t)(salt_counter >> (8U * index));
    }
    memset(digest, 0, sizeof(digest));
    memset(&sha, 0, sizeof(sha));
    if (sha256_init(&sha) != SHA256_OK ||
        sha256_update(&sha, domain, sizeof(domain) - 1U) != SHA256_OK ||
        (identity_length != 0U &&
         sha256_update(&sha, salt_context->identity, identity_length) !=
             SHA256_OK) ||
        sha256_update(&sha, material, sizeof(material)) != SHA256_OK ||
        sha256_final(&sha, digest) != SHA256_OK) {
        memset(digest, 0, sizeof(digest));
        memset(&sha, 0, sizeof(sha));
        memset(material, 0, sizeof(material));
        return false;
    }
    memcpy(output, digest, length);
    memset(digest, 0, sizeof(digest));
    memset(&sha, 0, sizeof(sha));
    memset(material, 0, sizeof(material));
    return true;
}

static void kernel_installer_account_screen(const char *disk_identity)
{
    struct installer_credential_screen screen;
    enum production_install_flow_credential_requirement requirement =
        production_install_flow_credential_requirement();
    const enum appliance_admin_credentials_prompt_mode prompt_mode =
        requirement ==
                PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY
            ? APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN
            : APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET;
    size_t index;

    /*
     * The salt names the target disk the operator chose: copy the identity the
     * plan writes by into the context the derivation reads.  The identity is
     * not secret; it is copied with a bound so a hardware string can never
     * overflow the context.
     */
    memset(&kernel_installer_credential_salt_context, 0,
           sizeof(kernel_installer_credential_salt_context));
    if (disk_identity != NULL) {
        for (index = 0U;
             index + 1U <
                 sizeof(kernel_installer_credential_salt_context.identity) &&
             disk_identity[index] != '\0'; ++index) {
            kernel_installer_credential_salt_context.identity[index] =
                disk_identity[index];
        }
    }
    memset(&screen, 0, sizeof(screen));
    if (requirement !=
        PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY) {
        if (production_install_flow_set_credential_salt(
                kernel_installer_credential_salt,
                &kernel_installer_credential_salt_context) !=
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK) {
            return;
        }
    }
    for (;;) {
        enum installer_credential_screen_action action =
            installer_credential_screen_run_mode(
                &screen, prompt_mode,
                kernel_installer_declared_language());
        enum production_install_flow_credential_status status;

        if (action != INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE) {
            break;
        }
        size_t login_length = 0U;
        size_t password_length = 0U;
        const char *login = installer_credential_screen_login(
            &screen, &login_length);
        const char *password = installer_credential_screen_password(
            &screen, &password_length);
        status = requirement ==
                         PRODUCTION_INSTALL_FLOW_CREDENTIAL_REQUIRED_VERIFY
                     ? production_install_flow_set_existing_operator_credential(
                           login, login_length, password, password_length)
                     : production_install_flow_set_operator_credential(
                           login, login_length, password, password_length);

        if (status == PRODUCTION_INSTALL_FLOW_CREDENTIAL_OK) {
            installer_credential_screen_reset(&screen);
            break;
        }
        /*
         * A mandatory flow gets another hidden conversation.  The password
         * itself and the owner's refusal details are never printed; a wrong
         * update password simply returns to the same login questions.
         */
        installer_credential_screen_reset(&screen);
        if (requirement ==
            PRODUCTION_INSTALL_FLOW_CREDENTIAL_OPTIONAL_SET) {
            break;
        }
    }
    /*
     * The screen's buffers are zeroed whether the pair was committed or the
     * operator cancelled: the password never survives the screen.
     */
    installer_credential_screen_reset(&screen);
}

static bool kernel_installer_disk_screen_done;

static void kernel_installer_disk_screen(void)
{
    struct installer_storage_disk chosen;
    struct installer_dual_boot_requirements requirements;
    enum installer_storage_scheme scheme = INSTALLER_STORAGE_SCHEME_NONE;
    uint64_t required_lba = 0U;
    uint32_t released_index = 0U;
    bool scheme_chosen = false;

    if (kernel_installer_disk_screen_done ||
        !production_install_flow_awaiting_choice()) {
        return;
    }
    /*
     * The screen shows once: after it returns, either the plan the operator
     * accepted is ready to be applied, or the run is refused.  Asking again
     * would re-open a screen the operator has already answered.
     */
    kernel_installer_disk_screen_done = true;
    if (installer_storage_session_choose_note(
            production_install_flow_report(), INSTALLER_STORAGE_NO_DEADLINE,
            &chosen, production_install_flow_choice_note(),
            kernel_installer_declared_language()) !=
        INSTALLER_STORAGE_DECISION_CONFIRMED) {
        production_install_flow_cancel_by_operator();
        return;
    }
    if (!production_install_flow_choose_disk(chosen.id)) {
        return;
    }
    /*
     * ITEM-514: the order is the one the ordinary installers use - disk, then
     * scheme, then the plan.  The scheme screen starts on the declared or
     * discovered default, refuses an unusable row by name, and draws the
     * destructive page for the erase scheme; the manual scheme opens the
     * ITEM-501 partition screen, and Q there keeps every partition and goes
     * back to the schemes.  The summary that follows is the last page before
     * the write, and Q on it comes back here.
     */
    if (installer_layout_contract_dual_boot_requirements(&requirements) ==
        INSTALLER_LAYOUT_CONTRACT_OK) {
        required_lba = requirements.system_length_lba +
                       requirements.persistence_length_lba +
                       requirements.recovery_length_lba;
    }
    for (;;) {
        if (!scheme_chosen) {
            if (installer_storage_session_scheme(
                    production_install_flow_report(), chosen.id,
                    production_install_flow_default_scheme(), required_lba,
                    INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA,
                    production_install_flow_contents_unknown(),
                    INSTALLER_STORAGE_NO_DEADLINE, &scheme,
                    kernel_installer_declared_language()) !=
                INSTALLER_STORAGE_DECISION_CONFIRMED) {
                production_install_flow_cancel_by_operator();
                return;
            }
            /*
             * ITEM-501: the manual scheme names the partitions the operator
             * may give up, with the free space beside what the appliance
             * needs.  ENTER on a partition's warning page makes the install
             * give that partition up; Q keeps every partition and comes back
             * to the schemes.
             */
            if (scheme == INSTALLER_STORAGE_SCHEME_MANUAL) {
                released_index = 0U;
                if (installer_storage_session_release(
                        production_install_flow_report(), chosen.id,
                        INSTALLER_STORAGE_NO_DEADLINE, &released_index,
                        required_lba, kernel_installer_declared_language()) ==
                    INSTALLER_STORAGE_DECISION_CONFIRMED) {
                    if (!production_install_flow_choose_scheme(scheme)) {
                        production_install_flow_cancel_by_operator();
                        return;
                    }
                    production_install_flow_release_partition(released_index);
                    scheme_chosen = true;
                } else {
                    continue;
                }
            } else if (!production_install_flow_choose_scheme(scheme)) {
                production_install_flow_cancel_by_operator();
                return;
            } else {
                /* A later scheme that is not manual carries no released
                 * partition, and the flow withdrew the earlier one. */
                released_index = 0U;
                scheme_chosen = true;
            }
        }
        /*
         * The summary renders the plan the installer's own planner builds for
         * this scheme, so the page the operator accepts and the plan the
         * writer executes are one plan.  Q goes back to the schemes and the
         * earlier choice is withdrawn before a new one is made.
         */
        if (installer_storage_session_summary(
                production_install_flow_report(), chosen.id,
                production_install_flow_declared_scheme(), scheme,
                released_index, INSTALLER_STORAGE_NO_DEADLINE,
                kernel_installer_declared_language()) ==
            INSTALLER_STORAGE_DECISION_CONFIRMED) {
            break;
        }
        scheme_chosen = false;
    }
    /*
     * ITEM-506 increment 4: the operator who accepted the plan sets the
     * account before the install is applied.  Cancelling the account screen
     * does not cancel the install: the run continues with
     * `admin=UNSET reason=OPERATOR_DECLINED`.  The chosen disk's identity is
     * what the salt names beside the boot tick, so the account and the disk it
     * was set for are bound together.
     */
    kernel_installer_account_screen(chosen.identity);
    /*
     * The operator accepted the summary for the disk and scheme he saw; the
     * plan that follows is that plan, and applying it is what the ENTER on the
     * summary page authorized.
     */
    (void)production_install_flow_confirm_by_operator();
}

/*
 * ITEM-500: an operator entry that refused before the storage screen could
 * open - no disk to choose from, an unsupported firmware, a declaration that
 * cannot be honoured - leaves the machine booting from the medium, which reads
 * as "nothing happened".  Here, where the local input is live, the reason is
 * painted on the screen and the machine waits for a key, so the person who
 * started the install reads what stopped it instead of guessing.
 *
 * The unattended entry is untouched: nobody is there to read a screen, and its
 * record is the statement of the refusal.
 */
static bool kernel_installer_refusal_screen_done;

/*
 * ITEM-541: the facts the refusal page needs beyond the refusal itself and the
 * observed storage.  On a machine without serial this page is the only place
 * that can answer "did this machine recognise the medium it booted from" and
 * "is the install journal bound to it", so the evidence is gathered here from
 * the two owners of those facts - the xHCI medium verdict and the journal
 * platform - and the journal's RAM ring lends its bounded tail.  The pointers
 * are borrowed and stay valid while the page is painted; NULL evidence is the
 * page's own "no evidence" form.
 */
static void kernel_installer_refusal_evidence(
    struct installer_refusal_evidence *evidence)
{
    struct install_journal_status journal;
    const char *medium_state = NULL;
    const char *medium_reason = NULL;
    size_t index = 0U;

    if (evidence == NULL) {
        return;
    }
    evidence->medium_state = "UNCHECKED";
    evidence->medium_reason = "NOT_PROBED";
    evidence->journal_state = "UNBOUND";
    evidence->journal_reason = "NONE";
    evidence->journal_pending = 0U;
    evidence->journal_dropped = 0U;
    evidence->journal_refused = 0U;
    for (index = 0U; index < INSTALLER_STORAGE_REFUSAL_TAIL_MAX; ++index) {
        evidence->records[index] = NULL;
    }
    evidence->record_count = 0U;

    (void)xhci_storage_medium_status(&medium_state, &medium_reason);
    if (medium_state != NULL) {
        evidence->medium_state = medium_state;
    }
    if (medium_reason != NULL) {
        evidence->medium_reason = medium_reason;
    }

    install_journal_platform_status(&journal);
    evidence->journal_state = install_journal_state_string(journal.state);
    evidence->journal_reason = journal.reason != NULL ? journal.reason : "NONE";
    evidence->journal_pending = journal.pending;
    evidence->journal_dropped = journal.dropped;
    evidence->journal_refused = journal.refused;

    index = 0U;
    while (index < INSTALLER_STORAGE_REFUSAL_TAIL_MAX) {
        const char *record =
            install_journal_platform_record((uint32_t)index, NULL);

        if (record == NULL) {
            break;
        }
        evidence->records[index] = record;
        ++index;
    }
    evidence->record_count = index;
}

static void kernel_installer_refusal_screen(void)
{
    const char *refusal;
    struct appliance_storage_report observed;
    struct installer_refusal_evidence evidence;

    if (kernel_installer_refusal_screen_done) {
        return;
    }
    refusal = production_install_flow_operator_refusal();
    if (refusal == NULL) {
        return;
    }
    kernel_installer_refusal_screen_done = true;
    /*
     * ITEM-509 increment 27: the refusal page reads the catalog in the
     * language the medium declared, so a refused Russian install says why in
     * Russian.
     *
     * ITEM-518: the page also reads the observed storage the flow captured -
     * the same model the `storage` command renders - so the reason code is not
     * the only thing the operator can act on.
     *
     * ITEM-541: and it reads whether the boot medium was recognised and
     * whether the journal is bound to it, with the bounded tail of the ring.
     */
    production_install_flow_storage_report(&observed);
    /*
     * ITEM-521 requirement 2: whatever the journal already holds is on the
     * medium before the page that says why the run stopped is painted.  The
     * refusal of the flush is published by the journal itself and never
     * changes the page or the run.
     */
    (void)install_journal_platform_flush();
    kernel_installer_refusal_evidence(&evidence);
    installer_storage_refusal_show(refusal, &observed, &evidence,
                                   kernel_installer_declared_language());
}
#endif

static void log_production_boot(void)
{
    const enum appliance_production_boot_status status =
        production_boot.status;
    const enum appliance_production_boot_readiness readiness =
        appliance_production_boot_readiness(&production_boot);
    const enum appliance_production_boot_mode mode =
        appliance_production_boot_mode(&production_boot);
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const char *backend = appliance_production_boot_backend_kind_string(
        production_boot.selected_kind);

    if (backend == NULL) {
        backend = "INVALID";
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /* ITEM-410: mirrored and stored - the storage verdict the hardware
         * run's FIRST_BOOT and STORAGE_STATE stages quote. */
        console_journal_begin();
        serial_write("[PRODUCTION-BOOT] status=");
        console_record_text("[PRODUCTION-BOOT] status=");
        serial_write(appliance_production_boot_status_string(status));
        console_record_text(
            appliance_production_boot_status_string(status));
        serial_write(" discovered=");
        console_record_text(" discovered=");
        serial_write_u64(production_boot.discovered_backend_count);
        console_record_u64(production_boot.discovered_backend_count);
        serial_write(" valid=");
        console_record_text(" valid=");
        serial_write_u64(production_boot.valid_backend_count);
        console_record_u64(production_boot.valid_backend_count);
        serial_write(" selected=");
        console_record_text(" selected=");
        serial_write(backend);
        console_record_text(backend);
        serial_write(" storage=");
        console_record_text(" storage=");
        serial_write(appliance_production_boot_readiness_string(readiness));
        console_record_text(
            appliance_production_boot_readiness_string(readiness));
        serial_write(" mode=");
        console_record_text(" mode=");
        serial_write(appliance_production_boot_mode_string(mode));
        console_record_text(
            appliance_production_boot_mode_string(mode));
        serial_write(" state=");
        console_record_text(" state=");
        serial_write(
            loader != NULL
                ? appliance_state_loader_status_string(loader->status)
                : "NOT_LOADED");
        console_record_text(
            loader != NULL
                ? appliance_state_loader_status_string(loader->status)
                : "NOT_LOADED");
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

/*
 * ITEM-509 increment 4: the console's language, the record behind it and the
 * one write the `language` command performs.  The console owns the vocabulary;
 * the system record stays with its owner, so the binding is this two-line
 * adapter and the console never sees the store's type.  A machine whose record
 * did not load publishes the default language and no binding, so `language ru`
 * answers that the setting is unavailable instead of pretending it was saved.
 */
static bool console_language_write(void *context,
                                   enum appliance_language language) {
    struct appliance_persistent_config *config = context;

    return config != NULL &&
           appliance_persistent_config_set_language(config, language) ==
               APPLIANCE_PERSISTENT_CONFIG_OK;
}

static struct console_language_binding console_language_binding = {
    NULL,
    console_language_write,
};

static void publish_console_language(void) {
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
    bool durable = false;

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
    /*
     * ITEM-509 increment 27 (509.7c-2): an install boot speaks the language the
     * medium declares, even when the machine still carries the previous
     * installation's record - otherwise the installer would show the old
     * system's language.  Nothing on the medium can be changed, so the console
     * publishes the language as not durable and binds no writer: the `language`
     * command answers UNAVAILABLE, which is the honest answer here.
     */
    if (kernel_installer_language_known) {
        console_publish_language(kernel_installer_declared_language(), false);
        console_bind_language(NULL);
        return;
    }
#endif
    if (loader != NULL &&
        appliance_persistent_config_language(
            &loader->system_config, &language) ==
            APPLIANCE_PERSISTENT_CONFIG_OK) {
        durable = true;
    }
    console_publish_language(language, durable);
    if (durable) {
        console_language_binding.context =
            &production_boot.state.system_config;
        console_bind_language(&console_language_binding);
    } else {
        console_bind_language(NULL);
    }
}

static void publish_production_boot_runtime(void)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    char hostname[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    size_t hostname_length = 0U;

    publish_console_language();
    if (loader == NULL ||
        appliance_persistent_config_hostname(
            &loader->system_config, hostname, sizeof(hostname),
            &hostname_length) != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return;
    }
    console_publish_boot_persistence(
        appliance_production_boot_readiness(&production_boot) ==
            APPLIANCE_PRODUCTION_BOOT_READINESS_READY,
        appliance_production_boot_configured_backend_count(
            &production_boot),
        loader->system_config.values.source,
        hostname, hostname_length);
}

/*
 * ITEM-461: how long one main-loop turn takes, and the worst so far.
 *
 * The owner's 2026-09-20 report was that the screen, the keyboard, the mouse
 * and the redraw were all slow; the mode was the suspected cause but nothing
 * measured the loop, so the suspicion could only be repeated.  The loop's own
 * turn is the number that decides it: the input is serviced, the reports are
 * decoded and the records are painted inside that turn, so a turn that stays
 * inside its budget is what "the console keeps up" means.  ITEM-463 moved the
 * measurement to the cycle counter: a turn that ends inside one 100 Hz tick
 * read as zero milliseconds, so the tick could not see the cost it was there
 * to bound.  The average over the heartbeat window and the worst turn of the
 * boot are microseconds now.  It lives outside the production-network guard
 * because the loop measures itself on every build, the diagnostics one
 * included.
 */
static uint64_t production_iteration_last_cycles;
static uint64_t production_iteration_window_us;
static uint64_t production_iteration_window_count;
static uint64_t production_iteration_max_us;
/*
 * MOUSE-DIAG (owner task 2026-09-27): the phase that spent the most recent
 * long turn, so the boot maximum is published with a name instead of a bare
 * number.  The phase accounting writes it while the completed turn's snapshot
 * is still the one in hand, and `log_production_iteration()` reads it for the
 * same turn a few instructions later.
 */
static const char *production_iteration_max_phase = "unknown";
/*
 * MOUSE-DIAG (2026-09-27, fix): the clock of that maximum.  `iter_max_us` was
 * published as a bare number scoped to the boot, and the record that named its
 * phase lived in the ring, which recycled it long before the run ended (the
 * `hw13` journal has `iter_max_us=3809337` and no `[ITER-MAX]` at all).  These
 * cumulative fields ride the `[LOOP-SUMMARY]` record instead, so the last
 * maximum survives to the shutdown however full the ring is.
 */
static uint64_t production_iteration_max_ticks;
static uint64_t production_iteration_max_second;
/*
 * MOUSE-DIAG (2026-09-27): the keyboard echo's window deltas.  The console
 * keeps cumulative counters; the loop publishes what the window added, and the
 * bases move with the publication so no window is counted twice.
 */
static uint64_t kernel_loop_kbd_echo_fast_base;
static uint64_t kernel_loop_kbd_echo_redraw_base;
static uint64_t kernel_loop_kbd_echo_chars_base;
static uint64_t kernel_loop_kbd_echo_redraw_us_base;
static uint64_t kernel_loop_kbd_echo_generation_base;
/*
 * MOUSE-DIAG step 3: the raw anchors of the phase window.  They exist only to
 * be published beside the existing accounting - no metric is computed from
 * them, and `loop_budget_accounting_mismatch()` is the only reader.
 */
static uint64_t kernel_loop_timing_start_cycles;
static uint64_t kernel_loop_timing_start_ticks;
static uint64_t kernel_loop_timing_start_real_seconds;
/*
 * ITEM-470 (СРОЧНО-20): the literal number of main-loop turns since boot.  The
 * owner's run read `passes` (~99/s, the service passes) next to `iter_us`
 * (~82 ms, i.e. ~12/s) with an eight-fold gap and no way to tell which metric
 * described the loop.  This counter is incremented once per turn, in the same
 * function that times it, and published next to `iter_us`.
 */
static uint64_t production_iteration_count;

static void log_production_iteration(void)
{
    const uint64_t now = timer_cycles();

    ++production_iteration_count;
    if (production_iteration_last_cycles != 0U) {
        const uint64_t micros =
            timer_cycles_to_us(now - production_iteration_last_cycles);

        production_iteration_window_us += micros;
        ++production_iteration_window_count;
        if (micros > production_iteration_max_us) {
            const uint64_t previous_max = production_iteration_max_us;

            production_iteration_max_us = micros;
            /*
             * MOUSE-DIAG (2026-09-27): a new boot maximum past the long-turn
             * budget names its phase, its clock and the loop count it happened
             * at.  `iter_max_us=3259516` alone could not be told from a
             * measurement artifact, and the window it happened in is the one
             * the journal drops first under load.
             */
            if (loop_budget_iter_max_should_publish(micros, previous_max)) {
                char record[XHCI_ITER_MAX_RECORD_BYTES];
                size_t length;

                production_iteration_max_ticks = timer_ticks();
                production_iteration_max_second = timer_real_seconds();
                length = xhci_format_iter_max_record(
                    record, sizeof(record), production_iteration_max_ticks,
                    production_iteration_count, production_iteration_max_second,
                    micros, production_iteration_max_phase);

                if (length > 0U) {
                    console_publish_journal(record, length);
                }
            }
        }
    }
    production_iteration_last_cycles = now;
}

static uint64_t production_iteration_average_us(void)
{
    /* ITEM-471: the average-turn arithmetic is the pure helper the host proof
       drives, so `iter_us * loops` is the window by construction. */
    return timer_window_average_us(production_iteration_window_us,
                                   production_iteration_window_count);
}

/* ITEM-461/463: the same numbers the heartbeat publishes, for `kbd`. */
uint64_t kernel_loop_iter_us(void) {
    return production_iteration_average_us();
}

uint64_t kernel_loop_iter_max_us(void) {
    return production_iteration_max_us;
}

/* ITEM-470: the literal loop turns since boot, next to the timed window. */
uint64_t kernel_loop_iteration_count(void) {
    return production_iteration_count;
}

/* ITEM-469: the once-a-second diagnostic heartbeat owns the averaging window. */
void kernel_loop_iter_window_reset(void) {
    production_iteration_window_us = 0U;
    production_iteration_window_count = 0U;
}

/*
 * ITEM-474 (round 13): where the turn's time goes.
 *
 * The round-13 hardware log (`/srv/hfoc-example/queue/hw6-log-all.txt`) read
 * `[LOOP-DIAG] iter_us=424022 ... loops=99 -> 195` over 42 real seconds and a
 * single scroll that moved the whole 4K body, and the owner's report was
 * "movement is jerky".  The average turn alone cannot say whether the loop is
 * working 400 ms or sleeping 400 ms: `iter_us` includes `cpu_hlt()`.  This
 * accounting splits the window into the console's own measured paint+scroll,
 * the xHCI service pass, input-event handling, the network block and the rest,
 * and publishes the work total beside the time spent halted - so the next run
 * reads "hlt dominates" or "the console eats the turn" instead of guessing.
 *
 * Every measured span subtracts the console cost the console module charged
 * while the span ran (its own TSC-based counters), so the five numbers are
 * disjoint and add up to the turn's work.  The turn's full period and the halt
 * time come from the gap between consecutive turns, measured at the top of the
 * loop; the first turn only opens the window.
 *
 * ITEM-476 (round 14 follow-up): a span is folded into the window only when the
 * turn that produced it is finalized, in the same step that folds that turn's
 * work and console cost.  Spans used to accumulate the moment they ended, so a
 * record published in the middle of a turn described N completed turns of work
 * beside N completed turns plus one unfinished turn's spans - and the parts
 * read larger than the whole (`other_us=0 work_us=264132` with 270869 us of
 * parts in the 2026-09-23 local run
 * `production-storage-boot-20260923T190005Z-561658`).  The window now holds
 * completed turns only, so `other_us` is a remainder that is never negative by
 * construction.
 */
enum kernel_loop_phase {
    KERNEL_LOOP_PHASE_XHCI = 0,
    KERNEL_LOOP_PHASE_INPUT,
    KERNEL_LOOP_PHASE_NETWORK,
    /*
     * ITEM-486 (round 23): `other_us` was one unnamed remainder, and the
     * owner's run showed a three-second freeze living inside it
     * (`other_us=3 108 092` beside `work_us=3 108 657`).  Three named
     * top-level spans now account for the remainder's parts, so the next
     * record names the subsystem instead of the remainder: the services and
     * application state, the journal records and their disk flush, and the
     * storage/queued writes.  `other_us` stays the residual.
     */
    KERNEL_LOOP_PHASE_SERVICES,
    KERNEL_LOOP_PHASE_JOURNAL,
    KERNEL_LOOP_PHASE_STORAGE,
    KERNEL_LOOP_PHASE_COUNT
};

/*
 * ITEM-479 (СРОЧНО-28, round 17): the input path's own sub-phases.
 *
 * The round-16 hardware run (`/srv/hfoc-example/queue/hw8-log-all.txt`) published
 * one turn of `input_us=1213704` beside `xhci_us=99`, and the phase budget
 * named "input" without saying whether the driver service and doorbell, the
 * keyboard's decoded events or the pointer's movement and drawing spent the
 * 1.2 s.  These three spans are measured inside the two top-level spans and
 * published beside them; the top-level `xhci_us`/`input_us` keep their old
 * meaning and their old names, so nothing already proven moves.
 */
enum kernel_loop_input_phase {
    KERNEL_LOOP_INPUT_XHCI = 0,
    KERNEL_LOOP_INPUT_KEYBOARD,
    KERNEL_LOOP_INPUT_POINTER,
    KERNEL_LOOP_INPUT_COUNT
};

struct kernel_loop_phase_span {
    uint64_t cycles;
    uint64_t console_us;
};

/* The turn in flight accumulates its spans here. */
static uint64_t kernel_loop_phase_us[KERNEL_LOOP_PHASE_COUNT];
/*
 * ITEM-487: the flush reading already published, so the window record names
 * each flush exactly once.
 */
static uint64_t kernel_bootlog_flush_seen;
/*
 * ITEM-488: how many phase windows have passed since the display reading was
 * last published.  The reading is a boot fact, but the journal is a bounded
 * ring that a long run overwrites - `hw17` booted on a real 3840x2160 firmware
 * framebuffer and by the time the machine was switched off the record that said
 * so had been overwritten by the run's own diagnostics.  Re-publishing it on a
 * slower cadence keeps the newest copy in the ring, so `BOOTLOG`, `hw-collect`
 * and the next acceptance run can read the display of the boot that is running.
 */
#define KERNEL_DISPLAY_PUBLISH_EVERY 5U
static uint64_t kernel_display_publications;
/*
 * ITEM-476: the published window holds completed turns only.  A turn's spans
 * are folded here at the next `kernel_loop_phase_turn_begin()`, in the same
 * step that folds that turn's work and console cost, so the record's parts
 * always describe the same set of turns as its `work_us`.
 */
static uint64_t kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_COUNT];
/*
 * ITEM-479: the input path's sub-spans, accumulated for the turn in flight and
 * folded into the published window at the same boundary as the top-level
 * spans, so a record never mixes an unfinished turn's subs with completed
 * turns (the ITEM-476 rule, one level down).
 */
static uint64_t kernel_loop_input_us[KERNEL_LOOP_INPUT_COUNT];
static uint64_t kernel_loop_input_window_us[KERNEL_LOOP_INPUT_COUNT];
/*
 * ITEM-482 (round 20): the input budgets' evidence.  `*_events`/`*_draws` are
 * the turn in flight, `*_window_*` the largest one the published window saw, so
 * `kbd_events <= kbd_budget` and `ptr_draws <= ptr_budget` name the worst turn
 * rather than a sum that a busy second would inflate.  The two cost windows sum
 * the console paint and the write-combining drain the keyboard path paid.
 */
static uint64_t kernel_loop_input_kbd_events;
static uint64_t kernel_loop_input_kbd_window_events;
static uint64_t kernel_loop_input_ptr_events;
static uint64_t kernel_loop_input_ptr_window_events;
static uint64_t kernel_loop_input_ptr_draws;
static uint64_t kernel_loop_input_ptr_window_draws;
static uint64_t kernel_loop_input_kbd_draw_us;
static uint64_t kernel_loop_input_kbd_window_draw_us;
static uint64_t kernel_loop_input_kbd_sync_us;
static uint64_t kernel_loop_input_kbd_window_sync_us;
/*
 * ITEM-479: the window's worst turn and the span it spent, plus how many turns
 * of the window ran longer than LOOP_BUDGET_LONG_TURN_US.  `worst_us` is the
 * full period (what `iter_max_us` measures for the boot) and `worst_name` is
 * resolved by `loop_budget_dominant()` from the same spans the record carries.
 */
static uint64_t kernel_loop_phase_worst_us;
static enum loop_budget_phase kernel_loop_phase_worst_name;
static uint64_t kernel_loop_phase_long_turns;
static uint64_t kernel_loop_phase_console_us;
static uint64_t kernel_loop_phase_work_us;
static uint64_t kernel_loop_phase_hlt_us;
static uint64_t kernel_loop_phase_total_us;
static uint64_t kernel_loop_phase_turns;
static uint64_t kernel_loop_phase_work_started_cycles;
static bool kernel_loop_phase_have_previous;
static uint64_t kernel_loop_phase_previous_start_cycles;
static uint64_t kernel_loop_phase_previous_work_us;
static uint64_t kernel_loop_phase_previous_console_us;
static uint64_t kernel_loop_phase_console_at_start_us;
static uint64_t kernel_loop_phase_publish_ticks;

static struct kernel_loop_phase_span kernel_loop_phase_begin(void) {
    struct kernel_loop_phase_span span;

    span.cycles = timer_cycles();
    span.console_us = console_cost_us();
    return span;
}

static void kernel_loop_phase_end(enum kernel_loop_phase phase,
                                  struct kernel_loop_phase_span span) {
    const uint64_t elapsed =
        timer_cycles_to_us(timer_cycles() - span.cycles);
    uint64_t console_spent = console_cost_us() - span.console_us;

    if (console_spent > elapsed) {
        console_spent = elapsed;
    }
    kernel_loop_phase_us[phase] += elapsed - console_spent;
}

/*
 * ITEM-479: the same measurement one level down, inside the driver and input
 * spans.  The console cost the console module charged while the sub-span ran is
 * subtracted here as well, so a sub-span never counts paint or scroll time the
 * top-level record already published as `console_us`.
 */
static void kernel_loop_phase_end_input(enum kernel_loop_input_phase phase,
                                        struct kernel_loop_phase_span span) {
    const uint64_t elapsed =
        timer_cycles_to_us(timer_cycles() - span.cycles);
    uint64_t console_spent = console_cost_us() - span.console_us;

    if (console_spent > elapsed) {
        console_spent = elapsed;
    }
    kernel_loop_input_us[phase] += elapsed - console_spent;
}

static void kernel_loop_phase_turn_begin(void) {
    const uint64_t now = timer_cycles();

    if (kernel_loop_phase_have_previous) {
        const uint64_t period =
            timer_cycles_to_us(now - kernel_loop_phase_previous_start_cycles);
        const uint64_t work = kernel_loop_phase_previous_work_us;
        /*
         * ITEM-479: name the worst turn's own largest span.  The snapshot is
         * taken before the in-flight spans are folded and cleared, so it
         * describes exactly the turn whose period is being finalized.
         */
        struct loop_budget_snapshot turn;

        turn.console_us = kernel_loop_phase_previous_console_us;
        turn.xhci_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_XHCI];
        turn.input_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_INPUT];
        turn.net_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_NETWORK];
        /* ITEM-486: the remainder's named parts, so the worst turn can say
           `svc`, `journal` or `store` instead of `other`. */
        turn.svc_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_SERVICES];
        turn.journal_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_JOURNAL];
        turn.store_us = kernel_loop_phase_us[KERNEL_LOOP_PHASE_STORAGE];
        turn.in_xhci_us = kernel_loop_input_us[KERNEL_LOOP_INPUT_XHCI];
        turn.in_kbd_us = kernel_loop_input_us[KERNEL_LOOP_INPUT_KEYBOARD];
        turn.in_ptr_us = kernel_loop_input_us[KERNEL_LOOP_INPUT_POINTER];
        turn.work_us = work;

        /*
         * ITEM-476: fold the finished turn's spans into the window in the same
         * step that folds its work and console cost.  Mixing an unfinished
         * turn's spans with a completed turn's work is what let the published
         * parts exceed `work_us`.
         */
        for (size_t index = 0U; index < KERNEL_LOOP_PHASE_COUNT; ++index) {
            kernel_loop_phase_window_us[index] += kernel_loop_phase_us[index];
            kernel_loop_phase_us[index] = 0U;
        }
        for (size_t index = 0U; index < KERNEL_LOOP_INPUT_COUNT; ++index) {
            kernel_loop_input_window_us[index] += kernel_loop_input_us[index];
            kernel_loop_input_us[index] = 0U;
        }
        /*
         * ITEM-482: fold the input budgets' counts with the spans they describe.
         * The window keeps the worst turn's event/draw counts and the sum of the
         * keyboard paint and drain, so the published fields always describe the
         * same set of turns as `work_us`.
         */
        if (kernel_loop_input_kbd_events >
            kernel_loop_input_kbd_window_events) {
            kernel_loop_input_kbd_window_events = kernel_loop_input_kbd_events;
        }
        if (kernel_loop_input_ptr_events >
            kernel_loop_input_ptr_window_events) {
            kernel_loop_input_ptr_window_events = kernel_loop_input_ptr_events;
        }
        if (kernel_loop_input_ptr_draws > kernel_loop_input_ptr_window_draws) {
            kernel_loop_input_ptr_window_draws = kernel_loop_input_ptr_draws;
        }
        kernel_loop_input_kbd_window_draw_us += kernel_loop_input_kbd_draw_us;
        kernel_loop_input_kbd_window_sync_us += kernel_loop_input_kbd_sync_us;
        kernel_loop_input_kbd_events = 0U;
        kernel_loop_input_ptr_events = 0U;
        kernel_loop_input_ptr_draws = 0U;
        kernel_loop_input_kbd_draw_us = 0U;
        kernel_loop_input_kbd_sync_us = 0U;
        kernel_loop_phase_total_us += period;
        kernel_loop_phase_work_us += work;
        kernel_loop_phase_console_us += kernel_loop_phase_previous_console_us;
        kernel_loop_phase_hlt_us += period > work ? period - work : 0U;
        ++kernel_loop_phase_turns;
        if (period > kernel_loop_phase_worst_us) {
            kernel_loop_phase_worst_us = period;
            kernel_loop_phase_worst_name = loop_budget_dominant(&turn);
        }
        if (loop_budget_is_long_turn(period)) {
            ++kernel_loop_phase_long_turns;
            /* MOUSE-DIAG (2026-09-27): which span spent this stall turn, taken
               while its snapshot is still the completed one. */
            production_iteration_max_phase = loop_budget_phase_name(
                loop_budget_dominant(&turn));
        }
    }
    kernel_loop_phase_previous_start_cycles = now;
    kernel_loop_phase_have_previous = true;
    kernel_loop_phase_work_started_cycles = now;
    kernel_loop_phase_console_at_start_us = console_cost_us();
}

static void kernel_loop_phase_turn_end(void) {
    kernel_loop_phase_previous_work_us =
        timer_cycles_to_us(timer_cycles() - kernel_loop_phase_work_started_cycles);
    kernel_loop_phase_previous_console_us =
        console_cost_us() - kernel_loop_phase_console_at_start_us;
}

static void kernel_loop_phase_window_reset(void) {
    for (size_t index = 0U; index < KERNEL_LOOP_PHASE_COUNT; ++index) {
        /*
         * ITEM-476: only the completed-turn window resets.  The turn in flight
         * keeps its spans and folds them at the next turn boundary, so no span
         * is counted twice or dropped.
         */
        kernel_loop_phase_window_us[index] = 0U;
    }
    /* ITEM-479: the sub-span window resets with the window it belongs to. */
    for (size_t index = 0U; index < KERNEL_LOOP_INPUT_COUNT; ++index) {
        kernel_loop_input_window_us[index] = 0U;
    }
    /* ITEM-482: the input budgets' window resets with the spans. */
    kernel_loop_input_kbd_window_events = 0U;
    kernel_loop_input_ptr_window_events = 0U;
    kernel_loop_input_ptr_window_draws = 0U;
    kernel_loop_input_kbd_window_draw_us = 0U;
    kernel_loop_input_kbd_window_sync_us = 0U;
    kernel_loop_phase_console_us = 0U;
    kernel_loop_phase_work_us = 0U;
    kernel_loop_phase_hlt_us = 0U;
    kernel_loop_phase_total_us = 0U;
    kernel_loop_phase_turns = 0U;
    kernel_loop_phase_worst_us = 0U;
    kernel_loop_phase_worst_name = LOOP_BUDGET_OTHER;
    kernel_loop_phase_long_turns = 0U;
    kernel_loop_timing_start_cycles = timer_cycles();
    kernel_loop_timing_start_ticks = timer_ticks();
    kernel_loop_timing_start_real_seconds = timer_real_seconds();
}

/*
 * ITEM-486 (round 24): the framebuffer telemetry clock.  The framebuffer
 * publishes microseconds and the cycle counter is the only clock fine enough
 * for one glyph; the boot binds it once the calibration can answer.  Before the
 * calibration is ready `timer_cycles_to_us()` returns zero, so the boot simply
 * contributes no time to the totals.  It is defined outside the production
 * network block because the boot binds it in every configuration.
 */
static uint64_t kernel_framebuffer_clock_us(void) {
    return timer_cycles_to_us(timer_cycles());
}

#ifdef HFOC_PRODUCTION_NETWORK
/*
 * ITEM-405: publish one network-record fragment to serial and to the console
 * mirror in the same step, so the two records cannot drift apart.  The serial
 * calls and their order are unchanged; the mirror is buffered and painted once
 * per record, after the serial sequence has ended.
 */
/*
 * ITEM-477 (round 15): the fragment writer appends to whichever mirror the
 * record opened with `console_record_begin()` or `console_journal_begin()`.
 * The serial calls are unchanged - the periodic records stay on the serial
 * stream and in the durable journal - and the journal-only opener is what
 * keeps a timer-driven record off the screen (see the four records below).
 */
#define NET_RECORD_WRITE(text) do { \
        serial_write(text); \
        console_record_text(text); \
    } while (0)
#define NET_RECORD_U64(value) do { \
        serial_write_u64(value); \
        console_record_u64(value); \
    } while (0)

static void log_production_network_mac(const uint8_t *mac)
{
    static const char digits[] = "0123456789ABCDEF";
    uint32_t index;

    for (index = 0U; index < NETWORK_DEVICE_MAC_LENGTH; ++index) {
        char text[3];

        if (index != 0U) {
            serial_write(":");
            /* ITEM-405: the same separator on the console mirror. */
            console_record_text(":");
        }
        text[0] = digits[(mac[index] >> 4U) & 0xFU];
        text[1] = digits[mac[index] & 0xFU];
        text[2] = '\0';
        serial_write(text);
        console_record_text(text);
    }
}

/*
 * ITEM-428: the loop's heartbeat.
 *
 * The record is published whether or not anything changed, so a machine that
 * is up always publishes and a machine that is not can be read as one: the
 * last heartbeat names the tick the loop reached, and the record that follows
 * it (or its absence) says where the loop stopped.  The live frame-path
 * counters ride along because they are what separates "there is nothing to
 * report" from "the transmit path stopped completing descriptors" - the
 * question the 2026-09-16 log could not answer.
 */
static struct appliance_loop_heartbeat production_loop_heartbeat;

/*
 * ITEM-467: the input path's own durable record.
 *
 * The heartbeat above carries the frame counters and then the input counters,
 * and on the hardware run that record was cut off in the durable journal
 * before `kbd_dropped=`, `kbd_gap_ms_max=` and `iter_max_us=` were reached, so
 * "the keyboard loses characters" could not be measured at all.  This record
 * carries only the input facts, keeps them short enough to fit the journal's
 * line bound, and adds the pointer's own evidence: whether its one interrupt
 * transfer is still in flight, how many completions were re-armed, how long
 * ago it last answered, and why it has delivered no report.
 *
 * It is published on the heartbeat's own cadence, immediately after it, so the
 * two records describe the same window and a reader can compare them.
 */
static uint64_t production_input_last_ticks;
/*
 * ITEM-486 (round 24): the mouse event path's window deltas.  The classifiers
 * own the previous record's cumulative counters, so `mouse_path` names where
 * the last second's events stopped rather than the boot's total.
 */
static uint32_t production_input_last_reports;
static uint32_t production_input_last_completions;
static uint32_t production_input_last_watchdogs;
static uint64_t production_input_last_flushes;
/*
 * ITEM-486 (round 24): the outcome the last input record published, kept so the
 * run summary can carry it without classifying the window a second time.
 */
static enum input_path_outcome production_input_last_outcome;

static void log_production_input_record(void)
{
    const uint64_t now = timer_ticks();
    const uint32_t kbd_gap_window = xhci_keyboard_gap_window_ms_max();
    uint64_t serial_flags;

    /*
     * ITEM-469: the input record is the once-a-second heartbeat now, not a
     * rider on the ten-second [PRODUCTION-LOOP] record.  The owner's seven
     * second run held exactly one input record (at t=10, before typing), so
     * the QEMU proof and the next hardware run need the cadence to be a
     * second.  `production_input_last_ticks == 0` publishes the first one
     * immediately.
     */
    if (production_input_last_ticks != 0U &&
        now - production_input_last_ticks < HFOC_TIMER_HZ) {
        return;
    }
    production_input_last_ticks = now;
    /*
     * ITEM-482 (round 20): `kbd_gap_ms_max` describes the window that is about
     * to be published, not the whole boot - reading it closes the window, so
     * the next record is one second's worst and a long boot no longer hides
     * the current keyboard behaviour behind an old maximum.
     */
    xhci_keyboard_gap_window_reset();
    serial_flags = serial_sequence_begin();

    /*
     * ITEM-470: the core record is what the QEMU proof parses and what the
     * durable journal keeps, so it carries the numbers that must never be cut:
     * the keyboard's loss facts, the queue depths (ITEM-469), the pointer's
     * unarmed windows, the literal loop turns and the boot's worst turn.  The
     * pointer's "why silent" tail moved to `[PRODUCTION-INPUT-TAIL]` below: it
     * is the longest field, it is only read after a fault, and keeping it here
     * pushed the record past the journal's 320-byte line bound when the
     * counters grow.
     */
    /* ITEM-477: a once-a-second record reaches serial and the journal, not
       the screen; see the journal-only builder in console.h. */
    console_journal_begin();
    NET_RECORD_WRITE("[PRODUCTION-INPUT] kbd_reports=");
    NET_RECORD_U64(xhci_keyboard_report_count());
    NET_RECORD_WRITE(" kbd_dropped=");
    NET_RECORD_U64(xhci_keyboard_dropped_count());
    NET_RECORD_WRITE(" kbd_gap_ms_max=");
    NET_RECORD_U64(kbd_gap_window);
    /*
     * ITEM-469: the depth of the armed-transfer queue and the missed-service
     * completions.  `kbd_armed>=2` while READY and `kbd_mse=0` are what the
     * QEMU fast-typing proof requires; on hardware they name whether the fix
     * actually closed the no-TRB window.
     */
    NET_RECORD_WRITE(" kbd_armed=");
    NET_RECORD_U64(xhci_keyboard_armed_count());
    NET_RECORD_WRITE(" kbd_mse=");
    NET_RECORD_U64(xhci_keyboard_mse_count());
    NET_RECORD_WRITE(" mouse_reports=");
    NET_RECORD_U64(xhci_mouse_report_count());
    NET_RECORD_WRITE(" mouse_moves=");
    NET_RECORD_U64(xhci_mouse_move_count());
    NET_RECORD_WRITE(" mouse_presses=");
    NET_RECORD_U64(xhci_mouse_press_count());
    /*
     * ITEM-470: `mouse_armed=16` is the pointer's coverage of one loop turn at
     * its 1 ms endpoint, and `mouse_no_trb=0` is the proof no completion left
     * the endpoint without a TRB - the silent loss `mouse_mse` cannot see.
     */
    NET_RECORD_WRITE(" mouse_armed=");
    NET_RECORD_U64(xhci_mouse_armed_count());
    NET_RECORD_WRITE(" mouse_mse=");
    NET_RECORD_U64(xhci_mouse_mse_count());
    NET_RECORD_WRITE(" mouse_no_trb=");
    NET_RECORD_U64(xhci_mouse_no_trb_count());
    /* ITEM-470: the literal turns beside the worst turn of the boot. */
    NET_RECORD_WRITE(" loops=");
    NET_RECORD_U64(kernel_loop_iteration_count());
    NET_RECORD_WRITE(" iter_max_us=");
    NET_RECORD_U64(production_iteration_max_us);
    NET_RECORD_WRITE("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();

    /*
     * ITEM-470: the pointer's pending/reposted/last-report/reason tail and the
     * keyboard's two pending facts.  They are the fields a fault is diagnosed
     * from, not the once-a-second proof, so they are a short second record.
     */
    serial_flags = serial_sequence_begin();
    /* ITEM-477: the tail is periodic too - journal and serial, no screen. */
    console_journal_begin();
    NET_RECORD_WRITE("[PRODUCTION-INPUT-TAIL]");
    NET_RECORD_WRITE(" kbd_pending=");
    NET_RECORD_U64(xhci_keyboard_pending() ? 1U : 0U);
    NET_RECORD_WRITE(" kbd_reposted=");
    NET_RECORD_U64(xhci_keyboard_repost_count());
    NET_RECORD_WRITE(" mouse_pending=");
    NET_RECORD_U64(xhci_mouse_pending() ? 1U : 0U);
    NET_RECORD_WRITE(" mouse_reposted=");
    NET_RECORD_U64(xhci_mouse_repost_count());
    NET_RECORD_WRITE(" mouse_last_ms=");
    NET_RECORD_U64(xhci_mouse_last_report_ms());
    /*
     * ITEM-473 (СРОЧНО-23): the pointer's arm depth and the controller's Event
     * Ring Full (21) count.  They live in the tail record, whose fields are few
     * enough to hold them, while the core record stays inside its 320-byte
     * bound: at depth sixteen a burst used to fill the sixteen-TRB event ring,
     * and `ringfull=0` at `mouse_arm_depth=16` is the QEMU proof that the ring
     * now absorbs the burst.
     */
    NET_RECORD_WRITE(" mouse_arm_depth=");
    NET_RECORD_U64(xhci_mouse_arm_depth());
    NET_RECORD_WRITE(" ringfull=");
    NET_RECORD_U64(xhci_event_ring_full_count());
    /*
     * ITEM-479 (round 17): how many silence episodes the stalled-endpoint
     * watchdog published and recovered for each role, beside the queue facts
     * they explain.
     */
    NET_RECORD_WRITE(" kbd_watchdog=");
    NET_RECORD_U64(xhci_keyboard_watchdog_count());
    NET_RECORD_WRITE(" mouse_watchdog=");
    NET_RECORD_U64(xhci_mouse_watchdog_count());
    NET_RECORD_WRITE(" mouse_reason=");
    NET_RECORD_WRITE(xhci_mouse_no_report_reason());
    NET_RECORD_WRITE("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();

    /*
     * ITEM-482 (round 20): the window metrics the owner's "the keyboard lags,
     * the mouse freezes" report is diagnosed from.  They get their own short
     * record so neither the core nor the tail record grows past the journal
     * bound: `kbd_last_ms` is the wait since the keyboard last answered,
     * `kbd_rate`/`mouse_rate` are reports per second, `kbd_echo_us_max` the
     * report-to-screen latency, and the mouse fields say how many silences
     * happened while the pointer was in use, how many wanted the stronger
     * reset, and the worst recovery cost.
     */
    /*
     * ITEM-486 (round 24): name where the mouse's events stopped in the window.
     * `mouse_reports`/`mouse_reposted` are cumulative, so the classifier's
     * window uses their deltas; the pointer's local events and draws are
     * already the last completed loop window.  This is the field that answers
     * "mouse_armed>0 and no report: did a physical completion happen?" without
     * making a reader reconstruct it from six counters.
     */
    {
        const uint32_t reports_now = xhci_mouse_report_count();
        const uint32_t completions_now = xhci_mouse_repost_count();
        const uint32_t watchdogs_now = xhci_mouse_watchdog_count();
        const uint64_t flushes_now = framebuffer_flush_count();
        struct input_path_window path_window;
        enum input_path_outcome path_outcome;

        memset(&path_window, 0, sizeof(path_window));
        path_window.mouse_armed = xhci_mouse_armed_count() != 0U;
        path_window.completions =
            completions_now - production_input_last_completions;
        path_window.reports = reports_now - production_input_last_reports;
        path_window.local_events = kernel_loop_input_ptr_window_events;
        path_window.pointer_draws = kernel_loop_input_ptr_window_draws;
        path_window.watchdog_rearms =
            watchdogs_now - production_input_last_watchdogs;
        path_window.watchdog_gave_up = xhci_mouse_watchdog_gave_up();
        path_window.framebuffer_flushes =
            flushes_now - production_input_last_flushes > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)(flushes_now - production_input_last_flushes);
        path_outcome = input_path_classify(&path_window);
        production_input_last_outcome = path_outcome;
        production_input_last_reports = reports_now;
        production_input_last_completions = completions_now;
        production_input_last_watchdogs = watchdogs_now;
        production_input_last_flushes = flushes_now;

        serial_flags = serial_sequence_begin();
        console_journal_begin();
        NET_RECORD_WRITE("[PRODUCTION-INPUT-WINDOW] kbd_last_ms=");
        NET_RECORD_U64(xhci_keyboard_last_report_ms());
        NET_RECORD_WRITE(" kbd_rate=");
        NET_RECORD_U64(xhci_keyboard_report_rate());
        NET_RECORD_WRITE(" kbd_echo_us_max=");
        NET_RECORD_U64(console_kbd_echo_us_max_value());
        NET_RECORD_WRITE(" mouse_rate=");
        NET_RECORD_U64(xhci_mouse_report_rate());
        NET_RECORD_WRITE(" mouse_active_silences=");
        NET_RECORD_U64(xhci_mouse_watchdog_active_count());
        NET_RECORD_WRITE(" mouse_reset_due=");
        NET_RECORD_U64(xhci_mouse_watchdog_escalations());
        NET_RECORD_WRITE(" mouse_recover_us_max=");
        NET_RECORD_U64(xhci_mouse_watchdog_recover_us_max());
        NET_RECORD_WRITE(" mouse_path=");
        NET_RECORD_WRITE(input_path_outcome_name(path_outcome));
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

/*
 * ITEM-486 (round 24): the run's surviving state.
 *
 * The durable journal is a 511-record ring, which at the records this kernel
 * publishes is about seventy-five seconds.  The motivating hardware run lasted
 * 4.7 minutes and lost its pointer at t~134 s, so by the time the log was read
 * the record that named the loss had already been reclaimed: the evidence
 * existed, and the ring had eaten it.
 *
 * Every field below is a *cumulative* counter, never a window delta, so the
 * newest summary describes the whole run no matter how many older records the
 * ring has since overwritten.  This is the cheap half of "the evidence has to
 * survive": an operator reads the last `[LOOP-SUMMARY]` in the log and sees the
 * run's worst turn, whether the pointer ever stopped completing physically
 * (`mouse_completed` is the driver's completion count, `mouse_reports` what it
 * decoded from them), how long the worst silence was, whether the watchdog gave
 * up, and what the framebuffer spent.
 *
 * It is published every fifth phase record, so the coverage the journal loses
 * to it is about one record in five seconds, and it is journal-only like the
 * other periodic records.  The window-scoped view lives in `[LOOP-PHASES]` and
 * `[PRODUCTION-INPUT-WINDOW]`, which are published beside it.
 */
#define KERNEL_LOOP_SUMMARY_EVERY 5U
static uint64_t kernel_loop_summary_publications;

/*
 * ITEM-488: the display reading, re-published so it survives the ring.
 *
 * `display_publish()` already wrote it once at boot; this is the same record on
 * a five-second cadence, so the newest copy is never older than the journal's
 * own coverage.  It is one bounded line, and it is the only way an operator or
 * a collected run can tell a machine with a GOP framebuffer from one that fell
 * back to text.
 */
static void log_production_loop_display(void) {
    const uint64_t publication = kernel_display_publications++;

    if (publication % KERNEL_DISPLAY_PUBLISH_EVERY != 0U) {
        return;
    }
    display_publish();
}

static void log_production_loop_summary(void) {
    const uint64_t publication = kernel_loop_summary_publications++;
    uint64_t serial_flags;

    if (publication % KERNEL_LOOP_SUMMARY_EVERY != 0U) {
        return;
    }
    serial_flags = serial_sequence_begin();
    /* ITEM-477: a periodic cost/state record must not pay a paint. */
    console_journal_begin();
    NET_RECORD_WRITE("[LOOP-SUMMARY] loops=");
    NET_RECORD_U64(kernel_loop_iteration_count());
    NET_RECORD_WRITE(" iter_max_us=");
    NET_RECORD_U64(production_iteration_max_us);
    /*
     * MOUSE-DIAG (2026-09-27, fix): the phase and the clock of that maximum,
     * cumulative like the maximum itself.  The once-per-maximum `[ITER-MAX]`
     * record names them too, but the journal's ring recycles it; this summary
     * is republished every few seconds, so "what spent the worst turn, and
     * when" is still readable in the last record before the shutdown.
     */
    NET_RECORD_WRITE(" iter_max_phase=");
    NET_RECORD_WRITE(production_iteration_max_phase);
    NET_RECORD_WRITE(" iter_max_ticks=");
    NET_RECORD_U64(production_iteration_max_ticks);
    NET_RECORD_WRITE(" iter_max_second=");
    NET_RECORD_U64(production_iteration_max_second);
    NET_RECORD_WRITE(" ticks=");
    NET_RECORD_U64(timer_ticks());
    NET_RECORD_WRITE(" real_seconds=");
    NET_RECORD_U64(timer_real_seconds());
    /*
     * The pointer's whole-run state.  `mouse_path` is the last window's named
     * outcome - which at the end of a run is the state the run finished in -
     * and the counters beside it are the boot's totals, so "the pointer went
     * quiet at some point" can be told from "the pointer never worked".
     */
    NET_RECORD_WRITE(" mouse_path=");
    NET_RECORD_WRITE(input_path_outcome_name(production_input_last_outcome));
    NET_RECORD_WRITE(" mouse_completed=");
    NET_RECORD_U64(xhci_mouse_repost_count());
    NET_RECORD_WRITE(" mouse_reports=");
    NET_RECORD_U64(xhci_mouse_report_count());
    NET_RECORD_WRITE(" mouse_armed=");
    NET_RECORD_U64(xhci_mouse_armed_count());
    NET_RECORD_WRITE(" mouse_watchdog=");
    NET_RECORD_U64(xhci_mouse_watchdog_count());
    NET_RECORD_WRITE(" mouse_silent_max_ms=");
    NET_RECORD_U64(xhci_mouse_watchdog_silence_ms_max());
    NET_RECORD_WRITE(" mouse_gave_up=");
    NET_RECORD_U64(xhci_mouse_watchdog_gave_up() ? 1U : 0U);
    /*
     * The framebuffer and the network pass, both cumulative: the glyph question
     * ("is one character tens of milliseconds?") and the budget question ("how
     * often did the receive pass stop on its bound rather than on an empty
     * ring?") are answered from the last summary without the window records.
     */
    NET_RECORD_WRITE(" fb_shadow_draw_us=");
    NET_RECORD_U64(framebuffer_shadow_draw_us());
    NET_RECORD_WRITE(" fb_flush_count=");
    NET_RECORD_U64(framebuffer_flush_count());
    NET_RECORD_WRITE(" net_budget_us=");
    NET_RECORD_U64(appliance_production_network_boot_service_budget_us());
    NET_RECORD_WRITE(" net_budget_hits=");
    NET_RECORD_U64(appliance_production_network_boot_service_budget_hits());
    NET_RECORD_WRITE("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();
}

/*
 * ITEM-474 (round 13): the once-a-second phase budget.
 *
 * `console_us` is the console's own TSC-measured paint+scroll, `xhci_us` the
 * driver service pass, `input_us` the decoded-event handling, `net_us` the
 * network block and `other_us` everything else the turn did; `work_us` is the
 * sum of that work and `hlt_us` the time the turn spent halted before the next
 * interrupt, so "sleeping" and "working" are told apart.  `turn_us` is the
 * average full period and `turns` how many turns the window held.  Like the
 * input record, it is published on its own once-a-second cadence so a short
 * run still shows the loop, and the window resets with the record.
 *
 * ITEM-479 (round 17): `input_us` was one number for three different places, so
 * the owner's 1.2 s stall could only be guessed at.  The record now carries the
 * three sub-spans (`in_xhci_us`, `in_kbd_us`, `in_ptr_us`), the window's worst
 * turn and the span it spent (`worst_us=`, `worst_phase=`) and how many turns
 * ran past `LOOP_BUDGET_LONG_TURN_US` (`over50=`).  The top-level fields keep
 * their names and meanings; the sub-spans are measured inside them.
 */
static void log_production_loop_phases(void) {
    const uint64_t now = timer_ticks();
    uint64_t console_us;
    uint64_t xhci_us;
    uint64_t input_us;
    uint64_t net_us;
    uint64_t svc_us;
    uint64_t journal_us;
    uint64_t store_us;
    uint64_t other_us;
    uint64_t work_us;
    uint64_t hlt_us;
    uint64_t turns;
    uint64_t turn_us;
    uint64_t in_xhci_us;
    uint64_t in_kbd_us;
    uint64_t in_ptr_us;
    uint64_t worst_us;
    uint64_t over50;
    uint64_t kbd_events;
    uint64_t kbd_draw_us;
    uint64_t kbd_sync_us;
    uint64_t ptr_events;
    uint64_t ptr_draws;
    uint64_t fb_shadow_draw_us;
    uint64_t fb_dirty_flush_us;
    uint64_t fb_dirty_bytes;
    uint64_t fb_dirty_rects;
    uint64_t fb_flush_count;
    uint64_t net_budget_us;
    uint64_t net_budget_hits;
    const char *worst_phase;
    uint64_t serial_flags;

    if (kernel_loop_phase_publish_ticks != 0U &&
        now - kernel_loop_phase_publish_ticks < HFOC_TIMER_HZ) {
        return;
    }
    kernel_loop_phase_publish_ticks = now;
    console_us = kernel_loop_phase_console_us;
    xhci_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_XHCI];
    input_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_INPUT];
    net_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_NETWORK];
    /* ITEM-486 (round 23): the former `other` remainder's named parts. */
    svc_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_SERVICES];
    journal_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_JOURNAL];
    store_us = kernel_loop_phase_window_us[KERNEL_LOOP_PHASE_STORAGE];
    work_us = kernel_loop_phase_work_us;
    {
        const uint64_t accounted = console_us + xhci_us + input_us + net_us +
                                   svc_us + journal_us + store_us;

        other_us = work_us > accounted ? work_us - accounted : 0U;
    }
    hlt_us = kernel_loop_phase_hlt_us;
    turns = kernel_loop_phase_turns;
    turn_us = turns != 0U ? kernel_loop_phase_total_us / turns : 0U;
    in_xhci_us = kernel_loop_input_window_us[KERNEL_LOOP_INPUT_XHCI];
    in_kbd_us = kernel_loop_input_window_us[KERNEL_LOOP_INPUT_KEYBOARD];
    in_ptr_us = kernel_loop_input_window_us[KERNEL_LOOP_INPUT_POINTER];
    worst_us = kernel_loop_phase_worst_us;
    over50 = kernel_loop_phase_long_turns;
    /*
     * ITEM-482 (round 20): the input budgets' own evidence.  `kbd_events` and
     * `ptr_draws` are the window's worst turn, so comparing them with the named
     * budgets says "no turn served more than the bound"; `kbd_draw_us` and
     * `kbd_sync_us` are the window's sums of the keyboard's paint and drain.
     */
    kbd_events = kernel_loop_input_kbd_window_events;
    kbd_draw_us = kernel_loop_input_kbd_window_draw_us;
    kbd_sync_us = kernel_loop_input_kbd_window_sync_us;
    ptr_events = kernel_loop_input_ptr_window_events;
    ptr_draws = kernel_loop_input_ptr_window_draws;
    /*
     * ITEM-486 (round 24): the framebuffer's own telemetry.  `fb_shadow_draw_us`
     * is the time the drawing primitives spent filling the RAM shadow,
     * `fb_dirty_flush_us` the time the batched rectangle copies spent writing
     * the device, and `fb_dirty_bytes`/`fb_dirty_rects`/`fb_flush_count` how
     * much and how often that batch wrote.  The owner's question - "does a
     * glyph cost tens or hundreds of milliseconds?" - is answered by the first
     * number beside the second.
     */
    fb_shadow_draw_us = framebuffer_shadow_draw_us();
    fb_dirty_flush_us = framebuffer_dirty_flush_us();
    fb_dirty_bytes = framebuffer_dirty_bytes();
    fb_dirty_rects = framebuffer_dirty_rects();
    fb_flush_count = framebuffer_flush_count();
    /*
     * ITEM-486 (round 24): the network pass's own bounded budget.  The two
     * numbers are the cap the receive loop honours and how many passes the
     * time bound - not an empty ring - ended, so `net_us` can be read against
     * a stated bound instead of guessed from a quiet link.
     */
    net_budget_us = 0U;
    net_budget_hits = 0U;
#ifdef HFOC_PRODUCTION_NETWORK
    net_budget_us = appliance_production_network_boot_service_budget_us();
    net_budget_hits = appliance_production_network_boot_service_budget_hits();
#endif
    worst_phase = loop_budget_phase_name(kernel_loop_phase_worst_name);

    serial_flags = serial_sequence_begin();
    /* ITEM-477: the phase budget is a cost record; it must not pay a paint. */
    console_journal_begin();
    NET_RECORD_WRITE("[LOOP-PHASES] turns=");
    NET_RECORD_U64(turns);
    NET_RECORD_WRITE(" console_us=");
    NET_RECORD_U64(console_us);
    NET_RECORD_WRITE(" xhci_us=");
    NET_RECORD_U64(xhci_us);
    NET_RECORD_WRITE(" input_us=");
    NET_RECORD_U64(input_us);
    NET_RECORD_WRITE(" net_us=");
    NET_RECORD_U64(net_us);
    /*
     * ITEM-486 (round 23): the former `other_us` remainder, split into the
     * services/application state, the journal records and their disk flush, and
     * the storage/queued writes, with `other_us` left as the residual.  A
     * three-second freeze inside the remainder now names its subsystem.
     */
    NET_RECORD_WRITE(" svc_us=");
    NET_RECORD_U64(svc_us);
    NET_RECORD_WRITE(" journal_us=");
    NET_RECORD_U64(journal_us);
    NET_RECORD_WRITE(" store_us=");
    NET_RECORD_U64(store_us);
    NET_RECORD_WRITE(" other_us=");
    NET_RECORD_U64(other_us);
    NET_RECORD_WRITE(" work_us=");
    NET_RECORD_U64(work_us);
    NET_RECORD_WRITE(" hlt_us=");
    NET_RECORD_U64(hlt_us);
    NET_RECORD_WRITE(" turn_us=");
    NET_RECORD_U64(turn_us);
    /*
     * ITEM-479: the input path's three sub-spans and the window's worst turn,
     * so the next hardware run reads "the pointer spent 1.2 s" or "the driver
     * service did" instead of one undifferentiated `input_us`.
     */
    NET_RECORD_WRITE(" in_xhci_us=");
    NET_RECORD_U64(in_xhci_us);
    NET_RECORD_WRITE(" in_kbd_us=");
    NET_RECORD_U64(in_kbd_us);
    NET_RECORD_WRITE(" in_ptr_us=");
    NET_RECORD_U64(in_ptr_us);
    NET_RECORD_WRITE(" worst_us=");
    NET_RECORD_U64(worst_us);
    NET_RECORD_WRITE(" worst_phase=");
    NET_RECORD_WRITE(worst_phase);
    NET_RECORD_WRITE(" over50=");
    NET_RECORD_U64(over50);
    /*
     * ITEM-486 (round 24): the two clocks of this loop, in the same record as
     * the window they disagree about.  `loops` counts real iterations and the
     * window above reports `turns`/`turn_us`/`worst_us`/`over50`; on `hw9`,
     * `hw10` and `hw11` the two readings differed by a steady ~1.55x, and a
     * reader who saw only one of them twice mistook the gap for a factor of
     * eight.  `ticks` is the PIT tick count the loop is paced by and
     * `real_seconds` the same clock expressed in whole seconds, so "iterations
     * per second", "ticks per second" and "turns per window" can be checked
     * against each other without a second record.
     */
    NET_RECORD_WRITE(" loops=");
    NET_RECORD_U64(kernel_loop_iteration_count());
    NET_RECORD_WRITE(" ticks=");
    NET_RECORD_U64(timer_ticks());
    NET_RECORD_WRITE(" real_seconds=");
    NET_RECORD_U64(timer_real_seconds());
    /*
     * ITEM-482 (round 20): the keyboard and pointer budgets the owner asked to
     * read beside the spans.  `kbd_events`/`ptr_draws` are the worst turn of the
     * window, so `kbd_events <= kbd_budget` and `ptr_draws <= ptr_budget` are
     * the proof that no input source made one turn unbounded; `kbd_draw_us` and
     * `kbd_sync_us` separate the keyboard echo's paint from its drain.
     */
    NET_RECORD_WRITE(" kbd_events=");
    NET_RECORD_U64(kbd_events);
    NET_RECORD_WRITE(" kbd_budget=");
    NET_RECORD_U64(LOCAL_INPUT_KEYBOARD_BUDGET);
    NET_RECORD_WRITE(" kbd_draw_us=");
    NET_RECORD_U64(kbd_draw_us);
    NET_RECORD_WRITE(" kbd_sync_us=");
    NET_RECORD_U64(kbd_sync_us);
    NET_RECORD_WRITE(" ptr_events=");
    NET_RECORD_U64(ptr_events);
    NET_RECORD_WRITE(" ptr_draws=");
    NET_RECORD_U64(ptr_draws);
    NET_RECORD_WRITE(" ptr_budget=");
    NET_RECORD_U64(LOCAL_INPUT_MOUSE_DRAW_BUDGET);
    /*
     * ITEM-477 (round 15): the lines the console actually painted since the
     * previous phase window, beside the console's own measured cost.  The
     * phase record is the once-a-second line the QEMU proof can read without
     * touching the framebuffer; a painted-line count that stays flat while the
     * heartbeat keeps publishing is the proof the periodic records reach the
     * journal and not the screen.
     */
    NET_RECORD_WRITE(" paint_lines=");
    NET_RECORD_U64(console_render_line_count());
    /*
     * ITEM-486 (round 24): the framebuffer's split cost.  The window numbers
     * above say where the turn went; these cumulative numbers say how much of
     * the console's work was the RAM shadow paint and how much was the batched
     * device copy, and how many bytes/rectangles that batch wrote.  A reader
     * divides `fb_shadow_draw_us` by the glyph count above it and sees whether
     * one glyph is microseconds or milliseconds.
     */
    NET_RECORD_WRITE(" fb_shadow_draw_us=");
    NET_RECORD_U64(fb_shadow_draw_us);
    NET_RECORD_WRITE(" fb_dirty_flush_us=");
    NET_RECORD_U64(fb_dirty_flush_us);
    NET_RECORD_WRITE(" fb_dirty_bytes=");
    NET_RECORD_U64(fb_dirty_bytes);
    NET_RECORD_WRITE(" fb_dirty_rects=");
    NET_RECORD_U64(fb_dirty_rects);
    NET_RECORD_WRITE(" fb_flush_count=");
    NET_RECORD_U64(fb_flush_count);
    /*
     * ITEM-486 (round 24): the network phase's bounded budget, beside the
     * `net_us` it bounds.
     */
    NET_RECORD_WRITE(" net_budget_us=");
    NET_RECORD_U64(net_budget_us);
    NET_RECORD_WRITE(" net_budget_hits=");
    NET_RECORD_U64(net_budget_hits);
    /*
     * ITEM-475 (round 14): which hardware source drives the 100 Hz tick and
     * how many of those wakeups the turn actually took per real second.  The
     * owner's run showed a machine living at ~2.4 wakeups/s with nothing in
     * the journal naming the tick source; `tick_source=pit` with `wake_hz`
     * near 100 is the production reading this item exists to restore.
     */
    NET_RECORD_WRITE(" tick_source=");
    NET_RECORD_WRITE(lapic_timer_tick_source_string(lapic_timer_tick_source()));
    NET_RECORD_WRITE(" wake_hz=");
    NET_RECORD_U64(timer_measured_tick_hz());
    NET_RECORD_WRITE("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();

    /*
     * MOUSE-DIAG (2026-09-27): the keyboard echo's split, once per phase
     * window.  `kbd_draw_us` already says how long the echo took; these say
     * whether it painted one cell or a whole line, how many characters that
     * repainted, how long the redraws took and how often the ITEM-483
     * generation check fell back to a full line.
     */
    {
        const uint64_t fast = console_input_echo_fast_count();
        const uint64_t redraw = console_input_echo_redraw_count();
        const uint64_t chars = console_input_echo_redraw_chars();
        const uint64_t redraw_us = console_input_echo_redraw_us_total();
        const uint64_t generation = console_input_echo_generation_invalidations();

        serial_flags = serial_sequence_begin();
        console_journal_begin();
        NET_RECORD_WRITE("[KBD-ECHO] fast=");
        NET_RECORD_U64(fast - kernel_loop_kbd_echo_fast_base);
        NET_RECORD_WRITE(" redraw=");
        NET_RECORD_U64(redraw - kernel_loop_kbd_echo_redraw_base);
        NET_RECORD_WRITE(" redraw_chars=");
        NET_RECORD_U64(chars - kernel_loop_kbd_echo_chars_base);
        NET_RECORD_WRITE(" redraw_us=");
        NET_RECORD_U64(redraw_us - kernel_loop_kbd_echo_redraw_us_base);
        NET_RECORD_WRITE(" redraw_us_max=");
        NET_RECORD_U64(console_input_echo_redraw_us_max());
        NET_RECORD_WRITE(" gen_invalid=");
        NET_RECORD_U64(generation - kernel_loop_kbd_echo_generation_base);
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
        kernel_loop_kbd_echo_fast_base = fast;
        kernel_loop_kbd_echo_redraw_base = redraw;
        kernel_loop_kbd_echo_chars_base = chars;
        kernel_loop_kbd_echo_redraw_us_base = redraw_us;
        kernel_loop_kbd_echo_generation_base = generation;
    }

    /*
     * MOUSE-DIAG step 3 (owner task 2026-09-27): the independent raw anchor of
     * the window just published, and the cost of the last journal flush.  Both
     * are telemetry only: the existing metrics keep their own arithmetic, and
     * `timing_accounting_mismatch` is a comparison, not a correction.
     */
    {
        struct xhci_timing_check check;
        char record[XHCI_TIMING_CHECK_RECORD_BYTES];
        const uint64_t end_cycles = timer_cycles();
        const uint64_t raw_elapsed_us = timer_cycles_to_us(
            end_cycles - kernel_loop_timing_start_cycles);
        const uint64_t sum_phase_us =
            console_us + xhci_us + input_us + net_us + svc_us + journal_us +
            store_us + other_us;
        size_t length;

        memset(&check, 0, sizeof(check));
        check.start_tsc = kernel_loop_timing_start_cycles;
        check.end_tsc = end_cycles;
        check.elapsed_cycles = end_cycles - kernel_loop_timing_start_cycles;
        check.ticks_start = kernel_loop_timing_start_ticks;
        check.ticks_end = timer_ticks();
        check.real_seconds_start = kernel_loop_timing_start_real_seconds;
        check.real_seconds_end = timer_real_seconds();
        check.tick_hz = timer_measured_tick_hz();
        check.raw_elapsed_us = raw_elapsed_us;
        check.sum_phase_us = sum_phase_us;
        check.work_us = work_us;
        check.turn_us = turn_us;
        check.worst_us = worst_us;
        check.iter_max_us = production_iteration_max_us;
        check.mismatch = loop_budget_accounting_mismatch(
                             sum_phase_us, raw_elapsed_us, turn_us)
                             ? 1U
                             : 0U;
        length = xhci_format_timing_check_record(record, sizeof(record),
                                                 &check);
        if (length > 0U) {
            console_publish_journal(record, length);
        }
        length = xhci_format_timing_check_tail_record(record, sizeof(record),
                                                      &check);
        if (length > 0U) {
            console_publish_journal(record, length);
        }
    }
    kernel_loop_phase_window_reset();
    /*
     * ITEM-487: the durable log's own cost, once per new flush.
     *
     * `hw15` measured the flush at 2.7-3.2 s for 64 records while the keyboard
     * waited behind it, and the flush itself could not say where that time
     * went.  The reading is published here rather than inside the store because
     * a record published during the flush is staged, not written: it reaches
     * the extent at the next flush, which is also when an operator reads it
     * beside the window record it belongs to.  Both halves go to the serial
     * stream and to the durable journal, like every other periodic record, so a
     * QEMU proof and a hardware run read the same numbers.
     *
     * ITEM-487 follow-up (hw16): each half is its **own** record, in its own
     * serial sequence and its own journal record.  `hw16` proved the fix and
     * could not read its own reading: the two halves were published as one
     * record, and a reader that resolves a slot as one named record - the
     * appliance's own dump and `scripts/hfoc-esp-read.py --bootlog` - does not
     * find `[BOOTLOG-FLUSH]` in a line that carries `[BOOTLOG-FLUSH-TAIL]` in
     * the middle of it.  Nothing else moves: same fields, same formatters, same
     * moment, no extra flush, no change to the ring, the drop policy, the
     * commit or the batch write - and both halves are published outside the
     * flush, so neither can lengthen it.
     */
    {
        struct appliance_boot_log_flush_stats stats;

        if (appliance_boot_log_store_last_flush(&stats) &&
            stats.count != kernel_bootlog_flush_seen) {
            char record[APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES];
            char tail[APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES];
            const size_t length = appliance_boot_log_store_format_flush_record(
                record, sizeof(record), &stats);
            const size_t tail_length =
                appliance_boot_log_store_format_flush_tail_record(
                    tail, sizeof(tail), &stats);

            kernel_bootlog_flush_seen = stats.count;
            if (length > 0U) {
                const uint64_t serial_flags = serial_sequence_begin();

                /* ITEM-477: a periodic cost record must not pay a paint. */
                console_journal_begin();
                NET_RECORD_WRITE(record);
                NET_RECORD_WRITE("\n");
                serial_sequence_end(serial_flags);
                console_journal_end();
            }
            if (tail_length > 0U) {
                const uint64_t serial_flags = serial_sequence_begin();

                console_journal_begin();
                NET_RECORD_WRITE(tail);
                NET_RECORD_WRITE("\n");
                serial_sequence_end(serial_flags);
                console_journal_end();
            }
        }
    }
    /*
     * ITEM-486 (round 24): the run's cumulative state, on its own slower
     * cadence, so the journal's newest records always describe the whole run
     * even though the ring only holds the last ~75 seconds of it.  Published
     * after the window reset because none of its fields is window-scoped.
     */
    log_production_loop_display();
    log_production_loop_summary();
}

static void log_production_loop_heartbeat(void)
{
    struct appliance_production_network_io_counters counters;

    if (!appliance_loop_heartbeat_due(
            &production_loop_heartbeat, hfoc_lwip_time_now())) {
        return;
    }
    memset(&counters, 0, sizeof(counters));
    appliance_production_network_boot_io_counters(&counters);
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /* ITEM-477: the ten-second liveness record is journal-only. */
        console_journal_begin();
        NET_RECORD_WRITE("[PRODUCTION-LOOP] state=RUNNING ticks=");
        NET_RECORD_U64(timer_ticks());
        NET_RECORD_WRITE(" interval=");
        NET_RECORD_U64(production_loop_heartbeat.interval_index);
        NET_RECORD_WRITE(" stage=");
        NET_RECORD_U64(appliance_production_network_boot_stage());
        if (counters.available) {
            NET_RECORD_WRITE(" tx_sub=");
            NET_RECORD_U64(counters.tx_submitted);
            NET_RECORD_WRITE(" tx_recl=");
            NET_RECORD_U64(counters.tx_reclaimed);
            NET_RECORD_WRITE(" tx_lost=");
            NET_RECORD_U64(counters.tx_abandoned);
            NET_RECORD_WRITE(" tx_full=");
            NET_RECORD_U64(counters.tx_ring_full);
            NET_RECORD_WRITE(" tx_rst=");
            NET_RECORD_U64(counters.tx_restarts);
            NET_RECORD_WRITE(" rx_ok=");
            NET_RECORD_U64(counters.rx_frames);
            /*
             * ITEM-435: the frames the device delivered and the stack refused,
             * and the receive ring's arming state as the part reported it.
             * Without `rx_drop` the 2026-09-17 record's `rx_ok=0 rx_err=0`
             * meant "the device delivered nothing" and "the device delivered
             * frames nothing accepted" at the same time; without `rx_arm` it
             * could not say whether the part ever held the ring.
             */
            NET_RECORD_WRITE(" rx_drop=");
            NET_RECORD_U64(counters.rx_dropped);
            NET_RECORD_WRITE(" rx_err=");
            NET_RECORD_U64(counters.rx_errors);
            NET_RECORD_WRITE(" rx_arm=");
            NET_RECORD_U64(counters.rx_ready);
            /*
             * ITEM-451: the same receive path, split into the classes a peer's
             * traffic actually falls into.  `rx_drop=407` on the 2026-09-19
             * run was one number for "unrelated LAN traffic", "a malformed
             * frame" and "the stack refused what it was sent"; these counters
             * answer the question the owner's laptop raised: did its ARP
             * request arrive and was it answered (`arp_req_rx`/`arp_rep_tx`),
             * did its echo arrive and was it answered
             * (`icmp_echo_rx`/`icmp_rep_tx`), did its panel SYN arrive and was
             * it accepted or refused (`syn_rx`/`synack_tx`/`rst_tx`), and how
             * much of the traffic was never addressed to this machine at all
             * (`nonipv4_rx`/`ipv4_other_rx`).
             */
            NET_RECORD_WRITE(" arp_req_rx=");
            NET_RECORD_U64(counters.classes.arp_request_rx);
            NET_RECORD_WRITE(" arp_rep_rx=");
            NET_RECORD_U64(counters.classes.arp_reply_rx);
            NET_RECORD_WRITE(" arp_rep_tx=");
            NET_RECORD_U64(counters.classes.arp_reply_tx);
            NET_RECORD_WRITE(" arp_req_tx=");
            NET_RECORD_U64(counters.classes.arp_request_tx);
            NET_RECORD_WRITE(" icmp_echo_rx=");
            NET_RECORD_U64(counters.classes.icmp_echo_rx);
            NET_RECORD_WRITE(" icmp_other_rx=");
            NET_RECORD_U64(counters.classes.icmp_other_rx);
            NET_RECORD_WRITE(" icmp_rep_tx=");
            NET_RECORD_U64(counters.classes.icmp_reply_tx);
            NET_RECORD_WRITE(" syn_rx=");
            NET_RECORD_U64(counters.classes.tcp_syn_rx);
            NET_RECORD_WRITE(" synack_tx=");
            NET_RECORD_U64(counters.classes.tcp_syn_ack_tx);
            NET_RECORD_WRITE(" rst_tx=");
            NET_RECORD_U64(counters.classes.tcp_rst_tx);
            NET_RECORD_WRITE(" ipv4_other_rx=");
            NET_RECORD_U64(counters.classes.ipv4_other_rx);
            NET_RECORD_WRITE(" nonipv4_rx=");
            NET_RECORD_U64(counters.classes.non_ipv4_rx);
            NET_RECORD_WRITE(" malformed_rx=");
            NET_RECORD_U64(counters.classes.malformed_rx);
            NET_RECORD_WRITE(" limited_rx=");
            NET_RECORD_U64(counters.classes.rate_limited_rx);
        }
        /*
         * ITEM-436: the input path's own traffic, on the same heartbeat. The
         * frame counters above say whether the wire moved; these say whether
         * the keyboard did, which is the question a machine whose console and
         * menu never answer raises. `kbd_reports` counts the transfers that
         * carried a whole 8-byte boot report, `kbd_ref` the completed
         * transfers that did not, and `kbd_last` the byte count of the last
         * completion - so a receiver that answers with short reports is read
         * as that, instead of as a silent keyboard.
         */
        NET_RECORD_WRITE(" kbd_reports=");
        NET_RECORD_U64(xhci_keyboard_report_count());
        NET_RECORD_WRITE(" kbd_xfer=");
        NET_RECORD_U64(xhci_keyboard_transfer_count());
        NET_RECORD_WRITE(" kbd_completions=");
        NET_RECORD_U64(xhci_keyboard_completion_count());
        NET_RECORD_WRITE(" kbd_err=");
        NET_RECORD_U64(xhci_keyboard_error_count());
        NET_RECORD_WRITE(" kbd_ref=");
        NET_RECORD_U64(xhci_keyboard_refused_count());
        NET_RECORD_WRITE(" kbd_last=");
        NET_RECORD_U64(xhci_keyboard_last_report_bytes());
        /*
         * ITEM-438: what those reports carried, and whether the keyboard ever
         * answered at all.  `kbd_reports` counts whole reports, so a receiver
         * that answers every poll with an idle report and one that is bound
         * with nothing to say are the same number; `kbd_keys` is the keys the
         * decoded reports produced, and `kbd_silent` is 1 once the silence has
         * outlasted the window below.  Together they read as the three
         * different states the 2026-09-17 log could not tell apart: never
         * answered (`kbd_reports=0 kbd_silent=1`), answered and said nothing
         * (`kbd_reports>0 kbd_keys=0`), answered with keys (`kbd_keys>0`).
         */
        NET_RECORD_WRITE(" kbd_keys=");
        NET_RECORD_U64(xhci_keyboard_key_count());
        NET_RECORD_WRITE(" kbd_silent=");
        NET_RECORD_U64(usb_hid_no_report_due(
                           xhci_keyboard_attached(),
                           xhci_keyboard_report_count(),
                           timer_uptime_seconds(),
                           USB_HID_NO_REPORT_WINDOW_SECONDS)
                           ? 1U
                           : 0U);
        /*
         * ITEM-458: what the input path refused and how long it made a report
         * wait.  `kbd_dropped` counts the reports the bounded console queue
         * could not hold (the 2026-09-19 report was characters vanishing while
         * the screen scrolled), and `kbd_gap_ms_max` the longest wait between
         * two reports the service pass delivered - with the ring drained to
         * empty on every pass that is the main loop's own period.
         */
        NET_RECORD_WRITE(" kbd_dropped=");
        NET_RECORD_U64(xhci_keyboard_dropped_count());
        NET_RECORD_WRITE(" kbd_gap_ms_max=");
        NET_RECORD_U64(xhci_keyboard_gap_ms_max());
        /* ITEM-461: how long the last completion waited for the next re-arm. */
        NET_RECORD_WRITE(" kbd_repost_ms_max=");
        NET_RECORD_U64(xhci_keyboard_repost_ms_max());
        /*
         * ITEM-448: the pointer's own traffic on the same line.  `mouse_moves`
         * is the decoder's count of reports that carried a movement, so a
         * bound mouse that is never touched and one that cannot report are
         * different records; `ptr=` is the console's arrow, present only once
         * a report has placed it.
         */
        NET_RECORD_WRITE(" mouse_reports=");
        NET_RECORD_U64(xhci_mouse_report_count());
        NET_RECORD_WRITE(" mouse_completions=");
        NET_RECORD_U64(xhci_mouse_completion_count());
        NET_RECORD_WRITE(" mouse_wheels=");
        NET_RECORD_U64(xhci_mouse_wheel_count());
        NET_RECORD_WRITE(" mouse_moves=");
        NET_RECORD_U64(xhci_mouse_move_count());
        NET_RECORD_WRITE(" mouse_presses=");
        NET_RECORD_U64(xhci_mouse_press_count());
        NET_RECORD_WRITE(" mouse_silent=");
        NET_RECORD_U64(usb_hid_no_report_due(
                           xhci_mouse_attached(),
                           xhci_mouse_report_count(),
                           timer_uptime_seconds(),
                           USB_HID_NO_REPORT_WINDOW_SECONDS)
                           ? 1U
                           : 0U);
        {
            int32_t pointer_x = 0;
            int32_t pointer_y = 0;
            const bool pointer_placed =
                console_pointer_position(&pointer_x, &pointer_y);

            NET_RECORD_WRITE(" ptr_ready=");
            NET_RECORD_U64(pointer_placed ? 1U : 0U);
            NET_RECORD_WRITE(" ptr_x=");
            NET_RECORD_U64((uint64_t)(pointer_x < 0 ? 0 : pointer_x));
            NET_RECORD_WRITE(" ptr_y=");
            NET_RECORD_U64((uint64_t)(pointer_y < 0 ? 0 : pointer_y));
        }
        /*
         * ITEM-461/463: the loop's own cost, so "the console kept up" is a
         * number and not a claim.  `iter_us` is the average turn since the
         * previous heartbeat and `iter_max_us` the worst turn of the boot; the
         * console's own paint, scroll and pointer costs ride here too, because
         * this record reaches the serial capture the QEMU proof holds.  The
         * microseconds come from the cycle counter: the 100 Hz tick read a
         * paint as zero.
         */
        NET_RECORD_WRITE(" iter_us=");
        NET_RECORD_U64(production_iteration_average_us());
        NET_RECORD_WRITE(" iter_max_us=");
        NET_RECORD_U64(production_iteration_max_us);
        NET_RECORD_WRITE(" paint_lines=");
        NET_RECORD_U64(console_render_line_count());
        NET_RECORD_WRITE(" paint_chars=");
        NET_RECORD_U64(console_render_char_count());
        NET_RECORD_WRITE(" paint_us=");
        NET_RECORD_U64(console_render_paint_us());
        NET_RECORD_WRITE(" paint_max_us=");
        NET_RECORD_U64(console_render_max_us());
        NET_RECORD_WRITE(" scroll_us=");
        NET_RECORD_U64(console_render_scroll_us());
        NET_RECORD_WRITE(" scroll_max_us=");
        NET_RECORD_U64(console_render_scroll_max_us());
        NET_RECORD_WRITE(" ptr_moves=");
        NET_RECORD_U64(console_render_pointer_moves());
        NET_RECORD_WRITE(" ptr_max_us=");
        NET_RECORD_U64(console_render_pointer_max_us());
        /* ITEM-466: the records the screen budget kept off the screen.  Every
           one of them is still in this journal, so the number reads as "the
           screen is quieter", not as "the machine stopped talking". */
        NET_RECORD_WRITE(" screen_suppressed=");
        NET_RECORD_U64(console_render_screen_suppressed());
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
    /*
     * ITEM-643: the live receive snapshot rides the same heartbeat but is a
     * separate bounded record, so a long [PRODUCTION-LOOP] line cannot push
     * it out.  It is journal-only like the loop record; `network`/`diag`
     * publishes the same composition.
     */
    if (counters.available) {
        char rx_record[APPLIANCE_PRODUCTION_NETWORK_RX_RECORD_MAX];

        if (appliance_production_network_rx_record_line(
                &counters, rx_record, sizeof(rx_record)) != 0U) {
            const uint64_t rx_flags = serial_sequence_begin();

            console_journal_begin();
            NET_RECORD_WRITE(rx_record);
            NET_RECORD_WRITE("\n");
            serial_sequence_end(rx_flags);
            console_journal_end();
        }
    }
    /*
     * ITEM-469: the input record is published by the caller on its own
     * once-a-second cadence (the loop calls log_production_input_record()
     * directly), not here: this function returns before its body when the
     * ten-second heartbeat is not due, which is why the owner's seven-second
     * run had a single input record.
     */
}

/*
 * ITEM-451: the first frame of each class names itself when it arrives.
 *
 * The heartbeat above carries the class counters, but it is published on the
 * loop's ten-second cadence and its clock is the PIT: on a loaded QEMU runner
 * the tick reaches ten seconds long after the traffic did, and on hardware a
 * machine switched off right after a failed ping would keep its evidence in
 * RAM.  This record is event-driven instead of clock-driven - it is published
 * on the loop turn that first sees a class that was empty before - so the
 * first ARP request, the first echo, the first panel SYN and the first
 * unrelated ethertype are each named once, immediately, on the serial line and
 * in the durable log.  The bound is the class count plus the two refusal
 * events: at most one record per class per boot.
 */
static uint32_t production_frame_class_published;

#define PRODUCTION_FRAME_ARP_REQUEST (1U << 0)
#define PRODUCTION_FRAME_ARP_REPLY (1U << 1)
#define PRODUCTION_FRAME_ICMP_ECHO (1U << 2)
#define PRODUCTION_FRAME_ICMP_OTHER (1U << 3)
#define PRODUCTION_FRAME_TCP_SYN (1U << 4)
#define PRODUCTION_FRAME_TCP_REFUSED (1U << 5)
#define PRODUCTION_FRAME_IPV4_OTHER (1U << 6)
#define PRODUCTION_FRAME_NON_IPV4 (1U << 7)
#define PRODUCTION_FRAME_MALFORMED (1U << 8)
#define PRODUCTION_FRAME_RATE_LIMITED (1U << 9)
#define PRODUCTION_FRAME_CLASS_COUNT 8U

struct production_frame_class {
    uint32_t bit;
    const char *name;
    uint32_t value;
    const char *first_field;
    uint32_t first_value;
    const char *second_field;
    uint32_t second_value;
};

static void log_production_frame_class(
    const struct production_frame_class *entry)
{
    const uint64_t serial_flags = serial_sequence_begin();

    console_journal_begin();
    NET_RECORD_WRITE("[PRODUCTION-FRAME] class=");
    NET_RECORD_WRITE(entry->name);
    if (entry->first_field != NULL) {
        NET_RECORD_WRITE(" ");
        NET_RECORD_WRITE(entry->first_field);
        NET_RECORD_WRITE("=");
        NET_RECORD_U64(entry->first_value);
    }
    if (entry->second_field != NULL) {
        NET_RECORD_WRITE(" ");
        NET_RECORD_WRITE(entry->second_field);
        NET_RECORD_WRITE("=");
        NET_RECORD_U64(entry->second_value);
    }
    NET_RECORD_WRITE("\n");
    serial_sequence_end(serial_flags);
    console_journal_end();
}

static void log_production_frame_classes(void)
{
    struct appliance_production_network_io_counters counters;
    struct production_frame_class classes[PRODUCTION_FRAME_CLASS_COUNT];
    const struct hfoc_lwip_proto_counters *frames;
    uint32_t index;

    memset(&counters, 0, sizeof(counters));
    appliance_production_network_boot_io_counters(&counters);
    frames = &counters.classes;
    memset(classes, 0, sizeof(classes));
    /*
     * The classes a peer's answer depends on come first, each with the counter
     * of what arrived and the counter of what left in answer: a name without
     * the answer beside it is the ambiguity this item exists to remove.
     */
    classes[0].bit = PRODUCTION_FRAME_ARP_REQUEST;
    classes[0].name = "ARP_REQUEST";
    classes[0].value = frames->arp_request_rx;
    classes[0].first_field = "arp_req_rx";
    classes[0].first_value = frames->arp_request_rx;
    classes[0].second_field = "arp_rep_tx";
    classes[0].second_value = frames->arp_reply_tx;
    classes[1].bit = PRODUCTION_FRAME_ICMP_ECHO;
    classes[1].name = "ICMP_ECHO";
    classes[1].value = frames->icmp_echo_rx;
    classes[1].first_field = "icmp_echo_rx";
    classes[1].first_value = frames->icmp_echo_rx;
    classes[1].second_field = "icmp_rep_tx";
    classes[1].second_value = frames->icmp_reply_tx;
    classes[2].bit = PRODUCTION_FRAME_TCP_SYN;
    classes[2].name = "TCP_SYN";
    classes[2].value = frames->tcp_syn_rx;
    classes[2].first_field = "syn_rx";
    classes[2].first_value = frames->tcp_syn_rx;
    classes[2].second_field = "synack_tx";
    classes[2].second_value = frames->tcp_syn_ack_tx;
    classes[3].bit = PRODUCTION_FRAME_ARP_REPLY;
    classes[3].name = "ARP_REPLY";
    classes[3].value = frames->arp_reply_rx;
    classes[3].first_field = "arp_rep_rx";
    classes[3].first_value = frames->arp_reply_rx;
    classes[4].bit = PRODUCTION_FRAME_ICMP_OTHER;
    classes[4].name = "ICMP_OTHER";
    classes[4].value = frames->icmp_other_rx;
    classes[4].first_field = "icmp_other_rx";
    classes[4].first_value = frames->icmp_other_rx;
    classes[5].bit = PRODUCTION_FRAME_IPV4_OTHER;
    classes[5].name = "IPV4_OTHER";
    classes[5].value = frames->ipv4_other_rx;
    classes[5].first_field = "ipv4_other_rx";
    classes[5].first_value = frames->ipv4_other_rx;
    classes[6].bit = PRODUCTION_FRAME_NON_IPV4;
    classes[6].name = "NON_IPV4";
    classes[6].value = frames->non_ipv4_rx;
    classes[6].first_field = "nonipv4_rx";
    classes[6].first_value = frames->non_ipv4_rx;
    classes[7].bit = PRODUCTION_FRAME_MALFORMED;
    classes[7].name = "MALFORMED";
    classes[7].value = frames->malformed_rx;
    classes[7].first_field = "malformed_rx";
    classes[7].first_value = frames->malformed_rx;
    for (index = 0U; index < PRODUCTION_FRAME_CLASS_COUNT; ++index) {
        if (classes[index].value == 0U ||
            (production_frame_class_published & classes[index].bit) != 0U) {
            continue;
        }
        production_frame_class_published |= classes[index].bit;
        log_production_frame_class(&classes[index]);
    }
    /*
     * A refused panel SYN is its own event: the SYN class is published when
     * the first SYN arrives, and this record names the refusal the moment the
     * first RST leaves, so a machine that answers the panel with a refusal and
     * a machine that never answers it are different records.
     */
    if (frames->tcp_rst_tx != 0U &&
        (production_frame_class_published & PRODUCTION_FRAME_TCP_REFUSED) ==
            0U) {
        const uint64_t serial_flags = serial_sequence_begin();

        production_frame_class_published |= PRODUCTION_FRAME_TCP_REFUSED;
        console_journal_begin();
        NET_RECORD_WRITE("[PRODUCTION-FRAME] class=TCP_REFUSED rst_tx=");
        NET_RECORD_U64(frames->tcp_rst_tx);
        NET_RECORD_WRITE(" syn_rx=");
        NET_RECORD_U64(frames->tcp_syn_rx);
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
    /*
     * The reachability window refusing an echo is the third event a ping
     * raises, and it is not a malformed frame: it gets its own line for the
     * same reason the SYN's refusal does.
     */
    if (frames->rate_limited_rx != 0U &&
        (production_frame_class_published & PRODUCTION_FRAME_RATE_LIMITED) ==
            0U) {
        const uint64_t serial_flags = serial_sequence_begin();

        production_frame_class_published |= PRODUCTION_FRAME_RATE_LIMITED;
        console_journal_begin();
        NET_RECORD_WRITE(
            "[PRODUCTION-FRAME] class=RATE_LIMITED limited_rx=");
        NET_RECORD_U64(frames->rate_limited_rx);
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

/*
 * ITEM-438: the keyboard's silence as a finding.
 *
 * A bound keyboard whose interrupt transfer is in flight and whose counters
 * are all zero is indistinguishable, in the record and on the screen, from a
 * keyboard nobody has touched - and that is what the 2026-09-17 hardware run
 * read: `[KBD ] state=READY`, a machine with a console and a menu that answer
 * nothing, and no line anywhere saying the device had never answered.  The
 * heartbeat carries `kbd_silent` for every reader of a full log; this record
 * is the one line an operator gets without one, published once, naming the
 * binding and the counters the next run has to compare.
 *
 * The decision is `usb_hid_no_report_due()` (host-proven); the window is the
 * console's own idle window, and the clock is the loop's uptime in seconds.
 */
static bool production_keyboard_silence_published;

/* The endpoint address is printed the way the boot record prints it (0xNN);
   that helper lives in the local-input section below. */
static void log_local_input_hex8(uint8_t value);

static void log_production_keyboard_silence(void)
{
    const uint64_t waited = timer_uptime_seconds();

    if (production_keyboard_silence_published ||
        !usb_hid_no_report_due(xhci_keyboard_attached(),
                               xhci_keyboard_report_count(), waited,
                               USB_HID_NO_REPORT_WINDOW_SECONDS)) {
        return;
    }
    production_keyboard_silence_published = true;

    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        NET_RECORD_WRITE("[KBD ] state=READY finding=NO_REPORT reports=");
        NET_RECORD_U64(xhci_keyboard_report_count());
        NET_RECORD_WRITE(" keys=");
        NET_RECORD_U64(xhci_keyboard_key_count());
        /* ITEM-458: the same two loss counters on the record that explains a
           keyboard nobody heard. */
        NET_RECORD_WRITE(" dropped=");
        NET_RECORD_U64(xhci_keyboard_dropped_count());
        NET_RECORD_WRITE(" gap_ms_max=");
        NET_RECORD_U64(xhci_keyboard_gap_ms_max());
        NET_RECORD_WRITE(" transfers=");
        NET_RECORD_U64(xhci_keyboard_transfer_count());
        NET_RECORD_WRITE(" waited=");
        NET_RECORD_U64(waited);
        NET_RECORD_WRITE("s window=");
        NET_RECORD_U64(USB_HID_NO_REPORT_WINDOW_SECONDS);
        NET_RECORD_WRITE("s iface=");
        NET_RECORD_U64(xhci_keyboard_interface());
        NET_RECORD_WRITE(" protocol=");
        NET_RECORD_U64(xhci_keyboard_protocol());
        NET_RECORD_WRITE(" dci=");
        NET_RECORD_U64(xhci_keyboard_dci());
        NET_RECORD_WRITE(" ep=0x");
        log_local_input_hex8(xhci_keyboard_endpoint_address());
        NET_RECORD_WRITE(" interval=");
        NET_RECORD_U64(xhci_keyboard_endpoint_interval());
        NET_RECORD_WRITE(" binterval=");
        NET_RECORD_U64(xhci_keyboard_endpoint_binterval());
        NET_RECORD_WRITE(" max_packet=");
        NET_RECORD_U64(xhci_keyboard_endpoint_max_packet());
        /*
         * ITEM-438: and how the reports were being read: `mode=REPORT` says
         * the binding came from the device's own report descriptor rather than
         * a declared boot protocol, `idle=REFUSED` says the once-a-second idle
         * report that would have proven the binding alive was refused - which
         * is the one state where silence is not by itself a defect.
         */
        NET_RECORD_WRITE(" mode=");
        NET_RECORD_WRITE(xhci_keyboard_report_mode() ? "REPORT" : "BOOT");
        NET_RECORD_WRITE(" idle=");
        NET_RECORD_WRITE(xhci_keyboard_idle_refused() ? "REFUSED" : "OK");
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

/*
 * ITEM-448: the pointer's silence as a finding, on the same rule as the
 * keyboard's.
 *
 * A mouse is asked for idle rate zero - report only on change - so an
 * untouched pointer legitimately sends nothing, and the one state that must
 * not read like that is a pointer that never answered at all.  The window and
 * the decision are the keyboard's own (`usb_hid_no_report_due`), so the two
 * records can be compared line for line; `reports=0 moves=0` with the binding
 * next to it says the device was bound and never spoke, which is what the next
 * hardware run has to be able to read.
 */
static bool production_mouse_silence_published;

static void log_production_mouse_silence(void)
{
    const uint64_t waited = timer_uptime_seconds();

    if (production_mouse_silence_published ||
        !usb_hid_no_report_due(xhci_mouse_attached(),
                               xhci_mouse_report_count(), waited,
                               USB_HID_NO_REPORT_WINDOW_SECONDS)) {
        return;
    }
    production_mouse_silence_published = true;

    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        NET_RECORD_WRITE("[MOUSE] state=READY finding=NO_REPORT reports=");
        NET_RECORD_U64(xhci_mouse_report_count());
        NET_RECORD_WRITE(" moves=");
        NET_RECORD_U64(xhci_mouse_move_count());
        NET_RECORD_WRITE(" transfers=");
        NET_RECORD_U64(xhci_mouse_transfer_count());
        NET_RECORD_WRITE(" errors=");
        NET_RECORD_U64(xhci_mouse_error_count());
        NET_RECORD_WRITE(" refused=");
        NET_RECORD_U64(xhci_mouse_refused_count());
        NET_RECORD_WRITE(" waited=");
        NET_RECORD_U64(waited);
        NET_RECORD_WRITE("s window=");
        NET_RECORD_U64(USB_HID_NO_REPORT_WINDOW_SECONDS);
        NET_RECORD_WRITE("s port=");
        NET_RECORD_U64(xhci_mouse_port());
        NET_RECORD_WRITE(" speed=");
        NET_RECORD_WRITE(xhci_speed_string(xhci_mouse_speed()));
        NET_RECORD_WRITE(" iface=");
        NET_RECORD_U64(xhci_mouse_interface());
        NET_RECORD_WRITE(" protocol=");
        NET_RECORD_U64(xhci_mouse_protocol());
        NET_RECORD_WRITE(" dci=");
        NET_RECORD_U64(xhci_mouse_dci());
        NET_RECORD_WRITE(" ep=0x");
        log_local_input_hex8(xhci_mouse_endpoint_address());
        NET_RECORD_WRITE(" interval=");
        NET_RECORD_U64(xhci_mouse_endpoint_interval());
        NET_RECORD_WRITE(" binterval=");
        NET_RECORD_U64(xhci_mouse_endpoint_binterval());
        NET_RECORD_WRITE(" max_packet=");
        NET_RECORD_U64(xhci_mouse_endpoint_max_packet());
        NET_RECORD_WRITE(" mode=");
        NET_RECORD_WRITE(xhci_mouse_report_mode() ? "REPORT" : "BOOT");
        NET_RECORD_WRITE("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

static void log_production_network_hex16(uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[5];

    text[0] = digits[(value >> 12U) & 0xFU];
    text[1] = digits[(value >> 8U) & 0xFU];
    text[2] = digits[(value >> 4U) & 0xFU];
    text[3] = digits[value & 0xFU];
    text[4] = '\0';
    serial_write(text);
    /* ITEM-405: the same field on the console mirror. */
    console_record_text(text);
}

static void log_production_network_hex8(uint8_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[3];

    text[0] = digits[(value >> 4U) & 0xFU];
    text[1] = digits[value & 0xFU];
    text[2] = '\0';
    serial_write(text);
    /* ITEM-405: the same field on the console mirror. */
    console_record_text(text);
}

static void log_production_network_hex32(uint32_t value)
{
    log_production_network_hex16((uint16_t)(value >> 16U));
    log_production_network_hex16((uint16_t)(value & 0xFFFFU));
}

/*
 * ITEM-583: the PHY evidence is its own record, so the long
 * `[PRODUCTION-NET]` line keeps its shape and a reader can tell a cable/PHY
 * failure from a driver failure: phyid names the part, BMCR says whether it
 * was powered down or isolated, BMSR/LPA say whether auto-negotiation ran and
 * what the link partner advertised.
 */
/*
 * ITEM-590: the PHY answer is its own journal record.
 *
 * It used to be appended after the `[PRODUCTION-NET]` record's terminator, and
 * the console's journal mirror drops fragments that arrive after a record
 * ended (they belong to no record).  The serial stream therefore carried
 * `[NET-PHY]` while the boot journal on the medium carried nothing - the
 * owner's Toshiba filled 512 slots and not one of them named the PHY.  The
 * composer owns its record now: `publish()` opens it, writes the line and its
 * terminator, and ends it, so the serial copy and the journal copy are the
 * same record.
 */
static void log_production_network_phy(void)
{
    struct appliance_production_network_phy_info info;

    memset(&info, 0, sizeof(info));
    appliance_production_network_boot_phy_info(&info);
    if (appliance_production_network_phy_record_publish(&info, serial_write)) {
        /* ITEM-592: the cold start's own evidence - profile, stages, PHYstatus
           and BMCR, the BMSR samples, the gigabit advertisement - as records
           of their own. */
        appliance_production_network_phy_detail_publish(&info, serial_write);
    }
}

static void log_production_network(void)
{
    const struct appliance_production_network *network =
        appliance_production_network_boot_network();
    /*
     * ITEM-389: the line is re-emitted whenever the honest state changes, so
     * a backend whose PHY only finishes negotiating after boot publishes the
     * transition to READY instead of leaving the first DEGRADED line behind.
     */
    static enum appliance_production_network_readiness last_readiness;
    static enum appliance_production_network_status last_status;
    static bool last_device_present;
    static bool last_link_up;
    static enum appliance_network_lifecycle_reason last_lifecycle_reason;
    static uint32_t last_lifecycle_hold;
    /*
     * ITEM-428: the backend's own reason is a field of this record, and a
     * driver that stops transmitting changes it (`TX_RING_FULL_EXHAUSTED`,
     * `TX_RING_FULL_RESTARTED`) without changing readiness, status, the device
     * or the link.  Without this the one line that names a stated refusal
     * would never be republished.
     */
    static const char *last_backend_reason;
    /*
     * ITEM-451: the three entry flags of the frame path.  They are set while
     * the machine comes up, so without them in this decision the one record
     * that names the receive path would never be republished when the path
     * finally opened.
     */
    static bool last_bound;
    static bool last_proto_published;
    static bool last_netif_live;
    static bool logged;
    const enum appliance_production_network_readiness readiness =
        appliance_production_network_boot_readiness();
    const enum appliance_production_network_status status =
        network != NULL ? network->status
                        : APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    const bool device_present =
        appliance_production_network_boot_device_present();
    /*
     * ITEM-389: the link field is the device's own live signal read through
     * the backend, so a link that changes after the bind is published again
     * even though the poll-driven composition is not re-bound.
     */
    const bool link_up = appliance_production_network_boot_link_up();
    /*
     * ITEM-408: the same record also carries why the addressing lifecycle
     * left ACTIVE, or held a live address through a failure, so a boot
     * without a serial capture names the path, the timeout and the link
     * sample instead of only showing that the address is gone.
     */
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct appliance_network_lifecycle *lifecycle =
        wire != NULL ? appliance_network_wire_lifecycle(wire) : NULL;
    const enum appliance_network_lifecycle_reason lifecycle_reason =
        lifecycle != NULL
            ? appliance_network_lifecycle_reason(lifecycle)
            : APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    const uint32_t lifecycle_hold =
        lifecycle != NULL
            ? appliance_network_lifecycle_hold_count(lifecycle)
            : 0U;
    /*
     * ITEM-428: the reason the backend publishes is part of the record's
     * content, so it takes part in the change decision.  The strings are the
     * backend's own literals, so comparing the pointer is comparing the value.
     */
    const char *backend_reason = appliance_production_network_boot_reason();
    /*
     * ITEM-451: the frame path's entry flags are part of the record's content,
     * so they take part in the change decision too.
     */
    const bool bound = appliance_production_network_bound(network);
    const bool proto_published =
        appliance_production_network_proto_published(network);
    const bool netif_live =
        appliance_production_network_netif_live(network);

    if (logged && readiness == last_readiness && status == last_status &&
        device_present == last_device_present && link_up == last_link_up &&
        lifecycle_reason == last_lifecycle_reason &&
        lifecycle_hold == last_lifecycle_hold &&
        backend_reason == last_backend_reason &&
        bound == last_bound &&
        proto_published == last_proto_published &&
        netif_live == last_netif_live) {
        return;
    }
    logged = true;
    last_readiness = readiness;
    last_status = status;
    last_device_present = device_present;
    last_link_up = link_up;
    last_lifecycle_reason = lifecycle_reason;
    last_lifecycle_hold = lifecycle_hold;
    last_backend_reason = backend_reason;
    last_bound = bound;
    last_proto_published = proto_published;
    last_netif_live = netif_live;

    {
        const uint64_t serial_flags = serial_sequence_begin();
        const char *reason = appliance_production_network_boot_reason();
        uint16_t vendor_id = 0U;
        uint16_t product_id = 0U;
        uint8_t revision = 0U;
        char chip_identity[48U];

        /*
         * ITEM-405: this record is why an operator could not tell whether the
         * NIC came up: the appliance printed only its banner and
         * `LOCAL INPUT: DISABLED`, and the `[PRODUCTION-NET]` line - which
         * names the device state, the readiness, the backend, the silicon and
         * the stage the network path stopped at - was serial-only.  The mirror
         * carries the same fragments to the local console.
         */
        console_journal_begin();
        NET_RECORD_WRITE("[PRODUCTION-NET] device=");
        NET_RECORD_WRITE(
            appliance_production_network_boot_device_state());
        NET_RECORD_WRITE(" readiness=");
        NET_RECORD_WRITE(appliance_production_network_readiness_string(
            readiness));
        NET_RECORD_WRITE(" backend=");
        NET_RECORD_WRITE(appliance_production_network_boot_backend());
        if (appliance_production_network_boot_pci_identity(
                &vendor_id, &product_id, &revision)) {
            NET_RECORD_WRITE(" pci=");
            log_production_network_hex16(vendor_id);
            NET_RECORD_WRITE(":");
            log_production_network_hex16(product_id);
            NET_RECORD_WRITE(" rev=");
            log_production_network_hex8(revision);
        }
        /*
         * ITEM-399: the silicon the Realtek backend identified, when it
         * identified one.  `pci=10EC:8168` covers a dozen parts of the 8168
         * family - the acceptance host's 8168H and the RTL8168E this product
         * must support among them - so the record states the XID the driver
         * decoded from TxConfig and the family its table mapped it to.  Both
         * fields go into the one record, inside the one serial sequence.
         */
        if (appliance_production_network_boot_chip_record(
                chip_identity, sizeof(chip_identity)) != 0U) {
            serial_write(chip_identity);
            console_record_text(chip_identity);
        }
        NET_RECORD_WRITE(" link=");
        NET_RECORD_WRITE(link_up ? "UP" : "DOWN");
        /*
         * ITEM-428: the PCI command register the live backend is running
         * with.  The 2026-09-16 hardware run's part answered every register
         * read while it never fetched a descriptor, and bit 2 of this register
         * is the one bit that separates those two facts - so the run publishes
         * it instead of leaving the next cycle to infer it.
         */
        {
            uint32_t pci_command = 0U;

            if (appliance_production_network_boot_pci_command(
                    &pci_command)) {
                NET_RECORD_WRITE(" pci_cmd=0x");
                log_production_network_hex32(pci_command);
            }
        }
        if (network != NULL) {
            NET_RECORD_WRITE(" status=");
            NET_RECORD_WRITE(appliance_production_network_status_string(
                status));
            NET_RECORD_WRITE(" stage=");
            NET_RECORD_U64(appliance_production_network_boot_stage());
            /*
             * ITEM-451: the three flags a frame has to pass before lwIP ever
             * sees it.  The 2026-09-19 run published a READY backend, a live
             * link and an acquired lease while the panel and ping answered
             * nothing, and nothing in the record said whether the receive path
             * was open at all.
             */
            NET_RECORD_WRITE(" bound=");
            NET_RECORD_U64(bound ? 1U : 0U);
            NET_RECORD_WRITE(" proto_published=");
            NET_RECORD_U64(proto_published ? 1U : 0U);
            NET_RECORD_WRITE(" netif_live=");
            NET_RECORD_U64(netif_live ? 1U : 0U);
        }
        if (network != NULL &&
            appliance_production_network_device(network) != NULL) {
            NET_RECORD_WRITE(" mac=");
            log_production_network_mac(
                appliance_production_network_mac(network));
        }
        if (reason != NULL) {
            NET_RECORD_WRITE(" reason=");
            NET_RECORD_WRITE(reason);
        }
        /*
         * ITEM-408: the device record and the addressing record are read as a
         * pair, so the reason the addressing lifecycle left ACTIVE - or held a
         * live address through a failure - is on both of them, under names
         * that cannot be confused with the driver's own bring-up reason.
         */
        if (lifecycle != NULL) {
            NET_RECORD_WRITE(" lifecycle_reason=");
            NET_RECORD_WRITE(
                appliance_network_lifecycle_reason_string(
                    lifecycle_reason) != NULL
                    ? appliance_network_lifecycle_reason_string(
                          lifecycle_reason)
                    : "UNKNOWN");
            NET_RECORD_WRITE(" lifecycle_hold=");
            NET_RECORD_U64(lifecycle_hold);
        }
        NET_RECORD_WRITE("\n");
        /*
         * ITEM-590: the device record is finished here, before the PHY one
         * starts.  Both the serial sequence and the journal record are closed,
         * so the PHY composer's own record is a record and not a tail of this
         * one - which is what the journal mirror was told to ignore.
         */
        serial_sequence_end(serial_flags);
        console_journal_end();
        log_production_network_phy();
    }
}

static void log_production_network_address(
    const struct network_ipv4_address *address)
{
    uint32_t index;

    if (address == NULL) {
        serial_write("none");
        return;
    }
    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U) {
            serial_write(".");
            /* ITEM-405: the same separator on the console mirror. */
            console_record_text(".");
        }
        serial_write_u64(address->octets[index]);
        console_record_u64(address->octets[index]);
    }
}

static enum appliance_network_lifecycle_state last_wire_state;
static bool last_dns_resolved;
static enum appliance_network_mode_action last_wire_mode;
static bool last_wire_clock_ready;
static bool last_wire_mode_logged;
/* ITEM-654: the SNTP source and the bounded client's verdict, republished
 * when either changes so an unreachable explicit server is not silent. */
static enum appliance_production_network_time_source last_wire_time_source;
static enum appliance_time_sync_state last_wire_time_state;
static enum appliance_time_sync_status last_wire_time_status;
static bool last_wire_time_valid;
static enum appliance_network_lifecycle_reason last_wire_reason;
static uint32_t last_wire_hold;
/* ITEM-416: the acquisition attempt, so a retry is visible as one. */
static uint32_t last_wire_attempt;
/*
 * ITEM-446: the static address the last NET-MODE record published, so a
 * declaration that lands after the record was first printed republishes it
 * with the address instead of leaving the reader with `mode=STATIC` and no
 * address.
 */
static bool last_wire_address_valid;
static struct network_ipv4_address last_wire_address;
static uint8_t last_wire_address_prefix;
/*
 * ITEM-428: the wire view the last published addressing record carried, so a
 * DHCP frame the transmit path refused - or an offer that arrived - republishes
 * the line even while the state word, the reason and the attempt count stay
 * where they were.  The republish is rate-limited: a counter that changes on
 * every frame must not turn a serving link into a record flood.
 */
static struct appliance_network_wire_dhcp_counters last_wire_counters;
static bool last_wire_counters_valid;
static uint32_t last_wire_counters_ms;
#define KERNEL_WIRE_COUNTER_REPUBLISH_MS 5000U

static void log_production_network_wire(void)
{
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    enum appliance_network_lifecycle_state state;
    enum appliance_network_mode_action mode =
        appliance_production_network_wire_boot_mode();
    const bool clock_ready = appliance_production_network_time_ready();
    struct network_ipv4_address time_server;
    enum appliance_production_network_time_source time_source =
        APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_NONE;
    const enum appliance_time_sync_state time_state =
        appliance_production_network_time_state();
    const enum appliance_time_sync_status time_status =
        appliance_production_network_time_status();
    const bool time_server_valid =
        appliance_production_network_time_server(&time_server, &time_source);
    const struct appliance_network_lifecycle *lifecycle =
        wire != NULL ? appliance_network_wire_lifecycle(wire) : NULL;
    const enum appliance_network_lifecycle_reason reason =
        lifecycle != NULL
            ? appliance_network_lifecycle_reason(lifecycle)
            : APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE;
    const uint32_t hold = lifecycle != NULL
        ? appliance_network_lifecycle_hold_count(lifecycle)
        : 0U;
    uint32_t wire_attempt = 0U;
    uint32_t wire_budget = 0U;
    /*
     * ITEM-428: the wire's own view of the DHCP exchange, published on the
     * addressing line so the next hardware run separates "the machine never
     * transmitted" from "it transmitted and was never answered" from "it was
     * answered and ignored the answer".  A counter-only change republishes the
     * record, rate-limited so a busy link cannot flood the log.
     */
    struct appliance_network_wire_dhcp_counters wire_counters;
    bool have_wire_counters;
    const struct network_ipv4_address *answer;
    struct appliance_production_network_wire_view wire_view;

    memset(&wire_counters, 0, sizeof(wire_counters));
    have_wire_counters =
        appliance_network_wire_dhcp_counters(wire, &wire_counters);
    /*
     * ITEM-435: the wire's own receive view, published next to the transmit
     * counters so the record separates "nothing arrived" from "something
     * arrived and the client refused it".
     */
    memset(&wire_view, 0, sizeof(wire_view));
    appliance_production_network_boot_wire_view(&wire_view);

    {
        /*
         * ITEM-446: the address a static declaration put in force, published
         * on the same line the mode is.  It is the active configuration's own
         * address, never a copy of the declaration, so a refused or overridden
         * declaration cannot print an address the interface does not have.
         */
        const struct network_ipv4_config *active_config =
            appliance_network_wire_active_config(wire);
        const bool address_valid =
            mode == APPLIANCE_NETWORK_MODE_ACTION_STATIC &&
            active_config != NULL;
        struct network_ipv4_address address;
        uint8_t prefix = 0U;

        memset(&address, 0, sizeof(address));
        if (address_valid) {
            address = active_config->address;
            prefix = active_config->prefix_length;
        }
        if (!last_wire_mode_logged || mode != last_wire_mode ||
            clock_ready != last_wire_clock_ready ||
            address_valid != last_wire_address_valid ||
            (address_valid &&
             (memcmp(&address, &last_wire_address, sizeof(address)) != 0 ||
              prefix != last_wire_address_prefix)) ||
            !last_wire_time_valid ||
            time_source != last_wire_time_source ||
            time_state != last_wire_time_state ||
            time_status != last_wire_time_status) {
            const uint64_t serial_flags = serial_sequence_begin();

            last_wire_mode = mode;
            last_wire_clock_ready = clock_ready;
            last_wire_mode_logged = true;
            last_wire_time_valid = true;
            last_wire_time_source = time_source;
            last_wire_time_state = time_state;
            last_wire_time_status = time_status;
            last_wire_address_valid = address_valid;
            last_wire_address = address;
            last_wire_address_prefix = prefix;
            console_journal_begin();
            NET_RECORD_WRITE("[PRODUCTION-NET-MODE] mode=");
            NET_RECORD_WRITE(
                appliance_network_mode_action_string(mode) != NULL
                    ? appliance_network_mode_action_string(mode)
                    : "NONE");
            if (address_valid) {
                NET_RECORD_WRITE(" address=");
                log_production_network_address(&address);
                NET_RECORD_WRITE("/");
                NET_RECORD_U64(prefix);
            }
            NET_RECORD_WRITE(" clock=");
            NET_RECORD_WRITE(clock_ready ? "READY" : "UNSYNCHRONIZED");
            /*
             * ITEM-654: name the selected time source and the client's own
             * verdict.  `sntp=INVALID` is a declaration that was refused, not
             * a silent fallback to the gateway.
             */
            if (time_source ==
                APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID) {
                NET_RECORD_WRITE(" sntp=INVALID");
            } else if (time_server_valid) {
                NET_RECORD_WRITE(" sntp=");
                log_production_network_address(&time_server);
            } else {
                NET_RECORD_WRITE(" sntp=NONE");
            }
            NET_RECORD_WRITE(" sntp_source=");
            NET_RECORD_WRITE(
                time_source == APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_GATEWAY
                    ? "GATEWAY"
                    : (time_source ==
                               APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_EXPLICIT
                           ? "EXPLICIT"
                           : (time_source ==
                                      APPLIANCE_PRODUCTION_NETWORK_TIME_SOURCE_INVALID
                                  ? "INVALID"
                                  : "NONE")));
            NET_RECORD_WRITE(" sntp_state=");
            NET_RECORD_WRITE(appliance_time_sync_state_string(time_state));
            NET_RECORD_WRITE(" sntp_status=");
            NET_RECORD_WRITE(
                appliance_time_sync_status_string(time_status));
            NET_RECORD_WRITE("\n");
            serial_sequence_end(serial_flags);
            console_journal_end();
        }
    }
    if (wire == NULL) {
        return;
    }
    state = appliance_network_wire_state(wire);
    /*
     * ITEM-408: the addressing record is re-published when the reason or the
     * number of survived failures changes, not only when the state word
     * changes: a lifecycle that stays ACTIVE while a renewal fails is exactly
     * the case the next hardware boot has to be able to read.
     */
    {
        uint32_t dhcp_attempt = 0U;
        uint32_t dhcp_budget = 0U;

        if (appliance_network_wire_dhcp_attempts(
                wire, &dhcp_attempt, &dhcp_budget)) {
            wire_attempt = dhcp_attempt;
            wire_budget = dhcp_budget;
        }
    }
    if (state != last_wire_state || reason != last_wire_reason ||
        hold != last_wire_hold || wire_attempt != last_wire_attempt ||
        (have_wire_counters &&
         (!last_wire_counters_valid ||
          ((uint32_t)(hfoc_lwip_time_now() - last_wire_counters_ms) >=
               KERNEL_WIRE_COUNTER_REPUBLISH_MS &&
           (wire_counters.discovers_sent !=
                last_wire_counters.discovers_sent ||
            wire_counters.requests_sent !=
                last_wire_counters.requests_sent ||
            wire_counters.offers_received !=
                last_wire_counters.offers_received ||
            wire_counters.acks_received !=
                last_wire_counters.acks_received ||
            wire_counters.send_refusals !=
                last_wire_counters.send_refusals))))) {
        const struct network_ipv4_config *active =
            appliance_network_wire_active_config(wire);
        uint32_t lease_seconds = 0U;

        last_wire_state = state;
        last_wire_reason = reason;
        last_wire_hold = hold;
        last_wire_attempt = wire_attempt;
        if (have_wire_counters) {
            last_wire_counters = wire_counters;
            last_wire_counters_valid = true;
            last_wire_counters_ms = hfoc_lwip_time_now();
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();

            console_journal_begin();
            NET_RECORD_WRITE("[PRODUCTION-DHCP] state=");
            NET_RECORD_WRITE(
                appliance_network_lifecycle_state_string(state));
            if (active != NULL) {
                NET_RECORD_WRITE(" lease=ACQUIRED address=");
                log_production_network_address(&active->address);
            } else {
                NET_RECORD_WRITE(" lease=NONE");
            }
            /*
             * ITEM-408: why the lifecycle is where it is - which path, which
             * timeout, which link sample - and how many consecutive failures
             * the live address survived.  Both fields are bounded, both are
             * console-mirrored, and neither adds a line.
             */
            NET_RECORD_WRITE(" reason=");
            NET_RECORD_WRITE(
                appliance_network_lifecycle_reason_string(reason) != NULL
                    ? appliance_network_lifecycle_reason_string(reason)
                    : "UNKNOWN");
            NET_RECORD_WRITE(" hold=");
            NET_RECORD_U64(hold);
            /*
             * ITEM-416: "still asking" is a state an operator has to be able to
             * read.  A client that is on its seventh attempt of forty is not a
             * machine that gave up, and the two used to look the same.
             */
            NET_RECORD_WRITE(" attempt=");
            NET_RECORD_U64(wire_attempt);
            NET_RECORD_WRITE("/");
            NET_RECORD_U64(wire_budget);
            if (appliance_network_wire_lease_time(
                    wire, &lease_seconds)) {
                NET_RECORD_WRITE(" lease_time=");
                NET_RECORD_U64(lease_seconds);
                NET_RECORD_WRITE("s");
            }
            /*
             * ITEM-428: the wire's own view of the exchange.  `discover=` and
             * `request=` are the DHCP frames the transmit path accepted,
             * `offer=`/`ack=` the replies the server sent, and `refused=` the
             * frames the transmit path would not take - which is what a full
             * transmit ring looks like from here, and the one field that was
             * missing when the 2026-09-16 run ended on `attempt=11/40`.
             */
            if (have_wire_counters) {
                NET_RECORD_WRITE(" discover=");
                NET_RECORD_U64(wire_counters.discovers_sent);
                NET_RECORD_WRITE(" request=");
                NET_RECORD_U64(wire_counters.requests_sent);
                NET_RECORD_WRITE(" offer=");
                NET_RECORD_U64(wire_counters.offers_received);
                NET_RECORD_WRITE(" ack=");
                NET_RECORD_U64(wire_counters.acks_received);
                NET_RECORD_WRITE(" refused=");
                NET_RECORD_U64(wire_counters.send_refusals);
                /*
                 * ITEM-435: the wire's own receive view.  `wire_rx=` counts
                 * the DHCP/DNS packets that reached the client and
                 * `wire_bad=` the ones it parsed and refused, so a record
                 * whose offer and ack are still zero says whether an answer
                 * arrived at all - the difference between a dead receive path
                 * and a client that will not take the answer.
                 */
                NET_RECORD_WRITE(" wire_rx=");
                NET_RECORD_U64(wire_view.received);
                NET_RECORD_WRITE(" wire_bad=");
                NET_RECORD_U64(wire_view.malformed);
            }
            NET_RECORD_WRITE("\n");
            serial_sequence_end(serial_flags);
            console_journal_end();
        }
    }
    answer = appliance_network_wire_dns_answer(wire);
    if (answer != NULL && !last_dns_resolved) {
        last_dns_resolved = true;
        {
            const uint64_t serial_flags = serial_sequence_begin();

            console_journal_begin();
            NET_RECORD_WRITE("[PRODUCTION-DNS] state=RESOLVED answer=");
            log_production_network_address(answer);
            NET_RECORD_WRITE("\n");
            serial_sequence_end(serial_flags);
            console_journal_end();
        }
    }
}

/*
 * ITEM-581: the machine's current network and panel readiness as one report.
 *
 * The owner's installed Toshiba had no address and no panel, and no command
 * answered what the network was doing; the boot records were serial-only and
 * `bootlog` painted nothing.  The same composition is used by the console, the
 * Status menu and `diag save`, so the screen and the exported file agree, and
 * it reads only values: no DHCP attempt, no listener touch, no USB command.
 *
 * The report reads the production network, TLS, entropy and administrator
 * owners, so it is compiled only with them.  A medium that compiles none of
 * them defines the same five records with UNAVAILABLE fields in
 * kernel_status_report_profile.c: the console and `diag save` link and print
 * the records on every medium, and exactly one definition exists per build.
 */
#if defined(HFOC_PRODUCTION_TLS)
static size_t kernel_status_append(char *buffer, size_t capacity,
                                   size_t length, const char *text)
{
    if (text == NULL) {
        return length;
    }
    while (*text != '\0' && length + 1U < capacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
    return length;
}

static size_t kernel_status_append_u64(char *buffer, size_t capacity,
                                       size_t length, uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        return kernel_status_append(buffer, capacity, length, "0");
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U && length + 1U < capacity) {
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

static size_t kernel_status_append_address(
    char *buffer, size_t capacity, size_t length,
    const struct network_ipv4_address *address)
{
    if (address == NULL) {
        return kernel_status_append(buffer, capacity, length, "none");
    }
    for (uint32_t index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U) {
            length = kernel_status_append(buffer, capacity, length, ".");
        }
        length = kernel_status_append_u64(buffer, capacity, length,
                                          address->octets[index]);
    }
    return length;
}

static void kernel_status_publish(kernel_status_sink_fn sink, void *context,
                                  const char *line)
{
    if (sink != NULL) {
        sink(context, line);
    }
}

#define KERNEL_STATUS_LINE_MAX 768U


void kernel_production_status_report(kernel_status_sink_fn sink,
                                     void *context)
{
    /*
     * The serving render is bounded by APPLIANCE_PRODUCTION_SERVING_RENDER_MAX
     * (704 bytes); a smaller buffer would truncate the [PANEL] record and hide
     * the certificate and listener fields it exists to name.
     */
    char line[KERNEL_STATUS_LINE_MAX];
    size_t length;
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();

    if (sink == NULL) {
        return;
    }
    /* ---- the console mode, read-only --------------------------------- */
    /*
     * ITEM-620: the owner's 1024x768 photo answered the mode question once;
     * the shared status must answer it again without a photo or a new SSH
     * command.  This reads the framebuffer owner's immutable fields only: no
     * MMIO, no rebind, no paint.  Before the framebuffer exists the record
     * says so instead of inventing a mode.
     */
    if (framebuffer_ready()) {
        const uint64_t width = framebuffer_width();
        const uint64_t height = framebuffer_height();
        const uint32_t scale = framebuffer_font_scale(width, height);
        const uint64_t cell_width =
            (uint64_t)FRAMEBUFFER_FONT_CELL_WIDTH * scale;
        const uint64_t cell_height =
            (uint64_t)FRAMEBUFFER_FONT_CELL_HEIGHT * scale;

        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(
            line, sizeof(line), length,
            "[FB-GEOMETRY] state=READY width=");
        length = kernel_status_append_u64(line, sizeof(line), length, width);
        length = kernel_status_append(line, sizeof(line), length, " height=");
        length = kernel_status_append_u64(line, sizeof(line), length, height);
        length = kernel_status_append(line, sizeof(line), length, " bpp=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          framebuffer_bpp());
        length = kernel_status_append(line, sizeof(line), length, " scale=");
        length = kernel_status_append_u64(line, sizeof(line), length, scale);
        length = kernel_status_append(line, sizeof(line), length, " cell_px=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          cell_width);
        length = kernel_status_append(line, sizeof(line), length, "x");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          cell_height);
        length = kernel_status_append(line, sizeof(line), length, " cells=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          width / cell_width);
        length = kernel_status_append(line, sizeof(line), length, "x");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          height / cell_height);
        kernel_status_publish(sink, context, line);
    } else {
        kernel_status_publish(sink, context,
                              "[FB-GEOMETRY] state=NOT_AVAILABLE");
    }

    /* ---- the bounded RAM namespace ----------------------------------- */
    if (kernel_vfs_ready) {
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(line, sizeof(line), length,
                                      "[VFS] state=READY root=/ nodes=");
        length = kernel_status_append_u64(
            line, sizeof(line), length, vfs_node_count(&kernel_vfs));
        length = kernel_status_append(line, sizeof(line), length, " open=");
        length = kernel_status_append_u64(
            line, sizeof(line), length, vfs_open_count(&kernel_vfs));
        length = kernel_status_append(line, sizeof(line), length,
                                      " data_bytes=");
        length = kernel_status_append_u64(
            line, sizeof(line), length, vfs_data_used(&kernel_vfs));
        length = kernel_status_append(line, sizeof(line), length,
                                      " data_capacity=");
        length = kernel_status_append_u64(
            line, sizeof(line), length, VFS_DATA_MAX);
        kernel_status_publish(sink, context, line);
    } else {
        kernel_status_publish(sink, context,
                              "[VFS] state=NOT_AVAILABLE");
    }

    /* ---- the driver's own view, always available --------------------- */
    length = 0U;
    line[0] = '\0';
    length = kernel_status_append(line, sizeof(line), length,
                                  "[NET-STATUS] device=");
    length = kernel_status_append(
        line, sizeof(line), length,
        appliance_production_network_boot_device_state());
    length = kernel_status_append(line, sizeof(line), length, " readiness=");
    length = kernel_status_append(
        line, sizeof(line), length,
        appliance_production_network_readiness_string(
            appliance_production_network_boot_readiness()));
    length = kernel_status_append(line, sizeof(line), length, " backend=");
    length = kernel_status_append(
        line, sizeof(line), length,
        appliance_production_network_boot_backend());
    {
        uint16_t vendor = 0U;
        uint16_t product = 0U;
        uint8_t revision = 0U;

        if (appliance_production_network_boot_pci_identity(
                &vendor, &product, &revision)) {
            length = kernel_status_append(line, sizeof(line), length, " pci=");
            length = kernel_status_append_hex16(line, sizeof(line), length,
                                                vendor);
            length = kernel_status_append(line, sizeof(line), length, ":");
            length = kernel_status_append_hex16(line, sizeof(line), length,
                                                product);
            length = kernel_status_append(line, sizeof(line), length, " rev=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              revision);
        }
    }
    length = kernel_status_append(line, sizeof(line), length, " link=");
    length = kernel_status_append(
        line, sizeof(line), length,
        appliance_production_network_boot_link_up() ? "UP" : "DOWN");
    length = kernel_status_append(line, sizeof(line), length, " mode=");
    length = kernel_status_append(
        line, sizeof(line), length,
        appliance_network_mode_action_string(
            appliance_production_network_wire_boot_mode()));
    length = kernel_status_append(line, sizeof(line), length, " stage=");
    length = kernel_status_append_u64(
        line, sizeof(line), length,
        appliance_production_network_boot_stage());
    {
        const char *reason = appliance_production_network_boot_reason();

        if (reason != NULL && reason[0] != '\0') {
            length = kernel_status_append(line, sizeof(line), length,
                                          " reason=");
            length = kernel_status_append(line, sizeof(line), length, reason);
        }
    }
    kernel_status_publish(sink, context, line);

    /* ---- the addressing lifecycle ------------------------------------ */
    length = 0U;
    line[0] = '\0';
    length = kernel_status_append(line, sizeof(line), length, "[NET-ADDR] state=");
    if (wire == NULL) {
        length = kernel_status_append(line, sizeof(line), length,
                                      "UNAVAILABLE reason=NO_WIRE");
        kernel_status_publish(sink, context, line);
    } else {
        const struct network_ipv4_config *active =
            appliance_network_wire_active_config(wire);
        struct appliance_network_wire_dhcp_counters counters;
        struct appliance_production_network_wire_view view;
        uint32_t attempt = 0U;
        uint32_t budget = 0U;
        uint32_t lease_seconds = 0U;
        const char *reason =
            appliance_network_wire_lifecycle(wire) != NULL
                ? appliance_network_lifecycle_reason_string(
                      appliance_network_lifecycle_reason(
                          appliance_network_wire_lifecycle(wire)))
                : NULL;

        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_network_lifecycle_state_string(
                appliance_network_wire_state(wire)));
        if (active != NULL) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " lease=ACQUIRED address=");
            length = kernel_status_append_address(line, sizeof(line), length,
                                                  &active->address);
            length = kernel_status_append(line, sizeof(line), length, "/");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              active->prefix_length);
            length = kernel_status_append(line, sizeof(line), length,
                                          " gateway=");
            length = kernel_status_append_address(line, sizeof(line), length,
                                                  &active->gateway);
            length = kernel_status_append(line, sizeof(line), length, " dns=");
            length = kernel_status_append_address(line, sizeof(line), length,
                                                  &active->dns);
        } else {
            length = kernel_status_append(line, sizeof(line), length,
                                          " lease=NONE");
        }
        /*
         * ITEM-612: the active netif's own MAC travels with the address, so a
         * peer's neighbour entry can be compared with the interface the
         * appliance actually bound - not with a value inferred from the chip
         * model.  An address the wire never copied is omitted, never zeroed.
         */
        {
            char mac[APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX];

            if (appliance_production_network_mac_text(
                    wire->local_mac, mac, sizeof(mac)) != 0U) {
                length = kernel_status_append(line, sizeof(line), length,
                                              " mac=");
                length = kernel_status_append(line, sizeof(line), length, mac);
            }
        }
        (void)appliance_network_wire_dhcp_attempts(wire, &attempt, &budget);
        if (appliance_network_wire_lease_time(wire, &lease_seconds)) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " lease_time=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              lease_seconds);
            length = kernel_status_append(line, sizeof(line), length, "s");
        }
        length = kernel_status_append(line, sizeof(line), length, " attempt=");
        length = kernel_status_append_u64(line, sizeof(line), length, attempt);
        length = kernel_status_append(line, sizeof(line), length, "/");
        length = kernel_status_append_u64(line, sizeof(line), length, budget);
        if (reason != NULL) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " reason=");
            length = kernel_status_append(line, sizeof(line), length, reason);
        }
        memset(&counters, 0, sizeof(counters));
        if (appliance_network_wire_dhcp_counters(wire, &counters)) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " discover=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              counters.discovers_sent);
            length = kernel_status_append(line, sizeof(line), length,
                                          " request=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              counters.requests_sent);
            length = kernel_status_append(line, sizeof(line), length,
                                          " offer=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              counters.offers_received);
            length = kernel_status_append(line, sizeof(line), length, " ack=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              counters.acks_received);
            length = kernel_status_append(line, sizeof(line), length,
                                          " refused=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              counters.send_refusals);
        }
        memset(&view, 0, sizeof(view));
        appliance_production_network_boot_wire_view(&view);
        if (view.available) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " wire_rx=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              view.received);
            length = kernel_status_append(line, sizeof(line), length,
                                          " wire_bad=");
            length = kernel_status_append_u64(line, sizeof(line), length,
                                              view.malformed);
        }
        kernel_status_publish(sink, context, line);
    }

    /* ---- the frame path behind the address --------------------------- */
    {
        struct appliance_production_network_io_counters counters;
        char record[APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX];

        memset(&counters, 0, sizeof(counters));
        appliance_production_network_boot_io_counters(&counters);
        /*
         * ITEM-612: the same ring and class counters the heartbeat writes to
         * the boot journal, now in the shared snapshot as well.  An export
         * that proved link-up and a lease but could not say whether the
         * peer's ARP/ICMP/TCP arrived was the gap this record closes.
         */
        if (appliance_production_network_io_record_line(
                &counters, record, sizeof(record)) != 0U) {
            kernel_status_publish(sink, context, record);
        }
        if (counters.available &&
            appliance_production_network_frame_record_line(
                &counters, record, sizeof(record)) != 0U) {
            kernel_status_publish(sink, context, record);
        }
        /*
         * ITEM-643: the receive path's live register/descriptor state, so an
         * export taken while the peer's probe is unanswered names whether the
         * ring was armed, what the part reported and which descriptors it
         * still owned.  An absent backend prints its named reason.
         */
        if (appliance_production_network_rx_record_line(
                &counters, record, sizeof(record)) != 0U) {
            kernel_status_publish(sink, context, record);
        }
    }

    /* ---- the PHY behind the link bit --------------------------------- */
    {
        struct appliance_production_network_phy_info info;
        char phy[APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX];

        memset(&info, 0, sizeof(info));
        appliance_production_network_boot_phy_info(&info);
        /*
         * ITEM-590: the boot journal and this report print one composition.
         * Composing the record twice is how the two copies could disagree
         * about a field the next hardware run depends on.
         */
        if (appliance_production_network_phy_record_line(&info, phy,
                                                        sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        /* ITEM-592: the cold start's evidence travels with the report, so a
           `diag save` file localises the stage instead of only the link. */
        if (appliance_production_network_phy_boot_line(&info, phy,
                                                       sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        if (appliance_production_network_phy_gigabit_line(&info, phy,
                                                          sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        if (appliance_production_network_phy_sample_line(&info, phy,
                                                         sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        if (appliance_production_network_phy_failure_line(&info, phy,
                                                          sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        if (appliance_production_network_phy_page_line(&info, phy,
                                                       sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
        if (appliance_production_network_phy_select_line(&info, phy,
                                                         sizeof(phy)) != 0U) {
            kernel_status_publish(sink, context, phy);
        }
    }

    /* ---- the HTTPS panel gate ---------------------------------------- */
    {
        char serving[APPLIANCE_PRODUCTION_SERVING_RENDER_MAX];
        uint32_t serving_length = 0U;

        memset(serving, 0, sizeof(serving));
        appliance_production_serving_render(serving, sizeof(serving),
                                            &serving_length);
        if (serving_length >= sizeof(serving)) {
            serving_length = sizeof(serving) - 1U;
        }
        serving[serving_length] = '\0';
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(line, sizeof(line), length, "[PANEL] ");
        length = kernel_status_append(line, sizeof(line), length, serving);
        kernel_status_publish(sink, context, line);
    }

    /* ---- the SSH diagnostic endpoint -------------------------------- */
    {
        char ssh[320];
        size_t ssh_length;

        ssh[0] = '\0';
        ssh_length = appliance_production_ssh_render(
            &production_ssh, ssh, sizeof(ssh));
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(line, sizeof(line), length,
                                      "[PRODUCTION-SSH] ");
        length = kernel_status_append(line, sizeof(line), length, ssh);
        if (ssh_length >= sizeof(ssh)) {
            length = kernel_status_append(line, sizeof(line), length,
                                          " truncated=yes");
        }
        kernel_status_publish(sink, context, line);
    }

    /* ---- the SSH public host key for the diagnostic medium ---------- */
    {
        char host_address[32];
        char hostkey[512];
        size_t hostkey_length;

        host_address[0] = '\0';
        if (wire != NULL) {
            const struct network_ipv4_config *active =
                appliance_network_wire_active_config(wire);

            if (active != NULL) {
                (void)kernel_status_append_address(
                    host_address, sizeof(host_address), 0U,
                    &active->address);
            }
        }
        hostkey[0] = '\0';
        hostkey_length = appliance_production_ssh_render_hostkey(
            &production_ssh,
            host_address[0] != '\0' ? host_address : "none",
            APPLIANCE_PRODUCTION_SSH_PORT, hostkey, sizeof(hostkey));
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(
            line, sizeof(line), length, "[PRODUCTION-SSH-HOSTKEY] ");
        length = kernel_status_append(
            line, sizeof(line), length, hostkey);
        if (hostkey_length >= sizeof(hostkey)) {
            length = kernel_status_append(
                line, sizeof(line), length, " truncated=yes");
        }
        kernel_status_publish(sink, context, line);
    }

    /* ---- the entropy owner ------------------------------------------- */
    {
        uint32_t max_leaf = 0U;
        bool rdrand = false;
        bool rdseed = false;

        appliance_production_entropy_cpu_flags(&max_leaf, &rdrand, &rdseed);
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(line, sizeof(line), length,
                                      "[RNG] source=");
        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_production_entropy_source_string());
        length = kernel_status_append(line, sizeof(line), length, " state=");
        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_production_entropy_state_string(
                appliance_production_entropy_state()));
        length = kernel_status_append(line, sizeof(line), length, " stage=");
        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_production_entropy_stage_string(
                appliance_production_entropy_failure_stage()));
        length = kernel_status_append(line, sizeof(line), length,
                                      " cpuid_max=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          max_leaf);
        length = kernel_status_append(line, sizeof(line), length, " rdrand=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          rdrand ? 1U : 0U);
        length = kernel_status_append(line, sizeof(line), length, " rdseed=");
        length = kernel_status_append_u64(line, sizeof(line), length,
                                          rdseed ? 1U : 0U);
        length = kernel_status_append(line, sizeof(line), length,
                                      " credited=");
        length = kernel_status_append_u64(
            line, sizeof(line), length,
            appliance_production_entropy_credited_bytes());
        length = kernel_status_append(line, sizeof(line), length, " calls=");
        length = kernel_status_append_u64(
            line, sizeof(line), length,
            appliance_production_entropy_random_calls());
        length = kernel_status_append(line, sizeof(line), length,
                                      " rotations=");
        length = kernel_status_append_u64(
            line, sizeof(line), length,
            appliance_production_entropy_rotations());
        kernel_status_publish(sink, context, line);
    }

    /* ---- the administrator enrollment -------------------------------- */
    {
        char admin[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
        size_t admin_length = 0U;

        memset(admin, 0, sizeof(admin));
        if (appliance_production_admin_enrollment_render(
                &production_admin_enrollment, admin, sizeof(admin),
                &admin_length) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
            admin_length < sizeof(admin)) {
            admin[admin_length] = '\0';
        } else {
            admin_length = 0U;
            admin[0] = '\0';
        }
        length = 0U;
        line[0] = '\0';
        length = kernel_status_append(line, sizeof(line), length,
                                      "[ADMIN] state=");
        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_production_admin_enrollment_state_string(
                appliance_production_admin_enrollment_state(
                    &production_admin_enrollment)));
        length = kernel_status_append(line, sizeof(line), length, " last=");
        length = kernel_status_append(
            line, sizeof(line), length,
            appliance_production_admin_enrollment_status_string(
                appliance_production_admin_enrollment_last_status(
                    &production_admin_enrollment)));
        if (admin_length != 0U) {
            length = kernel_status_append(line, sizeof(line), length, " ");
            length = kernel_status_append(line, sizeof(line), length, admin);
        }
        kernel_status_publish(sink, context, line);
    }

    /* ---- the last finished local login attempt ----------------------- */
    /*
     * ITEM-614: the same bounded record the serial stream carried, kept after
     * the attempt finished.  `diag save`/the SSH stream compose this report
     * later, so a late medium, a repeated save or a reset of the export
     * buffers cannot lose the measured latency.  With no attempt at all the
     * record says NOT_ATTEMPTED; an attempt in progress never shows the
     * previous success.
     */
    kernel_login_trace_status_report(sink, context);

    /* ---- the last browser sign-in attempt ---------------------------- */
    /*
     * ITEM-617: the panel credential adapter runs the account KDF inside the
     * serving poll.  The trace keeps only its attempt, start, duration and
     * result so the next hardware snapshot can name the loop-blocking stage
     * without carrying a credential.
     */
    appliance_panel_auth_trace_status_report(sink, context);

    /* ---- the retained diagnostic-save refusal ------------------------ */
    /*
     * ITEM-615: ITEM-558's record survived an attempt but only the local
     * `diag` command read it; the 64-record boot ring had already evicted the
     * hardware refusal, so the next SSH read could not name it.  Publish the
     * existing record through this one composition, read-only, so `network`,
     * Status, `diag save` and the SSH diag stream all carry the first failure.
     */
    diag_failure_status_report(diag_export_last_failure(), sink, context);
}
#endif /* HFOC_PRODUCTION_TLS */

/*
 * ITEM-394: the management binding is owned here so the console publication
 * can state the scope that is actually in force, and so the boot entry can
 * opt the management plane in to the operator's network.  The default stays
 * LOCAL_ONLY: no network ingress until the operator declares otherwise.
 */
static struct appliance_management_bind production_management_bind;
static char production_management_last_url[
    APPLIANCE_BOOT_CONSOLE_URL_MAX + 1U];
/*
 * ITEM-449: the relay owner.  It is published only when the install entry
 * declared a forward and the live netif could be bound to; its records say
 * LISTENING, a refusal with its source, or a session with its verdict and
 * counters, and an owner that could not start is published as UNAVAILABLE
 * instead of staying silent.
 */
#define APPLIANCE_PRODUCTION_FORWARD_GENERATION 1U
static struct hfoc_lwip_forward production_forward;
static bool production_forward_published;

static const char *boot_command_line(void)
{
    if (executable_file_request.response == NULL ||
        executable_file_request.response->executable_file == NULL) {
        return NULL;
    }
    return executable_file_request.response->executable_file->string;
}

static size_t boot_command_line_length(void)
{
    const char *command_line = boot_command_line();

    return command_line != NULL ? strlen(command_line) : 0U;
}

static const char *production_management_reachability(void)
{
    return appliance_management_bind_scope_string(
        appliance_management_bind_scope_of(&production_management_bind));
}

static const char *production_management_certificate_state(void)
{
#ifdef HFOC_PRODUCTION_TLS
    if (!appliance_production_certificate_available(
            &production_serving_certificate)) {
        return "unavailable";
    }
    return production_serving_certificate.persisted
               ? "persisted_self_signed"
               : "ephemeral_self_signed";
#else
    return "unavailable";
#endif
}

static bool production_management_url(
    char *buffer,
    size_t capacity,
    const struct network_ipv4_address *address)
{
    static const char prefix[] = "https://";
    size_t length = 0U;
    size_t index;

    if (buffer == NULL || capacity == 0U || address == NULL) {
        return false;
    }
    for (index = 0U; index + 1U < capacity && prefix[index] != '\0';
         ++index) {
        buffer[length++] = prefix[index];
    }
    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        char digits[4U];
        size_t count = 0U;
        uint32_t value = address->octets[index];

        do {
            digits[count++] = (char)('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U && count < sizeof(digits));
        while (count != 0U) {
            if (length + 2U >= capacity) {
                return false;
            }
            buffer[length++] = digits[--count];
        }
        if (index + 1U < NETWORK_IPV4_ADDRESS_OCTETS) {
            if (length + 2U >= capacity) {
                return false;
            }
            buffer[length++] = '.';
        }
    }
    if (length + 2U >= capacity) {
        return false;
    }
    buffer[length++] = '/';
    buffer[length] = '\0';
    return true;
}

/*
 * ITEM-394: publish the management address and URL where an operator without
 * a serial capture can read them.  The record is written once per address, so
 * a DHCP renewal that changes the address republishes instead of leaving a
 * stale URL on screen.
 */
static void publish_production_management(void)
{
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct network_ipv4_config *active =
        wire != NULL ? appliance_network_wire_active_config(wire) : NULL;
    char url[APPLIANCE_BOOT_CONSOLE_URL_MAX + 1U];

    if (active == NULL ||
        !production_management_url(url, sizeof(url), &active->address) ||
        strcmp(production_management_last_url, url) == 0) {
        return;
    }
    for (size_t index = 0U; index < sizeof(url); ++index) {
        production_management_last_url[index] = url[index];
        if (url[index] == '\0') {
            break;
        }
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /* ITEM-410: mirrored and stored - the management address, scope and
         * certificate state the hardware run's MANAGEMENT stage quotes. */
        console_journal_begin();
        serial_write("[PRODUCTION-MGMT] url=");
        console_record_text("[PRODUCTION-MGMT] url=");
        serial_write(url);
        console_record_text(url);
        serial_write(" reachability=");
        console_record_text(" reachability=");
        serial_write(production_management_reachability());
        console_record_text(production_management_reachability());
        serial_write(" cert=");
        console_record_text(" cert=");
        serial_write(production_management_certificate_state());
        console_record_text(production_management_certificate_state());
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
    console_publish_management(
        url, strlen(url), production_management_reachability(),
        production_management_certificate_state());
}

static struct appliance_production_first_boot production_first_boot;
static bool production_first_boot_ready;
static enum appliance_production_first_boot_state last_first_boot_state;

static size_t production_first_boot_text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void log_production_first_boot(void)
{
    enum appliance_production_first_boot_state state;

    if (!production_first_boot_ready ||
        production_first_boot.state == last_first_boot_state) {
        return;
    }
    last_first_boot_state = production_first_boot.state;
    state = production_first_boot.state;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        const char *first_boot_state_text;

        switch (state) {
        case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED:
            first_boot_state_text = "provisioned";
            break;
        case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED:
            first_boot_state_text = "reloaded";
            break;
        case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK:
            first_boot_state_text = "awaiting_network";
            break;
        case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED:
            first_boot_state_text = "failed";
            break;
        default:
            first_boot_state_text = "awaiting_storage";
            break;
        }
        /* ITEM-410: mirrored and stored - the first-boot state the hardware
         * run's FIRST_BOOT stage quotes. */
        console_journal_begin();
        serial_write("[PRODUCTION-FIRST-BOOT] first_boot=");
        console_record_text("[PRODUCTION-FIRST-BOOT] first_boot=");
        serial_write(first_boot_state_text);
        console_record_text(first_boot_state_text);
        serial_write(" hostname=");
        console_record_text(" hostname=");
        serial_write(production_first_boot.hostname);
        console_record_text(production_first_boot.hostname);
        /*
         * ITEM-403: name what this first-boot flow did with the administrator
         * credential.  A reloaded appliance publishes no field here: the
         * [ADMIN-ENROLLMENT] record owns the persisted credential state.
         */
        if (state == APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED) {
            const char *admin_text =
                appliance_production_first_boot_admin_provisioned(
                    &production_first_boot)
                    ? "provisioned"
                    : "deferred";

            serial_write(" admin=");
            console_record_text(" admin=");
            serial_write(admin_text);
            console_record_text(admin_text);
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
}

#ifdef HFOC_PRODUCTION_SERVICES
static enum appliance_production_service_activation_state
    last_production_service_state;

static void log_production_service(void)
{
    const struct appliance_production_service_activation *activation =
        appliance_production_service_boot_activation();
    struct appliance_production_service_activation_snapshot snapshot;
    char output[APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    if (activation == NULL ||
        appliance_production_service_activation_snapshot(
            activation, &snapshot) !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK ||
        appliance_production_service_activation_render(
            &snapshot, output, sizeof(output), &output_length) !=
            APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK) {
        return;
    }
    if (snapshot.state == last_production_service_state) {
        return;
    }
    last_production_service_state = snapshot.state;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PRODUCTION-SERVICE] ");
        serial_write(output);
        serial_sequence_end(serial_flags);
    }
}

#ifdef HFOC_PRODUCTION_TLS
/*
 * ITEM-386: publish the production serving record.  One line, written when the
 * serving state or one of its bounded counters changes, so the proof and an
 * operator see connections, denials and handshake failures as they happen.
 * when the listening state changes, carrying the listener state, the TLS
 * profile, the reachability mode, the bind scope and the bounded counters.
 * It never carries a key, a certificate payload or a session secret.
 */
static enum appliance_production_serving_state
    last_production_serving_state;
static uint32_t last_production_serving_counters;
static bool last_production_serving_logged;
/*
 * ITEM-461: the serving counters move every tick, so the record above was
 * published on almost every loop turn and the owner's 2026-09-20 screen was
 * flooded with it.  A state change is still immediate; an unchanged state is
 * published at most once per this interval.
 */
#define PRODUCTION_SERVING_PUBLISH_INTERVAL_MS 2000U
static uint32_t last_production_serving_publish_ms;

static uint32_t production_serving_counter_stamp(void)
{
    return appliance_production_serving_accept_count() +
           appliance_production_serving_request_count() +
           appliance_production_serving_denied_count() +
           appliance_production_serving_rejected_count() +
           appliance_production_serving_handshake_failure_count() +
           appliance_production_serving_rx_bytes() +
           appliance_production_serving_tls_attempts() +
           /*
            * ITEM-503: an operator certificate import changes no accept or
            * request counter, so without this the durable record that says
            * "applies on reboot" would never be republished.
            */
           appliance_production_serving_certificate_imports() * 1000003U;
}

static void log_production_serving(void)
{
    char output[APPLIANCE_PRODUCTION_SERVING_RENDER_MAX + 1U];
    uint32_t output_length = 0U;
    const enum appliance_production_serving_state state =
        appliance_production_serving_state();
    const uint32_t counters = production_serving_counter_stamp();
    const uint32_t now_ms = hfoc_lwip_time_now();

    if (last_production_serving_logged &&
        state == last_production_serving_state) {
        if (counters == last_production_serving_counters) {
            return;
        }
        if ((uint32_t)(now_ms - last_production_serving_publish_ms) <
            PRODUCTION_SERVING_PUBLISH_INTERVAL_MS) {
            return;
        }
    }
    last_production_serving_counters = counters;
    appliance_production_serving_render(
        output, sizeof(output), &output_length);
    if (output_length == 0U) {
        return;
    }
    last_production_serving_state = state;
    last_production_serving_logged = true;
    last_production_serving_publish_ms = now_ms;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /* ITEM-410: mirrored and stored - the serving publication the
         * MANAGEMENT stage quotes. */
        serial_write("[PRODUCTION-SERVING] ");
        serial_write(output);
        serial_write("\n");
        serial_sequence_end(serial_flags);
        {
            static const char serving_tag[] = "[PRODUCTION-SERVING] ";
            char record[APPLIANCE_PRODUCTION_SERVING_RENDER_MAX +
                        sizeof(serving_tag)];
            size_t record_length = 0U;

            memcpy(record, serving_tag, sizeof(serving_tag) - 1U);
            record_length = sizeof(serving_tag) - 1U;
            if (output_length <= sizeof(record) - record_length) {
                memcpy(record + record_length, output, output_length);
                record_length += output_length;
            }
            console_publish_journal(record, record_length);
        }
    }
}

/*
 * ITEM-508 increment 26: the ACME service record.  One line per state the
 * service actually reaches - waiting for the identity, a named account
 * refusal, or the bound account with the public thumbprint and the renewal
 * decision - mirrored into the durable log exactly as the serving record is.
 * The same text is not repeated while the state does not move.
 */
static bool last_production_acme_service_logged;
static char last_production_acme_service_record[
    APPLIANCE_PRODUCTION_ACME_SERVICE_RECORD_MAX + 1U];

static void log_production_acme_service(void)
{
    const char *record = appliance_production_acme_service_record(
        &production_acme_service);
    size_t record_length;

    if (record == NULL) {
        return;
    }
    record_length = strlen(record);
    if (record_length == 0U ||
        record_length > APPLIANCE_PRODUCTION_ACME_SERVICE_RECORD_MAX) {
        return;
    }
    if (last_production_acme_service_logged &&
        strcmp(record, last_production_acme_service_record) == 0) {
        return;
    }
    memcpy(last_production_acme_service_record, record, record_length + 1U);
    last_production_acme_service_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, record_length);
}

/*
 * ITEM-508 increment 32: the same ACME state as the Certificates page's
 * read-only block.  The panel view carries text, so the two numbers the
 * renewal policy owns are rendered here; every string points into static or
 * module-owned storage that outlives the render call, and the service getters
 * never expose a key, a nonce, a token or a certificate byte.  The call is
 * cheap enough for every turn: the serving path republishes the page only when
 * the view really changed.
 */
static char production_acme_panel_expires[16];
static char production_acme_panel_attempts[16];

static void production_acme_panel_u32(
    uint32_t value, char *output, size_t capacity)
{
    char digits[10];
    size_t count = 0U;
    size_t index;

    if (output == NULL || capacity == 0U) {
        return;
    }
    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    if (count + 1U > capacity) {
        output[0] = '\0';
        return;
    }
    for (index = 0U; index < count; ++index) {
        output[index] = digits[count - 1U - index];
    }
    output[count] = '\0';
}

static void publish_production_acme_panel_view(void)
{
    const struct appliance_acme_renewal *renewal =
        appliance_production_acme_service_renewal(&production_acme_service);
    struct appliance_management_web_ui_acme view;

    if (renewal == NULL) {
        return;
    }
    memset(&view, 0, sizeof(view));
    view.state = appliance_production_acme_service_state_string(
        appliance_production_acme_service_state(&production_acme_service));
    view.account = appliance_production_acme_service_account_bound(
                       &production_acme_service)
                       ? appliance_acme_account_status_string(
                             appliance_production_acme_service_account_status(
                                 &production_acme_service))
                       : "unbound";
    view.renewal = appliance_acme_renewal_decision_string(
        appliance_acme_renewal_last_decision(renewal));
    production_acme_panel_u32(
        appliance_acme_renewal_expires_at(renewal),
        production_acme_panel_expires, sizeof(production_acme_panel_expires));
    view.expires = production_acme_panel_expires;
    production_acme_panel_u32(
        appliance_acme_renewal_attempts(renewal),
        production_acme_panel_attempts,
        sizeof(production_acme_panel_attempts));
    view.attempts = production_acme_panel_attempts;
    /* ITEM-508 increment 37: the order path's own state and step. */
    view.order = appliance_production_acme_order_boot_state();
    view.published = true;
    (void)appliance_production_serving_publish_acme(&view);
}

/*
 * ITEM-508 increment 28: the HTTP-01 listener record.  One bounded line per
 * state the listener actually reaches, with the port, whether a token is armed
 * and the connection counters, mirrored into the durable log exactly as the
 * ACME service record is.  The same text is not repeated while nothing moves.
 */
static bool last_production_acme_http01_logged;
static char last_production_acme_http01_record[
    APPLIANCE_PRODUCTION_ACME_HTTP01_RECORD_MAX + 1U];

static void log_production_acme_http01(void)
{
    const char *record = appliance_production_acme_http01_record(
        &production_acme_http01);
    size_t record_length;

    if (record == NULL) {
        return;
    }
    record_length = strlen(record);
    if (record_length == 0U ||
        record_length > APPLIANCE_PRODUCTION_ACME_HTTP01_RECORD_MAX) {
        return;
    }
    if (last_production_acme_http01_logged &&
        strcmp(record, last_production_acme_http01_record) == 0) {
        return;
    }
    memcpy(last_production_acme_http01_record, record, record_length + 1U);
    last_production_acme_http01_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, record_length);
}
#endif

/*
 * ITEM-504 increment 5: the routing configuration's own record.  One line per
 * terminal state - a route the commit applied, a candidate a cancel dropped, or
 * a refusal that applied nothing, named with the model's own reason.  The body
 * is rendered next to the state that decided it; this writer only prefixes the
 * marker and mirrors the line into the durable log, exactly as the ITEM-449
 * relay record does for its own events.
 */
#ifdef HFOC_PRODUCTION_TLS
static void log_production_route(void)
{
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_route_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write("[PRODUCTION-ROUTE] ");
        console_record_text("[PRODUCTION-ROUTE] ");
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-504 increment 6: the table the durable store handed back at boot.  It
 * is published once, next to the routing record, and names the source and the
 * whole applied table as the model renders it; a boot that read no store says
 * so instead of implying an empty configuration it did not verify.
 */
static void log_production_routes(void)
{
    /*
     * ITEM-504 increment 6: a whole rendered table does not belong on the boot
     * stack, exactly as the certificate record does not; the loop is
     * single-threaded and this body is produced once per boot.
     */
    static char record[APPLIANCE_ROUTE_LIST_MAX + 96U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_route_restore_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write("[PRODUCTION-ROUTES] ");
        console_record_text("[PRODUCTION-ROUTES] ");
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-505: the network configuration's own record.  One line per terminal
 * state - a commit that wrote the durable record, a commit whose write failed,
 * or a stage the executor refused - with the source, the candidate, whether
 * the write proved itself and what the interface still runs.  The body already
 * carries the `[PRODUCTION-NET-SET]` marker, exactly as the console menu's own
 * network record does, so this writer only mirrors it into the durable log.
 */
static void log_production_network_set(void)
{
    static char record[APPLIANCE_PRODUCTION_SERVING_NETWORK_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_network_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-505 increment 3: the system hostname's own record.  One line per
 * terminal state - a commit that wrote the durable record, a commit whose
 * write failed, or a stage the executor refused - with the source, the
 * confirmed hostname, whether the write proved itself and the identity the
 * running system still serves.  The body already carries the
 * `[PRODUCTION-SYSTEM-SET]` marker, so this writer only mirrors it into the
 * durable log.
 */
static void log_production_system_set(void)
{
    static char record[APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_system_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-506 increment 6: the account credential change's own record.  One line
 * per terminal state of the commit - a commit that wrote the durable record, a
 * commit whose write or republish failed, or a commit refused because another
 * writer changed the account inside the window - with the source, the login
 * the operator confirmed, whether the login moved, whether the write proved
 * itself and whether the owner closed the issued sessions.  The body already
 * carries the `[PRODUCTION-CREDENTIAL-SET]` marker and never carries the
 * password, so this writer only mirrors it into the durable log.
 */
static void log_production_account_set(void)
{
    static char record[APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_account_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-507 increment 3: the SNI decision the handshake left.  The callback
 * runs inside mbedTLS, where the journal cannot be written, so the serving
 * path stores one bounded line and the loop publishes it here: which name the
 * client presented, which certificate was chosen and why.  The record never
 * carries certificate or key bytes.
 */
static void log_production_sni(void)
{
    static char record[APPLIANCE_PRODUCTION_SERVING_SNI_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_sni_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-507 increment 4: the name -> certificate/key mutation left its record.
 * It names the action, the exact name, how many names the durable record now
 * carries, the default declaration and whether the write proved itself; it
 * never carries certificate or key bytes.
 */
static void log_production_certificate_name(void)
{
    static char record[
        APPLIANCE_PRODUCTION_SERVING_CERTIFICATE_NAME_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_certificate_name_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-508 increment 34: the ACME control mutation left its record.  It names
 * the action, the committed enabled/requested values, the instant of a pending
 * request and whether the durable write proved itself; it never carries a key,
 * a nonce or a token.
 */
static void log_production_acme_control(void)
{
    static char record[APPLIANCE_PRODUCTION_SERVING_ACME_RECORD_MAX + 1U];
    uint32_t record_length = 0U;

    if (!appliance_production_serving_take_acme_record(
            record, (uint32_t)sizeof(record), &record_length)) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-508 increment 35: the order driver's record.  It names the state, the
 * step, the members and the leaf expiry on success, or the named refusal; it
 * carries no nonce, token, key or certificate byte.  The driver republishes
 * on every transition, and this writer mirrors only what moved.
 */
static void log_production_acme_order(void)
{
    const char *record = appliance_production_acme_order_boot_record();
    uint32_t record_length = 0U;

    if (record == NULL) {
        return;
    }

    while (record[record_length] != '\0' &&
           record_length <
               (uint32_t)sizeof(last_production_acme_order_record)) {
        ++record_length;
    }
    if (last_production_acme_order_logged &&
        strcmp(record, last_production_acme_order_record) == 0) {
        return;
    }
    if (record_length >=
        (uint32_t)sizeof(last_production_acme_order_record)) {
        return;
    }
    memcpy(last_production_acme_order_record, record, record_length + 1U);
    last_production_acme_order_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(record);
        console_record_text(record);
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}
#endif

/*
 * ITEM-375: publish the management TLS lifecycle record.  The record carries
 * the frozen policy, the honest certificate readiness and the HSTS lifetime,
 * and never carries a private key or a certificate payload.
 */
static enum appliance_management_tls_state last_production_tls_state;
static bool last_production_tls_logged;

static void log_production_tls(void)
{
    const struct appliance_management_tls *tls =
        appliance_production_service_boot_management_tls();
    struct appliance_management_tls_snapshot snapshot;
    char output[APPLIANCE_MANAGEMENT_TLS_RENDER_MAX + 1U];
    size_t output_length = 0U;

    if (tls == NULL ||
        appliance_management_tls_snapshot(
            tls,
            appliance_production_network_time_epoch_seconds(
                hfoc_lwip_time_now()),
            &snapshot) != APPLIANCE_MANAGEMENT_TLS_OK ||
        appliance_management_tls_render(
            &snapshot, output, sizeof(output), &output_length) !=
            APPLIANCE_MANAGEMENT_TLS_OK) {
        return;
    }
    if (last_production_tls_logged &&
        snapshot.state == last_production_tls_state) {
        return;
    }
    last_production_tls_state = snapshot.state;
    last_production_tls_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PRODUCTION-TLS] ");
        serial_write(output);
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-376: publish the durable update lifecycle record.  The record carries
 * the boot decision taken from the durable record -- the installed image, the
 * active slot, the attempt count and whether a rollback was selected -- so an
 * interrupted update is visible on the wire instead of being inferred.
 */
static bool last_production_update_logged;

static void log_production_update(void)
{
    const struct appliance_update_lifecycle *lifecycle =
        appliance_production_update_boot_lifecycle();
    struct appliance_update_lifecycle_snapshot snapshot;
    char output[APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX + 1U];
    size_t output_length = 0U;

    if (!appliance_production_update_boot_available()) {
        if (last_production_update_logged) {
            return;
        }
        last_production_update_logged = true;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write(
                "[UPDATE-ATOMICITY] update_lifecycle v=1 "
                "state=UNAVAILABLE reason=NO_DURABLE_STORE\n");
            serial_sequence_end(serial_flags);
        }
        return;
    }
    if (lifecycle == NULL || last_production_update_logged) {
        return;
    }
    if (appliance_update_lifecycle_snapshot(lifecycle, &snapshot) !=
            APPLIANCE_UPDATE_LIFECYCLE_OK ||
        appliance_update_lifecycle_render(
            &snapshot, output, sizeof(output), &output_length) !=
            APPLIANCE_UPDATE_LIFECYCLE_OK) {
        return;
    }
    last_production_update_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[UPDATE-ATOMICITY] ");
        serial_write(output);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-377: publish the hash-chained audit window.  Ordinary boot builds the
 * chain from the boot record and the structured audit records the kernel event
 * ring already holds, verifies it, runs one bounded external export with a
 * cursor and acknowledges every exported record, so the wire carries the exact
 * chain state instead of a claim that an audit trail exists.
 */
static bool last_production_audit_logged;

static void log_production_audit(void)
{
    char output[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX + 1U];
    size_t output_length = 0U;

    if (last_production_audit_logged ||
        !appliance_production_audit_boot_render(
            output, sizeof(output), &output_length)) {
        return;
    }
    last_production_audit_logged = true;
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /* ITEM-410: mirrored and stored - the audit chain verdict. */
        static const char audit_tag[] = "[AUDIT-INTEGRITY] ";
        char record[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX +
                    sizeof(audit_tag)];
        size_t record_length = sizeof(audit_tag) - 1U;

        serial_write("[AUDIT-INTEGRITY] ");
        serial_write(output);
        serial_write("\n");
        serial_sequence_end(serial_flags);
        memcpy(record, audit_tag, record_length);
        if (output_length <= sizeof(record) - record_length) {
            memcpy(record + record_length, output, output_length);
            record_length += output_length;
        }
        console_publish_journal(record, record_length);
    }
}
#endif
#endif

/* VMM test/diagnostic hook: expose HHDM offset to console. */
uint64_t vmm_hhdm_offset(void) {
    return hhdm_request.response->offset;
}

/*
 * ITEM-436: one two-digit hexadecimal field on both channels of a boot
 * record. The USB endpoint address is the one field of the keyboard binding
 * that is conventionally hexadecimal, and the production-network records'
 * own hex8 helper is compiled only into the production-network build, while
 * the input record is published by every build.
 */
static void log_local_input_hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[3];

    text[0] = digits[(value >> 4U) & 0xFU];
    text[1] = digits[value & 0xFU];
    text[2] = '\0';
    serial_write(text);
    console_record_text(text);
}


/*
 * ITEM-449: render one relay event.  The declaration line is the boot's; these
 * are the relay's own: LISTENING with the rule it serves, REFUSED with the
 * source that was turned away before any outbound connection existed, SESSION
 * with the verdict and the counters, and UNAVAILABLE when the listener could
 * not come up.  No line means nothing happened.  The owner, the declaration
 * and the relay itself live in the production network profile, so the renderer
 * does too - the installer kernel has no relay to report.
 */
#ifdef HFOC_PRODUCTION_NETWORK
static void log_production_forward_field(
    const char *label,
    uint32_t value)
{
    serial_write(label);
    console_record_text(label);
    serial_write_u64(value);
    console_record_u64(value);
}

static void log_production_forward_address(
    const uint8_t address[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    size_t octet;

    for (octet = 0U; octet < FORWARD_CONFIG_ADDRESS_LENGTH; ++octet) {
        if (octet > 0U) {
            serial_write(".");
            console_record_text(".");
        }
        serial_write_u64(address[octet]);
        console_record_u64(address[octet]);
    }
}

static void log_production_forward(void)
{
    enum hfoc_lwip_forward_event event;
    const struct forward_config *config;

    if (!production_forward_published) {
        return;
    }
    event = hfoc_lwip_forward_take_event(&production_forward);
    if (event == HFOC_LWIP_FORWARD_EVENT_NONE) {
        return;
    }
    config = hfoc_lwip_forward_config(&production_forward);
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write("[PRODUCTION-FORWARD] state=");
        console_record_text("[PRODUCTION-FORWARD] state=");
        switch (event) {
        case HFOC_LWIP_FORWARD_EVENT_LISTENING:
        {
            char rule[64];
            size_t rule_length = 0U;

            serial_write("[PRODUCTION-FORWARD] state=LISTENING ");
            console_record_text("[PRODUCTION-FORWARD] state=LISTENING ");
            if (config != NULL &&
                forward_config_render(config, rule, sizeof(rule),
                                      &rule_length) ==
                    FORWARD_CONFIG_OK) {
                serial_write(rule);
                console_record_text(rule);
            }
            break;
        }
        case HFOC_LWIP_FORWARD_EVENT_REFUSED_SOURCE:
            serial_write(
                "[PRODUCTION-FORWARD] state=REFUSED "
                "reason=SOURCE_NOT_ALLOWED src=");
            console_record_text(
                "[PRODUCTION-FORWARD] state=REFUSED "
                "reason=SOURCE_NOT_ALLOWED src=");
            if (hfoc_lwip_forward_source_known(&production_forward)) {
                log_production_forward_address(
                    hfoc_lwip_forward_last_source(&production_forward));
            } else {
                serial_write("UNKNOWN");
                console_record_text("UNKNOWN");
            }
            break;
        case HFOC_LWIP_FORWARD_EVENT_REFUSED_BUSY:
            serial_write("[PRODUCTION-FORWARD] state=REFUSED reason=BUSY");
            console_record_text(
                "[PRODUCTION-FORWARD] state=REFUSED reason=BUSY");
            break;
        case HFOC_LWIP_FORWARD_EVENT_SESSION_OK:
        case HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED:
            serial_write("[PRODUCTION-FORWARD] state=SESSION verdict=");
            console_record_text(
                "[PRODUCTION-FORWARD] state=SESSION verdict=");
            serial_write(hfoc_lwip_forward_verdict(&production_forward));
            console_record_text(
                hfoc_lwip_forward_verdict(&production_forward));
            log_production_forward_field(
                " accepted=",
                hfoc_lwip_forward_accepted(&production_forward));
            log_production_forward_field(
                " sessions=",
                hfoc_lwip_forward_sessions(&production_forward));
            log_production_forward_field(
                " completed=",
                hfoc_lwip_forward_completed(&production_forward));
            log_production_forward_field(
                " aborted=",
                hfoc_lwip_forward_aborted(&production_forward));
            log_production_forward_field(
                " refused_source=",
                hfoc_lwip_forward_refused_source(&production_forward));
            log_production_forward_field(
                " bytes_to_target=",
                hfoc_lwip_forward_bytes_to_target(&production_forward));
            log_production_forward_field(
                " bytes_to_client=",
                hfoc_lwip_forward_bytes_to_client(&production_forward));
            break;
        case HFOC_LWIP_FORWARD_EVENT_LISTEN_FAILED:
        {
            const char *detail = hfoc_lwip_forward_status_string(
                hfoc_lwip_forward_status_of(&production_forward));

            serial_write("UNAVAILABLE reason=LISTEN_FAILED detail=");
            console_record_text(
                "UNAVAILABLE reason=LISTEN_FAILED detail=");
            serial_write(detail != NULL ? detail : "UNKNOWN");
            console_record_text(detail != NULL ? detail : "UNKNOWN");
            break;
        }
        default:
            break;
        }
        serial_write("\n");
        console_journal_end();
        serial_sequence_end(serial_flags);
    }
}
#endif

/*
 * ITEM-622: the ordinary boot owns one bounded writable RAM namespace.  It
 * has no user-mode file descriptors yet; ITEM-623 binds those.  The tree is
 * created here so the boot proves the production VFS, not a host model.
 */

static void kernel_vfs_write_u64(uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        console_write("0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        char text[2];

        text[0] = digits[--count];
        text[1] = '\0';
        console_write(text);
    }
}

static const struct limine_file *kernel_rootfs_module(void)
{
    uint64_t index;

    if (module_request.response == NULL) {
        return NULL;
    }
    for (index = 0U; index < module_request.response->module_count;
         ++index) {
        const struct limine_file *module =
            module_request.response->modules[index];

        if (module != NULL && module->path != NULL &&
            module->address != NULL && module->size != 0U &&
            boot_chain_path_ends_with(module->path, "hfoc-rootfs.arc")) {
            return module;
        }
    }
    return NULL;
}

static void kernel_vfs_boot_init(void)
{
    enum vfs_status status = vfs_init(&kernel_vfs);
    const char *initramfs_state = "ABSENT";

    if (status == VFS_OK) {
        status = vfs_context_init(&kernel_vfs_context, &kernel_vfs);
    }
    if (status == VFS_OK) {
        status = vfs_mount_table_init(&kernel_vfs_mounts);
    }
    if (status == VFS_OK && vfs_mkdir(&kernel_vfs_context, "/tmp") != VFS_OK) {
        status = VFS_BAD_STATE;
    }
    if (status == VFS_OK && vfs_mkdir(&kernel_vfs_context, "/run") != VFS_OK) {
        status = VFS_BAD_STATE;
    }
    /*
     * ITEM-635: /home is created on the writable root before the read-only
     * initramfs is mounted at /, so the persisted /home/owner tree has a
     * writable parent.  The initramfs archive does not carry /home; adding it
     * to the archive would make the mount point read-only again.  No separate
     * ram mount is recorded for /home: the directory is already writable and
     * the existing [VFS] mounts=3 contract stays unchanged.
     */
    if (status == VFS_OK && vfs_mkdir(&kernel_vfs_context, "/home") != VFS_OK) {
        status = VFS_BAD_STATE;
    }
    if (status == VFS_OK &&
        vfs_mount_ram(&kernel_vfs_mounts, &kernel_vfs,
                      &kernel_vfs_context, "/tmp") != VFS_OK) {
        status = VFS_BAD_STATE;
    }
    if (status == VFS_OK &&
        vfs_mount_ram(&kernel_vfs_mounts, &kernel_vfs,
                      &kernel_vfs_context, "/run") != VFS_OK) {
        status = VFS_BAD_STATE;
    }
    if (status == VFS_OK) {
        const struct limine_file *rootfs = kernel_rootfs_module();

        if (rootfs != NULL &&
            vfs_mount_initramfs(&kernel_vfs_mounts, &kernel_vfs,
                                &kernel_vfs_context, "/", rootfs->address,
                                rootfs->size) == VFS_OK) {
            initramfs_state = "READY";
        } else if (rootfs != NULL) {
            initramfs_state = "ERROR";
        }
        if (vfs_mount_ram(&kernel_vfs_mounts, &kernel_vfs,
                          &kernel_vfs_context, "/") != VFS_OK &&
            rootfs == NULL) {
            status = VFS_BAD_STATE;
        }
    }
    kernel_vfs_ready = status == VFS_OK;
    console_write("[VFS] state=");
    console_write(kernel_vfs_ready ? "READY" : vfs_status_string(status));
    console_write(" root=/ nodes=");
    kernel_vfs_write_u64(vfs_node_count(&kernel_vfs));
    console_write(" open=0 data_bytes=");
    kernel_vfs_write_u64(vfs_data_used(&kernel_vfs));
    console_write(" data_capacity=");
    kernel_vfs_write_u64(VFS_DATA_MAX);
    console_write(" mounts=");
    kernel_vfs_write_u64(vfs_mount_count(&kernel_vfs_mounts));
    console_write(" initramfs=");
    console_write(initramfs_state);
    console_write("\n");
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[VFS] state=");
        serial_write(kernel_vfs_ready ? "READY" : "ERROR");
        serial_write(" root=/ mounts=");
        serial_write_hex(vfs_mount_count(&kernel_vfs_mounts));
        serial_write(" initramfs=");
        serial_write(initramfs_state);
        serial_write(" nodes=");
        serial_write_hex(vfs_node_count(&kernel_vfs));
        serial_write(" data_bytes=");
        serial_write_hex(vfs_data_used(&kernel_vfs));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

/*
 * ITEM-635: bind the production workspace after the VFS root exists.  The
 * image starts at partition block 514, after the eight fixed state stores and
 * the two host-seed companion slots; a first-use all-zero range is formatted,
 * an HFOCFS image is mounted, and any other content is refused without a
 * write.  The commit hook is published only after the import succeeds.
 */
static void kernel_workspace_boot_init(void)
{
    const struct appliance_state_loader *loader =
        appliance_production_boot_state_loader(&production_boot);
    const struct appliance_state_layout *layout =
        loader == NULL ? NULL : appliance_state_loader_layout(loader);
    const struct block_device *partition =
        loader == NULL ? NULL
                       : appliance_state_loader_partition_device(loader);
    enum hfocfs_workspace_status status;

    if (!kernel_vfs_ready) {
        serial_write("[WORKSPACE] state=OFF vfs-not-ready\n");
        return;
    }
    if (partition == NULL || layout == NULL) {
        serial_write("[WORKSPACE] state=OFF no-persistence-partition\n");
        return;
    }
    if (layout->block_capacity > HFOCFS_WORKSPACE_LAYOUT_BLOCKS) {
        serial_write("[WORKSPACE] state=REFUSED reason=LAYOUT\n");
        return;
    }
    status = hfocfs_workspace_bind(
        (struct block_device *)(uintptr_t)partition,
        kernel_workspace_state, sizeof(kernel_workspace_state), true,
        &kernel_workspace);
    if (status == HFOCFS_WORKSPACE_OK) {
        status = hfocfs_workspace_import(&kernel_workspace, &kernel_vfs,
                                         &kernel_vfs_context);
    }
    if (status == HFOCFS_WORKSPACE_OK) {
        hfocfs_workspace_activate(&kernel_workspace, &kernel_vfs,
                                  &kernel_vfs_context);
    }
    {
        const uint64_t flags = serial_sequence_begin();

        serial_write("[WORKSPACE] state=");
        serial_write(hfocfs_workspace_status_string(status));
        serial_write(" range=");
        serial_write_u64(HFOCFS_WORKSPACE_OFFSET_BLOCKS);
        serial_write("+");
        serial_write_u64(HFOCFS_WORKSPACE_IMAGE_BLOCKS);
        serial_write(" prefix=");
        serial_write(HFOCFS_WORKSPACE_PREFIX);
        serial_write(" hostseed=SEPARATE\n");
        serial_sequence_end(flags);
    }
}

const struct vfs *kernel_vfs_boot_instance(void)
{
    return kernel_vfs_ready ? &kernel_vfs : NULL;
}

/*
 * ITEM-619 follow-up: the panel credential job publishes its bounded
 * IN_PROGRESS/COMPLETED record on the serial stream, so the production proof
 * can order the health answer inside the measured KDF interval instead of
 * guessing from a live client thread.  The record carries no credential.
 */
static void kernel_panel_auth_serial_sink(const char *line)
{
    uint64_t flags;

    if (line == NULL) {
        return;
    }
    flags = serial_sequence_begin();
    serial_write(line);
    serial_write("\n");
    serial_sequence_end(flags);
}

void kmain(void) {
    cpu_cli();
    serial_init();
    appliance_panel_auth_trace_set_sink(kernel_panel_auth_serial_sink);
    serial_write("\nHFOC OS 1.0.0\n");
    serial_write("Legacy PIC interrupt profile.\n");
    /*
     * ITEM-410: the log is bound before the first record is published, so a
     * machine whose boot refuses early still leaves the reason on its disk.
     */
    kernel_boot_log_boot_init();
    if (appliance_identity_boot_init() != APPLIANCE_IDENTITY_OK) {
        kernel_panic("Appliance identity initialisation failed");
    }
    const struct appliance_identity *appliance_identity =
        appliance_identity_get();
    char appliance_identity_line[
        APPLIANCE_IDENTITY_RENDER_MAX + 1U];
    size_t appliance_identity_length = 0U;
    if (appliance_identity == NULL ||
        appliance_identity_render(
            appliance_identity,
            appliance_identity_line,
            sizeof(appliance_identity_line),
            &appliance_identity_length) != APPLIANCE_IDENTITY_OK) {
        kernel_panic("Appliance identity render failed");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[IDENTITY] ");
        serial_write(appliance_identity_line);

        if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
            kernel_panic("Limine base revision 6 is not supported");
        }
        log_bootloader();

        /*
         * ITEM-488: the display is a capability, not a precondition.
         *
         * This kernel used to panic when the firmware offered no framebuffer
         * and when the one it offered was not 32 bpp - which meant a machine
         * whose firmware has no GOP, or whose pixel format this kernel does not
         * write, could not install HFOC at all.  Neither is the machine's fault
         * and neither is fatal: the display abstraction probes what the
         * firmware gave, binds the text path when it cannot use it, and says so
         * in `[DISPLAY]`.  The console, the records and the installer all work
         * without a single painted pixel.
         */
        if (framebuffer_request.response == NULL ||
            framebuffer_request.response->framebuffer_count == 0U) {
            (void)display_probe_firmware(NULL, "NO_FRAMEBUFFER", true);
        } else if (!framebuffer_init(
                       framebuffer_request.response->framebuffers[0])) {
            (void)display_probe_firmware(
                framebuffer_request.response->framebuffers[0],
                "UNSUPPORTED_PIXEL_FORMAT", true);
        } else if (display_probe_firmware(
                       framebuffer_request.response->framebuffers[0], NULL,
                       true) != DISPLAY_PROBE_OK) {
            /*
             * The abstraction refused a framebuffer the console was willing to
             * accept (a pitch that cannot hold a scanline, a format it does not
             * write): unbind it, so "refused" means nothing paints into it.
             */
            framebuffer_release();
        }
        display_publish();
        /*
         * ITEM-461: the mode the firmware gave us, named before anything else
         * can be blamed on it.  The console is not up yet, so the record is
         * buffered and painted once after the banner - and stored in the
         * durable log either way.  ITEM-463 added a fallback flag.  ITEM-466
         * (round 5) drops the hardcoded request: the boot lets the firmware
         * pick its own preferred mode and this names that preference, the
         * modes the firmware offered and the largest one the rule would use if
         * the native mode had been refused.
         */
        {
            struct framebuffer_mode_choice mode_choice;

            if (!framebuffer_describe_modes(framebuffer_width(),
                                            framebuffer_height(),
                                            &mode_choice)) {
                mode_choice.preferred_width = framebuffer_width();
                mode_choice.preferred_height = framebuffer_height();
                mode_choice.chosen_width = framebuffer_width();
                mode_choice.chosen_height = framebuffer_height();
                mode_choice.available_count = 0U;
                mode_choice.fallback = 0U;
            }
            console_publish_display(
                framebuffer_width(), framebuffer_height(), framebuffer_pitch(),
                framebuffer_bpp(), framebuffer_mode_count(), &mode_choice);
        }

        if (memmap_request.response == NULL || hhdm_request.response == NULL) {
            kernel_panic("Memory map or HHDM response missing");
        }
        if (!pmm_init(memmap_request.response, hhdm_request.response->offset)) {
            /* ITEM-393: the one-line PMM records are .bss, not boot-stack
             * storage. kmain runs on the stack the bootloader handed over
             * and the storage-interruption target proved how thin its margin
             * is; a diagnostic buffer must not spend it. */
            static char pmm_failure_line[HFOC_PMM_DIAGNOSTIC_MAX];
            (void)pmm_init_diagnostic_render(
                pmm_init_diagnostic(),
                pmm_failure_line,
                sizeof(pmm_failure_line));
            {
                const uint64_t pmm_flags = serial_sequence_begin();
                serial_write("[PMM ] init failed ");
                serial_write(pmm_failure_line);
                serial_write("\n");
                serial_sequence_end(pmm_flags);
            }
            kernel_panic_detail(
                "Physical memory manager initialisation failed",
                pmm_failure_line);
        }
        const enum vmm_status vmm_status =
            vmm_init(hhdm_request.response->offset);
        if (vmm_status != VMM_OK) {
            kernel_panic("VMM initialisation failed");
        }
        if (!heap_init(hhdm_request.response->offset)) {
            serial_write("[HEAP] initialization failed; continuing without heap\n");
        serial_sequence_end(serial_flags);
    }
    }
    /*
     * ITEM-466: bind the console's cacheable RAM shadow before the console
     * paints anything.  The shadow is the console's own copy of the surface,
     * so a scroll moves rows in ordinary memory and only writes video memory;
     * the 2026-09-21 run showed a scroll reading the body back out of uncached
     * video (max_us=16315) and starving the keyboard.  A refused allocation is
     * not fatal: the framebuffer keeps its read-back path and the `[FB] shadow`
     * record says which one this boot uses.
     */
    {
        const uint64_t shadow_needed = framebuffer_shadow_required_bytes();
        uint64_t shadow_pages = 0U;
        uint64_t shadow_physical = 0U;
        bool shadow_bound = false;

        if (shadow_needed != 0U) {
            shadow_pages = (shadow_needed + 4095U) / 4096U;
            shadow_physical =
                pmm_alloc_contiguous_pages((uint32_t)shadow_pages);
            if (shadow_physical != 0U) {
                void *shadow_virtual = (void *)(uintptr_t)(
                    shadow_physical + hhdm_request.response->offset);
                if (framebuffer_shadow_init(shadow_virtual, shadow_needed)) {
                    shadow_bound = true;
                } else {
                    pmm_free_contiguous_pages(
                        shadow_physical, (uint32_t)shadow_pages);
                    shadow_physical = 0U;
                }
            }
        }
        {
            const uint64_t shadow_flags = serial_sequence_begin();
            serial_write("[FB] shadow bytes=");
            serial_write_u64(shadow_needed);
            serial_write(" pages=");
            serial_write_u64(shadow_pages);
            serial_write(" physical=");
            serial_write_hex(shadow_physical);
            serial_write(" bound=");
            serial_write_u64(shadow_bound ? 1U : 0U);
            serial_write("\n");
            serial_sequence_end(shadow_flags);
        }
    }
#ifdef HFOC_VMM_AS_QEMU_TEST
    serial_write("[VMM-AS] probing address space proof\n");
    const uint64_t vmm_as_test_phys = pmm_alloc_page();
    if (vmm_as_test_phys != 0U) {
        struct address_space vmm_as_test;
        const uint64_t hhdm = hhdm_request.response->offset;
        const enum vmm_status create_status =
            vmm_create_address_space(&vmm_as_test, hhdm);
        if (create_status == VMM_OK) {
            const enum vmm_status map_status = vmm_map_page_in(
                &vmm_as_test,
                0x1000ULL,
                vmm_as_test_phys,
                VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE,
                hhdm);
            if (map_status == VMM_OK) {
                struct address_space saved_as = *vmm_current_address_space();
                if (vmm_activate_address_space(&vmm_as_test) == VMM_OK) {
                    volatile uint64_t *const cell =
                        (volatile uint64_t *)(uintptr_t)0x1000ULL;
                    *cell = UINT64_C(0x4242424242424242);
                    if (*cell == UINT64_C(0x4242424242424242)) {
                        serial_write(
                            "[VMM-AS] address space map/activate/restore: OK\n");
                    } else {
                        serial_write("[VMM-AS] readback failed\n");
                    }
                    (void)vmm_activate_address_space(&saved_as);
                } else {
                    serial_write("[VMM-AS] activate failed\n");
                }
            } else {
                serial_write("[VMM-AS] map failed\n");
            }
        } else {
            serial_write("[VMM-AS] create failed\n");
        }
    } else {
        serial_write("[VMM-AS] page allocation failed\n");
    }
#endif
    const uint64_t first_page = pmm_alloc_page();
    if (first_page == 0U) {
        kernel_panic("Physical page allocation failed");
    }

    const uint64_t usable_mib = pmm_total_usable_bytes() / (1024ULL * 1024ULL);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[MEM ] Usable memory: ");
        serial_write_u64(usable_mib);
        serial_write(" MiB\n");
        serial_sequence_end(serial_flags);
    }
    {
        /*
         * ITEM-521 requirement 1 names the machine - its cores and its memory -
         * among what the medium's journal must carry.  The `[MEM ]` record
         * above is serial-only and the core count is written nowhere, so the
         * queue-side dry run of the prepared placement (2026-10-12) read the
         * kernel's own journal off the emulated medium and found the machine
         * half absent.  The record is rendered once and published through the
         * durable boot-log path, journal-only and inside one serial sequence,
         * exactly like the boot record above it.
         */
        const uint32_t machine_cores =
            mp_request.response != NULL &&
                    mp_request.response->cpu_count != 0U
                ? (uint32_t)mp_request.response->cpu_count
                : 1U;
        char machine_record[MACHINE_FACTS_RECORD_MAX];
        const size_t machine_length = machine_facts_record(
            machine_record, sizeof(machine_record), usable_mib, machine_cores);
        if (machine_length != 0U) {
            const uint64_t serial_flags = serial_sequence_begin();
            console_journal_begin();
            serial_write(machine_record);
            serial_write("\n");
            console_record_text(machine_record);
            serial_sequence_end(serial_flags);
            console_journal_end();
        }
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PMM ] First allocated page: ");
        serial_write_hex(first_page);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    {
        /*
         * ITEM-390: publish how much of the usable memory the bitmap actually
         * tracks. coverage=LIMITED with dropped_pages>0 is the honest state of
         * a machine whose RAM exceeds the page bitmap capacity; the kernel
         * boots and never pretends to cover memory it dropped.
         */
        static char pmm_report_line[HFOC_PMM_DIAGNOSTIC_MAX];
        (void)pmm_init_diagnostic_render(
            pmm_init_diagnostic(),
            pmm_report_line,
            sizeof(pmm_report_line));
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PMM ] ");
        serial_write(pmm_report_line);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    serial_write("[ACPI] Probing RSDP via Limine...\n");
    acpi_init((const struct limine_rsdp_response *)rsdp_request.response,
              memmap_request.response,
              hhdm_request.response->offset);

    struct cpu *bsp = cpu_by_index(0U);
    cpu_init_bsp(bsp, 0U, 0U);

    serial_write("[GDT ] Loading descriptor table...\n");
    gdt_init_for(bsp);
    tss_init_for(bsp);
    gdt_load_tr_for(bsp);
    serial_write("[GDT ] Table loaded; TSS/TR ready; installing IDT before segment reload.\n");
    interrupts_init();
    serial_write("[IDT ] 256 interrupt gates loaded.\n");
    gdt_reload_segments();
    serial_write("[GDT ] Code and data segments reloaded.\n");
    cpu_gs_base_set(bsp);

    const enum event_ring_status event_ring_status =
        event_ring_init(&kernel_event_ring);
    if (event_ring_status != EVENT_RING_OK) {
        kernel_panic("Event ring initialisation failed");
    }

    serial_write("[LAPIC] Probing Local APIC...\n");
    lapic_init(acpi_madt(),
               memmap_request.response,
               hhdm_request.response->offset);
    bsp->lapic_id = lapic_bsp_apic_id();

    smp_init(mp_request.response, acpi_madt(),
             hhdm_request.response->offset);

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PIC ] IOAPIC pre: master=");
        serial_write_hex(pic_master_mask());
        serial_write(" slave=");
        serial_write_hex(pic_slave_mask());
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    serial_write("[IOAPIC] Probing IOAPIC\n");
    ioapic_init(acpi_madt(), hhdm_request.response->offset);

    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PIC ] IOAPIC post: master=");
        serial_write_hex(pic_master_mask());
        serial_write(" slave=");
        serial_write_hex(pic_slave_mask());
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    pic_init();
    const enum ioapic_status irq1_route_status =
        ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);
    const enum ioapic_status irq0_route_status =
        ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR);
    const enum keyboard_init_status keyboard_status = keyboard_init();
    /*
     * ITEM-392: a machine with no usable i8042 boots. Local keyboard input is
     * one input path among several - the framebuffer console, the serial
     * console and the network management plane stay available - so every
     * status the driver can report publishes its reason and continues. Only an
     * unclassified status stops the boot, and it stops with that status as its
     * detail instead of a bare halt.
     */
    const char *const keyboard_reason =
        keyboard_init_status_string(keyboard_status);
    if (!keyboard_init_status_boots(keyboard_status)) {
        kernel_panic_detail("PS/2 keyboard initialisation failed",
                            keyboard_reason);
    }
    const bool ps2_input_available = keyboard_status == KEYBOARD_INIT_OK;
    const struct keyboard_init_diagnostic *const keyboard_diagnostic =
        keyboard_init_diagnostic();
    if (ps2_input_available) {
        const uint64_t serial_flags = serial_sequence_begin();

        /*
         * ITEM-405: the same record on the local console.  The serial calls
         * are unchanged; the mirror renders the same fragments after the
         * serial sequence has ended.
         */
        console_journal_begin();
        serial_write("[KBD ] Controller config: ");
        console_record_text("[KBD ] Controller config: ");
        serial_write_hex(keyboard_controller_config());
        console_record_hex64(keyboard_controller_config());
        serial_write("; scan set 2 with set-1 translation enabled.\n");
        console_record_text("; scan set 2 with set-1 translation enabled.");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
    /*
     * ITEM-395: the local console gets a second input path. The USB HID
     * bring-up is bounded at every handshake and is never fatal: a machine
     * with no xHCI controller, no device on its ports, a device that is not a
     * HID boot keyboard, or a controller that never answers publishes why and
     * keeps booting on the PS/2 path, the serial console and the network
     * management plane.
     */
    const enum xhci_status usb_status =
        xhci_keyboard_init(hhdm_request.response->offset);
    {
        struct local_input_report input_report;

        input_report.ps2_ready = ps2_input_available;
        input_report.ps2_reason = keyboard_reason;
        input_report.usb_ready = xhci_keyboard_attached();
        input_report.usb_reason = xhci_status_string(usb_status);
        input_report.usb_detail = xhci_keyboard_detail();
        input_report.usb_speed = xhci_speed_string(xhci_keyboard_speed());
        input_report.usb_vendor_id = xhci_keyboard_vendor_id();
        input_report.usb_product_id = xhci_keyboard_product_id();
        local_input_publish(&input_report);
    }
    /*
     * ITEM-392 + ITEM-395: local input is available when either keyboard path
     * is, and the record names the path that is live. The event loop merges
     * the two queues under the declared rule in local_input.h (PS/2 first).
     */
    const bool local_input_available = local_input_ready();
    const enum local_input_source local_input_source_now = local_input_source();
    {
        /*
         * ITEM-392: one record per boot, in the style of the other boot
         * records, so an operator reads why local keyboard input is missing
         * instead of only its absence. The status-port and configuration
         * bytes separate a machine with no controller from one whose
         * controller never answered. ITEM-395 appends the live input path:
         * reason= names why local input is in this state (OK, USB_HID, or the
         * PS/2 status when no path is usable), source= names the path that
         * feeds the console, and ps2=/usb= keep both drivers' own verdicts.
         */
        const uint64_t serial_flags = serial_sequence_begin();

        /*
         * ITEM-405: the same record on the local console.  The acceptance host
         * showed `LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)` while a USB
         * keyboard was physically connected, and this record - which names the
         * xHCI verdict behind `usb=` - was on a serial port nobody was
         * reading.  Every serial call below is unchanged; the mirror renders
         * the same fragments and is published after the serial sequence.
         */
        console_journal_begin();
        serial_write("[KBD ] state=");
        console_record_text("[KBD ] state=");
        serial_write(local_input_available ? "READY" : "UNAVAILABLE");
        console_record_text(local_input_available ? "READY" : "UNAVAILABLE");
        serial_write(" reason=");
        console_record_text(" reason=");
        serial_write(local_input_reason());
        console_record_text(local_input_reason());
        if (keyboard_diagnostic != NULL) {
            serial_write(" port=");
            console_record_text(" port=");
            serial_write_hex(keyboard_diagnostic->status_port);
            console_record_hex64(keyboard_diagnostic->status_port);
            serial_write(" config=");
            console_record_text(" config=");
            serial_write_hex(keyboard_diagnostic->controller_config);
            console_record_hex64(keyboard_diagnostic->controller_config);
        }
        serial_write(" source=");
        console_record_text(" source=");
        serial_write(local_input_source_string(local_input_source_now));
        console_record_text(
            local_input_source_string(local_input_source_now));
        serial_write(" ps2=");
        console_record_text(" ps2=");
        serial_write(keyboard_reason);
        console_record_text(keyboard_reason);
        serial_write(" usb=");
        console_record_text(" usb=");
        serial_write(xhci_status_string(usb_status));
        console_record_text(xhci_status_string(usb_status));
        if (local_input_source_now == LOCAL_INPUT_SOURCE_USB_HID) {
            serial_write(" speed=");
            console_record_text(" speed=");
            serial_write(xhci_speed_string(xhci_keyboard_speed()));
            console_record_text(xhci_speed_string(xhci_keyboard_speed()));
            serial_write(" vid=");
            console_record_text(" vid=");
            serial_write_hex(xhci_keyboard_vendor_id());
            console_record_hex64(xhci_keyboard_vendor_id());
            serial_write(" pid=");
            console_record_text(" pid=");
            serial_write_hex(xhci_keyboard_product_id());
            console_record_hex64(xhci_keyboard_product_id());
            /*
             * ITEM-436: which interface the driver bound, not only that it
             * bound one. The 2026-09-17 hardware run's keyboard is a Logitech
             * wireless receiver (vid=0x046D pid=0xC53F) that answers with
             * more than one HID interface, and `max_packet=12` alone did not
             * say whether the keyboard interface or another collection was
             * selected. The descriptor's own numbers are published next to
             * the packet size the endpoint context was built from, so the
             * next run reads the binding instead of inferring it.
             */
            serial_write(" iface=");
            console_record_text(" iface=");
            serial_write_u64(xhci_keyboard_interface());
            console_record_u64(xhci_keyboard_interface());
            serial_write(" protocol=");
            console_record_text(" protocol=");
            serial_write_u64(xhci_keyboard_protocol());
            console_record_u64(xhci_keyboard_protocol());
            serial_write(" ep=0x");
            console_record_text(" ep=0x");
            log_local_input_hex8(xhci_keyboard_endpoint_address());
            /*
             * ITEM-438: the DCI is what the doorbell names and what the
             * endpoint context is indexed by, and the descriptor's own
             * bInterval is the number the programmed `interval=` is the
             * exponent of for a low- or full-speed endpoint.  The 2026-09-17
             * record published only the second number, so "what the device
             * asked for" and "what the controller was told" could not be
             * compared on one line.
             */
            serial_write(" dci=");
            console_record_text(" dci=");
            serial_write_u64(xhci_keyboard_dci());
            console_record_u64(xhci_keyboard_dci());
            serial_write(" interval=");
            console_record_text(" interval=");
            serial_write_u64(xhci_keyboard_endpoint_interval());
            console_record_u64(xhci_keyboard_endpoint_interval());
            serial_write(" binterval=");
            console_record_text(" binterval=");
            serial_write_u64(xhci_keyboard_endpoint_binterval());
            console_record_u64(xhci_keyboard_endpoint_binterval());
            serial_write(" max_packet=");
            console_record_text(" max_packet=");
            serial_write_u64(xhci_keyboard_endpoint_max_packet());
            console_record_u64(xhci_keyboard_endpoint_max_packet());
            /*
             * ITEM-438: which protocol the reports are read in, and - for a
             * keyboard that was bound by its own report descriptor rather than
             * by a declared boot protocol - the layout that descriptor named.
             * `mode=REPORT report_id=0 keys_at=2 keys=6` says the interface
             * promised nothing and the descriptor did, which is the difference
             * the 2026-09-17 run could not read.  `idle=` says whether the
             * device accepted the once-a-second idle report that proves a
             * bound keyboard is alive without being touched.
             */
            serial_write(" mode=");
            console_record_text(" mode=");
            serial_write(xhci_keyboard_report_mode() ? "REPORT" : "BOOT");
            console_record_text(xhci_keyboard_report_mode() ? "REPORT" : "BOOT");
            if (xhci_keyboard_report_mode()) {
                serial_write(" report_id=");
                console_record_text(" report_id=");
                serial_write_u64(xhci_keyboard_report_id());
                console_record_u64(xhci_keyboard_report_id());
                serial_write(" keys_at=");
                console_record_text(" keys_at=");
                serial_write_u64(xhci_keyboard_key_offset());
                console_record_u64(xhci_keyboard_key_offset());
                serial_write(" keys=");
                console_record_text(" keys=");
                serial_write_u64(xhci_keyboard_key_slots());
                console_record_u64(xhci_keyboard_key_slots());
            }
            serial_write(" idle=");
            console_record_text(" idle=");
            serial_write(xhci_keyboard_idle_refused() ? "REFUSED" : "OK");
            console_record_text(xhci_keyboard_idle_refused() ? "REFUSED" : "OK");
            /*
             * ITEM-438: how the walk that produced this binding ended.  A
             * machine whose first keyboard-shaped device was silent and whose
             * real keyboard was on a later port reads
             * `candidates=2 silent=1 proven=1`; a machine whose only keyboard
             * ignores SET_IDLE and was re-bound after the walk reads
             * `candidates=1 silent=1 proven=0 retry=1`.
             */
            serial_write(" candidates=");
            console_record_text(" candidates=");
            serial_write_u64(xhci_keyboard_candidates_bound());
            console_record_u64(xhci_keyboard_candidates_bound());
            serial_write(" silent=");
            console_record_text(" silent=");
            serial_write_u64(xhci_keyboard_candidates_silent());
            console_record_u64(xhci_keyboard_candidates_silent());
            serial_write(" proven=");
            console_record_text(" proven=");
            serial_write_u64(xhci_keyboard_candidates_proven());
            console_record_u64(xhci_keyboard_candidates_proven());
            serial_write(" retry=");
            console_record_text(" retry=");
            serial_write_u64(xhci_keyboard_silent_retry() ? 1U : 0U);
            console_record_u64(xhci_keyboard_silent_retry() ? 1U : 0U);
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }
    /*
     * ITEM-448: the pointer's own record, the same evidence the [KBD ] line
     * carries for the keyboard.  A machine with a mouse reads the binding
     * (port, speed, vid/pid, interface, protocol, endpoint, interval, packet)
     * and the traffic its reports produced; a machine whose mouse was refused
     * or seen on a controller the walk had to leave reads which of the two it
     * was, with the device's own identity and interface list - so "no pointer"
     * is a finding with a reason, not a silence.
     */
    {
        const uint64_t serial_flags = serial_sequence_begin();

        console_journal_begin();
        serial_write("[MOUSE] state=");
        console_record_text("[MOUSE] state=");
        serial_write(xhci_mouse_attached() ? "READY" : "UNAVAILABLE");
        console_record_text(xhci_mouse_attached() ? "READY" : "UNAVAILABLE");
        serial_write(" reason=");
        console_record_text(" reason=");
        if (xhci_mouse_attached()) {
            serial_write("OK");
            console_record_text("OK");
        } else {
            serial_write(xhci_mouse_status_string());
            console_record_text(xhci_mouse_status_string());
            serial_write(" detail=");
            console_record_text(" detail=");
            serial_write(xhci_mouse_detail());
            console_record_text(xhci_mouse_detail());
        }
        if (xhci_mouse_attached()) {
            serial_write(" port=");
            console_record_text(" port=");
            serial_write_u64(xhci_mouse_port());
            console_record_u64(xhci_mouse_port());
            serial_write(" speed=");
            console_record_text(" speed=");
            serial_write(xhci_speed_string(xhci_mouse_speed()));
            console_record_text(xhci_speed_string(xhci_mouse_speed()));
            serial_write(" vid=");
            console_record_text(" vid=");
            serial_write_hex(xhci_mouse_vendor_id());
            console_record_hex64(xhci_mouse_vendor_id());
            serial_write(" pid=");
            console_record_text(" pid=");
            serial_write_hex(xhci_mouse_product_id());
            console_record_hex64(xhci_mouse_product_id());
            serial_write(" iface=");
            console_record_text(" iface=");
            serial_write_u64(xhci_mouse_interface());
            console_record_u64(xhci_mouse_interface());
            serial_write(" protocol=");
            console_record_text(" protocol=");
            serial_write_u64(xhci_mouse_protocol());
            console_record_u64(xhci_mouse_protocol());
            serial_write(" ep=0x");
            console_record_text(" ep=0x");
            log_local_input_hex8(xhci_mouse_endpoint_address());
            serial_write(" dci=");
            console_record_text(" dci=");
            serial_write_u64(xhci_mouse_dci());
            console_record_u64(xhci_mouse_dci());
            serial_write(" interval=");
            console_record_text(" interval=");
            serial_write_u64(xhci_mouse_endpoint_interval());
            console_record_u64(xhci_mouse_endpoint_interval());
            serial_write(" binterval=");
            console_record_text(" binterval=");
            serial_write_u64(xhci_mouse_endpoint_binterval());
            console_record_u64(xhci_mouse_endpoint_binterval());
            serial_write(" max_packet=");
            console_record_text(" max_packet=");
            serial_write_u64(xhci_mouse_endpoint_max_packet());
            console_record_u64(xhci_mouse_endpoint_max_packet());
            serial_write(" mode=");
            console_record_text(" mode=");
            serial_write(xhci_mouse_report_mode() ? "REPORT" : "BOOT");
            console_record_text(xhci_mouse_report_mode() ? "REPORT" : "BOOT");
            serial_write(" reports=");
            console_record_text(" reports=");
            serial_write_u64(xhci_mouse_report_count());
            console_record_u64(xhci_mouse_report_count());
            serial_write(" transfers=");
            console_record_text(" transfers=");
            serial_write_u64(xhci_mouse_transfer_count());
            console_record_u64(xhci_mouse_transfer_count());
            serial_write(" completions=");
            console_record_text(" completions=");
            serial_write_u64(xhci_mouse_completion_count());
            console_record_u64(xhci_mouse_completion_count());
            serial_write(" wheels=");
            console_record_text(" wheels=");
            serial_write_u64(xhci_mouse_wheel_count());
            console_record_u64(xhci_mouse_wheel_count());
            serial_write(" ep_restarts=");
            console_record_text(" ep_restarts=");
            serial_write_u64(xhci_mouse_endpoint_restarts());
            console_record_u64(xhci_mouse_endpoint_restarts());
            serial_write(" errors=");
            console_record_text(" errors=");
            serial_write_u64(xhci_mouse_error_count());
            console_record_u64(xhci_mouse_error_count());
            serial_write(" refused=");
            console_record_text(" refused=");
            serial_write_u64(xhci_mouse_refused_count());
            console_record_u64(xhci_mouse_refused_count());
            serial_write(" moves=");
            console_record_text(" moves=");
            serial_write_u64(xhci_mouse_move_count());
            console_record_u64(xhci_mouse_move_count());
            serial_write(" presses=");
            console_record_text(" presses=");
            serial_write_u64(xhci_mouse_press_count());
            console_record_u64(xhci_mouse_press_count());
            serial_write(" releases=");
            console_record_text(" releases=");
            serial_write_u64(xhci_mouse_release_count());
            console_record_u64(xhci_mouse_release_count());
        } else {
            /*
             * ITEM-448: the refused device's own identity and the interfaces it
             * declared, so "not a mouse" is a statement about a named device
             * rather than about the port it sat on.
             */
            serial_write(" device=");
            console_record_text(" device=");
            serial_write_u64(xhci_mouse_device_seen() ? 1U : 0U);
            console_record_u64(xhci_mouse_device_seen() ? 1U : 0U);
            serial_write(" vid=");
            console_record_text(" vid=");
            serial_write_hex(xhci_mouse_refused_vendor_id());
            console_record_hex64(xhci_mouse_refused_vendor_id());
            serial_write(" pid=");
            console_record_text(" pid=");
            serial_write_hex(xhci_mouse_refused_product_id());
            console_record_hex64(xhci_mouse_refused_product_id());
            serial_write(" interfaces=");
            console_record_text(" interfaces=");
            serial_write(xhci_mouse_interface_summary());
            console_record_text(xhci_mouse_interface_summary());
            serial_write(" seen=");
            console_record_text(" seen=");
            serial_write_u64(xhci_mouse_seen() ? 1U : 0U);
            console_record_u64(xhci_mouse_seen() ? 1U : 0U);
            if (xhci_mouse_seen()) {
                serial_write(" seen_port=");
                console_record_text(" seen_port=");
                serial_write_u64(xhci_mouse_seen_port());
                console_record_u64(xhci_mouse_seen_port());
                serial_write(" seen_speed=");
                console_record_text(" seen_speed=");
                serial_write(xhci_speed_string(xhci_mouse_seen_speed()));
                console_record_text(xhci_speed_string(xhci_mouse_seen_speed()));
                serial_write(" seen_controller=");
                console_record_text(" seen_controller=");
                serial_write(xhci_mouse_seen_controller());
                console_record_text(xhci_mouse_seen_controller());
            }
            serial_write(" candidates=");
            console_record_text(" candidates=");
            serial_write_u64(xhci_mouse_candidates_bound());
            console_record_u64(xhci_mouse_candidates_bound());
            serial_write(" released=");
            console_record_text(" released=");
            serial_write_u64(xhci_mouse_candidates_released());
            console_record_u64(xhci_mouse_candidates_released());
        }
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
    }

    timer_init(HFOC_TIMER_HZ);
    if (!hfoc_lwip_time_init_default()) {
        kernel_panic("lwIP monotonic time adapter init failed");
    }
    const bool irq1_ready =
        irq1_route_status == IOAPIC_OK &&
        ioapic_is_irq1_routed();
    if (irq1_ready) {
        pic_mask(1U);
        serial_write("[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.\n");
    } else {
        pic_unmask(1U);
        serial_write("[PIC ] IRQ1 unmasked; legacy PIC fallback retained.\n");
    }
    const bool irq0_ready =
        irq0_route_status == IOAPIC_OK &&
        ioapic_is_irq0_routed();
    if (irq0_ready) {
        pic_mask(0U);
        serial_write("[PIC ] IRQ0 masked; IOAPIC vector=0x40 active.\n");
    } else {
        pic_unmask(0U);
        serial_write("[PIC ] IRQ0 unmasked; legacy PIC timer retained.\n");
    }
    irq0_ioapic_active_publish(irq0_ready);
    isa_irq3_7_route_all();
    isa_irq8_15_route_all();
    lapic_timer_start_calibration(HFOC_TIMER_HZ);
    log_pic_masks();

    appliance_production_storage_boot_init(hhdm_request.response->offset);
    boot_chain_run();
    if (appliance_production_boot_init(&production_boot) !=
            APPLIANCE_PRODUCTION_BOOT_OK) {
        kernel_panic("Production boot coordinator initialisation failed");
    }
    if (boot_attestation_blocks_production_boot(
            &boot_attestation_state)) {
        (void)appliance_production_boot_set_attestation(
            &production_boot,
            APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED,
            boot_attestation_reason_string(
                boot_attestation_state.reason));
    } else if (boot_attestation_state.verdict ==
               BOOT_ATTESTATION_ATTESTED) {
        (void)appliance_production_boot_set_attestation(
            &production_boot,
            APPLIANCE_PRODUCTION_BOOT_ATTESTATION_TRUSTED, NULL);
    }
    if (appliance_production_boot_run(
            &production_boot, appliance_production_storage_probe, NULL) ==
            APPLIANCE_PRODUCTION_BOOT_NULL) {
        kernel_panic("Production boot coordinator initialisation failed");
    }
    log_production_boot();
    /*
     * ITEM-518: arm the console `storage` command with what the probe saw,
     * before the console loop can be reached and whether or not a state volume
     * exists - on the machine booted from the medium the command is the only
     * way to read the storage verdict without a serial capture.
     */
    {
        struct appliance_storage_report storage_report;

        appliance_production_storage_report(&storage_report);
        console_publish_storage_report(&storage_report);
    }
#ifndef HFOC_PRODUCTION_INSTALL_MEDIA
    /*
     * ITEM-410: an installed appliance writes its log to its own disk.  The
     * installer medium does not: the medium is never a write target, and the
     * install flow attaches the log to the disk it declared instead.
     */
    kernel_boot_log_attach_disks();
    /*
     * ITEM-497: the input path's own verdict of this boot is published as soon
     * as the log exists to keep it.  A boot that is switched off - or killed by
     * a proof - before the loop's next flush still leaves the verdict on the
     * disk it came up on, which is the whole point of the record.
     */
    xhci_input_verdict_publish_now();
#endif
    /*
     * ITEM-492: describe the machine's network controllers before anything
     * installs.  This is a capability reading - which controllers exist, and
     * whether this image carries a driver for each - and it publishes the
     * `[NIC]` records; the drivers themselves are bound later, where they
     * always were, and update the reading.  Nothing about the install depends
     * on the answer, and no network stack is required to produce it.
     */
    kernel_publish_nic_capabilities();
#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
    /*
     * ITEM-394: the installer medium reaches the product install run here,
     * after the storage probe has published the disks and before the appliance
     * paths.  On the appliance image this call does not exist.
     */
    production_install_media_boot(usable_mib);
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_RECOVERY_EXECUTOR_QEMU_TEST
    appliance_production_recovery_executor_qemu_test_init(
        appliance_production_boot_state_loader(&production_boot),
        appliance_production_boot_readiness(&production_boot));
#endif

#ifdef HFOC_PRODUCTION_NETWORK
    (void)appliance_production_network_boot_init(
        hhdm_request.response->offset);
    log_production_network();
    {
        struct appliance_network_config network_desired;
        struct appliance_persistent_network_config *network_store =
            appliance_production_boot_state_loader(&production_boot) != NULL
                ? &production_boot.state.network_config
                : NULL;
        /*
         * ITEM-447: the machine's own menu writes the same record the boot
         * reads, so a change from the screen survives the next reboot.
         *
         * ITEM-496: the binding is published only when that record exists and
         * is loaded.  Before this item the entry was offered on a machine with
         * no usable state volume too, the operator answered all four
         * questions, and only then learned `persisted=FAILED` - the one
         * refusal the operator could have been told before typing.  An
         * appliance that cannot store the answer now says `state=UNAVAILABLE
         * reason=NO_RECORD_OWNER` at the entry, which is the same record the
         * unbound host proof already pins.
         */
        if (network_store != NULL &&
            appliance_persistent_network_config_current(network_store) !=
                NULL) {
            console_network_binding.context = network_store;
            console_bind_network_config(&console_network_binding);
        } else {
            console_bind_network_config(NULL);
        }
        /*
         * A store record the operator committed (from the menu, ITEM-447) is
         * the most recent intent and wins.  A store that only carries the
         * synthesized default is not a setting, so the install declaration
         * fills it instead of being masked by it.
         */
        const bool network_store_committed =
            network_store != NULL &&
            appliance_persistent_network_config_source(network_store) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED;
        const struct appliance_network_config *network_desired_current =
            network_store_committed
                ? appliance_persistent_network_config_current(network_store)
                : NULL;

        /*
         * ITEM-446: the network the operator declared on the install medium
         * travels in the installed boot entry, exactly as the management scope
         * does.  A committed store wins - that is the setting an operator
         * changed from the menu - and a declaration fills the store the first
         * time it is seen, so the address survives on the same record the menu
         * reads and writes.  The declaration is validated before it is applied;
         * a malformed one is published as a refusal and the boot keeps the
         * stored configuration instead of guessing.
         */
        struct appliance_production_network_time_config time_config;

        memset(&time_config, 0, sizeof(time_config));
        {
            struct production_install_media_boot_network boot_network;
            enum production_install_media_status boot_network_status;

            memset(&boot_network, 0, sizeof(boot_network));
            boot_network_status = production_install_media_network_boot(
                boot_command_line(), boot_command_line_length(),
                &boot_network);
            /*
             * ITEM-654: the SNTP source is independent of the addressing
             * mode; a malformed value is still carried to the wire owner so
             * the record can say INVALID instead of hiding it.
             */
            time_config.declared = boot_network.sntp_declared;
            time_config.valid = boot_network.sntp_valid;
            if (boot_network.sntp_valid) {
                time_config.server = boot_network.sntp_address;
            }
            if (boot_network_status != PRODUCTION_INSTALL_MEDIA_OK) {
                const uint64_t serial_flags = serial_sequence_begin();

                console_journal_begin();
                serial_write(
                    "[PRODUCTION-NET-DECLARATION] state=REFUSED reason=");
                serial_write(
                    production_install_media_status_string(
                        boot_network_status) != NULL
                        ? production_install_media_status_string(
                              boot_network_status)
                        : "MALFORMED");
                serial_write("\n");
                console_journal_end();
                serial_sequence_end(serial_flags);
            } else if (boot_network.declared &&
                       network_desired_current == NULL) {
                const char *interface =
                    APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE;
                const size_t interface_length =
                    (size_t)strlen(interface);

                if (!boot_network.is_static) {
                    if (appliance_network_config_publish_dhcp(
                            &network_desired, interface,
                            interface_length) ==
                        APPLIANCE_NETWORK_CONFIG_OK) {
                        network_desired_current = &network_desired;
                    }
                    if (network_store != NULL) {
                        (void)appliance_persistent_network_config_set_dhcp(
                            network_store, interface, interface_length);
                    }
                } else if (boot_network.config_valid) {
                    if (appliance_network_config_publish_static(
                            &network_desired, interface, interface_length,
                            &boot_network.config) ==
                        APPLIANCE_NETWORK_CONFIG_OK) {
                        network_desired_current = &network_desired;
                    }
                    if (network_store != NULL) {
                        (void)appliance_persistent_network_config_set_static(
                            network_store, interface, interface_length,
                            &boot_network.config);
                    }
                }
            }
        }
        if (network_desired_current == NULL) {
            (void)appliance_network_config_publish_default(
                &network_desired);
            network_desired_current = &network_desired;
        }
        (void)appliance_production_network_wire_boot_init(
            network_desired_current, &time_config);
        {
            static bool management_bind_logged;

            if (!management_bind_logged) {
                /*
                 * ITEM-399: an installed appliance always declares its scope
                 * (`cmdline: hfoc.management=...` in the installed
                 * configuration, or in the generated menu that replaced it),
                 * so the record distinguishes an explicit declaration - either
                 * direction - from an entry that declares nothing at all and
                 * therefore really is the default.
                 */
                bool declared_interface = false;
                const bool declared =
                    production_install_media_management_declared(
                        boot_command_line(), boot_command_line_length(),
                        &declared_interface);
                const bool opt_in = declared && declared_interface;
                enum appliance_management_bind_status bind_status =
                    opt_in
                        ? appliance_management_bind_publish_management_interface(
                              &production_management_bind,
                              network_desired_current->interface,
                              (size_t)network_desired_current
                                  ->interface_length,
                              NULL, 0U)
                        : appliance_management_bind_publish_default(
                              &production_management_bind);

                if (bind_status == APPLIANCE_MANAGEMENT_BIND_OK) {
                    const uint64_t serial_flags = serial_sequence_begin();

                    management_bind_logged = true;
                    /* ITEM-410: mirrored and stored - the management scope
                     * the hardware run's management stages quote. */
                    console_journal_begin();
                    serial_write("[PRODUCTION-MGMT-BIND] scope=");
                    console_record_text("[PRODUCTION-MGMT-BIND] scope=");
                    serial_write(appliance_management_bind_scope_string(
                        appliance_management_bind_scope_of(
                            &production_management_bind)));
                    console_record_text(
                        appliance_management_bind_scope_string(
                            appliance_management_bind_scope_of(
                                &production_management_bind)));
                    /* ITEM-394: the audit states where the scope came from. */
                    serial_write(declared ? " source=boot-declaration"
                                          : " source=default");
                    console_record_text(
                        declared ? " source=boot-declaration"
                                 : " source=default");
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                    console_journal_end();
                }
            }
        }
        /*
         * ITEM-449: the forward the install medium declared travels in the
         * installed boot entry, exactly as the management scope and the
         * network do.  The declaration is validated before anything is
         * published or opened; a malformed one is refused with its status and
         * nothing is served, and an absent or disabled declaration says so
         * instead of leaving the port question unanswered.  The declaration
         * itself opens nothing: the relay owner publishes LISTENING when its
         * listener is live.
         */
        {
            struct production_install_media_boot_forward boot_forward;
            enum production_install_media_status forward_status;
            char forward_rule[64];
            size_t forward_rule_length = 0U;

            memset(&boot_forward, 0, sizeof(boot_forward));
            memset(forward_rule, 0, sizeof(forward_rule));
            forward_status = production_install_media_forward_boot(
                boot_command_line(), boot_command_line_length(),
                &boot_forward);
            if (forward_status != PRODUCTION_INSTALL_MEDIA_OK) {
                const char *forward_reason =
                    production_install_media_status_string(
                        forward_status) != NULL
                        ? production_install_media_status_string(
                              forward_status)
                        : "MALFORMED";
                const uint64_t serial_flags = serial_sequence_begin();

                console_journal_begin();
                serial_write(
                    "[PRODUCTION-FORWARD-DECLARATION] state=REFUSED reason=");
                console_record_text(
                    "[PRODUCTION-FORWARD-DECLARATION] state=REFUSED reason=");
                serial_write(forward_reason);
                console_record_text(forward_reason);
                serial_write("\n");
                console_journal_end();
                serial_sequence_end(serial_flags);
                {
                    const uint64_t unavailable_flags =
                        serial_sequence_begin();

                    console_journal_begin();
                    serial_write(
                        "[PRODUCTION-FORWARD] state=UNAVAILABLE "
                        "reason=DECLARATION_REFUSED\n");
                    console_record_text(
                        "[PRODUCTION-FORWARD] state=UNAVAILABLE "
                        "reason=DECLARATION_REFUSED\n");
                    console_journal_end();
                    serial_sequence_end(unavailable_flags);
                }
            } else if (boot_forward.config_valid &&
                       forward_config_render(
                           &boot_forward.config, forward_rule,
                           sizeof(forward_rule),
                           &forward_rule_length) == FORWARD_CONFIG_OK) {
                const uint64_t serial_flags = serial_sequence_begin();

                console_journal_begin();
                serial_write("[PRODUCTION-FORWARD] state=DECLARED ");
                console_record_text("[PRODUCTION-FORWARD] state=DECLARED ");
                serial_write(forward_rule);
                console_record_text(forward_rule);
                serial_write("\n");
                console_journal_end();
                serial_sequence_end(serial_flags);
                /*
                 * ITEM-449: the relay owner is created here, while the live
                 * netif is in hand.  It is not started here: the loop's
                 * service call brings the listener up once the interface has
                 * an address, and retries a bind that lost a race.  An owner
                 * that cannot be created is published as UNAVAILABLE with the
                 * status that says why.
                 */
                {
                    const struct appliance_production_network *network =
                        appliance_production_network_boot_network();
                    enum hfoc_lwip_forward_status forward_init_status =
                        network != NULL
                            ? hfoc_lwip_forward_init(
                                  &production_forward, &network->netif,
                                  &boot_forward.config,
                                  APPLIANCE_PRODUCTION_FORWARD_GENERATION)
                            : HFOC_LWIP_FORWARD_NETIF_NOT_LIVE;

                    if (forward_init_status == HFOC_LWIP_FORWARD_OK) {
                        production_forward_published = true;
                    } else {
                        const char *init_detail =
                            hfoc_lwip_forward_status_string(
                                forward_init_status);
                        const uint64_t init_flags =
                            serial_sequence_begin();

                        console_journal_begin();
                        serial_write(
                            "[PRODUCTION-FORWARD] state=UNAVAILABLE "
                            "reason=RELAY_UNAVAILABLE detail=");
                        console_record_text(
                            "[PRODUCTION-FORWARD] state=UNAVAILABLE "
                            "reason=RELAY_UNAVAILABLE detail=");
                        serial_write(init_detail != NULL ? init_detail
                                                         : "UNKNOWN");
                        console_record_text(init_detail != NULL
                                                ? init_detail
                                                : "UNKNOWN");
                        serial_write("\n");
                        console_journal_end();
                        serial_sequence_end(init_flags);
                    }
                }
            } else {
                const char *forward_reason =
                    boot_forward.declared ? "DISABLED" : "NO_DECLARATION";
                const uint64_t serial_flags = serial_sequence_begin();

                console_journal_begin();
                serial_write("[PRODUCTION-FORWARD] state=UNAVAILABLE reason=");
                console_record_text(
                    "[PRODUCTION-FORWARD] state=UNAVAILABLE reason=");
                serial_write(forward_reason);
                console_record_text(forward_reason);
                serial_write("\n");
                console_journal_end();
                serial_sequence_end(serial_flags);
            }
        }
        log_production_network_wire();
    }
    {
        const struct network_device_snapshot *network_device =
            appliance_production_network_device(
                appliance_production_network_boot_network());

        if (network_device != NULL) {
            production_first_boot_ready =
                appliance_production_first_boot_init(
                    &production_first_boot, &production_boot.state,
                    network_device) ==
                APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
        }
        log_production_first_boot();
    }
#ifdef HFOC_PRODUCTION_SERVICES
    (void)appliance_production_service_boot_init(
        appliance_production_boot_state_loader(&production_boot));
    log_production_service();
    log_production_tls();
#ifdef HFOC_PRODUCTION_TLS
    /*
     * ITEM-386: ordinary boot publishes a real hardware entropy owner, the
     * per-appliance management certificate and the management serving path.
     * Every step is published honestly: a CPU without RDRAND/RDSEED reports
     * AWAITING_ENTROPY and the listener stays down, and an appliance whose
     * state volume is unavailable still serves with a per-boot certificate
     * and reports DEGRADED.
     */
    (void)appliance_production_entropy_init(hfoc_lwip_time_now());
    (void)appliance_production_auth_init(&production_serving_auth);
    /*
     * ITEM-401: bind the persisted administrator fingerprint to the live
     * request gate and the local console.  No verifier is published here: the
     * gate stays closed until an operator presents the secret locally, and an
     * appliance whose record is still the factory placeholder reports
     * UNPROVISIONED instead of pretending to have a credential.
     */
    (void)appliance_production_admin_enrollment_init(
        &production_admin_enrollment, &production_serving_auth,
        appliance_production_boot_state_loader(&production_boot) != NULL
            ? &production_boot.state.secret_config
            : NULL);
    /*
     * ITEM-506: bind the salt source before anything can set the account, so
     * the first `credentials` conversation derives a real per-account salt.
     */
    (void)appliance_production_admin_enrollment_set_random(
        &production_admin_enrollment, kernel_admin_credential_random, NULL);
    console_bind_admin_enrollment(&production_admin_enrollment);
    console_bind_admin_credential_rekey(
        kernel_production_host_seed_rekey);
    console_bind_entropy_login(&production_entropy_login_binding);
#ifdef HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST
    /*
     * ITEM-507 increment 6: the two-certificate proof's fixture.  It sets the
     * test-only account and commits the two test-only name pairs before the
     * serving path reads the record, so the listener that comes up is the
     * production listener with a real declaration.  The define is set by one
     * fixture build and never by the production build.
     */
    appliance_certificate_name_fixture_seed(
        &production_admin_enrollment,
        appliance_production_boot_state_loader(&production_boot) != NULL
            ? &production_boot.state
            : NULL,
        hfoc_lwip_time_now());
#endif
    /*
     * ITEM-516: the durable after-boot policy decides whether this boot
     * publishes the credential without a local login.  It runs before the
     * enrollment record is rendered, so that record reports the state the
     * appliance is actually in.
     */
    publish_production_panel_gate();
    log_production_admin_enrollment();
    (void)appliance_production_serving_init(
        appliance_production_network_boot_network(),
        &production_serving_auth,
        NULL,
        appliance_production_boot_state_loader(&production_boot) != NULL
            ? &production_boot.state
            : NULL,
        appliance_production_service_boot_management_tls(),
        &production_serving_certificate,
        appliance_production_network_time_epoch_seconds(
            hfoc_lwip_time_now()));
    /*
     * ITEM-601: the SSH diagnostic owner shares the same network, state,
     * entropy and account owners as the management plane.  It publishes its
     * own gate; it never bypasses the local HOST_PROVISIONED unlock and does
     * not open port 22 until the host key and the one enrolled client key are
     * both durable.
     */
    appliance_production_ssh_init(
        &production_ssh,
        &production_boot.state,
        appliance_production_network_boot_network(),
        &production_admin_enrollment,
        appliance_production_entropy_random, NULL);
    console_bind_ssh_identity(&production_ssh.identity);
    console_bind_ssh_revoke(kernel_ssh_revoke, &production_ssh);
    /*
     * ITEM-508 increment 26: the ACME service waits for the certificate owner
     * to prepare the persisted identity; the event loop polls it and the
     * record says honestly whether the account is bound yet.
     */
    (void)appliance_production_acme_service_init(
        &production_acme_service, &production_serving_certificate);
    log_production_acme_service();
    publish_production_acme_panel_view();
    /*
     * ITEM-445 step 2: the browser sign-in form consults the same ITEM-401
     * enrollment the local console uses, so there is still one verifier, one
     * throttle and one session store.  An appliance whose record is still the
     * factory placeholder keeps the form UNAVAILABLE and says so.
     */
    (void)appliance_production_serving_bind_admin_enrollment(
        &production_admin_enrollment);
    /*
     * ITEM-400: publish the four read-only management snapshots and bind them
     * to the serving path under the same ITEM-365 request gate.  The first
     * publication happens before the listener is up, so it honestly reports
     * the awaiting state; the event loop republishes it as the owners move.
     */
    publish_production_management_evidence();
    (void)appliance_production_serving_publish_management_api(
        &production_management_api);
    log_production_serving();
    /*
     * ITEM-505 increment 2: publish the first live panel view before the
     * listener exists.  The serving path keeps it and hands it to the panel
     * the moment the panel is initialized, so the first section page a real
     * appliance serves already names the machine instead of `unavailable`.
     */
    publish_production_panel_view();
#endif
    (void)appliance_production_update_boot_init(&production_boot.state);
    log_production_update();
    (void)appliance_production_audit_boot_init(
        kernel_event_ring_get(),
        APPLIANCE_PRODUCTION_AUDIT_BOOT_GENERATION_FIRST,
        (uint64_t)appliance_production_boot_mode(&production_boot),
        (uint64_t)appliance_production_boot_readiness(&production_boot));
    log_production_audit();
#endif
#endif

    /*
     * ITEM-392: the console is told about the local input path before it
     * paints its readiness banner, so the local break-glass console never
     * advertises a keyboard the machine does not have.
     */
    console_publish_local_input(local_input_available);
    console_publish_input_source(local_input_source_now);
    console_init(
        usable_mib,
        mp_request.response != NULL &&
                mp_request.response->cpu_count != 0U
            ? (uint32_t)mp_request.response->cpu_count
            : 1U);
    kernel_vfs_boot_init();
    kernel_workspace_boot_init();
    publish_production_boot_runtime();
    {
        const uint64_t serial_flags = serial_sequence_begin();

        /*
         * ITEM-410: the readiness record is mirrored and stored, and it is the
         * trigger that puts the log on disk: this is the last record of a
         * successful boot, and a hardware run that reboots the machine
         * afterwards must still be able to read that the boot reached READY.
         */
        console_journal_begin();
        serial_write("[APPLIANCE-BOOT] runtime=");
        console_record_text("[APPLIANCE-BOOT] runtime=");
        serial_write(appliance_production_boot_readiness_string(
            appliance_production_boot_readiness(&production_boot)));
        console_record_text(appliance_production_boot_readiness_string(
            appliance_production_boot_readiness(&production_boot)));
        serial_write(" identity=OK config=");
        console_record_text(" identity=OK config=");
        serial_write(production_boot_config_source(&production_boot));
        console_record_text(
            production_boot_config_source(&production_boot));
        serial_write(" local_management=READY storage=");
        console_record_text(" local_management=READY storage=");
        serial_write(appliance_production_boot_readiness_string(
            appliance_production_boot_readiness(&production_boot)));
        console_record_text(appliance_production_boot_readiness_string(
            appliance_production_boot_readiness(&production_boot)));
        /*
         * ITEM-392: local keyboard availability is part of appliance
         * readiness, published rather than assumed. The local console is the
         * documented break-glass path, so an operator must be able to read
         * from the readiness record alone whether it has an input path.
         */
        serial_write(" local_input=");
        console_record_text(" local_input=");
        serial_write(local_input_available ? "AVAILABLE" : "UNAVAILABLE");
        console_record_text(
            local_input_available ? "AVAILABLE" : "UNAVAILABLE");
        /*
         * ITEM-395: which keyboard path supplies that local input, so an
         * operator reading only the readiness record knows whether the
         * break-glass console has a working keyboard and which one it is.
         */
        serial_write(" input_source=");
        console_record_text(" input_source=");
        serial_write(local_input_source_string(local_input_source_now));
        console_record_text(
            local_input_source_string(local_input_source_now));
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_journal_end();
        /* The end of boot is a flush moment: readiness is on the disk before
         * the machine is asked to do anything else. */
        appliance_boot_log_store_flush("readiness");
    }
#ifdef HFOC_PRODUCTION_TLS
    /*
     * ITEM-598: only a provisioned persistence partition starts the ordinary
     * login conversation.  An older installation keeps the explicit
     * `entropy seed` recovery instead of being asked for credentials that
     * cannot unlock anything.
     */
    if (kernel_production_host_seed_present()) {
        console_start_entropy_unlock();
    }
#endif
#ifdef HFOC_SCHEDULER_YIELD_QEMU_TEST
    const enum scheduler_status yield_init_status =
        sched_yield_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SCHED] yield proof init: ");
        serial_write(scheduler_status_string(yield_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_SCHEDULER_TICK_QEMU_TEST
    const enum scheduler_status tick_init_status =
        sched_tick_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SCHED-TICK] proof init: ");
        serial_write(scheduler_status_string(tick_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_SCHEDULER_WAIT_QEMU_TEST
    const enum scheduler_status wait_init_status =
        sched_wait_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[WAIT] proof init: ");
        serial_write(scheduler_status_string(wait_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_SCHEDULER_EXIT_QEMU_TEST
    const enum scheduler_status exit_init_status =
        sched_exit_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[EXIT] proof init: ");
        serial_write(scheduler_status_string(exit_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
    const enum scheduler_status stress_init_status =
        sched_stress_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[STRESS] proof init: ");
        serial_write(scheduler_status_string(stress_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_PROCESS_SCHEDULER_QEMU_TEST
    const enum process_status process_sched_init_status =
        process_sched_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PROC] proof init: ");
        serial_write(process_status_string(process_sched_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
#endif
#ifdef HFOC_USER_IRETQ_QEMU_TEST
    (void)user_iretq_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_SYSCALL_QEMU_TEST
    (void)syscall_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_USER_INIT_QEMU_TEST
    const enum user_init_status user_init_status =
        user_init_qemu_test_init(hhdm_request.response->offset);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[USER-INIT] init status: ");
        serial_write(user_init_status_string(user_init_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (user_init_status != USER_INIT_OK) {
        kernel_panic("User init proof init failed");
    }
#endif
#ifdef HFOC_BOOT_POLICY_QEMU_TEST
    boot_policy_qemu_test_init();
#endif
#ifdef HFOC_BLOCK_DEVICE_QEMU_TEST
    block_device_qemu_test_init();
#endif
#ifdef HFOC_DATA_REGION_QEMU_TEST
    data_region_qemu_test_init();
#endif
#ifdef HFOC_BLOCK_CACHE_QEMU_TEST
    block_cache_qemu_test_init();
#endif
#ifdef HFOC_CONFIG_JOURNAL_QEMU_TEST
    config_journal_qemu_test_init();
#endif
#ifdef HFOC_PHYSICAL_STORAGE_QEMU_TEST
    physical_storage_qemu_test_init();
    appliance_state_loader_qemu_test_init();
    ahci_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_HFOCFS_QEMU_TEST
    hfocfs_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_HFOCFS_WORKSPACE_QEMU_TEST
    hfocfs_workspace_qemu_test_init(hhdm_request.response->offset);
#endif
#if defined(HFOC_PRODUCTION_INSTALLER_QEMU_TEST) && \
    defined(HFOC_PRODUCTION_INSTALL_MEDIA)
    /*
     * ITEM-394: an authorised installer medium is a product install run, so
     * the in-process fixture proof never runs in the same boot and the serial
     * log of a product run stays clean.  The media the QEMU fixture proof
     * boots carries no declaration, so the fixture still owns that boot.
     */
    if (!production_install_flow_engaged()) {
        production_installer_qemu_test_init();
    }
#elif defined(HFOC_PRODUCTION_INSTALLER_QEMU_TEST)
    production_installer_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_CONFIG_QEMU_TEST
    appliance_persistent_config_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST
    appliance_network_config_qemu_test_init();
    appliance_network_diagnostics_qemu_test_init();
    appliance_dhcp_client_qemu_test_init();
    appliance_dns_resolver_qemu_test_init();
    appliance_network_lifecycle_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_NETWORK_CONFIG_QEMU_TEST
    appliance_persistent_network_config_qemu_test_init();
#endif
#ifdef HFOC_BACKUP_RESTORE_QEMU_TEST
    backup_restore_qemu_test_init();
#endif
#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST
    service_supervisor_qemu_test_init(hhdm_request.response->offset);
    appliance_service_manager_qemu_test_init();
    appliance_https_service_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_APPLIANCE_PERSISTENT_SERVICE_CONFIG_QEMU_TEST
    appliance_persistent_service_config_qemu_test_init(
        hhdm_request.response->offset);
#endif
#ifdef HFOC_APPLIANCE_SERVICE_FAILURE_HANDLER_QEMU_TEST
    appliance_service_failure_handler_qemu_test_init(
        hhdm_request.response->offset);
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_QEMU_TEST
    appliance_production_service_https_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_QEMU_TEST
    appliance_production_management_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_QEMU_TEST
    appliance_first_boot_integration_qemu_test_init();
#endif
#ifdef HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_QEMU_TEST
    appliance_recovery_factory_reset_integration_qemu_test_init();
#endif
#ifdef HFOC_LOCAL_CONTROL_QEMU_TEST
    local_control_qemu_test_init();
#endif
#ifdef HFOC_NETWORK_FOUNDATION_QEMU_TEST
    network_foundation_qemu_test_init();
#endif
#ifdef HFOC_ARP_ICMP_QEMU_TEST
    arp_icmp_qemu_test_init();
#endif
#ifdef HFOC_VIRTIO_FOUNDATION_QEMU_TEST
    virtio_foundation_qemu_test_init();
#endif
#ifdef HFOC_VIRTIO_NET_FRAME_QEMU_TEST
    virtio_net_frame_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_TRUST_POLICY_QEMU_TEST
    trust_policy_qemu_test_init();
#endif
#ifdef HFOC_BOOT_CHAIN_QEMU_TEST
    boot_chain_qemu_test_init();
#endif
#ifdef HFOC_UPDATE_LIFECYCLE_QEMU_TEST
    update_lifecycle_qemu_test_init();
#endif
#ifdef HFOC_SUPPORTABILITY_QEMU_TEST
    supportability_qemu_test_init();
#endif
#ifdef HFOC_ACCOUNTING_BOUNDARY_QEMU_TEST
    accounting_boundary_qemu_test_init();
#endif
#ifdef HFOC_STORAGE_INTERRUPTION_QEMU_TEST
    storage_interruption_qemu_test_init();
#endif
#ifdef HFOC_SERVICE_CASCADE_DRILL_QEMU_TEST
    service_cascade_drill_qemu_test_init(hhdm_request.response->offset);
#endif
#ifdef HFOC_LOCAL_CONTROL_OUTAGE_DRILL_QEMU_TEST
    local_control_outage_drill_qemu_test_init();
#endif
#ifdef HFOC_NETWORK_PARTITION_DRILL_QEMU_TEST
    network_partition_drill_qemu_test_init();
#endif
#ifdef HFOC_TRUST_FAILURE_DRILL_QEMU_TEST
    trust_failure_drill_qemu_test_init();
#endif
#ifdef HFOC_UPDATE_FAILURE_DRILL_QEMU_TEST
    update_failure_drill_qemu_test_init();
#endif
#ifdef HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_QEMU_TEST
    integration_replay_outage_drill_qemu_test_init();
#endif
#ifdef HFOC_POWER_CYCLE_CONTINUITY_DRILL_QEMU_TEST
    power_cycle_continuity_drill_qemu_test_init();
#endif
#ifdef HFOC_ENTROPY_QEMU_TEST
    entropy_qemu_test_init(hhdm_request.response->offset);
#endif
    cpu_sti();
    serial_write("[IRQ ] IF enabled; entering HLT event loop.\n");
    /* ITEM-463: start the cycle-counter calibration now that ticks move. */
    timer_cycle_clock_start();
    /*
     * ITEM-551: the USB 2.0 half.  The diagnostic medium sits on the machine's
     * EHCI function (the black port), and the xHCI half of the same machine is
     * the one failing at ENABLE_SLOT - so this walk is the only way the kernel
     * can reach the stick it was booted from.  It runs after the cycle-clock
     * calibration so every wait in it is a real bounded microsecond budget, and
     * it is never fatal: a machine with no EHCI function, no device on it, or a
     * device this minimal driver does not bind publishes `[EHCI]` and keeps
     * booting.
     */
    (void)ehci_storage_init(hhdm_request.response->offset);
    /* ITEM-486 (round 24): bind the framebuffer's own telemetry clock. */
    framebuffer_set_clock(kernel_framebuffer_clock_us);
    /* ITEM-474: from here the turn paints into one batch and flushes it. */
    console_begin_turn_batching();

    for (;;) {
        /* ITEM-474: close the previous turn's period and open this one's,
           before the turn's own cost is measured, so the whole turn - not the
           gap after it - is what `hlt_us` is subtracted from. */
        kernel_loop_phase_turn_begin();
        /* ITEM-461: the turn's own cost, measured before anything it does. */
        log_production_iteration();
        /* ITEM-463: finish the cycle-counter calibration within the first
           turns, before the costs the console publishes are read. */
        timer_cycle_clock_poll();
        struct keyboard_event event;
        struct kernel_loop_phase_span phase_span;
        struct kernel_loop_phase_span sub_span;
#ifdef HFOC_SERVICE_SUPERVISOR_QEMU_TEST
        /* ITEM-486: the supervisor's own turn, in the services span. */
        phase_span = kernel_loop_phase_begin();
        service_supervisor_qemu_poll();
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_SERVICES, phase_span);
#endif
        /*
         * ITEM-395: service the USB HID path and merge both local input
         * queues before the console consumes them. PS/2 keeps priority; the
         * USB queue is drained in a pass where PS/2 produced nothing.
         */
        /* ITEM-479: (1) the driver service and doorbell, inside the driver
           phase the record has always published as `xhci_us`. */
        phase_span = kernel_loop_phase_begin();
        sub_span = kernel_loop_phase_begin();
        local_input_service();
        kernel_loop_phase_end_input(KERNEL_LOOP_INPUT_XHCI, sub_span);
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_XHCI, phase_span);

        /* ITEM-479: (2) the keyboard's decoded events, inside `input_us`.
           ITEM-482: bounded to LOCAL_INPUT_KEYBOARD_BUDGET events per turn, so
           a burst of typing cannot starve the pointer; the rest stays queued and
           is served next turn, in order, with nothing dropped. */
        phase_span = kernel_loop_phase_begin();
        sub_span = kernel_loop_phase_begin();
        {
            const uint64_t kbd_started = timer_cycles();
            const uint64_t kbd_sync_before = console_keyboard_sync_us();
            uint32_t keyboard_served = 0U;

            while (keyboard_served < LOCAL_INPUT_KEYBOARD_BUDGET &&
                   local_input_pop_event(&event)) {
                console_process_event(&event);
                ++keyboard_served;
            }
            kernel_loop_input_kbd_events += keyboard_served;
            kernel_loop_input_kbd_draw_us +=
                timer_cycles_to_us(timer_cycles() - kbd_started);
            kernel_loop_input_kbd_sync_us +=
                console_keyboard_sync_us() - kbd_sync_before;
        }
        kernel_loop_phase_end_input(KERNEL_LOOP_INPUT_KEYBOARD, sub_span);
        /*
         * ITEM-448: the pointer's own events.  They are not merged with the
         * keyboard's queue: a mouse has no PS/2 counterpart to arbitrate
         * against, and a machine without one simply never produces an event
         * here.
         */
        /* ITEM-479: (3) the pointer's movement and drawing. */
        sub_span = kernel_loop_phase_begin();
        {
            /*
             * ITEM-484 (round 22): the arrow is visible as soon as the
             * controller reports a bound mouse, before the first report moves
             * it and without the operator clicking.  Readiness is the
             * controller's own state, so a zero report cannot delay it; the
             * call is a no-op once the arrow has been painted.
             */
            if (local_input_mouse_ready()) {
                console_pointer_ready();
            }
            /*
             * ITEM-480 (round 18): the pointer is served a bounded number of
             * coalesced batches per turn, so no input source can make one turn
             * unbounded.  The unserved reports stay queued and are served next
             * turn - nothing is lost - and the accumulated movement of a run
             * reaches the screen in one drawing.
             */
            struct usb_hid_mouse_batch mouse_batches
                [LOCAL_INPUT_MOUSE_DRAW_BUDGET];
            uint32_t mouse_batch_count = local_input_pop_mouse_batches(
                mouse_batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET);
            uint32_t mouse_batch_index;

            for (mouse_batch_index = 0U; mouse_batch_index < mouse_batch_count;
                 ++mouse_batch_index) {
                console_process_mouse_batch(
                    &mouse_batches[mouse_batch_index]);
                /* ITEM-482: the window's worst turn, not a busy second's sum. */
                kernel_loop_input_ptr_events +=
                    mouse_batches[mouse_batch_index].events;
                if (mouse_batches[mouse_batch_index].dx != 0 ||
                    mouse_batches[mouse_batch_index].dy != 0) {
                    ++kernel_loop_input_ptr_draws;
                }
            }
            /*
             * ITEM-484 (round 22): the deferred echo's replay belongs to this
             * input batch, not to the turn's tail.  A typed character on a
             * full screen used to be buffered by the batched scroll and
             * replayed by the end-of-turn `console_flush_deferred()`, after
             * the network, the heartbeat and the diagnostics - so the operator
             * saw the echo a whole turn late.  Replaying here, before the
             * batch's one drain, makes the echo visible inside the turn
             * without touching the scroll batch or its cost record; the
             * end-of-turn flush stays for records and diagnostics.
             */
            console_flush_deferred();
            /*
             * ITEM-482 (round 20): the whole input batch is drained once, after
             * the keyboard and the bounded mouse batches, instead of a drain per
             * report or per batch.  The mouse draws' write-combining stores are
             * what this makes visible; ITEM-484 adds the deferred echo's replay
             * to the same barrier.
             */
            console_end_input_batch();
        }
        kernel_loop_phase_end_input(KERNEL_LOOP_INPUT_POINTER, sub_span);
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_INPUT, phase_span);
        /* ITEM-486: the clock-driven console upkeep is application state. */
        phase_span = kernel_loop_phase_begin();
        while (timer_take_second_event()) {
            console_update_uptime(timer_uptime_seconds());
        }
        /*
         * ITEM-472 (СРОЧНО-22): the pointer's record is due on the honest
         * clock, not on the counted IRQ ticks - the tick counter can lose real
         * seconds, and with it the record the owner reads to feel the gain.
         */
        console_pointer_tick();
        /*
         * ITEM-430: the machine offers its own menu when nobody asks for it -
         * after an untouched keyboard's idle window, or at once when there is
         * no local input to touch.  The decision is local_menu.c's; this is
         * the loop turn it is asked on, and it is outside the production
         * network block because a machine whose network is absent still has a
         * screen and a menu.
         */
#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
        /*
         * ITEM-490: the operator entry names the disk before anything else on
         * this machine happens.  It runs before the menu offer, because a
         * machine waiting for the operator to choose a disk is not a machine
         * asking whether somebody wants a menu.
         */
        kernel_installer_disk_screen();
        /*
         * ITEM-500: an operator entry that refused before the screen could
         * open says why on the machine's own screen.  The two are exclusive:
         * a prepared run shows the disk screen, a refused one shows the
         * reason.
         */
        kernel_installer_refusal_screen();
#endif
        console_offer_local_menu(hfoc_lwip_time_now());
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_SERVICES, phase_span);
#ifdef HFOC_PRODUCTION_NETWORK
        phase_span = kernel_loop_phase_begin();
#endif
        (void)hfoc_lwip_time_poll();
#ifdef HFOC_PRODUCTION_NETWORK
        /*
         * ITEM-605: the HFOC adapter above services its own callback slots,
         * not lwIP's NO_SYS timeout list.  Without this call the production
         * loop never runs TCP retransmission/RTO, tcp_poll, keepalive, ARP
         * ageing or the other cyclic handlers; the QEMU network is lossless,
         * so only a real link with a dropped segment exposed it.  One
         * bounded turn per loop iteration, after the adapter's own slots and
         * before the network lifecycle polls that consume their effects.
         */
        (void)hfoc_lwip_timeouts_service();
        appliance_production_network_boot_poll(hfoc_lwip_time_now());
        appliance_production_network_wire_boot_poll(hfoc_lwip_time_now());
#ifdef HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST
        /*
         * ITEM-508 increment 15: the offline directory exchange runs on the
         * production network, so its driver needs the wire's published
         * lifecycle and the loop that moves it.
         */
        appliance_acme_directory_qemu_test_tick(hfoc_lwip_time_now());
#endif
#ifdef HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST
        /*
         * ITEM-508 increments 17/31: the offline ACME order runs on the same
         * production network and the same published lifecycle, and it gets the
         * production HTTP-01 listener so the fixture can arm it from the flow
         * it is really driving (increment 31).  The listener is initialised in
         * the service phase of earlier turns; the driver arms only once it is
         * really listening.
         */
        appliance_acme_order_qemu_test_tick(
            &production_acme_http01, hfoc_lwip_time_now());
#endif
        /*
         * ITEM-449: the relay's turn.  It brings the declared listener up
         * (retrying a bind that failed), pumps both directions, ends a session
         * that stopped moving, and leaves one event for the record above.
         */
        if (production_forward_published) {
            (void)hfoc_lwip_forward_service(
                &production_forward, hfoc_lwip_time_now());
            log_production_forward();
        }
        /* ITEM-389: publishes a backend whose state changes after boot, e.g.
           a PHY that only finishes negotiating once the loop is running. */
        log_production_network();
        log_production_network_wire();
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_NETWORK, phase_span);
        /*
         * ITEM-428: the loop's own liveness.  Every record above is a state
         * change, so a loop with nothing new to say published nothing - and on
         * 2026-09-16 the boot log therefore ended ten seconds in with no way to
         * tell a quiet machine from a stopped one.  The heartbeat names the
         * tick it was published at and the live frame-path counters, so the
         * next run reads "the loop is alive" and "the transmit path never
         * completed a descriptor" from the same line.
         *
         * ITEM-486: the once-a-second records are the journal span; the durable
         * log's own disk flush happens inside it and is published here.
         */
        phase_span = kernel_loop_phase_begin();
        log_production_loop_heartbeat();
        /*
         * ITEM-469: the input counters have their own once-a-second record, so
         * the QEMU fast-typing proof and the next hardware run see the queue
         * depth (`kbd_armed`) and the missed-service count (`kbd_mse`) during
         * the burst, not only at the ten-second heartbeat.
         */
        log_production_input_record();
        /*
         * ITEM-474 (round 13): the loop's own phase budget, once a second, so
         * the next run can tell a working turn from a sleeping one.
         */
        log_production_loop_phases();
        /*
         * ITEM-451: the heartbeat's clock is the PIT, so the first frames a
         * peer sent would wait for the next ten-second tick - and on a loaded
         * runner for much longer.  This names each class on the turn it first
         * appears and is bounded by the class count.
         */
        log_production_frame_classes();
        /* ITEM-438: a bound keyboard that never answers is published once,
           instead of reading like a keyboard nobody has touched. */
        log_production_keyboard_silence();
        /* ITEM-448: the same finding for a bound pointer that never moved. */
        log_production_mouse_silence();
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_JOURNAL, phase_span);
        /*
         * ITEM-468 (СРОЧНО-18): the input path's own heartbeat.  The records
         * above are published "on change", so a machine whose pointer stays
         * silent and whose keyboard arrives late published one snapshot and
         * nothing that showed the controller and the loop between two passes.
         * This publishes at most once a second - one short journal record per
         * live controller, per HID role and for the loop window - and changes
         * no behaviour.
         */
        {
            struct appliance_production_network_io_counters diag_counters;

            phase_span = kernel_loop_phase_begin();
            memset(&diag_counters, 0, sizeof(diag_counters));
            appliance_production_network_boot_io_counters(&diag_counters);
            xhci_service_diagnostics(diag_counters.available
                                         ? diag_counters.rx_frames
                                         : 0U);
            kernel_loop_phase_end(KERNEL_LOOP_PHASE_XHCI, phase_span);
        }
        /* ITEM-486: the management publication is application state. */
        phase_span = kernel_loop_phase_begin();
        publish_production_management();
        kernel_loop_phase_end(KERNEL_LOOP_PHASE_SERVICES, phase_span);
        if (production_first_boot_ready) {
            const struct appliance_network_wire *wire =
                appliance_production_network_wire_boot_wire();
            const struct network_ipv4_config *active =
                wire != NULL ? appliance_network_wire_active_config(wire)
                             : NULL;
            const bool network_ready = active != NULL;
            const struct appliance_identity *identity =
                appliance_identity_get();
            const char *hostname =
                identity != NULL &&
                        identity->hostname[0] != '\0'
                    ? identity->hostname
                    : APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME;

            /*
             * ITEM-403: ordinary first boot commits hostname and network and
             * defers the administrator credential to the local console
             * enrollment owner; no public placeholder is ever committed.
             *
             * ITEM-486: the commit is storage work; the record it publishes is
             * journal work, so the two are measured apart.
             */
            phase_span = kernel_loop_phase_begin();
            (void)appliance_production_first_boot_run_deferred(
                &production_first_boot, hostname,
                production_first_boot_text_length(hostname), active,
                network_ready, network_ready);
            kernel_loop_phase_end(KERNEL_LOOP_PHASE_STORAGE, phase_span);
            phase_span = kernel_loop_phase_begin();
            log_production_first_boot();
            kernel_loop_phase_end(KERNEL_LOOP_PHASE_JOURNAL, phase_span);
        }
#ifdef HFOC_PRODUCTION_SERVICES
        {
            const struct appliance_network_wire *service_wire =
                appliance_production_network_wire_boot_wire();
            const struct network_ipv4_config *service_active =
                service_wire != NULL
                    ? appliance_network_wire_active_config(service_wire)
                    : NULL;
            const uint32_t service_now_ms = hfoc_lwip_time_now();

            /*
             * ITEM-364 activates the fixed HTTPS service only when the
             * network is active and the entropy and certificate owners have
             * published readiness. ITEM-375 supplies the real certificate
             * owner: the management TLS lifecycle, which stays honestly
             * awaiting a validated import until one exists. The frozen
             * baseline still has no ordinary-boot entropy owner, so the
             * activation stays honestly awaiting readiness instead of
             * fabricating a TLS endpoint.
             *
             * ITEM-486: the service poll, its TLS evidence and its records are
             * the services span.
             */
            phase_span = kernel_loop_phase_begin();
            appliance_production_service_boot_poll_clock(
                service_active != NULL,
                appliance_production_entropy_ready(),
                appliance_production_network_time_ready(),
                service_now_ms,
                appliance_production_network_time_epoch_seconds(
                    service_now_ms));
#ifdef HFOC_PRODUCTION_TLS
            publish_production_management_evidence();
            appliance_production_serving_poll(
                service_now_ms,
                appliance_production_network_time_epoch_seconds(
                    service_now_ms));
            appliance_production_ssh_poll(&production_ssh, service_now_ms);
            /*
             * ITEM-508 increment 26: the ACME service binds the identity the
             * serving poll just prepared and republishes its record.  The
             * clock bit is the same validated wall clock the management
             * lifecycle uses; an unvalidated clock is refused by the policy
             * by name, not silently accepted.
             *
             * ITEM-508 increment 34: the durable control the operator set is
             * read here and handed to the service before it acts.  A service
             * without a published record would keep its pre-increment
             * behavior; production always publishes the loaded record, and an
             * absent record there is the off state.
             */
            {
                const struct appliance_acme_control *acme_control =
                    appliance_production_serving_acme_control();

                appliance_production_acme_service_set_control(
                    &production_acme_service,
                    acme_control != NULL &&
                        appliance_acme_control_enabled(acme_control),
                    acme_control != NULL &&
                        appliance_acme_control_order_requested(acme_control),
                    acme_control != NULL
                        ? appliance_acme_control_order_requested_at(
                              acme_control)
                        : 0U);
            }
            (void)appliance_production_acme_service_poll(
                &production_acme_service, &production_serving_certificate,
                appliance_production_network_time_epoch_seconds(
                    service_now_ms));
            /*
             * ITEM-508 increment 28: the HTTP-01 listener starts once, as soon
             * as an active wire configuration exists, and carries the same
             * validated clock the ACME service uses.  It comes up unarmed;
             * nothing on this boot arms a token until the order path does.
             */
            if (service_active != NULL && !production_acme_http01_started) {
                const struct appliance_production_network *http01_network =
                    appliance_production_network_boot_network();

                if (http01_network != NULL) {
                    (void)appliance_production_acme_http01_init(
                        &production_acme_http01, &http01_network->netif,
                        service_active);
                    production_acme_http01_started = true;
                }
            }
#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST)
            /*
             * ITEM-508 increments 29/31: both fixture families run with no
             * SNTP peer, so the production wall clock is zero - and the
             * decision module refuses a zero clock by name (`BAD_STATE` at
             * arming, `EXPIRED` on every request, measured on run
             * 37415921700).  The families therefore name one shared fixture
             * epoch and hand that value to the listener's poll, so the arming
             * instant and every later decision share one clock and the 900 s
             * answer window is the production window.  Production builds keep
             * the validated production clock, unchanged, in the branch below.
             */
            {
                const uint32_t fixture_now_seconds =
                    appliance_acme_fixture_epoch_seconds();

                (void)appliance_production_acme_http01_poll(
                    &production_acme_http01, fixture_now_seconds);
#ifdef HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST
                appliance_acme_http01_fixture_qemu_tick(
                    &production_acme_http01, fixture_now_seconds);
#endif
            }
#else
            (void)appliance_production_acme_http01_poll(
                &production_acme_http01,
                appliance_production_network_time_epoch_seconds(
                    service_now_ms));
#endif
            /*
             * ITEM-508 increment 35: the order path ordinary boot runs.  The
             * composition lives in its own TLS-profile translation unit - it
             * binds the mbedTLS transport link - and the loop just publishes
             * the owners it borrows, ticks it once and mirrors the driver's
             * record when it moves.
             */
#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
            /*
             * ITEM-508 increment 36: the wire-proof guest has no operator and
             * no first-boot account, so the production serving path never
             * prepares the account identity the order key comes from.  The
             * fixture prepares it through the same production call the serving
             * path uses, on the blank state volume the harness supplies.
             */
            if (!appliance_production_certificate_available(
                    &production_serving_certificate)) {
                const struct appliance_state_loader *acme_fixture_loader =
                    appliance_production_boot_state_loader(&production_boot);

                if (acme_fixture_loader != NULL) {
                    (void)appliance_production_certificate_prepare(
                        &production_serving_certificate,
                        (struct appliance_state_loader *)acme_fixture_loader,
                        appliance_acme_fixture_epoch_seconds());
                }
            }
#endif
            appliance_production_acme_order_boot_publish(
                &production_acme_service, &production_serving_certificate,
                &production_acme_http01);
            /*
             * ITEM-508 increment 36: the ACME fixture families run with no
             * SNTP peer, so the production wall clock is zero and an order
             * would refuse `BAD_CLOCK`.  They hand the same shared fixture
             * epoch the HTTP-01 listener already uses to the order path, and
             * the wire-proof family asks for its one order here.
             */
#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST)
            {
                const uint32_t acme_order_epoch =
                    appliance_acme_fixture_epoch_seconds();

#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
                appliance_production_acme_order_boot_fixture_request();
#endif
                appliance_production_acme_order_boot_tick(
                    service_now_ms, acme_order_epoch);
            }
#else
            appliance_production_acme_order_boot_tick(
                service_now_ms,
                appliance_production_network_time_epoch_seconds(
                    service_now_ms));
#endif
            log_production_acme_order();
#endif
            log_production_service();
            log_production_tls();
#ifdef HFOC_PRODUCTION_TLS
            log_production_serving();
            log_production_acme_service();
            /*
             * ITEM-508 increment 32: the read-only ACME block of the
             * Certificates page follows the same state the record above
             * publishes; the serving path republishes the page only when the
             * view moved, so this is cheap on every turn.
             */
            publish_production_acme_panel_view();
            log_production_acme_http01();
            /*
             * ITEM-505 increment 2: republish the live panel view as the
             * hostname, the management MAC, the service and the serving state
             * move; the panel keeps rendering the last values it was given.
             */
            publish_production_panel_view();
            /*
             * ITEM-504 increment 5: a routing mutation decided during this
             * turn publishes its own record here, before the loop moves on.
             * ITEM-505: the network mutation's record is drained on the same
             * turn boundary, and increment 3 drains the hostname record
             * beside it.
             */
            log_production_route();
            log_production_routes();
            log_production_network_set();
            log_production_system_set();
            log_production_account_set();
            log_production_sni();
            log_production_certificate_name();
            /* ITEM-508 increment 34: the ACME control mutation's record. */
            log_production_acme_control();
#endif
            kernel_loop_phase_end(KERNEL_LOOP_PHASE_SERVICES, phase_span);
        }
#endif
#endif
#ifdef HFOC_SCHEDULER_YIELD_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_SCHEDULER_TICK_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_SCHEDULER_WAIT_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_SCHEDULER_EXIT_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_PROCESS_SCHEDULER_QEMU_TEST
        (void)sched_yield();
#endif
#ifdef HFOC_USER_INIT_QEMU_TEST
        (void)sched_yield();
#endif

        /*
         * ITEM-474: one body move for every line this turn printed, then
         * close the turn's work measurement.  The flush is outside every
         * record's interrupt-off serial sequence, so the move no longer
         * swallows the PIT edges the next turn waits for.
         */
        console_flush_deferred();
        kernel_loop_phase_turn_end();

#ifdef HFOC_PRODUCTION_TLS
        /*
         * ITEM-629: once the owner shell brought the cooperative scheduler up,
         * give a runnable shell/TTY read a turn.  Before that this is a named
         * no-op and the ordinary loop is unchanged.
         */
        (void)sched_yield();
        /*
         * ITEM-655: a pending browser sign-in is real work for this loop.  Do
         * not park until the next PIT edge between its measured slices: the
         * next pass advances the same job and still runs the network, console
         * and watchdog work first.  `cpu_hlt()` resumes as soon as the job has
         * completed, been refused or was cancelled; a graceful peer FIN does
         * not cancel the bounded derivation early, so it is allowed to finish.
         */
        if (appliance_production_serving_panel_auth_pending()) {
            continue;
        }
#endif
        /* The next IRQ0/IRQ1 wakes the processor. Keyboard and timer work is
           kept in their interrupt handlers; rendering stays in this loop. */
        cpu_hlt();
    }
}
