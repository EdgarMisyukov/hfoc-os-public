/*
 * ITEM-568 host proof: every register mask the driver uses, against the
 * specification.
 *
 * The Toshiba run's words are only readable if the bit positions behind them
 * are right: `hse_usbcmd=0x00080010` means `PSE` only if PSE is bit 4, and
 * `hse_usbsts=0x00007010` means `PSS|RECL|HCH|HSE` only if those bits are where
 * this header says.  A wrong mask does not fail on QEMU - it fails as a
 * misread on the machine - so each constant is asserted against the value
 * EHCI 1.0 Tables 2-6, 2-7, 2-8, 2-16 and 2-3 give it, and the composite masks
 * are asserted to be exactly their members.
 */
#include "ehci.h"
#include "ehci_legacy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* Table 2-8 / 2-9: USBCMD and USBSTS, bit for bit. */
static void test_usbcmd_bits(void) {
    CHECK(EHCI_USBCMD_RS == 1U << 0, "USBCMD.RS is bit 0");
    CHECK(EHCI_USBCMD_HCRESET == 1U << 1, "USBCMD.HCRESET is bit 1");
    CHECK(EHCI_USBCMD_PSE == 1U << 4, "USBCMD.PSE is bit 4");
    CHECK(EHCI_USBCMD_ASE == 1U << 5, "USBCMD.ASE is bit 5");
    CHECK(EHCI_USBCMD_IAAD == 1U << 6, "USBCMD.IAAD is bit 6");
    CHECK(EHCI_USBCMD_ITC_MASK == 0x00FF0000U,
          "USBCMD.ITC is bits 23:16");
    CHECK((EHCI_USBCMD_PSE & EHCI_USBCMD_ASE) == 0U &&
              (EHCI_USBCMD_RS & EHCI_USBCMD_ITC_MASK) == 0U,
          "the USBCMD bits do not overlap");
}

static void test_usbsts_bits(void) {
    CHECK(EHCI_USBSTS_UI == 1U << 0, "USBSTS.UI is bit 0");
    CHECK(EHCI_USBSTS_UEE == 1U << 1, "USBSTS.UEE is bit 1");
    CHECK(EHCI_USBSTS_PCD == 1U << 2, "USBSTS.PCD is bit 2");
    CHECK(EHCI_USBSTS_HSE == 1U << 4, "USBSTS.HSE is bit 4");
    CHECK(EHCI_USBSTS_IAA == 1U << 5, "USBSTS.IAA is bit 5");
    CHECK(EHCI_USBSTS_HCH == 1U << 12, "USBSTS.HCH is bit 12");
    CHECK(EHCI_USBSTS_RECL == 1U << 13, "USBSTS.RECL is bit 13");
    CHECK(EHCI_USBSTS_PSS == 1U << 14, "USBSTS.PSS is bit 14");
    CHECK(EHCI_USBSTS_ASS == 1U << 15, "USBSTS.ASS is bit 15");
    CHECK(EHCI_USBSTS_ERROR ==
              (EHCI_USBSTS_UEE | EHCI_USBSTS_HSE | EHCI_USBSTS_PCD),
          "the error mask is exactly USB error, host system error and port "
          "change detect");
    /* The words the Toshiba printed, decoded through these masks. */
    CHECK((0x00080010U & EHCI_USBCMD_PSE) != 0U &&
              (0x00080010U & EHCI_USBCMD_RS) == 0U &&
              (0x00080010U & EHCI_USBCMD_ASE) == 0U,
          "0x00080010 is PSE set with RS and ASE clear");
    CHECK((0x00007010U & EHCI_USBSTS_HSE) != 0U &&
              (0x00007010U & EHCI_USBSTS_HCH) != 0U &&
              (0x00007010U & EHCI_USBSTS_PSS) != 0U &&
              (0x00007010U & EHCI_USBSTS_RECL) != 0U,
          "0x00007010 is HSE with the controller halted and the periodic "
          "schedule running");
}

/* Table 2-16: PORTSC. */
static void test_portsc_bits(void) {
    CHECK(EHCI_PORTSC_CCS == 1U << 0, "PORTSC.CCS is bit 0");
    CHECK(EHCI_PORTSC_CSC == 1U << 1, "PORTSC.CSC is bit 1");
    CHECK(EHCI_PORTSC_PE == 1U << 2, "PORTSC.PE is bit 2");
    CHECK(EHCI_PORTSC_PEC == 1U << 3, "PORTSC.PEC is bit 3");
    CHECK(EHCI_PORTSC_OCA == 1U << 4, "PORTSC.OCA is bit 4");
    CHECK(EHCI_PORTSC_OCC == 1U << 5, "PORTSC.OCC is bit 5");
    CHECK(EHCI_PORTSC_FPR == 1U << 6, "PORTSC.FPR is bit 6");
    CHECK(EHCI_PORTSC_SUSP == 1U << 7, "PORTSC.SUSP is bit 7");
    CHECK(EHCI_PORTSC_PR == 1U << 8, "PORTSC.PR is bit 8");
    CHECK(EHCI_PORTSC_LS_MASK == 0x00000C00U, "PORTSC.LS is bits 11:10");
    CHECK(EHCI_PORTSC_LS_LOW == 0x00000400U, "PORTSC.LS low-speed is 01b");
    CHECK(EHCI_PORTSC_PP == 1U << 12, "PORTSC.PP is bit 12");
    CHECK(EHCI_PORTSC_PO == 1U << 13, "PORTSC.PO is bit 13");
    CHECK(EHCI_PORTSC_PS_MASK == 0x0C000000U, "PORTSC.PS is bits 27:26");
    CHECK(EHCI_PORTSC_PS_SHIFT == 26U, "PORTSC.PS starts at bit 26");
    CHECK(EHCI_PORTSC_PS_HIGH == 0x08000000U, "PORTSC.PS high-speed is 10b");
    CHECK(EHCI_PORTSC_PS_LOW == 0x04000000U, "PORTSC.PS low-speed is 01b");
    CHECK(EHCI_PORTSC_CHANGE ==
              (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC),
          "the change bits are exactly connect, enable and over-current");
    CHECK(EHCI_PORTSC_WRITABLE ==
              (EHCI_PORTSC_PR | EHCI_PORTSC_PO | EHCI_PORTSC_PP |
               EHCI_PORTSC_CHANGE | EHCI_PORTSC_SUSP | EHCI_PORTSC_FPR),
          "the write mask is exactly the writable subset");
    CHECK((EHCI_PORTSC_WRITABLE & EHCI_PORTSC_PE) == 0U,
          "the write mask does not carry Port Enable: writing it is not how a "
          "reset completes");
    CHECK((EHCI_PORTSC_WRITABLE & EHCI_PORTSC_CCS) == 0U &&
              (EHCI_PORTSC_WRITABLE & EHCI_PORTSC_LS_MASK) == 0U &&
              (EHCI_PORTSC_WRITABLE & EHCI_PORTSC_PS_MASK) == 0U,
          "the write mask carries no read-only connect, line-status or speed "
          "bit");
}

/* The capability registers: CAPLENGTH at 0, HCSPARAMS at 4, HCCPARAMS at 8. */
static void test_capability_bits(void) {
    CHECK(EHCI_CAP_CAPLENGTH == 0x00U, "CAPLENGTH is the first capability byte");
    CHECK(EHCI_CAP_HCIVERSION == 0x02U,
          "HCIVERSION follows CAPLENGTH in the same dword");
    CHECK(EHCI_CAP_HCSPARAMS == 0x04U, "HCSPARAMS is at 0x04");
    CHECK(EHCI_CAP_HCCPARAMS == 0x08U, "HCCPARAMS is at 0x08");
    CHECK(EHCI_CAP_HCSP_PORTROUTE == 0x0CU, "HCSP_PORTROUTE is at 0x0C");
    CHECK(EHCI_CAPLENGTH_MASK == 0x000000FFU,
          "CAPLENGTH is the low byte of the first dword");
    CHECK(EHCI_HCSPARAMS_PORTS_MASK == 0x0FU, "HCSPARAMS.N_PORTS is bits 3:0");
    CHECK(EHCI_HCSPARAMS_PPC == 1U << 4, "HCSPARAMS.PPC is bit 4");
    CHECK(EHCI_HCCPARAMS_64BIT == 1U,
          "HCCPARAMS.64-bit addressing is bit 0");
    CHECK(EHCI_HCCPARAMS_EECP_MASK == 0x0000FF00U,
          "HCCPARAMS.EECP is bits 15:8");
    CHECK((EHCI_HCCPARAMS_EECP_MASK >> EHCI_HCCPARAMS_EECP_SHIFT) == 0xFFU,
          "the EECP mask shifts down to one byte");
}

/* The operational window: offsets are relative to CAPLENGTH, PORTSC stride 4. */
static void test_operational_offsets(void) {
    CHECK(EHCI_OP_USBCMD == 0x00U, "USBCMD is the first operational dword");
    CHECK(EHCI_OP_USBSTS == 0x04U, "USBSTS follows USBCMD");
    CHECK(EHCI_OP_PERIODICLISTBASE == 0x14U,
          "PERIODICLISTBASE is operational offset 0x14");
    CHECK(EHCI_OP_ASYNCLISTADDR == 0x18U,
          "ASYNCLISTADDR is operational offset 0x18");
    CHECK(EHCI_OP_CONFIGFLAG == 0x40U, "CONFIGFLAG is operational offset 0x40");
    CHECK(EHCI_OP_PORTSC == 0x44U, "PORTSC port 1 is operational offset 0x44");
    CHECK(EHCI_OP_PORTSC_STRIDE == 0x04U, "root ports are one dword apart");
    CHECK(EHCI_OP_PORTSC + EHCI_OP_PORTSC_STRIDE == 0x48U,
          "PORTSC port 2 is operational offset 0x48");
    /* The address a 32-bit device actually decodes. */
    CHECK(0x20U + EHCI_OP_PORTSC == 0x64U,
          "with CAPLENGTH 0x20, PORTSC port 1 is BAR0 + 0x64");
    CHECK(0x20U + EHCI_OP_USBCMD == 0x20U && 0x20U + EHCI_OP_USBSTS == 0x24U,
          "with CAPLENGTH 0x20, USBCMD is BAR0 + 0x20 and USBSTS BAR0 + 0x24");
}

/* Section 5 and Table 2-3: the legacy-support capability. */
static void test_legacy_bits(void) {
    CHECK(EHCI_LEGACY_CAPABILITY_ID == 0x01U,
          "the legacy-support capability id is 01h");
    CHECK(EHCI_LEGACY_BIOS_OWNED == 1U << 16,
          "USBLEGSUP.BIOS-owned is bit 16");
    CHECK(EHCI_LEGACY_OS_OWNED == 1U << 24,
          "USBLEGSUP.OS-owned is bit 24");
    CHECK(EHCI_LEGACY_EECP_MIN == 0x40U,
          "the EECP must be 40h or greater");
    CHECK(EHCI_LEGACY_CTL_ENABLES == 0x0000FFFFU,
          "USBLEGCTLSTS SMI enables are bits 15:0");
    CHECK((EHCI_LEGACY_BIOS_OWNED & EHCI_LEGACY_OS_OWNED) == 0U,
          "the two semaphores are different bits");
}

int main(void) {
    test_usbcmd_bits();
    test_usbsts_bits();
    test_portsc_bits();
    test_capability_bits();
    test_operational_offsets();
    test_legacy_bits();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI register mask host test: ALL OK\n");
    return 0;
}
