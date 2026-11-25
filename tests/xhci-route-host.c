/*
 * ITEM-576 host proof: the Intel xHCI port-routing decision through the
 * production PCI path, on a substituted configuration space.
 *
 * The production machine's own registers cannot be read without booting the
 * machine, so the proof models the documented 8086:1E31 topology with the
 * addresses the chipset description fixes:
 *
 *   00:14.0  D20:F0  xHCI        8086:1E31, the routing registers 0xD0..0xDC
 *   00:1A.0  D26:F0  EHCI #1     8086:1E2D, the controller with the boot
 *                                medium behind its rate-matching hub
 *   00:1D.0  D29:F0  EHCI #2     8086:1E26, the routing domain
 *
 * and drives the same `xhci_route_ports()` the kernel calls, through the same
 * `struct xhci_route_ops` seam the driver implements.  The cases pin what must
 * never be guessed:
 *
 *   * the measured ae67d8ee registers (0/F/0/F) produce USB3_PSSEN=F first,
 *     then XUSB2PR=F, each verified by read-back, and preserve the reserved
 *     bits outside the four-bit fields;
 *   * two EHCI functions are the documented topology, not an ambiguity; a
 *     missing or misidentified participant is REFUSE_TOPOLOGY with the failed
 *     participant named and no write at all;
 *   * another vendor, an unverified 8-series controller and an unknown Intel
 *     function are evidence-only;
 *   * an unreadable register, an all-ones answer and a mask bit outside the
 *     1E31 fields refuse before any write;
 *   * a refused write and a lying read-back restore the previous routing and
 *     report it, not left half-written.
 */
#include "xhci_route.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

#define FAKE_REGS 4U
#define FAKE_WRITES 8U

struct fake_pci {
    bool have_xhci;
    bool have_d26;
    bool have_d29;
    bool d26_wrong_vendor;
    bool d26_wrong_class;
    bool d29_wrong_vendor;
    bool d29_wrong_class;
    uint32_t regs[FAKE_REGS];
    bool fail_regs;
    bool all_ones_regs;
    /* Offset the configuration space refuses to write; 0xFFFF for none. */
    uint16_t refuse_write_offset;
    bool refuse_once;
    /* Bits the configuration space silently drops on every write; with
       `drop_once` the lie is transient so the restore can succeed. */
    uint32_t drop_on_write;
    bool drop_once;
    uint16_t writes[FAKE_WRITES];
    uint32_t written[FAKE_WRITES];
    size_t write_count;
};

static int reg_index(uint16_t offset) {
    switch (offset) {
    case XHCI_ROUTE_XUSB2PR:
        return 0;
    case XHCI_ROUTE_USB2PRM:
        return 1;
    case XHCI_ROUTE_USB3_PSSEN:
        return 2;
    case XHCI_ROUTE_USB3PRM:
        return 3;
    default:
        return -1;
    }
}

static bool fake_read(void *context, uint8_t bus, uint8_t device,
                      uint8_t function, uint16_t offset, uint32_t *value) {
    struct fake_pci *pci = context;

    if (pci == NULL || value == NULL || bus != 0U || function != 0U) {
        return false;
    }
    if (device == 0x14U) {
        if (!pci->have_xhci) {
            return false;
        }
        if (offset == 0x00U) {
            *value = 0x8086U | (0x1E31U << 16U);
            return true;
        }
        if (offset == 0x08U) {
            *value = (0x30U << 8U) | (0x03U << 16U) | (0x0CU << 24U);
            return true;
        }
        const int index = reg_index(offset);

        if (index < 0) {
            return false;
        }
        if (pci->fail_regs) {
            return false;
        }
        *value = pci->all_ones_regs ? 0xFFFFFFFFU : pci->regs[index];
        return true;
    }
    if (device == 0x1AU || device == 0x1DU) {
        const bool present = device == 0x1AU ? pci->have_d26 : pci->have_d29;
        const bool wrong_vendor =
            device == 0x1AU ? pci->d26_wrong_vendor : pci->d29_wrong_vendor;
        const bool wrong_class =
            device == 0x1AU ? pci->d26_wrong_class : pci->d29_wrong_class;

        if (!present) {
            return false;
        }
        if (offset == 0x00U) {
            const uint16_t vendor = wrong_vendor ? 0x1234U : 0x8086U;
            const uint16_t id = device == 0x1AU ? 0x1E2DU : 0x1E26U;

            *value = vendor | ((uint32_t)id << 16U);
            return true;
        }
        if (offset == 0x08U) {
            const uint8_t class_code = wrong_class ? 0x01U : 0x0CU;

            *value = (0x20U << 8U) | (0x03U << 16U) |
                     ((uint32_t)class_code << 24U);
            return true;
        }
        return false;
    }
    return false;
}

static bool fake_write(void *context, uint8_t bus, uint8_t device,
                       uint8_t function, uint16_t offset, uint32_t value) {
    struct fake_pci *pci = context;
    const int index = reg_index(offset);

    if (pci == NULL || bus != 0U || device != 0x14U || function != 0U ||
        index < 0) {
        return false;
    }
    if (offset == pci->refuse_write_offset) {
        if (pci->refuse_once) {
            pci->refuse_write_offset = 0xFFFFU;
        }
        return false;
    }
    pci->regs[index] = value & ~pci->drop_on_write;
    if (pci->drop_once) {
        pci->drop_on_write = 0U;
    }
    if (pci->write_count < FAKE_WRITES) {
        pci->writes[pci->write_count] = offset;
        pci->written[pci->write_count] = value;
        pci->write_count += 1U;
    }
    return true;
}

static struct xhci_route_ops fake_ops(void) {
    struct xhci_route_ops ops = {fake_read, fake_write};
    return ops;
}

/* The measured ae67d8ee state: both routing registers clear, both masks 0xF,
   both EHCI functions present. */
static struct fake_pci production_state(void) {
    struct fake_pci pci;

    memset(&pci, 0, sizeof(pci));
    pci.have_xhci = true;
    pci.have_d26 = true;
    pci.have_d29 = true;
    pci.regs[0] = 0x00000000U;
    pci.regs[1] = 0x0000000FU;
    pci.regs[2] = 0x00000000U;
    pci.regs[3] = 0x0000000FU;
    pci.refuse_write_offset = 0xFFFFU;
    return pci;
}

static void test_the_measured_state_is_routed(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_OK,
          "the documented topology is routed");
    CHECK(pci.write_count == 2U, "exactly two registers were written");
    CHECK(pci.write_count >= 2U && pci.writes[0] == XHCI_ROUTE_USB3_PSSEN,
          "SuperSpeed terminations are enabled before the USB2 switch-over");
    CHECK(pci.write_count >= 2U && pci.writes[1] == XHCI_ROUTE_XUSB2PR,
          "the USB2 routing register is written second");
    CHECK(pci.regs[0] == 0x0000000FU,
          "the measured USB2 mask becomes the routing value");
    CHECK(pci.regs[2] == 0x0000000FU,
          "the measured SuperSpeed mask becomes the PSSEN value");
    CHECK(result.usb2_field == 0x0000000FU &&
              result.target_usb2 == 0x0000000FU &&
              result.target_usb3 == 0x0000000FU,
          "the four-bit fields and targets are recorded");
    CHECK(result.after.read_ok && result.after.xusb2pr == 0x0000000FU &&
              result.after.usb3_pssen == 0x0000000FU,
          "the after snapshot is the verified read-back");
    CHECK(strstr(result.detail, "d26=OK d29=OK") != NULL &&
              strstr(result.detail, "u2m=0x0000000F") != NULL &&
              strstr(result.detail, "u3=0x0000000F") != NULL,
          "the record names both participants and the written values");
}

static void test_reserved_bits_are_preserved(void) {
    struct fake_pci pci = production_state();

    pci.regs[0] = 0x80000000U;
    pci.regs[2] = 0x10000000U;
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_OK, "the switchable bits are routed");
    CHECK(pci.regs[0] == 0x8000000FU,
          "bits outside the four-bit USB2 field are preserved");
    CHECK(pci.regs[2] == 0x1000000FU,
          "bits outside the four-bit SuperSpeed field are preserved");
}

static void test_a_missing_participant_is_a_named_refusal(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    pci.have_d29 = false;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_TOPOLOGY,
          "a missing routing EHCI is a topology refusal");
    CHECK(strstr(result.detail, "d29=READ") != NULL,
          "the refusal names the participant that did not answer");
    CHECK(pci.write_count == 0U, "no register is written without the topology");
}

static void test_the_medium_controller_must_be_the_documented_one(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    pci.d26_wrong_class = true;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_TOPOLOGY,
          "a wrong function at D26:F0 is not the documented medium controller");
    CHECK(strstr(result.detail, "d26=CLASS") != NULL,
          "the refusal names the class mismatch");

    pci = production_state();
    pci.d29_wrong_vendor = true;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_TOPOLOGY,
          "a non-Intel function at D29:F0 is not the routing companion");
    CHECK(strstr(result.detail, "d29=VID") != NULL,
          "the refusal names the vendor mismatch");
}

static void test_an_xhci_outside_d20f0_is_not_the_rule(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x15U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_TOPOLOGY,
          "a 1E31 outside D20:F0 is refused");
    CHECK(strstr(result.detail, "xhci=NOT_00:14.0") != NULL,
          "the refusal names the address that does not match the rule");
    CHECK(pci.write_count == 0U, "no write happens outside the documented rule");
}

static void test_other_controllers_are_evidence_only(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x1B36U, 0x000DU, &result);
    CHECK(result.outcome == XHCI_ROUTE_SKIP_NOT_INTEL,
          "another vendor's controller is not routed");
    CHECK(pci.write_count == 0U, "a foreign controller sees no write");

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x8C31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_SKIP_UNVERIFIED_TOPOLOGY,
          "a controller whose rule is not verified is evidence-only");
    CHECK(pci.write_count == 0U, "an unverified controller sees no write");

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1234U, &result);
    CHECK(result.outcome == XHCI_ROUTE_SKIP_UNKNOWN_DEVICE,
          "an unknown Intel controller is not guessed");
}

static void test_an_unreadable_register_or_mask_is_refused(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    pci.fail_regs = true;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_READ,
          "a refused configuration read stops the routing");

    pci = production_state();
    pci.all_ones_regs = true;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_READ,
          "an all-ones register is a read failure, not a routing state");

    pci = production_state();
    pci.regs[1] = 0x0004000FU;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_MASK,
          "a USB2 mask beyond the four-bit field is refused");
    CHECK(pci.write_count == 0U, "an impossible mask is not written");

    pci = production_state();
    pci.regs[3] = 0x00000010U;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_REFUSE_MASK,
          "a SuperSpeed mask beyond the four-bit field is refused");
}

static void test_zero_masks_have_nothing_to_route(void) {
    struct fake_pci pci = production_state();
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    pci.regs[1] = 0U;
    pci.regs[3] = 0U;
    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_SKIP_NOTHING,
          "masks that name no switchable port leave the registers alone");
    CHECK(pci.write_count == 0U, "no register is written when nothing is switchable");
}

static void test_a_refused_write_restores_the_previous_routing(void) {
    struct fake_pci pci = production_state();

    pci.regs[0] = 0x00000001U;
    pci.regs[2] = 0x00000002U;
    pci.refuse_write_offset = XHCI_ROUTE_XUSB2PR;
    pci.refuse_once = true;
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_FAIL_WRITE,
          "a refused USB2 write is reported as a write failure");
    CHECK(result.restored, "a transient refusal is undone by the restore");
    CHECK(pci.regs[0] == 0x00000001U && pci.regs[2] == 0x00000002U,
          "both routing registers are back to their previous values");
}

static void test_a_persistent_refusal_is_reported(void) {
    struct fake_pci pci = production_state();

    pci.refuse_write_offset = XHCI_ROUTE_XUSB2PR;
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_FAIL_WRITE,
          "a persistent refusal stays a write failure");
    CHECK(!result.restored,
          "a refused register cannot be restored and the record says so");
    CHECK(pci.regs[2] == 0U,
          "the SuperSpeed register that could be restored is back");
}

static void test_a_lying_read_back_restores_the_previous_routing(void) {
    struct fake_pci pci = production_state();

    pci.regs[0] = 0x00000001U;
    pci.regs[2] = 0x00000002U;
    pci.drop_on_write = 0x00000002U;
    pci.drop_once = true;
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_FAIL_READBACK,
          "a read-back that does not hold the written value is a failure");
    CHECK(result.restored, "the previous routing is restored");
    CHECK(pci.regs[0] == 0x00000001U && pci.regs[2] == 0x00000002U,
          "the lying register is put back to its previous value");
}

static void test_a_persistent_lie_is_reported_as_an_unrestored_register(void) {
    struct fake_pci pci = production_state();

    pci.regs[0] = 0x00000001U;
    pci.regs[2] = 0x00000002U;
    pci.drop_on_write = 0x00000002U;
    pci.drop_once = false;
    const struct xhci_route_ops ops = fake_ops();
    struct xhci_route_result result;

    xhci_route_ports(&ops, &pci, 0U, 0x14U, 0U, 0x8086U, 0x1E31U, &result);
    CHECK(result.outcome == XHCI_ROUTE_FAIL_READBACK,
          "a persistent lie stays a read-back failure");
    CHECK(!result.restored,
          "a register that cannot hold the old value is reported unrestored");
    CHECK(pci.regs[0] == 0x00000001U,
          "the register whose write never started is untouched");
}

int main(void) {
    test_the_measured_state_is_routed();
    test_reserved_bits_are_preserved();
    test_a_missing_participant_is_a_named_refusal();
    test_the_medium_controller_must_be_the_documented_one();
    test_an_xhci_outside_d20f0_is_not_the_rule();
    test_other_controllers_are_evidence_only();
    test_an_unreadable_register_or_mask_is_refused();
    test_zero_masks_have_nothing_to_route();
    test_a_refused_write_restores_the_previous_routing();
    test_a_persistent_refusal_is_reported();
    test_a_lying_read_back_restores_the_previous_routing();
    test_a_persistent_lie_is_reported_as_an_unrestored_register();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("xHCI route host test: ALL OK\n");
    return 0;
}
