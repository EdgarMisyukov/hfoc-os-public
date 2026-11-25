/*
 * ITEM-563 host proof: the real root-port read path, PORTSC address to walk.
 *
 * The hardware run reported `ports=2 connected=0` on both USB 2 controllers
 * while the saved Linux capture of the same machine has a hub on the EHCI bus.
 * `connected=0` was decided from one PORTSC read, before anything powered the
 * port, with no debounce window, and the value was not kept.  This proof drives
 * the production probe (kernel/src/ehci_port.c) over a substituted MMIO window
 * that is laid out like the controller's own: CAPLENGTH = 0x20 at BAR+0, the
 * operational registers after it, PORTSC of port N at
 * BAR + CAPLENGTH + 0x44 + (N - 1) * 4.
 *
 * It requires, in order:
 *
 *   * the address: a connected value placed at BAR + 0x44 (what a reader that
 *     forgot CAPLENGTH would fetch) is not what the probe reports;
 *   * two ports with different values are read as different ports;
 *   * a port that is unpowered and only reveals its connect after the power
 *     and the debounce step is connected after the probe, with both reads kept;
 *   * a port with nothing on it is empty only after the bounded window was
 *     spent, and says so;
 *   * the walk, driven by the same probe, finds the device the old single read
 *     could not see.
 */
#include "ehci.h"
#include "ehci_port.h"
#include "ehci_walk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* A controller window: capability dwords, then the operational registers. */
#define FAKE_WORDS 64U

struct fake_mmio {
    uint32_t words[FAKE_WORDS];
    uint32_t writes;
    uint32_t last_write_offset;
    uint32_t last_write_value;
    uint32_t delays;
    uint32_t delayed_us;
    /* Reveal the connect only after this many settle steps (0: immediately). */
    uint32_t reveal_after_steps;
    uint32_t steps;
    uint32_t reveal_portsc;
    uint32_t portsc_offset;
};

static struct fake_mmio *fake_mmio_from(void *context) {
    return context;
}

static uint32_t fake_read(void *context, uint32_t offset) {
    struct fake_mmio *fake = fake_mmio_from(context);
    const uint32_t index = offset / 4U;

    if (index >= FAKE_WORDS) {
        return 0U;
    }
    if (offset == fake->portsc_offset) {
        /* A port that was powered by the probe reveals its device later, so a
           single read taken before the window would miss it. */
        if (fake->reveal_portsc != 0U && fake->writes != 0U) {
            if (fake->steps < fake->reveal_after_steps) {
                return fake->words[index] & ~EHCI_PORTSC_CCS;
            }
            return fake->reveal_portsc;
        }
    }
    return fake->words[index];
}

static void fake_write(void *context, uint32_t offset, uint32_t value) {
    struct fake_mmio *fake = fake_mmio_from(context);

    ++fake->writes;
    fake->last_write_offset = offset;
    fake->last_write_value = value;
    if (offset / 4U < FAKE_WORDS) {
        fake->words[offset / 4U] = value;
    }
}

static void fake_delay(void *context, uint32_t microseconds) {
    struct fake_mmio *fake = fake_mmio_from(context);

    ++fake->delays;
    fake->delayed_us += microseconds;
    if (microseconds == EHCI_PORT_SETTLE_STEP_US) {
        ++fake->steps;
    }
}

static struct ehci_port_ops fake_ops(struct fake_mmio *fake) {
    struct ehci_port_ops ops;

    ops.context = fake;
    ops.read = fake_read;
    ops.write = fake_write;
    ops.delay_us = fake_delay;
    return ops;
}

static void fake_portsc(struct fake_mmio *fake, uint32_t port,
                        uint32_t portsc) {
    const uint32_t offset = ehci_port_offset(0x20U, port);

    fake->words[offset / 4U] = portsc;
    fake->portsc_offset = offset;
}

/* --- the walk's transport, driven by the production probe ---------------- */

struct port_walk {
    struct fake_mmio *fake;
    uint8_t caplength;
    bool ppc;
    uint32_t probe_count;
    struct ehci_port_state last[EHCI_PORT_RECORDS];
};

static bool port_walk_read(void *context, uint32_t port, uint32_t *portsc) {
    struct port_walk *walk = context;
    const struct ehci_port_ops ops = fake_ops(walk->fake);
    struct ehci_port_state state;

    if (port == 0U || portsc == NULL) {
        return false;
    }
    if (!ehci_port_probe(&ops, walk->caplength, port, walk->ppc, &state)) {
        return false;
    }
    if (port <= EHCI_PORT_RECORDS) {
        walk->last[port - 1U] = state;
    }
    ++walk->probe_count;
    *portsc = state.portsc;
    return true;
}

static bool port_walk_reset(void *context, uint32_t port, uint32_t *portsc) {
    struct port_walk *walk = context;
    const struct ehci_port_ops ops = fake_ops(walk->fake);

    (void)port;
    (void)ops;
    /* The reset enables the device the probe settled on. */
    *portsc = walk->last[port - 1U].portsc | EHCI_PORTSC_PE;
    return true;
}

/* The device behind the root port answers as a mass-storage device. */
struct device_model {
    uint32_t addressed;
    bool attached;
};

static bool device_control(void *context, uint8_t address, uint16_t max_packet,
                           uint8_t request_type, uint8_t request,
                           uint16_t value, uint16_t index, uint16_t length,
                           bool data_in, const void *out_data, void *in_data,
                           uint32_t *moved) {
    struct device_model *model = context;
    uint8_t *buffer = in_data;

    (void)address;
    (void)max_packet;
    (void)index;
    (void)out_data;
    if (moved != NULL) {
        *moved = 0U;
    }
    if (request_type == 0x00U && request == 0x05U) {
        model->addressed = value;
        return true;
    }
    if (request_type == 0x80U && request == 0x06U && data_in &&
        buffer != NULL) {
        memset(buffer, 0, length);
        if (length >= 8U) {
            buffer[0] = 18U;
            buffer[1] = 0x01U;
            buffer[2] = 0x00U;
            buffer[3] = 0x02U;
            buffer[4] = 0U; /* mass storage, not a hub */
            buffer[7] = 64U;
        }
        if (length >= 18U) {
            buffer[8] = 0x34U;
            buffer[9] = 0x12U;
            buffer[10] = 0x01U;
            buffer[11] = 0x00U;
        }
        if (moved != NULL) {
            *moved = length;
        }
        return true;
    }
    return true;
}

static bool device_begin(void *context, uint8_t address, uint16_t max_packet,
                         uint32_t root_port, uint8_t hub_address,
                         uint8_t hub_port, uint16_t vendor_id,
                         uint16_t product_id) {
    (void)context;
    (void)address;
    (void)max_packet;
    (void)root_port;
    (void)vendor_id;
    (void)product_id;
    return hub_address == 0U && hub_port == 0U;
}

static bool device_attach(void *context) {
    struct device_model *model = context;

    model->attached = true;
    return true;
}

static bool device_verify(void *context) {
    (void)context;
    return true;
}

static enum ehci_walk_port_state hub_bring_up(void *context,
                                              uint8_t hub_address,
                                              uint16_t hub_max_packet,
                                              uint8_t port,
                                              uint8_t power_good_ms,
                                              uint16_t *status,
                                              uint16_t *change,
                                              const char **verdict) {
    (void)context;
    (void)hub_address;
    (void)hub_max_packet;
    (void)port;
    (void)power_good_ms;
    if (status != NULL) {
        *status = 0U;
    }
    if (change != NULL) {
        *change = 0U;
    }
    if (verdict != NULL) {
        *verdict = NULL;
    }
    return EHCI_WALK_PORT_EMPTY;
}

static void no_delay(void *context, uint32_t microseconds) {
    (void)context;
    (void)microseconds;
}

static struct ehci_walk_ops walk_ops(struct port_walk *walk,
                                     struct device_model *model) {
    struct ehci_walk_ops ops = {0};

    ops.context = walk;
    ops.control = device_control;
    ops.root_port_read = port_walk_read;
    ops.root_port_reset = port_walk_reset;
    ops.hub_port_bring_up = hub_bring_up;
    ops.device_begin = device_begin;
    ops.attach_storage = device_attach;
    ops.verify_medium = device_verify;
    ops.delay_us = no_delay;
    ops.last_failure = NULL;
    (void)model;
    return ops;
}

/* --- cases --------------------------------------------------------------- */

static void test_the_portsc_address_uses_the_controllers_caplength(void) {
    struct fake_mmio fake;
    const struct ehci_port_ops ops = fake_ops(&fake);
    struct ehci_port_state state;

    memset(&fake, 0, sizeof(fake));
    /* What a reader without CAPLENGTH would fetch: a plausible connected word
       at BAR + 0x44.  It must never be what the probe reports. */
    fake.words[0x44U / 4U] = EHCI_PORTSC_CCS | EHCI_PORTSC_PE;
    fake_portsc(&fake, 1U, EHCI_PORTSC_CCS | EHCI_PORTSC_PE | EHCI_PORTSC_PP);

    CHECK(ehci_port_offset(0x20U, 1U) == 0x64U,
          "port 1 lives at CAPLENGTH + 0x44");
    CHECK(ehci_port_offset(0x20U, 2U) == 0x68U,
          "port 2 lives one dword further");
    CHECK(ehci_port_offset(0x00U, 1U) == 0x44U,
          "a zero CAPLENGTH would read the wrong dword, which this proof pins");

    CHECK(ehci_port_probe(&ops, 0x20U, 1U, false, &state),
          "a readable port probes");
    CHECK(state.raw_first == (EHCI_PORTSC_CCS | EHCI_PORTSC_PE |
                              EHCI_PORTSC_PP),
          "the probe read the port's own PORTSC, not the dword at BAR + 0x44");
    CHECK(state.connected && state.connected_first,
          "the connected word is reported as connected");
    CHECK(state.waited_us == 0U && !state.powered,
          "a connected, powered port needs neither a wait nor a power write");
}

static void test_two_ports_read_differently(void) {
    struct fake_mmio fake;
    const struct ehci_port_ops ops = fake_ops(&fake);
    struct ehci_port_state first;
    struct ehci_port_state second;

    memset(&fake, 0, sizeof(fake));
    fake_portsc(&fake, 1U, EHCI_PORTSC_CCS | EHCI_PORTSC_PE | EHCI_PORTSC_PP);
    fake.words[ehci_port_offset(0x20U, 2U) / 4U] = EHCI_PORTSC_PP;

    CHECK(ehci_port_probe(&ops, 0x20U, 1U, false, &first) &&
              ehci_port_probe(&ops, 0x20U, 2U, false, &second),
          "both ports probe");
    CHECK(first.connected && !second.connected,
          "two ports with different words are told apart");
    CHECK(second.waited_us == EHCI_PORT_SETTLE_WINDOW_US &&
              second.settle_full,
          "the empty port is empty only after the whole window was spent");
}

static void test_an_unpowered_port_is_powered_before_it_is_called_empty(void) {
    struct fake_mmio fake;
    const struct ehci_port_ops ops = fake_ops(&fake);
    struct ehci_port_state state;

    memset(&fake, 0, sizeof(fake));
    /* The port comes out of reset unpowered and its device appears after the
       power-good interval and one debounce step. */
    fake_portsc(&fake, 1U, 0U);
    fake.reveal_after_steps = 1U;
    fake.reveal_portsc = EHCI_PORTSC_CCS | EHCI_PORTSC_PE | EHCI_PORTSC_PP;

    CHECK(ehci_port_probe(&ops, 0x20U, 1U, true, &state),
          "an unpowered port still probes");
    CHECK(state.raw_first == 0U && !state.connected_first,
          "the first read says nothing is connected, which is why it cannot "
          "decide the port is empty");
    CHECK(state.powered,
          "the probe switched the port on because HCSPARAMS says software owns "
          "power");
    CHECK(fake.writes == 1U &&
              fake.last_write_offset == ehci_port_offset(0x20U, 1U) &&
              (fake.last_write_value & EHCI_PORTSC_PP) != 0U,
          "the power write set PP at the port's own offset");
    CHECK(state.connected,
          "the device the first read could not see is found after power and "
          "debounce");
    CHECK(state.waited_us == EHCI_PORT_SETTLE_STEP_US,
          "exactly one debounce step was needed and it was recorded");
}

static void test_a_port_with_nothing_on_it_is_empty_by_measurement(void) {
    struct fake_mmio fake;
    const struct ehci_port_ops ops = fake_ops(&fake);
    struct ehci_port_state state;

    memset(&fake, 0, sizeof(fake));
    fake_portsc(&fake, 2U, EHCI_PORTSC_PP);

    CHECK(ehci_port_probe(&ops, 0x20U, 2U, false, &state), "the port probes");
    CHECK(!state.connected && state.settle_full &&
              state.waited_us == EHCI_PORT_SETTLE_WINDOW_US,
          "a truly empty port says how long it was watched, not just zero");
    CHECK(state.raw_first == EHCI_PORTSC_PP &&
              state.portsc == EHCI_PORTSC_PP,
          "both reads are kept, so a reader can compare them");
    CHECK(fake.writes == 0U,
          "a controller that does not own port power is never written to");
}

static void test_the_power_write_actually_sets_pp(void) {
    CHECK(ehci_portsc_power_value(0U) == EHCI_PORTSC_PP,
          "powering an off port sets PP");
    CHECK((ehci_portsc_power_value(EHCI_PORTSC_CSC) & EHCI_PORTSC_CSC) != 0U,
          "the change bits are written back, as every other port write does");
    CHECK((ehci_portsc_power_value(EHCI_PORTSC_PP) & EHCI_PORTSC_PP) != 0U,
          "an already powered port stays powered");
    CHECK((ehci_portsc_write_value(0U, EHCI_PORTSC_PP) & EHCI_PORTSC_PP) == 0U,
          "the helper the driver used before this item wrote no PP at all, "
          "which is why no port was ever powered");
}

static void test_the_walk_sees_the_device_a_single_read_missed(void) {
    struct fake_mmio fake;
    struct port_walk walk;
    struct device_model model;
    struct ehci_walk_report report;
    struct ehci_walk_ops ops = {0};

    memset(&fake, 0, sizeof(fake));
    memset(&model, 0, sizeof(model));
    memset(&walk, 0, sizeof(walk));
    /* Port 1 is unpowered and reveals the stick only after the window; port 2
       is powered and truly empty.  Exactly one device is on the controller. */
    fake_portsc(&fake, 1U, 0U);
    fake.words[ehci_port_offset(0x20U, 1U) / 4U] = 0U;
    fake.reveal_after_steps = 2U;
    fake.reveal_portsc = EHCI_PORTSC_CCS | EHCI_PORTSC_PE | EHCI_PORTSC_PP;
    walk.fake = &fake;
    walk.caplength = 0x20U;
    walk.ppc = true;
    ops = walk_ops(&walk, &model);

    CHECK(ehci_walk_run(&ops, 2U, &report),
          "the walk finds the device the old single read called absent");
    CHECK(report.selected == 1 && report.attached == 1U,
          "the device on the powered port was attached and accepted");
    CHECK(walk.probe_count == 1U,
          "the walk stopped probing once a device was accepted");
    CHECK(walk.last[0].connected && walk.last[0].powered &&
              walk.last[0].waited_us ==
                  fake.reveal_after_steps * EHCI_PORT_SETTLE_STEP_US,
          "the working port's own evidence says it was powered and waited for");
}

static void test_an_empty_controller_is_measured_port_by_port(void) {
    struct fake_mmio fake;
    struct port_walk walk;
    struct device_model model;
    struct ehci_walk_report report;
    struct ehci_walk_ops ops = {0};

    memset(&fake, 0, sizeof(fake));
    memset(&model, 0, sizeof(model));
    memset(&walk, 0, sizeof(walk));
    fake_portsc(&fake, 1U, EHCI_PORTSC_PP);
    fake.words[ehci_port_offset(0x20U, 2U) / 4U] = 0U;
    walk.fake = &fake;
    walk.caplength = 0x20U;
    walk.ppc = true;
    ops = walk_ops(&walk, &model);

    CHECK(!ehci_walk_run(&ops, 2U, &report),
          "a controller with nothing on either port finds no medium");
    CHECK(walk.probe_count == 2U,
          "every root port of an empty controller was probed");
    CHECK(!walk.last[0].connected && walk.last[0].settle_full &&
              walk.last[1].connected_first == false &&
              walk.last[1].settle_full,
          "both empty ports say they were watched for the whole window");
    CHECK(walk.last[1].powered,
          "the port that read unpowered was switched on before the verdict");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_NO_PORT &&
              strcmp(report.failure, "NO_CONNECTED_PORT") == 0,
          "the walk names an empty controller from measured ports, not from a "
          "single zero");
}

int main(void) {
    test_the_portsc_address_uses_the_controllers_caplength();
    test_two_ports_read_differently();
    test_an_unpowered_port_is_powered_before_it_is_called_empty();
    test_a_port_with_nothing_on_it_is_empty_by_measurement();
    test_the_power_write_actually_sets_pp();
    test_the_walk_sees_the_device_a_single_read_missed();
    test_an_empty_controller_is_measured_port_by_port();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI root port read path host test: ALL OK\n");
    return 0;
}
