/*
 * ITEM-574/576: the Intel xHCI USB port-routing stage.
 *
 * Panther Point and Lynx Point have one xHCI and two EHCI functions; the
 * chipset description (Intel 7 Series PCH datasheet, sections 5.19 and
 * 16.2.1.3, and the USB routing registers in 17.1.33-34) fixes which of them
 * own the switchable USB 2.0 lines:
 *
 *   00:14.0  D20:F0  xHCI            the controller these registers belong to
 *   00:1A.0  D26:F0  EHCI #1         a separate controller; the report's boot
 *                                    medium is on its root port (behind the
 *                                    rate-matching hub), so its ports are NOT
 *                                    part of the routing domain
 *   00:1D.0  D29:F0  EHCI #2         the controller whose USB 2.0 lines the
 *                                    XUSB2PR register moves between it and the
 *                                    xHCI
 *
 * Two EHCI functions are therefore the documented topology, not an ambiguity.
 * For 8086:1E31 the USB 2.0 routing field is four bits and the SuperSpeed
 * field is four bits; the router writes exactly the mask bits the firmware
 * declares switchable (Linux's usb_enable_intel_xhci_ports() semantics:
 * USB3PRM -> USB3_PSSEN, then USB2PRM -> XUSB2PR, each verified).
 *
 * The routing registers live in the xHCI function's PCI configuration space:
 *
 *   0xD0 XUSB2PR    USB 2.0 port routing  - bit set: the port belongs to xHCI
 *   0xD4 XUSB2PRM   USB 2.0 routing mask  - bit set: the OS may move it
 *   0xD8 USB3_PSSEN USB 3.0 SuperSpeed termination enable
 *   0xDC USB3PRM    USB 3.0 SuperSpeed enable mask
 *
 * This module never touches hardware itself: every access goes through
 * `struct xhci_route_ops`, which is what lets tests/xhci-route-host.c drive
 * the production decision on a substituted PCI configuration space with the
 * three functions above.
 *
 * The boot medium's safety is a topology argument, not a root-port status:
 * the medium is on 00:1A.0's root port, behind the rate-matching hub, and
 * that controller is not the one the routing registers move.  A root PORTSC
 * CCS bit is not a proof of a downstream device (the RMH keeps it set when
 * the child is gone), so this module does not read one and the driver does
 * not restore the routing on a root-port hint; the exported EHCI records and
 * the successful `diag save` are the medium's own proof.
 *
 * The rule is verified for 8086:1E31 only.  The other known 7/8-series IDs
 * are reported as SKIP_UNVERIFIED_TOPOLOGY until their topology and field
 * widths are checked the same way; nothing is written for them.
 */
#ifndef XHCI_ROUTE_H
#define XHCI_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XHCI_ROUTE_XUSB2PR 0x00D0U
#define XHCI_ROUTE_USB2PRM 0x00D4U
#define XHCI_ROUTE_USB3_PSSEN 0x00D8U
#define XHCI_ROUTE_USB3PRM 0x00DCU

/* 8086:1E31: four USB 2.0 lines and four SuperSpeed lanes are switchable.
   A mask bit outside its field is not a routing register this driver
   understands, so nothing is written. */
#define XHCI_ROUTE_USB2_FIELD 0x0000000FU
#define XHCI_ROUTE_USB3_FIELD 0x0000000FU

/* The documented 8086:1E31 participants, by PCI address.  The addresses are
   part of the rule: two EHCI functions are expected, and only D29:F0's USB
   2.0 lines are in the routing domain (datasheet 5.19, 16.2.1.3, 17.1.33-34). */
#define XHCI_ROUTE_1E31_BUS 0U
#define XHCI_ROUTE_1E31_XHCI_DEVICE 0x14U
#define XHCI_ROUTE_1E31_MEDIUM_EHCI_DEVICE 0x1AU
#define XHCI_ROUTE_1E31_ROUTE_EHCI_DEVICE 0x1DU
#define XHCI_ROUTE_1E31_FUNCTION 0U
#define XHCI_ROUTE_DETAIL_BYTES 80U

struct xhci_route_regs {
    uint32_t xusb2pr;
    uint32_t usb2prm;
    uint32_t usb3_pssen;
    uint32_t usb3prm;
    /* Every read completed and no register answered all-ones. */
    bool read_ok;
};

struct xhci_route_ops {
    /* Read one dword of the named PCI function's configuration space. */
    bool (*read32)(void *context, uint8_t bus, uint8_t device,
                   uint8_t function, uint16_t offset, uint32_t *value);
    /* Write one dword; false when the access itself failed. */
    bool (*write32)(void *context, uint8_t bus, uint8_t device,
                    uint8_t function, uint16_t offset, uint32_t value);
};

enum xhci_route_outcome {
    XHCI_ROUTE_OK, /* routing register(s) written and verified */
    XHCI_ROUTE_SKIP_NOT_INTEL,
    XHCI_ROUTE_SKIP_UNKNOWN_DEVICE,
    /* A known controller whose topology/field rules are not verified yet. */
    XHCI_ROUTE_SKIP_UNVERIFIED_TOPOLOGY,
    /* The documented 8086:1E31 participants are absent or not the expected
       functions; no write is attempted. */
    XHCI_ROUTE_REFUSE_TOPOLOGY,
    XHCI_ROUTE_REFUSE_READ,
    XHCI_ROUTE_REFUSE_MASK,
    XHCI_ROUTE_SKIP_NOTHING,
    XHCI_ROUTE_FAIL_WRITE,
    XHCI_ROUTE_FAIL_READBACK,
};

struct xhci_route_result {
    enum xhci_route_outcome outcome;
    struct xhci_route_regs before;
    struct xhci_route_regs after;
    /* The fields 8086:1E31 defines for the two groups. */
    uint32_t usb2_field;
    uint32_t usb3_field;
    /* The switchable bits and the values written (equal to the corresponding
       before value when the register was not changed). */
    uint32_t writable_usb2;
    uint32_t writable_usb3;
    uint32_t target_usb2;
    uint32_t target_usb3;
    bool wrote_usb2;
    bool wrote_usb3;
    bool restored;
    char detail[XHCI_ROUTE_DETAIL_BYTES];
};

/* True for the Intel xHCI functions this driver knows by name. */
bool xhci_route_device_known(uint16_t vendor, uint16_t device);

/* Human-readable outcome for the exported record. */
const char *xhci_route_outcome_string(enum xhci_route_outcome outcome);

/*
 * Verify the documented participants of the xHCI function at
 * bus/device/function, read the four routing registers, write the SuperSpeed
 * register before the USB 2.0 register, and verify each write by reading it
 * back.  A failed write or a mismatched read-back restores the register(s)
 * this call changed and reports it.
 *
 * No hardware is touched beyond the documented participants' configuration
 * space and the four named registers.  Nothing is written when the topology
 * is not the documented one, when a mask carries a bit outside its field, or
 * when a register cannot be read.
 */
void xhci_route_ports(const struct xhci_route_ops *ops, void *context,
                      uint8_t bus, uint8_t device, uint8_t function,
                      uint16_t vendor, uint16_t device_id,
                      struct xhci_route_result *result);

#endif
