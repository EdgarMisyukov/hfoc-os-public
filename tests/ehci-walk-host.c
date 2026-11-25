/*
 * ITEM-556 host proof: the production EHCI walk, through a scripted transport.
 *
 * Every hardware run of the export failed inside this walk, not inside an
 * encoder: the machine hangs its USB 2.0 ports behind an Intel rate matching
 * hub, and a walk that stops one device early (a card reader accepted before
 * the boot stick) never reaches the medium.  Classifying status words cannot
 * see that.  So this proof runs the driver's own walk
 * (kernel/src/ehci_walk.c) over a model of the machine:
 *
 *   root port 1 -> high-speed hub (the rate matching hub)
 *                   port 1 -> a foreign mass-storage device
 *                   port 2 -> the boot stick
 *
 * and asserts what the walk actually did, not what it would be nice for it to
 * do: the first device was attached and then rejected by the boot-medium guard,
 * the walk continued inside the same hub, the second device was attached and
 * accepted, the report counts two attachments and two probes, and the selected
 * device is the second one - the only device the guard accepted, which is what
 * the save path writes to.
 */
#include "ehci.h"
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

enum mock_device {
    MOCK_NONE = 0,
    MOCK_HUB = 1,
    MOCK_FOREIGN = 2,
    MOCK_STICK = 3
};

struct mock_attempt {
    uint8_t address;
    uint8_t hub_address;
    uint8_t hub_port;
    uint32_t root_port;
    uint16_t max_packet;
    uint16_t vendor_id;
    uint16_t product_id;
};

struct mock {
    enum mock_device pending;
    enum mock_device addressed[8];
    enum mock_device last_device;
    struct mock_attempt attempts[4];
    uint32_t attempt_count;
    uint32_t attach_count;
    uint32_t probe_count;
    bool verify_answer[4];
    uint32_t hub_ports;
    bool hub_empty[16];
    bool hub_refused[16];
    /* ITEM-570: the raw wPortStatus/wPortChange the hub sends for a port, and
       the device behind it.  When a status word is set the mock classifies it
       with the production ehci_hub_port_verdict(), exactly as the driver does;
       otherwise the old empty/refused flags still drive the state. */
    uint16_t hub_status[16];
    uint16_t hub_change[16];
    bool hub_status_set[16];
    enum mock_device hub_device[16];
    bool root_refuse[4];
    uint8_t hub_power_good_ms;
    /* What the hub's class descriptor request actually carried. */
    uint32_t class_request_type;
    uint32_t class_request_request;
    uint32_t class_request_value;
    bool hub_configured;
    bool class_before_config;
    uint32_t root_ports;
    bool root_connected[4];
    enum mock_device root_device[4];
    uint16_t hub_max_packet;
    /* ITEM-557: the waits the walk asked the transport for, in order. */
    uint32_t delay_us[16];
    uint32_t delay_count;
    /* ITEM-558: the first descriptor read can be refused, and the transport
       then has a primary cause to hand the walk. */
    bool fail_device8;
    /* ITEM-562: which device refuses its first descriptor, so a machine with a
       failing port and a working one can be walked in the same proof. */
    enum mock_device fail_device8_on;
    const char *failure_text;
    uint32_t failure_calls;
    /* The same calls as events, so the proof can assert that a wait comes
       between a reset (or SET_ADDRESS) and the transfer that follows it. */
    enum mock_event {
        MOCK_EVENT_RESET,
        MOCK_EVENT_ADDRESS,
        MOCK_EVENT_DELAY
    } events[32];
    uint32_t event_count;
};

static void mock_event_push(struct mock *mock, enum mock_event event) {
    if (mock->event_count <
        sizeof(mock->events) / sizeof(mock->events[0])) {
        mock->events[mock->event_count++] = event;
    }
}

static void fill_device_descriptor(enum mock_device device,
                                   uint8_t max_packet,
                                   uint8_t *buffer,
                                   size_t length) {
    uint16_t vendor = 0U;
    uint16_t product = 0U;
    uint8_t device_class = 0U;

    memset(buffer, 0, length);
    if (length < 8U) {
        return;
    }
    switch (device) {
    case MOCK_HUB:
        device_class = EHCI_USB_CLASS_HUB;
        vendor = 0x8087U; /* the Intel rate matching hub */
        product = 0x0024U;
        break;
    case MOCK_FOREIGN:
        device_class = 0U;
        vendor = 0xDEADU;
        product = 0x0001U;
        break;
    case MOCK_STICK:
        device_class = 0U;
        vendor = 0xABCDU;
        product = 0x0002U;
        break;
    case MOCK_NONE:
    default:
        return;
    }
    buffer[0] = 18U;
    buffer[1] = 0x01U;
    buffer[2] = 0x00U;
    buffer[3] = 0x02U;
    buffer[4] = device_class;
    buffer[7] = max_packet;
    if (length >= 18U) {
        buffer[8] = (uint8_t)(vendor & 0xFFU);
        buffer[9] = (uint8_t)(vendor >> 8U);
        buffer[10] = (uint8_t)(product & 0xFFU);
        buffer[11] = (uint8_t)(product >> 8U);
    }
}

static bool mock_control(void *context,
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
    struct mock *mock = context;

    (void)max_packet;
    (void)index;
    (void)out_data;
    if (moved != NULL) {
        *moved = 0U;
    }
    if (request_type == 0x00U && request == 0x05U) {
        /* SET_ADDRESS: the device that answered address 0 takes the new one. */
        mock_event_push(mock, MOCK_EVENT_ADDRESS);
        if (address != 0U || mock->pending == MOCK_NONE || value < 1U ||
            value >= 8U) {
            return false;
        }
        mock->addressed[value] = mock->pending;
        mock->pending = MOCK_NONE;
        return true;
    }
    if (request_type == 0x00U && request == 0x09U) {
        if (mock->addressed[address] == MOCK_HUB) {
            mock->hub_configured = true;
        }
        return true;
    }
    if (request_type == 0xA0U && request == 0x06U && value == 0x2900U) {
        uint8_t descriptor[EHCI_HUB_DESCRIPTOR_PREFIX];

        mock->class_request_type = request_type;
        mock->class_request_request = request;
        mock->class_request_value = value;
        if (!mock->hub_configured) {
            mock->class_before_config = true;
        }
        if (address == 0U || mock->addressed[address] != MOCK_HUB ||
            !data_in || length < EHCI_HUB_DESCRIPTOR_PREFIX ||
            in_data == NULL) {
            return false;
        }
        memset(descriptor, 0, sizeof(descriptor));
        descriptor[0] = 9U;
        descriptor[1] = EHCI_USB_DESCRIPTOR_HUB;
        descriptor[2] = (uint8_t)mock->hub_ports;
        descriptor[5] = (uint8_t)(mock->hub_power_good_ms / 2U);
        memcpy(in_data, descriptor, EHCI_HUB_DESCRIPTOR_PREFIX);
        if (moved != NULL) {
            *moved = EHCI_HUB_DESCRIPTOR_PREFIX;
        }
        return true;
    }
    if (request_type == 0x80U && request == 0x06U) {
        enum mock_device device;
        size_t want = length;

        if (mock->fail_device8 && address == 0U &&
            (mock->fail_device8_on == MOCK_NONE ||
             mock->fail_device8_on == mock->pending)) {
            /* The controller refused the very first eight-byte descriptor read
               of the device the port reset enabled. */
            return false;
        }

        if (address == 0U) {
            device = mock->pending;
        } else if (address < 8U) {
            device = mock->addressed[address];
        } else {
            return false;
        }
        if (device == MOCK_NONE || in_data == NULL || !data_in) {
            return false;
        }
        if (want > 18U) {
            want = 18U;
        }
        fill_device_descriptor(device, device == MOCK_HUB ? 64U : 32U,
                               in_data, want);
        mock->last_device = device;
        if (moved != NULL) {
            *moved = (uint32_t)want;
        }
        return true;
    }
    return false;
}

static bool mock_root_read(void *context, uint32_t port, uint32_t *portsc) {
    struct mock *mock = context;

    if (port == 0U || port > mock->root_ports || portsc == NULL) {
        return false;
    }
    *portsc = mock->root_connected[port] ? EHCI_PORTSC_CCS : 0U;
    return true;
}

static bool mock_root_reset(void *context, uint32_t port, uint32_t *portsc) {
    struct mock *mock = context;

    if (port == 0U || port > mock->root_ports || portsc == NULL) {
        return false;
    }
    if (mock->root_refuse[port]) {
        /* A connected port that does not enable (a companion-owned device, or
           a reset the controller never completed). */
        *portsc = EHCI_PORTSC_CCS;
        return false;
    }
    /* The device a root port reset enables answers at address 0 next. */
    mock_event_push(mock, MOCK_EVENT_RESET);
    mock->pending = mock->root_device[port];
    *portsc = EHCI_PORTSC_CCS | EHCI_PORTSC_PE | EHCI_PORTSC_PS_HIGH;
    return true;
}

/*
 * ITEM-562: a hub port is empty, up or refused - three answers, because an
 * empty port on the machine's rate matching hub is a normal machine and must
 * not be recorded as a fault.
 */
static enum ehci_walk_port_state mock_hub_bring_up(void *context,
                                                   uint8_t hub_address,
                                                   uint16_t hub_max_packet,
                                                   uint8_t port,
                                                   uint8_t power_good_ms,
                                                   uint16_t *status,
                                                   uint16_t *change,
                                                   const char **verdict) {
    struct mock *mock = context;

    if (status != NULL) {
        *status = 0U;
    }
    if (change != NULL) {
        *change = 0U;
    }
    if (verdict != NULL) {
        *verdict = NULL;
    }
    if (hub_address == 0U || mock->addressed[hub_address] != MOCK_HUB ||
        power_good_ms != mock->hub_power_good_ms || port == 0U ||
        port > mock->hub_ports) {
        return EHCI_WALK_PORT_REFUSED;
    }
    mock->hub_max_packet = hub_max_packet;
    if (mock->hub_status_set[port]) {
        /* The production classifier is what runs on the machine. */
        const uint16_t word = mock->hub_status[port];
        const enum ehci_hub_port_verdict verdict_word =
            ehci_hub_port_verdict(word);

        if (status != NULL) {
            *status = word;
        }
        if (change != NULL) {
            *change = mock->hub_change[port];
        }
        if (verdict != NULL) {
            *verdict = ehci_hub_port_verdict_string(verdict_word);
        }
        if (verdict_word == EHCI_HUB_PORT_EMPTY) {
            return EHCI_WALK_PORT_EMPTY;
        }
        if (verdict_word != EHCI_HUB_PORT_HIGH_SPEED) {
            return EHCI_WALK_PORT_REFUSED;
        }
        mock_event_push(mock, MOCK_EVENT_RESET);
        mock->pending = mock->hub_device[port] != MOCK_NONE
                            ? mock->hub_device[port]
                            : (port == 1U ? MOCK_FOREIGN : MOCK_STICK);
        return EHCI_WALK_PORT_UP;
    }
    if (mock->hub_empty[port]) {
        return EHCI_WALK_PORT_EMPTY;
    }
    if (mock->hub_refused[port]) {
        return EHCI_WALK_PORT_REFUSED;
    }
    mock_event_push(mock, MOCK_EVENT_RESET);
    mock->pending = port == 1U ? MOCK_FOREIGN : MOCK_STICK;
    return EHCI_WALK_PORT_UP;
}

static bool mock_device_begin(void *context, uint8_t address,
                              uint16_t max_packet, uint32_t root_port,
                              uint8_t hub_address, uint8_t hub_port,
                              uint16_t vendor_id, uint16_t product_id) {
    struct mock *mock = context;
    struct mock_attempt *attempt;

    if (mock->attempt_count >= 4U) {
        return false;
    }
    attempt = &mock->attempts[mock->attempt_count++];
    attempt->address = address;
    attempt->hub_address = hub_address;
    attempt->hub_port = hub_port;
    attempt->root_port = root_port;
    attempt->max_packet = max_packet;
    attempt->vendor_id = vendor_id;
    attempt->product_id = product_id;
    return true;
}

static bool mock_attach(void *context) {
    struct mock *mock = context;

    ++mock->attach_count;
    return true;
}

static bool mock_verify(void *context) {
    struct mock *mock = context;
    const uint32_t index = mock->attach_count;

    ++mock->probe_count;
    if (index == 0U || index > 4U) {
        return false;
    }
    return mock->verify_answer[index - 1U];
}

static const char *mock_last_failure(void *context) {
    struct mock *mock = context;

    ++mock->failure_calls;
    return mock->failure_text;
}

static void mock_delay_us(void *context, uint32_t microseconds) {
    struct mock *mock = context;

    mock_event_push(mock, MOCK_EVENT_DELAY);
    if (mock->delay_count <
        sizeof(mock->delay_us) / sizeof(mock->delay_us[0])) {
        mock->delay_us[mock->delay_count++] = microseconds;
    }
}

static struct ehci_walk_ops mock_ops(struct mock *mock) {
    struct ehci_walk_ops ops = {0};

    ops.context = mock;
    ops.control = mock_control;
    ops.root_port_read = mock_root_read;
    ops.root_port_reset = mock_root_reset;
    ops.hub_port_bring_up = mock_hub_bring_up;
    ops.device_begin = mock_device_begin;
    ops.attach_storage = mock_attach;
    ops.verify_medium = mock_verify;
    ops.delay_us = mock_delay_us;
    ops.last_failure = mock_last_failure;
    return ops;
}

static void machine_with_hub(struct mock *mock) {
    memset(mock, 0, sizeof(*mock));
    mock->root_ports = 2U;
    mock->root_connected[1] = true;
    mock->root_device[1] = MOCK_HUB;
    mock->hub_ports = 2U;
    mock->hub_power_good_ms = 20U;
    /* The boot-medium guard accepts only the stick. */
    mock->verify_answer[0] = false;
    mock->verify_answer[1] = true;
}

static void test_walk_continues_past_foreign_storage(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    machine_with_hub(&mock);
    CHECK(ehci_walk_run(&ops, mock.root_ports, &report),
          "the walk finds the boot stick behind the hub");
    CHECK(report.attached == 2U && report.probes == 2U,
          "both devices were attached and both offered to the guard");
    CHECK(report.ports_connected == 1U,
          "one root port carried the hub");
    CHECK(report.selected == 2, "the second attachment is the selected device");
    CHECK(report.selected_hub_address == 1U && report.selected_hub_port == 2U,
          "the selected device is on the hub port the stick was plugged into");
    CHECK(mock.attempt_count == 2U, "the walk tried two devices");
    CHECK(mock.attempts[0].vendor_id == 0xDEADU &&
              mock.attempts[0].hub_port == 1U,
          "the first device was the foreign storage on hub port 1");
    CHECK(mock.attempts[1].vendor_id == 0xABCDU &&
              mock.attempts[1].hub_port == 2U &&
              mock.attempts[1].address == 3U,
          "the second was the boot stick on hub port 2, at its own address");
    CHECK(mock.attempts[0].max_packet == 32U &&
              mock.attempts[1].max_packet == 32U,
          "the device's own EP0 size reached the storage stack");
    CHECK(report.selected_max_packet == 32U,
          "the selected device kept its own EP0 size");
    CHECK(mock.class_request_type == 0xA0U &&
              mock.class_request_request == 0x06U &&
              mock.class_request_value == 0x2900U,
          "the hub descriptor came from the class request 0xA0/0x06/0x2900");
    CHECK(!mock.class_before_config,
          "the hub was configured before its descriptor was read");
    CHECK(mock.hub_max_packet == 64U,
          "the hub's own control-pipe size reached its port requests");
    CHECK(report.selected_address != 0U &&
              report.selected_address != mock.attempts[0].address,
          "the selected device is not the foreign one");
    /*
     * ITEM-557: the walk waits what USB 2.0 says it owes - 10 ms after the
     * root port reset, 10 ms after the hub's port reset, and 2 ms after
     * SET_ADDRESS - and it waits them before the transfer they protect.  The
     * counts are exact: 1 root reset, 1 hub port, 2 devices addressed.
     */
    /* Three resets (the root port and the hub's two ports) and three
       addresses (the hub and the two devices behind it), in that order. */
    CHECK(mock.delay_count == 6U,
          "the walk asked for one recovery interval per reset and address");
    CHECK(mock.delay_count == 6U &&
              mock.delay_us[0] == EHCI_WALK_RESET_RECOVERY_US &&
              mock.delay_us[2] == EHCI_WALK_RESET_RECOVERY_US &&
              mock.delay_us[4] == EHCI_WALK_RESET_RECOVERY_US,
          "each port reset is followed by the 10 ms device recovery");
    CHECK(mock.delay_count == 6U &&
              mock.delay_us[1] == EHCI_WALK_ADDRESS_RECOVERY_US &&
              mock.delay_us[3] == EHCI_WALK_ADDRESS_RECOVERY_US &&
              mock.delay_us[5] == EHCI_WALK_ADDRESS_RECOVERY_US,
          "each SET_ADDRESS is followed by the 2 ms recovery interval");
    {
        bool every_transition_waits = true;
        uint32_t resets = 0U;
        uint32_t addresses = 0U;

        for (uint32_t index = 0U; index < mock.event_count; ++index) {
            if (mock.events[index] == MOCK_EVENT_RESET) {
                ++resets;
                if (index + 1U >= mock.event_count ||
                    mock.events[index + 1U] != MOCK_EVENT_DELAY) {
                    every_transition_waits = false;
                }
            }
            if (mock.events[index] == MOCK_EVENT_ADDRESS) {
                ++addresses;
                if (index + 1U >= mock.event_count ||
                    mock.events[index + 1U] != MOCK_EVENT_DELAY) {
                    every_transition_waits = false;
                }
            }
        }
        CHECK(resets == 3U && addresses == 3U,
              "three resets and three addresses were driven");
        CHECK(every_transition_waits,
              "every reset and SET_ADDRESS is followed by its wait before the "
              "next transfer");
    }
}

static void test_walk_reports_no_medium_after_only_foreign(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    machine_with_hub(&mock);
    mock.hub_ports = 1U;
    mock.verify_answer[0] = false;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a machine with only a foreign storage has no boot medium");
    CHECK(report.attached == 1U && report.probes == 1U &&
              report.selected == -1,
          "the foreign device was attached, probed and rejected");
    /* ITEM-562: a device that attached and was refused by the boot-medium
       guard is "not the boot medium", not a missing device and not a
       descriptor failure.  The old expectation (NO_DEVICE) is what let the
       driver relabel the machine as DESCRIPTOR_FAILED/NO_DEVICE. */
    CHECK(report.failure != NULL && strcmp(report.failure, "NO_MEDIUM") == 0,
          "a rejected device is named as a refused medium, not a missing one");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_MEDIUM,
          "the walk keeps the outcome apart from a device that never answered");
    /* ITEM-576: the RMH keeps the root port's CCS set while the only child is
       a foreign device, so a connected root port is not evidence of the boot
       medium - the walk's own child-level device is. */
    CHECK(report.ports_connected == 1U,
          "the RMH root port stays connected while the child medium is gone");
    CHECK(report.port_count == 1U &&
              report.ports[0].outcome == EHCI_WALK_OUTCOME_MEDIUM &&
              report.ports[0].hub_port == 1U,
          "the rejected device is recorded against the hub port it hung on");
}

static void test_walk_binds_a_root_port_storage(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    memset(&mock, 0, sizeof(mock));
    mock.root_ports = 2U;
    mock.root_connected[1] = true;
    mock.root_device[1] = MOCK_STICK;
    mock.verify_answer[0] = true;
    CHECK(ehci_walk_run(&ops, mock.root_ports, &report),
          "a stick on a root port is bound by the same walk");
    CHECK(report.attached == 1U && report.probes == 1U &&
              report.selected == 1 && report.selected_hub_address == 0U,
          "the root device is the first attachment, with no hub path");
    CHECK(mock.attempts[0].max_packet == 32U &&
              report.selected_max_packet == 32U,
          "the root device kept its own EP0 size");
}

/* ITEM-557: the recovery intervals are required, not optional.  A transport
   that cannot wait is refused before the walk touches a device. */
static void test_walk_refuses_a_transport_without_recovery(void) {
    struct mock mock;
    struct ehci_walk_report report;
    struct ehci_walk_ops ops = {0};

    machine_with_hub(&mock);
    ops = mock_ops(&mock);
    ops.delay_us = NULL;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a transport that cannot wait is refused");
    CHECK(mock.event_count == 0U && mock.attempt_count == 0U,
          "nothing was reset or addressed without the recovery intervals");
}

/*
 * ITEM-558: the walk's stage is not the cause.  The first eight-byte
 * descriptor read is refused by the transport, which names the transfer in its
 * own text; the report must carry both, and the text must still be there when
 * the caller reads the report (the "cleanup" is the walk returning and the
 * driver mapping the report).
 */
static void test_walk_keeps_the_primary_cause_of_device8(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    memset(&mock, 0, sizeof(mock));
    mock.root_ports = 1U;
    mock.root_connected[1] = true;
    mock.root_device[1] = MOCK_STICK;
    mock.fail_device8 = true;
    mock.failure_text =
        "pci=00:1D.0 port=1 result=XACTERR index=0/3 qtd=0x00000048 "
        "addr=0 ep=0x80 hub=0 hub_port=0 received=0 residual=8 "
        "usbcmd=0x00080031 usbsts=0x00000001 portsc=0x00001005 "
        "q0=0x00480008 q1=0x00000000 q2=0x00000000";

    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a refused first descriptor fails the walk");
    CHECK(report.failure != NULL && strcmp(report.failure, "DEVICE8") == 0,
          "the walk names the stage DEVICE8");
    CHECK(report.failure_detail != NULL &&
              strstr(report.failure_detail, "result=XACTERR") != NULL &&
              strstr(report.failure_detail, "qtd=0x00000048") != NULL &&
              strstr(report.failure_detail, "received=0") != NULL,
          "the report keeps the transport's primary cause beside the stage");
    CHECK(report.attached == 0U && report.selected == -1,
          "no device was attached or selected by a refused first read");
    CHECK(mock.failure_calls == 1U,
          "the transport's cause is asked for exactly when the transfer fails");
    CHECK(mock.addressed[1] == MOCK_NONE && mock.attempt_count == 0U,
          "a device that never answered a descriptor was never addressed");
}

/*
 * ITEM-562: the walk reports where each path ended, not one fallback string.
 *
 * The hardware run printed `DESCRIPTOR_FAILED/NO_DEVICE` for a machine with two
 * controllers, no medium and no descriptor read: the driver replaced a specific
 * port verdict with the walk's fallback.  These cases pin the production walk's
 * own naming, which is what the driver now keeps.
 */
static void test_no_connected_port_says_so(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    memset(&mock, 0, sizeof(mock));
    mock.root_ports = 2U;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a machine with nothing plugged in has no boot medium");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_NO_PORT &&
              strcmp(report.failure, "NO_CONNECTED_PORT") == 0,
          "an empty machine is named as having no connected port");
    CHECK(report.port_count == 0U && report.failures == 0U,
          "no port record is invented for a port nothing was on");
    CHECK(ehci_walk_outcome_status(report.outcome) == EHCI_STATUS_NO_DEVICE,
          "the status an empty machine maps to is NO_DEVICE, not a transfer");
}

static void test_root_port_reset_refusal_is_named(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    memset(&mock, 0, sizeof(mock));
    mock.root_ports = 2U;
    mock.root_connected[1] = true;
    mock.root_device[1] = MOCK_STICK;
    mock.root_refuse[1] = true;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a port that does not enable carries no usable device");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_PORT_RESET,
          "the walk names the reset, not a descriptor it never read");
    CHECK(strcmp(report.failure, "ROOT_RESET") == 0,
          "the stage names the root port reset");
    CHECK(report.ports_connected == 1U && report.failures == 1U &&
              report.port_count == 1U,
          "the connected port is counted and recorded once");
    CHECK(report.ports[0].port == 1U &&
              report.ports[0].outcome == EHCI_WALK_OUTCOME_PORT_RESET,
          "the record keeps the port number and its outcome");
    CHECK(report.failure_port == 1U && report.failure_hub_address == 0U,
          "the first refusal names the root port it happened on");
    CHECK(ehci_walk_outcome_status(report.outcome) ==
              EHCI_STATUS_PORT_NOT_ENABLED,
          "a reset refusal maps to the port status, not to a descriptor one");
    CHECK(mock.attempt_count == 0U,
          "no device was addressed after the port refused its reset");
}

static void test_an_empty_hub_port_is_not_a_refusal(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    machine_with_hub(&mock);
    /* The rate matching hub's first port is empty, the second carries the
       stick: exactly the machine the owner has, minus the foreign device. */
    mock.hub_empty[1] = true;
    mock.verify_answer[0] = true;
    CHECK(ehci_walk_run(&ops, mock.root_ports, &report),
          "the walk finds the stick behind the hub's second port");
    CHECK(report.selected == 1 && report.attached == 1U,
          "the stick was the first and only attachment");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_OK,
          "a walk that bound its medium reports that outcome");
    CHECK(report.failures == 1U,
          "the accepted port is recorded and the empty one is not a failure");
    CHECK(report.ports[0].hub_port == 2U &&
              report.ports[0].outcome == EHCI_WALK_OUTCOME_OK,
          "the accepted device is recorded against the hub port it hung on");
}

static void test_a_refused_hub_port_is_named(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    machine_with_hub(&mock);
    mock.hub_refused[1] = true;
    mock.hub_empty[2] = true;
    mock.verify_answer[0] = true;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "a hub port that will not come up ends the machine's path");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_HUB &&
              strcmp(report.failure, "HUB_PORT") == 0,
          "the refusal names the hub port");
    CHECK(report.failure_hub_address == 1U && report.failure_hub_port == 1U,
          "the record keeps the hub and the port that refused");
    CHECK(mock.attempt_count == 0U,
          "no device was addressed on the refused hub port");
}

static void test_the_first_refusal_survives_a_later_port(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    memset(&mock, 0, sizeof(mock));
    mock.root_ports = 2U;
    mock.root_connected[1] = true;
    mock.root_device[1] = MOCK_STICK;
    mock.root_connected[2] = true;
    mock.root_device[2] = MOCK_FOREIGN;
    mock.fail_device8 = true;
    mock.fail_device8_on = MOCK_STICK;
    mock.failure_text = "result=XACTERR qtd=0x00000048";
    /* Port 2's device attaches and the guard rejects it: a later, softer
       outcome that used to replace the earlier, harder one. */
    mock.verify_answer[0] = false;
    CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
          "the machine has no boot medium");
    CHECK(report.outcome == EHCI_WALK_OUTCOME_TRANSFER &&
              strcmp(report.failure, "DEVICE8") == 0,
          "the first concrete refusal survives the later port");
    CHECK(report.failure_port == 1U,
          "the refusal still names the port it happened on");
    CHECK(report.failures == 2U && report.port_count == 2U,
          "both ports are recorded, in walk order");
    CHECK(report.ports[0].outcome == EHCI_WALK_OUTCOME_TRANSFER &&
              report.ports[1].outcome == EHCI_WALK_OUTCOME_MEDIUM,
          "each port keeps its own outcome");
    CHECK(report.ports[0].detail != NULL &&
              strstr(report.ports[0].detail, "result=XACTERR") != NULL,
          "the first port keeps the transport's own text");
}


/* ITEM-570: a downstream port is classified from the hub's raw wPortStatus, and
   the speed bits are the specification's (low = bit 9, high = bit 10).  A
   high-speed port must reach addressing and the boot-medium guard, not be
   refused as a low-speed port that would need split transactions. */
/* ITEM-582: the RMH-level proof.  A hub carries a bound device on port 1 and a
   stick on port 3; port 2 is empty at boot and the stick appears there later.
   The production skip rules (one root port, one downstream port) must not cost
   the walk the other downstream ports, must not reset the skipped device, and
   the second walk must reach the late device. */
static bool mock_port_skip(void *context, uint32_t port) {
    (void)context;
    return port == 2U;
}

static bool mock_hub_port_skip(void *context, uint32_t root_port,
                               uint8_t hub_address, uint8_t hub_port) {
    (void)context;
    (void)hub_address;
    return root_port == 1U && hub_port == 1U;
}

static void test_a_late_stick_is_found_behind_a_skipped_hub_port(void) {
    struct mock mock;
    struct ehci_walk_report first;
    struct ehci_walk_report second;
    struct ehci_walk_ops ops;

    machine_with_hub(&mock);
    mock.root_ports = 3U;
    mock.hub_ports = 3U;
    mock.hub_empty[1] = false;
    mock.hub_device[1] = MOCK_FOREIGN;
    mock.hub_empty[2] = true;
    mock.hub_empty[3] = false;
    mock.hub_device[3] = MOCK_STICK;
    mock.verify_answer[0] = true;
    mock.verify_answer[1] = true;
    /* A skipped port that were reset would be refused here, so a clean walk
       proves the skip. */
    mock.hub_refused[1] = true;
    ops = mock_ops(&mock);
    ops.port_skip = mock_port_skip;
    ops.hub_port_skip = mock_hub_port_skip;

    CHECK(ehci_walk_run(&ops, mock.root_ports, &first),
          "the first walk reaches the stick on hub port 3");
    CHECK(first.selected == 1 && first.selected_hub_port == 3U,
          "the selected device is hub port 3, not the skipped port 1");
    CHECK(mock.attempt_count == 1U &&
              mock.attempts[0].hub_port == 3U,
          "the skipped hub port and the skipped root port were not tried");
    CHECK(first.ports_connected == 1U,
          "the hub's root port was measured; the skipped root port was not "
          "reset or counted");

    /* The operator plugs the diagnostic stick into hub port 2 after boot. */
    mock.hub_empty[2] = false;
    mock.hub_device[2] = MOCK_STICK;
    mock.attempt_count = 0U;
    mock.hub_refused[1] = true;
    CHECK(ehci_walk_run(&ops, mock.root_ports, &second),
          "the second walk reaches the stick plugged in after boot");
    CHECK(second.selected == 1 && second.selected_hub_port == 2U,
          "the late device is the one inside the hub, on its own port");
    CHECK(mock.attempts[0].hub_port == 2U,
          "the walk started with the late device, not the skipped one");
}

static void test_a_high_speed_hub_port_reaches_the_medium(void) {
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    machine_with_hub(&mock);
    mock.hub_ports = 1U;
    mock.hub_status_set[1] = true;
    mock.hub_status[1] = 0x0503U; /* connected, enabled, power, HS (bit 10) */
    mock.hub_change[1] = 0x0010U;
    mock.hub_device[1] = MOCK_STICK;
    mock.verify_answer[0] = true;

    CHECK(ehci_walk_run(&ops, mock.root_ports, &report),
          "a high-speed hub port is walked to the boot medium");
    CHECK(mock.attempt_count == 1U && mock.attempts[0].hub_port == 1U,
          "the device behind the high-speed port was offered to the bridge");
    CHECK(mock.attempts[0].address >= 1U,
          "the high-speed device was addressed through the walk");
    CHECK(report.attached == 1U && report.probes == 1U,
          "the high-speed device was attached and its medium verified");
    CHECK(report.selected_hub_port == 1U,
          "the selected medium is the one on the high-speed downstream port");
    CHECK(mock.hub_status_set[1] && mock.hub_status[1] == 0x0503U,
          "the raw wPortStatus that drove the verdict is the one expected");
}

/* The same walk, with the slower words: low speed (bit 9) and the test mode
   (bit 11) are not high speed, and the refusal keeps the hub's own words. */
static void test_a_slower_hub_port_is_refused_with_its_raw_words(void) {
    static const uint16_t words[] = {0x0303U, 0x0903U, 0x0103U};
    static const char *const verdicts[] = {"LOW_SPEED", "FULL_SPEED",
                                           "FULL_SPEED"};
    struct mock mock;
    struct ehci_walk_report report;
    const struct ehci_walk_ops ops = mock_ops(&mock);

    for (uint32_t index = 0U; index < 3U; ++index) {
        memset(&mock, 0, sizeof(mock));
        machine_with_hub(&mock);
        mock.hub_ports = 1U;
        mock.hub_status_set[1] = true;
        mock.hub_status[1] = words[index];
        mock.hub_change[1] = 0x0001U;
        mock.hub_device[1] = MOCK_STICK;
        mock.verify_answer[0] = true;

        CHECK(!ehci_walk_run(&ops, mock.root_ports, &report),
              "a port that is not high speed does not reach the medium");
        CHECK(mock.attempt_count == 0U,
              "the walk does not address a device it cannot use");
        CHECK(report.outcome == EHCI_WALK_OUTCOME_HUB &&
                  report.failure != NULL &&
                  strcmp(report.failure, "HUB_PORT") == 0,
              "the refusal is the hub port's own stage");
        CHECK(report.port_count >= 1U &&
                  report.ports[0].hub_status == words[index] &&
                  report.ports[0].hub_change == 0x0001U,
              "the record keeps the raw wPortStatus and wPortChange");
        CHECK(report.port_count >= 1U &&
                  report.ports[0].hub_verdict != NULL &&
                  strcmp(report.ports[0].hub_verdict, verdicts[index]) == 0,
              "the record keeps the verdict the raw word produced");
    }
}

int main(void) {
    test_walk_continues_past_foreign_storage();
    test_walk_reports_no_medium_after_only_foreign();
    test_no_connected_port_says_so();
    test_root_port_reset_refusal_is_named();
    test_an_empty_hub_port_is_not_a_refusal();
    test_a_refused_hub_port_is_named();
    test_the_first_refusal_survives_a_later_port();
    test_walk_binds_a_root_port_storage();
    test_walk_refuses_a_transport_without_recovery();
    test_walk_keeps_the_primary_cause_of_device8();

    test_a_high_speed_hub_port_reaches_the_medium();
    test_a_late_stick_is_found_behind_a_skipped_hub_port();
    test_a_slower_hub_port_is_refused_with_its_raw_words();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("EHCI walk host test: ALL OK");
    return 0;
}
