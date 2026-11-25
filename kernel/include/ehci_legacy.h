#ifndef HFOC_EHCI_LEGACY_H
#define HFOC_EHCI_LEGACY_H

/*
 * ITEM-568: the BIOS -> OS handoff, in the address space the specification puts
 * it in.
 *
 * EHCI 1.0 Table 2-7: the EECP field of HCCPARAMS is "the offset in PCI
 * configuration space of the first EHCI extended capability", and it "must be
 * 40h or greater"; section 5 adds that each capability's Next field is the
 * same kind of offset, that USBLEGSUP lives at EECP+00h and USBLEGCTLSTS at
 * EECP+04h.
 *
 * The previous handoff read `BAR0 + EECP*4` and stepped `offset + Next*4` -
 * a different address space and a different unit.  On the owner's laptop it
 * therefore read unrelated MMIO dwords, found no capability id 01h and
 * reported `legacy=NOT_FOUND`, so the firmware was never asked to release the
 * controller.  The sequence below is the one Linux performs in
 * `drivers/usb/host/pci-quirks.c` (`quirk_usb_disable_ehci()` and
 * `ehci_bios_handoff()`): walk configuration space by byte offset, and when
 * the legacy capability carries the BIOS-owned semaphore, set the OS-owned
 * semaphore, wait up to a second for the BIOS semaphore to clear, force it if
 * it does not, and disable the legacy SMIs in any case.
 *
 * Kept pure so the host proof can drive it on a fake configuration space,
 * including a decoy "capability" left in the MMIO window that the walk must
 * never read.
 */

#include <stdbool.h>
#include <stdint.h>

/* The legacy-support capability id of EHCI extended capabilities (spec 5). */
#define EHCI_LEGACY_CAPABILITY_ID 0x01U
/* USBLEGSUP: the two ownership semaphores (spec Table 2-3). */
#define EHCI_LEGACY_BIOS_OWNED 0x00010000U
#define EHCI_LEGACY_OS_OWNED 0x01000000U
/* The capability pointer must be 40h or greater (spec Table 2-7), and the
   standard configuration space is 256 bytes. */
#define EHCI_LEGACY_EECP_MIN 0x40U
#define EHCI_LEGACY_CONFIG_BYTES 0x100U
/* The list is walked with a bound, not until it loops. */
#define EHCI_LEGACY_MAX_ENTRIES 8U
/* How many capability words one controller keeps as evidence. */
#define EHCI_LEGACY_CAPS_KEPT 4U
/* The BIOS gets one second to hand the controller over (the budget Linux
   uses), and the wait is a bounded series of 1 ms steps. */
#define EHCI_LEGACY_BUDGET_US 1000000U
#define EHCI_LEGACY_STEP_US 1000U

/* The configuration space a handoff runs on, and its clock. */
struct ehci_legacy_ops {
    void *context;
    uint32_t (*read32)(void *context, uint16_t offset);
    void (*write32)(void *context, uint16_t offset, uint32_t value);
    void (*delay_us)(void *context, uint32_t microseconds);
};

enum ehci_legacy_result {
    /* The capability was found, the OS semaphore is set and the BIOS one is
       clear. */
    EHCI_LEGACY_TAKEN = 0,
    /* The capability was found and the BIOS semaphore was already clear: the
       firmware had released the controller. */
    EHCI_LEGACY_NOT_OWNED,
    /* The BIOS would not release it; it was forced clear and the refusal is
       recorded rather than hidden. */
    EHCI_LEGACY_TIMEOUT,
    /* The chain was walked to its end and no legacy capability was in it. */
    EHCI_LEGACY_NOT_FOUND,
    /* HCCPARAMS.EECP is zero or out of range: this controller has no extended
       capabilities at all. */
    EHCI_LEGACY_ABSENT
};

struct ehci_legacy_state {
    uint32_t eecp;              /* HCCPARAMS.EECP, a configuration-space offset */
    uint32_t cap_count;
    uint16_t cap_offset[EHCI_LEGACY_CAPS_KEPT];
    uint32_t caps[EHCI_LEGACY_CAPS_KEPT];
    uint16_t legacy_offset;     /* configuration-space offset of USBLEGSUP */
    uint32_t sup_before;
    uint32_t sup_written;
    uint32_t sup_after;
    uint32_t ctl_before;
    uint32_t ctl_after;
    uint32_t waited_us;
    bool forced;
    enum ehci_legacy_result result;
};

/* `hccparams` is the dword read from BAR0 + 0x08. */
enum ehci_legacy_result ehci_legacy_handoff_run(
    const struct ehci_legacy_ops *ops,
    uint32_t hccparams,
    struct ehci_legacy_state *state);

/* The USBLEGCTLSTS bits this sequence clears: the legacy SMI enables are
   bits 15:0 (spec Table 2-4). */
#define EHCI_LEGACY_CTL_ENABLES 0x0000FFFFU

#endif
