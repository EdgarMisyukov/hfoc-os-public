/*
 * ITEM-556: the EHCI walk the driver runs, over a substituted transport.
 *
 * The logic here is the driver's own: the root-port scan, the address sequence
 * (eight-byte descriptor, SET_ADDRESS, eighteen-byte descriptor), the hub's
 * configuration and class-descriptor request, the downstream-port walk, the
 * attachment of every mass-storage device and the boot-medium guard, including
 * the rule that a rejected device does not end the search.  What it does not
 * have is MMIO: every register and transfer goes through `struct
 * ehci_walk_ops`, which the driver fills with its controller and a host proof
 * fills with a scripted model.
 */

#include "ehci_walk.h"

#include <stddef.h>

#define WALK_DEVICE8 8U
#define WALK_DEVICE_DESCRIPTOR_BYTES 18U
#define WALK_MAX_ADDRESS 7U
#define WALK_MAX_HUB_PORTS 15U

struct ehci_walk_state {
    const struct ehci_walk_ops *ops;
    struct ehci_walk_report *report;
    uint32_t port;
    uint32_t portsc;
    uint8_t hub_address;
    uint8_t hub_port;
    uint16_t hub_status;
    uint16_t hub_change;
    const char *hub_verdict;
    uint8_t next_address;
    bool hub_seen;
};

/*
 * ITEM-558: a stage is not a cause.  Every refusal records both - the stage the
 * walk stopped at and the transport's own text for the transfer that failed -
 * so a reader can tell "DEVICE8" from "DEVICE8, and the transfer said
 * result=XACTERR qtd=0x00000048".  The text comes from the transport and lives
 * as long as it does; the walk only borrows it.
 */
static const char *walk_transport_text(const struct ehci_walk_state *state) {
    return state->ops->last_failure != NULL
               ? state->ops->last_failure(state->ops->context)
               : NULL;
}

/*
 * ITEM-562: one connected port's path, recorded where it ended.  Every stop is
 * kept - not just the last one - so a port that failed at its reset and a port
 * that failed at a descriptor are both in the caller's record, in walk order,
 * and the first concrete refusal can be named without being overwritten.
 */
static void walk_stop(struct ehci_walk_state *state,
                      enum ehci_walk_outcome outcome,
                      const char *stage) {
    struct ehci_walk_report *report = state->report;

    ++report->failures;
    if (report->port_count < EHCI_WALK_PORT_RECORDS) {
        struct ehci_walk_port *record = &report->ports[report->port_count++];

        record->port = state->port;
        record->portsc = state->portsc;
        record->hub_address = state->hub_address;
        record->hub_port = state->hub_port;
        record->hub_status = state->hub_status;
        record->hub_change = state->hub_change;
        record->hub_verdict = state->hub_verdict;
        record->outcome = outcome;
        record->stage = stage;
        record->detail = walk_transport_text(state);
    }
}

/* The named stage time (a descriptor or a hub request) still names its cause. */
static void walk_fail(struct ehci_walk_state *state,
                      enum ehci_walk_outcome outcome,
                      const char *stage) {
    walk_stop(state, outcome, stage);
}

static bool walk_read_descriptor(struct ehci_walk_state *state,
                                 uint8_t address,
                                 uint16_t max_packet,
                                 uint8_t type,
                                 uint16_t length,
                                 void *buffer,
                                 uint32_t *received) {
    return state->ops->control(state->ops->context, address, max_packet,
                               0x80U, 0x06U,
                               (uint16_t)((uint16_t)type << 8U), 0U, length,
                               true, NULL, buffer, received);
}

static bool walk_set_configuration(struct ehci_walk_state *state,
                                   uint8_t address,
                                   uint16_t max_packet,
                                   uint16_t value) {
    return state->ops->control(state->ops->context, address, max_packet, 0x00U,
                               0x09U, value, 0U, 0U, false, NULL, NULL, NULL);
}

/*
 * Address the device a port reset has just enabled and read its own
 * descriptor.  A device behind a hub is reached the same way: for a high-speed
 * hub the device's own address is what the queue head names.
 */
static bool walk_address_and_read(struct ehci_walk_state *state,
                                  uint8_t *address_out,
                                  uint8_t *class_out,
                                  uint16_t *max_packet_out,
                                  uint16_t *vendor_out,
                                  uint16_t *product_out) {
    uint8_t descriptor[WALK_DEVICE_DESCRIPTOR_BYTES];
    uint16_t max_packet = 64U;
    uint32_t received = 0U;
    uint8_t address;

    if (state->next_address == 0U) {
        state->next_address = 1U;
    }
    for (size_t index = 0U; index < sizeof(descriptor); ++index) {
        descriptor[index] = 0U;
    }
    if (!walk_read_descriptor(state, 0U, max_packet, 0x01U, WALK_DEVICE8,
                              descriptor, &received) ||
        received < WALK_DEVICE8 || descriptor[7] == 0U) {
        walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "DEVICE8");
        return false;
    }
    max_packet = descriptor[7];
    if (max_packet < 8U || max_packet > 64U) {
        walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "EP0_MPS");
        return false;
    }
    address = state->next_address;
    if (address >= WALK_MAX_ADDRESS) {
        walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "OUT_OF_ADR");
        return false;
    }
    state->next_address = (uint8_t)(address + 1U);
    if (!state->ops->control(state->ops->context, 0U, max_packet, 0x00U,
                             0x05U, address, 0U, 0U, false, NULL, NULL,
                             NULL)) {
        walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "SET_ADDRESS");
        return false;
    }
    /* USB 2.0 section 9.2.6.3: the device is given 2 ms to recover before the
       next transfer.  Skipping it makes the first read at the new address a
       race against the device's own state machine. */
    state->ops->delay_us(state->ops->context, EHCI_WALK_ADDRESS_RECOVERY_US);
    for (size_t index = 0U; index < sizeof(descriptor); ++index) {
        descriptor[index] = 0U;
    }
    if (!walk_read_descriptor(state, address, max_packet, 0x01U,
                              WALK_DEVICE_DESCRIPTOR_BYTES, descriptor,
                              &received) ||
        received < WALK_DEVICE_DESCRIPTOR_BYTES) {
        walk_fail(state, EHCI_WALK_OUTCOME_TRANSFER, "DEVICE18");
        return false;
    }
    *address_out = address;
    *class_out = descriptor[4];
    *max_packet_out = max_packet;
    *vendor_out = (uint16_t)((uint16_t)descriptor[8] |
                             ((uint16_t)descriptor[9] << 8U));
    *product_out = (uint16_t)((uint16_t)descriptor[10] |
                              ((uint16_t)descriptor[11] << 8U));
    return true;
}

/* One storage device: hand it to the storage stack and ask the boot-medium
   guard about it.  The guard's answer is the only thing that ends the walk. */
static bool walk_offer(struct ehci_walk_state *state,
                       uint8_t address,
                       uint16_t max_packet,
                       uint8_t hub_address,
                       uint8_t hub_port,
                       uint16_t vendor_id,
                       uint16_t product_id,
                       struct ehci_walk_report *report) {
    if (!state->ops->device_begin(state->ops->context, address, max_packet,
                                  state->port, hub_address, hub_port,
                                  vendor_id, product_id)) {
        walk_fail(state, EHCI_WALK_OUTCOME_ATTACH, "DEVICE_BEGIN");
        return false;
    }
    if (!state->ops->attach_storage(state->ops->context)) {
        /* The attach path names its own status; the walk names the stage. */
        walk_fail(state, EHCI_WALK_OUTCOME_ATTACH, "ATTACH");
        return false;
    }
    ++report->attached;
    ++report->probes;
    if (state->ops->verify_medium(state->ops->context)) {
        report->selected = (int)report->attached;
        report->selected_address = address;
        report->selected_max_packet = max_packet;
        report->selected_hub_address = hub_address;
        report->selected_hub_port = hub_port;
        walk_stop(state, EHCI_WALK_OUTCOME_OK, "MEDIUM");
        return true;
    }
    /* A device that is not the boot medium is not a fault: the walk keeps
       looking, and the report says how many devices it offered. */
    walk_stop(state, EHCI_WALK_OUTCOME_MEDIUM, "NO_MEDIUM");
    return false;
}

/* Configure the hub and walk its downstream ports, one level deep. */
static bool walk_hub(struct ehci_walk_state *state,
                     uint8_t hub_address,
                     uint16_t hub_max_packet,
                     struct ehci_walk_report *report) {
    uint8_t descriptor[EHCI_HUB_DESCRIPTOR_PREFIX];
    uint8_t ports;
    uint8_t power_good_ms;
    uint32_t received = 0U;
    uint8_t request_type = 0U;
    uint8_t request = 0U;
    uint16_t value = 0U;

    state->hub_seen = true;
    state->hub_address = hub_address;
    /* The hub's single configuration first (USB 2.0 section 11.24.2.5 reads the
       hub descriptor from a configured hub), then the class request. */
    if (!walk_set_configuration(state, hub_address, hub_max_packet, 1U)) {
        walk_fail(state, EHCI_WALK_OUTCOME_HUB, "HUB_CONFIG");
        return false;
    }
    ehci_hub_get_descriptor_fields(&request_type, &request, &value);
    for (size_t index = 0U; index < sizeof(descriptor); ++index) {
        descriptor[index] = 0U;
    }
    if (!state->ops->control(state->ops->context, hub_address, hub_max_packet,
                             request_type, request, value, 0U,
                             EHCI_HUB_DESCRIPTOR_PREFIX, true, NULL,
                             descriptor, &received) ||
        received < EHCI_HUB_DESCRIPTOR_PREFIX) {
        walk_fail(state, EHCI_WALK_OUTCOME_HUB, "HUB_DESC");
        return false;
    }
    ports = ehci_hub_port_count(descriptor, received);
    power_good_ms = ehci_hub_power_good_ms(descriptor, received);
    if (ports == 0U || ports > WALK_MAX_HUB_PORTS) {
        walk_fail(state, EHCI_WALK_OUTCOME_HUB, "HUB_PORTS");
        return false;
    }
    for (uint8_t port = 1U; port <= ports; ++port) {
        uint8_t address = 0U;
        uint8_t device_class = 0U;
        uint16_t max_packet = 0U;
        uint16_t vendor_id = 0U;
        uint16_t product_id = 0U;

        state->hub_address = hub_address;
        state->hub_port = port;
        state->portsc = 0U;
        state->hub_status = 0U;
        state->hub_change = 0U;
        state->hub_verdict = NULL;
        {
            const enum ehci_walk_port_state brought =
                state->ops->hub_port_bring_up(
                    state->ops->context, hub_address, hub_max_packet, port,
                    power_good_ms, &state->hub_status, &state->hub_change,
                    &state->hub_verdict);

            if (brought == EHCI_WALK_PORT_EMPTY) {
                /* Nothing is plugged into this hub port: not a refusal. */
                state->hub_address = 0U;
                state->hub_port = 0U;
                continue;
            }
            if (brought != EHCI_WALK_PORT_UP) {
                walk_fail(state, EHCI_WALK_OUTCOME_HUB, "HUB_PORT");
                state->hub_address = 0U;
                state->hub_port = 0U;
                continue;
            }
        }
        state->portsc = 0U;
        /* A hub port reset releases the device, and the device still needs its
           10 ms before it answers address 0. */
        state->ops->delay_us(state->ops->context,
                             EHCI_WALK_RESET_RECOVERY_US);
        if (!walk_address_and_read(state, &address, &device_class, &max_packet,
                                   &vendor_id, &product_id)) {
            continue;
        }
        if (device_class == EHCI_USB_CLASS_HUB) {
            /* One hub level: a second would need a repeat of this walk, and the
               machine does not present one. */
            continue;
        }
        if (walk_offer(state, address, max_packet, hub_address, port,
                       vendor_id, product_id, report)) {
            return true;
        }
    }
    return false;
}

/*
 * ITEM-562: which refusal the caller is told about.
 *
 * The walk may offer several devices and stop several ways.  The one that
 * matters is the first concrete refusal in walk order - a port that did not
 * reset, a hub port that did not come up, a transfer that failed, a device that
 * would not attach - because that is the earliest point the machine's path
 * actually ended.  A guard refusal is a "not this device" answer, kept only when
 * every port merely offered a device and none failed; and a machine with no
 * connected port says so instead of borrowing a descriptor's name.
 */
static void walk_finish(struct ehci_walk_report *report) {
    const struct ehci_walk_port *hard = NULL;
    const struct ehci_walk_port *medium = NULL;

    for (uint32_t index = 0U; index < report->port_count; ++index) {
        const struct ehci_walk_port *record = &report->ports[index];

        if (record->outcome == EHCI_WALK_OUTCOME_PORT_RESET ||
            record->outcome == EHCI_WALK_OUTCOME_HUB ||
            record->outcome == EHCI_WALK_OUTCOME_TRANSFER ||
            record->outcome == EHCI_WALK_OUTCOME_ATTACH) {
            hard = record;
            break;
        }
        if (record->outcome == EHCI_WALK_OUTCOME_MEDIUM && medium == NULL) {
            medium = record;
        }
    }
    if (hard != NULL) {
        report->outcome = hard->outcome;
        report->failure = hard->stage;
        report->failure_detail = hard->detail;
        report->failure_port = hard->port;
        report->failure_hub_address = hard->hub_address;
        report->failure_hub_port = hard->hub_port;
        return;
    }
    if (medium != NULL) {
        report->outcome = EHCI_WALK_OUTCOME_MEDIUM;
        report->failure = "NO_MEDIUM";
        report->failure_detail = medium->detail;
        report->failure_port = medium->port;
        report->failure_hub_address = medium->hub_address;
        report->failure_hub_port = medium->hub_port;
        return;
    }
    if (report->ports_connected == 0U) {
        report->outcome = EHCI_WALK_OUTCOME_NO_PORT;
        report->failure = "NO_CONNECTED_PORT";
        return;
    }
    /* Ports carried devices and none of them became storage. */
    report->outcome = EHCI_WALK_OUTCOME_NO_DEVICE;
    report->failure = "NO_STORAGE_DEVICE";
    report->failure_detail = NULL;
}

/* The EHCI status an outcome corresponds to (see the header). */
enum ehci_status ehci_walk_outcome_status(enum ehci_walk_outcome outcome) {
    switch (outcome) {
    case EHCI_WALK_OUTCOME_PORT_RESET:
    case EHCI_WALK_OUTCOME_HUB:
        return EHCI_STATUS_PORT_NOT_ENABLED;
    case EHCI_WALK_OUTCOME_TRANSFER:
        return EHCI_STATUS_DESCRIPTOR_FAILED;
    case EHCI_WALK_OUTCOME_MEDIUM:
        return EHCI_STATUS_MEDIUM_NOT_MATCHED;
    case EHCI_WALK_OUTCOME_NO_PORT:
    case EHCI_WALK_OUTCOME_NO_DEVICE:
        return EHCI_STATUS_NO_DEVICE;
    case EHCI_WALK_OUTCOME_ATTACH:
    case EHCI_WALK_OUTCOME_OK:
    case EHCI_WALK_OUTCOME_NONE:
        break;
    }
    return EHCI_STATUS_UNKNOWN;
}

bool ehci_walk_run(const struct ehci_walk_ops *ops,
                   uint32_t port_count,
                   struct ehci_walk_report *report) {
    struct ehci_walk_state state;

    if (ops == NULL || report == NULL || ops->control == NULL ||
        ops->root_port_read == NULL || ops->root_port_reset == NULL ||
        ops->hub_port_bring_up == NULL || ops->device_begin == NULL ||
        ops->attach_storage == NULL || ops->verify_medium == NULL ||
        ops->delay_us == NULL) {
        return false;
    }
    report->ports_connected = 0U;
    report->attached = 0U;
    report->probes = 0U;
    report->selected = -1;
    report->selected_address = 0U;
    report->selected_max_packet = 0U;
    report->selected_hub_address = 0U;
    report->selected_hub_port = 0U;
    report->port_count = 0U;
    report->failures = 0U;
    report->outcome = EHCI_WALK_OUTCOME_NONE;
    report->failure = NULL;
    report->failure_detail = NULL;
    report->failure_port = 0U;
    report->failure_hub_address = 0U;
    report->failure_hub_port = 0U;
    state.ops = ops;
    state.report = report;
    state.port = 0U;
    state.portsc = 0U;
    state.next_address = 1U;
    state.hub_address = 0U;
    state.hub_port = 0U;
    state.hub_seen = false;

    for (uint32_t port = 1U; port <= port_count; ++port) {
        uint32_t portsc = 0U;
        uint8_t address = 0U;
        uint8_t device_class = 0U;
        uint16_t max_packet = 0U;
        uint16_t vendor_id = 0U;
        uint16_t product_id = 0U;

        state.port = port;
        state.portsc = 0U;
        state.hub_address = 0U;
        state.hub_port = 0U;
        if (ops->port_skip != NULL &&
            ops->port_skip(ops->context, port)) {
            continue;
        }
        if (!ops->root_port_read(ops->context, port, &portsc)) {
            continue;
        }
        if ((portsc & EHCI_PORTSC_CCS) == 0U) {
            continue;
        }
        state.portsc = portsc;
        ++report->ports_connected;
        if (!ops->root_port_reset(ops->context, port, &portsc)) {
            /* A port that carried a device and did not enable is a refusal of
               its own: it must not be reported as a descriptor failure later. */
            state.portsc = portsc;
            walk_fail(&state, EHCI_WALK_OUTCOME_PORT_RESET, "ROOT_RESET");
            continue;
        }
        state.portsc = portsc;
        /* USB 2.0 section 7.1.7.5: TRSTRCY, 10 ms after the reset before the
           first SETUP.  The controller reports the port enabled before the
           device is ready; the machine's `DEVICE8` refusal is what a missing
           wait looks like on hardware, and QEMU's instant answers hid it. */
        ops->delay_us(ops->context, EHCI_WALK_RESET_RECOVERY_US);
        if (!walk_address_and_read(&state, &address, &device_class,
                                   &max_packet, &vendor_id, &product_id)) {
            continue;
        }
        if (device_class == EHCI_USB_CLASS_HUB) {
            if (walk_hub(&state, address, max_packet, report)) {
                report->outcome = EHCI_WALK_OUTCOME_OK;
                return true;
            }
            continue;
        }
        if (walk_offer(&state, address, max_packet, 0U, 0U, vendor_id,
                       product_id, report)) {
            report->outcome = EHCI_WALK_OUTCOME_OK;
            return true;
        }
    }
    walk_finish(report);
    return false;
}
