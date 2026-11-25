/*
 * ITEM-551: the polling EHCI host driver, and the USB 2.0 mass-storage device
 * the machine actually boots from.
 *
 * Why this exists: the owner's Toshiba L875 boots the diagnostic stick through
 * the firmware and hands the kernel a machine where the stick is invisible.
 * The machine's own Linux capture names the shape - `00:14.0` is the xHCI
 * function (the one failing at `ENABLE_SLOT`), while `00:1a.0` and `00:1d.0`
 * are Intel 7 Series EHCIs and the black USB 2 port enumerates on
 * `Bus 001: ehci-pci/2p 480M`.  Until this item the kernel carried no EHCI
 * driver, so `[INSTALL-MEDIUM] state=UNAVAILABLE reason=NO_DEVICE` was the
 * honest verdict of a machine whose boot medium the firmware could read and the
 * kernel could not.
 *
 * Scope, deliberately minimal and in the shape the rest of the storage stack
 * already has:
 *
 *   * one EHCI function at a time, high-speed devices on its own root ports
 *     only - no hubs, no split transactions, no full/low-speed companions;
 *   * no interrupt is enabled and none is routed (`USBINTR` stays zero): every
 *     wait is a bounded poll of the completion the controller writes into
 *     memory, so the IRQ/APIC/SMP scope of the project is untouched;
 *   * a control pipe and one bulk pipe - the two pipes USB Mass Storage
 *     Bulk-Only Transport needs - carried by the same `usb_storage` core and
 *     `usb_storage_block_device` view the xHCI path already proves;
 *   * the boot-medium guard and the reserved `HFOC/INSTALL.LOG` binding are the
 *     existing `install_medium_identify()` and
 *     `install_journal_platform_bind_medium()`, so the diagnostic export writes
 *     exactly where the medium's build reserved the space and nowhere else.
 *
 * The registers, the queue head and the queue element transfer descriptor are
 * EHCI 1.0 structures; the pure encoders and the transfer planner they use live
 * in kernel/src/ehci_queue.c and are proven on the host.
 */

#include "acpi.h"
#include "ehci.h"
#include "ehci_engine.h"
#include "ehci_init.h"
#include "ehci_legacy.h"
#include "ehci_port.h"
#include "ehci_regs.h"
#include "ehci_verdict.h"
#include "ehci_walk.h"

#include "arch/x86_64/timer.h"
#include "console.h"
#include "install_journal_platform.h"
#include "install_medium.h"
#include "kstring.h"
#include "pci.h"
#include "pci_io.h"
#include "pmm.h"
#include "serial.h"
#include "usb_hid.h"
#include "usb_storage.h"
#include "vmm.h"

#define EHCI_PCI_COMMAND 0x04U
#define EHCI_PCI_COMMAND_MEMORY 0x0002U
#define EHCI_PCI_COMMAND_BUS_MASTER 0x0004U
#define EHCI_PCI_COMMAND_INTX_DISABLE 0x0400U

#define EHCI_MMIO_MIN_BYTES 0x1000U
#define EHCI_MMIO_MAX_BYTES (16U * 4096U)

/* USB request codes this driver issues itself (USB 2.0 table 9-4). */
#define EHCI_REQUEST_CLEAR_FEATURE 0x01U
#define EHCI_REQUEST_SET_ADDRESS 0x05U
#define EHCI_REQUEST_RECIPIENT_MASK 0x1FU
#define EHCI_REQUEST_RECIPIENT_ENDPOINT 0x02U
#define EHCI_REQUEST_FEATURE_ENDPOINT_HALT 0x00U

/* Bounds.  Every wait is bounded and named; a stuck device costs milliseconds,
   never the boot. */
#define EHCI_RESET_BUDGET_US 250000U
#define EHCI_CONFIGFLAG_BUDGET_US 100000U
#define EHCI_ASYNC_BUDGET_US 100000U
/* ITEM-567: the reset's hold and completion window live with the sequence
   itself (kernel/include/ehci_port.h), so the driver has one copy. */
#define EHCI_ADDRESS_SETTLE_US 2000U
#define EHCI_TRANSFER_BUDGET_US 500000U
#define EHCI_CONTROL_BUDGET_US 200000U
#define EHCI_IAA_BUDGET_US 100000U
/* Fallback budgets when the cycle clock is not calibrated yet: `pause` spins at
   the same order of magnitude the xHCI driver uses for its own bounded waits. */
#define EHCI_SPIN_WAIT 4000000U
#define EHCI_SPIN_TRANSFER 20000000U

/* The device address this driver gives the device it binds, and the next one
   it may hand to a device behind a hub. */
#define EHCI_DEVICE_ADDRESS 1U
#define EHCI_DEFAULT_ADDRESS 0U
#define EHCI_MAX_ADDRESS 7U

/*
 * USB 2.0 hubs (chapter 11) and the integrated rate matching hub the owner's
 * machine uses.
 *
 * The Intel 7 Series PCH does not put its USB 2.0 root ports on the wire: the
 * datasheet (326776-003, section 5.19) describes an Integrated Rate Matching
 * Hub between the EHCI controller and the ports, and the machine's own Linux
 * capture shows it as a hub device on `Bus 001: ehci-pci/2p 480M` with the
 * card reader and the camera behind it.  A driver that binds only a device on
 * the root port therefore never sees the stick in the black port - which is
 * exactly the `NO_MEDIUM` the first hardware run of ITEM-552 reported.
 *
 * A high-speed hub is transparent for a high-speed device behind it: no split
 * transaction is involved, so the EHCI queue head addresses the device directly
 * and this is still a polling driver.  Full/low-speed devices need split
 * transactions through the hub's transaction translator, which this driver
 * does not implement - they are refused by name.
 */
#define EHCI_HUB_MAX_PORTS 15U
#define EHCI_HUB_POWER_BUDGET_US 100000U
#define EHCI_HUB_RESET_BUDGET_US 200000U
#define EHCI_EP0_MAX_PACKET_HIGH_SPEED 64U
#define EHCI_DEVICE_DESCRIPTOR_BYTES 18U
#define EHCI_CONFIG_DESCRIPTOR_MAX 256U

/*
 * ITEM-582: the installed machine refused `diag save` with
 * `ehci=DESCRIPTOR_FAILED/CONFIG_TOTAL` and the record could not say which
 * candidate produced it, what request was sent, what came back or how many
 * bytes the device itself computed as its configuration length.  The walk
 * offers several devices (the owner's machine has a rate matching hub), so a
 * refusal on one candidate must not erase the others: every candidate keeps its
 * own record, in walk order, and the report and the export read that array.
 */
#define EHCI_CONFIG_RECORDS 8U
#define EHCI_CONFIG_RAW_BYTES 9U

struct ehci_config_evidence {
    bool valid;
    uint32_t root_port;
    uint8_t hub_address;
    uint8_t hub_port;
    uint8_t address;
    uint16_t vendor_id;
    uint16_t product_id;
    /* The eight bytes of the GET_DESCRIPTOR(CONFIGURATION) request exactly as
       the driver sent them: bmRequestType, bRequest, wValue lo/hi, wIndex
       lo/hi, wLength lo/hi. */
    uint8_t setup[8];
    uint32_t requested;   /* the request's own wLength */
    uint32_t received;    /* bytes the device returned for that request */
    uint8_t raw[EHCI_CONFIG_RAW_BYTES];
    uint16_t total;       /* wTotalLength computed from the nine bytes, 0 none */
    uint8_t interface_class; /* the interface class the configuration declared */
    bool block_bound;     /* a BOT interface was found and the stick bound */
    const char *stage;    /* CONFIG9, CONFIG_TOTAL, CONFIG, PARSE, ... */
    const char *detail;   /* the stage's own token, when it has one */
};

struct ehci_device {
    bool bound;
    uint8_t address;
    uint16_t ep0_max_packet;
    /* The root port the device (or the hub it hangs behind) sits on. */
    uint32_t root_port;
    uint8_t config_value;
    uint8_t interface_number;
    /* Where the bound device hangs: the hub's address and port, or 0/0 for a
       device on the controller's own root port. */
    uint8_t hub_address;
    uint8_t hub_port;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t endpoint_in_max_packet;
    uint16_t endpoint_out_max_packet;
    bool in_toggle;
    bool out_toggle;
    uint16_t vendor_id;
    uint16_t product_id;
    struct usb_storage usb;
    struct usb_storage_block_device block;
    bool block_bound;
    bool medium_probed;
    bool medium_matched;
    enum install_medium_result medium_result;
    const char *medium_reason;
    /* ITEM-582: the separately prepared diagnostic stick, bound as the export
       medium on an installed system, never as the boot medium. */
    bool diagnostic_probed;
    bool diagnostic_bound;
    const char *diagnostic_result;
    const char *diagnostic_reason;
};

struct ehci_controller {
    bool present;
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    volatile uint8_t *regs;
    uint32_t bar_physical;
    uint64_t bar_length;
    uint32_t mmio_bytes;
    uint8_t caplength;
    uint16_t hci_version;
    uint32_t hcsparams;
    uint32_t hccparams;
    /* ITEM-569: HCCPARAMS.AC64, kept as the format decision it is. */
    bool ac64;
    uint32_t ports;
    uint8_t *dma;
    uint64_t dma_physical;
    struct ehci_qh *list_head;
    struct ehci_qh *transfer;
    struct ehci_qtd *qtds;
    uint8_t *bounce;
    uint32_t qtds_used;
    bool async_enabled;
    /* ITEM-557: the async-advance doorbell was not acknowledged, so the
       controller never confirmed it left the transfer queue head.  The fixed
       DMA pool must not be rewritten under it: the engine refuses further
       transfers until the controller is reset, and the record names it. */
    bool engine_stalled;
    struct ehci_device device;
    /* ITEM-554: why the last transfer failed, kept before the queue head is
       rewritten so a refusal carries the descriptor and the registers it
       failed on instead of only a stage name. */
    struct {
        bool valid;
        enum ehci_transfer_result result;
        uint8_t address;
        uint8_t endpoint;
        uint8_t hub_address;
        uint8_t hub_port;
        uint32_t failed_index;
        uint32_t failed_token;
        uint32_t token_count;
        uint32_t tokens[EHCI_MAX_QTD_PER_TRANSFER];
        uint32_t usbcmd;
        uint32_t usbsts;
        uint32_t portsc;
        /* ITEM-558: what the transfer actually moved, and the root port it ran
           on, so the detail names the device path and not only the stage. */
        uint32_t received;
        uint32_t residual;
        uint8_t root_port;
        /* ITEM-559: the controller's own verdict - a halted or errored
           schedule - kept beside the descriptor classifier's result, and
           whether the stop was acknowledged. */
        enum ehci_engine_cause cause;
        /* ITEM-564: the stop's verdict and its doorbell, kept apart from the
           transfer's cause so a cleanup failure cannot overwrite it. */
        enum ehci_engine_cleanup cleanup;
        uint32_t doorbell;
        uint32_t doorbell_readback;
        uint32_t usbcmd_before_link;
        uint32_t usbsts_before_link;
        uint32_t usbcmd_first_refusal;
        uint32_t usbsts_first_refusal;
        uint32_t usbcmd_after;
        uint32_t usbsts_after;
        uint32_t ass_waited_us;
        bool stale_iaa;
        bool schedule_running;
        bool ass_seen;
        bool halted_confirmed;
        bool restarted;
        bool restart_confirmed;
        bool quiesced;
        bool stalled;
        /* The rendered snapshot the operator reads (or NULL-length when none). */
        char text[EHCI_FAILURE_TEXT_MAX];
    } failure;
    /* The next free USB address on this controller; the root device takes 1 and
       every further device behind a hub takes the next one. */
    uint8_t next_address;
    bool hub_seen;
    uint8_t hub_address;
    uint8_t hub_ports;
    /* ITEM-556: where the selected device hung, for the record. */
    uint8_t selected_hub_address;
    uint8_t selected_hub_port;
    /* ITEM-556: the handoff verdict and the registers read back after reset,
       so the record proves the controller was reconfigured under the OS and
       not left with the firmware's interrupt mask. */
    const char *legacy;
    uint32_t usbintr;
    uint32_t usbcmd_after_reset;
    uint32_t usbsts_after_reset;
    /* ITEM-560: what the controller was told before the first start, and what
       it reported back.  The HSE of ITEM-559 could not be attributed to a
       moment because none of this was kept. */
    struct {
        uint32_t command_before;
        uint32_t command;
        uint32_t status_before;
        uint32_t status_after_clear;
        bool read_back_ok;
    } pci;
    struct {
        uint32_t usbcmd_before_reset;
        uint32_t usbsts_before_reset;
        bool halted_before;
        uint32_t usbcmd_after_hcreset;
        uint32_t usbsts_after_hcreset;
        bool pse_was_set;
        bool pss_was_set;
        uint32_t pse_clear_word;
        uint32_t pse_clear_readback;
        bool pss_stopped;
        uint32_t periodiclistbase;
        uint32_t asynclistaddr;
        uint32_t ctrldssegment;
        uint32_t configflag;
        uint32_t usbintr;
        uint32_t usbcmd_before_start;
        uint32_t usbcmd_at_start;
        uint32_t usbcmd_readback;
        uint32_t usbsts_at_start;
        bool periodic_off;
        bool async_on;
    } start;
    enum ehci_hse_stage hse_stage;
    uint32_t hse_usbcmd;
    uint32_t hse_usbsts;
    /* ITEM-562: this controller's own walk verdict.  Kept per controller so a
       second controller can never answer for the first one. */
    enum ehci_walk_outcome walk_outcome;
    const char *walk_stage;
    /* ITEM-564: the last stop's own facts, so the [EHCI] line says whether the
       doorbell was acknowledged, refused or ignored - and with which words. */
    enum ehci_engine_cleanup last_cleanup;
    uint32_t last_doorbell;
    uint32_t last_doorbell_readback;
    uint32_t last_usbcmd_after;
    uint32_t last_usbsts_after;
    bool last_stale_iaa;
    bool last_halted_confirmed;
    bool last_restarted;
    bool last_restart_confirmed;
    uint32_t walk_port;
    uint8_t walk_hub_address;
    uint8_t walk_hub_port;
    uint32_t attached;
    /* ITEM-562: every connected port's path, in walk order, so the machine can
       say which port ended where instead of one stage for the whole walk. */
    struct ehci_walk_port walk_ports[EHCI_WALK_PORT_RECORDS];
    uint32_t walk_port_count;
    /* ITEM-563: HCSPARAMS.PPC and every root port's own two reads, including
       the ports that read empty - the evidence `connected=0` never had. */
    bool ppc;
    struct ehci_port_state port_probes[EHCI_PORT_RECORDS];
    uint32_t port_probe_count;
    /* ITEM-582: the boot measurements, kept apart from the rescan's.  The
       rescan re-reads PORTSC (a stick plugged in after boot changes CCS), and
       the first snapshot must not be overwritten by that reading. */
    struct ehci_port_state boot_port_probes[EHCI_PORT_RECORDS];
    uint32_t boot_port_probe_count;
    bool boot_probes_saved;
    /* ITEM-561: the initialization module's own verdict, kept as its name so
       the start evidence names the step that refused the controller. */
    const char *init_cause;
    /* The DMA pool's physical range, for the record and the reachability
       check. */
    uint64_t dma_base;
    uint32_t dma_bytes;
    enum ehci_status status;
    const char *detail;
    uint32_t ports_connected;
    uint32_t port;
    uint32_t portsc;
    /* ITEM-567: each reset port's own words, so a refusal can be read without
       treating the pre-reset port record as a post-reset snapshot. */
    struct ehci_port_reset_state reset_probes[EHCI_PORT_RECORDS];
    uint32_t reset_probe_count;
    /* ITEM-568: the PCI status word at the first HSE sample, kept before any
       other configuration access could clear it. */
    uint32_t reset_pcists[EHCI_PORT_RECORDS];
    bool reset_pcists_valid[EHCI_PORT_RECORDS];
    /* ITEM-568: the BIOS -> OS handoff's own words, in configuration space. */
    struct ehci_legacy_state legacy_state;
    bool legacy_valid;
    /* ITEM-582: one configuration record per candidate the walk offered, in
       walk order.  A refusal on one device keeps the others, so the export can
       say which candidate failed and with which bytes. */
    struct ehci_config_evidence config_records[EHCI_CONFIG_RECORDS];
    uint32_t config_count;
    /* ITEM-582: the root ports a device was already attached on, so a later
       rescan (the operator plugged the diagnostic stick in after boot) does
       not reset a port that already carries a working device. */
    uint32_t known_ports_mask;
    /* ITEM-582: the root ports a working storage device is bound on.  A later
       rescan skips exactly these, so a device that is already serving the
       machine is never reset by the search for the diagnostic stick. */
    uint32_t bound_ports_mask;
    /* ITEM-582: the journal owns the attached device, so the driver must never
       zero its structure, clear its bound flags or reset its port while the
       export is being written, flushed and read back. */
    bool pinned_diagnostic;
    /* ITEM-582: a rescan's own report, kept beside the boot walk's.  The boot
       verdict and its port records stay intact: the operator's later plug-in
       must not overwrite the evidence of why the machine started without the
       medium. */
    bool rescan_done;
    const char *rescan_outcome;
    struct ehci_walk_port rescan_ports[EHCI_WALK_PORT_RECORDS];
    uint32_t rescan_port_count;
    uint32_t rescan_connected;
    uint32_t rescan_attached;
};

static struct ehci_controller ehci_controllers[EHCI_MAX_CONTROLLERS];
static uint32_t ehci_scanned;
static int ehci_active = -1;
/*
 * ITEM-562: the machine's answer is chosen from every controller's own entry,
 * never written by whichever controller was scanned last.  The entries and the
 * rendered list stay in RAM for the refusal record and the `ehci` command, so a
 * machine whose journal is unbound can still be asked what each controller did.
 */
static struct ehci_verdict_entry ehci_entries[EHCI_MAX_CONTROLLERS];
static struct ehci_verdict_choice ehci_choice = {
    -1, EHCI_STATUS_CONTROLLER_ABSENT, "NOT_PROBED", false, 0U};
static char ehci_entry_text[EHCI_VERDICT_TEXT_MAX];
static bool ehci_entries_ready;
static uint64_t ehci_hhdm;

/* ITEM-562: the name of a walk outcome, for the per-port record. */
static const char *ehci_walk_outcome_string(enum ehci_walk_outcome outcome) {
    switch (outcome) {
    case EHCI_WALK_OUTCOME_OK:
        return "OK";
    case EHCI_WALK_OUTCOME_MEDIUM:
        return "NOT_MEDIUM";
    case EHCI_WALK_OUTCOME_NO_PORT:
        return "NO_PORT";
    case EHCI_WALK_OUTCOME_PORT_RESET:
        return "PORT_RESET";
    case EHCI_WALK_OUTCOME_HUB:
        return "HUB";
    case EHCI_WALK_OUTCOME_TRANSFER:
        return "TRANSFER";
    case EHCI_WALK_OUTCOME_ATTACH:
        return "ATTACH";
    case EHCI_WALK_OUTCOME_NO_DEVICE:
        return "NO_DEVICE";
    case EHCI_WALK_OUTCOME_NONE:
        break;
    }
    return "NONE";
}

/* --------------------------------------------------------------- records --- */

#define EHCI_PUBLISH(text)                                                       \
    do {                                                                         \
        serial_write(text);                                                      \
        console_record_text(text);                                               \
    } while (0)
#define EHCI_PUBLISH_U64(value)                                                  \
    do {                                                                         \
        serial_write_u64(value);                                                 \
        console_record_u64(value);                                               \
    } while (0)
#define EHCI_PUBLISH_HEX(value)                                                  \
    do {                                                                         \
        serial_write_hex(value);                                                 \
        console_record_hex64(value);                                             \
    } while (0)

static void ehci_publish_hex2(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[3];

    text[0] = digits[(value >> 4U) & 0x0FU];
    text[1] = digits[value & 0x0FU];
    text[2] = '\0';
    EHCI_PUBLISH(text);
}

/* ITEM-569: a 64-bit address, printed as 16 hex digits (both halves) on the
   serial record.  The old records printed the low dword only, which is exactly
   how the 64-bit buffer format's defect stayed invisible. */
static void ehci_publish_hex64(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[19];

    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0U; index < 16U; ++index) {
        text[2U + index] = digits[(value >> (60U - index * 4U)) & 0x0FU];
    }
    text[18] = '\0';
    EHCI_PUBLISH(text);
}

static void ehci_publish_location(const struct ehci_controller *controller) {
    ehci_publish_hex2(controller->location.bus);
    EHCI_PUBLISH(":");
    ehci_publish_hex2(controller->location.device);
    EHCI_PUBLISH(".");
    EHCI_PUBLISH_U64(controller->location.function);
}

static void ehci_log_controller(const struct ehci_controller *controller) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    EHCI_PUBLISH("[EHCI] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" vid=");
    EHCI_PUBLISH_HEX(controller->vendor_id);
    EHCI_PUBLISH(" did=");
    EHCI_PUBLISH_HEX(controller->device_id);
    EHCI_PUBLISH(" hci=");
    EHCI_PUBLISH_U64(controller->hci_version);
    EHCI_PUBLISH(" hccparams=0x");
    EHCI_PUBLISH_HEX(controller->hccparams);
    EHCI_PUBLISH(" ac64=");
    EHCI_PUBLISH_U64(controller->ac64 ? 1U : 0U);
    EHCI_PUBLISH(" legacy=");
    EHCI_PUBLISH(controller->legacy != NULL ? controller->legacy : "UNKNOWN");
    EHCI_PUBLISH(" usbintr=0x");
    EHCI_PUBLISH_HEX(controller->usbintr);
    EHCI_PUBLISH(" usbcmd=0x");
    EHCI_PUBLISH_HEX(controller->usbcmd_after_reset);
    EHCI_PUBLISH(" usbsts=0x");
    EHCI_PUBLISH_HEX(controller->usbsts_after_reset);
    EHCI_PUBLISH(" ports=");
    EHCI_PUBLISH_U64(controller->ports);
    EHCI_PUBLISH(" connected=");
    EHCI_PUBLISH_U64(controller->ports_connected);
    EHCI_PUBLISH(" state=");
    EHCI_PUBLISH(controller->status == EHCI_STATUS_OK ? "READY" : "UNAVAILABLE");
    EHCI_PUBLISH(" status=");
    EHCI_PUBLISH(ehci_status_string(controller->status));
    EHCI_PUBLISH(" detail=");
    EHCI_PUBLISH(controller->detail != NULL ? controller->detail : "UNKNOWN");
    /* ITEM-562: which port's path this controller's walk ended at, and how many
       devices it offered the guard before that - on the controller's own line,
       so two controllers are never confused in the record. */
    EHCI_PUBLISH(" walk=");
    EHCI_PUBLISH(ehci_walk_outcome_string(controller->walk_outcome));
    if (controller->walk_stage != NULL) {
        EHCI_PUBLISH(" walk_stage=");
        EHCI_PUBLISH(controller->walk_stage);
    }
    EHCI_PUBLISH(" walk_port=");
    EHCI_PUBLISH_U64(controller->walk_port);
    if (controller->walk_hub_address != 0U) {
        EHCI_PUBLISH(" walk_hub=");
        EHCI_PUBLISH_U64(controller->walk_hub_address);
        EHCI_PUBLISH(".");
        EHCI_PUBLISH_U64(controller->walk_hub_port);
    }
    EHCI_PUBLISH(" attached=");
    EHCI_PUBLISH_U64(controller->attached);
    /* ITEM-563: what the controller said about its ports, so `connected=0` can
       be read against PPC and the number of ports it applies to. */
    EHCI_PUBLISH(" sync=");
    EHCI_PUBLISH(ehci_engine_cleanup_string(controller->last_cleanup));
    if (controller->last_doorbell != 0U) {
        EHCI_PUBLISH(" doorbell=0x");
        EHCI_PUBLISH_HEX(controller->last_doorbell);
        EHCI_PUBLISH("/0x");
        EHCI_PUBLISH_HEX(controller->last_doorbell_readback);
    }
    EHCI_PUBLISH(" stale_iaa=");
    EHCI_PUBLISH_U64(controller->last_stale_iaa ? 1U : 0U);
    EHCI_PUBLISH(" halt=");
    EHCI_PUBLISH_U64(controller->last_halted_confirmed ? 1U : 0U);
    EHCI_PUBLISH(" restart=");
    EHCI_PUBLISH_U64(controller->last_restarted ? 1U : 0U);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_U64(controller->last_restart_confirmed ? 1U : 0U);
    if (controller->last_usbcmd_after != 0U) {
        EHCI_PUBLISH(" after=0x");
        EHCI_PUBLISH_HEX(controller->last_usbcmd_after);
        EHCI_PUBLISH("/0x");
        EHCI_PUBLISH_HEX(controller->last_usbsts_after);
    }
    EHCI_PUBLISH(" hcsparams=0x");
    EHCI_PUBLISH_HEX(controller->hcsparams);
    EHCI_PUBLISH(" ppc=");
    EHCI_PUBLISH_U64(controller->ppc ? 1U : 0U);
    EHCI_PUBLISH(" probed=");
    EHCI_PUBLISH_U64(controller->port_probe_count);
    if (controller->port != 0U) {
        EHCI_PUBLISH(" port=");
        EHCI_PUBLISH_U64(controller->port);
        EHCI_PUBLISH(" portsc=");
        EHCI_PUBLISH_HEX(controller->portsc);
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

static const char *ehci_port_reset_result_string(
    enum ehci_port_reset_result result) {
    switch (result) {
    case EHCI_PORT_RESET_OK:
        return "OK";
    case EHCI_PORT_RESET_NOT_CONNECTED:
        return "NOT_CONNECTED";
    case EHCI_PORT_RESET_PR_LATCHED:
        return "PR_LATCHED";
    case EHCI_PORT_RESET_NOT_ENABLED:
        return "NOT_ENABLED";
    case EHCI_PORT_RESET_CONTROLLER_HALTED:
        return "CONTROLLER_HALTED";
    }
    return "UNKNOWN";
}

static const char *ehci_port_reset_point_string(uint32_t point) {
    switch (point) {
    case EHCI_PORT_RESET_POINT_BEFORE:
        return "BEFORE";
    case EHCI_PORT_RESET_POINT_AFTER_SET:
        return "AFTER_SET";
    case EHCI_PORT_RESET_POINT_HOLD:
        return "HOLD";
    case EHCI_PORT_RESET_POINT_AFTER_CLEAR:
        return "AFTER_CLEAR";
    case EHCI_PORT_RESET_POINT_SETTLE:
        return "SETTLE";
    case EHCI_PORT_RESET_POINT_POLL:
        return "POLL";
    case EHCI_PORT_RESET_POINT_AFTER:
        return "AFTER";
    default:
        return "NONE";
    }
}

static void ehci_log_port(const struct ehci_controller *controller,
                          uint32_t port,
                          uint32_t portsc,
                          const struct ehci_port_reset_state *state) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    EHCI_PUBLISH("[EHCI-PORT] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" port=");
    EHCI_PUBLISH_U64(port);
    EHCI_PUBLISH(" ccs=");
    EHCI_PUBLISH_U64(ehci_port_connected(portsc) ? 1U : 0U);
    EHCI_PUBLISH(" ped=");
    EHCI_PUBLISH_U64(ehci_port_enabled(portsc) ? 1U : 0U);
    EHCI_PUBLISH(" speed=");
    EHCI_PUBLISH_U64(ehci_port_speed_code(portsc));
    EHCI_PUBLISH(" companion=");
    EHCI_PUBLISH_U64(ehci_port_owned_by_companion(portsc) ? 1U : 0U);
    EHCI_PUBLISH(" portsc=");
    EHCI_PUBLISH_HEX(portsc);
    EHCI_PUBLISH(" reset=");
    EHCI_PUBLISH(state != NULL ? ehci_port_reset_result_string(state->result)
                               : "NONE");
    if (state != NULL) {
        EHCI_PUBLISH(" rb=");
        EHCI_PUBLISH_HEX(state->portsc_before);
        EHCI_PUBLISH(" rs=");
        EHCI_PUBLISH_HEX(state->portsc_set);
        EHCI_PUBLISH(" rh=");
        EHCI_PUBLISH_HEX(state->portsc_hold);
        EHCI_PUBLISH(" rc=");
        EHCI_PUBLISH_HEX(state->portsc_cleared);
        EHCI_PUBLISH(" rb2=");
        EHCI_PUBLISH_HEX(state->portsc_cleared_readback);
        EHCI_PUBLISH(" ra=");
        EHCI_PUBLISH_HEX(state->portsc_after);
        EHCI_PUBLISH(" wait_us=");
        EHCI_PUBLISH_U64(state->waited_us);
        EHCI_PUBLISH(" rec=");
        EHCI_PUBLISH_U64(state->cleared_twice ? 1U : 0U);
        EHCI_PUBLISH(" power=");
        EHCI_PUBLISH_U64(state->powered ? 1U : 0U);
        EHCI_PUBLISH(" hse=");
        EHCI_PUBLISH(ehci_port_reset_point_string(state->hse_point));
        EHCI_PUBLISH(" hcmd=0x");
        EHCI_PUBLISH_HEX(state->hse_usbcmd);
        EHCI_PUBLISH(" hsts=0x");
        EHCI_PUBLISH_HEX(state->hse_usbsts);
        EHCI_PUBLISH(" cmd=0x");
        EHCI_PUBLISH_HEX(state->usbcmd[EHCI_PORT_RESET_POINT_BEFORE]);
        EHCI_PUBLISH("/0x");
        EHCI_PUBLISH_HEX(state->usbcmd[EHCI_PORT_RESET_POINT_AFTER]);
        EHCI_PUBLISH(" sts=0x");
        EHCI_PUBLISH_HEX(state->usbsts[EHCI_PORT_RESET_POINT_BEFORE]);
        EHCI_PUBLISH("/0x");
        EHCI_PUBLISH_HEX(state->usbsts[EHCI_PORT_RESET_POINT_AFTER]);
        if (port != 0U && port <= EHCI_PORT_RECORDS &&
            controller->reset_pcists_valid[port - 1U]) {
            EHCI_PUBLISH(" pcists=0x");
            EHCI_PUBLISH_HEX(controller->reset_pcists[port - 1U]);
        }
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

static void ehci_log_storage(const struct ehci_controller *controller,
                             const char *state,
                             const char *reason,
                             const char *detail) {
    const struct ehci_device *device = &controller->device;
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    EHCI_PUBLISH("[EHCI-STORAGE] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" vid=");
    EHCI_PUBLISH_HEX(device->vendor_id);
    EHCI_PUBLISH(" pid=");
    EHCI_PUBLISH_HEX(device->product_id);
    EHCI_PUBLISH(" state=");
    EHCI_PUBLISH(state);
    EHCI_PUBLISH(" reason=");
    EHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    if (detail != NULL && detail[0] != '\0') {
        EHCI_PUBLISH(" detail=");
        EHCI_PUBLISH(detail);
    }
    if (device->hub_address != 0U) {
        EHCI_PUBLISH(" hub=");
        EHCI_PUBLISH_U64(device->hub_address);
        EHCI_PUBLISH(" hub_port=");
        EHCI_PUBLISH_U64(device->hub_port);
    }
    if (device->block_bound) {
        EHCI_PUBLISH(" blocks=");
        EHCI_PUBLISH_U64(device->usb.block_count);
        EHCI_PUBLISH(" block=");
        EHCI_PUBLISH_U64(device->usb.block_size);
        EHCI_PUBLISH(" iface=");
        EHCI_PUBLISH_U64(device->interface_number);
        EHCI_PUBLISH(" ep_in=");
        EHCI_PUBLISH_HEX(device->endpoint_in);
        EHCI_PUBLISH(" ep_out=");
        EHCI_PUBLISH_HEX(device->endpoint_out);
        EHCI_PUBLISH(" commands=");
        EHCI_PUBLISH_U64(device->usb.command_count);
        EHCI_PUBLISH(" reads=");
        EHCI_PUBLISH_U64(device->usb.read_count);
        EHCI_PUBLISH(" writes=");
        EHCI_PUBLISH_U64(device->usb.write_count);
        EHCI_PUBLISH(" errors=");
        EHCI_PUBLISH_U64(device->usb.transport_error_count);
        /* ITEM-572: the write cache the device reported and the flush policy
           that followed from it. */
        EHCI_PUBLISH(" cache=");
        EHCI_PUBLISH(device->usb.cache_known ? "KNOWN" : "UNKNOWN");
        EHCI_PUBLISH(" wce=");
        EHCI_PUBLISH(device->usb.cache_known
                         ? (device->usb.wce ? "1" : "0")
                         : "-");
        EHCI_PUBLISH(" flush=");
        EHCI_PUBLISH(
            usb_storage_flush_policy_string(device->usb.flush_policy));
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* The medium record mirrors the xHCI one: the state and reason the identity
   guard produced, plus its partition extent when it matched. */
static void ehci_log_medium(const struct ehci_controller *controller,
                            const struct install_medium_boot_identity *boot,
                            const struct install_medium_verdict *verdict,
                            const char *state,
                            const char *reason) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    EHCI_PUBLISH("[INSTALL-MEDIUM] controller=ehci pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" state=");
    EHCI_PUBLISH(state);
    EHCI_PUBLISH(" reason=");
    EHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    if (boot != NULL) {
        EHCI_PUBLISH(" media=");
        EHCI_PUBLISH_U64(boot->media_type);
        EHCI_PUBLISH(" disk_id=");
        EHCI_PUBLISH_HEX(boot->mbr_disk_id);
        EHCI_PUBLISH(" partition=");
        EHCI_PUBLISH_U64(boot->partition_index);
    }
    if (verdict != NULL && verdict->result == INSTALL_MEDIUM_MATCH) {
        EHCI_PUBLISH(" type=");
        EHCI_PUBLISH_HEX(verdict->partition_type);
        EHCI_PUBLISH(" first_lba=");
        EHCI_PUBLISH_U64(verdict->first_lba);
        EHCI_PUBLISH(" length_lba=");
        EHCI_PUBLISH_U64(verdict->length_lba);
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* ITEM-582: the diagnostic-medium attempt's own line, written whether it bound
   or not: a stick that the preparation did not stamp, a FAT volume without the
   reserved journal and a medium the guard refused are different answers. */
static void ehci_log_diagnostic(const struct ehci_controller *controller,
                                bool bound,
                                const char *result,
                                const char *reason,
                                const char *placement) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    EHCI_PUBLISH("[DIAG-MEDIUM] controller=ehci pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" state=");
    EHCI_PUBLISH(bound ? "BOUND" : "REFUSED");
    EHCI_PUBLISH(" result=");
    EHCI_PUBLISH(result != NULL ? result : "UNAVAILABLE");
    EHCI_PUBLISH(" reason=");
    EHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    if (bound && placement != NULL) {
        EHCI_PUBLISH(" placement=");
        EHCI_PUBLISH(placement);
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* ---------------------------------------------------------------- timing --- */

static void ehci_barrier(void) {
    __asm__ volatile ("mfence" : : : "memory");
}

static void ehci_pause(uint32_t count) {
    for (uint32_t spin = 0U; spin < count; ++spin) {
        __asm__ volatile ("pause");
    }
}

static uint64_t ehci_deadline(uint32_t microseconds) {
    const uint64_t per_second = timer_cycles_per_second();

    if (per_second == 0U || microseconds == 0U) {
        return 0U;
    }
    return timer_cycles() +
           (per_second / 1000000U + 1U) * (uint64_t)microseconds;
}

static bool ehci_expired(uint64_t deadline) {
    return deadline != 0U && timer_cycles() > deadline;
}

static void ehci_wait_us(uint32_t microseconds) {
    const uint64_t deadline = ehci_deadline(microseconds);

    if (deadline != 0U) {
        while (!ehci_expired(deadline)) {
            __asm__ volatile ("pause");
        }
        return;
    }
    ehci_pause(microseconds * 100U);
}

/* -------------------------------------------------------------- registers --- */

static uint32_t ehci_read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void ehci_write32(volatile uint8_t *base, uint32_t offset,
                         uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}


static uint32_t ehci_op_read(const struct ehci_controller *controller,
                             uint32_t offset) {
    /* ITEM-566: the one address computation, proved over a fake BAR. */
    return ehci_reg_read(controller->regs, controller->caplength, offset);
}

static void ehci_op_write(struct ehci_controller *controller,
                          uint32_t offset,
                          uint32_t value) {
    ehci_reg_write(controller->regs, controller->caplength, offset, value);
}

/* ITEM-563/567: the port module's MMIO and clock, absolute offsets from BAR0. */
static uint32_t ehci_port_mmio_read(void *context, uint32_t offset) {
    struct ehci_controller *controller = context;

    return ehci_read32(controller->regs, offset);
}

static void ehci_port_mmio_write(void *context, uint32_t offset,
                                 uint32_t value) {
    struct ehci_controller *controller = context;

    ehci_write32(controller->regs, offset, value);
}

static void ehci_port_mmio_delay(void *context, uint32_t microseconds) {
    (void)context;
    ehci_wait_us(microseconds);
}

/* ITEM-561: the initialization module's view of the BAR, absolute offsets. */
static uint32_t ehci_init_read(void *context, uint32_t offset) {
    struct ehci_controller *controller = context;

    return ehci_read32(controller->regs, offset);
}

static void ehci_init_write(void *context, uint32_t offset, uint32_t value) {
    struct ehci_controller *controller = context;

    ehci_write32(controller->regs, offset, value);
}

static void ehci_init_barrier(void *context) {
    (void)context;
    ehci_barrier();
}

static uint64_t ehci_init_deadline(void *context, uint32_t microseconds) {
    (void)context;
    return ehci_deadline(microseconds);
}

static bool ehci_init_expired(void *context, uint64_t deadline) {
    (void)context;
    return ehci_expired(deadline);
}

static void ehci_init_pause(void *context) {
    (void)context;
    __asm__ volatile ("pause");
}

/*
 * ITEM-560: the first moment the controller reported a Host System Error.
 *
 * The transfer snapshot of ITEM-559 said HSE was set *when the transfer
 * failed*, but not whether it had been set before the transfer head was
 * linked, at the schedule start, or during the wait - and that is exactly the
 * difference between "the transfer tripped it" and "the controller was
 * already dead".  Each start and walk step is sampled; the first sample that
 * carries HSE is kept once, with the register pair it saw, and nothing later
 * (a second sample, the cleanup, the report) overwrites it.
 */
static void ehci_sample_hse(struct ehci_controller *controller,
                            enum ehci_hse_stage stage) {
    uint32_t usbsts;

    if (controller->hse_stage != EHCI_HSE_STAGE_NONE) {
        return;
    }
    usbsts = ehci_op_read(controller, EHCI_OP_USBSTS);
    if ((usbsts & EHCI_USBSTS_HSE) == 0U) {
        return;
    }
    controller->hse_stage = stage;
    controller->hse_usbcmd = ehci_op_read(controller, EHCI_OP_USBCMD);
    controller->hse_usbsts = usbsts;
}

const char *ehci_hse_stage_string(enum ehci_hse_stage stage) {
    switch (stage) {
    case EHCI_HSE_STAGE_NONE:
        return "NONE";
    case EHCI_HSE_STAGE_AFTER_RESET:
        return "AFTER_RESET";
    case EHCI_HSE_STAGE_AFTER_CONFIGFLAG:
        return "AFTER_CONFIGFLAG";
    case EHCI_HSE_STAGE_AFTER_ASYNC_ADDRESS:
        return "AFTER_ASYNC_ADDRESS";
    case EHCI_HSE_STAGE_AFTER_SCHEDULE:
        return "AFTER_SCHEDULE";
    case EHCI_HSE_STAGE_AFTER_PORT_RESET:
        return "AFTER_PORT_RESET";
    case EHCI_HSE_STAGE_BEFORE_LINK:
        return "BEFORE_LINK";
    case EHCI_HSE_STAGE_IN_WAIT:
        return "IN_WAIT";
    }
    return "UNKNOWN";
}

/* ------------------------------------------------------------- DMA/memory --- */

static uint8_t *ehci_dma_at(const struct ehci_controller *controller,
                            uint32_t offset) {
    return controller->dma + offset;
}


static uint32_t ehci_dma_physical(const struct ehci_controller *controller,
                                  uint32_t offset) {
    return (uint32_t)(controller->dma_physical + offset);
}

/* ITEM-569: the descriptors live at a stride, not at sizeof(struct): in the
   64-bit format a qTD is 52 bytes but its slot is 64, and indexing the C array
   would step 52 bytes into the next descriptor's page. */
static struct ehci_qtd *ehci_qtd_slot(const struct ehci_controller *controller,
                                      uint32_t index) {
    return (struct ehci_qtd *)(void *)(controller->dma + EHCI_QTD_OFFSET +
                                       index * EHCI_QTD_STRIDE);
}

static uint32_t ehci_qtd_physical(const struct ehci_controller *controller,
                                  uint32_t index) {
    return ehci_dma_physical(controller,
                             EHCI_QTD_OFFSET + index * EHCI_QTD_STRIDE);
}

static uint32_t ehci_bounce_physical(const struct ehci_controller *controller,
                                     uint32_t local) {
    return (uint32_t)(controller->dma_physical + EHCI_BOUNCE_OFFSET + local);
}

/* ----------------------------------------------------------- controller --- */

static void ehci_zero(void *target, size_t length) {
    uint8_t *bytes = target;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void ehci_copy(void *target, const void *source, size_t length) {
    uint8_t *to = target;
    const uint8_t *from = source;

    for (size_t index = 0U; index < length; ++index) {
        to[index] = from[index];
    }
}

static void ehci_set_status(struct ehci_controller *controller,
                            enum ehci_status status,
                            const char *detail) {
    controller->status = status;
    controller->detail = detail != NULL ? detail : "UNKNOWN";
}

static bool ehci_map_bar(struct ehci_controller *controller,
                         const struct pci_bar *bar,
                         uint64_t hhdm_offset) {
    uint64_t length = bar->length;
    uint64_t pages;

    if (length < EHCI_MMIO_MIN_BYTES) {
        length = EHCI_MMIO_MIN_BYTES;
    }
    if (length > EHCI_MMIO_MAX_BYTES) {
        length = EHCI_MMIO_MAX_BYTES;
    }
    controller->mmio_bytes = (uint32_t)length;
    pages = (length + EHCI_PAGE_BYTES - 1U) / EHCI_PAGE_BYTES;
    for (uint64_t page = 0U; page < pages; ++page) {
        volatile void *window = NULL;

        if (vmm_map_mmio_page(bar->base + page * EHCI_PAGE_BYTES, hhdm_offset,
                              &window) != VMM_OK ||
            window == NULL) {
            ehci_set_status(controller, EHCI_STATUS_MMIO_MAP_FAILED, "MAP");
            return false;
        }
        if (page == 0U) {
            controller->regs = (volatile uint8_t *)window;
        }
    }
    return true;
}

static bool ehci_dma_acquire(struct ehci_controller *controller) {
    const uint64_t physical = pmm_alloc_contiguous_pages(EHCI_DMA_PAGES);

    if (physical == 0U ||
        physical + EHCI_DMA_BYTES > 0x100000000ULL) {
        /* EHCI addresses memory with 32 bits; a pool above 4 GiB cannot be
           described to it. */
        ehci_set_status(controller, EHCI_STATUS_DMA_UNAVAILABLE, "ABOVE_4G");
        return false;
    }
    if ((physical % EHCI_PAGE_BYTES) != 0U) {
        /* The periodic frame list must be 4 KiB aligned (EHCI 1.0 section
           2.3.5), and it lives at a page offset of the pool. */
        ehci_set_status(controller, EHCI_STATUS_DMA_UNAVAILABLE, "ALIGN");
        return false;
    }
    controller->dma_physical = physical;
    controller->dma = (uint8_t *)(uintptr_t)(physical + ehci_hhdm);
    ehci_zero(controller->dma, EHCI_DMA_BYTES);
    {
        /* Every entry terminates: a valid empty periodic schedule. */
        volatile uint32_t *frame_list = (volatile uint32_t *)ehci_dma_at(
            controller, EHCI_FRAME_LIST_OFFSET);

        for (uint32_t entry = 0U; entry < EHCI_FRAME_LIST_ENTRIES; ++entry) {
            frame_list[entry] = EHCI_FRAME_LIST_TERMINATE;
        }
    }
    controller->list_head =
        (struct ehci_qh *)ehci_dma_at(controller, EHCI_LIST_HEAD_OFFSET);
    controller->transfer =
        (struct ehci_qh *)ehci_dma_at(controller, EHCI_TRANSFER_QH_OFFSET);
    controller->qtds = (struct ehci_qtd *)ehci_dma_at(controller,
                                                      EHCI_QTD_OFFSET);
    controller->bounce = ehci_dma_at(controller,
                                     EHCI_BOUNCE_OFFSET + EHCI_SETUP_LOCAL);
    return true;
}

/*
 * ITEM-568: the BIOS -> OS ownership handoff, in PCI configuration space.
 *
 * EHCI 1.0 Table 2-7: HCCPARAMS.EECP is "the offset in PCI configuration space
 * of the first EHCI extended capability" and must be 40h or greater; section 5
 * puts USBLEGSUP at EECP+00h (BIOS/OS semaphores, Table 2-3) and USBLEGCTLSTS
 * at EECP+04h (the legacy SMI enables, Table 2-4).  The handoff this replaces
 * read BAR0 + EECP*4 and stepped offset + Next*4 - MMIO, dword-scaled - so on
 * the owner's laptop it read unrelated dwords, found no capability id 01h and
 * reported `legacy=NOT_FOUND`: the firmware was never asked to release the
 * controller, and a writer outside this driver kept programming USBCMD (the
 * run's `hse_usbcmd=0x00080010` is PSE with RS clear, a word the driver never
 * writes).
 *
 * The sequence itself is the pure module's (kernel/src/ehci_legacy.c), so the
 * host proof can drive it on a fake configuration space; this adapter is the
 * configuration access.
 */
struct ehci_legacy_config {
    struct pci_config_io *io;
    struct ehci_controller *controller;
};

static uint32_t ehci_legacy_read32(void *context, uint16_t offset) {
    struct ehci_legacy_config *config = context;
    const struct pci_location *location = &config->controller->location;
    uint32_t value = UINT32_MAX;

    if (!config->io->read32(config->io->context, location->bus,
                            location->device, location->function, offset,
                            &value)) {
        return UINT32_MAX;
    }
    return value;
}

static void ehci_legacy_write32(void *context, uint16_t offset,
                                uint32_t value) {
    struct ehci_legacy_config *config = context;
    const struct pci_location *location = &config->controller->location;

    (void)config->io->write32(config->io->context, location->bus,
                              location->device, location->function, offset,
                              value);
}

static void ehci_legacy_delay_us(void *context, uint32_t microseconds) {
    (void)context;
    ehci_wait_us(microseconds);
}

static const char *ehci_legacy_result_string(enum ehci_legacy_result result) {
    switch (result) {
    case EHCI_LEGACY_TAKEN:
        return "TAKEN";
    case EHCI_LEGACY_NOT_OWNED:
        return "NOT_OWNED";
    case EHCI_LEGACY_TIMEOUT:
        return "TIMEOUT";
    case EHCI_LEGACY_NOT_FOUND:
        return "NOT_FOUND";
    case EHCI_LEGACY_ABSENT:
        return "ABSENT";
    }
    return "UNKNOWN";
}

static void ehci_legacy_handoff(struct ehci_controller *controller,
                                struct pci_config_io *io) {
    struct ehci_legacy_config config = {io, controller};
    const struct ehci_legacy_ops ops = {
        &config, ehci_legacy_read32, ehci_legacy_write32,
        ehci_legacy_delay_us};

    (void)ehci_legacy_handoff_run(&ops, controller->hccparams,
                                  &controller->legacy_state);
    controller->legacy = ehci_legacy_result_string(
        controller->legacy_state.result);
    controller->legacy_valid = true;
}

/*
 * ITEM-569: the DMA pool's own record.  The Toshiba's refusal was a host system
 * error on the first transfer, and the question the next run must answer is
 * what the controller was pointed at: the format it chooses (HCCPARAMS.AC64),
 * the slot sizes, and every structure address and data-buffer address with
 * both halves, because the 64-bit format's buffer pointer is a 64-bit value
 * split over two dwords - printing one half was how the old record hid the
 * defect.
 */
static void ehci_log_dma(const struct ehci_controller *controller) {
    const uint64_t flags = serial_sequence_begin();
    const uint64_t pool = controller->dma_physical;

    if (controller->regs == NULL || controller->dma == NULL) {
        return;
    }
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-DMA] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" ac64=");
    EHCI_PUBLISH_U64(controller->ac64 ? 1U : 0U);
    EHCI_PUBLISH(" hccparams=0x");
    EHCI_PUBLISH_HEX(controller->hccparams);
    EHCI_PUBLISH(" pool=0x");
    ehci_publish_hex64(pool);
    EHCI_PUBLISH(" qh=0x");
    ehci_publish_hex64(pool + EHCI_LIST_HEAD_OFFSET);
    EHCI_PUBLISH("/0x");
    ehci_publish_hex64(pool + EHCI_TRANSFER_QH_OFFSET);
    EHCI_PUBLISH(" qh_bytes=");
    EHCI_PUBLISH_U64(EHCI_QH64_BYTES);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_U64(EHCI_QH_BYTES);
    EHCI_PUBLISH(" qh_stride=");
    EHCI_PUBLISH_U64(EHCI_QH_STRIDE);
    EHCI_PUBLISH(" qtd=0x");
    ehci_publish_hex64(pool + EHCI_QTD_OFFSET);
    EHCI_PUBLISH(" qtd_bytes=");
    EHCI_PUBLISH_U64(EHCI_QTD64_BYTES);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_U64(EHCI_QTD_BYTES);
    EHCI_PUBLISH(" qtd_stride=");
    EHCI_PUBLISH_U64(EHCI_QTD_STRIDE);
    EHCI_PUBLISH(" bounce=0x");
    ehci_publish_hex64(pool + EHCI_BOUNCE_OFFSET);
    EHCI_PUBLISH(" setup=0x");
    ehci_publish_hex64(pool + EHCI_BOUNCE_OFFSET + EHCI_SETUP_LOCAL);
    EHCI_PUBLISH(" data=0x");
    ehci_publish_hex64(pool + EHCI_BOUNCE_OFFSET + EHCI_DATA_LOCAL);
    EHCI_PUBLISH(" bulk=0x");
    ehci_publish_hex64(pool + EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL);
    EHCI_PUBLISH(" plb=0x");
    ehci_publish_hex64(pool + EHCI_FRAME_LIST_OFFSET);
    EHCI_PUBLISH(" dss=0x");
    EHCI_PUBLISH_HEX(controller->start.ctrldssegment == UINT32_MAX
                         ? 0U
                         : controller->start.ctrldssegment);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* The handoff's own record: the chain, the semaphores and the SMI enables. */
static void ehci_log_legacy(const struct ehci_controller *controller) {
    const struct ehci_legacy_state *state = &controller->legacy_state;
    const uint64_t flags = serial_sequence_begin();

    if (!controller->legacy_valid) {
        return;
    }
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-LEGACY] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" eecp=0x");
    EHCI_PUBLISH_HEX(state->eecp);
    EHCI_PUBLISH(" result=");
    EHCI_PUBLISH(ehci_legacy_result_string(state->result));
    EHCI_PUBLISH(" caps=");
    EHCI_PUBLISH_U64(state->cap_count);
    for (uint32_t index = 0U; index < state->cap_count; ++index) {
        EHCI_PUBLISH(" cap");
        EHCI_PUBLISH_U64(index);
        EHCI_PUBLISH("=0x");
        EHCI_PUBLISH_HEX(state->cap_offset[index]);
        EHCI_PUBLISH(":0x");
        EHCI_PUBLISH_HEX(state->caps[index]);
    }
    EHCI_PUBLISH(" sup=0x");
    EHCI_PUBLISH_HEX(state->sup_before);
    EHCI_PUBLISH("/0x");
    EHCI_PUBLISH_HEX(state->sup_written);
    EHCI_PUBLISH("/0x");
    EHCI_PUBLISH_HEX(state->sup_after);
    EHCI_PUBLISH(" ctl=0x");
    EHCI_PUBLISH_HEX(state->ctl_before);
    EHCI_PUBLISH("/0x");
    EHCI_PUBLISH_HEX(state->ctl_after);
    EHCI_PUBLISH(" waited_us=");
    EHCI_PUBLISH_U64(state->waited_us);
    EHCI_PUBLISH(" forced=");
    EHCI_PUBLISH_U64(state->forced ? 1U : 0U);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* ITEM-561: the start evidence, rendered after the text helpers below. */
static void ehci_publish_start(const struct ehci_controller *controller);


/* ITEM-561: the HSE stage an initialization refusal belongs to. */
static enum ehci_hse_stage ehci_init_cause_stage(enum ehci_init_cause cause) {
    switch (cause) {
    case EHCI_INIT_CONFIGFLAG_TIMEOUT:
        return EHCI_HSE_STAGE_AFTER_CONFIGFLAG;
    case EHCI_INIT_HCH_TIMEOUT:
    case EHCI_INIT_ASS_TIMEOUT:
    case EHCI_INIT_PERIODIC_ON:
        return EHCI_HSE_STAGE_AFTER_SCHEDULE;
    case EHCI_INIT_OK:
    case EHCI_INIT_INVALID:
    case EHCI_INIT_CAPLENGTH:
    case EHCI_INIT_PORTS:
    case EHCI_INIT_WINDOW:
    case EHCI_INIT_HALT_FAILED:
    case EHCI_INIT_RESET_TIMEOUT:
    case EHCI_INIT_PERIODIC_STOP_TIMEOUT:
        break;
    }
    return EHCI_HSE_STAGE_AFTER_RESET;
}

/*
 * Reset, claim the ports, and publish the asynchronous list the transfers ride.
 *
 * CONFIGFLAG is set to one so every root port is routed to this controller
 * rather than to a companion controller this driver does not implement: on the
 * owner's machine the USB 2 ports belong to the EHCI function, and a port left
 * owned by the companion is a port whose device the kernel can never see.
 */
static bool ehci_controller_start(struct ehci_controller *controller,
                                  struct pci_config_io *io) {
    uint32_t command = 0U;

    {
        /*
         * ITEM-560: the command word and the status word share one dword, and
         * the status word is write-one-to-clear.  The old code read the dword,
         * ORed the command bits and wrote it back: that *cleared* the PCI
         * status errors before anything could read them, so the Host System
         * Error's PCI cause (master abort, target abort, parity) was lost
         * before the first register was touched.  Now the status is kept
         * first, then cleared deliberately, then the command is enabled and
         * read back.
         */
        uint32_t config_dword = 0U;

        if (!io->read32(io->context, controller->location.bus,
                        controller->location.device,
                        controller->location.function, EHCI_PCI_COMMAND,
                        &config_dword)) {
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "PCI_READ");
            return false;
        }
        controller->pci.command_before = config_dword & 0xFFFFU;
        controller->pci.status_before = config_dword >> 16U;
        /* Write the status back to itself: the error bits are W1C, so this
           clears exactly them and keeps the command word unchanged. */
        if (!io->write32(io->context, controller->location.bus,
                         controller->location.device,
                         controller->location.function, EHCI_PCI_COMMAND,
                         config_dword)) {
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "PCI_WRITE");
            return false;
        }
        if (!io->read32(io->context, controller->location.bus,
                        controller->location.device,
                        controller->location.function, EHCI_PCI_COMMAND,
                        &config_dword)) {
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "PCI_READ");
            return false;
        }
        controller->pci.status_after_clear = config_dword >> 16U;

        command = controller->pci.command_before |
                  EHCI_PCI_COMMAND_MEMORY | EHCI_PCI_COMMAND_BUS_MASTER |
                  EHCI_PCI_COMMAND_INTX_DISABLE;
        if (!io->write32(io->context, controller->location.bus,
                         controller->location.device,
                         controller->location.function, EHCI_PCI_COMMAND,
                         command)) {
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "PCI_WRITE");
            return false;
        }
        if (!io->read32(io->context, controller->location.bus,
                        controller->location.device,
                        controller->location.function, EHCI_PCI_COMMAND,
                        &config_dword)) {
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "PCI_READ");
            return false;
        }
        controller->pci.command = config_dword & 0xFFFFU;
        controller->pci.read_back_ok =
            (controller->pci.command &
             (EHCI_PCI_COMMAND_MEMORY | EHCI_PCI_COMMAND_BUS_MASTER)) ==
            (EHCI_PCI_COMMAND_MEMORY | EHCI_PCI_COMMAND_BUS_MASTER);
    }
    if (!controller->pci.read_back_ok) {
        /* Without bus mastering the controller's DMA is a PCI master abort,
           which the controller reports as HSE. */
        ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT, "BUSMASTER");
        return false;
    }

    /*
     * ITEM-561: the register-level initialization is one testable sequence.
     * The old code wrote USBINTR/USBCMD/USBSTS *before* it had read CAPLENGTH,
     * so those accesses used offset zero and landed in the capability space -
     * the reset was never issued, the firmware's periodic schedule survived and
     * the driver started a schedule it had not configured (the Host System
     * Error of the earlier runs).  The module reads and validates CAPLENGTH
     * first, stops, resets, waits the periodic status out, programs the
     * schedules and starts with an explicit word.
     */
    {
        const struct ehci_init_ops init_ops = {
            controller, ehci_init_read, ehci_init_write, ehci_init_barrier,
            ehci_init_deadline, ehci_init_expired, ehci_init_pause};
        struct ehci_init_result init;
        uint32_t ring = (uint32_t)ehci_dma_physical(controller,
                                                    EHCI_LIST_HEAD_OFFSET);

        /*
         * ITEM-560 (kept): the pool the controller must reach, checked against
         * what the controller can address, before it is told to walk anything.
         * A pool above the controller's address width is not a transfer that
         * fails, it is a transfer the controller cannot dereference at all.
         */
        controller->hccparams = ehci_read32(controller->regs,
                                            EHCI_CAP_HCCPARAMS);
        controller->dma_base = controller->dma_physical;
        controller->dma_bytes = EHCI_DMA_BYTES;
        if (!ehci_ring_reachable(controller->dma_base, controller->dma_bytes,
                                 (controller->hccparams &
                                  EHCI_HCCPARAMS_64BIT) != 0U)) {
            ehci_set_status(controller, EHCI_STATUS_DMA_UNAVAILABLE,
                            "UNREACHABLE");
            return false;
        }
        ehci_legacy_handoff(controller, io);
        ehci_log_legacy(controller);
        /* The ring is host memory: build it before the controller is reset. */
        ehci_ring_list_head(controller->list_head, ring);
        ehci_init_run(&init_ops, controller->mmio_bytes,
                      (controller->hccparams & EHCI_HCCPARAMS_64BIT) != 0U,
                      ehci_dma_physical(controller, EHCI_FRAME_LIST_OFFSET),
                      ring, EHCI_RESET_BUDGET_US, EHCI_RESET_BUDGET_US,
                      EHCI_ASYNC_BUDGET_US, &init);
        controller->start.halted_before = init.halted_before;
        controller->start.usbcmd_before_reset = init.usbcmd_before;
        controller->start.usbsts_before_reset = init.usbsts_before;
        controller->start.usbcmd_after_hcreset = init.usbcmd_after_reset;
        controller->start.usbsts_after_hcreset = init.usbsts_after_reset;
        controller->start.pse_was_set = init.pse_was_set;
        controller->start.pss_was_set = init.pss_was_set;
        controller->start.pse_clear_word = init.pse_clear_word;
        controller->start.pse_clear_readback = init.pse_clear_readback;
        controller->start.pss_stopped = init.pss_stopped;
        controller->start.periodiclistbase = init.periodiclistbase;
        controller->start.ctrldssegment = init.ctrldssegment_written
                                              ? init.ctrldssegment
                                              : UINT32_MAX;
        controller->start.asynclistaddr = init.asynclistaddr;
        controller->start.configflag = init.configflag;
        controller->start.usbcmd_at_start = init.start_word;
        controller->start.usbcmd_readback = init.start_readback;
        controller->start.usbsts_at_start = init.usbsts_after_start;
        controller->start.periodic_off = init.periodic_off;
        controller->start.async_on = init.async_on;
        controller->start.usbintr = init.usbintr_after;
        controller->usbintr = init.usbintr_after;
        controller->start.usbcmd_before_start = init.pse_clear_word;
        controller->init_cause = ehci_init_cause_string(init.cause);
        controller->caplength = (uint8_t)init.caplength;
        controller->ports = init.ports;
        controller->hcsparams = init.hcsparams;
        controller->hccparams = init.hccparams;
        controller->ac64 =
            (init.hccparams & EHCI_HCCPARAMS_64BIT) != 0U;
        controller->ppc = (init.hcsparams & EHCI_HCSPARAMS_PPC) != 0U;
        controller->hci_version = (uint16_t)(ehci_read32(
                                                 controller->regs,
                                                 EHCI_CAP_CAPLENGTH) >>
                                             16U);
        if (init.cause != EHCI_INIT_OK) {
            /* The HSE sample keeps the stage the refusal belongs to, so ITEM-560's
               first-HSE evidence still says *where* the controller was when it
               stopped answering. */
            ehci_sample_hse(controller,
                            ehci_init_cause_stage(init.cause));
            ehci_set_status(controller, EHCI_STATUS_RESET_TIMEOUT,
                            ehci_init_cause_string(init.cause));
            /* An early refusal is exactly the case that had no evidence: the
               report is never written, so the start line is published here,
               before the walk abandons the controller. */
            ehci_publish_start(controller);
    ehci_log_dma(controller);
            return false;
        }
        ehci_sample_hse(controller, EHCI_HSE_STAGE_AFTER_SCHEDULE);
        if ((init.hccparams & EHCI_HCCPARAMS_64BIT) == 0U) {
            controller->start.ctrldssegment = UINT32_MAX;
        }
    }
    controller->async_enabled = true;
    controller->usbcmd_after_reset = controller->start.usbcmd_readback;
    controller->usbsts_after_reset = controller->start.usbsts_at_start;
    ehci_set_status(controller, EHCI_STATUS_OK, "READY");
    ehci_publish_start(controller);
    return true;
}

/* ----------------------------------------------------------- descriptors --- */

struct ehci_segment {
    uint64_t physical;
    uint32_t length;
    uint8_t pid;
    bool data_toggle;
    bool interrupt_on_complete;
};

/* ITEM-554: record the failing transfer before anything is unlinked or
   rewritten, and publish it as one bounded record.  The first hardware run that
   reached this code reported only `DEVICE8`, so the descriptor, its status, the
   controller's own registers and where the device hung were lost. */
/*
 * ITEM-558: the snapshot's renderer.  It composes one bounded line - the PCI
 * location, the root port, the transfer's result and descriptor, the device
 * path, the bytes moved and the registers - so the detail line, the report and
 * the serial record all quote the same text instead of three renderings that
 * can drift.
 */
static size_t ehci_failure_append(char *buffer,
                                  size_t size,
                                  size_t length,
                                  const char *text) {
    while (text != NULL && *text != '\0') {
        if (length + 1U >= size) {
            return length;
        }
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
    return length;
}

static size_t ehci_failure_u64(char *buffer,
                               size_t size,
                               size_t length,
                               uint64_t value) {
    char digits[21];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) {
        if (length + 1U >= size) {
            return length;
        }
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

static size_t ehci_failure_hex(char *buffer,
                               size_t size,
                               size_t length,
                               uint64_t value,
                               uint32_t digits) {
    static const char hex_digits[] = "0123456789ABCDEF";

    length = ehci_failure_append(buffer, size, length, "0x");
    for (uint32_t index = 0U; index < digits; ++index) {
        const uint32_t shift = (digits - 1U - index) * 4U;

        if (length + 1U >= size) {
            return length;
        }
        buffer[length++] = hex_digits[(value >> shift) & 0x0FU];
    }
    buffer[length] = '\0';
    return length;
}

static size_t ehci_failure_location(char *buffer,
                                    size_t size,
                                    size_t length,
                                    const struct ehci_controller *controller) {
    static const char hex_digits[] = "0123456789ABCDEF";
    char location[9];

    location[0] = hex_digits[(controller->location.bus >> 4U) & 0x0FU];
    location[1] = hex_digits[controller->location.bus & 0x0FU];
    location[2] = ':';
    location[3] = hex_digits[(controller->location.device >> 4U) & 0x0FU];
    location[4] = hex_digits[controller->location.device & 0x0FU];
    location[5] = '.';
    location[6] = hex_digits[controller->location.function & 0x0FU];
    location[7] = '\0';
    return ehci_failure_append(buffer, size, length, location);
}

static void ehci_render_failure(struct ehci_controller *controller,
                                bool is_control,
                                const struct ehci_segment *segments,
                                uint32_t token_count) {
    char *buffer = controller->failure.text;
    const size_t size = sizeof(controller->failure.text);
    size_t length = 0U;
    uint32_t received = 0U;

    if (segments != NULL) {
        const uint32_t first = is_control ? 1U : 0U;
        const uint32_t last =
            token_count > (is_control ? 1U : 0U) ? token_count - (is_control ? 1U : 0U)
                                                 : first;

        for (uint32_t index = first; index < last; ++index) {
            const uint32_t token = ehci_qtd_slot(controller, index)->token;
            uint32_t residual;
            uint32_t expected = segments[index].length;

            if ((token & EHCI_QTD_STATUS_ACTIVE) != 0U) {
                continue;
            }
            residual = (token & EHCI_QTD_TOTAL_MASK) >> EHCI_QTD_TOTAL_SHIFT;
            received += expected > residual ? expected - residual : 0U;
        }
    }
    controller->failure.received = received;
    length = ehci_failure_append(buffer, size, length, "pci=");
    length = ehci_failure_location(buffer, size, length, controller);
    length = ehci_failure_append(buffer, size, length, " port=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.root_port);
    length = ehci_failure_append(buffer, size, length, " result=");
    length = ehci_failure_append(
        buffer, size, length,
        ehci_transfer_result_string(controller->failure.result));
    length = ehci_failure_append(buffer, size, length, " cause=");
    length = ehci_failure_append(
        buffer, size, length,
        ehci_engine_cause_string(controller->failure.cause));
    length = ehci_failure_append(buffer, size, length, " quiesced=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.quiesced ? 1U : 0U);
    length = ehci_failure_append(buffer, size, length, " stalled=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.stalled ? 1U : 0U);
    /* ITEM-564: what the stop did, separately from what the transfer did. */
    length = ehci_failure_append(buffer, size, length, " cleanup=");
    length = ehci_failure_append(
        buffer, size, length,
        ehci_engine_cleanup_string(controller->failure.cleanup));
    length = ehci_failure_append(buffer, size, length, " schedule=");
    length = ehci_failure_append(buffer, size, length,
                                 controller->failure.schedule_running
                                     ? "running"
                                     : "off");
    length = ehci_failure_append(buffer, size, length, " stale_iaa=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.stale_iaa ? 1U : 0U);
    length = ehci_failure_append(buffer, size, length, " doorbell=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.doorbell, 8U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.doorbell_readback, 8U);
    /* ITEM-566: who changed the schedule.  The words before the head was
       linked, at the first refusal and after the stop, so a run can be read
       without guessing which stage saw RS/ASE/ASS/HCH change. */
    length = ehci_failure_append(buffer, size, length, " before=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbcmd_before_link, 8U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbsts_before_link, 8U);
    length = ehci_failure_append(buffer, size, length, " first=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbcmd_first_refusal, 8U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbsts_first_refusal, 8U);
    length = ehci_failure_append(buffer, size, length, " after=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbcmd_after, 8U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbsts_after, 8U);
    length = ehci_failure_append(buffer, size, length, " ass_wait_us=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.ass_waited_us);
    length = ehci_failure_append(buffer, size, length, " halt=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.halted_confirmed ? 1U : 0U);
    length = ehci_failure_append(buffer, size, length, " restart=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.restarted ? 1U : 0U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_u64(
        buffer, size, length,
        controller->failure.restart_confirmed ? 1U : 0U);
    length = ehci_failure_append(buffer, size, length, " index=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.failed_index);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.token_count);
    length = ehci_failure_append(buffer, size, length, " qtd=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.failed_token, 8U);
    length = ehci_failure_append(buffer, size, length, " addr=");
    length = ehci_failure_u64(buffer, size, length, controller->failure.address);
    length = ehci_failure_append(buffer, size, length, " ep=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.endpoint, 2U);
    length = ehci_failure_append(buffer, size, length, " hub=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.hub_address);
    length = ehci_failure_append(buffer, size, length, " hub_port=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.hub_port);
    length = ehci_failure_append(buffer, size, length, " received=");
    length = ehci_failure_u64(buffer, size, length, received);
    length = ehci_failure_append(buffer, size, length, " residual=");
    length = ehci_failure_u64(buffer, size, length,
                              controller->failure.residual);
    length = ehci_failure_append(buffer, size, length, " usbcmd=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbcmd, 8U);
    length = ehci_failure_append(buffer, size, length, " usbsts=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.usbsts, 8U);
    length = ehci_failure_append(buffer, size, length, " portsc=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->failure.portsc, 8U);
    for (uint32_t index = 0U; index < token_count; ++index) {
        length = ehci_failure_append(buffer, size, length, " q");
        length = ehci_failure_u64(buffer, size, length, index);
        length = ehci_failure_append(buffer, size, length, "=");
        length = ehci_failure_hex(buffer, size, length,
                                  controller->failure.tokens[index], 8U);
    }
    /* ITEM-560: the state the controller was started from, the PCI words that
       show whether it was a bus master at all, and the first moment HSE was
       reported. */
    length = ehci_failure_append(buffer, size, length, " pcicmd=");
    length = ehci_failure_hex(buffer, size, length, controller->pci.command,
                              4U);
    length = ehci_failure_append(buffer, size, length, " pcists=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->pci.status_before, 4U);
    length = ehci_failure_append(buffer, size, length, " start=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->start.usbcmd_at_start, 8U);
    length = ehci_failure_append(buffer, size, length, "/");
    length = ehci_failure_hex(buffer, size, length,
                              controller->start.usbsts_at_start, 8U);
    length = ehci_failure_append(buffer, size, length, " al=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->start.asynclistaddr, 8U);
    length = ehci_failure_append(buffer, size, length, " plb=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->start.periodiclistbase, 8U);
    length = ehci_failure_append(buffer, size, length, " dss=");
    if (controller->start.ctrldssegment == UINT32_MAX) {
        length = ehci_failure_append(buffer, size, length, "NONE");
    } else {
        length = ehci_failure_hex(buffer, size, length,
                                  controller->start.ctrldssegment, 8U);
    }
    length = ehci_failure_append(buffer, size, length, " cf=");
    length = ehci_failure_hex(buffer, size, length,
                              controller->start.configflag, 8U);
    length = ehci_failure_append(buffer, size, length, " dma=");
    length = ehci_failure_hex(buffer, size, length, controller->dma_base, 8U);
    length = ehci_failure_append(buffer, size, length, " dmar=");
    length = ehci_failure_append(buffer, size, length,
                                 acpi_dmar_present() ? "present" : "absent");
    length = ehci_failure_append(buffer, size, length, " hse=");
    length = ehci_failure_append(buffer, size, length,
                                 ehci_hse_stage_string(controller->hse_stage));
    if (controller->hse_stage != EHCI_HSE_STAGE_NONE) {
        length = ehci_failure_append(buffer, size, length, " hse_usbcmd=");
        length = ehci_failure_hex(buffer, size, length,
                                  controller->hse_usbcmd, 8U);
        length = ehci_failure_append(buffer, size, length, " hse_usbsts=");
        length = ehci_failure_hex(buffer, size, length,
                                  controller->hse_usbsts, 8U);
    }
}

static void ehci_capture_failure(
    struct ehci_controller *controller,
    const struct ehci_engine_result *outcome,
    uint8_t address,
    uint8_t endpoint,
    uint32_t token_count,
    const struct ehci_segment *segments,
    bool is_control) {
    uint64_t flags = serial_sequence_begin();
    const enum ehci_transfer_result result = outcome->transfer;
    const uint32_t failed_index = outcome->failed_index;

    controller->failure.valid = true;
    controller->failure.result = result;
    controller->failure.cause = outcome->cause;
    controller->failure.cleanup = outcome->cleanup;
    controller->failure.doorbell = outcome->doorbell;
    controller->failure.doorbell_readback = outcome->doorbell_readback;
    controller->failure.usbcmd_before_link = outcome->usbcmd_before_link;
    controller->failure.usbsts_before_link = outcome->usbsts_before_link;
    controller->failure.usbcmd_first_refusal = outcome->usbcmd_first_refusal;
    controller->failure.usbsts_first_refusal = outcome->usbsts_first_refusal;
    controller->failure.usbcmd_after = outcome->usbcmd_after;
    controller->failure.usbsts_after = outcome->usbsts_after;
    controller->failure.ass_waited_us = outcome->ass_waited_us;
    controller->failure.stale_iaa = outcome->stale_iaa;
    controller->failure.schedule_running = outcome->schedule_running;
    controller->failure.ass_seen = outcome->ass_seen;
    controller->failure.halted_confirmed = outcome->halted_confirmed;
    controller->failure.restarted = outcome->restarted;
    controller->failure.restart_confirmed = outcome->restart_confirmed;
    controller->failure.quiesced = outcome->quiesced;
    controller->failure.stalled = outcome->stalled;
    controller->failure.address = address;
    controller->failure.endpoint = endpoint;
    controller->failure.hub_address = controller->device.hub_address;
    controller->failure.hub_port = controller->device.hub_port;
    controller->failure.failed_index = failed_index;
    controller->failure.token_count = token_count;
    controller->failure.failed_token =
        failed_index < token_count
            ? ehci_qtd_slot(controller, failed_index)->token
            : 0U;
    controller->failure.residual =
        failed_index < token_count
            ? (controller->failure.failed_token & EHCI_QTD_TOTAL_MASK) >>
                  EHCI_QTD_TOTAL_SHIFT
            : 0U;
    controller->failure.root_port = controller->port;
    for (uint32_t index = 0U; index < token_count; ++index) {
        controller->failure.tokens[index] =
            ehci_qtd_slot(controller, index)->token;
    }
    /* ITEM-559: the registers are the engine's snapshot taken before the
       unlink, so the record names the failure state and not the stop. */
    controller->failure.usbcmd = outcome->usbcmd;
    controller->failure.usbsts = outcome->usbsts;
    controller->failure.portsc = outcome->portsc;
    ehci_render_failure(controller, is_control, segments, token_count);

    /*
     * ITEM-564: four short records instead of one long line.  The hardware run's
     * snapshot was readable only from a photograph of a wrapped screen, and the
     * wrap is exactly where the transfer's verdict and the stop's ran together.
     * Every record here fits one row, so `ehci` and a photo show it whole.
     */
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-FAIL] result=");
    EHCI_PUBLISH(ehci_transfer_result_string(result));
    EHCI_PUBLISH(" cause=");
    EHCI_PUBLISH(ehci_engine_cause_string(outcome->cause));
    EHCI_PUBLISH(" index=");
    EHCI_PUBLISH_U64(failed_index);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_U64(token_count);
    EHCI_PUBLISH(" qtd=");
    EHCI_PUBLISH_HEX(controller->failure.failed_token);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);

    flags = serial_sequence_begin();
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-FAIL-TOKENS]");
    for (uint32_t index = 0U; index < token_count; ++index) {
        EHCI_PUBLISH(" q");
        EHCI_PUBLISH_U64(index);
        EHCI_PUBLISH("=");
        EHCI_PUBLISH_HEX(controller->failure.tokens[index]);
    }
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);

    flags = serial_sequence_begin();
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-FAIL-REG] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" port=");
    EHCI_PUBLISH_U64(controller->failure.root_port);
    EHCI_PUBLISH(" usbcmd=");
    EHCI_PUBLISH_HEX(controller->failure.usbcmd);
    EHCI_PUBLISH(" usbsts=");
    EHCI_PUBLISH_HEX(controller->failure.usbsts);
    EHCI_PUBLISH(" portsc=");
    EHCI_PUBLISH_HEX(controller->failure.portsc);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);

    flags = serial_sequence_begin();
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-FAIL-SYNC] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" cleanup=");
    EHCI_PUBLISH(ehci_engine_cleanup_string(outcome->cleanup));
    EHCI_PUBLISH(" schedule=");
    EHCI_PUBLISH(outcome->schedule_running ? "running" : "off");
    EHCI_PUBLISH(" stale_iaa=");
    EHCI_PUBLISH_U64(outcome->stale_iaa ? 1U : 0U);
    EHCI_PUBLISH(" doorbell=");
    EHCI_PUBLISH_HEX(outcome->doorbell);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_HEX(outcome->doorbell_readback);
    EHCI_PUBLISH(" quiesced=");
    EHCI_PUBLISH_U64(outcome->quiesced ? 1U : 0U);
    EHCI_PUBLISH(" stalled=");
    EHCI_PUBLISH_U64(outcome->stalled ? 1U : 0U);
    EHCI_PUBLISH(" halt=");
    EHCI_PUBLISH_U64(outcome->halted_confirmed ? 1U : 0U);
    EHCI_PUBLISH(" restart=");
    EHCI_PUBLISH_U64(outcome->restarted ? 1U : 0U);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_U64(outcome->restart_confirmed ? 1U : 0U);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);

    flags = serial_sequence_begin();
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-FAIL-STATE] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" before=");
    EHCI_PUBLISH_HEX(outcome->usbcmd_before_link);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_HEX(outcome->usbsts_before_link);
    EHCI_PUBLISH(" first=");
    EHCI_PUBLISH_HEX(outcome->usbcmd_first_refusal);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_HEX(outcome->usbsts_first_refusal);
    EHCI_PUBLISH(" after=");
    EHCI_PUBLISH_HEX(outcome->usbcmd_after);
    EHCI_PUBLISH("/");
    EHCI_PUBLISH_HEX(outcome->usbsts_after);
    EHCI_PUBLISH(" ass_wait_us=");
    EHCI_PUBLISH_U64(outcome->ass_waited_us);
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* ITEM-559: the engine's view of this controller - operational registers, the
   descriptors' token dwords, the list head's link, and the driver's clock. */
static uint32_t ehci_engine_read(void *context, uint32_t offset) {
    struct ehci_controller *controller = context;

    return ehci_op_read(controller, offset);
}

static void ehci_engine_write(void *context, uint32_t offset, uint32_t value) {
    struct ehci_controller *controller = context;

    ehci_op_write(controller, offset, value);
}

static uint32_t ehci_engine_token(void *context, uint32_t index) {
    struct ehci_controller *controller = context;

    return ehci_qtd_slot(controller, index)->token;
}

static void ehci_engine_link(void *context, bool linked) {
    struct ehci_controller *controller = context;

    controller->list_head->link =
        linked ? ehci_link(ehci_dma_physical(controller,
                                             EHCI_TRANSFER_QH_OFFSET), false)
               : ehci_link(ehci_dma_physical(controller,
                                             EHCI_LIST_HEAD_OFFSET), false);
}

static void ehci_engine_barrier(void *context) {
    (void)context;
    ehci_barrier();
}

static uint64_t ehci_engine_deadline(void *context, uint32_t microseconds) {
    (void)context;
    return ehci_deadline(microseconds);
}

static bool ehci_engine_expired(void *context, uint64_t deadline) {
    (void)context;
    return ehci_expired(deadline);
}

static void ehci_engine_pause(void *context) {
    (void)context;
    __asm__ volatile ("pause");
}

/*
 * Run one transfer: queue the segments as descriptors behind the transfer
 * queue head, link it behind the list head, and poll the last descriptor's
 * Active bit until the controller clears it or the budget runs out.  The
 * transfer head's link closes the asynchronous list back to the list head, so
 * the list stays circular while the controller walks it; afterwards the list
 * head points at itself again and the asynchronous-advance doorbell guarantees
 * the controller has left the transfer head before it is rewritten.
 *
 * `last_toggle` is the data toggle the controller says it expects next; for a
 * bulk pipe that is the only place the value is written down.
 */
static enum ehci_transfer_result ehci_execute(
    struct ehci_controller *controller,
    uint8_t address,
    uint8_t endpoint,
    uint16_t max_packet,
    bool is_control,
    const struct ehci_segment *segments,
    uint32_t segment_count,
    uint32_t budget_us,
    bool *last_toggle) {

    if (segments == NULL || segment_count == 0U ||
        segment_count > EHCI_MAX_QTD_PER_TRANSFER) {
        return EHCI_TRANSFER_OTHER;
    }
    if (controller->engine_stalled) {
        /* The controller never confirmed it left the pool; nothing may rewrite
           the queue head under it. */
        return EHCI_TRANSFER_OTHER;
    }
    controller->qtds_used = segment_count;
    for (uint32_t index = 0U; index < segment_count; ++index) {
        const uint32_t token = ehci_qtd_token(
            segments[index].length, segments[index].data_toggle,
            segments[index].interrupt_on_complete, 3U, segments[index].pid,
            EHCI_QTD_STATUS_ACTIVE);
        const uint32_t next =
            index + 1U < segment_count ? ehci_qtd_physical(controller,
                                                           index + 1U)
                                       : EHCI_QH_TERMINATE;

        ehci_ring_qtd(ehci_qtd_slot(controller, index), next, token,
                      segments[index].physical, segments[index].length,
                      controller->ac64);
    }
    /* ITEM-560: the transfer head is built by the same pure code the host
       proof walks - C = 0 on the high-speed control pipe, DTC = 1, Mult = 1
       (EHCI 1.0 section 3.6.2) - and its link back to the list head is
       published with the barrier the engine takes before it links it in. */
    ehci_ring_transfer_head(
        controller->transfer,
        ehci_dma_physical(controller, EHCI_LIST_HEAD_OFFSET),
        ehci_dma_physical(controller, EHCI_QTD_OFFSET), max_packet,
        (uint8_t)endpoint, address);
    ehci_barrier();

    {
        struct ehci_engine_ops engine;
        struct ehci_engine_result outcome;

        engine.context = controller;
        engine.read = ehci_engine_read;
        engine.write = ehci_engine_write;
        engine.qtd_token = ehci_engine_token;
        engine.link_transfer_head = ehci_engine_link;
        engine.barrier = ehci_engine_barrier;
        engine.deadline = ehci_engine_deadline;
        engine.expired = ehci_engine_expired;
        engine.pause = ehci_engine_pause;
        engine.portsc_offset =
            controller->port != 0U
                ? EHCI_OP_PORTSC +
                      (controller->port - 1U) * EHCI_OP_PORTSC_STRIDE
                : 0U;

        ehci_sample_hse(controller, EHCI_HSE_STAGE_BEFORE_LINK);
        ehci_engine_run(&engine, segment_count, budget_us,
                        EHCI_IAA_BUDGET_US, controller->engine_stalled,
                        &outcome);
        if (controller->hse_stage == EHCI_HSE_STAGE_NONE &&
            outcome.cause == EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR) {
            controller->hse_stage = EHCI_HSE_STAGE_IN_WAIT;
            controller->hse_usbcmd = outcome.usbcmd;
            controller->hse_usbsts = outcome.usbsts;
        }
        controller->engine_stalled = outcome.stalled;
    controller->last_cleanup = outcome.cleanup;
    controller->last_doorbell = outcome.doorbell;
    controller->last_doorbell_readback = outcome.doorbell_readback;
    controller->last_usbcmd_after = outcome.usbcmd_after;
    controller->last_usbsts_after = outcome.usbsts_after;
    controller->last_stale_iaa = outcome.stale_iaa;
    controller->last_halted_confirmed = outcome.halted_confirmed;
    controller->last_restarted = outcome.restarted;
    controller->last_restart_confirmed = outcome.restart_confirmed;
    if (outcome.cleanup != EHCI_ENGINE_CLEANUP_ACKNOWLEDGED &&
        outcome.cleanup != EHCI_ENGINE_CLEANUP_NOT_RUNNING) {
        /*
         * ITEM-564: the stop's own evidence, on its own record, so a doorbell
         * the controller refused or ignored is visible even when the transfer
         * itself completed and the pool is safe.
         */
        console_journal_begin();
        EHCI_PUBLISH("[EHCI-SYNC] pci=");
        ehci_publish_location(controller);
        EHCI_PUBLISH(" port=");
        EHCI_PUBLISH_U64(controller->port);
        EHCI_PUBLISH(" transfer=");
        EHCI_PUBLISH(ehci_transfer_result_string(outcome.transfer));
        EHCI_PUBLISH(" cleanup=");
        EHCI_PUBLISH(ehci_engine_cleanup_string(outcome.cleanup));
        EHCI_PUBLISH(" doorbell=0x");
        EHCI_PUBLISH_HEX(outcome.doorbell);
        EHCI_PUBLISH("/0x");
        EHCI_PUBLISH_HEX(outcome.doorbell_readback);
        EHCI_PUBLISH(" stalled=");
        EHCI_PUBLISH_U64(outcome.stalled ? 1U : 0U);
        EHCI_PUBLISH("\n");
        console_journal_end();
    }
        /*
         * ITEM-564: a stop the controller never acknowledged is a failure even
         * when the transfer itself completed - the queue head may still be in
         * the controller's hands - so it is recorded, and the transfer's own
         * verdict stays where it is instead of being replaced by it.
         */
        if (outcome.transfer != EHCI_TRANSFER_OK ||
            outcome.cause != EHCI_ENGINE_CAUSE_NONE || outcome.stalled) {
            ehci_capture_failure(controller, &outcome, address, endpoint,
                                 segment_count, segments, is_control);
            /*
             * The driver's status names the transfer's own verdict: the
             * controller's state when it has one, the stop's only when the
             * transfer itself reported nothing.  `RESET_TIMEOUT` was the old
             * name for every one of these and said nothing about either.
             */
            if (outcome.cause != EHCI_ENGINE_CAUSE_NONE) {
                ehci_set_status(controller, EHCI_STATUS_TRANSFER_FAILED,
                                ehci_engine_cause_string(outcome.cause));
            } else if (outcome.transfer != EHCI_TRANSFER_OK) {
                ehci_set_status(controller, EHCI_STATUS_TRANSFER_FAILED,
                                ehci_transfer_result_string(outcome.transfer));
            } else {
                ehci_set_status(controller, EHCI_STATUS_TRANSFER_FAILED,
                                "ASYNC_ADVANCE");
            }
        }
        if (last_toggle != NULL) {
            *last_toggle =
                (controller->transfer->overlay_token & EHCI_QTD_DT) != 0U;
        }
        if (outcome.cause != EHCI_ENGINE_CAUSE_NONE) {
            return EHCI_TRANSFER_OTHER;
        }
        return outcome.transfer;
    }
}

static uint32_t ehci_qtd_residual(const struct ehci_controller *controller,
                                  uint32_t index) {
    const uint32_t token = ehci_qtd_slot(controller, index)->token;

    return (token & EHCI_QTD_TOTAL_MASK) >> EHCI_QTD_TOTAL_SHIFT;
}

/* -------------------------------------------------------------- transfers --- */

static enum ehci_transfer_result ehci_control(struct ehci_controller *controller,
                                              uint8_t address,
                                              uint16_t max_packet,
                                              uint8_t request_type,
                                              uint8_t request,
                                              uint16_t value,
                                              uint16_t index,
                                              uint16_t length,
                                              bool data_in,
                                              const void *out_data,
                                              void *in_data,
                                              uint32_t *transferred) {
    struct ehci_segment segments[EHCI_MAX_QTD_PER_TRANSFER];
    struct ehci_transfer_plan plan;
    uint8_t *setup;
    uint32_t count = 0U;
    uint32_t moved = 0U;
    enum ehci_transfer_result result;
    bool toggle = true;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    if (!ehci_plan_transfer(length, max_packet, &plan) ||
        length > EHCI_DATA_BYTES) {
        return EHCI_TRANSFER_OTHER;
    }
    setup = controller->bounce + EHCI_SETUP_LOCAL;
    (void)ehci_control_setup(setup, request_type, request, value, index,
                             length);

    segments[count].physical = ehci_bounce_physical(controller,
                                                    EHCI_SETUP_LOCAL);
    segments[count].length = EHCI_SETUP_BYTES;
    segments[count].pid = EHCI_QTD_PID_SETUP;
    segments[count].data_toggle = false;
    segments[count].interrupt_on_complete = false;
    ++count;

    if (length != 0U) {
        if (!data_in && out_data != NULL) {
            ehci_copy(controller->bounce + EHCI_DATA_LOCAL, out_data, length);
        }
        for (uint32_t chunk = 0U; chunk < plan.qtd_count; ++chunk) {
            segments[count].physical = ehci_bounce_physical(
                controller, (uint32_t)(EHCI_DATA_LOCAL +
                                       ehci_plan_offset(&plan, chunk)));
            segments[count].length = plan.length[chunk];
            segments[count].pid = data_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
            segments[count].data_toggle = toggle;
            segments[count].interrupt_on_complete = false;
            ++count;
            if ((plan.packets[chunk] & 1U) != 0U) {
                toggle = !toggle;
            }
        }
    }

    segments[count].physical = 0U;
    segments[count].length = 0U;
    segments[count].pid = data_in ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
    segments[count].data_toggle = true;
    segments[count].interrupt_on_complete = true;
    ++count;

    result = ehci_execute(controller, address, 0U, max_packet, true, segments,
                          count, EHCI_CONTROL_BUDGET_US, NULL);
    if (result != EHCI_TRANSFER_OK) {
        return result;
    }
    if (length != 0U) {
        const uint32_t residual = ehci_qtd_residual(controller, count - 2U);

        moved = plan.total_bytes > residual ? plan.total_bytes - residual : 0U;
        if (data_in && in_data != NULL && moved != 0U) {
            ehci_copy(in_data, controller->bounce + EHCI_DATA_LOCAL, moved);
        }
    }
    if (transferred != NULL) {
        *transferred = moved;
    }
    return EHCI_TRANSFER_OK;
}

static bool ehci_control_request(struct ehci_controller *controller,
                                 uint8_t address,
                                 uint16_t max_packet,
                                 uint8_t request_type,
                                 uint8_t request,
                                 uint16_t value,
                                 uint16_t index,
                                 uint16_t length,
                                 bool data_in,
                                 const void *out_data,
                                 void *in_data,
                                 uint32_t *transferred) {
    return ehci_control(controller, address, max_packet, request_type, request,
                        value, index, length, data_in, out_data, in_data,
                        transferred) == EHCI_TRANSFER_OK;
}

/* One bulk transfer of at most EHCI_BULK_BYTES at a time; longer requests are
   split here, each piece a separate transfer so a short packet ends the piece
   exactly where USB puts it. */
static bool ehci_bulk(struct ehci_controller *controller,
                      uint8_t address,
                      uint8_t endpoint,
                      uint16_t max_packet,
                      bool data_in,
                      void *buffer,
                      uint32_t length,
                      uint32_t *transferred) {
    const uint8_t *source = buffer;
    uint8_t *target = buffer;
    uint32_t remaining = length;
    uint32_t done = 0U;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    if (length == 0U) {
        return true;
    }
    while (remaining != 0U) {
        struct ehci_segment segments[EHCI_MAX_QTD_PER_TRANSFER];
        struct ehci_transfer_plan plan;
        uint32_t chunk = remaining;
        uint32_t count = 0U;
        bool toggle = data_in ? controller->device.in_toggle
                              : controller->device.out_toggle;
        bool next_toggle = toggle;
        enum ehci_transfer_result result;
        uint32_t moved = 0U;
        uint8_t *bounce = controller->bounce + EHCI_BULK_LOCAL;

        if (chunk > EHCI_BULK_BYTES) {
            chunk = EHCI_BULK_BYTES;
        }
        if (!ehci_plan_transfer(chunk, max_packet, &plan)) {
            return false;
        }
        if (!data_in) {
            ehci_copy(bounce, source, chunk);
        }
        for (uint32_t index = 0U; index < plan.qtd_count; ++index) {
            segments[count].physical = ehci_bounce_physical(
                controller, (uint32_t)(EHCI_BULK_LOCAL +
                                       ehci_plan_offset(&plan, index)));
            segments[count].length = plan.length[index];
            segments[count].pid = data_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
            segments[count].data_toggle = toggle;
            segments[count].interrupt_on_complete =
                index + 1U == plan.qtd_count;
            if ((plan.packets[index] & 1U) != 0U) {
                toggle = !toggle;
            }
            ++count;
        }
        result = ehci_execute(controller, address, endpoint, max_packet, false,
                              segments, count, EHCI_TRANSFER_BUDGET_US,
                              &next_toggle);
        if (result != EHCI_TRANSFER_OK) {
            return false;
        }
        if (data_in) {
            controller->device.in_toggle = next_toggle;
        } else {
            controller->device.out_toggle = next_toggle;
        }
        {
            const uint32_t residual = ehci_qtd_residual(controller, count - 1U);

            moved = plan.total_bytes > residual ? plan.total_bytes - residual
                                                : 0U;
        }
        if (moved != 0U && data_in) {
            ehci_copy(target, bounce, moved);
        }
        done += moved;
        if (moved != chunk) {
            break;
        }
        if (!data_in) {
            source += chunk;
        } else {
            target += chunk;
        }
        remaining -= chunk;
    }
    if (transferred != NULL) {
        *transferred = done;
    }
    return true;
}

/* --------------------------------------------------------- control plane --- */

static bool ehci_read_descriptor(struct ehci_controller *controller,
                                 uint8_t address,
                                 uint16_t max_packet,
                                 uint8_t type,
                                 uint16_t length,
                                 void *buffer,
                                 uint32_t *received) {
    return ehci_control_request(controller, address, max_packet,
                                USB_REQUEST_DIRECTION_DEVICE_TO_HOST |
                                    USB_REQUEST_TYPE_STANDARD,
                                USB_REQUEST_GET_DESCRIPTOR,
                                (uint16_t)((uint16_t)type << 8U), 0U, length,
                                true, NULL, buffer, received);
}

/* -------------------------------------------------------------- transport --- */

static bool ehci_transport_control(void *context,
                                   uint8_t request_type,
                                   uint8_t request,
                                   uint16_t value,
                                   uint16_t index,
                                   uint16_t length,
                                   bool data_in,
                                   void *data,
                                   uint32_t *transferred) {
    struct ehci_controller *controller = context;
    const uint8_t address = controller->device.address;
    const uint16_t max_packet = controller->device.ep0_max_packet;
    uint32_t moved = 0U;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    if (ehci_control(controller, address, max_packet, request_type, request,
                     value, index, length, data_in, data, data, &moved) !=
        EHCI_TRANSFER_OK) {
        return false;
    }
    /* Clearing an endpoint halt resets that endpoint's data toggle to DATA0
       (USB 2.0 section 9.4.5), and the BOT reset recovery is built on exactly
       that promise. */
    if (request == EHCI_REQUEST_CLEAR_FEATURE &&
        (request_type & EHCI_REQUEST_RECIPIENT_MASK) ==
            EHCI_REQUEST_RECIPIENT_ENDPOINT &&
        value == EHCI_REQUEST_FEATURE_ENDPOINT_HALT) {
        const uint8_t endpoint = (uint8_t)(index & 0x0FU);

        if ((index & 0x80U) != 0U || endpoint == controller->device.endpoint_in) {
            controller->device.in_toggle = false;
        }
        if ((index & 0x80U) == 0U ||
            endpoint == controller->device.endpoint_out) {
            controller->device.out_toggle = false;
        }
    }
    if (transferred != NULL) {
        *transferred = data_in ? moved : length;
    }
    return true;
}

static bool ehci_transport_bulk_in(void *context,
                                   void *buffer,
                                   uint32_t length,
                                   uint32_t *transferred) {
    struct ehci_controller *controller = context;

    return ehci_bulk(controller, controller->device.address,
                     controller->device.endpoint_in,
                     controller->device.endpoint_in_max_packet, true, buffer,
                     length, transferred);
}

static bool ehci_transport_bulk_out(void *context,
                                    const void *buffer,
                                    uint32_t length,
                                    uint32_t *transferred) {
    struct ehci_controller *controller = context;

    return ehci_bulk(controller, controller->device.address,
                     controller->device.endpoint_out,
                     controller->device.endpoint_out_max_packet, false,
                     (void *)buffer, length, transferred);
}

/* ------------------------------------------------------------- enumerate --- */

static bool ehci_port_reset(struct ehci_controller *controller,
                            uint32_t port,
                            uint32_t *portsc_out) {
    const struct ehci_port_ops ops = {
        controller, ehci_port_mmio_read, ehci_port_mmio_write,
        ehci_port_mmio_delay};
    struct ehci_port_reset_state state;

    /* ITEM-567: the sequence is the port module's, so the host proof runs the
       same code over a substituted MMIO window. */
    (void)ehci_port_reset_run(&ops, controller->caplength, port,
                              controller->ppc, &state);
    controller->port = port;
    controller->portsc = state.portsc_after != 0U ? state.portsc_after
                                                  : state.portsc_before;
    if (port != 0U && port <= EHCI_PORT_RECORDS) {
        controller->reset_probes[port - 1U] = state;
        controller->reset_pcists_valid[port - 1U] = false;
        if (state.hse_point != EHCI_PORT_RESET_POINT_NONE) {
            /* Read, never written: the PCI status error bits are W1C, and this
               is the first evidence of what the host-side access was. */
            controller->reset_pcists[port - 1U] =
                pci_io_read32(controller->location.bus,
                              controller->location.device,
                              controller->location.function,
                              EHCI_PCI_COMMAND) >> 16U;
            controller->reset_pcists_valid[port - 1U] = true;
        }
        if (controller->reset_probe_count < port) {
            controller->reset_probe_count = port;
        }
    }
    ehci_log_port(controller, port,
                  state.portsc_after != 0U ? state.portsc_after
                                           : state.portsc_before,
                  &state);
    ehci_sample_hse(controller, EHCI_HSE_STAGE_AFTER_PORT_RESET);
    switch (state.result) {
    case EHCI_PORT_RESET_OK:
        break;
    case EHCI_PORT_RESET_NOT_CONNECTED:
        ehci_set_status(controller, EHCI_STATUS_PORT_NOT_ENABLED, "PE");
        return false;
    case EHCI_PORT_RESET_PR_LATCHED:
        ehci_set_status(controller, EHCI_STATUS_PORT_RESET_TIMEOUT, "PR");
        return false;
    case EHCI_PORT_RESET_NOT_ENABLED:
        ehci_set_status(controller, EHCI_STATUS_PORT_NOT_ENABLED, "PE");
        return false;
    case EHCI_PORT_RESET_CONTROLLER_HALTED:
        /* The port is not the fault: the controller stopped.  Naming HSE (or a
           halt without it) is the difference between a port that will not come
           up and a controller that cannot drive one. */
        ehci_set_status(controller, EHCI_STATUS_HOST_SYSTEM_ERROR,
                        (state.usbsts[state.hse_point !=
                                           EHCI_PORT_RESET_POINT_NONE
                                       ? state.hse_point
                                       : 0U] &
                         EHCI_USBSTS_HSE) != 0U
                            ? "HSE"
                            : "HCH");
        return false;
    }
    *portsc_out = state.portsc_after;
    /*
     * PORTSC.PS is the speed the controller measured, and a real controller
     * reports 2 (high) for the device this driver binds.  It is not a
     * prerequisite the driver may refuse on: the port has already enabled the
     * device, and a controller model may leave the field at zero (QEMU's EHCI
     * does not implement it at all) while still running the high-speed
     * handshake.  A low-speed device on this port is the one combination that
     * cannot be real, and it is refused by name.
     */
    if (ehci_port_speed_code(state.portsc_after) == 1U) {
        ehci_set_status(controller, EHCI_STATUS_UNSUPPORTED_SPEED, "LOW");
        return false;
    }
    if (ehci_port_owned_by_companion(state.portsc_after)) {
        ehci_set_status(controller, EHCI_STATUS_UNSUPPORTED_SPEED, "COMPANION");
        return false;
    }
    return true;
}

/*
 * Take the next free USB address on this controller.  Address 0 is reserved for
 * the device a port reset has just enabled; the root device becomes 1 and a
 * device behind a hub takes the next value, so the hub and the stick behind it
 * are two distinct addresses on the same controller.
 */
/*
 * ITEM-582: the candidate's own configuration evidence.
 *
 * The installed machine's refusal was `DESCRIPTOR_FAILED/CONFIG_TOTAL` with no
 * record of the candidate, the request or the nine bytes the device answered,
 * so the failure could not be told apart from a foreign device the driver does
 * not support (a camera, a hub, a phone).  Every candidate keeps its record and
 * every record is published, so a refusal names the bytes it judged.
 */
static struct ehci_config_evidence *ehci_config_begin(
    struct ehci_controller *controller) {
    struct ehci_config_evidence *record;

    if (controller->config_count >= EHCI_CONFIG_RECORDS) {
        return NULL;
    }
    record = &controller->config_records[controller->config_count++];
    ehci_zero(record, sizeof(*record));
    record->valid = true;
    record->root_port = controller->device.root_port;
    record->hub_address = controller->device.hub_address;
    record->hub_port = controller->device.hub_port;
    record->address = controller->device.address;
    record->vendor_id = controller->device.vendor_id;
    record->product_id = controller->device.product_id;
    record->setup[0] = (uint8_t)(USB_REQUEST_DIRECTION_DEVICE_TO_HOST |
                                 USB_REQUEST_TYPE_STANDARD);
    record->setup[1] = (uint8_t)USB_REQUEST_GET_DESCRIPTOR;
    record->setup[2] = (uint8_t)(USB_DESCRIPTOR_CONFIGURATION & 0xFFU);
    record->setup[3] = (uint8_t)((USB_DESCRIPTOR_CONFIGURATION >> 8U) & 0xFFU);
    record->setup[4] = 0U;
    record->setup[5] = 0U;
    record->setup[6] = (uint8_t)(USB_CONFIG_DESCRIPTOR_PREFIX & 0xFFU);
    record->setup[7] =
        (uint8_t)((USB_CONFIG_DESCRIPTOR_PREFIX >> 8U) & 0xFFU);
    record->requested = USB_CONFIG_DESCRIPTOR_PREFIX;
    return record;
}

static void ehci_config_stage(struct ehci_config_evidence *record,
                              const char *stage,
                              const char *detail) {
    if (record == NULL) {
        return;
    }
    record->stage = stage;
    record->detail = detail;
}

/* The class field (offset 5) of the first interface descriptor in a
   configuration blob, or 0 when the blob declares none. */
static uint8_t ehci_config_interface_class(const uint8_t *descriptor,
                                           uint32_t length) {
    uint32_t offset = 0U;

    if (descriptor == NULL) {
        return 0U;
    }
    while (offset + 2U <= length) {
        const uint8_t size = descriptor[offset];
        const uint8_t type = descriptor[offset + 1U];

        if (size < 2U || offset + size > length) {
            return 0U;
        }
        if (type == USB_DESCRIPTOR_INTERFACE && size >= 6U) {
            return descriptor[offset + 5U];
        }
        offset += size;
    }
    return 0U;
}

/* One line per candidate: the port it hung on, its own identity, the request
   the driver sent, the nine bytes that came back, the length the device
   computed and the stage that judged it. */
static void ehci_config_publish(const struct ehci_controller *controller,
                                const struct ehci_config_evidence *record) {
    const uint64_t flags = serial_sequence_begin();

    if (record == NULL || !record->valid) {
        return;
    }
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-CONFIG] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(" root=");
    EHCI_PUBLISH_U64(record->root_port);
    if (record->hub_address != 0U) {
        EHCI_PUBLISH(" hub=");
        EHCI_PUBLISH_U64(record->hub_address);
        EHCI_PUBLISH(" hub_port=");
        EHCI_PUBLISH_U64(record->hub_port);
    }
    EHCI_PUBLISH(" addr=");
    EHCI_PUBLISH_U64(record->address);
    EHCI_PUBLISH(" vid=");
    EHCI_PUBLISH_HEX(record->vendor_id);
    EHCI_PUBLISH(" pid=");
    EHCI_PUBLISH_HEX(record->product_id);
    EHCI_PUBLISH(" stage=");
    EHCI_PUBLISH(record->stage != NULL ? record->stage : "UNKNOWN");
    if (record->detail != NULL && record->detail[0] != '\0') {
        EHCI_PUBLISH(" detail=");
        EHCI_PUBLISH(record->detail);
    }
    EHCI_PUBLISH(" setup=");
    for (uint32_t index = 0U; index < sizeof(record->setup); ++index) {
        if (index != 0U) {
            EHCI_PUBLISH(" ");
        }
        ehci_publish_hex2(record->setup[index]);
    }
    EHCI_PUBLISH(" received=");
    EHCI_PUBLISH_U64(record->received);
    EHCI_PUBLISH(" requested=");
    EHCI_PUBLISH_U64(record->requested);
    EHCI_PUBLISH(" total=");
    EHCI_PUBLISH_U64(record->total);
    EHCI_PUBLISH(" raw=");
    for (uint32_t index = 0U; index < EHCI_CONFIG_RAW_BYTES; ++index) {
        if (index != 0U) {
            EHCI_PUBLISH(" ");
        }
        ehci_publish_hex2(record->raw[index]);
    }
    if (record->interface_class != 0U) {
        EHCI_PUBLISH(" iface=");
        EHCI_PUBLISH_HEX(record->interface_class);
    }
    EHCI_PUBLISH(" bound=");
    EHCI_PUBLISH(record->block_bound ? "1" : "0");
    EHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/* Read the configuration, select it and bind the BOT storage core.  This is the
   tail every enumerated device goes through, whether it hangs on a root port or
   behind the hub. */
static bool ehci_attach_storage(struct ehci_controller *controller) {
    uint8_t descriptor[EHCI_CONFIG_DESCRIPTOR_MAX];
    struct ehci_device *device = &controller->device;
    struct ehci_config_evidence *evidence = ehci_config_begin(controller);
    uint32_t received = 0U;
    uint16_t config_total;
    struct usb_storage_config config;
    struct usb_storage_transport transport;
    enum usb_storage_status status;

    ehci_zero(descriptor, sizeof(descriptor));
    if (!ehci_read_descriptor(controller, device->address,
                              device->ep0_max_packet,
                              USB_DESCRIPTOR_CONFIGURATION,
                              USB_CONFIG_DESCRIPTOR_PREFIX, descriptor,
                              &received) ||
        received < USB_CONFIG_DESCRIPTOR_PREFIX) {
        if (evidence != NULL) {
            evidence->received = received;
            for (uint32_t index = 0U; index < EHCI_CONFIG_RAW_BYTES; ++index) {
                evidence->raw[index] = descriptor[index];
            }
        }
        ehci_config_stage(evidence, "CONFIG9", NULL);
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_DESCRIPTOR_FAILED, "CONFIG9");
        return false;
    }
    if (evidence != NULL) {
        evidence->received = received;
        for (uint32_t index = 0U; index < EHCI_CONFIG_RAW_BYTES; ++index) {
            evidence->raw[index] = descriptor[index];
        }
        evidence->total = usb_hid_config_total_length(
            descriptor, USB_CONFIG_DESCRIPTOR_PREFIX);
    }
    config_total = usb_hid_config_total_length(descriptor,
                                               USB_CONFIG_DESCRIPTOR_PREFIX);
    if (config_total <= USB_CONFIG_DESCRIPTOR_PREFIX ||
        config_total > EHCI_CONFIG_DESCRIPTOR_MAX) {
        /*
         * The nine bytes are a descriptor the driver cannot walk: a device
         * that answered something else (a foreign class whose interface blob
         * starts with a class-specific descriptor) is not the same failure as
         * a stick whose wTotalLength is zero or past the driver's buffer.
         */
        if (descriptor[1] != USB_DESCRIPTOR_CONFIGURATION) {
            ehci_config_stage(evidence, "CONFIG9", "NOT_CONFIGURATION");
        } else {
            ehci_config_stage(evidence, "CONFIG_TOTAL", "OUT_OF_RANGE");
        }
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_DESCRIPTOR_FAILED,
                        "CONFIG_TOTAL");
        return false;
    }
    ehci_zero(descriptor, sizeof(descriptor));
    if (evidence != NULL) {
        evidence->requested = config_total;
    }
    if (!ehci_read_descriptor(controller, device->address,
                              device->ep0_max_packet,
                              USB_DESCRIPTOR_CONFIGURATION, config_total,
                              descriptor, &received) ||
        received < config_total) {
        if (evidence != NULL) {
            evidence->received = received;
        }
        ehci_config_stage(evidence, "CONFIG", NULL);
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_DESCRIPTOR_FAILED, "CONFIG");
        return false;
    }
    if (evidence != NULL) {
        evidence->received = received;
        evidence->interface_class =
            ehci_config_interface_class(descriptor, received);
    }
    status = usb_storage_parse_config(descriptor, received, &config);
    if (status != USB_STORAGE_OK) {
        ehci_config_stage(evidence, "PARSE", usb_storage_status_string(status));
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_CONFIG_PARSE_FAILED,
                        usb_storage_status_string(status));
        return false;
    }
    if (!ehci_control_request(controller, device->address,
                              device->ep0_max_packet,
                              USB_REQUEST_TYPE_STANDARD,
                              USB_REQUEST_SET_CONFIGURATION,
                              config.configuration_value, 0U, 0U, false, NULL,
                              NULL, NULL)) {
        ehci_config_stage(evidence, "SET_CONFIGURATION", NULL);
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_SET_CONFIGURATION_FAILED,
                        "SET_CONFIGURATION");
        return false;
    }
    device->config_value = config.configuration_value;
    device->interface_number = config.interface_number;
    device->endpoint_in = config.bulk_in_endpoint;
    device->endpoint_out = config.bulk_out_endpoint;
    device->endpoint_in_max_packet = config.bulk_in_max_packet;
    device->endpoint_out_max_packet = config.bulk_out_max_packet;
    device->in_toggle = false;
    device->out_toggle = false;

    ehci_zero(&transport, sizeof(transport));
    transport.context = controller;
    transport.control = ehci_transport_control;
    transport.bulk_in = ehci_transport_bulk_in;
    transport.bulk_out = ehci_transport_bulk_out;
    status = usb_storage_bind(&device->usb, &transport, &config);
    if (status != USB_STORAGE_OK) {
        ehci_config_stage(evidence, "STORAGE",
                          usb_storage_status_string(status));
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_STORAGE_REFUSED,
                        usb_storage_status_string(status));
        ehci_log_storage(controller, "REFUSED",
                         usb_storage_status_string(status), NULL);
        return false;
    }
    if (usb_storage_block_device_bind(&device->usb, &device->block) !=
        BLOCK_DEVICE_OK) {
        ehci_config_stage(evidence, "BLOCK_DEVICE", NULL);
        ehci_config_publish(controller, evidence);
        ehci_set_status(controller, EHCI_STATUS_BLOCK_DEVICE_FAILED,
                        "BLOCK_DEVICE");
        ehci_log_storage(controller, "REFUSED", "BLOCK_DEVICE", NULL);
        return false;
    }
    device->block_bound = true;
    device->bound = true;
    if (device->root_port != 0U && device->root_port <= 32U) {
        controller->bound_ports_mask |= (uint32_t)1U << (device->root_port - 1U);
    }
    if (evidence != NULL) {
        evidence->block_bound = true;
    }
    ehci_config_stage(evidence, "OK", NULL);
    ehci_config_publish(controller, evidence);
    return true;
}

/* --------------------------------------------------------------- hub path --- */

static bool ehci_hub_request(struct ehci_controller *controller,
                             uint8_t hub_address,
                             uint8_t request_type,
                             uint8_t request,
                             uint16_t value,
                             uint16_t index,
                             bool data_in,
                             void *data,
                             uint16_t length) {
    uint32_t moved = 0U;

    return ehci_control(controller, hub_address,
                        controller->device.ep0_max_packet, request_type,
                        request, value, index, length, data_in, data, data,
                        &moved) == EHCI_TRANSFER_OK;
}

static bool ehci_hub_port_status(struct ehci_controller *controller,
                                 uint8_t hub_address,
                                 uint8_t port,
                                 uint16_t *status,
                                 uint16_t *change) {
    uint8_t bytes[4];

    ehci_zero(bytes, sizeof(bytes));
    if (!ehci_hub_request(controller, hub_address,
                          EHCI_HUB_REQUEST_TYPE_IN,
                          EHCI_HUB_REQUEST_GET_STATUS, 0U, port, true, bytes,
                          sizeof(bytes))) {
        return false;
    }
    if (status != NULL) {
        *status = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
    }
    if (change != NULL) {
        *change = (uint16_t)((uint16_t)bytes[2] | ((uint16_t)bytes[3] << 8U));
    }
    return true;
}

static bool ehci_hub_feature(struct ehci_controller *controller,
                             uint8_t hub_address,
                             uint8_t port,
                             uint8_t request,
                             uint16_t feature) {
    return ehci_hub_request(controller, hub_address,
                            EHCI_HUB_REQUEST_TYPE_OUT, request, feature, port,
                            false, NULL, 0U);
}

/* Power one downstream port, wait for the device to answer, reset it and wait
   for the hub to finish.  Returns the port status the hub settled on; only a
   high-speed device can be used without a transaction translator. */
static bool ehci_hub_bring_up(struct ehci_controller *controller,
                              uint8_t hub_address,
                              uint8_t port,
                              uint8_t power_good_ms,
                              bool *empty,
                              uint16_t *status_out,
                              uint16_t *change_out,
                              const char **verdict_out) {
    uint16_t status = 0U;
    uint16_t change = 0U;

    /* ITEM-562: a port nothing is plugged into is the normal case on the
       machine's rate matching hub, not a refusal the walk should name. */
    if (empty != NULL) {
        *empty = false;
    }
    /* ITEM-570: every exit leaves the hub's own words behind, so the walk's
       record can be checked against what the hub said. */
    if (status_out != NULL) {
        *status_out = 0U;
    }
    if (change_out != NULL) {
        *change_out = 0U;
    }
    if (verdict_out != NULL) {
        *verdict_out = NULL;
    }

    if (!ehci_hub_feature(controller, hub_address, port,
                          EHCI_HUB_REQUEST_SET_FEATURE,
                          EHCI_HUB_FEATURE_PORT_POWER)) {
        control:
        if (status_out != NULL) {
            *status_out = status;
        }
        if (change_out != NULL) {
            *change_out = change;
        }
        return false;
    }
    if (power_good_ms != 0U) {
        ehci_wait_us((uint32_t)power_good_ms * 2U * 1000U);
    }
    {
        const uint64_t deadline = ehci_deadline(EHCI_HUB_POWER_BUDGET_US);
        uint32_t spins = 0U;

        for (;;) {
            if (!ehci_hub_port_status(controller, hub_address, port, &status,
                                      &change)) {
                goto control;
            }
            if ((status & 0x0001U) != 0U) {
                break;
            }
            if (deadline != 0U ? ehci_expired(deadline) :
                                 ++spins > EHCI_SPIN_WAIT) {
                /* The device connection bit never appeared: nothing is on the
                   port.  It is reported as empty, not as a refusal. */
                if (empty != NULL) {
                    *empty = true;
                }
                if (status_out != NULL) {
                    *status_out = status;
                }
                if (change_out != NULL) {
                    *change_out = change;
                }
                return false;
            }
            __asm__ volatile ("pause");
        }
    }
    if (!ehci_hub_feature(controller, hub_address, port,
                          EHCI_HUB_REQUEST_SET_FEATURE,
                          EHCI_HUB_FEATURE_PORT_RESET)) {
        goto control;
    }
    {
        const uint64_t deadline = ehci_deadline(EHCI_HUB_RESET_BUDGET_US);
        uint32_t spins = 0U;

        for (;;) {
            if (!ehci_hub_port_status(controller, hub_address, port, &status,
                                      &change)) {
                goto control;
            }
            if (ehci_hub_reset_complete(change)) {
                break;
            }
            if (deadline != 0U ? ehci_expired(deadline) :
                                 ++spins > EHCI_SPIN_WAIT) {
                ehci_set_status(controller, EHCI_STATUS_PORT_RESET_TIMEOUT,
                                "HUB_RESET");
                if (status_out != NULL) {
                    *status_out = status;
                }
                if (change_out != NULL) {
                    *change_out = change;
                }
                return false;
            }
            __asm__ volatile ("pause");
        }
    }
    (void)ehci_hub_feature(controller, hub_address, port,
                           EHCI_HUB_REQUEST_CLEAR_FEATURE,
                           EHCI_HUB_FEATURE_C_PORT_RESET);
    {
        const enum ehci_hub_port_verdict verdict = ehci_hub_port_verdict(status);

        if (status_out != NULL) {
            *status_out = status;
        }
        if (change_out != NULL) {
            *change_out = change;
        }
        if (verdict_out != NULL) {
            *verdict_out = ehci_hub_port_verdict_string(verdict);
        }
        if (verdict == EHCI_HUB_PORT_NOT_ENABLED ||
            verdict == EHCI_HUB_PORT_EMPTY) {
            ehci_set_status(controller, EHCI_STATUS_PORT_NOT_ENABLED,
                            ehci_hub_port_verdict_string(verdict));
            if (verdict == EHCI_HUB_PORT_EMPTY && empty != NULL) {
                *empty = true;
            }
            return false;
        }
        /* A high-speed hub with a high-speed device needs no transaction
           translator; anything slower would need split transactions, which this
           driver does not implement.  ITEM-570: the bits this reads are the
           specification's (bit 9 low, bit 10 high), not the shifted pair that
           refused the stick's own port as low speed. */
        if (verdict != EHCI_HUB_PORT_HIGH_SPEED) {
            ehci_set_status(controller, EHCI_STATUS_UNSUPPORTED_SPEED,
                            ehci_hub_port_verdict_string(verdict));
            return false;
        }
    }
    return true;
}

/*
 * Configure the hub and walk its downstream ports.
 *
 * The Intel 7 Series USB 2.0 ports hang behind an Integrated Rate Matching Hub
 * (datasheet 326776-003 section 5.19), and the machine's Linux capture shows
 * it on `Bus 001` with the card reader and the camera behind it.  A device on
 * one of its ports is reached with its own USB address, so the rest of the
 * driver - descriptors, configuration, the BOT bind - is the code the root
 * path already uses.
 */
/* The boot-medium guard, defined with the probe below: the walk's verify step. */
static bool ehci_medium_probe(struct ehci_controller *controller);

/* ------------------------------------------------------------ walk ops --- */

static bool ehci_walk_control(void *context,
                              uint8_t address,
                              uint16_t max_packet,
                              uint8_t request_type,
                              uint8_t request,
                              uint16_t value,
                              uint16_t index,
                              uint16_t length,
                              bool data_in,
                              const void *out_data,
                              void *in_data,
                              uint32_t *moved) {
    struct ehci_controller *controller = context;
    uint32_t received = 0U;
    const enum ehci_transfer_result result =
        ehci_control(controller, address, max_packet, request_type, request,
                     value, index, length, data_in, out_data, in_data,
                     &received);

    if (moved != NULL) {
        *moved = received;
    }
    return result == EHCI_TRANSFER_OK;
}

/* ITEM-557: the walk's recovery intervals, on the driver's own clock. */
static void ehci_walk_delay(void *context, uint32_t microseconds) {
    (void)context;
    ehci_wait_us(microseconds);
}

/* ITEM-558: the transport's text for the transfer that just failed. */
static const char *ehci_walk_last_failure(void *context) {
    struct ehci_controller *controller = context;

    if (!controller->failure.valid || controller->failure.text[0] == '\0') {
        return NULL;
    }
    return controller->failure.text;
}

/* ITEM-563: the port record, rendered after the text helpers below. */
static void ehci_log_portsc(const struct ehci_controller *controller,
                            uint32_t port,
                            const struct ehci_port_state *state);

/*
 * Measure every root port once, before the walk asks anything: power it when
 * the controller says software owns power, play out the connect window, and
 * publish the answer for the console.  The walk then consumes these settled
 * values, so a port is never measured twice and an empty one is empty by the
 * same bounded measurement the record carries.
 */
static void ehci_measure_ports(struct ehci_controller *controller) {
    const struct ehci_port_ops ops = {
        controller, ehci_port_mmio_read, ehci_port_mmio_write,
        ehci_port_mmio_delay};

    controller->port_probe_count = 0U;
    for (uint32_t port = 1U;
         port <= controller->ports && port <= EHCI_PORT_RECORDS; ++port) {
        struct ehci_port_state state;

        if (!ehci_port_probe(&ops, controller->caplength, port,
                             controller->ppc, &state)) {
            continue;
        }
        controller->port_probes[port - 1U] = state;
        controller->port_probe_count = port;
        ehci_log_portsc(controller, port, &state);
    }
}

static bool ehci_walk_root_read(void *context, uint32_t port,
                                uint32_t *portsc) {
    struct ehci_controller *controller = context;

    if (portsc == NULL || port == 0U || port > EHCI_PORT_RECORDS) {
        return false;
    }
    if (port > controller->port_probe_count) {
        return false;
    }
    *portsc = controller->port_probes[port - 1U].portsc;
    return true;
}

static bool ehci_walk_root_reset(void *context, uint32_t port,
                                 uint32_t *portsc) {
    return ehci_port_reset((struct ehci_controller *)context, port, portsc);
}

static enum ehci_walk_port_state ehci_walk_hub_bring_up(
    void *context,
    uint8_t hub_address,
    uint16_t hub_max_packet,
    uint8_t port,
    uint8_t power_good_ms,
    uint16_t *status,
    uint16_t *change,
    const char **verdict) {
    struct ehci_controller *controller = context;
    bool empty = false;

    /* The hub's class requests go through ehci_hub_request(), which reads the
       control-pipe size from the driver's device state.  The walk knows the
       size it read from the hub descriptor; put it there before the first
       request, instead of leaving the previous device's size (or zero). */
    controller->device.ep0_max_packet = hub_max_packet;
    if (ehci_hub_bring_up(controller, hub_address, port, power_good_ms, &empty,
                          status, change, verdict)) {
        return EHCI_WALK_PORT_UP;
    }
    return empty ? EHCI_WALK_PORT_EMPTY : EHCI_WALK_PORT_REFUSED;
}

static bool ehci_walk_device_begin(void *context, uint8_t address,
                                   uint16_t max_packet, uint32_t root_port,
                                   uint8_t hub_address, uint8_t hub_port,
                                   uint16_t vendor_id, uint16_t product_id) {
    struct ehci_controller *controller = context;

    /* ITEM-582: the journal holds a pointer into this structure.  Once the
       diagnostic medium is bound, no later candidate may zero or replace it. */
    if (controller->pinned_diagnostic) {
        return false;
    }
    ehci_zero(&controller->device, sizeof(controller->device));
    controller->device.address = address;
    controller->device.ep0_max_packet = max_packet;
    controller->device.root_port = root_port;
    controller->device.hub_address = hub_address;
    controller->device.hub_port = hub_port;
    controller->device.vendor_id = vendor_id;
    controller->device.product_id = product_id;
    if (root_port != 0U && root_port <= 32U) {
        controller->known_ports_mask |= (uint32_t)1U << (root_port - 1U);
    }
    if (hub_address != 0U) {
        controller->hub_seen = true;
        controller->hub_address = hub_address;
    }
    return true;
}

static bool ehci_walk_attach(void *context) {
    return ehci_attach_storage((struct ehci_controller *)context);
}

/* A device the guard rejects does not end the walk; the next device - on the
   next port of the same hub, or on the next root port - still gets its turn. */
static bool ehci_walk_verify(void *context) {
    struct ehci_controller *controller = context;

    if (ehci_medium_probe(controller)) {
        return true;
    }
    /* ITEM-582: a diagnostic medium is not the boot medium, but the journal is
       bound to this device; clearing its bound flags here would invalidate the
       write path the export is going to use. */
    if (controller->pinned_diagnostic) {
        return true;
    }
    ehci_log_storage(controller, "BOUND", "NOT_MEDIUM", NULL);
    controller->device.bound = false;
    controller->device.block_bound = false;
    return false;
}

/* --------------------------------------------------------------- medium --- */

/* The same read-only probe the xHCI path runs: LBA 0, the loader's own
   identity, and the journal placement only when the guard says MATCH. */
static bool ehci_medium_probe(struct ehci_controller *controller) {
    struct ehci_device *device = &controller->device;
    struct install_medium_boot_identity boot;
    struct install_medium_verdict verdict;
    static uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES];

    if (!device->block_bound) {
        device->medium_probed = true;
        device->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        device->medium_reason = "NO_BLOCK_DEVICE";
        ehci_log_medium(controller, NULL, NULL, "UNAVAILABLE",
                        "NO_BLOCK_DEVICE");
        return false;
    }
    if (!install_medium_boot_identity(&boot)) {
        device->medium_probed = true;
        device->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        device->medium_reason = "NO_LOADER_IDENTITY";
        ehci_log_medium(controller, NULL, NULL, "UNAVAILABLE",
                        "NO_LOADER_IDENTITY");
        return false;
    }
    if (block_device_read(&device->block.device, 0U, sector,
                          sizeof(sector)) != BLOCK_DEVICE_OK) {
        device->medium_probed = true;
        device->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        device->medium_reason = "READ";
        ehci_log_medium(controller, &boot, NULL, "UNAVAILABLE", "READ");
        return false;
    }
    verdict = install_medium_identify(&boot, sector,
                                      device->block.device.block_count);
    device->medium_probed = true;
    device->medium_result = verdict.result;
    device->medium_reason = verdict.reason;
    ehci_log_medium(controller, &boot, &verdict,
                    install_medium_result_string(verdict.result),
                    verdict.reason);
    if (verdict.result == INSTALL_MEDIUM_MATCH) {
        device->medium_matched = true;
        if (!install_journal_platform_bind_medium(&device->block.device,
                                                  &verdict)) {
            (void)install_journal_platform_medium_unbound();
        }
        return true;
    }
    /*
     * ITEM-582: the installed machine boots from its own disk, so the boot
     * guard can never match the diagnostic stick and the installer guard must
     * not be relaxed to make it.  The stick is bound only by its own prepared
     * identity and the reserved journal the preparation wrote; the result is
     * kept per device and published, so `diag save` can say why it still has
     * no medium.
     */
    device->diagnostic_probed = true;
    device->diagnostic_bound =
        install_journal_platform_bind_diagnostic(&device->block.device,
                                                 &verdict);
    if (device->diagnostic_bound) {
        /*
         * ITEM-582: the journal's backend holds `&device->block.device` for
         * the rest of the boot.  Pin the device and its port so neither the
         * next walk candidate nor a later rescan can replace or reset it.
         */
        controller->pinned_diagnostic = true;
        device->bound = true;
        device->block_bound = true;
        if (device->root_port != 0U && device->root_port <= 32U) {
            controller->bound_ports_mask |=
                (uint32_t)1U << (device->root_port - 1U);
        }
    }
    {
        bool attempted = false;
        const char *result = NULL;
        const char *reason = NULL;
        const char *placement = NULL;

        install_journal_platform_diagnostic_state(&attempted, &result, &reason,
                                                  &placement);
        device->diagnostic_result = result != NULL ? result : "UNAVAILABLE";
        device->diagnostic_reason = reason != NULL ? reason : "UNKNOWN";
        ehci_log_diagnostic(controller, device->diagnostic_bound, result,
                            reason, placement);
    }
    return false;
}

/* ------------------------------------------------------------- scan/init --- */

static int ehci_current(void) {
    return ehci_active;
}

static struct ehci_controller *ehci_controller_at(int index) {
    if (index < 0 || (uint32_t)index >= ehci_scanned) {
        return NULL;
    }
    return &ehci_controllers[index];
}

/*
 * ITEM-582: a rescan skips the device the machine is using, but only at the
 * granularity that occupies it.  A root-port device owns its root port; a
 * device behind a hub owns only its downstream port, and the root port must
 * still be walked so the hub's other ports are searched.
 */
static bool ehci_walk_port_skip(void *context, uint32_t port) {
    const struct ehci_controller *controller = context;
    const struct ehci_device *device = &controller->device;

    if (port == 0U || port > 32U || !device->bound) {
        return false;
    }
    return device->hub_address == 0U && device->root_port == port;
}

static bool ehci_walk_hub_port_skip(void *context, uint32_t root_port,
                                    uint8_t hub_address, uint8_t hub_port) {
    const struct ehci_controller *controller = context;
    const struct ehci_device *device = &controller->device;

    if (!device->bound) {
        return false;
    }
    return device->root_port == root_port &&
           device->hub_address == hub_address &&
           device->hub_port == hub_port;
}

/*
 * ITEM-582: look for the diagnostic stick again after boot.
 *
 * `ehci_storage_medium_publish()` re-probed only the device it had already
 * attached, so a stick plugged in after the system started was never seen: the
 * walk ran once, at boot.  This runs the same walk again over the same ports -
 * the driver's own enumeration, not a second one - while skipping every port a
 * bound device already occupies.  The boot walk's verdict and port records are
 * left as they were; the rescan keeps its own.
 */
static void ehci_rescan_ports(struct ehci_controller *controller) {
    struct ehci_walk_ops ops = {0};
    struct ehci_walk_report report;

    /*
     * ITEM-582: the boot walk's measurements were cached in `port_probes`, and
     * the walk's root-port read returns them; a stick plugged in after boot
     * would keep `CCS=0` forever and never be enumerated.  Keep the boot
     * snapshot, then measure every root port again from the controller before
     * the walk, so the walk reads the wire as it is now.
     */
    if (!controller->boot_probes_saved) {
        controller->boot_port_probe_count = controller->port_probe_count;
        for (uint32_t index = 0U; index < EHCI_PORT_RECORDS; ++index) {
            controller->boot_port_probes[index] =
                controller->port_probes[index];
        }
        controller->boot_probes_saved = true;
    }
    ehci_measure_ports(controller);

    ehci_zero(&report, sizeof(report));
    ops.context = controller;
    ops.control = ehci_walk_control;
    ops.root_port_read = ehci_walk_root_read;
    ops.root_port_reset = ehci_walk_root_reset;
    ops.hub_port_bring_up = ehci_walk_hub_bring_up;
    ops.device_begin = ehci_walk_device_begin;
    ops.attach_storage = ehci_walk_attach;
    ops.verify_medium = ehci_walk_verify;
    ops.port_skip = ehci_walk_port_skip;
    ops.hub_port_skip = ehci_walk_hub_port_skip;
    ops.delay_us = ehci_walk_delay;
    ops.last_failure = ehci_walk_last_failure;
    (void)ehci_walk_run(&ops, controller->ports, &report);
    controller->rescan_done = true;
    controller->rescan_outcome = report.failure;
    controller->rescan_connected = report.ports_connected;
    controller->rescan_attached = report.attached;
    controller->rescan_port_count = report.port_count;
    for (uint32_t index = 0U; index < report.port_count &&
                               index < EHCI_WALK_PORT_RECORDS; ++index) {
        controller->rescan_ports[index] = report.ports[index];
    }
    {
        const uint64_t flags = serial_sequence_begin();

        console_journal_begin();
        EHCI_PUBLISH("[EHCI-RESCAN] pci=");
        ehci_publish_location(controller);
        EHCI_PUBLISH(" attached=");
        EHCI_PUBLISH_U64(report.attached);
        EHCI_PUBLISH(" connected=");
        EHCI_PUBLISH_U64(report.ports_connected);
        EHCI_PUBLISH(" outcome=");
        EHCI_PUBLISH(ehci_walk_outcome_string(report.outcome));
        if (report.failure != NULL) {
            EHCI_PUBLISH(" stage=");
            EHCI_PUBLISH(report.failure);
        }
        EHCI_PUBLISH("\n");
        serial_sequence_end(flags);
        console_journal_end();
    }
}

static void ehci_scan_ports(struct ehci_controller *controller) {
    struct ehci_walk_ops ops = {0};
    struct ehci_walk_report report;

    ops.context = controller;
    ops.control = ehci_walk_control;
    ops.root_port_read = ehci_walk_root_read;
    ops.root_port_reset = ehci_walk_root_reset;
    ops.hub_port_bring_up = ehci_walk_hub_bring_up;
    ops.device_begin = ehci_walk_device_begin;
    ops.attach_storage = ehci_walk_attach;
    ops.verify_medium = ehci_walk_verify;
    ops.delay_us = ehci_walk_delay;
    ops.last_failure = ehci_walk_last_failure;
    /* Every root port is measured, connected or not, before the walk judges. */
    ehci_measure_ports(controller);
    {
        const bool bound = ehci_walk_run(&ops, controller->ports, &report);

        /* ITEM-562: the walk's own verdict is kept on this controller, and the
           controller's status is only filled where it has none.  The old code
           called ehci_set_status(DESCRIPTOR_FAILED, report.failure) here for
           every refusal with a named stage - including the walk's fallback
           string - so a port that failed at its reset (`PORT_NOT_ENABLED`) was
           reported as a descriptor failure, and the machine's only answer was
           `DESCRIPTOR_FAILED/NO_DEVICE`. */
        controller->ports_connected = report.ports_connected;
        controller->attached = report.attached;
        controller->walk_outcome = report.outcome;
        controller->walk_stage = report.failure;
        controller->walk_port = report.failure_port;
        controller->walk_hub_address = report.failure_hub_address;
        controller->walk_hub_port = report.failure_hub_port;
        controller->walk_port_count = report.port_count;
        for (uint32_t port_index = 0U;
             port_index < report.port_count &&
             port_index < EHCI_WALK_PORT_RECORDS;
             ++port_index) {
            controller->walk_ports[port_index] = report.ports[port_index];
        }
        if (bound) {
            /* ITEM-571: the selected device is the answer.  A first candidate
               that refused (NOT_READY, STORAGE_REFUSED) left the controller's
               status set, and this branch used to return with it still in
               place - the machine's whole answer was the first refusal even
               though the walk had found and bound the medium.  Its own walk
               record keeps that refusal; the controller's status is the
               selected device's. */
            controller->selected_hub_address = report.selected_hub_address;
            controller->selected_hub_port = report.selected_hub_port;
            controller->device.medium_matched = true;
            ehci_set_status(controller, EHCI_STATUS_OK, "BOUND");
            ehci_active = (int)(controller - ehci_controllers);
            return;
        }
        if (report.outcome == EHCI_WALK_OUTCOME_MEDIUM && report.attached != 0U) {
            /* A device attached and the guard refused it: not a fault, and
               not a reason to claim a transfer failed. */
            ehci_set_status(controller, EHCI_STATUS_MEDIUM_NOT_MATCHED,
                            "NOT_THE_BOOT_MEDIUM");
            return;
        }
        if (controller->status == EHCI_STATUS_OK ||
            controller->status == EHCI_STATUS_UNKNOWN) {
            const enum ehci_status mapped =
                ehci_walk_outcome_status(report.outcome);

            if (mapped != EHCI_STATUS_UNKNOWN) {
                ehci_set_status(controller, mapped,
                                report.failure != NULL ? report.failure
                                                       : "NO_DEVICE");
            }
        }
        /* Otherwise the driver already named the step (the port reset, the hub
           port, the attach path); that status and its detail stand. */
    }
}

/* ITEM-562: publish the machine's answer from every controller's own entry. */
static enum ehci_status ehci_aggregate(uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];

        ehci_verdict_fill(&ehci_entries[index], controller->location.bus,
                          controller->location.device,
                          controller->location.function,
                          controller->regs != NULL, controller->status,
                          controller->detail, controller->ports, NULL, false);
        ehci_entries[index].connected = controller->ports_connected;
        ehci_entries[index].attached = controller->attached;
        ehci_entries[index].outcome = controller->walk_outcome;
        ehci_entries[index].failure = controller->walk_stage;
        ehci_entries[index].bound =
            controller->device.bound && controller->device.medium_matched;
    }
    ehci_verdict_choose(ehci_entries, count, &ehci_choice);
    (void)ehci_verdict_render(ehci_entries, count, ehci_entry_text,
                              sizeof(ehci_entry_text));
    ehci_entries_ready = true;
    return ehci_choice.status;
}

enum ehci_status ehci_storage_init(uint64_t hhdm_offset) {
    struct pci_config_io *io;
    struct pci_device_header headers[EHCI_MAX_CONTROLLERS];
    size_t count = 0U;
    enum pci_status pci_status;

    ehci_zero(ehci_controllers, sizeof(ehci_controllers));
    ehci_scanned = 0U;
    ehci_active = -1;
    ehci_entries_ready = false;
    ehci_entry_text[0] = '\0';
    ehci_choice.index = -1;
    ehci_choice.status = EHCI_STATUS_CONTROLLER_ABSENT;
    ehci_choice.detail = "NOT_PROBED";
    ehci_choice.storage_bound = false;
    ehci_choice.controllers = 0U;
    ehci_hhdm = hhdm_offset;

    io = pci_io_kernel();
    if (io == NULL || io->read32 == NULL || io->write32 == NULL) {
        ehci_choice.status = EHCI_STATUS_PCI_SCAN_FAILED;
        ehci_choice.detail = "NO_IO";
        return ehci_choice.status;
    }
    /* BAR0 is the register window this driver needs; another BAR of the
       function may be an I/O or a second window that does not decode, and that
       must not hide the one the driver uses (the lesson ITEM-543 wrote down for
       the L875's AHCI controller). */
    pci_status = pci_scan_class_required(
        io, EHCI_PCI_CLASS_SERIAL_BUS, EHCI_PCI_SUBCLASS_USB,
        EHCI_PCI_PROGIF_EHCI, PCI_BAR_REQUIRED(0U), headers,
        EHCI_MAX_CONTROLLERS, &count, NULL);
    if (pci_status == PCI_CAPACITY) {
        count = EHCI_MAX_CONTROLLERS;
    } else if (pci_status != PCI_OK) {
        count = 0U;
    }
    ehci_scanned = (uint32_t)count;
    if (count == 0U) {
        ehci_choice.status = EHCI_STATUS_CONTROLLER_ABSENT;
        ehci_choice.detail = "NO_EHCI_FUNCTION";
        return ehci_choice.status;
    }
    for (size_t index = 0U; index < count; ++index) {
        struct ehci_controller *controller = &ehci_controllers[index];
        const struct pci_device_header *header = &headers[index];

        controller->present = true;
        controller->location = header->location;
        controller->vendor_id = header->vendor_id;
        controller->device_id = header->device_id;
        controller->status = EHCI_STATUS_UNKNOWN;
        controller->detail = "NOT_PROBED";
        ehci_set_status(controller, EHCI_STATUS_UNKNOWN, "NOT_PROBED");
        if (!header->bars[0].present || header->bars[0].io_space ||
            header->bars[0].base == 0U) {
            ehci_set_status(controller, EHCI_STATUS_NO_MEMORY_BAR, "BAR0");
            ehci_log_controller(controller);
            continue;
        }
        controller->bar_physical = (uint32_t)header->bars[0].base;
        controller->bar_length = header->bars[0].length;
        if (!ehci_map_bar(controller, &header->bars[0], hhdm_offset)) {
            controller->regs = NULL;
            ehci_log_controller(controller);
            continue;
        }
        if (!ehci_dma_acquire(controller)) {
            controller->regs = NULL;
            ehci_log_controller(controller);
            continue;
        }
        if (!ehci_controller_start(controller, io)) {
            ehci_log_controller(controller);
            continue;
        }
        ehci_scan_ports(controller);
        if (controller->device.bound && controller->device.medium_matched) {
            /* The storage record is published once, after the probe: its
               counters then include the medium's own sector read, which is
               what a reader compares against the boot medium verdict. */
            ehci_log_storage(controller, "BOUND", "OK", NULL);
            ehci_log_controller(controller);
            (void)ehci_aggregate((uint32_t)count);
            return EHCI_STATUS_OK;
        }
        ehci_log_controller(controller);
    }
    /* Every controller keeps its own verdict; the machine's answer is chosen
       from the entries by information, ties going to the first in scan order. */
    (void)ehci_aggregate((uint32_t)count);
    return ehci_choice.status;
}

const char *ehci_controllers_text(void) {
    return ehci_entries_ready ? ehci_entry_text : "";
}

/* The console command's sink: the report's own lines, printed as they are. */
static void ehci_console_sink(void *context, const char *text) {
    (void)context;
    if (text == NULL) {
        return;
    }
    console_write(text);
    console_putc('\n');
}

void ehci_diagnostics_print(void) {
    if (ehci_scanned == 0U) {
        console_write("EHCI: no controller was scanned on this machine\n");
        return;
    }
    ehci_diagnostics_report(ehci_console_sink, NULL);
    if (ehci_entries_ready && ehci_entry_text[0] != '\0') {
        console_write("[EHCI-VERDICT] ");
        console_write(ehci_entry_text);
        console_putc('\n');
    }
}

enum ehci_status ehci_status(void) {
    const int index = ehci_current();
    const struct ehci_controller *controller = ehci_controller_at(index);

    return controller != NULL ? controller->status : ehci_choice.status;
}

const char *ehci_failure_text(void) {
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];

        if (controller->failure.valid && controller->failure.text[0] != '\0') {
            return controller->failure.text;
        }
    }
    return NULL;
}

const char *ehci_failure_primary(void) {
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];

        if (!controller->failure.valid) {
            continue;
        }
        /* ITEM-559: a controller that halted or lost the bus explains the
           transfer better than the descriptor classifier's OTHER, and the
           operator's short line must say so. */
        if (controller->failure.cause != EHCI_ENGINE_CAUSE_NONE) {
            return ehci_engine_cause_string(controller->failure.cause);
        }
        if (controller->failure.result != EHCI_TRANSFER_OK) {
            return ehci_transfer_result_string(controller->failure.result);
        }
        /* Only the stop failed: the descriptor did complete, and the name the
           operator needs is the one for a queue head the controller never
           released. */
        return "ASYNC_ADVANCE";
    }
    return NULL;
}

const char *ehci_failure_result(void) {
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];

        if (controller->failure.valid) {
            return ehci_transfer_result_string(controller->failure.result);
        }
    }
    return NULL;
}

const char *ehci_detail(void) {
    const int index = ehci_current();
    const struct ehci_controller *controller = ehci_controller_at(index);

    return controller != NULL ? controller->detail : ehci_choice.detail;
}

bool ehci_storage_ready(void) {
    const struct ehci_controller *controller =
        ehci_controller_at(ehci_current());

    return controller != NULL && controller->device.bound;
}

struct block_device *ehci_storage_block_device(void) {
    struct ehci_controller *controller =
        ehci_controller_at(ehci_current());

    if (controller == NULL || !controller->device.block_bound) {
        return NULL;
    }
    return &controller->device.block.device;
}

bool ehci_storage_fault_text(char *buffer, size_t size) {
    const struct ehci_controller *controller = NULL;

    if (ehci_active >= 0 && (uint32_t)ehci_active < ehci_scanned) {
        controller = &ehci_controllers[ehci_active];
    } else {
        for (uint32_t index = 0U; index < ehci_scanned; ++index) {
            if (ehci_controllers[index].device.usb.fault.valid) {
                controller = &ehci_controllers[index];
                break;
            }
        }
    }
    if (controller == NULL) {
        return false;
    }
    return usb_storage_fault_text(
        usb_storage_last_fault(&controller->device.usb), buffer, size);
}

bool ehci_storage_medium_publish(void) {
    bool ready = install_journal_platform_ready();

    /*
     * ITEM-582: an installed machine boots from its SATA disk, so no USB
     * controller is ever made "active" by a boot-medium selection.  The search
     * must therefore walk every controller the boot scan found, not
     * `ehci_current()`: the diagnostic stick may hang on any of them.
     */
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        struct ehci_controller *controller = &ehci_controllers[index];

        if (!controller->present) {
            continue;
        }
        if (!ready && !controller->pinned_diagnostic) {
            /* The device the boot walk attached: probe it again (it may have
               been the diagnostic stick that was not ready at boot). */
            (void)ehci_medium_probe(controller);
            ready = install_journal_platform_ready();
        }
        if (!ready) {
            /* Nothing bound: the operator may have plugged the stick in after
               boot.  The rescan skips every port a bound device occupies. */
            ehci_rescan_ports(controller);
            ready = install_journal_platform_ready();
        }
        if (ready) {
            break;
        }
    }
    return ready;
}

/* ITEM-582: the answer is chosen from every controller's own device, because
   the installed machine has no active boot USB controller. */
static const struct ehci_controller *ehci_storage_controller(void) {
    const struct ehci_controller *probed = NULL;

    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];

        if (!controller->present) {
            continue;
        }
        if (controller->device.diagnostic_bound) {
            return controller;
        }
        if (controller->device.medium_probed && probed == NULL) {
            probed = controller;
        }
    }
    return probed;
}

const char *ehci_storage_medium_result(void) {
    const struct ehci_controller *controller = ehci_storage_controller();

    if (controller == NULL || !controller->device.medium_probed) {
        return "UNCHECKED";
    }
    if (controller->device.diagnostic_bound) {
        return "DIAGNOSTIC";
    }
    return install_medium_result_string(controller->device.medium_result);
}

/* ------------------------------------------------------------- report --- */

/* A bounded line builder for the report: no serial, no console, no journal -
   the export owns the destination. */
struct ehci_text {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void ehci_text_begin(struct ehci_text *text, char *buffer, size_t size) {
    text->buffer = buffer;
    text->size = size;
    text->length = 0U;
    text->overflow = false;
    if (size != 0U) {
        buffer[0] = '\0';
    }
}

static void ehci_text_write(struct ehci_text *text, const char *value) {
    while (value != NULL && *value != '\0') {
        if (text->length + 1U >= text->size) {
            text->overflow = true;
            return;
        }
        text->buffer[text->length++] = *value++;
        text->buffer[text->length] = '\0';
    }
}

static void ehci_text_u64(struct ehci_text *text, uint64_t value) {
    char digits[21];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        char one[2];

        one[0] = digits[--count];
        one[1] = '\0';
        ehci_text_write(text, one);
    }
}

static void ehci_text_hex(struct ehci_text *text, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char rendered[17];
    uint32_t shift = 60U;

    for (uint32_t index = 0U; index < 16U; ++index) {
        rendered[index] = digits[(value >> shift) & 0x0FU];
        shift -= 4U;
    }
    rendered[16] = '\0';
    ehci_text_write(text, "0x");
    ehci_text_write(text, rendered);
}

/* The exported record prints a register the way the serial line does: eight
   hex digits, so a reader can compare the two without reformatting. */
static void ehci_text_hex32(struct ehci_text *text, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char rendered[9];
    uint32_t shift = 28U;

    for (uint32_t index = 0U; index < 8U; ++index) {
        rendered[index] = digits[(value >> shift) & 0x0FU];
        shift -= 4U;
    }
    rendered[8] = '\0';
    ehci_text_write(text, "0x");
    ehci_text_write(text, rendered);
}

static void ehci_text_location(struct ehci_text *text,
                               const struct ehci_controller *controller) {
    static const char digits[] = "0123456789ABCDEF";
    char location[9];

    location[0] = digits[(controller->location.bus >> 4U) & 0x0FU];
    location[1] = digits[controller->location.bus & 0x0FU];
    location[2] = ':';
    location[3] = digits[(controller->location.device >> 4U) & 0x0FU];
    location[4] = digits[controller->location.device & 0x0FU];
    location[5] = '.';
    location[6] = digits[controller->location.function & 0x0FU];
    location[7] = '\0';
    ehci_text_write(text, location);
}

/* One `[EHCI-PORTSC]` record per root port, whatever it read. */
static void ehci_log_portsc(const struct ehci_controller *controller,
                            uint32_t port,
                            const struct ehci_port_state *state) {
    const uint64_t flags = serial_sequence_begin();
    char line[192];
    struct ehci_text text;

    ehci_text_begin(&text, line, sizeof(line));
    ehci_text_write(&text, "[EHCI-PORTSC] ");
    ehci_text_write(&text, "port=");
    ehci_text_u64(&text, port);
    ehci_text_write(&text, " first=");
    ehci_text_hex32(&text, state->raw_first);
    ehci_text_write(&text, " portsc=");
    ehci_text_hex32(&text, state->portsc);
    ehci_text_write(&text, " ccs=");
    ehci_text_u64(&text, ehci_port_connected(state->portsc) ? 1U : 0U);
    ehci_text_write(&text, " ccs_first=");
    ehci_text_u64(&text, ehci_port_connected(state->raw_first) ? 1U : 0U);
    ehci_text_write(&text, " ppc=");
    ehci_text_u64(&text, controller->ppc ? 1U : 0U);
    ehci_text_write(&text, " pp=");
    ehci_text_u64(&text, (state->portsc & EHCI_PORTSC_PP) != 0U ? 1U : 0U);
    ehci_text_write(&text, " powered=");
    ehci_text_u64(&text, state->powered ? 1U : 0U);
    ehci_text_write(&text, " waited_us=");
    ehci_text_u64(&text, state->waited_us);
    ehci_text_write(&text, " settle=");
    ehci_text_write(&text, state->connected
                               ? "CONNECTED"
                               : (state->settle_full ? "EMPTY" : "PARTIAL"));
    ehci_text_write(&text, "\n");
    if (text.overflow) {
        return;
    }
    console_journal_begin();
    EHCI_PUBLISH("[EHCI-PORTSC] pci=");
    ehci_publish_location(controller);
    EHCI_PUBLISH(line + (sizeof("[EHCI-PORTSC]") - 1U));
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * The walk's root port source.  It is the production probe, not a raw read:
 * the port is powered when the controller says software owns power, the
 * connect window is played out, and both reads are kept for the record - so
 * "empty" means "empty after a bounded wait", and the next hardware run can
 * show why it was believed.
 */
/*
 * ITEM-561: one bounded rendering of the start evidence.
 *
 * The fields are the ones the earlier runs could not answer: whether the
 * function was a bus master and what its status word said before the first
 * register was written (`pcicmd`, `pcists`, `pcists_clr`), whether the firmware
 * had handed the controller over (`legacy`), the words read at the stop and the
 * reset and what the reset came back with (`stop`, `halted`, `hcr`), the
 * periodic schedule the firmware had left running and the word, read-back and
 * settled status of the attempt to stop it (`pse`, `pss`, `clr`, `clr_rb`,
 * `pss_stop`), the addresses that were programmed (`plb`, `dss`, `al`), the
 * start word the controller was given and the one it read back (`start`), the
 * status that came with it (`sts`), whether the schedules were off and on
 * (`periodic`, `async`), the first moment the controller reported HSE (`hse`,
 * with the words that carried it) and the initialization module's own verdict
 * (`cause`).
 *
 * One rendering, three readers: the start path publishes it as `[EHCI-START]`
 * whether the start succeeded or refused, the report carries the same line, and
 * the refusal record keeps it as `ehci_start=` so `diag` prints it on a machine
 * whose report medium was never reached.
 */
static void ehci_start_render(const struct ehci_controller *controller,
                              char *buffer,
                              size_t size) {
    struct ehci_text text;

    ehci_text_begin(&text, buffer, size);
    ehci_text_write(&text, "pci=");
    ehci_text_location(&text, controller);
    ehci_text_write(&text, " cap=");
    ehci_text_hex32(&text, controller->caplength);
    ehci_text_write(&text, " ports=");
    ehci_text_u64(&text, controller->ports);
    ehci_text_write(&text, " legacy=");
    ehci_text_write(&text, controller->legacy != NULL ? controller->legacy
                                                      : "UNKNOWN");
    ehci_text_write(&text, " pcicmd=");
    ehci_text_hex32(&text, controller->pci.command);
    ehci_text_write(&text, " pcists=");
    ehci_text_hex32(&text, controller->pci.status_before);
    ehci_text_write(&text, " pcists_clr=");
    ehci_text_hex32(&text, controller->pci.status_after_clear);
    ehci_text_write(&text, " stop=");
    ehci_text_hex32(&text, controller->start.usbcmd_before_reset);
    ehci_text_write(&text, "/");
    ehci_text_hex32(&text, controller->start.usbsts_before_reset);
    ehci_text_write(&text, controller->start.halted_before ? " halted=1"
                                                           : " halted=0");
    ehci_text_write(&text, " hcr=");
    ehci_text_hex32(&text, controller->start.usbcmd_after_hcreset);
    ehci_text_write(&text, "/");
    ehci_text_hex32(&text, controller->start.usbsts_after_hcreset);
    ehci_text_write(&text, controller->start.pse_was_set ? " pse=1"
                                                         : " pse=0");
    ehci_text_write(&text, controller->start.pss_was_set ? " pss=1"
                                                         : " pss=0");
    ehci_text_write(&text, " clr=");
    ehci_text_hex32(&text, controller->start.pse_clear_word);
    ehci_text_write(&text, " clr_rb=");
    ehci_text_hex32(&text, controller->start.pse_clear_readback);
    ehci_text_write(&text, controller->start.pss_stopped ? " pss_stop=1"
                                                         : " pss_stop=0");
    ehci_text_write(&text, " cf=");
    ehci_text_hex32(&text, controller->start.configflag);
    ehci_text_write(&text, " plb=");
    ehci_text_hex32(&text, controller->start.periodiclistbase);
    ehci_text_write(&text, " dss=");
    if (controller->start.ctrldssegment == UINT32_MAX) {
        ehci_text_write(&text, "NONE");
    } else {
        ehci_text_hex32(&text, controller->start.ctrldssegment);
    }
    ehci_text_write(&text, " al=");
    ehci_text_hex32(&text, controller->start.asynclistaddr);
    ehci_text_write(&text, " dma=");
    ehci_text_hex32(&text, (uint32_t)controller->dma_base);
    ehci_text_write(&text, " dmar=");
    ehci_text_write(&text, acpi_dmar_present() ? "present" : "absent");
    ehci_text_write(&text, " usbintr=");
    ehci_text_hex32(&text, controller->start.usbintr);
    ehci_text_write(&text, " start=");
    ehci_text_hex32(&text, controller->start.usbcmd_at_start);
    ehci_text_write(&text, "/");
    ehci_text_hex32(&text, controller->start.usbcmd_readback);
    ehci_text_write(&text, " sts=");
    ehci_text_hex32(&text, controller->start.usbsts_at_start);
    ehci_text_write(&text, controller->start.periodic_off ? " periodic=off"
                                                          : " periodic=on");
    ehci_text_write(&text, controller->start.async_on ? " async=on"
                                                      : " async=off");
    ehci_text_write(&text, " hse=");
    ehci_text_write(&text, ehci_hse_stage_string(controller->hse_stage));
    if (controller->hse_stage != EHCI_HSE_STAGE_NONE) {
        ehci_text_write(&text, " hse_usbcmd=");
        ehci_text_hex32(&text, controller->hse_usbcmd);
        ehci_text_write(&text, " hse_usbsts=");
        ehci_text_hex32(&text, controller->hse_usbsts);
    }
    ehci_text_write(&text, " cause=");
    ehci_text_write(&text, controller->init_cause != NULL
                               ? controller->init_cause
                               : "UNKNOWN");
    ehci_text_write(&text, "\n");
}

/*
 * ITEM-562: one start rendering per controller, not one for the boot.  The
 * hardware runs showed `pci=00:1A.0` in one run and `pci=00:1D.0` in the next,
 * because each start overwrote the previous snapshot - and a reader took the
 * last controller's picture for the machine's.
 */
static char ehci_start_text[EHCI_MAX_CONTROLLERS][EHCI_START_TEXT_MAX];
static bool ehci_start_text_valid[EHCI_MAX_CONTROLLERS];

/* The chosen controller's rendering, which is what the refusal record quotes. */
const char *ehci_start_evidence(void) {
    const int index = ehci_current();

    if (index >= 0 && (uint32_t)index < EHCI_MAX_CONTROLLERS &&
        ehci_start_text_valid[index]) {
        return ehci_start_text[index];
    }
    return NULL;
}

/* Every started controller's rendering, in scan order, separated by ` | `. */
size_t ehci_start_evidence_all(char *buffer, size_t size) {
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const char *text;

        if (!ehci_start_text_valid[index]) {
            continue;
        }
        text = ehci_start_text[index];
        for (size_t step = 0U; text[step] != '\0'; ++step) {
            if (length + 4U >= size) {
                return length;
            }
            if (length != 0U && step == 0U) {
                buffer[length++] = ' ';
                buffer[length++] = '|';
                buffer[length++] = ' ';
            }
            buffer[length++] = text[step];
        }
    }
    if (length != 0U) {
        buffer[length] = '\0';
    }
    return length;
}

/*
 * Publish the start evidence as one console record at the moment the controller
 * is started - on both outcomes, because the early refusal is the case whose
 * report never reaches a medium.
 */
static void ehci_publish_start(const struct ehci_controller *controller) {
    char line[EHCI_START_TEXT_MAX + 16U];
    size_t length = 0U;
    static const char prefix[] = "[EHCI-START] ";
    const uint64_t flags = serial_sequence_begin();
    const int index = (int)(controller - ehci_controllers);

    while (prefix[length] != '\0') {
        line[length] = prefix[length];
        ++length;
    }
    ehci_start_render(controller, line + length, sizeof(line) - length);

    /* The record's own copy: the same bytes without the console prefix. */
    for (length = 0U; prefix[length] != '\0'; ++length) {
    }
    if (index >= 0 && (uint32_t)index < EHCI_MAX_CONTROLLERS) {
        size_t copied = 0U;

        ehci_start_text_valid[index] = false;
        while (line[length + copied] != '\0' &&
               line[length + copied] != '\n' &&
               copied + 1U < EHCI_START_TEXT_MAX) {
            ehci_start_text[index][copied] = line[length + copied];
            ++copied;
        }
        ehci_start_text[index][copied] = '\0';
        ehci_start_text_valid[index] = copied != 0U;
    }

    console_journal_begin();
    EHCI_PUBLISH(line);
    console_journal_end();
    serial_sequence_end(flags);
}

/*
 * ITEM-563/582: the raw PORTSC of every root port.  The boot snapshot and the
 * rescan's fresh measurements are separate records with separate prefixes, so
 * the machine that found nothing at boot and the machine that found the stick
 * later both remain readable.
 */
static void ehci_report_portsc(ehci_report_sink_fn sink, void *context,
                               const struct ehci_controller *controller,
                               const struct ehci_port_state *probes,
                               uint32_t count,
                               const char *prefix) {
        for (uint32_t port_index = 0U;
             port_index < count; ++port_index) {
            const struct ehci_port_state *state = &probes[port_index];
            char port_line[192];
            struct ehci_text port_text;

            ehci_text_begin(&port_text, port_line, sizeof(port_line));
            ehci_text_write(&port_text, prefix);
            ehci_text_location(&port_text, controller);
            ehci_text_write(&port_text, " port=");
            ehci_text_u64(&port_text, port_index + 1U);
            ehci_text_write(&port_text, " first=");
            ehci_text_hex32(&port_text, state->raw_first);
            ehci_text_write(&port_text, " portsc=");
            ehci_text_hex32(&port_text, state->portsc);
            ehci_text_write(&port_text, " ccs=");
            ehci_text_u64(&port_text,
                          ehci_port_connected(state->portsc) ? 1U : 0U);
            ehci_text_write(&port_text, " ccs_first=");
            ehci_text_u64(&port_text,
                          ehci_port_connected(state->raw_first) ? 1U : 0U);
            ehci_text_write(&port_text, " ppc=");
            ehci_text_u64(&port_text, controller->ppc ? 1U : 0U);
            ehci_text_write(&port_text, " pp=");
            ehci_text_u64(&port_text,
                          (state->portsc & EHCI_PORTSC_PP) != 0U ? 1U : 0U);
            ehci_text_write(&port_text, " powered=");
            ehci_text_u64(&port_text, state->powered ? 1U : 0U);
            ehci_text_write(&port_text, " waited_us=");
            ehci_text_u64(&port_text, state->waited_us);
            ehci_text_write(&port_text, " settle=");
            ehci_text_write(&port_text,
                            state->connected
                                ? "CONNECTED"
                                : (state->settle_full ? "EMPTY" : "PARTIAL"));
            if (!port_text.overflow) {
                sink(context, port_line);
            }
        }
}

void ehci_diagnostics_report(ehci_report_sink_fn sink, void *context) {
    if (sink == NULL) {
        return;
    }
    for (uint32_t index = 0U; index < ehci_scanned; ++index) {
        const struct ehci_controller *controller = &ehci_controllers[index];
        /*
         * ITEM-564: the buffer has to hold the whole controller record.  At 256
         * bytes it did not, and an overflowing rendering is dropped silently -
         * the run whose report lacked `[EHCI]` was this bound, not a missing
         * record.  The value is the same one the start line uses, where the
         * fields are of the same order.
         */
        char line[EHCI_START_TEXT_MAX + 16U];
        struct ehci_text text;

        if (!controller->present) {
            continue;
        }
        ehci_text_begin(&text, line, sizeof(line));
        ehci_text_write(&text, "[EHCI] pci=");
        ehci_text_location(&text, controller);
        ehci_text_write(&text, " state=");
        ehci_text_write(&text,
                        controller->status == EHCI_STATUS_OK ? "READY"
                                                             : "UNAVAILABLE");
        ehci_text_write(&text, " status=");
        ehci_text_write(&text, ehci_status_string(controller->status));
        ehci_text_write(&text, " detail=");
        ehci_text_write(&text, controller->detail != NULL ? controller->detail
                                                          : "UNKNOWN");
        /* ITEM-556: the handoff verdict and the registers read back after the
           reset travel into the exported file with the controller, so the
           report alone shows whether the firmware let go and what state the
           controller came back in. */
        ehci_text_write(&text, " legacy=");
        ehci_text_write(&text, controller->legacy != NULL ? controller->legacy
                                                          : "UNKNOWN");
        ehci_text_write(&text, " usbintr=");
        ehci_text_hex32(&text, controller->usbintr);
        ehci_text_write(&text, " usbcmd=");
        ehci_text_hex32(&text, controller->usbcmd_after_reset);
        ehci_text_write(&text, " usbsts=");
        ehci_text_hex32(&text, controller->usbsts_after_reset);
        ehci_text_write(&text, " ports=");
        ehci_text_u64(&text, controller->ports);
        ehci_text_write(&text, " connected=");
        ehci_text_u64(&text, controller->ports_connected);
        ehci_text_write(&text, " attached=");
        ehci_text_u64(&text, controller->attached);
        ehci_text_write(&text, " walk=");
        ehci_text_write(&text,
                        ehci_walk_outcome_string(controller->walk_outcome));
        if (controller->walk_stage != NULL) {
            ehci_text_write(&text, " walk_stage=");
            ehci_text_write(&text, controller->walk_stage);
        }
        ehci_text_write(&text, " walk_port=");
        ehci_text_u64(&text, controller->walk_port);
        ehci_text_write(&text, " sync=");
        ehci_text_write(&text,
                        ehci_engine_cleanup_string(controller->last_cleanup));
        if (controller->last_doorbell != 0U) {
            ehci_text_write(&text, " doorbell=");
            ehci_text_hex32(&text, controller->last_doorbell);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->last_doorbell_readback);
        }
        ehci_text_write(&text, " stale_iaa=");
        ehci_text_u64(&text, controller->last_stale_iaa ? 1U : 0U);
        ehci_text_write(&text, " halt=");
        ehci_text_u64(&text, controller->last_halted_confirmed ? 1U : 0U);
        ehci_text_write(&text, " restart=");
        ehci_text_u64(&text, controller->last_restarted ? 1U : 0U);
        ehci_text_write(&text, "/");
        ehci_text_u64(&text, controller->last_restart_confirmed ? 1U : 0U);
        if (controller->last_usbcmd_after != 0U) {
            ehci_text_write(&text, " after=");
            ehci_text_hex32(&text, controller->last_usbcmd_after);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->last_usbsts_after);
        }
        ehci_text_write(&text, " hcsparams=");
        ehci_text_hex32(&text, controller->hcsparams);
        ehci_text_write(&text, " hccparams=");
        ehci_text_hex32(&text, controller->hccparams);
        ehci_text_write(&text, " ac64=");
        ehci_text_u64(&text, controller->ac64 ? 1U : 0U);
        ehci_text_write(&text, " ppc=");
        ehci_text_u64(&text, controller->ppc ? 1U : 0U);
        ehci_text_write(&text, " probed=");
        ehci_text_u64(&text, controller->port_probe_count);
        if (controller->port != 0U) {
            ehci_text_write(&text, " port=");
            ehci_text_u64(&text, controller->port);
            ehci_text_write(&text, " portsc=");
            ehci_text_hex(&text, controller->portsc);
        }
        if (!text.overflow) {
            sink(context, line);
        }
        /*
         * ITEM-569: the DMA pool and its format.  The buffer addresses are
         * printed with both halves: a 64-bit descriptor buffer pointer is two
         * dwords, and a record that printed one dword could not show the
         * format at all.
         */
        if (controller->regs != NULL && controller->dma != NULL) {
            char dma_line[EHCI_START_TEXT_MAX + 16U];
            struct ehci_text dma_text;
            const uint64_t pool = controller->dma_physical;

            ehci_text_begin(&dma_text, dma_line, sizeof(dma_line));
            ehci_text_write(&dma_text, "[EHCI-DMA] pci=");
            ehci_text_location(&dma_text, controller);
            ehci_text_write(&dma_text, " ac64=");
            ehci_text_u64(&dma_text, controller->ac64 ? 1U : 0U);
            ehci_text_write(&dma_text, " hccparams=");
            ehci_text_hex32(&dma_text, controller->hccparams);
            ehci_text_write(&dma_text, " pool=");
            ehci_text_hex(&dma_text, pool);
            ehci_text_write(&dma_text, " qh=");
            ehci_text_hex(&dma_text, pool + EHCI_LIST_HEAD_OFFSET);
            ehci_text_write(&dma_text, "/");
            ehci_text_hex(&dma_text, pool + EHCI_TRANSFER_QH_OFFSET);
            ehci_text_write(&dma_text, " qh_bytes=");
            ehci_text_u64(&dma_text, EHCI_QH64_BYTES);
            ehci_text_write(&dma_text, "/");
            ehci_text_u64(&dma_text, EHCI_QH_BYTES);
            ehci_text_write(&dma_text, " qh_stride=");
            ehci_text_u64(&dma_text, EHCI_QH_STRIDE);
            ehci_text_write(&dma_text, " qtd=");
            ehci_text_hex(&dma_text, pool + EHCI_QTD_OFFSET);
            ehci_text_write(&dma_text, " qtd_bytes=");
            ehci_text_u64(&dma_text, EHCI_QTD64_BYTES);
            ehci_text_write(&dma_text, "/");
            ehci_text_u64(&dma_text, EHCI_QTD_BYTES);
            ehci_text_write(&dma_text, " qtd_stride=");
            ehci_text_u64(&dma_text, EHCI_QTD_STRIDE);
            ehci_text_write(&dma_text, " bounce=");
            ehci_text_hex(&dma_text, pool + EHCI_BOUNCE_OFFSET);
            ehci_text_write(&dma_text, " setup=");
            ehci_text_hex(&dma_text,
                          pool + EHCI_BOUNCE_OFFSET + EHCI_SETUP_LOCAL);
            ehci_text_write(&dma_text, " data=");
            ehci_text_hex(&dma_text,
                          pool + EHCI_BOUNCE_OFFSET + EHCI_DATA_LOCAL);
            ehci_text_write(&dma_text, " bulk=");
            ehci_text_hex(&dma_text,
                          pool + EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL);
            ehci_text_write(&dma_text, " plb=");
            ehci_text_hex(&dma_text, pool + EHCI_FRAME_LIST_OFFSET);
            if (!dma_text.overflow) {
                sink(context, dma_line);
            }
        }
        /*
         * ITEM-567: the reset's own words, one record per port that was reset.
         * The earlier runs had only the pre-reset port record on screen, so the
         * refusal `detail=PR` could not be read: the words written to assert
         * and to drop the reset, the words read while it was held and after it
         * was dropped, and how much of the completion window was used are here.
         */
        for (uint32_t port_index = 0U;
             port_index < controller->reset_probe_count &&
             port_index < EHCI_PORT_RECORDS;
             ++port_index) {
            const struct ehci_port_reset_state *state =
                &controller->reset_probes[port_index];
            char reset_line[EHCI_START_TEXT_MAX + 16U];
            struct ehci_text reset_text;

            /* A port the walk never reset has no record; its zeroed slot is not
               a "port read zero" and must not be published as one. */
            if (state->portsc_before == 0U && state->portsc_set == 0U) {
                continue;
            }
            ehci_text_begin(&reset_text, reset_line, sizeof(reset_line));
            ehci_text_write(&reset_text, "[EHCI-PORT] pci=");
            ehci_text_location(&reset_text, controller);
            ehci_text_write(&reset_text, " port=");
            ehci_text_u64(&reset_text, port_index + 1U);
            ehci_text_write(&reset_text, " reset=");
            ehci_text_write(&reset_text,
                            ehci_port_reset_result_string(state->result));
            ehci_text_write(&reset_text, " rb=");
            ehci_text_hex32(&reset_text, state->portsc_before);
            ehci_text_write(&reset_text, " rs=");
            ehci_text_hex32(&reset_text, state->portsc_set);
            ehci_text_write(&reset_text, " rh=");
            ehci_text_hex32(&reset_text, state->portsc_hold);
            ehci_text_write(&reset_text, " rc=");
            ehci_text_hex32(&reset_text, state->portsc_cleared);
            ehci_text_write(&reset_text, " rb2=");
            ehci_text_hex32(&reset_text, state->portsc_cleared_readback);
            ehci_text_write(&reset_text, " ra=");
            ehci_text_hex32(&reset_text, state->portsc_after);
            ehci_text_write(&reset_text, " wait_us=");
            ehci_text_u64(&reset_text, state->waited_us);
            ehci_text_write(&reset_text, " rec=");
            ehci_text_u64(&reset_text, state->cleared_twice ? 1U : 0U);
            ehci_text_write(&reset_text, " power=");
            ehci_text_u64(&reset_text, state->powered ? 1U : 0U);
            ehci_text_write(&reset_text, " hse=");
            ehci_text_write(&reset_text,
                            ehci_port_reset_point_string(state->hse_point));
            ehci_text_write(&reset_text, " hcmd=");
            ehci_text_hex32(&reset_text, state->hse_usbcmd);
            ehci_text_write(&reset_text, " hsts=");
            ehci_text_hex32(&reset_text, state->hse_usbsts);
            ehci_text_write(&reset_text, " cmd=");
            ehci_text_hex32(&reset_text,
                            state->usbcmd[EHCI_PORT_RESET_POINT_BEFORE]);
            ehci_text_write(&reset_text, "/");
            ehci_text_hex32(&reset_text,
                            state->usbcmd[EHCI_PORT_RESET_POINT_AFTER]);
            ehci_text_write(&reset_text, " sts=");
            ehci_text_hex32(&reset_text,
                            state->usbsts[EHCI_PORT_RESET_POINT_BEFORE]);
            ehci_text_write(&reset_text, "/");
            ehci_text_hex32(&reset_text,
                            state->usbsts[EHCI_PORT_RESET_POINT_AFTER]);
            if (controller->reset_pcists_valid[port_index]) {
                ehci_text_write(&reset_text, " pcists=");
                ehci_text_hex32(&reset_text,
                                controller->reset_pcists[port_index]);
            }
            if (!reset_text.overflow) {
                sink(context, reset_line);
            }
        }
        /*
         * ITEM-568: the BIOS -> OS handoff's own record.  It was journal-only,
         * and the run that needed it (`legacy=NOT_FOUND`) had an unbound
         * journal: the configuration-space pointer, the chain words, the
         * ownership semaphores and the SMI enables are what tell whether the
         * firmware was ever asked to release the controller.
         */
        if (controller->legacy_valid) {
            const struct ehci_legacy_state *legacy = &controller->legacy_state;
            char legacy_line[EHCI_START_TEXT_MAX + 16U];
            struct ehci_text legacy_text;

            ehci_text_begin(&legacy_text, legacy_line, sizeof(legacy_line));
            ehci_text_write(&legacy_text, "[EHCI-LEGACY] pci=");
            ehci_text_location(&legacy_text, controller);
            ehci_text_write(&legacy_text, " eecp=");
            ehci_text_hex32(&legacy_text, legacy->eecp);
            ehci_text_write(&legacy_text, " result=");
            ehci_text_write(&legacy_text,
                            ehci_legacy_result_string(legacy->result));
            ehci_text_write(&legacy_text, " caps=");
            ehci_text_u64(&legacy_text, legacy->cap_count);
            for (uint32_t cap = 0U; cap < legacy->cap_count; ++cap) {
                ehci_text_write(&legacy_text, " cap");
                ehci_text_u64(&legacy_text, cap);
                ehci_text_write(&legacy_text, "=");
                ehci_text_hex32(&legacy_text, legacy->cap_offset[cap]);
                ehci_text_write(&legacy_text, ":");
                ehci_text_hex32(&legacy_text, legacy->caps[cap]);
            }
            ehci_text_write(&legacy_text, " sup=");
            ehci_text_hex32(&legacy_text, legacy->sup_before);
            ehci_text_write(&legacy_text, "/");
            ehci_text_hex32(&legacy_text, legacy->sup_written);
            ehci_text_write(&legacy_text, "/");
            ehci_text_hex32(&legacy_text, legacy->sup_after);
            ehci_text_write(&legacy_text, " ctl=");
            ehci_text_hex32(&legacy_text, legacy->ctl_before);
            ehci_text_write(&legacy_text, "/");
            ehci_text_hex32(&legacy_text, legacy->ctl_after);
            ehci_text_write(&legacy_text, " waited_us=");
            ehci_text_u64(&legacy_text, legacy->waited_us);
            ehci_text_write(&legacy_text, " forced=");
            ehci_text_u64(&legacy_text, legacy->forced ? 1U : 0U);
            if (!legacy_text.overflow) {
                sink(context, legacy_line);
            }
        }
        /*
         * ITEM-571: the bound device's last failed command - opcode, CSW status
         * and residue, and the sense the device answered.  `diag save` builds
         * this report before it touches the journal, so the refusal names the
         * device's own answer even when the medium cannot be written.
         */
        {
            char fault[192];

            if (usb_storage_fault_text(
                    usb_storage_last_fault(&controller->device.usb), fault,
                    sizeof(fault))) {
                char fault_line[EHCI_START_TEXT_MAX + 16U];
                struct ehci_text fault_text;

                ehci_text_begin(&fault_text, fault_line, sizeof(fault_line));
                ehci_text_write(&fault_text, "[EHCI-STORAGE-FAULT] pci=");
                ehci_text_location(&fault_text, controller);
                ehci_text_write(&fault_text, " ");
                ehci_text_write(&fault_text, fault);
                if (!fault_text.overflow) {
                    sink(context, fault_line);
                }
            }
        }
        if (controller->boot_probes_saved) {
            ehci_report_portsc(sink, context, controller,
                               controller->boot_port_probes,
                               controller->boot_port_probe_count,
                               "[EHCI-PORTSC]");
            ehci_report_portsc(sink, context, controller,
                               controller->port_probes,
                               controller->port_probe_count,
                               "[EHCI-RESCAN-PORTSC]");
        } else {
            ehci_report_portsc(sink, context, controller,
                               controller->port_probes,
                               controller->port_probe_count,
                               "[EHCI-PORTSC]");
        }
        /*
         * ITEM-562: each connected port's path.  The run that reported
         * `DESCRIPTOR_FAILED/NO_DEVICE` had two controllers and no descriptor
         * read at all; a single stage per controller could not say which port
         * ended where, or that a port was empty rather than refused.
         */
        for (uint32_t port_index = 0U;
             port_index < controller->walk_port_count &&
             port_index < EHCI_WALK_PORT_RECORDS;
             ++port_index) {
            const struct ehci_walk_port *record =
                &controller->walk_ports[port_index];
            char path_line[192];
            struct ehci_text path_text;

            ehci_text_begin(&path_text, path_line, sizeof(path_line));
            ehci_text_write(&path_text, "[EHCI-PATH] pci=");
            ehci_text_location(&path_text, controller);
            ehci_text_write(&path_text, " port=");
            ehci_text_u64(&path_text, record->port);
            if (record->hub_address != 0U) {
                ehci_text_write(&path_text, " hub=");
                ehci_text_u64(&path_text, record->hub_address);
                ehci_text_write(&path_text, ".");
                ehci_text_u64(&path_text, record->hub_port);
            }
            if (record->hub_address != 0U) {
                ehci_text_write(&path_text, " wstatus=");
                ehci_text_hex32(&path_text, record->hub_status);
                ehci_text_write(&path_text, " wchange=");
                ehci_text_hex32(&path_text, record->hub_change);
                ehci_text_write(&path_text, " verdict=");
                ehci_text_write(&path_text, record->hub_verdict != NULL
                                                ? record->hub_verdict
                                                : "UNKNOWN");
            }
            ehci_text_write(&path_text, " outcome=");
            ehci_text_write(&path_text,
                            ehci_walk_outcome_string(record->outcome));
            ehci_text_write(&path_text, " stage=");
            ehci_text_write(&path_text, record->stage != NULL
                                            ? record->stage
                                            : "NONE");
            if (!path_text.overflow) {
                sink(context, path_line);
            }
        }
        /*
         * ITEM-560: the state the controller was started from, the PCI words
         * that show whether it was a bus master, the pool it must reach and the
         * first moment it reported HSE.  Its own record, so the [EHCI] line
         * above stays inside one journal record.
         */
        {
            char start_line[EHCI_START_TEXT_MAX + 16U];
            size_t length = 0U;
            static const char prefix[] = "[EHCI-START] ";

            while (prefix[length] != '\0') {
                start_line[length] = prefix[length];
                ++length;
            }
            ehci_start_render(controller, start_line + length,
                              sizeof(start_line) - length);
            /* The rendering ends with the newline the serial line needs; the
               report's sink takes one line at a time. */
            for (size_t index = 0U; start_line[index] != '\0'; ++index) {
                if (start_line[index] == '\n') {
                    start_line[index] = '\0';
                    break;
                }
            }
            sink(context, start_line);
        }
        /*
         * ITEM-558: the transfer that stopped the walk travels with the report.
         * Before this, the [EHCI] record named the stage and the snapshot was
         * only on the serial stream - which the machine that failed does not
         * have.
         */
        if (controller->failure.valid && controller->failure.text[0] != '\0') {
            /*
             * ITEM-564: the same four short records the serial stream carries,
             * rendered here so the report file and the `ehci` command hand the
             * operator values that fit a row instead of one wrapped line where
             * the transfer's verdict and the stop's ran together.
             */
            char line[192];
            struct ehci_text text;

            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-FAIL] result=");
            ehci_text_write(&text, ehci_transfer_result_string(
                                        controller->failure.result));
            ehci_text_write(&text, " cause=");
            ehci_text_write(&text, ehci_engine_cause_string(
                                        controller->failure.cause));
            ehci_text_write(&text, " index=");
            ehci_text_u64(&text, controller->failure.failed_index);
            ehci_text_write(&text, "/");
            ehci_text_u64(&text, controller->failure.token_count);
            ehci_text_write(&text, " qtd=");
            ehci_text_hex32(&text, controller->failure.failed_token);
            if (!text.overflow) {
                sink(context, line);
            }

            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-FAIL-TOKENS]");
            for (uint32_t index = 0U;
                 index < controller->failure.token_count; ++index) {
                ehci_text_write(&text, " q");
                ehci_text_u64(&text, index);
                ehci_text_write(&text, "=");
                ehci_text_hex32(&text, controller->failure.tokens[index]);
            }
            if (!text.overflow) {
                sink(context, line);
            }

            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-FAIL-REG] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " port=");
            ehci_text_u64(&text, controller->failure.root_port);
            ehci_text_write(&text, " usbcmd=");
            ehci_text_hex32(&text, controller->failure.usbcmd);
            ehci_text_write(&text, " usbsts=");
            ehci_text_hex32(&text, controller->failure.usbsts);
            ehci_text_write(&text, " portsc=");
            ehci_text_hex32(&text, controller->failure.portsc);
            if (!text.overflow) {
                sink(context, line);
            }

            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-FAIL-SYNC] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " cleanup=");
            ehci_text_write(&text, ehci_engine_cleanup_string(
                                        controller->failure.cleanup));
            ehci_text_write(&text, " schedule=");
            ehci_text_write(&text, controller->failure.schedule_running
                                        ? "running"
                                        : "off");
            ehci_text_write(&text, " stale_iaa=");
            ehci_text_u64(&text, controller->failure.stale_iaa ? 1U : 0U);
            ehci_text_write(&text, " doorbell=");
            ehci_text_hex32(&text, controller->failure.doorbell);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->failure.doorbell_readback);
            ehci_text_write(&text, " quiesced=");
            ehci_text_u64(&text, controller->failure.quiesced ? 1U : 0U);
            ehci_text_write(&text, " stalled=");
            ehci_text_u64(&text, controller->failure.stalled ? 1U : 0U);
            ehci_text_write(&text, " halt=");
            ehci_text_u64(&text,
                          controller->failure.halted_confirmed ? 1U : 0U);
            ehci_text_write(&text, " restart=");
            ehci_text_u64(&text, controller->failure.restarted ? 1U : 0U);
            ehci_text_write(&text, "/");
            ehci_text_u64(&text,
                          controller->failure.restart_confirmed ? 1U : 0U);
            if (!text.overflow) {
                sink(context, line);
            }

            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-FAIL-STATE] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " before=");
            ehci_text_hex32(&text, controller->failure.usbcmd_before_link);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->failure.usbsts_before_link);
            ehci_text_write(&text, " first=");
            ehci_text_hex32(&text, controller->failure.usbcmd_first_refusal);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->failure.usbsts_first_refusal);
            ehci_text_write(&text, " after=");
            ehci_text_hex32(&text, controller->failure.usbcmd_after);
            ehci_text_write(&text, "/");
            ehci_text_hex32(&text, controller->failure.usbsts_after);
            ehci_text_write(&text, " ass_wait_us=");
            ehci_text_u64(&text, controller->failure.ass_waited_us);
            if (!text.overflow) {
                sink(context, line);
            }
        }
        if (controller->device.block_bound) {
            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-STORAGE] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " state=BOUND blocks=");
            ehci_text_u64(&text, controller->device.usb.block_count);
            ehci_text_write(&text, " block=");
            ehci_text_u64(&text, controller->device.usb.block_size);
            ehci_text_write(&text, " commands=");
            ehci_text_u64(&text, controller->device.usb.command_count);
            ehci_text_write(&text, " reads=");
            ehci_text_u64(&text, controller->device.usb.read_count);
            ehci_text_write(&text, " writes=");
            ehci_text_u64(&text, controller->device.usb.write_count);
            ehci_text_write(&text, " errors=");
            ehci_text_u64(&text, controller->device.usb.transport_error_count);
            ehci_text_write(&text, " cache=");
            ehci_text_write(&text, controller->device.usb.cache_known
                                       ? "KNOWN"
                                       : "UNKNOWN");
            ehci_text_write(&text, " wce=");
            ehci_text_write(&text, controller->device.usb.cache_known
                                       ? (controller->device.usb.wce ? "1"
                                                                     : "0")
                                       : "-");
            ehci_text_write(&text, " flush=");
            ehci_text_write(&text, usb_storage_flush_policy_string(
                                       controller->device.usb.flush_policy));
            if (!text.overflow) {
                sink(context, line);
            }
        }
        if (controller->device.medium_probed) {
            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[INSTALL-MEDIUM] controller=ehci pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " state=");
            ehci_text_write(&text, install_medium_result_string(
                                       controller->device.medium_result));
            ehci_text_write(&text, " reason=");
            ehci_text_write(&text, controller->device.medium_reason != NULL
                                       ? controller->device.medium_reason
                                       : "UNKNOWN");
            if (!text.overflow) {
                sink(context, line);
            }
        }
        /* ITEM-582: the diagnostic-medium attempt, per controller. */
        if (controller->device.diagnostic_probed) {
            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[DIAG-MEDIUM] controller=ehci pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " state=");
            ehci_text_write(&text, controller->device.diagnostic_bound
                                       ? "BOUND"
                                       : "REFUSED");
            ehci_text_write(&text, " result=");
            ehci_text_write(&text, controller->device.diagnostic_result != NULL
                                       ? controller->device.diagnostic_result
                                       : "UNAVAILABLE");
            ehci_text_write(&text, " reason=");
            ehci_text_write(&text, controller->device.diagnostic_reason != NULL
                                       ? controller->device.diagnostic_reason
                                       : "UNKNOWN");
            if (!text.overflow) {
                sink(context, line);
            }
        }
        if (controller->rescan_done) {
            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-RESCAN] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " attached=");
            ehci_text_u64(&text, controller->rescan_attached);
            ehci_text_write(&text, " connected=");
            ehci_text_u64(&text, controller->rescan_connected);
            ehci_text_write(&text, " outcome=");
            ehci_text_write(&text, controller->rescan_outcome != NULL
                                       ? controller->rescan_outcome
                                       : "NONE");
            if (!text.overflow) {
                sink(context, line);
            }
        }
        /*
         * ITEM-582: every configuration candidate, in walk order.  The refused
         * candidate is not the last one the walk saw, so the records are kept
         * per candidate: a foreign device (a camera, a phone, a hub) and a
         * stick the driver could not configure are different answers, and the
         * export must carry both the identity and the bytes.
         */
        for (uint32_t record_index = 0U;
             record_index < controller->config_count; ++record_index) {
            const struct ehci_config_evidence *record =
                &controller->config_records[record_index];

            if (!record->valid) {
                continue;
            }
            ehci_text_begin(&text, line, sizeof(line));
            ehci_text_write(&text, "[EHCI-CONFIG] pci=");
            ehci_text_location(&text, controller);
            ehci_text_write(&text, " root=");
            ehci_text_u64(&text, record->root_port);
            if (record->hub_address != 0U) {
                ehci_text_write(&text, " hub=");
                ehci_text_u64(&text, record->hub_address);
                ehci_text_write(&text, " hub_port=");
                ehci_text_u64(&text, record->hub_port);
            }
            ehci_text_write(&text, " addr=");
            ehci_text_u64(&text, record->address);
            ehci_text_write(&text, " vid=");
            ehci_text_hex(&text, record->vendor_id);
            ehci_text_write(&text, " pid=");
            ehci_text_hex(&text, record->product_id);
            ehci_text_write(&text, " stage=");
            ehci_text_write(&text, record->stage != NULL ? record->stage
                                                         : "UNKNOWN");
            if (record->detail != NULL && record->detail[0] != '\0') {
                ehci_text_write(&text, " detail=");
                ehci_text_write(&text, record->detail);
            }
            ehci_text_write(&text, " setup=");
            for (uint32_t byte = 0U; byte < sizeof(record->setup); ++byte) {
                static const char hex[] = "0123456789ABCDEF";
                char pair[3];

                if (byte != 0U) {
                    ehci_text_write(&text, " ");
                }
                pair[0] = hex[(record->setup[byte] >> 4U) & 0x0FU];
                pair[1] = hex[record->setup[byte] & 0x0FU];
                pair[2] = '\0';
                ehci_text_write(&text, pair);
            }
            ehci_text_write(&text, " received=");
            ehci_text_u64(&text, record->received);
            ehci_text_write(&text, " requested=");
            ehci_text_u64(&text, record->requested);
            ehci_text_write(&text, " total=");
            ehci_text_u64(&text, record->total);
            ehci_text_write(&text, " raw=");
            for (uint32_t byte = 0U; byte < EHCI_CONFIG_RAW_BYTES; ++byte) {
                static const char hex[] = "0123456789ABCDEF";
                char pair[3];

                if (byte != 0U) {
                    ehci_text_write(&text, " ");
                }
                pair[0] = hex[(record->raw[byte] >> 4U) & 0x0FU];
                pair[1] = hex[record->raw[byte] & 0x0FU];
                pair[2] = '\0';
                ehci_text_write(&text, pair);
            }
            if (record->interface_class != 0U) {
                ehci_text_write(&text, " iface=");
                ehci_text_hex(&text, record->interface_class);
            }
            ehci_text_write(&text, " bound=");
            ehci_text_u64(&text, record->block_bound ? 1U : 0U);
            if (!text.overflow) {
                sink(context, line);
            }
        }
    }
}

bool ehci_storage_medium_status(const char **state, const char **reason) {
    const struct ehci_controller *controller = ehci_storage_controller();

    if (controller == NULL || !controller->device.medium_probed) {
        if (state != NULL) {
            *state = "UNCHECKED";
        }
        if (reason != NULL) {
            *reason = "NOT_PROBED";
        }
        return false;
    }
    if (controller->device.diagnostic_bound) {
        if (state != NULL) {
            *state = "DIAGNOSTIC";
        }
        if (reason != NULL) {
            *reason = "BOUND";
        }
        return true;
    }
    if (state != NULL) {
        *state =
            install_medium_result_string(controller->device.medium_result);
    }
    if (reason != NULL) {
        *reason = controller->device.medium_reason != NULL
                      ? controller->device.medium_reason
                      : "UNKNOWN";
    }
    return controller->device.medium_result == INSTALL_MEDIUM_MATCH;
}
